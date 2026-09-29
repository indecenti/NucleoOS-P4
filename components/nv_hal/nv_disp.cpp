// nv_disp — tear-free display compositor. See nv_disp.h.
//
// Buffer roles. s_front is the buffer on screen, or the one already queued to replace it (a switch
// requested with esp_lcd_panel_draw_bitmap() takes effect at the next frame start). LVGL never
// writes into s_front; direct writers and readers only touch s_front, under s_lock.
//
// Presenting a frame (LVGL task, last flush of the frame):
//   1. carry the direct region (video) from the old front into the new frame,
//   2. esp_lcd_panel_draw_bitmap(new) = cache write-back of the frame + switch request,
//   3. leave LVGL's "flushing" state set; LVGL calls flush_wait_cb before it writes into the other
//      buffer again, and that returns only once the vsync interrupt confirmed the switch.
#include "nv_disp.h"
#include "nv_2d.h"
#include "nv_log.h"

#include <cstring>
#include <initializer_list>

#include "esp_cache.h"
#include "esp_heap_caps.h"
#include "esp_lcd_mipi_dsi.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lvgl_port.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "disp";

namespace {

// The DPI interrupt picks the buffer for the next frame before it calls on_vsync: a switch requested
// a few microseconds earlier may have missed that restart. Only a vsync comfortably after the request
// proves the new buffer is being scanned (a missed one costs one extra frame of latency, never a tear).
constexpr int64_t    kSwapGuardUs  = 50;
// A panel that stopped refreshing must slow LVGL down, not hang it.
constexpr TickType_t kVsyncTimeout = pdMS_TO_TICKS(100);
constexpr int        kMaxRects     = 32;     // = LV_INV_BUF_SIZE: LVGL itself tracks no more
constexpr size_t     kAlign        = 128;    // PSRAM cache line on the P4

struct Rect { int x1, y1, x2, y2; };   // physical pixels, inclusive

// A small set of rectangles. `unknown` (overflow) means "could be anything": callers stay safe by
// treating it as the whole screen when it is what must be copied, and as nothing when it is what
// may be skipped.
struct RectSet {
    Rect r[kMaxRects];
    int  n = 0;
    bool unknown = false;
    void clear() { n = 0; unknown = false; }
    void add(const Rect &x) {
        if (unknown) return;
        for (int i = 0; i < n; i++)   // already covered
            if (x.x1 >= r[i].x1 && x.y1 >= r[i].y1 && x.x2 <= r[i].x2 && x.y2 <= r[i].y2) return;
        if (n == kMaxRects) { unknown = true; return; }
        r[n++] = x;
    }
};

esp_lcd_panel_handle_t s_panel = nullptr;
lv_display_t          *s_disp = nullptr;
int                    s_w = 0, s_h = 0;
size_t                 s_fb_bytes = 0;
uint8_t               *s_fb[2] = {};
volatile int           s_front = 0;

SemaphoreHandle_t s_lock = nullptr;    // front-buffer ownership: present vs readers / direct writers
SemaphoreHandle_t s_vsync = nullptr;   // given by on_vsync once a requested switch took effect
bool              s_swap_pending = false;   // shared with the ISR (atomic accesses)
int64_t           s_swap_t_us = 0;
bool              s_swap_outstanding = false;   // LVGL task: a switch not yet waited for

// rotated mode (PARTIAL rendering + PPA rotation into the back buffer)
bool                s_rotated = false;
int                 s_rot = LV_DISPLAY_ROTATION_0;
uint8_t            *s_part[2] = {};
size_t              s_part_bytes = 0;
ppa_client_handle_t s_ppa = nullptr;
RectSet             s_prev;   // strips presented in the previous frame
RectSet             s_cur;    // strips written into the back buffer this frame
RectSet             s_inv;    // what the coming frame will redraw (from LV_EVENT_INVALIDATE_AREA)
bool                s_mode_busy = false;

bool s_region_on = false;   // under s_lock
Rect s_region{};

// layer (under s_lock; the pump timer belongs to the LVGL task)
nv_disp_layer_t   s_layer{};
bool              s_layer_on = false;
uint32_t          s_layer_gen[2] = {};    // generation each buffer holds (0: must be redrawn fresh)
uint32_t          s_layer_failed = 0;     // generation whose draw failed: not retried
uint32_t          s_layer_draw_est = 0;   // this layer's measured cost (us) of a redraw / a carry
uint32_t          s_layer_copy_est = 0;
uint32_t          s_layer_choices = 0;
bool              s_layer_unify = false;  // detached: bring the back buffer's rectangle up to the front
Rect              s_layer_last{};         // rectangle(s) to unify
lv_timer_t       *s_layer_pump = nullptr;
uint32_t          s_layer_pump_ms = 0;
constexpr uint32_t kPumpIdleMs = 200;     // pump period with no layer (attached: LV_DEF_REFR_PERIOD)

bool s_cache_align = false;   // rows are cache-line aligned: row-granular M2C is legal (copy_rect)

nv_disp_stats_t   s_st{};
volatile uint32_t s_vsyncs = 0;
int64_t           s_render_t0 = 0;     // LV_EVENT_RENDER_START of the frame being built
int64_t           s_last_present = 0;

inline uint32_t ema(uint32_t avg, uint32_t v) { return avg ? (avg * 7 + v) / 8 : v; }
inline uint32_t us_since(int64_t t0) { return (uint32_t)(esp_timer_get_time() - t0); }

// ---- vsync ---------------------------------------------------------------------------------------
bool IRAM_ATTR on_vsync(esp_lcd_panel_handle_t, esp_lcd_dpi_panel_event_data_t *, void *) {
    s_vsyncs = s_vsyncs + 1;
    if (!__atomic_load_n(&s_swap_pending, __ATOMIC_ACQUIRE)) return false;
    if (esp_timer_get_time() - s_swap_t_us < kSwapGuardUs) return false;
    __atomic_store_n(&s_swap_pending, false, __ATOMIC_RELAXED);
    BaseType_t hp = pdFALSE;
    xSemaphoreGiveFromISR(s_vsync, &hp);
    return hp == pdTRUE;
}

// Block until the last requested switch is on screen: the old front may be reused after that.
void wait_swap(void) {
    if (!s_swap_outstanding) return;
    const int64_t t0 = esp_timer_get_time();
    if (xSemaphoreTake(s_vsync, kVsyncTimeout) != pdTRUE) {
        __atomic_store_n(&s_swap_pending, false, __ATOMIC_RELEASE);
        if (s_st.vsync_timeouts++ % 64 == 0)
            NV_LOGW(TAG, "vsync not seen within %lu ms (%lu times): panel not refreshing?",
                    (unsigned long)pdTICKS_TO_MS(kVsyncTimeout), (unsigned long)s_st.vsync_timeouts);
    }
    s_swap_outstanding = false;
    const uint32_t us = us_since(t0);
    s_st.wait_us_avg = ema(s_st.wait_us_avg, us);
    if (us > s_st.wait_us_max) s_st.wait_us_max = us;
}

// ---- pixel copies between the two buffers ---------------------------------------------------------
// CPU copy of a rectangle from buffer `from` to buffer `to`. The source rows are invalidated first
// (DMA writers — PPA, video — may have changed them behind the cache) and the destination rows are
// written back after, so the next DMA reader (the panel, or a PPA that invalidates its output) sees
// them. Whole rows are synced: a panel row is a multiple of the cache line, and M2C refuses
// unaligned ranges (it would silently do nothing).
void copy_rect(int from, int to, const Rect &r) {
    const size_t row = (size_t)s_w * 2;
    uint8_t *src = s_fb[from] + (size_t)r.y1 * row, *dst = s_fb[to] + (size_t)r.y1 * row;
    const size_t span = (size_t)(r.y2 - r.y1 + 1) * row;
    const size_t off = (size_t)r.x1 * 2, len = (size_t)(r.x2 - r.x1 + 1) * 2;
    // Whole rows are one contiguous span: AXI-GDMA copies it in long bursts (the CPU managed ~40 MB/s
    // here while the camera and panel DMA loaded PSRAM) and handles the cache itself.
    if (off == 0 && len == row && nv_2d_copy(dst, src, span, 100) == ESP_OK) return;
    if (s_cache_align) esp_cache_msync(src, span, ESP_CACHE_MSYNC_FLAG_DIR_M2C);
    if (off == 0 && len == row) {
        memcpy(dst, src, span);
    } else {
        for (int y = r.y1; y <= r.y2; y++, src += row, dst += row) memcpy(dst + off, src + off, len);
        src = s_fb[from] + (size_t)r.y1 * row;
        dst = s_fb[to] + (size_t)r.y1 * row;
    }
    esp_cache_msync(dst, span, ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_UNALIGNED);
}

// `a` minus `b` into out[] (0..4 pieces); returns the count.
int subtract(const Rect &a, const Rect &b, Rect out[4]) {
    if (b.x2 < a.x1 || b.x1 > a.x2 || b.y2 < a.y1 || b.y1 > a.y2) { out[0] = a; return 1; }
    int n = 0;
    if (b.y1 > a.y1) out[n++] = { a.x1, a.y1, a.x2, b.y1 - 1 };                      // above
    if (b.y2 < a.y2) out[n++] = { a.x1, b.y2 + 1, a.x2, a.y2 };                      // below
    const int y1 = b.y1 > a.y1 ? b.y1 : a.y1, y2 = b.y2 < a.y2 ? b.y2 : a.y2;
    if (b.x1 > a.x1) out[n++] = { a.x1, y1, b.x1 - 1, y2 };                          // left
    if (b.x2 < a.x2) out[n++] = { b.x2 + 1, y1, a.x2, y2 };                          // right
    return n;
}

// Copy `p` minus every rectangle of `skip` from `from` to `to`. When the pieces would not fit the
// work list, the leftovers are copied whole: over-copying is always correct, under-copying is not.
void copy_minus(int from, int to, const Rect &p, const RectSet &skip) {
    constexpr int kWork = 64;
    Rect work[kWork], next[kWork];
    int n = 1;
    work[0] = p;
    if (!skip.unknown) {
        for (int i = 0; i < skip.n && n > 0; i++) {
            int m = 0;
            bool overflow = false;
            for (int j = 0; j < n; j++) {
                Rect pieces[4];
                const int k = subtract(work[j], skip.r[i], pieces);
                if (m + k > kWork) { overflow = true; break; }
                for (int q = 0; q < k; q++) next[m++] = pieces[q];
            }
            if (overflow) break;   // keep `work` as it is (a superset of what must be copied)
            memcpy(work, next, sizeof(Rect) * m);
            n = m;
        }
    }
    for (int j = 0; j < n; j++) copy_rect(from, to, work[j]);
}

Rect to_physical(const lv_area_t &a);

// ---- layer --------------------------------------------------------------------------------------
Rect layer_rect(void) { return { s_layer.x, s_layer.y, s_layer.x + s_layer.w - 1, s_layer.y + s_layer.h - 1 }; }

bool overlaps(const Rect &a, const Rect &b) {
    return !(b.x2 < a.x1 || b.x1 > a.x2 || b.y2 < a.y1 || b.y1 > a.y2);
}

// LVGL rendered `r` (physical) into a buffer. Inside the layer's rectangle that paint covers the
// picture in this buffer, and LVGL's next sync copies it into the other one: redraw both, fresh.
void layer_painted(const Rect &r) {
    if (s_layer_on && overlaps(r, layer_rect())) s_layer_gen[0] = s_layer_gen[1] = 0;
}

// Caller holds s_lock. Bring buffer `idx` (about to be presented) up to the newest picture. When the
// front buffer already holds it, it can be carried over (a copy of the whole rectangle) or drawn
// again (the producer's scale, often cheaper: a small source scales faster than the big rectangle
// copies): take whichever has measured cheaper for this layer, and re-measure the other now and
// then, since the costs follow the picture geometry and the memory load.
void compose_layer(int idx) {
    if (!s_layer_on) return;
    const uint32_t want = s_layer.latest(s_layer.ctx);
    if (!want || s_layer_gen[idx] == want || want == s_layer_failed) return;
    const bool fresh = s_layer_gen[idx] == 0;
    bool carry = false;
    if (s_layer_gen[s_front] == want) {
        const bool probe = (++s_layer_choices & 63) == 0;
        carry = !s_layer_copy_est || (s_layer_copy_est <= s_layer_draw_est) != probe;
        if (fresh && !s_layer_draw_est) carry = true;   // a fresh draw also paints the margins
    }
    const int64_t t0 = esp_timer_get_time();
    if (carry) {
        copy_rect(s_front, idx, layer_rect());
        s_layer_gen[idx] = want;
        const uint32_t us = us_since(t0);
        s_layer_copy_est = s_layer_copy_est ? (s_layer_copy_est * 3 + us) / 4 : us;
        s_st.layer_copies++;
        s_st.layer_copy_us_avg = ema(s_st.layer_copy_us_avg, us);
    } else {
        s_layer_gen[idx] = s_layer.draw(s_layer.ctx, (uint16_t *)s_fb[idx], s_w, fresh);
        if (!s_layer_gen[idx]) s_layer_failed = want;
        const uint32_t us = us_since(t0);
        if (!fresh && s_layer_gen[idx])   // fresh draws also clear the margins: not comparable
            s_layer_draw_est = s_layer_draw_est ? (s_layer_draw_est * 3 + us) / 4 : us;
        s_st.layer_draws++;
        s_st.layer_draw_us_avg = ema(s_st.layer_draw_us_avg, us);
    }
}

// LVGL lock held (LVGL task, or nv_disp_layer_update), s_lock not held. LVGL presents only what it
// rendered and its refresh timer sleeps while nothing is invalid: when the screen lacks the newest
// picture, invalidate one pixel outside the layer so a frame (and its compose) follows.
void layer_kick(void) {
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool due = false;
    Rect lr{};
    if (s_layer_on) {
        const uint32_t want = s_layer.latest(s_layer.ctx);
        due = want && want != s_layer_gen[s_front] && want != s_layer_failed;
        lr = layer_rect();
    }
    xSemaphoreGive(s_lock);
    if (!due) return;
    const int hres = lv_display_get_horizontal_resolution(s_disp);
    const int vres = lv_display_get_vertical_resolution(s_disp);
    const lv_area_t corners[2] = { { hres - 1, vres - 1, hres - 1, vres - 1 }, { 0, 0, 0, 0 } };
    const lv_area_t *pick = &corners[1];
    for (const lv_area_t &c : corners)
        if (!overlaps(to_physical(c), lr)) { pick = &c; break; }
    lv_obj_invalidate_area(lv_display_get_layer_sys(s_disp), pick);   // sys layer spans the screen
}

// LV_EVENT_REFR_START (nothing rendered yet). A detached layer leaves the two buffers holding
// different pictures in its rectangle, and LVGL would then alternate them under whatever it draws
// next (a drawer sliding over a paused video): make the back buffer match the screen once.
void layer_unify(void) {
    if (!s_layer_unify) return;
    wait_swap();   // rotated mode: the back buffer may still be scanned until the last switch lands
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_layer_unify) copy_rect(s_front, s_front ^ 1, s_layer_last);
    s_layer_unify = false;
    xSemaphoreGive(s_lock);
}

// LVGL timer: the safety net for a kick nv_disp_layer_update could not deliver (LVGL was busy
// outside a refresh). One refresh period while a layer is attached, a slow idle tick otherwise.
void layer_pump(lv_timer_t *t) {
    const uint32_t ms = s_layer_on ? LV_DEF_REFR_PERIOD : kPumpIdleMs;
    if (ms != s_layer_pump_ms) { lv_timer_set_period(t, ms); s_layer_pump_ms = ms; }
    if (s_layer_on) layer_kick();
}

// ---- presenting ----------------------------------------------------------------------------------
// Caller holds s_lock. `idx` holds the finished frame and becomes the front.
void present(int idx) {
    const int64_t t0 = esp_timer_get_time();
    if (s_render_t0) s_st.render_us_avg = ema(s_st.render_us_avg, (uint32_t)(t0 - s_render_t0));
    // frame-to-frame interval while animating (gaps over 100 ms are idle time, not frame time)
    if (s_last_present && t0 - s_last_present < 100000)
        s_st.frame_us_avg = ema(s_st.frame_us_avg, (uint32_t)(t0 - s_last_present));
    s_last_present = t0;
    if (s_region_on) copy_rect(s_front, idx, s_region);   // the video's latest frame comes along
    compose_layer(idx);
    xSemaphoreTake(s_vsync, 0);   // drop a confirmation that arrived after a timeout
    // Writes the frame back from the cache, then queues the switch (applied at the next frame start).
    esp_lcd_panel_draw_bitmap(s_panel, 0, 0, s_w, s_h, s_fb[idx]);
    s_front = idx;
    s_swap_t_us = esp_timer_get_time();
    __atomic_store_n(&s_swap_pending, true, __ATOMIC_RELEASE);
    s_swap_outstanding = true;
    s_st.swaps++;
    s_st.present_us_avg = ema(s_st.present_us_avg, us_since(t0));
}

// Logical (rotated) area -> physical panel rectangle, matching the PPA's rotation direction.
Rect to_physical(const lv_area_t &a) {
    const int hres = lv_display_get_horizontal_resolution(s_disp);
    const int vres = lv_display_get_vertical_resolution(s_disp);
    switch (s_rot) {
    case LV_DISPLAY_ROTATION_90:  return { a.y1, hres - a.x2 - 1, a.y2, hres - a.x1 - 1 };
    case LV_DISPLAY_ROTATION_180: return { hres - a.x2 - 1, vres - a.y2 - 1, hres - a.x1 - 1, vres - a.y1 - 1 };
    case LV_DISPLAY_ROTATION_270: return { vres - a.y2 - 1, a.x1, vres - a.y1 - 1, a.x2 };
    default:                      return { a.x1, a.y1, a.x2, a.y2 };
    }
}

// Rotated mode: rotate one rendered strip straight into the back buffer.
void rotate_strip(const lv_area_t &a, const uint8_t *px, int back, Rect *phys) {
    const int w = a.x2 - a.x1 + 1, h = a.y2 - a.y1 + 1;
    *phys = to_physical(a);
    ppa_srm_oper_config_t op = {};
    op.in.buffer = px;
    op.in.pic_w = (uint32_t)w;  op.in.pic_h = (uint32_t)h;
    op.in.block_w = (uint32_t)w; op.in.block_h = (uint32_t)h;
    op.in.srm_cm = PPA_SRM_COLOR_MODE_RGB565;
    op.out.buffer = s_fb[back];
    op.out.buffer_size = (uint32_t)s_fb_bytes;
    op.out.pic_w = (uint32_t)s_w; op.out.pic_h = (uint32_t)s_h;
    op.out.block_offset_x = (uint32_t)phys->x1; op.out.block_offset_y = (uint32_t)phys->y1;
    op.out.srm_cm = PPA_SRM_COLOR_MODE_RGB565;
    op.rotation_angle = (ppa_srm_rotation_angle_t)s_rot;   // LV_DISPLAY_ROTATION_n == PPA_SRM_ROTATION_ANGLE_n
    op.scale_x = 1.0f; op.scale_y = 1.0f;
    op.mode = PPA_TRANS_MODE_BLOCKING;
    const esp_err_t e = nv_2d_srm(s_ppa, &op);
    if (e != ESP_OK) NV_LOGW(TAG, "rotate strip %dx%d failed: %s", w, h, esp_err_to_name(e));
}

// Rotated mode, before the first strip of a frame: the back buffer still holds the frame before the
// previous one. Bring over what the previous frame changed, except what this frame redraws anyway.
void begin_rotated_frame(void) {
    wait_swap();   // the back buffer was the front until the last switch landed
    const int64_t t0 = esp_timer_get_time();
    const int front = s_front, back = front ^ 1;
    if (s_prev.unknown) {
        copy_minus(front, back, Rect{ 0, 0, s_w - 1, s_h - 1 }, s_inv);
    } else {
        for (int i = 0; i < s_prev.n; i++) copy_minus(front, back, s_prev.r[i], s_inv);
    }
    if (s_prev.n || s_prev.unknown) s_st.sync_us_avg = ema(s_st.sync_us_avg, us_since(t0));
    s_prev.clear();
    s_inv.clear();
    s_cur.clear();
}

// ---- LVGL glue -----------------------------------------------------------------------------------
void flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px) {
    if (!s_rotated) {
        // DIRECT: LVGL rendered into the back buffer itself; only the last area presents it.
        layer_painted(Rect{ area->x1, area->y1, area->x2, area->y2 });
        if (!lv_display_flush_is_last(disp)) { lv_display_flush_ready(disp); return; }
        const int idx = (px >= s_fb[1] && px < s_fb[1] + s_fb_bytes) ? 1 : 0;
        if (idx == s_front) s_st.violations++;
        xSemaphoreTake(s_lock, portMAX_DELAY);
        present(idx);
        xSemaphoreGive(s_lock);
        return;   // "flushing" stays set until flush_wait_cb sees the switch on screen
    }
    const int back = s_front ^ 1;
    Rect r;
    rotate_strip(*area, px, back, &r);
    s_cur.add(r);
    layer_painted(r);
    if (!lv_display_flush_is_last(disp)) { lv_display_flush_ready(disp); return; }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    present(back);
    xSemaphoreGive(s_lock);
    s_prev = s_cur;
    s_cur.clear();
}

void flush_wait_cb(lv_display_t *) { wait_swap(); }

void set_rotation_mode(int rot);

void event_cb(lv_event_t *e) {
    switch (lv_event_get_code(e)) {
    case LV_EVENT_INVALIDATE_AREA:
        if (s_rotated) {
            const auto *a = static_cast<const lv_area_t *>(lv_event_get_param(e));
            if (a) s_inv.add(to_physical(*a));
        }
        lvgl_port_task_wake(LVGL_PORT_EVENT_DISPLAY, nullptr);
        break;
    case LV_EVENT_REFR_REQUEST:
        lvgl_port_task_wake(LVGL_PORT_EVENT_DISPLAY, nullptr);
        break;
    case LV_EVENT_REFR_START:
        layer_unify();
        layer_kick();
        break;
    case LV_EVENT_REFR_READY:   // a picture that arrived during this frame gets the next one
        layer_kick();
        break;
    case LV_EVENT_RENDER_START:
        if (s_rotated) begin_rotated_frame();
        s_render_t0 = esp_timer_get_time();
        break;
    case LV_EVENT_RESOLUTION_CHANGED:
        set_rotation_mode(lv_display_get_rotation(s_disp));
        break;
    default:
        break;
    }
}

void free_strips(void) {
    for (auto &p : s_part) { heap_caps_free(p); p = nullptr; }
    s_part_bytes = 0;
}

// Switch between DIRECT (landscape) and PARTIAL + PPA rotation. LVGL invalidates the whole screen
// before it reports the new resolution, so the first frame of the new mode redraws everything.
void set_rotation_mode(int rot) {
    if (s_mode_busy) return;
    s_mode_busy = true;
    wait_swap();
    if (rot == LV_DISPLAY_ROTATION_0) {
        lv_display_set_buffers(s_disp, s_fb[s_front ^ 1], s_fb[s_front], s_fb_bytes,
                               LV_DISPLAY_RENDER_MODE_DIRECT);
        free_strips();
        s_rotated = false;
    } else {
        size_t bytes = 0;
        if (!s_part[0]) {
            // a quarter of the screen per strip (few PPA jobs per frame); smaller if PSRAM is tight
            for (const int div : { 4, 10 }) {
                bytes = ((size_t)s_w * s_h / div * 2 + kAlign - 1) & ~(kAlign - 1);
                s_part[0] = (uint8_t *)heap_caps_aligned_alloc(kAlign, bytes, MALLOC_CAP_SPIRAM);
                s_part[1] = (uint8_t *)heap_caps_aligned_alloc(kAlign, bytes, MALLOC_CAP_SPIRAM);
                if (s_part[0] && s_part[1]) { s_part_bytes = bytes; break; }
                free_strips();
            }
        }
        bytes = s_part_bytes;
        if (!s_ppa) {
            ppa_client_config_t c = {};
            c.oper_type = PPA_OPERATION_SRM;
            if (ppa_register_client(&c, &s_ppa) != ESP_OK) s_ppa = nullptr;
        }
        if (!s_part[0] || !s_ppa) {
            NV_LOGE(TAG, "no memory for rotated rendering: staying in landscape");
            free_strips();
            lv_display_set_rotation(s_disp, LV_DISPLAY_ROTATION_0);   // re-enters with s_mode_busy set
            lv_display_set_buffers(s_disp, s_fb[s_front ^ 1], s_fb[s_front], s_fb_bytes,
                                   LV_DISPLAY_RENDER_MODE_DIRECT);
            s_rotated = false;
            rot = LV_DISPLAY_ROTATION_0;
        } else {
            lv_display_set_buffers(s_disp, s_part[0], s_part[1], bytes, LV_DISPLAY_RENDER_MODE_PARTIAL);
            s_rotated = true;
        }
        s_prev.clear();
        s_cur.clear();
        s_inv.clear();
    }
    s_rot = rot;
    s_mode_busy = false;
    NV_LOGI(TAG, "rotation %d: %s", rot * 90,
            s_rotated ? "partial rendering, PPA rotation into the back buffer" : "direct rendering");
}

}  // namespace

lv_display_t *nv_disp_create(esp_lcd_panel_handle_t panel, int hres, int vres) {
    if (s_disp || !panel) return s_disp;
    s_panel = panel;
    s_w = hres;
    s_h = vres;
    s_fb_bytes = (size_t)hres * vres * 2;
    void *fb0 = nullptr, *fb1 = nullptr;
    if (esp_lcd_dpi_panel_get_frame_buffer(panel, 2, &fb0, &fb1) != ESP_OK || !fb0 || !fb1) {
        NV_LOGE(TAG, "the DPI panel must be created with num_fbs = 2");
        return nullptr;
    }
    s_fb[0] = (uint8_t *)fb0;
    s_fb[1] = (uint8_t *)fb1;
    // Row-granular invalidation (copy_rect) needs cache-line aligned rows; esp_cache_msync refuses
    // an unaligned M2C range, so ask it directly (invalidating a row of a fresh buffer is harmless).
    const size_t row = (size_t)hres * 2;
    if (esp_cache_msync(fb0, row, ESP_CACHE_MSYNC_FLAG_DIR_M2C) == ESP_OK &&
        esp_cache_msync(fb1, row, ESP_CACHE_MSYNC_FLAG_DIR_M2C) == ESP_OK)
        s_cache_align = true;
    else
        NV_LOGW(TAG, "frame buffer rows not cache-line aligned: DMA-written pixels copied uncached");
    s_front = 0;   // the DPI driver starts scanning buffer 0
    s_lock = xSemaphoreCreateMutex();
    s_vsync = xSemaphoreCreateBinary();
    if (!s_lock || !s_vsync) { NV_LOGE(TAG, "no memory"); return nullptr; }

    esp_lcd_dpi_panel_event_callbacks_t cbs = {};
    cbs.on_refresh_done = on_vsync;
    if (esp_lcd_dpi_panel_register_event_callbacks(panel, &cbs, nullptr) != ESP_OK) {
        NV_LOGE(TAG, "vsync callback registration failed");
        return nullptr;
    }

    lvgl_port_lock(0);
    lv_display_t *disp = lv_display_create(hres, vres);
    if (disp) {
        s_disp = disp;
        lv_display_set_color_format(disp, LV_COLOR_FORMAT_RGB565);
        lv_display_set_buffers(disp, s_fb[1], s_fb[0], s_fb_bytes, LV_DISPLAY_RENDER_MODE_DIRECT);
        lv_display_set_flush_cb(disp, flush_cb);
        lv_display_set_flush_wait_cb(disp, flush_wait_cb);
        lv_display_add_event_cb(disp, event_cb, LV_EVENT_INVALIDATE_AREA, nullptr);
        lv_display_add_event_cb(disp, event_cb, LV_EVENT_REFR_REQUEST, nullptr);
        lv_display_add_event_cb(disp, event_cb, LV_EVENT_REFR_START, nullptr);
        lv_display_add_event_cb(disp, event_cb, LV_EVENT_REFR_READY, nullptr);
        lv_display_add_event_cb(disp, event_cb, LV_EVENT_RENDER_START, nullptr);
        lv_display_add_event_cb(disp, event_cb, LV_EVENT_RESOLUTION_CHANGED, nullptr);
        s_layer_pump_ms = kPumpIdleMs;
        s_layer_pump = lv_timer_create(layer_pump, s_layer_pump_ms, nullptr);
    }
    lvgl_port_unlock();
    if (!disp) { NV_LOGE(TAG, "lv_display_create failed"); return nullptr; }
    NV_LOGI(TAG, "double-buffered %dx%d, swap at vsync, direct rendering", hres, vres);
    return disp;
}

bool nv_disp_front_begin(nv_disp_surface_t *out, uint32_t timeout_ms) {
    if (!s_lock || !out) return false;
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(timeout_ms)) != pdTRUE) return false;
    out->px = (uint16_t *)s_fb[s_front];
    out->w = s_w;
    out->h = s_h;
    out->stride = s_w;
    return true;
}

void nv_disp_front_end(void) {
    if (s_lock) xSemaphoreGive(s_lock);
}

void nv_disp_set_direct_region(int x, int y, int w, int h) {
    if (!s_lock) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (w <= 0 || h <= 0) {
        s_region_on = false;
    } else {
        const int x2 = x + w - 1 < s_w - 1 ? x + w - 1 : s_w - 1;
        const int y2 = y + h - 1 < s_h - 1 ? y + h - 1 : s_h - 1;
        s_region = { x < 0 ? 0 : x, y < 0 ? 0 : y, x2, y2 };
        s_region_on = s_region.x1 <= s_region.x2 && s_region.y1 <= s_region.y2;
    }
    xSemaphoreGive(s_lock);
}

void nv_disp_layer_set(const nv_disp_layer_t *layer) {
    if (!s_lock) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_layer_on) {   // leaving this rectangle: see layer_unify
        const Rect r = layer_rect();
        if (s_layer_unify) {
            s_layer_last = { s_layer_last.x1 < r.x1 ? s_layer_last.x1 : r.x1, s_layer_last.y1 < r.y1 ? s_layer_last.y1 : r.y1,
                             s_layer_last.x2 > r.x2 ? s_layer_last.x2 : r.x2, s_layer_last.y2 > r.y2 ? s_layer_last.y2 : r.y2 };
        } else {
            s_layer_last = r;
        }
        s_layer_unify = true;
    }
    s_layer_on = layer && layer->latest && layer->draw && layer->w > 0 && layer->h > 0;
    if (s_layer_on) {
        s_layer = *layer;
        if (s_layer.x < 0) { s_layer.w += s_layer.x; s_layer.x = 0; }
        if (s_layer.y < 0) { s_layer.h += s_layer.y; s_layer.y = 0; }
        if (s_layer.x + s_layer.w > s_w) s_layer.w = s_w - s_layer.x;
        if (s_layer.y + s_layer.h > s_h) s_layer.h = s_h - s_layer.y;
        s_layer_on = s_layer.w > 0 && s_layer.h > 0;
    }
    s_layer_gen[0] = s_layer_gen[1] = 0;
    s_layer_failed = 0;
    s_layer_draw_est = s_layer_copy_est = 0;   // new geometry, new costs
    const bool on = s_layer_on;
    xSemaphoreGive(s_lock);
    if (on) nv_disp_layer_update();   // first picture (a detach unifies on LVGL's next frame)
}

void nv_disp_layer_update(void) {
    if (!s_lock) return;
    // Kick right away when LVGL is free (1 ms at most); when it is busy, the end of its refresh
    // (LV_EVENT_REFR_READY) or the pump kicks instead. Never blocks on a render.
    if (lvgl_port_lock(1)) {
        layer_kick();
        lvgl_port_unlock();
    }
    lvgl_port_task_wake(LVGL_PORT_EVENT_DISPLAY, nullptr);
}

void nv_disp_get_stats(nv_disp_stats_t *out) {
    if (!out) return;
    *out = s_st;
    out->vsyncs = s_vsyncs;
    out->rotated = s_rotated;
    out->rotation = s_rot;
}
