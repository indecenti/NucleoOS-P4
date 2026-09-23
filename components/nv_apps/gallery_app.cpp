// gallery_app — photos and videos on the microSD card.
//   Scan  : on the background worker, /sdcard + /sdcard/Photos + /sdcard/DCIM (or an opened
//           file's folder): images (.jpg/.jpeg/.png/.bmp) and videos (.avi/.mpg/.mpeg/.mp4),
//           NEWEST FIRST, capped at kMaxItems. Image headers are probed (no raster).
//   Grid  : 16:9 tiles (4 columns in landscape). JPEG photos and Motion-JPEG videos (the camera's
//           AVIs: their first frame) get a GALLERY_THUMB_W x H thumbnail from gallery_thumb_cache,
//           held in PSRAM and drawn 1:1. Scrolling therefore costs plain RAM blits: no SD read per
//           redraw (the old .bin files overflowed LVGL's 2 MB image cache and were re-read from SD
//           while scrolling), no decode, no image transform, no translucent caption strips.
//           Videos carry a play badge.
//   Viewer: one item at a time on black, controls in the side margins. Photos: HW JPEG decode +
//           PPA fit into one reused buffer (PNG/BMP: LVGL's SW decoder). Videos: the first frame
//           and a play button that opens the video player.
// Navigation between grid and viewer is DEFERRED via lv_async_call: both builders run
// lv_obj_clean(nv_ui_app_content()), which would free the subtree whose child fired the
// triggering event (tap / swipe / Back) — a use-after-free. Scheduling the rebuild for the
// next LVGL loop lets the event fully unwind first.
#include "apps_internal.h"

#include "nv_app.h"
#include "nv_ui_host.h"
#include "nv_gesture.h"
#include "nv_icons.h"
#include "nv_i18n.h"
#include "nv_fonts.h"
#include "nv_theme.h"
#include "nv_notify.h"
#include "gallery_jpeg_hw.h"
#include "gallery_thumb_cache.h"
#include "nv_bgwork.h"
#include "nv_mem_attr.h"   // NV_PSRAM_BSS: item table / queues out of internal SRAM
#include "nv_open.h"       // opened on a photo: viewer on its folder; file actions; play videos
// LVGL 9.5 declares this in src/misc/cache/instance/lv_image_cache.h, which lvgl.h does not pull in.
extern "C" void lv_image_cache_drop(const void *src);

#include "lvgl.h"
#include "esp_lvgl_port.h"   // lvgl_port_lock: the workers apply results cross-thread
#include "esp_heap_caps.h"

#include <dirent.h>
#include <sys/stat.h>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cctype>
#include <strings.h>

namespace {

// ---------------------------------------------------------------- data model
constexpr int     kMaxItems   = 60;    // shown (the newest); each holds a ~72 KB thumbnail
constexpr int     kMaxScan    = 256;   // candidates considered before the newest-first cut
constexpr int     kMaxPathLen = 160;
constexpr int     kMaxNameLen = 64;
constexpr int32_t kMaxDecodeW = 2048;  // oversized guard (px): a 4000x3000 phone photo
constexpr int32_t kMaxDecodeH = 2048;  // exceeds this -> placeholder, never decoded

enum class Kind : uint8_t { Jpeg, Image, Avi, Video };   // Image = PNG/BMP, Video = MPEG-1/MP4

struct GalleryItem {
    char     path[kMaxPathLen];   // POSIX path, e.g. "/sdcard/DCIM/IMG_20260923_232457.jpg"
    char     name[kMaxNameLen];   // basename
    uint16_t w, h;                // photos: header probe (0/0 if it failed)
    uint32_t size;                // bytes, for the viewer's info line
    Kind     kind;
    bool     probe_ok;            // decodable
    bool     oversized;           // over kMaxDecode*: never decoded
    bool     thumb_failed;        // the worker couldn't make a thumbnail: placeholder
    uint8_t *thumb_px;            // PSRAM thumbnail pixels (owned), or null
    lv_image_dsc_t thumb_dsc;     // describes thumb_px; its address is the LVGL image src
};

NV_PSRAM_BSS GalleryItem s_items[kMaxItems];   // LVGL thread + scan worker (under the port lock)
int s_item_count = 0;

enum class ScanResult { Images, NoCard, NoImages };
ScanResult s_scan_result = ScanResult::NoImages;

int s_index = 0;
int s_grid_focus = -1;   // tile to scroll into view when returning from the viewer

bool is_video(const GalleryItem &it) { return it.kind == Kind::Avi || it.kind == Kind::Video; }
bool wants_thumb(const GalleryItem &it) {
    return (it.kind == Kind::Jpeg && it.probe_ok && !it.oversized) || it.kind == Kind::Avi;
}

// LVGL "S:" source for the SW decoders (PNG/BMP, header probe). They match the extension
// case-sensitively and camera files are often .JPG: FAT is case-insensitive, so lowercasing the
// extension still opens the real file.
void lv_src(const char *posix, char *out, size_t n) {
    snprintf(out, n, "S:%s", posix);
    if (char *dot = strrchr(out, '.'))
        for (char *p = dot + 1; *p; ++p) *p = (char)tolower((unsigned char)*p);
}

// Release every thumbnail. LVGL may still cache an entry keyed by a thumb_dsc address, so the
// whole image cache is dropped first (cheap; only ever on teardown / delete).
void free_thumbs(void) {
    lv_image_cache_drop(nullptr);
    for (int i = 0; i < kMaxItems; i++) {
        GalleryItem &it = s_items[i];
        if (it.thumb_px) heap_caps_free(it.thumb_px);
        it.thumb_px = nullptr;
    }
}

// ---------------------------------------------------------------- deferred navigation
enum class Page { Grid, Viewer };
Page s_pending_page  = Page::Grid;
int  s_pending_index = 0;
bool s_nav_pending   = false;   // coalesce a burst + guard a failed schedule from wedging nav

void build_grid(void);
void build_viewer(int index);

void nav_apply(void *) {
    s_nav_pending = false;
    if (s_pending_page == Page::Grid) build_grid();
    else                              build_viewer(s_pending_index);
}

void nav_to(Page p, int index) {
    s_pending_page  = p;
    s_pending_index = index;
    if (s_nav_pending) return;
    if (lv_async_call(nav_apply, nullptr) == LV_RESULT_OK) s_nav_pending = true;
}

// Opened on a photo by another app (nv_open "gallery.view"): only that photo's folder is scanned,
// the viewer opens straight on it, and leaving the viewer returns to the app that asked.
NV_PSRAM_BSS char s_intent_dir[NV_OPEN_PATH_MAX];
NV_PSRAM_BSS char s_intent_path[NV_OPEN_PATH_MAX];
bool s_intent_mode = false;
bool s_intent_open = false;

void go_grid(void) {
    if (s_intent_mode) { nv_open_finish(); return; }
    s_grid_focus = s_index;
    nav_to(Page::Grid, 0);
}

// ---------------------------------------------------------------- SD scan
bool ext_is(const char *name, const char *const *exts) {
    const char *dot = strrchr(name, '.');
    if (!dot || dot == name) return false;
    for (; *exts; ++exts)
        if (!strcasecmp(dot + 1, *exts)) return true;
    return false;
}
const char *const kJpegExt[]  = {"jpg", "jpeg", nullptr};
const char *const kImageExt[] = {"png", "bmp", nullptr};
const char *const kAviExt[]   = {"avi", nullptr};
const char *const kVideoExt[] = {"mpg", "mpeg", "mp4", nullptr};

bool media_kind(const char *name, Kind *k) {
    if (ext_is(name, kJpegExt))  { *k = Kind::Jpeg;  return true; }
    if (ext_is(name, kImageExt)) { *k = Kind::Image; return true; }
    if (ext_is(name, kAviExt))   { *k = Kind::Avi;   return true; }
    if (ext_is(name, kVideoExt)) { *k = Kind::Video; return true; }
    return false;
}

// Walk JPEG segment markers to the first SOFn. Only baseline (SOF0) is decodable (the HW decoder
// and TJPGD) — progressive (SOF2) parses a valid header but fails the raster decode.
bool jpeg_is_baseline(const char *posix_path) {
    FILE *f = fopen(posix_path, "rb");
    if (!f) return true;
    bool baseline = true;
    const int b0 = fgetc(f), b1 = fgetc(f);
    if (b0 == 0xFF && b1 == 0xD8) {
        for (;;) {
            int c = fgetc(f);
            if (c == EOF) break;
            if (c != 0xFF) continue;
            int m;
            do { m = fgetc(f); } while (m == 0xFF);
            if (m == EOF || m == 0xD9 || m == 0xDA) break;
            if (m == 0x01 || (m >= 0xD0 && m <= 0xD7)) continue;
            const int hi = fgetc(f), lo = fgetc(f);
            if (hi == EOF || lo == EOF) break;
            const int len = ((hi << 8) | lo) - 2;
            if (m >= 0xC0 && m <= 0xCF && m != 0xC4 && m != 0xC8 && m != 0xCC) {
                baseline = (m == 0xC0);
                break;
            }
            if (len > 0) fseek(f, len, SEEK_CUR);
        }
    }
    fclose(f);
    return baseline;
}

struct Cand { const char *dir; char name[kMaxNameLen]; time_t mtime; uint32_t size; };

// Newest first; equal times (FAT has 2 s resolution) fall back to the name, descending, which
// keeps the camera's IMG_/VID_YYYYMMDD_HHMMSS files in shooting order.
int cand_cmp(const void *a, const void *b) {
    const Cand *x = (const Cand *)a, *y = (const Cand *)b;
    if (x->mtime != y->mtime) return x->mtime > y->mtime ? -1 : 1;
    return -strcmp(x->name, y->name);
}

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat-truncation"

// Enumerate one directory (non-recursive) into the candidate table. Returns whether it existed
// (so "no card" can be told from "no media").
bool collect_dir(const char *dir, Cand *cand, int *n) {
    DIR *d = opendir(dir);
    if (!d) return false;
    struct dirent *e;
    while ((e = readdir(d)) != nullptr && *n < kMaxScan) {
        if (e->d_name[0] == '.' || e->d_type == DT_DIR) continue;
        if (strlen(e->d_name) >= (size_t)kMaxNameLen) continue;   // would not round-trip
        Kind k;
        if (!media_kind(e->d_name, &k)) continue;
        char full[kMaxPathLen];
        snprintf(full, sizeof full, "%s/%s", dir, e->d_name);
        struct stat st;
        if (stat(full, &st) != 0 || !S_ISREG(st.st_mode)) continue;
        Cand &c = cand[(*n)++];
        c.dir = dir;
        snprintf(c.name, sizeof c.name, "%s", e->d_name);
        c.mtime = st.st_mtime;
        c.size = (uint32_t)st.st_size;
    }
    closedir(d);
    return true;
}

// Fill s_items[s_item_count] from one candidate: kind, and for images a header probe.
void add_item(const Cand &c) {
    GalleryItem &it = s_items[s_item_count];
    memset(&it, 0, sizeof it);
    snprintf(it.path, sizeof it.path, "%s/%s", c.dir, c.name);
    snprintf(it.name, sizeof it.name, "%s", c.name);
    it.size = c.size;
    media_kind(c.name, &it.kind);
    if (is_video(it)) {
        it.probe_ok = true;   // playability is the player's call; the tile shows what it can
    } else {
        // LVGL's decoder/cache internals are not thread-safe: take the (recursive) port lock for
        // just this small header read, so the UI sees short pauses rather than one long freeze.
        char src[kMaxPathLen + 2];
        lv_src(it.path, src, sizeof src);
        lv_image_header_t hdr;
        lv_memzero(&hdr, sizeof hdr);
        bool info_ok = false;
        if (lvgl_port_lock(2000)) {
            info_ok = lv_image_decoder_get_info(src, &hdr) == LV_RESULT_OK;
            lvgl_port_unlock();
        }
        if (info_ok) {
            it.probe_ok  = true;
            it.w         = (uint16_t)hdr.w;
            it.h         = (uint16_t)hdr.h;
            it.oversized = (hdr.w > kMaxDecodeW || hdr.h > kMaxDecodeH);
            if (it.kind == Kind::Jpeg && !jpeg_is_baseline(it.path)) it.probe_ok = false;
        }
    }
    s_item_count++;
}
#pragma GCC diagnostic pop

// Rebuild s_items from scratch (once per app open, on the worker).
void scan_sdcard(void) {
    s_item_count = 0;
    Cand *cand = (Cand *)heap_caps_malloc(sizeof(Cand) * kMaxScan, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!cand) { s_scan_result = ScanResult::NoImages; return; }
    int n = 0;
    bool have_root;
    if (s_intent_dir[0]) {
        have_root = collect_dir(s_intent_dir, cand, &n);
    } else {
        have_root = collect_dir("/sdcard", cand, &n);
        collect_dir("/sdcard/Photos", cand, &n);
        collect_dir("/sdcard/DCIM", cand, &n);
    }
    qsort(cand, (size_t)n, sizeof(Cand), cand_cmp);
    for (int i = 0; i < n && s_item_count < kMaxItems; i++) add_item(cand[i]);
    heap_caps_free(cand);

    if (!have_root)             s_scan_result = ScanResult::NoCard;
    else if (s_item_count == 0) s_scan_result = ScanResult::NoImages;
    else                        s_scan_result = ScanResult::Images;
}

// ---------------------------------------------------------------- shared placeholder
void fill_placeholder(lv_obj_t *parent, const char *sym, bool on_black) {
    const NvTheme *th = nv_theme_get();
    lv_obj_t *l = lv_label_create(parent);
    lv_label_set_text(l, sym);
    lv_obj_set_style_text_font(l, &nv_font_28, 0);
    lv_obj_set_style_text_color(l, on_black ? lv_color_hex(0x8A8A94) : th->text_dim, 0);
    lv_obj_center(l);
}
const char *placeholder_sym(const GalleryItem &it) {
    if (is_video(it)) return LV_SYMBOL_VIDEO;
    if (!it.probe_ok) return LV_SYMBOL_WARNING;
    return LV_SYMBOL_IMAGE;
}

// ---------------------------------------------------------------- background thumbnail loader
// The grid paints at once: tiles whose thumbnail is already in RAM show it, the rest start as a
// plain surface and are handed (in grid order, newest first) to one batch job on nv_bgwork. For
// each it reads the cached thumbnail from SD, or builds it (HW decode + PPA + SD write), then
// installs it under the LVGL lock. The job carries LVGL-thread COPIES of the paths and the grid
// generation; teardown bumps the generation, so stale results are freed instead of installed.
NV_PSRAM_BSS lv_obj_t *s_tiles[kMaxItems];   // grid tiles (lv_image), valid while the grid lives
volatile uint32_t s_thumb_gen = 0;
bool s_scanned = false;   // scan once per app open; grid<->viewer nav must not re-probe the SD

struct ThumbEntry { int idx; bool video; char posix[kMaxPathLen]; };
struct ThumbBatch { uint32_t gen; int n; ThumbEntry e[]; };

void thumb_builder_stop(void) {
    s_thumb_gen = s_thumb_gen + 1;
    for (int i = 0; i < kMaxItems; i++) s_tiles[i] = nullptr;
}

void show_thumb(lv_obj_t *tile, GalleryItem &it) {
    lv_obj_clean(tile);   // placeholder glyph, if any (the play badge is re-added by the caller)
    lv_image_set_src(tile, &it.thumb_dsc);
}
void add_play_badge(lv_obj_t *tile, int d);

void thumb_batch_job(void *arg) {
    ThumbBatch *b = (ThumbBatch *)arg;
    int built = 0;
    for (int k = 0; k < b->n && b->gen == s_thumb_gen; k++) {   // racy read: early exit only
        ThumbEntry &te = b->e[k];
        lv_image_dsc_t dsc;
        uint8_t *px = nullptr;
        bool did_build = false;
        const bool ok = gallery_thumb_get(te.posix, te.video, &dsc, &px, &did_build);
        if (did_build) built++;
        if (built == 6 && lvgl_port_lock(1000)) {   // a real backlog: say why tiles fill slowly
            if (b->gen == s_thumb_gen) nv_toast(NV_NOTE_INFO, nv_tr(NV_STR_GENERATING_THUMBS));
            lvgl_port_unlock();
        }
        if (!lvgl_port_lock(1000)) { if (px) heap_caps_free(px); continue; }
        if (b->gen == s_thumb_gen && te.idx < s_item_count && !strcmp(s_items[te.idx].path, te.posix)) {
            GalleryItem &it = s_items[te.idx];
            lv_obj_t *tile = s_tiles[te.idx];
            if (ok && !it.thumb_px) {
                it.thumb_px = px;
                it.thumb_dsc = dsc;
                px = nullptr;
                if (tile) {
                    show_thumb(tile, it);
                    if (is_video(it)) add_play_badge(tile, 48);
                }
            } else if (!ok) {
                it.thumb_failed = true;
                if (tile && lv_obj_get_child_count(tile) == 0) {
                    if (is_video(it)) add_play_badge(tile, 48);   // the badge alone says "video"
                    else              fill_placeholder(tile, placeholder_sym(it), false);
                }
            }
        }
        lvgl_port_unlock();
        if (px) heap_caps_free(px);   // stale: grid gone, item moved, or already loaded
    }
    free(b);
}

void grid_deleted(lv_event_t *) { thumb_builder_stop(); }

// ============================================================ grid page
void tile_cb(lv_event_t *e) {
    nav_to(Page::Viewer, (int)(intptr_t)lv_event_get_user_data(e));
}

// Round play mark over a video's thumbnail. Small and shared by grid and viewer; the only
// translucent element on a tile, so the thumbnail itself still blits as plain RGB565.
void add_play_badge(lv_obj_t *parent, int d) {
    lv_obj_t *c = lv_obj_create(parent);
    lv_obj_remove_style_all(c);
    lv_obj_set_size(c, d, d);
    lv_obj_center(c);
    lv_obj_set_style_radius(c, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(c, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(c, LV_OPA_50, 0);
    lv_obj_set_style_border_width(c, 2, 0);
    lv_obj_set_style_border_color(c, lv_color_white(), 0);
    lv_obj_remove_flag(c, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(c, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *l = lv_label_create(c);
    lv_label_set_text(l, LV_SYMBOL_PLAY);
    lv_obj_set_style_text_font(l, d >= 64 ? &nv_font_28 : &nv_font_20, 0);
    lv_obj_set_style_text_color(l, lv_color_white(), 0);
    lv_obj_align(l, LV_ALIGN_CENTER, d / 20, 0);   // optical centre of the triangle
}

void build_empty_state(lv_obj_t *content, const NvTheme *th) {
    lv_obj_t *col = lv_obj_create(content);
    lv_obj_remove_style_all(col);
    lv_obj_set_size(col, lv_pct(100), lv_pct(100));
    lv_obj_set_style_pad_all(col, 24, 0);
    lv_obj_set_style_pad_row(col, 10, 0);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(col, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(col, LV_OBJ_FLAG_SCROLLABLE);

    const bool no_card = (s_scan_result == ScanResult::NoCard);
    lv_obj_t *icon = lv_label_create(col);
    lv_label_set_text(icon, no_card ? LV_SYMBOL_SD_CARD : LV_SYMBOL_IMAGE);
    lv_obj_set_style_text_font(icon, &nv_font_28, 0);
    lv_obj_set_style_text_color(icon, th->text_dim, 0);

    lv_obj_t *primary = lv_label_create(col);
    lv_label_set_text(primary, nv_tr(no_card ? NV_STR_SD_MISSING : NV_STR_NO_PHOTOS));
    lv_obj_set_style_text_font(primary, &nv_font_20, 0);
    lv_obj_set_style_text_color(primary, th->text_strong, 0);

    lv_obj_t *hint = lv_label_create(col);
    lv_label_set_text(hint, nv_tr(NV_STR_GAL_HINT));
    lv_obj_set_style_text_color(hint, th->text_dim, 0);
    lv_obj_set_style_text_align(hint, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(hint, lv_pct(90));
    lv_label_set_long_mode(hint, LV_LABEL_LONG_WRAP);
}

void build_grid(void) {
    lv_obj_t *content = nv_ui_app_content();   // re-read: never cache across a deferred switch
    if (!content) return;
    thumb_builder_stop();                      // orphan any in-flight batch BEFORE freeing tiles
    lv_obj_clean(content);
    nv_ui_set_back(nullptr);                   // grid: Back closes the app
    nv_ui_set_title(nv_tr(NV_STR_APP_GALLERY));
    const NvTheme *th = nv_theme_get();
    lv_obj_set_style_bg_color(content, th->bg, 0);
    lv_obj_set_style_bg_opa(content, LV_OPA_COVER, 0);

    // Scan the SD ONCE per app open, on the BACKGROUND worker (directory walks + stats + header
    // probes are hundreds of ms of SD I/O). A spinner shows meanwhile; the worker re-enters
    // build_grid under the LVGL lock when s_items is ready (generation-guarded).
    if (!s_scanned) {
        s_scanned = true;
        lv_obj_t *sp = lv_spinner_create(content);
        lv_obj_set_size(sp, 48, 48);
        lv_obj_center(sp);
        // The spinner is the scan's teardown sentinel: it dies on app close (content clean),
        // bumping the generation so the worker's pending rebuild is dropped.
        lv_obj_add_event_cb(sp, [](lv_event_t *) { s_thumb_gen = s_thumb_gen + 1; }, LV_EVENT_DELETE, nullptr);
        struct ScanJob { uint32_t gen; };
        ScanJob *j = (ScanJob *)malloc(sizeof(ScanJob));
        if (j) {
            j->gen = s_thumb_gen;
            const bool queued = nv_bgwork_submit(
                [](void *arg) {
                    ScanJob *s = (ScanJob *)arg;
                    scan_sdcard();
                    for (int attempt = 0; attempt < 10; attempt++) {
                        if (s->gen != s_thumb_gen) break;
                        if (lvgl_port_lock(1000)) {
                            if (s->gen == s_thumb_gen) build_grid();
                            lvgl_port_unlock();
                            break;
                        }
                    }
                    free(s);
                },
                j);
            if (queued) return;
            free(j);
        }
        scan_sdcard();               // worker unavailable (rare): synchronous scan
        lv_obj_delete(sp);
    }
    if (s_intent_open) {   // scan is in: jump to the opened file (FAT is case-insensitive)
        s_intent_open = false;
        for (int i = 0; i < s_item_count; i++)
            if (!strcasecmp(s_items[i].path, s_intent_path)) { build_viewer(i); return; }
    }
    if (s_item_count == 0) { build_empty_state(content, th); return; }

    // Tile geometry: 16:9, never wider than the thumbnail (it is drawn 1:1, centred and clipped).
    lv_obj_update_layout(content);
    const int cw = lv_obj_get_content_width(content);
    constexpr int kPad = 12, kGap = 6;
    int cols = cw >= 800 ? 4 : 3;
    int tile_w = (cw - 2 * kPad - (cols - 1) * kGap) / cols;
    while (tile_w > GALLERY_THUMB_W) { cols++; tile_w = (cw - 2 * kPad - (cols - 1) * kGap) / cols; }
    const int tile_h = tile_w * 9 / 16;

    lv_obj_t *list = lv_obj_create(content);
    lv_obj_remove_style_all(list);
    lv_obj_set_size(list, lv_pct(100), lv_pct(100));
    lv_obj_set_style_pad_all(list, kPad, 0);
    lv_obj_set_style_pad_row(list, kGap, 0);
    lv_obj_set_style_pad_column(list, kGap, 0);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_scroll_dir(list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_ACTIVE);
    lv_obj_add_event_cb(list, grid_deleted, LV_EVENT_DELETE, nullptr);

    // header: one line, full width
    lv_obj_t *hdr = lv_label_create(list);
    lv_obj_set_width(hdr, lv_pct(100));
    lv_obj_set_style_pad_bottom(hdr, 4, 0);
    lv_obj_set_style_text_font(hdr, &nv_font_20, 0);
    lv_obj_set_style_text_color(hdr, th->text_strong, 0);
    char count[32];
    snprintf(count, sizeof count, nv_tr(NV_STR_ITEMS_FMT), s_item_count);
    lv_label_set_text_fmt(hdr, "%s  \xC2\xB7  %s", nv_tr(NV_STR_GAL_TITLE), count);

    ThumbBatch *b = (ThumbBatch *)heap_caps_malloc(sizeof(ThumbBatch) + (size_t)s_item_count * sizeof(ThumbEntry),
                                                   MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (b) { b->gen = s_thumb_gen; b->n = 0; }

    for (int i = 0; i < s_item_count; i++) {
        GalleryItem &it = s_items[i];
        // The tile IS the image: one object per item, drawn 1:1 over a flat surface.
        lv_obj_t *tile = lv_image_create(list);
        lv_obj_set_size(tile, tile_w, tile_h);
        lv_image_set_inner_align(tile, LV_IMAGE_ALIGN_CENTER);
        lv_obj_set_style_bg_color(tile, th->surface2, 0);
        lv_obj_set_style_bg_opa(tile, LV_OPA_COVER, 0);
        lv_obj_set_style_border_color(tile, th->primary, LV_STATE_PRESSED);
        lv_obj_set_style_border_width(tile, 3, LV_STATE_PRESSED);
        lv_obj_add_flag(tile, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(tile, tile_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        s_tiles[i] = tile;

        if (it.thumb_px) {
            lv_image_set_src(tile, &it.thumb_dsc);
        } else if (wants_thumb(it) && !it.thumb_failed) {
            if (b) {
                ThumbEntry &te = b->e[b->n++];
                te.idx = i;
                te.video = it.kind == Kind::Avi;
                memcpy(te.posix, it.path, sizeof te.posix);   // same size, NUL-terminated
            }
        } else if (!is_video(it)) {
            fill_placeholder(tile, placeholder_sym(it), false);
        }
        if (is_video(it) && (it.thumb_px || !wants_thumb(it) || it.thumb_failed)) add_play_badge(tile, 48);
    }

    // Best-effort: a full worker queue just leaves blank tiles until the next grid build.
    if (b && (b->n == 0 || !nv_bgwork_submit(thumb_batch_job, b))) free(b);

    if (s_grid_focus >= 0 && s_grid_focus < s_item_count) {   // back from the viewer
        lv_obj_update_layout(list);
        lv_obj_scroll_to_view(s_tiles[s_grid_focus], LV_ANIM_OFF);
    }
    s_grid_focus = -1;
}

// ============================================================ viewer page
// One item on black. The photo is fitted into a buffer the size of the content area and drawn
// 1:1; the controls sit in the margins that the 1/16-step fit leaves beside a 16:9 picture.
lv_obj_t *s_photo = nullptr;
lv_obj_t *s_stage = nullptr;
lv_obj_t *s_counter = nullptr, *s_info = nullptr, *s_prev = nullptr, *s_next = nullptr;
lv_obj_t *s_side = nullptr;   // top-right column: file actions + delete
lv_obj_t *s_trash_icon = nullptr;
bool s_chrome = true;
bool s_del_armed = false;

uint8_t       *s_viewer_buf = nullptr;
size_t         s_viewer_cap = 0;
int            s_viewer_w = 0, s_viewer_h = 0;
lv_image_dsc_t s_viewer_dsc;

void free_viewer_buf(void) {
    if (!s_viewer_buf) return;
    lv_image_cache_drop(&s_viewer_dsc);
    heap_caps_free(s_viewer_buf);
    s_viewer_buf = nullptr;
    s_viewer_cap = 0;
    s_viewer_w = s_viewer_h = 0;
}

bool ensure_viewer_buf(void) {
    lv_obj_t *content = nv_ui_app_content();
    if (!content) return false;
    const int w = lv_obj_get_width(content), h = lv_obj_get_height(content);
    if (w <= 0 || h <= 0) return false;
    if (s_viewer_buf && (w != s_viewer_w || h != s_viewer_h)) free_viewer_buf();   // rotated
    if (s_viewer_buf) return true;
    const size_t cap = gallery_ppa_align_size((size_t)w * h * 2);
    uint8_t *buf = (uint8_t *)heap_caps_aligned_alloc(GALLERY_PPA_ALIGN, cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) return false;
    s_viewer_buf = buf;
    s_viewer_cap = cap;
    s_viewer_w = w;
    s_viewer_h = h;
    return true;
}

// Solid round control (no translucency: nothing to blend over the photo each frame).
lv_obj_t *round_btn(lv_obj_t *parent, int d, const char *sym, lv_event_cb_t cb, void *user) {
    lv_obj_t *b = lv_obj_create(parent);
    lv_obj_remove_style_all(b);
    lv_obj_set_size(b, d, d);
    lv_obj_set_style_radius(b, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(0x26262B), 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(0x3A3A42), LV_STATE_PRESSED);
    lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(b, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_ext_click_area(b, 8);
    if (cb) lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, user);
    lv_obj_t *l = lv_label_create(b);
    lv_label_set_text(l, sym);
    lv_obj_set_style_text_color(l, lv_color_white(), 0);
    lv_obj_center(l);
    return b;
}

void action_cb(lv_event_t *e) {
    const NvOpenHandler *h = (const NvOpenHandler *)lv_event_get_user_data(e);
    if (h && s_index >= 0 && s_index < s_item_count) nv_open_run(h, s_items[s_index].path);
}
void delete_cb(lv_event_t *);

// Top-right column: the OS's file actions for this item's type (e.g. "Set as wallpaper"), then
// delete. Rebuilt per item.
void refresh_side(const GalleryItem &it) {
    if (!s_side) return;
    lv_obj_clean(s_side);
    s_trash_icon = nullptr;
    if (it.probe_ok) {
        const NvOpenHandler *acts[3];
        const int n = nv_open_handlers(it.path, NV_OPEN_ACTION, acts, 3);
        const char *mime = nv_open_mime(it.path);
        for (int k = 0; k < n; k++) round_btn(s_side, 48, nv_open_handler_symbol(acts[k], mime), action_cb, (void *)acts[k]);
    }
    lv_obj_t *trash = round_btn(s_side, 48, LV_SYMBOL_TRASH, delete_cb, nullptr);
    s_trash_icon = lv_obj_get_child(trash, 0);
}

void refresh_chrome(int idx) {
    if (idx < 0 || idx >= s_item_count) return;
    const GalleryItem &it = s_items[idx];
    refresh_side(it);
    nv_ui_set_title(it.name);
    if (s_counter) lv_label_set_text_fmt(s_counter, "%d / %d", idx + 1, s_item_count);
    if (s_info) {
        const unsigned kb = (unsigned)(it.size / 1024);
        char sz[24];
        if (kb >= 1024) snprintf(sz, sizeof sz, "%u.%u MB", kb / 1024, (kb % 1024) * 10 / 1024);
        else            snprintf(sz, sizeof sz, "%u KB", kb);
        // Two short lines: they fit the ~90 px margin beside a 16:9 picture instead of covering it.
        if (it.w && it.h) lv_label_set_text_fmt(s_info, "%u\xC3\x97%u\n%s", it.w, it.h, sz);
        else              lv_label_set_text(s_info, sz);
    }
    auto edge = [](lv_obj_t *btn, bool hide) {
        if (!btn) return;
        if (hide || !s_chrome) lv_obj_add_flag(btn, LV_OBJ_FLAG_HIDDEN);
        else                   lv_obj_remove_flag(btn, LV_OBJ_FLAG_HIDDEN);
    };
    edge(s_prev, idx == 0);
    edge(s_next, idx == s_item_count - 1);
}

void play_cb(lv_event_t *) {
    if (s_index >= 0 && s_index < s_item_count) nv_open_file(s_items[s_index].path);   // deferred
}

// Show item `idx` on the stage: at most one full-size picture is decoded at a time.
void show_photo(int idx) {
    if (s_item_count == 0) return;
    if (idx < 0) idx = 0;
    if (idx >= s_item_count) idx = s_item_count - 1;
    s_index = idx;
    if (!s_stage) return;

    lv_obj_clean(s_stage);
    s_photo = nullptr;
    s_del_armed = false;

    GalleryItem &it = s_items[idx];
    gallery_raster_t r = {};
    bool decoded = false;
    if (ensure_viewer_buf()) {
        if (it.kind == Kind::Avi)
            decoded = gallery_jpeg_hw_decode_avi_poster(it.path, &r);
        else if (it.kind == Kind::Jpeg && it.probe_ok && !it.oversized)
            decoded = gallery_jpeg_hw_decode_file(it.path, &r);
    }
    if (decoded) {
        if (it.kind == Kind::Avi) { it.w = (uint16_t)r.w; it.h = (uint16_t)r.h; }
        memset(s_viewer_buf, 0, s_viewer_cap);   // black letterbox (PPA writes only the picture)
        if (gallery_ppa_scale_fit(&r, s_viewer_buf, s_viewer_w, s_viewer_h, s_viewer_cap)) {
            lv_image_cache_drop(&s_viewer_dsc);   // same descriptor, new pixels
            memset(&s_viewer_dsc, 0, sizeof s_viewer_dsc);
            s_viewer_dsc.header.magic  = LV_IMAGE_HEADER_MAGIC;
            s_viewer_dsc.header.cf     = LV_COLOR_FORMAT_RGB565;
            s_viewer_dsc.header.w      = (uint32_t)s_viewer_w;
            s_viewer_dsc.header.h      = (uint32_t)s_viewer_h;
            s_viewer_dsc.header.stride = (uint32_t)(s_viewer_w * 2);
            s_viewer_dsc.data_size = (uint32_t)s_viewer_cap;
            s_viewer_dsc.data      = s_viewer_buf;
            s_photo = lv_image_create(s_stage);
            lv_image_set_src(s_photo, &s_viewer_dsc);
            lv_obj_center(s_photo);
            lv_obj_remove_flag(s_photo, LV_OBJ_FLAG_CLICKABLE);   // taps reach the stage
        }
        gallery_jpeg_hw_free(&r);
    }
    if (!s_photo) {
        if (it.kind == Kind::Image && it.probe_ok && !it.oversized) {
            // PNG/BMP: LVGL's SW decoder, drawn 1:1 (a scaled draw is a slow transform here).
            char src[kMaxPathLen + 2];
            lv_src(it.path, src, sizeof src);
            s_photo = lv_image_create(s_stage);
            lv_image_set_src(s_photo, src);
            lv_obj_center(s_photo);
            lv_obj_remove_flag(s_photo, LV_OBJ_FLAG_CLICKABLE);
        } else {
            fill_placeholder(s_stage, it.oversized ? LV_SYMBOL_IMAGE : placeholder_sym(it), true);
        }
    }
    if (is_video(it)) {   // big play button over the poster: opens the video player
        lv_obj_t *play = lv_obj_create(s_stage);
        lv_obj_remove_style_all(play);
        lv_obj_set_size(play, 96, 96);
        lv_obj_center(play);
        lv_obj_set_style_radius(play, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(play, lv_color_white(), 0);
        lv_obj_set_style_bg_opa(play, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(play, lv_color_hex(0xD0D0D8), LV_STATE_PRESSED);
        lv_obj_add_flag(play, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_ext_click_area(play, 12);
        lv_obj_add_event_cb(play, play_cb, LV_EVENT_CLICKED, nullptr);
        lv_obj_t *l = lv_label_create(play);
        lv_label_set_text(l, LV_SYMBOL_PLAY);
        lv_obj_set_style_text_font(l, &nv_font_28, 0);
        lv_obj_set_style_text_color(l, lv_color_black(), 0);
        lv_obj_align(l, LV_ALIGN_CENTER, 4, 0);
    }
    refresh_chrome(idx);
}

void set_chrome_shown(bool show) {
    s_chrome = show;
    lv_obj_t *parts[] = {s_counter, s_info, s_side};
    for (lv_obj_t *p : parts) {
        if (!p) continue;
        if (show) lv_obj_remove_flag(p, LV_OBJ_FLAG_HIDDEN);
        else      lv_obj_add_flag(p, LV_OBJ_FLAG_HIDDEN);
    }
    refresh_chrome(s_index);   // arrows follow s_chrome and the ends of the list
}

void stage_tap_cb(lv_event_t *) { set_chrome_shown(!s_chrome); }

void viewer_swipe_down(void *) { go_grid(); }
// The gesture fires while the pressed child is still unwinding its own event dispatch, so
// show_photo() (which cleans s_stage) must not run inline: defer one LVGL loop.
void carousel_next_apply(void *) { show_photo(s_index + 1); }
void carousel_prev_apply(void *) { show_photo(s_index - 1); }
void viewer_swipe_left(void *)  { lv_async_call(carousel_next_apply, nullptr); }
void viewer_swipe_right(void *) { lv_async_call(carousel_prev_apply, nullptr); }
void prev_cb(lv_event_t *) { lv_async_call(carousel_prev_apply, nullptr); }
void next_cb(lv_event_t *) { lv_async_call(carousel_next_apply, nullptr); }

// Delete: 1st tap arms (icon turns red + toast), 2nd tap deletes and moves on (or back to the
// grid after the last one) — the files-app "tap again to confirm" idiom, no modal.
void delete_apply(void *) {
    if (s_item_count == 0 || s_index >= s_item_count) return;
    GalleryItem &it = s_items[s_index];
    if (remove(it.path) != 0) {
        nv_toast(NV_NOTE_ERROR, nv_tr(NV_STR_GAL_DELETE_FAILED));
        return;
    }
    gallery_thumb_cache_evict(it.path);
    nv_toast(NV_NOTE_OK, nv_tr(NV_STR_DELETE));
    // Every thumb_dsc address shifts below: no cached entry may keep pointing at the old pixels.
    lv_image_cache_drop(nullptr);
    if (it.thumb_px) heap_caps_free(it.thumb_px);
    for (int i = s_index; i < s_item_count - 1; i++) s_items[i] = s_items[i + 1];
    s_item_count--;
    memset(&s_items[s_item_count], 0, sizeof(GalleryItem));
    if (s_item_count == 0) { go_grid(); return; }
    show_photo(s_index >= s_item_count ? s_item_count - 1 : s_index);
}
void delete_cb(lv_event_t *) {
    if (s_item_count == 0) return;
    if (!s_del_armed) {
        s_del_armed = true;
        if (s_trash_icon) lv_obj_set_style_text_color(s_trash_icon, nv_theme_get()->danger, 0);
        nv_toast(NV_NOTE_WARN, nv_tr(NV_STR_TAP_AGAIN));
        return;
    }
    s_del_armed = false;
    lv_async_call(delete_apply, nullptr);   // show_photo rebuilds the column this button is in
}

void viewer_deleted(lv_event_t *) {
    if (s_photo) lv_image_set_src(s_photo, nullptr);
    free_viewer_buf();
    s_photo = s_stage = s_counter = s_info = s_prev = s_next = s_side = s_trash_icon = nullptr;
    s_chrome = true;
    s_del_armed = false;
}

void build_viewer(int index) {
    if (s_item_count == 0) { build_grid(); return; }
    if (index < 0) index = 0;
    if (index >= s_item_count) index = s_item_count - 1;
    s_index  = index;
    s_chrome = true;

    lv_obj_t *content = nv_ui_app_content();
    if (!content) return;
    thumb_builder_stop();                      // the grid's tiles are about to be freed by clean
    lv_obj_clean(content);
    nv_ui_set_back(go_grid);

    lv_obj_t *root = lv_obj_create(content);
    lv_obj_remove_style_all(root);
    lv_obj_set_size(root, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_color(root, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);
    lv_obj_clear_flag(root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(root, viewer_deleted, LV_EVENT_DELETE, nullptr);
    nv_gesture_bind(root, LV_DIR_BOTTOM, viewer_swipe_down, nullptr);
    nv_gesture_bind(root, LV_DIR_LEFT,  viewer_swipe_left,  nullptr);
    nv_gesture_bind(root, LV_DIR_RIGHT, viewer_swipe_right, nullptr);

    s_stage = lv_obj_create(root);
    lv_obj_remove_style_all(s_stage);
    lv_obj_set_size(s_stage, lv_pct(100), lv_pct(100));
    lv_obj_clear_flag(s_stage, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_stage, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_stage, stage_tap_cb, LV_EVENT_CLICKED, nullptr);

    s_counter = lv_label_create(root);
    lv_obj_set_style_text_color(s_counter, lv_color_hex(0xC8C8D0), 0);
    lv_obj_align(s_counter, LV_ALIGN_TOP_LEFT, 16, 12);

    s_info = lv_label_create(root);
    lv_obj_set_style_text_font(s_info, &nv_font_14, 0);
    lv_obj_set_style_text_color(s_info, lv_color_hex(0x8A8A94), 0);
    lv_obj_align(s_info, LV_ALIGN_BOTTOM_LEFT, 16, -12);

    s_side = lv_obj_create(root);
    lv_obj_remove_style_all(s_side);
    lv_obj_set_size(s_side, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(s_side, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s_side, 10, 0);
    lv_obj_align(s_side, LV_ALIGN_TOP_RIGHT, -16, 12);
    lv_obj_clear_flag(s_side, LV_OBJ_FLAG_SCROLLABLE);

    s_prev = round_btn(root, 52, LV_SYMBOL_LEFT, prev_cb, nullptr);
    lv_obj_align(s_prev, LV_ALIGN_LEFT_MID, 16, 0);
    s_next = round_btn(root, 52, LV_SYMBOL_RIGHT, next_cb, nullptr);
    lv_obj_align(s_next, LV_ALIGN_RIGHT_MID, -16, 0);

    show_photo(index);
}

// ============================================================ app entry
// Content DELETE = the app is closing (grid<->viewer only cleans its children): hand every
// thumbnail's PSRAM back instead of holding ~4 MB until the Gallery is opened again.
void content_deleted(lv_event_t *) {
    s_thumb_gen = s_thumb_gen + 1;
    free_thumbs();
    free_viewer_buf();
    s_item_count = 0;
    s_scanned = false;
}

void gallery_build(lv_obj_t *content) {
    lv_async_call_cancel(nav_apply, nullptr);   // no stale deferred switch into this fresh open
    thumb_builder_stop();
    free_thumbs();                              // a rebuild (theme/language) re-scans below
    s_item_count = 0;
    s_nav_pending  = false;
    s_pending_page = Page::Grid;
    s_index = 0;
    s_grid_focus = -1;
    s_scanned = false;
    lv_obj_remove_event_cb(content, content_deleted);
    lv_obj_add_event_cb(content, content_deleted, LV_EVENT_DELETE, nullptr);

    // Opened on a file: scan its folder only and open the viewer on it once the scan is in.
    s_intent_mode = s_intent_open = false;
    s_intent_dir[0] = s_intent_path[0] = '\0';
    const NvIntent *in = nv_open_intent();
    const char *slash = (in && in->verb == NV_INTENT_OPEN) ? strrchr(in->path, '/') : nullptr;
    if (slash && slash != in->path) {
        snprintf(s_intent_dir, sizeof s_intent_dir, "%.*s", (int)(slash - in->path), in->path);
        snprintf(s_intent_path, sizeof s_intent_path, "%s", in->path);
        s_intent_mode = s_intent_open = true;
    } else if (in && in->verb == NV_INTENT_RESUME && in->path[0]) {
        // Back from the video player: the full gallery, with the viewer on the clip that played.
        snprintf(s_intent_path, sizeof s_intent_path, "%s", in->path);
        s_intent_open = true;
    }
    build_grid();   // runs during app construction (not from a child event) -> safe to build now
}

const NvApp kGalleryApp = {"gallery", "Gallery", &nv_icon_gallery, 12u << 20, gallery_build,
                           NV_STR_APP_GALLERY, nullptr};

// The formats the viewer shows (HW JPEG, SW PNG/BMP).
const NvOpenHandler kGalleryView = {
    "gallery.view", "gallery", "image/jpeg image/png image/bmp", -1, nullptr, LV_SYMBOL_IMAGE,
    NV_OPEN_OPENER, 50, nullptr, nullptr,
};

}  // namespace

void gallery_app_register(void) {
    nv_app_register(&kGalleryApp);
    nv_open_register(&kGalleryView);
}
