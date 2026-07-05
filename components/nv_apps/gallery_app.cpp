// gallery_app — real-photo gallery reading images off the microSD card.
//   Scan  : POSIX opendir/readdir over /sdcard (+ /sdcard/Photos, /sdcard/DCIM) for
//           .jpg/.jpeg/.png/.bmp, capped at kMaxPhotos. Each file's header is probed with
//           lv_image_decoder_get_info() (cheap — reads a few header bytes, no raster) so we
//           know its size and can flag oversized/corrupt images WITHOUT decoding them.
//   Grid  : responsive 3-column thumbnail wall of lv_image objects (COVER-scaled into the
//           cell). JPEGs get a small persistent SD-cached thumbnail (gallery_thumb_cache,
//           built once via the HW JPEG decoder + PPA downscale — see gallery_jpeg_hw) loaded
//           through LVGL's native .bin file decoder: near-zero per-tile cost, no full-res
//           decode on every open/scroll. PNG/BMP (no HW decode path on this chip) and any
//           failed cache build fall back to the original full-res SW decode. Oversized/broken
//           items render a placeholder, never a decode.
//   Viewer: full-screen ONE image at a time — a single lv_image whose src is (re)set on
//           prev/next/swipe. JPEGs go through the same HW decode + PPA letterbox-fit into one
//           reused PSRAM buffer; PNG/BMP or a HW-path failure fall back to the SW decoder.
//           Never decodes/holds more than the current photo.
// LVGL FS (letter 'S' -> /sdcard) + TJPGD/LODEPNG/BMP decoders are enabled in sdkconfig;
// the SD is mounted at /sdcard before any app opens (app_main.cpp).
//
// Navigation between grid and viewer is DEFERRED via lv_async_call: both builders run
// lv_obj_clean(nv_ui_app_content()), which would free the subtree whose child fired the
// triggering event (tap / swipe / Back) — a use-after-free. Scheduling the rebuild for the
// next LVGL loop lets the event fully unwind first. Same pattern nv_ui.cpp uses for the
// live theme/lang refresh.
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

#include "lvgl.h"
#include "esp_heap_caps.h"

#include <dirent.h>
#include <sys/stat.h>
#include <cstdint>   // intptr_t
#include <cstdio>    // snprintf
#include <cstring>   // strrchr/strcmp
#include <cctype>    // tolower

namespace {

// ---------------------------------------------------------------- data model
constexpr int     kMaxPhotos  = 60;    // hard cap on scanned images (bounded, static)
constexpr int     kMaxPathLen = 160;   // "S:/sdcard/DCIM/100APPLE/IMG_1234.JPG" fits
constexpr int     kMaxNameLen = 48;    // basename for the caption
constexpr int32_t kMaxDecodeW = 2048;  // oversized guard (px): a 4000x3000 phone photo
constexpr int32_t kMaxDecodeH = 2048;  // exceeds this -> flagged, placeholder, never decoded.

struct GalleryItem {
    char     path[kMaxPathLen];  // LVGL src, e.g. "S:/sdcard/DCIM/IMG_0001.JPG"
    char     name[kMaxNameLen];  // basename for the caption
    uint16_t w, h;               // from header probe; 0/0 if probe failed
    bool     probe_ok;           // header read succeeded (a real, decodable image)
    bool     oversized;          // w>kMaxDecodeW || h>kMaxDecodeH -> never full-decode
    char     thumb_path[kMaxPathLen];  // cached grid thumbnail ("S:..."); empty = none (fall
                                        // back to `path`, today's SW decode) -- PNG/BMP,
                                        // oversized, or cache-build failures leave this empty.
    bool     needs_thumb;              // JPEG with no fresh cached thumb yet -> queued for thumb_tick
};

GalleryItem s_items[kMaxPhotos];
int         s_item_count = 0;   // 0 => empty state

// Bumped by add_item() whenever gallery_thumb_cache_get_or_build() actually had to build (not
// just hit a fresh cache) -- scan_sdcard() uses this to decide whether the first-run backlog
// toast is worth showing.
int s_thumbs_built_this_scan = 0;

// Why the scan produced no photos — lets the empty state name the real cause.
enum class ScanResult { Images, NoCard, NoImages };
ScanResult s_scan_result = ScanResult::NoImages;

int s_index = 0;

// ---------------------------------------------------------------- deferred navigation
enum class Page { Grid, Viewer };
Page s_pending_page  = Page::Grid;
int  s_pending_index = 0;
bool s_nav_pending   = false;   // coalesce a burst + guard a failed schedule from wedging nav

void build_grid(void);
void build_viewer(int index);

void nav_apply(void *) {   // runs next LVGL loop: the firing event has fully unwound, so the
    s_nav_pending = false; // lv_obj_clean(content) inside the builder is now safe.
    if (s_pending_page == Page::Grid) build_grid();
    else                              build_viewer(s_pending_index);
}

void nav_to(Page p, int index) {
    s_pending_page  = p;
    s_pending_index = index;
    if (s_nav_pending) return;                                   // already scheduled -> coalesce
    if (lv_async_call(nav_apply, nullptr) == LV_RESULT_OK)       // only latch on success, so a
        s_nav_pending = true;                                    // failed schedule can't wedge nav
}

void go_grid(void) { nav_to(Page::Grid, 0); }   // Back / swipe-down trampoline (must defer too)

// ---------------------------------------------------------------- SD scan
bool has_image_ext(const char *name) {
    const char *dot = strrchr(name, '.');
    if (!dot || dot == name) return false;
    char ext[8] = {0};
    size_t i = 0;
    for (const char *p = dot + 1; *p && i < sizeof(ext) - 1; ++p, ++i)
        ext[i] = (char)tolower((unsigned char)*p);
    return !strcmp(ext, "jpg")  || !strcmp(ext, "jpeg") ||
           !strcmp(ext, "png")  || !strcmp(ext, "bmp");
}

bool is_jpeg_ext(const char *name) {
    const char *dot = strrchr(name, '.');
    if (!dot) return false;
    char ext[8] = {0};
    size_t i = 0;
    for (const char *p = dot + 1; *p && i < sizeof(ext) - 1; ++p, ++i)
        ext[i] = (char)tolower((unsigned char)*p);
    return !strcmp(ext, "jpg") || !strcmp(ext, "jpeg");
}

// Walk JPEG segment markers to the first SOFn. Only baseline (SOF0) is decodable by the bundled
// TJPGD — progressive (SOF2) and others parse a valid header (so the cheap probe passes) but then
// fail the raster decode, leaving a blank page. Detect them here and route to the placeholder.
bool jpeg_is_baseline(const char *posix_path) {
    FILE *f = fopen(posix_path, "rb");
    if (!f) return true;                       // can't inspect -> don't block; let the decoder try
    bool baseline = true;
    const int b0 = fgetc(f), b1 = fgetc(f);
    if (b0 == 0xFF && b1 == 0xD8) {            // SOI
        for (;;) {
            int c = fgetc(f);
            if (c == EOF) break;
            if (c != 0xFF) continue;                          // hunt for a marker prefix
            int m;
            do { m = fgetc(f); } while (m == 0xFF);           // skip fill bytes
            if (m == EOF || m == 0xD9 || m == 0xDA) break;    // EOI / SOS -> no SOF found
            if (m == 0x01 || (m >= 0xD0 && m <= 0xD7)) continue;  // standalone markers (no length)
            const int hi = fgetc(f), lo = fgetc(f);
            if (hi == EOF || lo == EOF) break;
            const int len = ((hi << 8) | lo) - 2;
            if (m >= 0xC0 && m <= 0xCF && m != 0xC4 && m != 0xC8 && m != 0xCC) {
                baseline = (m == 0xC0);        // SOF0 = baseline; SOF1/2/... unsupported by TJPGD
                break;
            }
            if (len > 0) fseek(f, len, SEEK_CUR);
        }
    }
    fclose(f);
    return baseline;
}

// Path composition below intentionally truncates pathological (>150-char) names into fixed
// buffers — a truncated path simply fails the probe and shows a placeholder, never a crash.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat-truncation"

// Probe one file and, if it looks like an image, append it to s_items. Returns false once the
// cap is reached (caller stops scanning). `dir_path` is a POSIX path ("/sdcard/DCIM"); the
// stored src uses the LVGL "S:" prefix over the same absolute path (LV_FS_POSIX_PATH="").
bool add_item(const char *dir_path, const char *fname) {
    if (s_item_count >= kMaxPhotos) return false;
    if (!has_image_ext(fname)) return true;

    GalleryItem &it = s_items[s_item_count];
    // src string: "S:" + absolute posix path, e.g. "S:/sdcard/DCIM/IMG_0001.JPG"
    snprintf(it.path, sizeof(it.path), "S:%s/%s", dir_path, fname);
    snprintf(it.name, sizeof(it.name), "%s", fname);

    // LVGL's TJPGD/BMP decoders match the extension CASE-SENSITIVELY; camera files are .JPG
    // (uppercase) and would otherwise all fail. FAT is case-insensitive, so lowercasing just the
    // extension in the LVGL src still opens the real on-disk file while satisfying the decoders.
    if (char *dot = strrchr(it.path, '.'))
        for (char *p = dot + 1; *p; ++p) *p = (char)tolower((unsigned char)*p);

    // Header probe: reads only the JPEG SOF / PNG IHDR — no raster is allocated. This is the
    // core guard: we learn w/h (and decodability) before ever attempting a full decode.
    it.thumb_path[0] = '\0';
    it.needs_thumb   = false;

    lv_image_header_t hdr;
    lv_memzero(&hdr, sizeof(hdr));
    if (lv_image_decoder_get_info(it.path, &hdr) == LV_RESULT_OK) {
        it.probe_ok  = true;
        it.w         = hdr.w;
        it.h         = hdr.h;
        it.oversized = (hdr.w > kMaxDecodeW || hdr.h > kMaxDecodeH);
        // Progressive JPEGs pass the header probe but fail the actual decode -> placeholder.
        if (is_jpeg_ext(fname)) {
            char posix[kMaxPathLen];
            snprintf(posix, sizeof(posix), "%s/%s", dir_path, fname);
            if (!jpeg_is_baseline(posix)) it.probe_ok = false;

            // Cheap cache lookup ONLY — no decode here. Building 60 full-res thumbnails inline on
            // the LVGL thread is exactly what froze the grid on open. A fresh cached thumb is used
            // immediately; a miss leaves thumb_path empty and is queued for the incremental builder
            // (thumb_tick), which decodes one-per-tick off the render path. PNG/BMP have no HW path.
            if (it.probe_ok && !it.oversized) {
                if (!gallery_thumb_cache_lookup(posix, it.thumb_path, sizeof(it.thumb_path))) {
                    it.needs_thumb = true;
                    s_thumbs_built_this_scan++;
                }
            }
        }
    } else {
        it.probe_ok  = false;   // corrupt / truncated / unsupported -> placeholder, never decoded
        it.w = it.h  = 0;
        it.oversized = false;
    }
    s_item_count++;
    return true;
}

// Enumerate a single directory (non-recursive) feeding image files to add_item().
// Returns true if the directory existed (so we can tell "no card" from "no images").
bool scan_dir(const char *dir_path) {
    DIR *d = opendir(dir_path);
    if (!d) return false;
    struct dirent *e;
    while ((e = readdir(d)) != nullptr) {
        if (e->d_name[0] == '.') continue;     // skip "." / ".." / hidden

        char full[kMaxPathLen];
        snprintf(full, sizeof(full), "%s/%s", dir_path, e->d_name);

        bool is_regular;
        if (e->d_type == DT_REG)      is_regular = true;
        else if (e->d_type == DT_DIR) is_regular = false;
        else {                                 // DT_UNKNOWN -> confirm via stat
            struct stat st;
            if (stat(full, &st) != 0) continue;
            is_regular = S_ISREG(st.st_mode);
        }
        if (!is_regular) continue;

        if (!add_item(dir_path, e->d_name)) break;   // hit kMaxPhotos
    }
    closedir(d);
    return true;
}
#pragma GCC diagnostic pop

// Rebuild s_items from scratch (called every time the app opens / rebuilds).
void scan_sdcard(void) {
    s_item_count = 0;
    s_thumbs_built_this_scan = 0;
    // Scan the root plus the two directories phones commonly nest photos in. opendir on the
    // root tells us whether the card is mounted at all.
    const bool have_root = scan_dir("/sdcard");
    scan_dir("/sdcard/Photos");
    scan_dir("/sdcard/DCIM");

    if (!have_root)             s_scan_result = ScanResult::NoCard;
    else if (s_item_count == 0) s_scan_result = ScanResult::NoImages;
    else                        s_scan_result = ScanResult::Images;

    // First-run (or post-backlog) thumbnail generation is a one-time cost; only worth a toast
    // when it was actually noticeable, not for the steady-state "0-1 new photos" case.
    if (s_thumbs_built_this_scan > 5) nv_toast(NV_NOTE_INFO, nv_tr(NV_STR_GENERATING_THUMBS));
}

// ---------------------------------------------------------------- shared placeholder
// A tile/page fills its parent with a centered symbol on a surface bg — used for oversized or
// undecodable images so we never attempt a decode. `on_scrim` picks light ink for the viewer's
// black page; the grid passes false for theme-consistent tile ink.
void fill_placeholder(lv_obj_t *parent, const char *sym, bool on_scrim) {
    const NvTheme *th = nv_theme_get();
    if (!on_scrim) {
        lv_obj_set_style_bg_color(parent, th->surface2, 0);
        lv_obj_set_style_bg_opa(parent, LV_OPA_COVER, 0);
    }
    lv_obj_t *l = lv_label_create(parent);
    lv_label_set_text(l, sym);
    lv_obj_set_style_text_font(l, &nv_font_28, 0);  // pinned: large glyph, fixed placeholder
    lv_obj_set_style_text_color(l, on_scrim ? th->on_primary : th->text_dim, 0);
    lv_obj_center(l);
}

// ---------------------------------------------------------------- incremental thumbnail builder
// The grid paints instantly: cached thumbs load from SD, misses show a blank surface tile. This
// LVGL-thread timer then decodes ONE missing thumbnail per tick (HW JPEG + PPA downscale, off the
// scan/render path) and swaps it into its tile. One-per-tick keeps each ~tens-of-ms build from
// stacking into the whole-batch freeze the old inline build caused. Tile pointers are captured in
// build_grid and invalidated (timer stopped + array nulled) before any lv_obj_clean, so a tick
// never touches a freed object.
lv_obj_t   *s_tile_img[kMaxPhotos] = {nullptr};   // per-tile lv_image awaiting a thumb
int         s_thumb_queue[kMaxPhotos];
int         s_thumb_qn = 0, s_thumb_qi = 0;
lv_timer_t *s_thumb_timer = nullptr;
bool        s_scanned = false;   // scan once per app open; grid<->viewer nav must NOT re-probe the SD

void thumb_builder_stop(void) {
    if (s_thumb_timer) { lv_timer_delete(s_thumb_timer); s_thumb_timer = nullptr; }
    s_thumb_qn = s_thumb_qi = 0;
    for (int i = 0; i < kMaxPhotos; i++) s_tile_img[i] = nullptr;
}

void thumb_tick(lv_timer_t *) {
    if (s_thumb_qi >= s_thumb_qn) { thumb_builder_stop(); return; }
    const int i = s_thumb_queue[s_thumb_qi++];
    if (i < 0 || i >= s_item_count) return;
    GalleryItem &it = s_items[i];
    lv_obj_t *img = s_tile_img[i];
    if (!it.needs_thumb || !img) return;   // built already, or its tile is gone

    char posix[kMaxPathLen];
    snprintf(posix, sizeof(posix), "%s", it.path + 2);   // drop the LVGL "S:" prefix
    bool did_build = false;
    it.needs_thumb = false;
    if (gallery_thumb_cache_get_or_build(posix, it.w, it.h, it.thumb_path,
                                          sizeof(it.thumb_path), &did_build) && it.thumb_path[0])
        lv_image_set_src(img, it.thumb_path);   // real thumb, streamed cheaply from SD from now on
    else {
        it.thumb_path[0] = '\0';                // HW build failed (rare): last-resort SW full-res decode
        lv_image_set_src(img, it.path);
    }
    s_tile_img[i] = nullptr;
}

// Stop the builder whenever the grid container is torn down — including the app CLOSE path (Back
// from the grid), which frees the tiles with no app-close hook to call us. Without this the global
// timer would keep firing and lv_image_set_src() a freed tile.
void grid_deleted(lv_event_t *) { thumb_builder_stop(); }

// ============================================================ grid page
void tile_cb(lv_event_t *e) {
    nav_to(Page::Viewer, (int)(intptr_t)lv_event_get_user_data(e));
}

// The centered column shown when there are no images (no card / empty card / no matches).
void build_empty_state(lv_obj_t *content, const NvTheme *th) {
    lv_obj_t *col = lv_obj_create(content);
    lv_obj_remove_style_all(col);
    lv_obj_set_size(col, lv_pct(100), lv_pct(100));
    lv_obj_set_style_pad_all(col, 24, 0);
    lv_obj_set_style_pad_row(col, 10, 0);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(col, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(col, LV_OBJ_FLAG_SCROLLABLE);

    const bool no_card = (s_scan_result == ScanResult::NoCard);

    lv_obj_t *icon = lv_label_create(col);
    lv_label_set_text(icon, no_card ? LV_SYMBOL_SD_CARD : LV_SYMBOL_IMAGE);
    lv_obj_set_style_text_font(icon, &nv_font_28, 0);  // pinned: hero empty-state glyph
    lv_obj_set_style_text_color(icon, th->text_dim, 0);

    lv_obj_t *primary = lv_label_create(col);
    lv_label_set_text(primary, nv_tr(no_card ? NV_STR_SD_MISSING : NV_STR_NO_PHOTOS));
    lv_obj_set_style_text_font(primary, &nv_font_20, 0);
    lv_obj_set_style_text_color(primary, th->text_strong, 0);

    lv_obj_t *hint = lv_label_create(col);
    lv_label_set_text(hint, nv_tr(NV_STR_ADD_PHOTOS_HINT));
    lv_obj_set_style_text_color(hint, th->text_dim, 0);
    lv_obj_set_style_text_align(hint, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(hint, lv_pct(90));
    lv_label_set_long_mode(hint, LV_LABEL_LONG_WRAP);
}

void build_grid(void) {
    lv_obj_t *content = nv_ui_app_content();   // re-read: never cache across a deferred switch
    if (!content) return;
    thumb_builder_stop();                      // kill the old timer + drop tile ptrs BEFORE freeing them
    lv_obj_clean(content);
    nv_ui_set_back(nullptr);                   // grid: Back closes the app
    nv_ui_set_title(nv_tr(NV_STR_APP_GALLERY));
    const NvTheme *th = nv_theme_get();
    lv_obj_set_style_bg_color(content, th->bg, 0);
    lv_obj_set_style_bg_opa(content, LV_OPA_COVER, 0);

    // Scan the SD ONCE per app open. Returning from the viewer must not re-opendir/re-probe every
    // file (60 x several SD reads) — that made back-navigation crawl. s_items already reflects any
    // in-viewer deletions.
    if (!s_scanned) { scan_sdcard(); s_scanned = true; }
    if (s_item_count == 0) { build_empty_state(content, th); return; }

    lv_obj_t *col = lv_obj_create(content);
    lv_obj_remove_style_all(col);
    lv_obj_set_size(col, lv_pct(100), lv_pct(100));
    lv_obj_set_style_pad_all(col, 16, 0);
    lv_obj_set_style_pad_row(col, 12, 0);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_clear_flag(col, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(col, grid_deleted, LV_EVENT_DELETE, nullptr);   // stop builder on close/rebuild

    // header
    lv_obj_t *hdr = lv_obj_create(col);
    lv_obj_remove_style_all(hdr);
    lv_obj_set_size(hdr, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(hdr, LV_FLEX_FLOW_COLUMN);
    lv_obj_t *ht = lv_label_create(hdr);
    lv_label_set_text(ht, nv_tr(NV_STR_PHOTOS));
    lv_obj_set_style_text_font(ht, &nv_font_28, 0);  // pinned: hero header, fixed layout
    lv_obj_set_style_text_color(ht, th->text_strong, 0);
    lv_obj_t *hs = lv_label_create(hdr);
    lv_label_set_text_fmt(hs, nv_tr(NV_STR_ITEMS_FMT), s_item_count);
    lv_obj_set_style_text_color(hs, th->text_dim, 0);

    // 3-column grid
    static int32_t col_dsc[] = {LV_GRID_FR(1), LV_GRID_FR(1), LV_GRID_FR(1),
                                LV_GRID_TEMPLATE_LAST};
    static int32_t row_dsc[(kMaxPhotos + 2) / 3 + 1];
    const int rows = (s_item_count + 2) / 3;
    for (int r = 0; r < rows; r++) row_dsc[r] = LV_GRID_CONTENT;
    row_dsc[rows] = LV_GRID_TEMPLATE_LAST;

    lv_obj_t *grid = lv_obj_create(col);
    lv_obj_remove_style_all(grid);
    lv_obj_set_width(grid, lv_pct(100));
    lv_obj_set_flex_grow(grid, 1);
    lv_obj_set_style_pad_column(grid, 12, 0);
    lv_obj_set_style_pad_row(grid, 12, 0);
    lv_obj_set_grid_dsc_array(grid, col_dsc, row_dsc);
    lv_obj_set_layout(grid, LV_LAYOUT_GRID);
    lv_obj_set_scroll_dir(grid, LV_DIR_VER);
    lv_obj_set_style_pad_bottom(grid, 8, 0);

    for (int i = 0; i < s_item_count; i++) {
        const GalleryItem &it = s_items[i];
        lv_obj_t *tile = lv_obj_create(grid);
        lv_obj_remove_style_all(tile);
        lv_obj_set_height(tile, 150);
        lv_obj_set_grid_cell(tile, LV_GRID_ALIGN_STRETCH, i % 3, 1,
                                    LV_GRID_ALIGN_STRETCH, i / 3, 1);
        lv_obj_set_style_radius(tile, 16, 0);
        // (No clip_corner: corner clipping needs a draw-layer/mask the P4 renderer stalls on.)
        lv_obj_clear_flag(tile, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(tile, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_bg_color(tile, th->surface, 0);
        lv_obj_set_style_bg_opa(tile, LV_OPA_COVER, 0);
        // Layer-free press feedback only. (shadow blur + transform_scale each render the tile
        // through an off-screen draw layer, which stalls the ESP32-P4 software renderer -> WDT.)
        lv_obj_set_style_bg_opa(tile, LV_OPA_80, LV_STATE_PRESSED);
        lv_obj_add_event_cb(tile, tile_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);

        if (it.probe_ok && !it.oversized) {
            // Cached HW thumbnail loads instantly (streamed from SD via LVGL's .bin decoder). A
            // miss (needs_thumb) starts blank and is queued for thumb_tick to fill in — no full-res
            // decode on the render path. PNG/BMP (no HW path) keep the original SW decode.
            lv_obj_t *img = lv_image_create(tile);
            lv_image_set_inner_align(img, LV_IMAGE_ALIGN_COVER);
            lv_obj_set_size(img, lv_pct(100), lv_pct(100));
            lv_obj_clear_flag(img, LV_OBJ_FLAG_CLICKABLE);  // let the tile catch the tap
            if (it.thumb_path[0]) {
                lv_image_set_src(img, it.thumb_path);
            } else if (it.needs_thumb) {
                s_tile_img[i] = img;                              // filled in by thumb_tick
                if (s_thumb_qn < kMaxPhotos) s_thumb_queue[s_thumb_qn++] = i;
            } else {
                lv_image_set_src(img, it.path);                  // PNG/BMP: original SW decode
            }
        } else {
            // Oversized or undecodable: placeholder only — never rasterized.
            fill_placeholder(tile, it.oversized ? LV_SYMBOL_IMAGE : LV_SYMBOL_WARNING, false);
        }

        lv_obj_t *cap = lv_obj_create(tile);
        lv_obj_remove_style_all(cap);
        lv_obj_set_size(cap, lv_pct(100), 34);
        lv_obj_align(cap, LV_ALIGN_BOTTOM_MID, 0, 0);
        lv_obj_set_style_bg_color(cap, th->scrim, 0);
        lv_obj_set_style_bg_opa(cap, LV_OPA_50, 0);
        lv_obj_clear_flag(cap, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_clear_flag(cap, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_t *ct = lv_label_create(cap);
        lv_label_set_text(ct, it.name);
        lv_obj_set_style_text_color(ct, th->on_primary, 0);  // on the black scrim strip: stay light
        lv_obj_set_width(ct, lv_pct(94));
        lv_label_set_long_mode(ct, LV_LABEL_LONG_DOT);
        lv_obj_align(ct, LV_ALIGN_LEFT_MID, 10, 0);
    }

    // Kick off the incremental thumbnail builder for any tiles still awaiting a thumb. 15 ms/tick
    // yields to rendering + input between builds so the grid scrolls smoothly while thumbs pop in.
    if (s_thumb_qn > 0 && !s_thumb_timer) s_thumb_timer = lv_timer_create(thumb_tick, 15, nullptr);
}

// ============================================================ viewer page
// Single-decode viewer: one lv_image whose src is re-set on navigation. At any instant at most
// one photo is decoded (plus whatever LVGL keeps in its bounded cache).
lv_obj_t *s_photo = nullptr;   // the lv_image (created only for decodable, in-budget photos)
lv_obj_t *s_stage = nullptr;   // black container the photo/placeholder lives in
lv_obj_t *s_counter = nullptr, *s_cap_name = nullptr, *s_cap_size = nullptr;
lv_obj_t *s_dots = nullptr, *s_topbar = nullptr, *s_botbar = nullptr,
         *s_prev = nullptr, *s_next = nullptr;
lv_obj_t *s_trash_btn = nullptr, *s_trash_icon = nullptr;
bool s_chrome = true;
bool s_del_armed = false;   // trash tapped once; a 2nd tap actually deletes (files_app pattern)

// ---- persistent HW-decode viewer buffer -----------------------------------------------------
// Lazily allocated once, sized to the fixed app content-area pixel size, and reused across every
// navigation (prev/next/swipe) for the rest of the process lifetime -- same "small fixed
// footprint, acceptable" reasoning as the JPEG engine/PPA client in gallery_jpeg_hw.cpp. Paired
// with a static lv_image_dsc_t so lv_image_set_src() can point straight at PSRAM, no file/SD
// round-trip for the currently-viewed photo.
uint8_t       *s_viewer_buf = nullptr;
size_t         s_viewer_cap = 0;
int            s_viewer_w = 0, s_viewer_h = 0;
lv_image_dsc_t s_viewer_dsc;

// Sizing to the app content area's real pixel dimensions (not a generous guess) means PPA's
// letterbox fit and LVGL's own CONTAIN fit below produce the identical result -- a mismatched
// buffer size would double-letterbox (visible borders within borders).
bool ensure_viewer_buf(void) {
    if (s_viewer_buf) return true;
    lv_obj_t *content = nv_ui_app_content();
    if (!content) return false;
    int w = lv_obj_get_width(content);
    int h = lv_obj_get_height(content);
    if (w <= 0 || h <= 0) return false;

    const size_t cap = gallery_ppa_align_size((size_t)w * h * 2);
    uint8_t *buf = (uint8_t *)heap_caps_aligned_alloc(GALLERY_PPA_ALIGN, cap,
                                                       MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) return false;

    s_viewer_buf = buf;
    s_viewer_cap = cap;
    s_viewer_w   = w;
    s_viewer_h   = h;
    return true;
}

void refresh_chrome(int idx) {
    if (idx < 0 || idx >= s_item_count) return;
    const GalleryItem &it = s_items[idx];
    nv_ui_set_title(it.name);   // keep the OS app-bar title in sync with the photo on screen
    if (s_counter)  lv_label_set_text_fmt(s_counter, "%d / %d", idx + 1, s_item_count);
    if (s_cap_name) lv_label_set_text(s_cap_name, it.name);
    if (s_cap_size) {
        if (it.probe_ok) lv_label_set_text_fmt(s_cap_size, "%d x %d", it.w, it.h);
        else             lv_label_set_text(s_cap_size, nv_tr(NV_STR_IMAGE_UNAVAILABLE));
    }
    // Dim the boundary arrow via bg_opa (layer-free: whole-object "opa" hangs the P4 software
    // renderer, see nv_gesture.cpp-adjacent note in build_viewer / [[lvgl-draw-layers-wdt]]).
    auto edge_arrow = [](lv_obj_t *btn, bool at_edge) {
        if (!btn) return;
        lv_obj_set_style_bg_opa(btn, at_edge ? LV_OPA_10 : LV_OPA_40, 0);
        if (at_edge) lv_obj_remove_flag(btn, LV_OBJ_FLAG_CLICKABLE);
        else         lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);
    };
    edge_arrow(s_prev, idx == 0);
    edge_arrow(s_next, idx == s_item_count - 1);
    if (s_dots) {
        uint32_t n = lv_obj_get_child_count(s_dots);
        // The dot rail is capped (<=12), so map the real index proportionally into it — otherwise
        // no dot lights up past the 12th photo. When n == count this is exact (active == idx).
        int active = (n > 0 && s_item_count > 0)
                         ? (int)((int64_t)idx * (int)n / s_item_count)
                         : idx;
        if (active >= (int)n) active = (int)n - 1;
        for (uint32_t k = 0; k < n; k++) {
            lv_obj_t *d = lv_obj_get_child(s_dots, k);
            const bool on = ((int)k == active);
            lv_obj_set_style_bg_opa(d, on ? LV_OPA_COVER : LV_OPA_40, 0);
            lv_obj_set_width(d, on ? 18 : 8);
        }
    }
}

// Show item `idx` on the single stage: clears the previous photo (releasing its raster to the
// cache) and either shows the new lv_image or a placeholder. Only ONE image is ever decoded.
void show_photo(int idx) {
    if (s_item_count == 0) return;   // empty gallery: never index s_items[-1]
    if (idx < 0) idx = 0;
    if (idx >= s_item_count) idx = s_item_count - 1;
    s_index = idx;
    if (!s_stage) return;

    lv_obj_clean(s_stage);   // drop the old lv_image -> its decoded raster is freed/evictable
    s_photo = nullptr;

    // Any navigation cancels a pending delete-confirm on the photo just left.
    s_del_armed = false;
    if (s_trash_icon) lv_obj_set_style_text_color(s_trash_icon, nv_theme_get()->on_primary, 0);

    const GalleryItem &it = s_items[idx];
    bool used_hw = false;
    if (it.probe_ok && !it.oversized && is_jpeg_ext(it.name) && ensure_viewer_buf()) {
        // HW-decode the full photo into a TRANSIENT scratch buffer (native res, up to
        // 2048x2048x2 = 8MB -- NOT the persistent viewer buffer, which is fixed at the content-
        // area size) then PPA letterbox-fit it into the reused viewer buffer.
        char posix[kMaxPathLen];
        snprintf(posix, sizeof(posix), "%s", it.path + 2);   // drop the LVGL "S:" prefix
        uint8_t *scratch = nullptr;
        size_t   scratch_len = 0;
        if (gallery_jpeg_hw_decode_file(posix, it.w, it.h, &scratch, &scratch_len)) {
            // PPA only writes the scaled region -- pre-clear to the theme scrim so a letterbox
            // border shows the right color instead of the previous photo's stale pixels.
            const NvTheme *th = nv_theme_get();
            const uint16_t border565 = lv_color_to_u16(th->scrim);
            uint16_t *px = (uint16_t *)s_viewer_buf;
            for (size_t i = 0, n = (size_t)s_viewer_w * s_viewer_h; i < n; i++) px[i] = border565;

            if (gallery_ppa_scale_fit(scratch, it.w, it.h, s_viewer_buf, s_viewer_w, s_viewer_h,
                                       s_viewer_cap)) {
                s_viewer_dsc.header.magic      = LV_IMAGE_HEADER_MAGIC;
                s_viewer_dsc.header.cf         = LV_COLOR_FORMAT_RGB565;
                s_viewer_dsc.header.flags      = 0;
                s_viewer_dsc.header.w          = (uint32_t)s_viewer_w;
                s_viewer_dsc.header.h          = (uint32_t)s_viewer_h;
                s_viewer_dsc.header.stride     = (uint32_t)(s_viewer_w * 2);
                s_viewer_dsc.header.reserved_2 = 0;
                s_viewer_dsc.data_size = (uint32_t)s_viewer_cap;
                s_viewer_dsc.data      = s_viewer_buf;

                s_photo = lv_image_create(s_stage);
                lv_image_set_src(s_photo, &s_viewer_dsc);
                lv_image_set_inner_align(s_photo, LV_IMAGE_ALIGN_CONTAIN);
                lv_obj_set_size(s_photo, lv_pct(100), lv_pct(100));
                lv_obj_clear_flag(s_photo, LV_OBJ_FLAG_CLICKABLE);
                used_hw = true;
            }
            gallery_jpeg_hw_free(scratch);
        }
    }
    if (!used_hw) {
        // Fallback: PNG/BMP (no HW decode path on this chip), or any HW-path failure -- the
        // original SW TJPGD/LODEPNG/BMP path, unchanged.
        if (it.probe_ok && !it.oversized) {
            s_photo = lv_image_create(s_stage);
            lv_image_set_src(s_photo, it.path);
            lv_image_set_inner_align(s_photo, LV_IMAGE_ALIGN_CONTAIN);  // fit whole photo, letterbox
            lv_obj_set_size(s_photo, lv_pct(100), lv_pct(100));
            lv_obj_clear_flag(s_photo, LV_OBJ_FLAG_CLICKABLE);          // tap bubbles to the stage
        } else {
            fill_placeholder(s_stage, it.oversized ? LV_SYMBOL_IMAGE : LV_SYMBOL_WARNING, true);
        }
    }
    refresh_chrome(idx);
}

void set_chrome_shown(bool show) {
    s_chrome = show;
    lv_obj_t *bars[] = {s_topbar, s_botbar, s_prev, s_next};
    for (lv_obj_t *b : bars) {
        if (!b) continue;
        if (show) lv_obj_remove_flag(b, LV_OBJ_FLAG_HIDDEN);
        else      lv_obj_add_flag(b, LV_OBJ_FLAG_HIDDEN);
    }
}

void stage_tap_cb(lv_event_t *) { set_chrome_shown(!s_chrome); }  // immersive toggle

void viewer_swipe_down(void *) { go_grid(); }  // swipe down closes the viewer (deferred)

// Carousel swipe: the gesture fires while the pressed child (stage/image) is still unwinding
// its own event dispatch, so — same reason nav_to() defers — show_photo() (which cleans
// s_stage) must NOT run inline here. Defer one LVGL loop, same as the grid<->viewer switch.
void carousel_next_apply(void *) { show_photo(s_index + 1); }
void carousel_prev_apply(void *) { show_photo(s_index - 1); }
void viewer_swipe_left(void *)  { lv_async_call(carousel_next_apply, nullptr); }  // left = next
void viewer_swipe_right(void *) { lv_async_call(carousel_prev_apply, nullptr); }  // right = prev

void prev_cb(lv_event_t *) { show_photo(s_index - 1); }
void next_cb(lv_event_t *) { show_photo(s_index + 1); }

// Trash button: 1st tap arms (icon turns danger-red + toast), 2nd tap deletes the file for real
// and advances to the next photo (or back to the grid if that was the last one). Mirrors the
// files_app "tap again to confirm" idiom — no modal needed for a single-item, undo-free delete.
void delete_cb(lv_event_t *) {
    if (s_item_count == 0) return;
    if (!s_del_armed) {
        s_del_armed = true;
        if (s_trash_icon) lv_obj_set_style_text_color(s_trash_icon, nv_theme_get()->danger, 0);
        nv_toast(NV_NOTE_WARN, nv_tr(NV_STR_TAP_AGAIN));
        return;
    }
    s_del_armed = false;
    // it.path is "S:/sdcard/..." -> the real POSIX path drops the LVGL "S:" drive prefix.
    char posix[kMaxPathLen];
    snprintf(posix, sizeof(posix), "%s", s_items[s_index].path + 2);
    if (remove(posix) != 0) {
        nv_toast(NV_NOTE_ERROR, nv_tr(NV_STR_SAVE_FAILED));
        return;
    }
    gallery_thumb_cache_evict(posix);   // best-effort; an orphaned .bin is harmless clutter
    nv_toast(NV_NOTE_OK, nv_tr(NV_STR_DELETE));
    for (int i = s_index; i < s_item_count - 1; i++) s_items[i] = s_items[i + 1];
    s_item_count--;
    if (s_item_count == 0) { go_grid(); return; }
    show_photo(s_index >= s_item_count ? s_item_count - 1 : s_index);
}

void viewer_deleted(lv_event_t *) {
    s_photo = s_stage = s_counter = s_cap_name = s_cap_size = nullptr;
    s_dots = s_topbar = s_botbar = s_prev = s_next = nullptr;
    s_trash_btn = s_trash_icon = nullptr;
    s_chrome = true;
    s_del_armed = false;
}

lv_obj_t *nav_button(lv_obj_t *parent, const char *sym, lv_align_t align, lv_event_cb_t cb) {
    lv_obj_t *b = lv_button_create(parent);
    lv_obj_set_size(b, 56, 56);
    lv_obj_align(b, align, align == LV_ALIGN_LEFT_MID ? 12 : -12, 0);
    lv_obj_set_style_radius(b, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(b, nv_theme_get()->scrim, 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_40, 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_70, LV_STATE_PRESSED);
    lv_obj_set_style_shadow_width(b, 0, 0);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, nullptr);
    lv_obj_t *l = lv_label_create(b);
    lv_label_set_text(l, sym);
    lv_obj_set_style_text_color(l, nv_theme_get()->on_primary, 0);  // on scrim button: stay light
    lv_obj_center(l);
    return b;
}

void build_viewer(int index) {
    if (s_item_count == 0) { build_grid(); return; }  // nothing to show -> fall back to grid
    if (index < 0) index = 0;
    if (index >= s_item_count) index = s_item_count - 1;
    s_index  = index;
    s_chrome = true;

    lv_obj_t *content = nv_ui_app_content();   // re-read: never cache across a deferred switch
    if (!content) return;
    thumb_builder_stop();                      // the grid's tiles are about to be freed by clean
    lv_obj_clean(content);
    nv_ui_set_back(go_grid);                   // viewer: Back returns to the grid (deferred)
    nv_ui_set_title(s_items[index].name);
    const NvTheme *th = nv_theme_get();

    lv_obj_t *root = lv_obj_create(content);
    lv_obj_remove_style_all(root);
    lv_obj_set_size(root, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_color(root, th->scrim, 0);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);
    lv_obj_clear_flag(root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(root, viewer_deleted, LV_EVENT_DELETE, nullptr);
    // Content gesture via the OS standard: consumed on the viewer, never leaks to SystemUI.
    nv_gesture_bind(root, LV_DIR_BOTTOM, viewer_swipe_down, nullptr);
    nv_gesture_bind(root, LV_DIR_LEFT,  viewer_swipe_left,  nullptr);  // carousel: swipe -> next
    nv_gesture_bind(root, LV_DIR_RIGHT, viewer_swipe_right, nullptr);  // carousel: swipe -> prev

    // single-image stage (fills the viewport; taps toggle immersive chrome)
    s_stage = lv_obj_create(root);
    lv_obj_remove_style_all(s_stage);
    lv_obj_set_size(s_stage, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_opa(s_stage, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(s_stage, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_stage, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_stage, stage_tap_cb, LV_EVENT_CLICKED, nullptr);

    // top bar: counter
    s_topbar = lv_obj_create(root);
    lv_obj_remove_style_all(s_topbar);
    lv_obj_set_size(s_topbar, lv_pct(100), 44);
    lv_obj_align(s_topbar, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_bg_color(s_topbar, th->scrim, 0);
    lv_obj_set_style_bg_opa(s_topbar, LV_OPA_40, 0);
    lv_obj_clear_flag(s_topbar, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(s_topbar, LV_OBJ_FLAG_SCROLLABLE);
    s_counter = lv_label_create(s_topbar);
    lv_obj_set_style_text_color(s_counter, th->on_primary, 0);  // on scrim top bar: stay light
    lv_obj_center(s_counter);

    s_trash_btn = lv_button_create(s_topbar);
    lv_obj_remove_style_all(s_trash_btn);
    lv_obj_set_size(s_trash_btn, 44, 44);
    lv_obj_align(s_trash_btn, LV_ALIGN_RIGHT_MID, -4, 0);
    lv_obj_set_style_radius(s_trash_btn, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(s_trash_btn, LV_OPA_TRANSP, 0);
    lv_obj_set_style_bg_color(s_trash_btn, th->scrim, LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(s_trash_btn, LV_OPA_60, LV_STATE_PRESSED);
    lv_obj_set_style_shadow_width(s_trash_btn, 0, 0);
    lv_obj_add_event_cb(s_trash_btn, delete_cb, LV_EVENT_CLICKED, nullptr);
    s_trash_icon = lv_label_create(s_trash_btn);
    lv_label_set_text(s_trash_icon, LV_SYMBOL_TRASH);
    lv_obj_set_style_text_color(s_trash_icon, th->on_primary, 0);
    lv_obj_center(s_trash_icon);

    // bottom bar: dots + caption (filename + dimensions)
    s_botbar = lv_obj_create(root);
    lv_obj_remove_style_all(s_botbar);
    lv_obj_set_size(s_botbar, lv_pct(100), 86);
    lv_obj_align(s_botbar, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(s_botbar, th->scrim, 0);
    lv_obj_set_style_bg_opa(s_botbar, LV_OPA_50, 0);
    lv_obj_set_style_pad_hor(s_botbar, 18, 0);
    lv_obj_set_style_pad_ver(s_botbar, 8, 0);
    lv_obj_set_style_pad_row(s_botbar, 2, 0);
    lv_obj_set_flex_flow(s_botbar, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_botbar, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START);
    lv_obj_clear_flag(s_botbar, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(s_botbar, LV_OBJ_FLAG_SCROLLABLE);

    s_dots = lv_obj_create(s_botbar);
    lv_obj_remove_style_all(s_dots);
    lv_obj_set_size(s_dots, lv_pct(100), 10);
    lv_obj_set_flex_flow(s_dots, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(s_dots, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(s_dots, 5, 0);
    lv_obj_clear_flag(s_dots, LV_OBJ_FLAG_SCROLLABLE);
    // Cap the dot count so a large gallery doesn't overflow the bar; the counter still shows N.
    const int dot_n = s_item_count > 12 ? 12 : s_item_count;
    for (int i = 0; i < dot_n; i++) {
        lv_obj_t *d = lv_obj_create(s_dots);
        lv_obj_remove_style_all(d);
        lv_obj_set_size(d, 8, 8);
        lv_obj_set_style_radius(d, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(d, th->on_primary, 0);  // page dots on scrim bar: stay light
        lv_obj_set_style_bg_opa(d, LV_OPA_40, 0);
    }
    s_cap_name = lv_label_create(s_botbar);
    lv_obj_set_style_text_font(s_cap_name, &nv_font_20, 0);  // pinned: fixed 86px bottom bar
    lv_obj_set_style_text_color(s_cap_name, th->on_primary, 0);  // on scrim bottom bar: stay light
    lv_obj_set_width(s_cap_name, lv_pct(100));
    lv_label_set_long_mode(s_cap_name, LV_LABEL_LONG_DOT);
    s_cap_size = lv_label_create(s_botbar);
    lv_obj_set_style_text_color(s_cap_size, th->text_dim, 0);

    // prev / next
    s_prev = nav_button(root, LV_SYMBOL_LEFT,  LV_ALIGN_LEFT_MID,  prev_cb);
    s_next = nav_button(root, LV_SYMBOL_RIGHT, LV_ALIGN_RIGHT_MID, next_cb);

    show_photo(index);   // decode + display exactly this one image
}

// ============================================================ app entry
void gallery_build(lv_obj_t *content) {
    (void)content;
    // Drop any nav rebuild still queued from a previous instance + reset nav state, so a stale
    // deferred switch can't fire into this fresh open.
    lv_async_call_cancel(nav_apply, nullptr);
    thumb_builder_stop();                       // clear any timer/tile ptrs left by a prior instance
    s_nav_pending  = false;
    s_pending_page = Page::Grid;
    s_index = 0;
    s_scanned = false;                          // fresh open -> scan the SD once
    build_grid();   // runs during app construction (not from a child event) -> safe to build now
}

const NvApp kGalleryApp = {"gallery", "Gallery", &nv_icon_gallery, 12u << 20, gallery_build,
                           NV_STR_APP_GALLERY, nullptr};

}  // namespace

void gallery_app_register(void) { nv_app_register(&kGalleryApp); }
