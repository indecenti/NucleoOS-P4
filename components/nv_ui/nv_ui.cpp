// nv_ui — SystemUI only: app registry, shared widget kit, app-host API, status bar,
// icon launcher (driven by the registry), notification shade, and gestures.
// Contains NO app-specific code — apps live in the nv_apps component and self-register.
#include <string.h>

#include "nv_ui.h"
#include "nv_app.h"
#include "nv_icons.h"      // nv_icon_wasm — used to keep WASM apps out of the smart dock
#include "nv_ui_kit.h"
#include "nv_ui_host.h"
#include "nv_ime.h"
#include "nv_gesture.h"
#include "nv_notify.h"

#include "esp_timer.h"
#include "lvgl.h"
#include "lvgl_private.h"  // lv_image_decoder_dsc_t full type (wallpaper one-shot decode)
#include "esp_lvgl_port.h"

#include "nv_log.h"
#include "nv_memory_broker.h"
#include "nv_config.h"
#include "nv_time.h"
#include "nv_hal.h"
#include "nv_usb.h"
#include "nv_wifi.h"
#include "nv_audio.h"
#include "nv_sd.h"
#include "nv_time.h"
#include "nv_i18n.h"
#include "nv_event_bus.h"
#include "nv_fonts.h"
#include "nv_theme.h"

#include "esp_app_desc.h"  // running-firmware version (post-update boot notification)
#include "esp_heap_caps.h" // 64B-aligned PSRAM wallpaper buffers (PPA cache-line requirement)
#include "driver/ppa.h"    // hardware rotate of the cached wallpaper (portrait variant)
#include <sys/stat.h>      // wallpaper file presence probe

#include <cstdio>   // snprintf (launcher order persistence keys)
#include <cstdint>  // intptr_t (slot <-> user_data packing)
#include <cstring>  // strcmp (PIN compare)

static const char *TAG = "ui";

// ================================================================= app registry (public C)
namespace {
constexpr int kMaxApps = 32;
const NvApp *g_apps[kMaxApps];
int g_app_n = 0;
}  // namespace

void nv_app_register(const NvApp *app) {
    if (app && g_app_n < kMaxApps) g_apps[g_app_n++] = app;
}
int nv_app_count(void) { return g_app_n; }
const NvApp *nv_app_at(int i) { return (i >= 0 && i < g_app_n) ? g_apps[i] : nullptr; }

// ================================================================= widget kit (public C)
lv_obj_t *nv_kit_scroll_column(lv_obj_t *parent) {
    lv_obj_t *c = lv_obj_create(parent);
    lv_obj_remove_style_all(c);
    lv_obj_set_size(c, lv_pct(100), lv_pct(100));
    lv_obj_set_style_pad_all(c, NV_SP_4, 0);   // spacing scale: 16
    lv_obj_set_style_pad_row(c, NV_SP_3, 0);   // spacing scale: 12
    lv_obj_set_flex_flow(c, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scroll_dir(c, LV_DIR_VER);
    // A slim auto-hiding scroll indicator so long pages read as scrollable (layer-free).
    lv_obj_set_scrollbar_mode(c, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_set_style_bg_color(c, nv_theme_get()->text_dim, LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_opa(c, LV_OPA_40, LV_PART_SCROLLBAR);
    lv_obj_set_style_width(c, 4, LV_PART_SCROLLBAR);
    lv_obj_set_style_radius(c, LV_RADIUS_CIRCLE, LV_PART_SCROLLBAR);
    return c;
}
lv_obj_t *nv_kit_info(lv_obj_t *parent) {
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_width(l, lv_pct(100));
    lv_obj_set_style_text_color(l, nv_theme_get()->text, 0);  // primary body copy (was text_dim)
    return l;
}
lv_obj_t *nv_kit_row(lv_obj_t *parent, const char *label) {
    lv_obj_t *r = lv_obj_create(parent);
    lv_obj_remove_style_all(r);
    lv_obj_set_size(r, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_style_min_height(r, 56, 0);   // >= 44px touch target, with breathing room
    lv_obj_set_style_bg_color(r, nv_theme_get()->surface, 0);
    lv_obj_set_style_bg_opa(r, LV_OPA_COVER, 0);
    // Layer-free tap feedback: rows made CLICKABLE (choice/nav rows) lift to surface2 when pressed;
    // static rows never enter the pressed state, so this is a no-op for them.
    lv_obj_set_style_bg_color(r, nv_theme_get()->surface2, LV_STATE_PRESSED);
    lv_obj_set_style_radius(r, NV_RAD_SM, 0);
    lv_obj_set_style_pad_all(r, NV_SP_4, 0);      // spacing scale: 16
    lv_obj_set_flex_flow(r, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(r, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(r, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *l = lv_label_create(r);
    lv_label_set_text(l, label);
    lv_obj_set_style_text_color(l, nv_theme_get()->text, 0);
    return r;
}
// Shared themed button — consistent radius, >=44px target, layer-free pressed feedback.
lv_obj_t *nv_kit_button(lv_obj_t *parent, const char *label, bool primary) {
    const NvTheme *th = nv_theme_get();
    lv_obj_t *b = lv_button_create(parent);
    lv_obj_set_style_min_height(b, NV_TOUCH_MIN, 0);
    lv_obj_set_style_min_width(b, NV_TOUCH_MIN, 0);
    lv_obj_set_style_radius(b, NV_RAD_SM, 0);
    lv_obj_set_style_pad_hor(b, NV_SP_4, 0);
    lv_obj_set_style_bg_color(b, primary ? th->primary : th->surface3, 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_80, LV_STATE_PRESSED);   // press dip, no draw layer
    lv_obj_set_style_text_color(b, primary ? th->on_primary : th->text_strong, 0);
    // Grow the hit box a touch beyond the visual bounds so near-misses still register.
    // NucleoOS buttons are spaced >= NV_SP_3 apart, so 8px never overlaps a neighbour.
    lv_obj_set_ext_click_area(b, NV_SP_2);
    lv_obj_t *l = lv_label_create(b);
    lv_label_set_text(l, label);
    lv_obj_center(l);
    return b;
}
lv_obj_t *nv_kit_slider_row(lv_obj_t *col, const char *name, int value, int lo, int hi,
                            lv_event_cb_t cb) {
    lv_obj_t *s = lv_slider_create(nv_kit_row(col, name));
    // Fill the remaining row width responsively (was a fixed 320 that could clip long labels).
    lv_obj_set_flex_grow(s, 1);
    lv_obj_set_style_min_width(s, 200, 0);
    lv_obj_set_style_pad_all(s, 6, LV_PART_KNOB);   // enlarge the knob drag target
    lv_slider_set_range(s, lo, hi);
    lv_slider_set_value(s, value, LV_ANIM_OFF);
    lv_obj_add_event_cb(s, cb, LV_EVENT_VALUE_CHANGED, nullptr);
    return s;
}
lv_obj_t *nv_kit_switch_row(lv_obj_t *col, const char *name, bool on, lv_event_cb_t cb) {
    lv_obj_t *sw = lv_switch_create(nv_kit_row(col, name));
    if (on) lv_obj_add_state(sw, LV_STATE_CHECKED);
    lv_obj_add_event_cb(sw, cb, LV_EVENT_VALUE_CHANGED, nullptr);
    return sw;
}
lv_obj_t *nv_kit_textarea_ex(lv_obj_t *parent, const char *placeholder, bool one_line,
                             nv_ime_type_t type, nv_ime_return_t ret) {
    const NvTheme *th = nv_theme_get();
    lv_obj_t *ta = lv_textarea_create(parent);
    lv_textarea_set_one_line(ta, one_line);
    if (placeholder) lv_textarea_set_placeholder_text(ta, placeholder);
    lv_obj_set_style_bg_color(ta, th->surface, 0);
    lv_obj_set_style_bg_opa(ta, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(ta, th->surface2, 0);
    lv_obj_set_style_text_color(ta, th->text, 0);
    lv_obj_set_style_radius(ta, 12, 0);
    nv_ime_bind_ex(ta, type, ret);  // auto-shows the shared IME on tap, class-aware
    return ta;
}
lv_obj_t *nv_kit_textarea(lv_obj_t *parent, const char *placeholder, bool one_line) {
    return nv_kit_textarea_ex(parent, placeholder, one_line, NV_IME_TEXT, NV_IME_RET_DEFAULT);
}

// ================================================================= SystemUI internals
namespace {

constexpr int kStatusH = 34;
constexpr int kHeaderH = 44;
constexpr int kHomeInset = 24;   // reserved bottom band for the home pill + BOTTOM gesture strip

// ---- design scale (spacing / radius / touch) — snap literals onto one deliberate ladder ----
constexpr int kSp2      = 8;    // dense gaps
constexpr int kSp3      = 12;   // default gap
constexpr int kSp4      = 16;   // default padding
constexpr int kRadSm    = 12;   // rows, keys, buttons, textareas
constexpr int kRadMd    = 16;   // cards, tiles, chips
constexpr int kTouchMin = 44;   // minimum touch target (both axes)

// ---- notification shade ----
constexpr int kShadeH    = 480;   // panel height (header + QS + sliders + notif on a 600px display)
constexpr uint32_t kShadeMs = 240;  // slide + fade duration

// ---- launcher grid geometry — RUNTIME: recomputed per orientation in grid_compute().
// 6x3 landscape / 3x6 portrait; both are 18 slots per page, so the persisted order maps
// 1:1 across a rotation. Coordinates are strip-local (pages laid side by side).
constexpr int kTileW    = 136;               // transparent hit-box width
constexpr int kTileH    = 122;               // transparent hit-box height
constexpr int kIconY    = 0;                 // icon top within tile
constexpr int kLabelY   = 92;                // label top within tile (4px baseline gap)

struct GridGeom {
    int cols, rows;    // 6x3 landscape / 3x6 portrait
    int cellW, cellH;  // slot pitch
    int padL, padT;    // origin of the page-local slot 0
    int cap;           // slots per page (18 in both orientations)
};
GridGeom s_g;          // valid after grid_compute() (start of build_launcher)

// ---- drag / reorder / pages ----
constexpr uint32_t kSnapDurMs   = 200;   // relayout animation duration
constexpr int      kMaxPages    = 3;     // ceil((32 apps + 8 folders) / 18)
constexpr uint32_t kPageAnimMs  = 220;   // page slide duration
constexpr int      kEdgeFlipPx  = 28;    // finger-at-edge zone that flips the page mid-drag...
constexpr uint32_t kEdgeFlipMs  = 450;   // ...after this dwell
constexpr uint32_t kReorderDwellMs = 200;  // hover a NEW slot this long before neighbours reshuffle
                                           // — lets you settle onto a tile's centre to form a folder
                                           // instead of the target darting away (reorder was instant)

// ---- folders ----
constexpr int kMaxFolders = 8;
constexpr int kFolderCap  = 9;           // members per folder (fits the 3-wide overlay grid)
constexpr int kEntFolder  = 100;         // entry >= kEntFolder -> folder id (entry - kEntFolder)

// ---- smart dock (usage-ranked favorites pill) ----
constexpr int  kDockN    = 5;              // dock slots
constexpr int  kDockIcon = 56;             // rendered icon + tap cell size
constexpr int  kDockH    = 64;             // pill height
constexpr int  kDockMarg = 14;             // pill <-> screen-bottom gap
constexpr char kUsageKeyFmt[] = "lu_%.11s"; // launch counter per app id (NVS key <= 15 chars)

// ---- recents (recency-ordered task switcher; RAM-only, clears on reboot like Android) ----
constexpr int kRecentsN = 6;               // most-recent apps kept (fit on one screen, no scroll)
constexpr int kThumbW   = 176;             // card preview: PPA-downscaled RGB565 grabbed on exit
constexpr int kThumbH   = 104;             // ~panel aspect (1024:600); raw .bin on SD

// ---- order persistence (schema v2: entries + folders; v1 "lord%d" read once to migrate) ----
constexpr char kOrdCountKey[]   = "lgn";      // entry count (missing -> migrate from v1)
constexpr char kOrdEntryFmt[]   = "lge%d";    // slot -> entry value
constexpr char kFolderNFmt[]    = "lgf%dn";   // folder -> member count (0 = free record)
constexpr char kFolderMemFmt[]  = "lgf%dm%d"; // folder, member -> app registry index
constexpr char kFolderNameFmt[] = "lgf%dnm";  // folder -> display name (string)
constexpr char kOrderKeyFmt[]   = "lord%d";   // LEGACY v1 key

lv_obj_t *s_statusbar = nullptr;
lv_obj_t *s_clock = nullptr;
lv_obj_t *s_date  = nullptr;
lv_obj_t *s_heap = nullptr;
lv_obj_t *s_wifi_ico = nullptr;     // status-bar Wi-Fi glyph (driven by qs_wifi config)
lv_obj_t *s_wifi_ssid = nullptr;    // connected SSID shown next to the glyph (hidden when not connected)
lv_obj_t *s_sd_ico = nullptr;       // status-bar SD glyph (visible only while a card is mounted)
lv_obj_t *s_launcher = nullptr;
lv_obj_t *s_shade_scrim = nullptr;  // full-screen dim + tap-to-close catcher (screen child)
lv_obj_t *s_shade = nullptr;        // sliding panel (child of scrim)
bool      s_shade_open = false;     // single source of truth; blocks double-fire / stuck scrim
bool      s_shade_gesture_on = true; // false => the shade bezel/status swipe is ignored (video player)
lv_obj_t *s_bell        = nullptr;  // status-bar unread badge (bell + count; hidden at 0)
lv_obj_t *s_shade_clock = nullptr;  // shade hero clock — refreshed on every open (was stale)
lv_obj_t *s_shade_date  = nullptr;
lv_obj_t *s_notif_list  = nullptr;  // shade notification list (rebuilt via nv_notify listener)
lv_obj_t *s_notif_clear = nullptr;  // "Clear all" button (hidden while the list is empty)

void refresh_shade_clock(void) {
    if (!s_shade_clock) return;
    char hero[24];
    nv_time_format(hero, sizeof hero, nv_time_is_24h() ? "%H:%M" : "%I:%M %p");
    lv_label_set_text(s_shade_clock, hero);
    char subd[40];
    nv_time_format(subd, sizeof subd, "%A %d %B");
    lv_label_set_text(s_shade_date, subd);
}

void update_bell(void) {
    if (!s_bell) return;
    const int u = nv_notify_unread();
    if (u > 0) {
        lv_label_set_text_fmt(s_bell, LV_SYMBOL_BELL " %d", u);
        lv_obj_remove_flag(s_bell, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_bell, LV_OBJ_FLAG_HIDDEN);
    }
}

// Themed wallpaper: a subtle vertical gradient (canvas -> surface) behind the launcher and the
// lock screen. Layer-free (inline gradient fill — no shadow/transform, safe on the P4 sw
// renderer). A future SD image override slots in here. Re-applied on theme change.
void apply_wallpaper(lv_obj_t *o) {
    const NvTheme *th = nv_theme_get();
    lv_obj_set_style_bg_color(o, th->bg, 0);
    lv_obj_set_style_bg_grad_color(o, th->surface, 0);
    lv_obj_set_style_bg_grad_dir(o, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
}

// -------------------------------------------------------------- wallpaper (SD, decode-once)
// A user wallpaper at /sdcard/wallpaper.jpg (exactly panel-sized, 1024x600) is decoded ONCE
// into a 64-byte-aligned PSRAM buffer and blitted as a plain lv_image behind the page strip —
// LVGL never re-decodes it, so per-frame cost equals any static bitmap. The portrait variant
// is produced on demand by the P4 PPA (hardware rotate, blocking, ~ms — the driver handles
// cache sync for aligned buffers) and cached too: orientation toggles stay instant.
// Any failure (no SD, missing/undersized file, decoder error, OOM) falls back to the themed
// gradient silently — the wallpaper is strictly additive.
constexpr char kWallPath[]   = "/sdcard/wallpaper.jpg";
constexpr char kWallLvPath[] = "S:/sdcard/wallpaper.jpg";
// PPA rotation (counter-clockwise) used for the portrait variant. If the portrait wallpaper
// shows upside down on hardware, flip this to PPA_SRM_ROTATION_ANGLE_270 — nothing else moves.
constexpr ppa_srm_rotation_angle_t kWallPortraitAngle = PPA_SRM_ROTATION_ANGLE_90;

uint8_t       *s_wall_land = nullptr;   // panel-native (1024x600) RGB565, 64B-aligned PSRAM
uint8_t       *s_wall_port = nullptr;   // rotated 600x1024 variant (lazy, PPA-generated)
lv_image_dsc_t s_wall_dsc;              // src descriptor; re-pointed per orientation
bool           s_wall_failed = false;   // decode failed once -> stop retrying this boot

int wall_w(void) { return LV_HOR_RES > LV_VER_RES ? LV_HOR_RES : LV_VER_RES; }  // 1024
int wall_h(void) { return LV_HOR_RES > LV_VER_RES ? LV_VER_RES : LV_HOR_RES; }  // 600

bool wall_load_landscape(void) {
    if (s_wall_land) return true;
    if (s_wall_failed || !nv_sd_is_mounted()) return false;
    struct stat st;
    if (stat(kWallPath, &st) != 0) return false;   // no file: silent, retry on next rebuild

    lv_image_decoder_dsc_t dsc;
    if (lv_image_decoder_open(&dsc, kWallLvPath, nullptr) != LV_RESULT_OK) {
        NV_LOGW(TAG, "wallpaper: decoder open failed");
        s_wall_failed = true;
        return false;
    }
    const lv_draw_buf_t *db = dsc.decoded;
    const int W = wall_w(), H = wall_h();
    if (db && (int)db->header.w == W && (int)db->header.h == H &&
        db->header.cf == LV_COLOR_FORMAT_RGB565) {
        // Tight, cache-line-aligned PSRAM copy (the decoder's stride may be padded; the PPA
        // needs 64B alignment on both buffers).
        s_wall_land = (uint8_t *)heap_caps_aligned_calloc(64, 1, (size_t)W * H * 2,
                                                          MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (s_wall_land) {
            const uint32_t src_stride = db->header.stride ? db->header.stride : (uint32_t)W * 2;
            for (int y = 0; y < H; y++)
                memcpy(s_wall_land + (size_t)y * W * 2,
                       (const uint8_t *)db->data + (size_t)y * src_stride, (size_t)W * 2);
            NV_LOGI(TAG, "wallpaper: cached %dx%d RGB565 in PSRAM", W, H);
        }
    } else {
        NV_LOGW(TAG, "wallpaper: need exactly %dx%d RGB565 jpg (got %ux%u cf=%d)",
                W, H, db ? (unsigned)db->header.w : 0, db ? (unsigned)db->header.h : 0,
                db ? (int)db->header.cf : -1);
        s_wall_failed = true;
    }
    lv_image_decoder_close(&dsc);
    return s_wall_land != nullptr;
}

bool wall_ensure_portrait(void) {
    if (s_wall_port) return true;
    if (!s_wall_land) return false;
    const int W = wall_w(), H = wall_h();
    s_wall_port = (uint8_t *)heap_caps_aligned_calloc(64, 1, (size_t)W * H * 2,
                                                      MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_wall_port) return false;

    ppa_client_handle_t cl = nullptr;
    ppa_client_config_t ccfg = {};
    ccfg.oper_type = PPA_OPERATION_SRM;
    if (ppa_register_client(&ccfg, &cl) != ESP_OK) {
        heap_caps_free(s_wall_port);
        s_wall_port = nullptr;
        return false;
    }
    ppa_srm_oper_config_t op = {};
    op.in.buffer         = s_wall_land;
    op.in.pic_w          = W;
    op.in.pic_h          = H;
    op.in.block_w        = W;
    op.in.block_h        = H;
    op.in.srm_cm         = PPA_SRM_COLOR_MODE_RGB565;
    op.out.buffer        = s_wall_port;
    op.out.buffer_size   = (uint32_t)W * H * 2;
    op.out.pic_w         = H;                       // rotated: 600 x 1024
    op.out.pic_h         = W;
    op.out.srm_cm        = PPA_SRM_COLOR_MODE_RGB565;
    op.rotation_angle    = kWallPortraitAngle;
    op.scale_x           = 1.0f;
    op.scale_y           = 1.0f;
    op.mode              = PPA_TRANS_MODE_BLOCKING;
    const esp_err_t err = ppa_do_scale_rotate_mirror(cl, &op);
    ppa_unregister_client(cl);
    if (err != ESP_OK) {
        NV_LOGW(TAG, "wallpaper: PPA rotate failed (%d)", (int)err);
        heap_caps_free(s_wall_port);
        s_wall_port = nullptr;
    }
    return s_wall_port != nullptr;
}

// Called from build_launcher: adds the wallpaper image (if available for the current
// orientation) as the FIRST launcher child, so everything else draws above it.
void wall_attach(lv_obj_t *launcher) {
    if (!wall_load_landscape()) return;
    const bool portrait = LV_VER_RES > LV_HOR_RES;
    const uint8_t *buf = portrait ? (wall_ensure_portrait() ? s_wall_port : nullptr)
                                  : s_wall_land;
    if (!buf) return;
    s_wall_dsc = {};
    s_wall_dsc.header.magic  = LV_IMAGE_HEADER_MAGIC;
    s_wall_dsc.header.cf     = LV_COLOR_FORMAT_RGB565;
    s_wall_dsc.header.w      = LV_HOR_RES;
    s_wall_dsc.header.h      = LV_VER_RES;
    s_wall_dsc.header.stride = (uint32_t)LV_HOR_RES * 2;
    s_wall_dsc.data          = buf;
    s_wall_dsc.data_size     = (uint32_t)LV_HOR_RES * LV_VER_RES * 2;
    lv_obj_t *w = lv_image_create(launcher);
    lv_image_set_src(w, &s_wall_dsc);
    // Full-screen image on a status-bar-inset launcher: shift up so the visible part is the
    // image's lower region (the top kStatusH px sit under the status bar and are clipped).
    lv_obj_align(w, LV_ALIGN_TOP_MID, 0, -kStatusH);
    lv_obj_remove_flag(w, LV_OBJ_FLAG_CLICKABLE);
}

lv_obj_t *s_app = nullptr;          // current foreground app (solo-mode); NULL at home
lv_obj_t *s_app_title = nullptr;    // header title label (apps may retitle it)
lv_obj_t *s_app_content = nullptr;  // app content area (apps rebuild it for in-app nav)
lv_obj_t *s_app_hdr = nullptr;      // header bar (resized in place on display rotation)
lv_obj_t *s_app_pill = nullptr;     // home indicator (re-anchors itself; kept for symmetry)
bool      s_fullscreen = false;      // a game asked for the whole panel (no status bar / header / pill)
void (*s_app_back)(void) = nullptr; // in-app back handler; NULL => Back closes the app
const NvApp *s_app_cur = nullptr;   // descriptor of the open app; NULL at home (for live re-render)

// Translated launcher/title label for an app: nv_tr(name_id) when set, else the English .name.
const char *app_label(const NvApp *a) {
    if (!a) return "";
    return a->name_id >= 0 ? nv_tr((nv_str_id_t)a->name_id) : a->name;
}

// ---- launcher model + edit-mode state ----
// An ENTRY occupies one grid slot: an app (value = registry index) or a folder
// (value = kEntFolder + folder id). Folders hold app registry indices only — no nesting.
constexpr int kMaxEntries = kMaxApps + kMaxFolders;   // 40; fits within kMaxPages pages

struct Folder {
    char name[24];
    int  mem[kFolderCap];   // app registry indices
    int  n;                 // member count; 0 == free record
};
Folder    s_folders[kMaxFolders] = {};

lv_obj_t *s_strip = nullptr;                  // wide slide plane: pages laid side by side
lv_obj_t *s_dots  = nullptr;                  // page indicator (one dot child per page)
lv_obj_t *s_tiles[kMaxEntries] = {nullptr};   // tile obj per VISUAL slot; index == slot
int       s_order[kMaxEntries] = {0};         // slot -> entry (the model)
int       s_tile_n             = 0;           // number of live entries
int       s_pages              = 1;           // page count (= ceil(s_tile_n / s_g.cap))
int       s_page               = 0;           // current page (kept across rebuilds, clamped)

bool      s_launcher_edit      = false;      // true while in edit/reorder mode
int       s_drag_slot          = -1;         // slot index of the tile being dragged, or -1
int       s_merge_slot         = -1;         // armed folder-merge target slot while dragging, or -1
uint32_t  s_edge_since         = 0;          // lv_tick when the drag entered an edge zone (0 = outside)
int       s_reorder_pending    = -1;         // slot the drag is dwelling over for a reorder, or -1
uint32_t  s_reorder_since      = 0;          // lv_tick when that dwell started (0 = none)

bool entry_is_folder(int e) { return e >= kEntFolder; }
Folder *entry_folder(int e) { return entry_is_folder(e) ? &s_folders[e - kEntFolder] : nullptr; }

void folder_open(int f);        // fwd: modal folder overlay (defined after the search overlay)
void rebuild_launcher(void);    // fwd: delete + rebuild the launcher subtree (defined below)
void dock_refresh(void);        // fwd: re-rank + rebuild the smart dock (defined below)
void usage_bump(const NvApp *a);           // fwd: launch counter (smart dock ranking)
void recents_push(const NvApp *a);         // fwd: recency-ordered task switcher (defined below)
void open_recents(void);                   // fwd: recents overlay (bottom-edge swipe at home)
void search_open(lv_event_t *);            // fwd: search overlay (swipe-down gesture opens it)
void search_close_deferred(void);          // fwd: dismiss the search overlay (Back / left-edge)
bool search_is_open(void);                 // fwd: true while the search overlay is up

// -------------------------------------------------------------- status bar
void status_tick(lv_timer_t *) {
    // Real wall clock (NTP-synced once online; build-time seed before that).
    char tbuf[24];
    nv_time_format(tbuf, sizeof(tbuf), nv_time_is_24h() ? "%H:%M" : "%I:%M %p");
    lv_label_set_text(s_clock, tbuf);
    if (s_date) {
        // Localized date: strftime's %a/%b are C-locale (English only), so build it from the
        // active language's calendar names instead.
        struct tm tmv;
        nv_time_now(&tmv);
        lv_label_set_text_fmt(s_date, "%s %02d %s",
                              nv_i18n_wday_short(tmv.tm_wday), tmv.tm_mday,
                              nv_i18n_month_short(tmv.tm_mon));
    }
    // Right cluster: the Wi-Fi glyph reflects the real radio state (heap HUD removed — that debug
    // readout lives in Settings > Memory / Diagnostics now, not the always-on status bar).
    if (s_wifi_ico) {
        const NvTheme *th = nv_theme_get();
        const bool en = nv_wifi_is_enabled();
        const nv_wifi_state_t st = en ? nv_wifi_get_state() : NV_WIFI_DISABLED;
        // "Online" = associated with an IP AND the internet was actually reached (SNTP synced).
        // SNTP is a one-shot-per-connection sync, so this needs no continuous ping.
        const bool online = (st == NV_WIFI_CONNECTED) && nv_time_is_synced();

        // Glyph colour encodes connectivity, not raw signal: green = online (internet confirmed),
        // accent = linked but not yet online / scanning / connecting, red = failed, dim = off.
        lv_color_t c = th->text_dim;
        switch (st) {
            case NV_WIFI_CONNECTED:  c = online ? th->success_solid : th->accent; break;
            case NV_WIFI_FAILED:     c = th->danger; break;
            case NV_WIFI_SCANNING:
            case NV_WIFI_CONNECTING: c = th->accent; break;
            default:                 c = th->text_dim; break;
        }
        lv_obj_set_style_text_color(s_wifi_ico, c, 0);

        // SSID label next to the glyph: show the connected network name, hide it otherwise.
        if (s_wifi_ssid) {
            char ssid[33] = "";
            if (st == NV_WIFI_CONNECTED && nv_wifi_get_connected(ssid, sizeof(ssid), nullptr, 0, nullptr)) {
                lv_label_set_text(s_wifi_ssid, ssid);
                lv_obj_set_style_text_color(s_wifi_ssid, c, 0);
                lv_obj_remove_flag(s_wifi_ssid, LV_OBJ_FLAG_HIDDEN);
            } else {
                lv_obj_add_flag(s_wifi_ssid, LV_OBJ_FLAG_HIDDEN);
            }
        }

        // Feedback on state changes (runs on the LVGL thread -> safe to touch widgets/toasts).
        static int  s_last_wifi_st = -1;
        static bool s_last_online  = false;
        if ((int)st != s_last_wifi_st) {
            if (st == NV_WIFI_CONNECTED) {
                char ip[16] = "";
                nv_wifi_get_connected(nullptr, 0, ip, sizeof(ip), nullptr);
                char m[64];
                lv_snprintf(m, sizeof(m), "%s  -  IP %s", nv_tr(NV_STR_WIFI_CONNECTED), ip);
                nv_notify_post(NV_NOTE_OK, "Wi-Fi", m);   // toast + stored in the shade
            } else if (st == NV_WIFI_FAILED) {
                nv_notify_post(NV_NOTE_WARN, "Wi-Fi", nv_tr(NV_STR_WIFI_FAILED));
            }
            s_last_wifi_st = (int)st;
        }
        if (online != s_last_online) {
            if (online) nv_toast(NV_NOTE_OK, nv_tr(NV_STR_WIFI_ONLINE));  // transient only
            s_last_online = online;
        }
    }
    if (s_sd_ico) {   // show the SD glyph only while a card is actually mounted
        if (nv_sd_is_mounted()) lv_obj_remove_flag(s_sd_ico, LV_OBJ_FLAG_HIDDEN);
        else                    lv_obj_add_flag(s_sd_ico, LV_OBJ_FLAG_HIDDEN);
    }
}

// -------------------------------------------------------------- notification shade
// Object tree (Android-style):
//   s_shade_scrim (screen child; full-screen dim + tap-to-close; hidden when closed)
//   └─ s_shade    (panel; slides y from -kShadeH -> 0; child of scrim so raising/hiding
//                  the scrim moves the whole subtree at once)
//   nv_gesture TOP edge strip (centralized) owns the open/close bezel swipes
// s_shade_open is the single source of truth; both open/close are idempotent under it, and
// the scrim's HIDDEN flag is added ONLY by the close-completed callback so an interrupted
// animation can never leave a stuck, dead scrim.

void sync_qs_chips(void);  // fwd: defined with the quick-settings chips below

void shade_anim_y(void *var, int32_t v)   { lv_obj_set_y((lv_obj_t *)var, v); }
void shade_anim_opa(void *var, int32_t v) {
    lv_obj_set_style_bg_opa((lv_obj_t *)var, (lv_opa_t)v, 0);
}
void shade_closed_done(lv_anim_t *a) {  // hide the scrim subtree only after the fade-out
    lv_obj_add_flag((lv_obj_t *)a->var, LV_OBJ_FLAG_HIDDEN);
}

void open_shade(void) {
    if (!s_shade || s_shade_open) return;   // idempotent: no double-open
    s_shade_open = true;

    // Supersede any in-flight close anims so open wins cleanly (no stuck partial state).
    lv_anim_delete(s_shade, shade_anim_y);
    lv_anim_delete(s_shade_scrim, shade_anim_opa);

    // Dismiss the keyboard so it can't sit over the shade, then raise the whole shade subtree
    // above the app plane AND the IME, and keep the catcher just under it.
    nv_ime_hide();
    refresh_shade_clock();                   // hero clock/date current at the moment of open
    sync_qs_chips();                         // re-read real state (Settings may have changed it)
    nv_notify_mark_read();                   // opening the shade clears the status-bar badge
    lv_obj_remove_flag(s_shade_scrim, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_shade_scrim);   // scrim (+ panel child) to top
    nv_gesture_raise();                      // edge strips back on top (close swipe stays reachable)

    lv_anim_t ay;  lv_anim_init(&ay);
    lv_anim_set_var(&ay, s_shade);
    lv_anim_set_exec_cb(&ay, shade_anim_y);
    lv_anim_set_values(&ay, lv_obj_get_y(s_shade), 0);   // slide down to 0
    lv_anim_set_duration(&ay, kShadeMs);
    lv_anim_set_path_cb(&ay, lv_anim_path_ease_out);
    lv_anim_start(&ay);

    lv_anim_t ao;  lv_anim_init(&ao);
    lv_anim_set_var(&ao, s_shade_scrim);
    lv_anim_set_exec_cb(&ao, shade_anim_opa);
    lv_anim_set_values(&ao, LV_OPA_TRANSP, nv_theme_get()->scrim_opa);  // theme-tuned dim level
    lv_anim_set_duration(&ao, kShadeMs);
    lv_anim_start(&ao);
    NV_LOGI(TAG, "notification shade opened");
}

void close_shade(void) {
    if (!s_shade || !s_shade_open) return;   // idempotent: no double-close
    s_shade_open = false;

    lv_anim_delete(s_shade, shade_anim_y);       // supersede any open anim
    lv_anim_delete(s_shade_scrim, shade_anim_opa);

    lv_anim_t ay;  lv_anim_init(&ay);
    lv_anim_set_var(&ay, s_shade);
    lv_anim_set_exec_cb(&ay, shade_anim_y);
    lv_anim_set_values(&ay, lv_obj_get_y(s_shade), -kShadeH);   // slide up off the top
    lv_anim_set_duration(&ay, kShadeMs);
    lv_anim_set_path_cb(&ay, lv_anim_path_ease_in);
    lv_anim_start(&ay);

    lv_anim_t ao;  lv_anim_init(&ao);
    lv_anim_set_var(&ao, s_shade_scrim);
    lv_anim_set_exec_cb(&ao, shade_anim_opa);
    lv_anim_set_values(&ao, lv_obj_get_style_bg_opa(s_shade_scrim, LV_PART_MAIN), LV_OPA_TRANSP);
    lv_anim_set_duration(&ao, kShadeMs);
    lv_anim_set_completed_cb(&ao, shade_closed_done);   // HIDDEN only after fade-out
    lv_anim_start(&ao);
    NV_LOGI(TAG, "notification shade closing");
}

void scrim_tap_cb(lv_event_t *) { close_shade(); }   // tap the dim area behind the panel

// nv_gesture TOP edge: swipe DOWN opens the shade, swipe UP closes it (strip sits above the panel)
void top_edge_cb(lv_dir_t dir, void *) {
    if (dir == LV_DIR_BOTTOM) { if (s_shade_gesture_on) open_shade(); }
    else if (dir == LV_DIR_TOP && s_shade_open) close_shade();
}
void status_gesture(lv_event_t *) {  // redundant path: swipe down on the status bar -> open
    if (!s_shade_gesture_on) return;
    lv_indev_t *ind = lv_indev_active();
    if (ind && lv_indev_get_gesture_dir(ind) == LV_DIR_BOTTOM) open_shade();
}
void shade_gesture(lv_event_t *) {   // swipe up on the shade panel -> close
    lv_indev_t *ind = lv_indev_active();
    if (ind && lv_indev_get_gesture_dir(ind) == LV_DIR_TOP) close_shade();
}
void qs_toggle(lv_event_t *e) {
    lv_obj_t *b = lv_event_get_target_obj(e);
    auto *key = static_cast<const char *>(lv_event_get_user_data(e));
    const bool on = lv_obj_has_state(b, LV_STATE_CHECKED);
    nv_config_set_bool(key, on);
    NV_LOGI(TAG, "quick setting '%s' = %d", key, on);
}
// Wi-Fi chip: actually powers the radio (nv_wifi) in addition to persisting the toggle, so the
// quick setting reflects and controls real state instead of a dead flag.
void qs_wifi_toggle(lv_event_t *e) {
    const bool on = lv_obj_has_state(lv_event_get_target_obj(e), LV_STATE_CHECKED);
    nv_wifi_set_enabled(on);
    nv_config_set_bool("qs_wifi", on);
    NV_LOGI(TAG, "wi-fi radio %s", on ? "on" : "off");
}
// Mute chip: live-applies to the codec (same key/path as Settings -> Sound).
void qs_mute_toggle(lv_event_t *e) {
    const bool on = lv_obj_has_state(lv_event_get_target_obj(e), LV_STATE_CHECKED);
    nv_audio_set_mute(on);
    nv_config_set_bool("mute", on);
}
// Dark-theme chip: recomposes the theme; THEME_CHANGED triggers the async UI rebuild (the
// shade re-opens closed — acceptable, the whole chrome re-skins in one pass).
void qs_lock_cb(lv_event_t *) { close_shade(); nv_ui_lock(); }   // momentary action, not a toggle

// Screenshot: encode the panel framebuffer (P4 hardware JPEG) off the LVGL thread. The shade is
// closed first and the capture deferred ~350 ms so the dismissed shade isn't in the shot.
void screenshot_worker(void *arg) {
    char *path = static_cast<char *>(arg);
    const bool ok = nv_hal_screenshot(path);
    if (lvgl_port_lock(1000)) {
        nv_notify_post(ok ? NV_NOTE_OK : NV_NOTE_WARN, nv_tr(NV_STR_SCREENSHOT),
                       ok ? nv_tr(NV_STR_SHOT_SAVED) : nv_tr(NV_STR_SHOT_FAIL));
        lvgl_port_unlock();
    }
    free(path);
    vTaskDelete(nullptr);
}
void screenshot_deferred(lv_timer_t *t) {
    lv_timer_delete(t);
    mkdir("/sdcard/Screenshots", 0777);   // best effort; encoder write fails loudly if absent
    char *path = static_cast<char *>(malloc(64));
    if (!path) return;
    snprintf(path, 64, "/sdcard/Screenshots/shot-%lu.jpg",
             (unsigned long)(esp_timer_get_time() / 1000));
    // Low priority: the HW encoder blocks this task ~50 ms; keep it off the UI's back.
    if (xTaskCreate(screenshot_worker, "ss_cap", 4096, path, 3, nullptr) != pdPASS) free(path);
}
void qs_screenshot_cb(lv_event_t *) {
    close_shade();
    lv_timer_t *t = lv_timer_create(screenshot_deferred, 350, nullptr);
    lv_timer_set_repeat_count(t, 1);
}

void qs_theme_toggle(lv_event_t *e) {
    const bool dark = lv_obj_has_state(lv_event_get_target_obj(e), LV_STATE_CHECKED);
    nv_theme_set_mode(dark ? NV_THEME_DARK : NV_THEME_LIGHT);
}
// Rotate chip: swap landscape (1024x600) <-> portrait (600x1024). esp_lvgl_port re-renders
// through the PPA (sw_rotate); the whole chrome then rebuilds via the same deferred path a
// theme change uses (grid_compute() re-reads LV_HOR_RES there). Strips resize here because
// they are NOT rebuilt by the refresh.
void on_ui_invalidate(nv_event_t, const void *, void *);   // fwd: coalesced async UI rebuild
void qs_rotate_toggle(lv_event_t *e) {
    const bool portrait = lv_obj_has_state(lv_event_get_target_obj(e), LV_STATE_CHECKED);
    nv_config_set_int("rotation", portrait ? 90 : 0);
    lv_display_set_rotation(lv_display_get_default(),
                            portrait ? LV_DISPLAY_ROTATION_90 : LV_DISPLAY_ROTATION_0);
    nv_gesture_relayout();
    on_ui_invalidate((nv_event_t)0, nullptr, nullptr);
    NV_LOGI(TAG, "display rotation -> %d", portrait ? 90 : 0);
}
// initial_on: -1 => read from config; else force the chip's starting checked state.
lv_obj_t *qs_chip(lv_obj_t *parent, const char *label, const char *key,
                  lv_event_cb_t cb = qs_toggle, int initial_on = -1) {
    const NvTheme *th = nv_theme_get();
    lv_obj_t *b = lv_button_create(parent);
    lv_obj_add_flag(b, LV_OBJ_FLAG_CHECKABLE);
    lv_obj_set_size(b, 150, 56);                       // 56px tall: comfortably >= touch min
    lv_obj_set_style_radius(b, kRadMd, 0);             // 16: card/chip radius
    lv_obj_set_style_bg_color(b, th->surface2, 0);
    lv_obj_set_style_bg_color(b, th->primary, LV_STATE_CHECKED);
    lv_obj_set_style_bg_opa(b, LV_OPA_80, LV_STATE_PRESSED);  // press feedback
    const bool on = (initial_on >= 0) ? (initial_on != 0) : nv_config_get_bool(key, false);
    if (on) lv_obj_add_state(b, LV_STATE_CHECKED);
    lv_obj_add_event_cb(b, cb, LV_EVENT_VALUE_CHANGED, (void *)key);
    // Token the text on the BUTTON (inherited by the label) so it's legible on surface2 and,
    // when checked, on the primary fill — never the LVGL default grey.
    lv_obj_set_style_text_color(b, th->text, 0);
    lv_obj_set_style_text_color(b, th->on_primary, LV_STATE_CHECKED);
    lv_obj_t *l = lv_label_create(b);
    lv_label_set_text(l, label);
    lv_obj_center(l);
    return b;
}

// Chip refs + re-sync: state can change elsewhere (Settings pages), so every shade open
// re-reads the real sources instead of trusting the state captured at build time.
lv_obj_t *s_qs_wifi = nullptr, *s_qs_dark = nullptr, *s_qs_mute = nullptr, *s_qs_rot = nullptr;

void qs_set(lv_obj_t *chip, bool on) {
    if (!chip) return;
    if (on) lv_obj_add_state(chip, LV_STATE_CHECKED);
    else    lv_obj_remove_state(chip, LV_STATE_CHECKED);
}
void sync_qs_chips(void) {
    qs_set(s_qs_wifi, nv_wifi_is_enabled());
    qs_set(s_qs_dark, nv_theme_get_mode() == NV_THEME_DARK);
    qs_set(s_qs_mute, nv_config_get_bool("mute", false));
    qs_set(s_qs_rot, nv_config_get_int("rotation", 0) == 90);
    // DND has no other UI surface yet; its chip state IS the source of truth.
}
// Sliders: apply LIVE on VALUE_CHANGED, persist ONCE on RELEASED (a drag fires dozens of
// VALUE_CHANGED — writing NVS on each would wear flash and stutter the drag).
void shade_bright_cb(lv_event_t *e) {
    const int v = lv_slider_get_value(lv_event_get_target_obj(e));
    if (lv_event_get_code(e) == LV_EVENT_VALUE_CHANGED) nv_hal_backlight_set(v);
    else nv_config_set_int("brightness", v);
}
void shade_volume_cb(lv_event_t *e) {
    const int v = lv_slider_get_value(lv_event_get_target_obj(e));
    if (lv_event_get_code(e) == LV_EVENT_VALUE_CHANGED) nv_audio_set_volume(v);
    else nv_config_set_int("volume", v);
}

// icon + slider row (shared by brightness and volume)
lv_obj_t *shade_slider_row(lv_obj_t *panel, const NvTheme *th, const char *sym,
                           int min, int max, int val, lv_event_cb_t cb) {
    lv_obj_t *row = lv_obj_create(panel);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, kSp3, 0);         // 12
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *ico = lv_label_create(row);
    lv_label_set_text(ico, sym);
    lv_obj_set_style_text_color(ico, th->text_dim, 0);

    lv_obj_t *sl = lv_slider_create(row);
    lv_obj_set_flex_grow(sl, 1);
    lv_slider_set_range(sl, min, max);
    lv_slider_set_value(sl, val, LV_ANIM_OFF);
    lv_obj_set_style_pad_all(sl, 6, LV_PART_KNOB);     // enlarge the drag target toward 44px
    lv_obj_add_event_cb(sl, cb, LV_EVENT_VALUE_CHANGED, nullptr);
    lv_obj_add_event_cb(sl, cb, LV_EVENT_RELEASED, nullptr);
    return row;
}

// --- notification center list (rebuilt on every post/clear via the nv_notify listener) ---
void notif_clear_cb(lv_event_t *) { nv_notify_clear(); }

void rebuild_notif_list(void) {
    if (!s_notif_list) return;
    lv_obj_clean(s_notif_list);
    const NvTheme *th = nv_theme_get();
    const int n = nv_notify_count();

    if (s_notif_clear) {   // "Clear all" only makes sense with something to clear
        if (n) lv_obj_remove_flag(s_notif_clear, LV_OBJ_FLAG_HIDDEN);
        else   lv_obj_add_flag(s_notif_clear, LV_OBJ_FLAG_HIDDEN);
    }

    if (!n) {
        lv_obj_t *empty = lv_label_create(s_notif_list);
        lv_label_set_text_fmt(empty, LV_SYMBOL_BELL "  %s", nv_tr(NV_STR_NO_NOTIFICATIONS));
        lv_obj_set_style_text_color(empty, th->text_dim, 0);
        return;
    }

    for (int i = 0; i < n; i++) {
        const NvNote *note = nv_notify_get(i);
        if (!note) break;

        lv_obj_t *card = lv_obj_create(s_notif_list);
        lv_obj_remove_style_all(card);
        lv_obj_set_size(card, lv_pct(100), LV_SIZE_CONTENT);
        lv_obj_set_style_bg_color(card, th->surface2, 0);
        lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(card, kRadSm, 0);          // 12
        lv_obj_set_style_pad_all(card, kSp2 + 2, 0);       // 10
        lv_obj_set_style_pad_column(card, kSp3, 0);        // 12
        lv_obj_set_flex_flow(card, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(card, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t *ico = lv_label_create(card);
        lv_label_set_text(ico, nv_note_kind_symbol(note->kind));
        lv_obj_set_style_text_color(ico, nv_note_kind_color(note->kind), 0);

        lv_obj_t *col = lv_obj_create(card);
        lv_obj_remove_style_all(col);
        lv_obj_set_flex_grow(col, 1);
        lv_obj_set_height(col, LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
        lv_obj_clear_flag(col, LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t *title = lv_label_create(col);
        lv_label_set_text(title, note->title);
        lv_obj_set_style_text_font(title, &nv_font_14, 0);
        lv_obj_set_style_text_color(title, th->text_strong, 0);

        lv_obj_t *body = lv_label_create(col);
        lv_label_set_text(body, note->text);
        lv_obj_set_style_text_color(body, th->text, 0);
        lv_obj_set_width(body, lv_pct(100));
        lv_label_set_long_mode(body, LV_LABEL_LONG_MODE_WRAP);

        lv_obj_t *when = lv_label_create(card);
        lv_label_set_text(when, note->when);
        lv_obj_set_style_text_font(when, &nv_font_14, 0);
        lv_obj_set_style_text_color(when, th->text_dim, 0);
    }
}

// nv_notify listener: every post/clear/mark_read refreshes the badge and the (possibly
// hidden) shade list. Cheap: <= NV_NOTIFY_CAP small cards, LVGL thread only.
void on_notify_changed(void) {
    update_bell();
    rebuild_notif_list();
}

void build_shade_content(lv_obj_t *panel, const NvTheme *th) {
    // --- header: big clock + date (labels kept; refreshed live on every open) ---
    lv_obj_t *hdr = lv_obj_create(panel);
    lv_obj_remove_style_all(hdr);
    lv_obj_set_size(hdr, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(hdr, LV_FLEX_FLOW_COLUMN);
    lv_obj_clear_flag(hdr, LV_OBJ_FLAG_SCROLLABLE);

    s_shade_clock = lv_label_create(hdr);
    lv_obj_set_style_text_font(s_shade_clock, &nv_font_28, 0);   // hero clock
    lv_obj_set_style_text_color(s_shade_clock, th->text_strong, 0);

    s_shade_date = lv_label_create(hdr);
    lv_obj_set_style_text_color(s_shade_date, th->text_dim, 0);
    refresh_shade_clock();

    // --- quick settings: all four chips control REAL state (no dead flags) ---
    lv_obj_t *qs = lv_obj_create(panel);
    lv_obj_remove_style_all(qs);
    lv_obj_set_size(qs, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(qs, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_gap(qs, kSp3, 0);             // 12
    lv_obj_clear_flag(qs, LV_OBJ_FLAG_SCROLLABLE);
    s_qs_wifi = qs_chip(qs, LV_SYMBOL_WIFI "  Wi-Fi", "qs_wifi", qs_wifi_toggle,
                        nv_wifi_is_enabled());
    char dark[48];
    lv_snprintf(dark, sizeof dark, LV_SYMBOL_EYE_CLOSE "  %s", nv_tr(NV_STR_DARK));
    s_qs_dark = qs_chip(qs, dark, "thmode", qs_theme_toggle,
                        nv_theme_get_mode() == NV_THEME_DARK);
    qs_chip(qs, LV_SYMBOL_BELL "  DND", "qs_dnd");     // real: nv_notify reads it live
    char mute[48];  // symbol prefix + translated "Mute"
    lv_snprintf(mute, sizeof mute, LV_SYMBOL_MUTE "  %s", nv_tr(NV_STR_MUTE));
    s_qs_mute = qs_chip(qs, mute, "mute", qs_mute_toggle);
    char rot[48];   // portrait/landscape toggle (checked == portrait)
    lv_snprintf(rot, sizeof rot, LV_SYMBOL_REFRESH "  %s", nv_tr(NV_STR_ROTATE));
    s_qs_rot = qs_chip(qs, rot, "rotation", qs_rotate_toggle,
                       nv_config_get_int("rotation", 0) == 90);

    // "Lock now": a momentary action chip (not a toggle) — closes the shade and raises the lock.
    lv_obj_t *lockb = lv_button_create(qs);
    lv_obj_set_size(lockb, 150, 56);
    lv_obj_set_style_radius(lockb, kRadMd, 0);
    lv_obj_set_style_bg_color(lockb, th->surface2, 0);
    lv_obj_set_style_bg_opa(lockb, LV_OPA_80, LV_STATE_PRESSED);
    lv_obj_set_style_text_color(lockb, th->text, 0);
    lv_obj_add_event_cb(lockb, qs_lock_cb, LV_EVENT_CLICKED, nullptr);
    lv_obj_t *lockl = lv_label_create(lockb);
    lv_label_set_text(lockl, nv_tr(NV_STR_LOCK_NOW));
    lv_obj_center(lockl);

    // "Screenshot": momentary action chip — captures the panel to /Screenshots on the SD.
    lv_obj_t *shotb = lv_button_create(qs);
    lv_obj_set_size(shotb, 150, 56);
    lv_obj_set_style_radius(shotb, kRadMd, 0);
    lv_obj_set_style_bg_color(shotb, th->surface2, 0);
    lv_obj_set_style_bg_opa(shotb, LV_OPA_80, LV_STATE_PRESSED);
    lv_obj_set_style_text_color(shotb, th->text, 0);
    lv_obj_add_event_cb(shotb, qs_screenshot_cb, LV_EVENT_CLICKED, nullptr);
    lv_obj_t *shotl = lv_label_create(shotb);
    lv_label_set_text_fmt(shotl, LV_SYMBOL_IMAGE "  %s", nv_tr(NV_STR_SCREENSHOT));
    lv_obj_center(shotl);

    // --- sliders: brightness + volume, live apply / persist-on-release ---
    shade_slider_row(panel, th, LV_SYMBOL_EYE_OPEN, 5, 100,
                     nv_config_get_int("brightness", 90), shade_bright_cb);
    shade_slider_row(panel, th, LV_SYMBOL_VOLUME_MAX, 0, 100,
                     nv_config_get_int("volume", 60), shade_volume_cb);

    // --- notification center: section header + clear-all + scrollable list ---
    lv_obj_t *nh = lv_obj_create(panel);
    lv_obj_remove_style_all(nh);
    lv_obj_set_size(nh, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(nh, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(nh, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(nh, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *nt = lv_label_create(nh);
    lv_label_set_text(nt, nv_tr(NV_STR_NOTIFICATIONS));
    lv_obj_set_style_text_font(nt, &nv_font_14, 0);
    lv_obj_set_style_text_color(nt, th->text_dim, 0);

    s_notif_clear = lv_button_create(nh);
    lv_obj_set_style_bg_color(s_notif_clear, th->surface2, 0);
    lv_obj_set_style_bg_opa(s_notif_clear, LV_OPA_80, LV_STATE_PRESSED);
    lv_obj_set_style_radius(s_notif_clear, kRadSm, 0);
    lv_obj_set_style_pad_hor(s_notif_clear, kSp3, 0);
    lv_obj_set_style_pad_ver(s_notif_clear, 6, 0);
    lv_obj_add_event_cb(s_notif_clear, notif_clear_cb, LV_EVENT_CLICKED, nullptr);
    lv_obj_t *cl = lv_label_create(s_notif_clear);
    lv_label_set_text_fmt(cl, LV_SYMBOL_TRASH "  %s", nv_tr(NV_STR_CLEAR_ALL));
    lv_obj_set_style_text_font(cl, &nv_font_14, 0);
    lv_obj_set_style_text_color(cl, th->text, 0);

    s_notif_list = lv_obj_create(panel);
    lv_obj_remove_style_all(s_notif_list);
    lv_obj_set_width(s_notif_list, lv_pct(100));
    lv_obj_set_flex_grow(s_notif_list, 1);             // takes all remaining panel height
    lv_obj_set_flex_flow(s_notif_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s_notif_list, kSp2, 0);   // 8
    // Scrollable within its box (panel itself stays non-scrollable so the close swipe works).
    rebuild_notif_list();

    // --- bottom drag-handle affordance: a centered pill signalling "swipe up to close".
    // Pure rounded rect (layer-free). The shade still closes by swipe-up or scrim tap.
    lv_obj_t *griprow = lv_obj_create(panel);
    lv_obj_remove_style_all(griprow);
    lv_obj_set_size(griprow, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(griprow, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(griprow, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_margin_top(griprow, kSp2, 0);
    lv_obj_clear_flag(griprow, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *grip = lv_obj_create(griprow);
    lv_obj_remove_style_all(grip);
    lv_obj_set_size(grip, 44, 5);
    lv_obj_set_style_bg_color(grip, th->text_dim, 0);
    lv_obj_set_style_bg_opa(grip, LV_OPA_50, 0);
    lv_obj_set_style_radius(grip, LV_RADIUS_CIRCLE, 0);
}

void build_shade(lv_obj_t *scr) {
    const NvTheme *th = nv_theme_get();

    // --- scrim: full-screen dim + tap-to-close, starts hidden & transparent ---
    s_shade_scrim = lv_obj_create(scr);
    lv_obj_remove_style_all(s_shade_scrim);
    lv_obj_set_size(s_shade_scrim, LV_HOR_RES, LV_VER_RES);
    lv_obj_align(s_shade_scrim, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_bg_color(s_shade_scrim, th->scrim, 0);
    lv_obj_set_style_bg_opa(s_shade_scrim, LV_OPA_TRANSP, 0);   // fade target set in anim
    lv_obj_add_flag(s_shade_scrim, LV_OBJ_FLAG_CLICKABLE);      // catches taps on the dim area
    lv_obj_remove_flag(s_shade_scrim, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_shade_scrim, LV_OBJ_FLAG_HIDDEN);         // closed == hidden
    lv_obj_add_event_cb(s_shade_scrim, scrim_tap_cb, LV_EVENT_CLICKED, nullptr);

    // --- panel: child of scrim; raising/hiding the scrim moves the whole shade ---
    s_shade = lv_obj_create(s_shade_scrim);
    lv_obj_remove_style_all(s_shade);
    lv_obj_set_size(s_shade, LV_HOR_RES, kShadeH);
    lv_obj_set_pos(s_shade, 0, -kShadeH);                       // parked above the top edge
    lv_obj_set_style_bg_color(s_shade, th->shade_bg, 0);
    lv_obj_set_style_bg_opa(s_shade, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_shade, 0, 0);
    lv_obj_set_style_pad_all(s_shade, kSp4, 0);                 // 16
    lv_obj_set_style_pad_row(s_shade, kSp3, 0);                 // 12
    lv_obj_set_flex_flow(s_shade, LV_FLEX_FLOW_COLUMN);
    lv_obj_clear_flag(s_shade, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(s_shade, shade_gesture, LV_EVENT_GESTURE, nullptr);

    build_shade_content(s_shade, th);
}

// -------------------------------------------------------------- app host (solo-mode)
void close_app(void);
void exit_edit_mode(void);         // fwd: launcher edit mode (defined below)
void back_clicked(lv_event_t *) {
    if (s_app_back) s_app_back();  // in-app back (app-defined sub-page pop)
    else close_app();
}

// nv_gesture LEFT edge (strip enabled while an app OR the search overlay is up): swipe right ->
// back. An open shade suppresses the in-app strips (the shade owns the overlay). At home the strip
// is enabled only for the search overlay, so with no app open a right-swipe dismisses search.
void left_edge_cb(lv_dir_t dir, void *) {
    if (dir != LV_DIR_RIGHT || s_shade_open) return;
    if (s_app) { back_clicked(nullptr); return; }
    search_close_deferred();   // no-op if search isn't open (guarded inside)
}
// nv_gesture BOTTOM edge (always enabled): swipe up -> home when an app is open, or the
// recents/task-switcher overlay when already at home. Android-style single gesture.
void bottom_edge_cb(lv_dir_t dir, void *) {
    if (dir != LV_DIR_TOP || s_shade_open) return;
    if (search_is_open()) { search_close_deferred(); return; }  // swipe-up dismisses search too
    if (s_app) close_app();
    else       open_recents();
}

// App-open slide-in: animate a style translate_y offset (layer-free — NOT a transform/opacity
// draw layer, so it's safe on the P4 software renderer) from a small drop down to rest.
constexpr int32_t kAppSlide = 44;
void app_slide_cb(void *o, int32_t v) { lv_obj_set_style_translate_y((lv_obj_t *)o, v, 0); }

}  // namespace

// Fullscreen app mode (games): cover the whole panel. Hide the status bar, header and home pill and
// stretch the app plane + content to the full 1024x600 so a full-size game canvas isn't clipped by
// the chrome. Restore just re-shows the status bar; header/content/pill are rebuilt on the next
// open_app. Safe to call from an app's build() (header already exists; the pill checks s_fullscreen
// when it is created just after build). Public (extern "C" via nv_ui.h) — apps_app.cpp calls it.
void nv_ui_app_fullscreen(bool on) {
    s_fullscreen = on;
    if (on) {
        if (s_statusbar)    lv_obj_add_flag(s_statusbar, LV_OBJ_FLAG_HIDDEN);
        if (s_app)        { lv_obj_set_size(s_app, LV_HOR_RES, LV_VER_RES);
                            lv_obj_align(s_app, LV_ALIGN_TOP_MID, 0, 0); }
        if (s_app_hdr)      lv_obj_add_flag(s_app_hdr, LV_OBJ_FLAG_HIDDEN);
        if (s_app_pill)     lv_obj_add_flag(s_app_pill, LV_OBJ_FLAG_HIDDEN);
        if (s_app_content){ lv_obj_set_size(s_app_content, LV_HOR_RES, LV_VER_RES);
                            lv_obj_align(s_app_content, LV_ALIGN_TOP_MID, 0, 0); }
    } else {
        // Full restore so this is a safe runtime TOGGLE (the video player flips it on/off live),
        // not just a build-time one-shot: re-show the chrome and put the app plane + content back to
        // their windowed geometry (mirrors open_app's layout at lines above).
        if (s_statusbar)    lv_obj_clear_flag(s_statusbar, LV_OBJ_FLAG_HIDDEN);
        if (s_app_hdr)      lv_obj_clear_flag(s_app_hdr, LV_OBJ_FLAG_HIDDEN);
        if (s_app_pill)     lv_obj_clear_flag(s_app_pill, LV_OBJ_FLAG_HIDDEN);
        if (s_app)        { lv_obj_set_size(s_app, LV_HOR_RES, LV_VER_RES - kStatusH);
                            lv_obj_align(s_app, LV_ALIGN_TOP_MID, 0, kStatusH); }
        if (s_app_content){ lv_obj_set_size(s_app_content, LV_HOR_RES, LV_VER_RES - kStatusH - kHeaderH - kHomeInset);
                            lv_obj_align(s_app_content, LV_ALIGN_TOP_MID, 0, kHeaderH); }
    }
}

// Route Back to an app handler (a fullscreen game pops its own screen) instead of closing; nullptr
// restores default. nv_ui_close_app lets the app close itself. (close_app / s_app_back are visible
// here as anonymous-namespace members declared earlier in this file.)
void nv_ui_set_back_handler(void (*fn)(void)) { s_app_back = fn; }
void nv_ui_close_app(void) { close_app(); }

namespace {

void open_app(const NvApp *a) {
    exit_edit_mode();  // leave a clean launcher state if a tap raced edit mode (no-op otherwise)
    if (s_app || !a) return;
    s_fullscreen = false;   // reset; a game's build() re-enables it via nv_ui_app_fullscreen(true)
    NV_LOGI(TAG, "launch '%s' — broker requests %u KB", a->name, (unsigned)(a->ram_budget / 1024));
    // Broker gate: if the budget can't be met, undo the service suspend and refuse the launch
    // (never crash — the manifest ram_budget of a WASM app is untrusted).
    if (!nv_mem_request(a->ram_budget, nullptr, 0)) {
        nv_mem_release();
        nv_toast(NV_NOTE_WARN, "Not enough memory");
        return;
    }
    s_app_back = nullptr;
    s_app_cur = a;  // remember the open descriptor so a language change can re-render it live
    usage_bump(a);   // feed the smart-dock ranking (only successful launches count)
    recents_push(a); // move to front of the recency-ordered task switcher

    lv_obj_add_flag(s_launcher, LV_OBJ_FLAG_HIDDEN);

    s_app = lv_obj_create(lv_screen_active());
    lv_obj_remove_style_all(s_app);
    lv_obj_set_size(s_app, LV_HOR_RES, LV_VER_RES - kStatusH);
    lv_obj_align(s_app, LV_ALIGN_TOP_MID, 0, kStatusH);
    lv_obj_set_style_bg_color(s_app, nv_theme_get()->bg, 0);
    lv_obj_set_style_bg_opa(s_app, LV_OPA_COVER, 0);
    lv_obj_clear_flag(s_app, LV_OBJ_FLAG_SCROLLABLE);
    // MANDATORY containment (nv_gesture standard): no gesture bubbles past the app plane, so an
    // in-app swipe (scroll drift, text-field tap, content gesture) can NEVER trigger a system
    // action like go-home. System gestures live exclusively on the bezel edge strips.
    nv_gesture_isolate(s_app);

    const NvTheme *th = nv_theme_get();
    lv_obj_t *hdr = lv_obj_create(s_app);
    s_app_hdr = hdr;
    lv_obj_remove_style_all(hdr);
    lv_obj_set_size(hdr, LV_HOR_RES, kHeaderH);
    lv_obj_align(hdr, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_bg_color(hdr, th->header, 0);
    lv_obj_set_style_bg_opa(hdr, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_hor(hdr, kSp3, 0);            // 12: align with status bar edge
    lv_obj_clear_flag(hdr, LV_OBJ_FLAG_SCROLLABLE);
    // Elevation: a hairline bottom border separates the chrome from the canvas.
    // (No box-shadow: shadow blur forces an off-screen draw layer that stalls the ESP32-P4
    //  software renderer -> task-WDT. Border alone is layer-free.)
    lv_obj_set_style_border_side(hdr, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_width(hdr, 1, 0);
    lv_obj_set_style_border_color(hdr, th->surface2, 0);

    // No back button: navigation is gesture-based (Android-12 style) — LEFT-edge swipe = back,
    // BOTTOM-edge swipe-up = home (see the home pill below). Material top bar = a left-aligned
    // title only.
    s_app_title = lv_label_create(hdr);
    lv_label_set_text(s_app_title, app_label(a));
    lv_obj_set_style_text_font(s_app_title, &nv_font_20, 0);  // header title = hierarchy peak
    lv_obj_set_style_text_color(s_app_title, th->text_strong, 0);
    lv_obj_align(s_app_title, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_set_width(s_app_title, LV_HOR_RES - 2 * kSp4);
    lv_label_set_long_mode(s_app_title, LV_LABEL_LONG_MODE_DOTS);

    s_app_content = lv_obj_create(s_app);
    lv_obj_remove_style_all(s_app_content);
    // Reserve the bottom home-indicator band so app content never sits under the BOTTOM gesture strip.
    lv_obj_set_size(s_app_content, LV_HOR_RES, LV_VER_RES - kStatusH - kHeaderH - kHomeInset);
    lv_obj_align(s_app_content, LV_ALIGN_TOP_MID, 0, kHeaderH);
    lv_obj_clear_flag(s_app_content, LV_OBJ_FLAG_SCROLLABLE);

    if (a->build) {
        a->build(s_app_content);
    } else {
        lv_obj_t *c = nv_kit_scroll_column(s_app_content);
        lv_label_set_text_fmt(nv_kit_info(c), "%s\n\n%s", app_label(a),
                              nv_tr(NV_STR_APP_COMING_SOON));
    }

    // Home indicator ("pill") — a subtle rounded bar at the bottom center; the BOTTOM edge strip
    // owns the swipe-up-to-home gesture, so the pill itself is a visual affordance only.
    lv_obj_t *pill = lv_obj_create(s_app);
    s_app_pill = pill;
    lv_obj_remove_style_all(pill);
    lv_obj_set_size(pill, 132, 5);
    lv_obj_align(pill, LV_ALIGN_BOTTOM_MID, 0, -8);
    lv_obj_set_style_radius(pill, 3, 0);
    lv_obj_set_style_bg_color(pill, th->text_dim, 0);
    lv_obj_set_style_bg_opa(pill, LV_OPA_50, 0);
    lv_obj_clear_flag(pill, LV_OBJ_FLAG_CLICKABLE);
    if (s_fullscreen) lv_obj_add_flag(pill, LV_OBJ_FLAG_HIDDEN);   // game asked for full panel

    // System gestures in-app (centralized in nv_gesture): LEFT edge right-swipe -> back,
    // BOTTOM edge up-swipe -> home, TOP edge down-swipe -> shade. Enable the app-only strips and
    // re-raise all strips above the freshly-created app plane.
    nv_gesture_set_edge_enabled(NV_GESTURE_EDGE_LEFT, true);
    nv_gesture_set_edge_enabled(NV_GESTURE_EDGE_BOTTOM, true);
    nv_gesture_raise();

    // Slide-in: a translate_y offset animation (layer-free) gives a modern "push up" without the
    // whole-object opacity fade that would stall the P4 software renderer.
    lv_obj_set_style_translate_y(s_app, kAppSlide, 0);
    lv_anim_t sa;
    lv_anim_init(&sa);
    lv_anim_set_var(&sa, s_app);
    lv_anim_set_exec_cb(&sa, app_slide_cb);
    lv_anim_set_values(&sa, kAppSlide, 0);
    lv_anim_set_duration(&sa, 190);
    lv_anim_set_path_cb(&sa, lv_anim_path_ease_out);
    lv_anim_start(&sa);
}

void close_app(void) {
    if (!s_app) return;
    // If the app was left in fullscreen (game, or the video player closed mid-FS), restore the
    // status bar so the launcher we return to isn't left chrome-less. s_fullscreen is reset here
    // too (open_app also resets it, but a stale `true` would wrongly hide the next app's home pill).
    if (s_fullscreen) {
        if (s_statusbar) lv_obj_clear_flag(s_statusbar, LV_OBJ_FLAG_HIDDEN);
        s_fullscreen = false;
    }
    // Grab a Recents preview of the app's last screen BEFORE tearing it down (PPA downscale of
    // the live framebuffer). Best-effort + additive: a failure just leaves the card icon-only.
    if (s_app_cur && s_app_cur->id && nv_sd_is_mounted()) {
        mkdir("/sdcard/nucleos", 0777);
        mkdir("/sdcard/nucleos/recents", 0777);
        char tp[96];
        snprintf(tp, sizeof tp, "/sdcard/nucleos/recents/%s.bin", s_app_cur->id);
        nv_hal_thumbnail(tp, kThumbW, kThumbH);
    }
    nv_ime_hide();  // a bound field is about to be deleted; drop the IME binding first
    lv_obj_delete(s_app);  // apps free their own timers via LV_EVENT_DELETE on their content
    s_app = nullptr;
    s_app_title = nullptr;
    s_app_content = nullptr;
    s_app_hdr = nullptr;
    s_app_pill = nullptr;
    s_app_back = nullptr;
    s_app_cur = nullptr;
    nv_mem_release();
    nv_gesture_set_edge_enabled(NV_GESTURE_EDGE_LEFT, false);    // back strip is in-app only
    // BOTTOM strip stays enabled at home: there it opens Recents (see bottom_edge_cb).
    lv_obj_clear_flag(s_launcher, LV_OBJ_FLAG_HIDDEN);
    dock_refresh();   // the launch that just ended may have changed the usage ranking
    NV_LOGI(TAG, "app closed -> launcher");
}

// -------------------------------------------------------------- smart dock (usage-ranked)
// A floating favorites pill the OS curates ITSELF: apps are ranked by real launch counters
// (persisted per app id), so the five most-used apps are always one tap away on every page.
// No manual pinning to manage — the dock adapts to how the device is actually used.
lv_obj_t *s_dock = nullptr;   // pill object; child of s_launcher (does not slide with pages)

uint32_t usage_get(const NvApp *a) {
    char key[16];
    snprintf(key, sizeof key, kUsageKeyFmt, a->id);
    return (uint32_t)nv_config_get_int(key, 0);
}

void usage_bump(const NvApp *a) {
    char key[16];
    snprintf(key, sizeof key, kUsageKeyFmt, a->id);
    nv_config_set_int(key, nv_config_get_int(key, 0) + 1);
}

// Top-N registry indices by launch count (insertion sort into a tiny fixed array). Ties and
// never-launched apps fall back to registry order, so the dock is full from the first boot.
void dock_rank(int out[kDockN], int *out_n) {
    uint32_t best[kDockN];
    int n = 0;
    const int napp = nv_app_count();
    for (int i = 0; i < napp; i++) {
        const NvApp *ai = nv_app_at(i);
        if (ai->icon == &nv_icon_wasm) continue;   // WASM apps/games (Hello WASM, Nucleo Tanks) never
                                                   // auto-rank into the smart dock — they live in Apps
        const uint32_t c = usage_get(ai);
        int pos = n;
        while (pos > 0 && c > best[pos - 1]) pos--;
        if (pos >= kDockN) continue;
        const int last = (n < kDockN) ? n : kDockN - 1;
        for (int j = last; j > pos; j--) {
            best[j] = best[j - 1];
            out[j]  = out[j - 1];
        }
        best[pos] = c;
        out[pos]  = i;
        if (n < kDockN) n++;
    }
    *out_n = n;
}

void dock_launch_cb(lv_event_t *e) {
    open_app(nv_app_at((int)(intptr_t)lv_event_get_user_data(e)));
}

void build_dock(void) {
    int idx[kDockN];
    int n = 0;
    dock_rank(idx, &n);
    if (!n || !s_launcher) return;
    const NvTheme *th = nv_theme_get();

    s_dock = lv_obj_create(s_launcher);
    lv_obj_remove_style_all(s_dock);
    lv_obj_set_size(s_dock, LV_SIZE_CONTENT, kDockH);
    lv_obj_align(s_dock, LV_ALIGN_BOTTOM_MID, 0, -kDockMarg);
    lv_obj_set_style_radius(s_dock, kDockH / 2, 0);
    lv_obj_set_style_bg_color(s_dock, th->surface, 0);
    lv_obj_set_style_bg_opa(s_dock, LV_OPA_80, 0);
    lv_obj_set_style_border_width(s_dock, 1, 0);
    lv_obj_set_style_border_color(s_dock, th->surface3, 0);
    lv_obj_set_style_pad_hor(s_dock, kSp4, 0);
    lv_obj_set_style_pad_column(s_dock, kSp3, 0);
    lv_obj_set_flex_flow(s_dock, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(s_dock, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(s_dock, LV_OBJ_FLAG_SCROLLABLE);

    for (int i = 0; i < n; i++) {
        const NvApp *a = nv_app_at(idx[i]);
        if (!a) continue;
        lv_obj_t *cell = lv_obj_create(s_dock);   // tap cell clips the 80px icon box to 56px
        lv_obj_remove_style_all(cell);
        lv_obj_set_size(cell, kDockIcon, kDockIcon);
        lv_obj_set_style_radius(cell, kRadSm, 0);
        lv_obj_set_style_bg_opa(cell, LV_OPA_TRANSP, 0);
        lv_obj_set_style_bg_color(cell, th->text_strong, LV_STATE_PRESSED);
        lv_obj_set_style_bg_opa(cell, LV_OPA_10, LV_STATE_PRESSED);
        lv_obj_add_flag(cell, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_remove_flag(cell, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_event_cb(cell, dock_launch_cb, LV_EVENT_CLICKED, (void *)(intptr_t)idx[i]);

        lv_obj_t *img = lv_image_create(cell);
        lv_image_set_src(img, a->icon);
        lv_image_set_scale(img, 256 * kDockIcon / 80);   // 80px icon -> 56px (no draw layer)
        lv_obj_center(img);
        lv_obj_remove_flag(img, LV_OBJ_FLAG_CLICKABLE);
    }
}

// Cheap re-rank: the pill is a handful of objects, so delete + rebuild beats bookkeeping.
void dock_refresh(void) {
    if (s_dock) {
        lv_obj_delete(s_dock);
        s_dock = nullptr;
    }
    build_dock();
}

// -------------------------------------------------------------- recents (task switcher)
// A recency-ordered overlay (most-recent first) opened by the bottom-edge swipe at home.
// Complements the smart dock (which ranks by frequency): recents answers "what was I just in".
int s_recents[kRecentsN];
int s_recents_n = 0;
lv_obj_t *s_recents_ov = nullptr;   // full-screen overlay while open; nullptr when closed
uint8_t *s_thumb_buf[kRecentsN] = {};  // per-card RGB565 preview buffers (freed on close)

int app_index(const NvApp *a) {
    const int n = nv_app_count();
    for (int i = 0; i < n; i++) if (nv_app_at(i) == a) return i;
    return -1;
}

void recents_push(const NvApp *a) {
    const int idx = app_index(a);
    if (idx < 0) return;
    int tmp[kRecentsN];
    int w = 0;
    tmp[w++] = idx;                                    // new front
    for (int i = 0; i < s_recents_n && w < kRecentsN; i++)
        if (s_recents[i] != idx) tmp[w++] = s_recents[i];  // keep the rest, deduped
    s_recents_n = w;
    for (int i = 0; i < w; i++) s_recents[i] = tmp[i];
}

void recents_close(void) {
    if (!s_recents_ov) return;
    lv_obj_delete(s_recents_ov);   // deletes the canvases first, so no draw references the buffers
    s_recents_ov = nullptr;
    for (int i = 0; i < kRecentsN; i++)
        if (s_thumb_buf[i]) { heap_caps_free(s_thumb_buf[i]); s_thumb_buf[i] = nullptr; }
    nv_gesture_raise();   // keep the edge strips above whatever is now top-most
}
void recents_scrim_cb(lv_event_t *) { recents_close(); }
void recents_card_cb(lv_event_t *e) {
    const int idx = (int)(intptr_t)lv_event_get_user_data(e);
    recents_close();
    open_app(nv_app_at(idx));
}

void open_recents(void) {
    if (s_recents_ov || s_app || s_shade_open) return;   // only from a clean home
    const NvTheme *th = nv_theme_get();

    s_recents_ov = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(s_recents_ov);
    lv_obj_set_size(s_recents_ov, LV_HOR_RES, LV_VER_RES);
    lv_obj_set_style_bg_color(s_recents_ov, th->scrim, 0);
    lv_obj_set_style_bg_opa(s_recents_ov, LV_OPA_COVER, 0);
    lv_obj_add_flag(s_recents_ov, LV_OBJ_FLAG_CLICKABLE);   // tap the backdrop to dismiss
    lv_obj_remove_flag(s_recents_ov, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(s_recents_ov, recents_scrim_cb, LV_EVENT_CLICKED, nullptr);

    lv_obj_t *title = lv_label_create(s_recents_ov);
    lv_label_set_text(title, nv_tr(NV_STR_RECENTS));
    lv_obj_set_style_text_font(title, &nv_font_20, 0);
    lv_obj_set_style_text_color(title, th->text_strong, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, kStatusH + kSp4);

    if (s_recents_n == 0) {
        lv_obj_t *empty = lv_label_create(s_recents_ov);
        lv_label_set_text(empty, nv_tr(NV_STR_NO_RECENTS));
        lv_obj_set_style_text_color(empty, th->text_dim, 0);
        lv_obj_center(empty);
        return;
    }

    // Horizontal, center-snapped strip of app cards (most-recent first).
    lv_obj_t *row = lv_obj_create(s_recents_ov);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, lv_pct(100), 240);
    lv_obj_center(row);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, kSp4, 0);
    lv_obj_set_style_pad_hor(row, kHomeInset, 0);
    lv_obj_set_scroll_dir(row, LV_DIR_HOR);
    lv_obj_set_scroll_snap_x(row, LV_SCROLL_SNAP_CENTER);

    for (int i = 0; i < s_recents_n; i++) {
        const NvApp *a = nv_app_at(s_recents[i]);
        if (!a) continue;
        lv_obj_t *card = lv_obj_create(row);
        lv_obj_remove_style_all(card);
        lv_obj_set_size(card, 200, 220);
        lv_obj_set_style_radius(card, kRadMd, 0);
        lv_obj_set_style_bg_color(card, th->surface, 0);
        lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(card, 1, 0);
        lv_obj_set_style_border_color(card, th->surface3, 0);
        lv_obj_set_style_pad_all(card, kSp4, 0);
        lv_obj_set_style_pad_row(card, kSp3, 0);
        lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(card, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_event_cb(card, recents_card_cb, LV_EVENT_CLICKED, (void *)(intptr_t)s_recents[i]);

        // Preview: the app's last-screen thumbnail if one was captured on exit, else its icon.
        uint8_t *tb = nullptr;
        char tp[96];
        snprintf(tp, sizeof tp, "/sdcard/nucleos/recents/%s.bin", a->id);
        FILE *tf = fopen(tp, "rb");
        if (tf) {
            const size_t len = (size_t)kThumbW * kThumbH * 2;
            tb = (uint8_t *)heap_caps_malloc(len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            if (tb && fread(tb, 1, len, tf) != len) { heap_caps_free(tb); tb = nullptr; }
            fclose(tf);
        }
        if (tb) {
            s_thumb_buf[i] = tb;   // freed in recents_close (after the canvas is deleted)
            // NOTE: no radius/clip_corner here — clip_corner hangs the P4 software renderer
            // (see the draw-layers WDT lesson). Square preview is fine.
            lv_obj_t *cv = lv_canvas_create(card);
            lv_canvas_set_buffer(cv, tb, kThumbW, kThumbH, LV_COLOR_FORMAT_RGB565);
            lv_obj_remove_flag(cv, LV_OBJ_FLAG_CLICKABLE);
        } else {
            lv_obj_t *img = lv_image_create(card);
            lv_image_set_src(img, a->icon);
            lv_obj_remove_flag(img, LV_OBJ_FLAG_CLICKABLE);
        }

        lv_obj_t *nm = lv_label_create(card);
        lv_label_set_text(nm, app_label(a));
        lv_obj_set_style_text_color(nm, th->text_strong, 0);
    }
}

// -------------------------------------------------------------- launcher grid math
// Recompute the grid for the CURRENT orientation. Verified fits (dock + dots band below):
//   landscape 1024x566: padL 24, 6 cols span 976; rows end at 442; dots ~478; dock 488..552
//   portrait   600x990: padL 52, 3 cols span 496; rows end at 886; dots ~902; dock 912..976
void grid_compute(void) {
    const int W = LV_HOR_RES;
    const bool portrait = LV_VER_RES > W;
    s_g.cols  = portrait ? 3 : 6;
    s_g.rows  = portrait ? 6 : 3;
    s_g.cellW = portrait ? 180 : 168;
    s_g.cellH = portrait ? 140 : 128;
    s_g.padT  = 64;   // leaves room for the search pill (44px @ y10)
    s_g.padL  = (W - ((s_g.cols - 1) * s_g.cellW + kTileW)) / 2;
    s_g.cap   = s_g.cols * s_g.rows;
}

// slot -> tile top-left in STRIP coords (each page occupies one screen-width of the strip).
void slot_xy(int slot, int *x, int *y) {
    const int page  = slot / s_g.cap;
    const int local = slot % s_g.cap;
    *x = page * LV_HOR_RES + s_g.padL + (local % s_g.cols) * s_g.cellW;
    *y = s_g.padT + (local / s_g.cols) * s_g.cellH;
}

int nearest_slot(int px, int py) {
    // px,py = dragged tile's current top-left in STRIP coords.
    // Clamp numerators non-negative first so integer division rounds half-up cleanly
    // (truncation-toward-zero would bias when dragged left of / above the origin).
    if (px < 0) px = 0;
    int page = px / LV_HOR_RES;
    if (page > s_pages - 1) page = s_pages - 1;
    int nx = px - page * LV_HOR_RES - s_g.padL; if (nx < 0) nx = 0;
    int ny = py - s_g.padT;                     if (ny < 0) ny = 0;
    int col = (nx + s_g.cellW / 2) / s_g.cellW;
    if (col > s_g.cols - 1) col = s_g.cols - 1;
    int row = (ny + s_g.cellH / 2) / s_g.cellH;
    if (row > s_g.rows - 1) row = s_g.rows - 1;
    int t = page * s_g.cap + row * s_g.cols + col;
    if (t >= s_tile_n) t = s_tile_n - 1;
    return t;
}

// -------------------------------------------------------------- launcher order persistence
// Schema v2. Folders load first (members validated against the registry, no duplicates),
// then the entry list; invalid records are dropped and every unreferenced app is appended,
// so registry growth (an OTA adds an app) or a corrupt record can never lose an icon.
void order_load(void) {
    const int napp = nv_app_count();
    bool used[kMaxApps] = {false};

    // ---- folders (id == array index; n == 0 marks a free record) ----
    for (int f = 0; f < kMaxFolders; f++) {
        Folder *fo = &s_folders[f];
        fo->n = 0;
        fo->name[0] = '\0';
        char key[16];
        snprintf(key, sizeof key, kFolderNFmt, f);
        int n = nv_config_get_int(key, 0);
        if (n < 2) continue;                     // a real folder has >= 2 members
        if (n > kFolderCap) n = kFolderCap;
        for (int m = 0; m < n; m++) {
            snprintf(key, sizeof key, kFolderMemFmt, f, m);
            const int v = nv_config_get_int(key, -1);
            if (v >= 0 && v < napp && !used[v]) { fo->mem[fo->n++] = v; used[v] = true; }
        }
        if (fo->n < 2) {                         // decayed below a real folder: dissolve
            for (int m = 0; m < fo->n; m++) used[fo->mem[m]] = false;
            fo->n = 0;
            continue;
        }
        snprintf(key, sizeof key, kFolderNameFmt, f);
        nv_config_get_str(key, nv_tr(NV_STR_FOLDER), fo->name, sizeof fo->name);
    }

    // ---- entries ----
    int n = 0;
    bool fseen[kMaxFolders] = {false};
    const int persisted = nv_config_get_int(kOrdCountKey, -1);
    if (persisted >= 0) {
        const int lim = persisted > kMaxEntries ? kMaxEntries : persisted;
        for (int i = 0; i < lim; i++) {
            char key[16];
            snprintf(key, sizeof key, kOrdEntryFmt, i);
            const int v = nv_config_get_int(key, -1);
            if (v >= kEntFolder) {
                const int f = v - kEntFolder;
                if (f < kMaxFolders && s_folders[f].n >= 2 && !fseen[f]) {
                    fseen[f] = true;
                    s_order[n++] = v;
                }
            } else if (v >= 0 && v < napp && !used[v]) {
                used[v] = true;
                s_order[n++] = v;
            }
        }
    } else {
        // LEGACY v1 migration: "lord%d" was a plain app permutation (no folders).
        for (int i = 0; i < napp && i < kMaxEntries; i++) {
            char key[8];
            snprintf(key, sizeof key, kOrderKeyFmt, i);
            const int v = nv_config_get_int(key, -1);
            if (v >= 0 && v < napp && !used[v]) { used[v] = true; s_order[n++] = v; }
        }
    }
    // Orphans go at the end: folders with valid members but no slot, then unreferenced apps.
    for (int f = 0; f < kMaxFolders; f++)
        if (s_folders[f].n >= 2 && !fseen[f] && n < kMaxEntries) s_order[n++] = kEntFolder + f;
    for (int a = 0; a < napp && n < kMaxEntries; a++)
        if (!used[a]) { used[a] = true; s_order[n++] = a; }

    s_tile_n = n;
    s_pages = (n + s_g.cap - 1) / s_g.cap;
    if (s_pages < 1) s_pages = 1;
    if (s_pages > kMaxPages) s_pages = kMaxPages;   // defensive; kMaxEntries fits kMaxPages
    s_page = nv_config_get_int("lpage", s_page);    // resume on the last-viewed page
    if (s_page > s_pages - 1) s_page = s_pages - 1;
    if (s_page < 0) s_page = 0;
}

void order_save(void) {
    char key[16];
    nv_config_set_int(kOrdCountKey, s_tile_n);
    for (int i = 0; i < s_tile_n; i++) {
        snprintf(key, sizeof key, kOrdEntryFmt, i);
        nv_config_set_int(key, s_order[i]);
    }
    for (int f = 0; f < kMaxFolders; f++) {
        snprintf(key, sizeof key, kFolderNFmt, f);
        nv_config_set_int(key, s_folders[f].n);
        for (int m = 0; m < s_folders[f].n; m++) {
            snprintf(key, sizeof key, kFolderMemFmt, f, m);
            nv_config_set_int(key, s_folders[f].mem[m]);
        }
        if (s_folders[f].n) {
            snprintf(key, sizeof key, kFolderNameFmt, f);
            nv_config_set_str(key, s_folders[f].name);
        }
    }
}

// -------------------------------------------------------------- tile positioning / relayout
void anim_set_x(void *var, int32_t v) { lv_obj_set_x((lv_obj_t *)var, v); }
void anim_set_y(void *var, int32_t v) { lv_obj_set_y((lv_obj_t *)var, v); }

void tile_apply_pos(int slot, bool animate) {
    lv_obj_t *tile = s_tiles[slot];
    if (!tile) return;
    lv_obj_set_user_data(tile, (void *)(intptr_t)slot);  // slot is authoritative; keep it current
    int x, y;
    slot_xy(slot, &x, &y);
    if (!animate) {
        lv_obj_set_pos(tile, x, y);
        return;
    }
    // Already resting at the target slot: skip the anim. commit_reorder() calls relayout_all()
    // on every slot boundary the finger crosses; without this guard all 18 tiles fire an x+y
    // anim each pass (36 anims), even the ones that never move — an anim storm the P4 software
    // renderer chokes on.
    if (lv_obj_get_x_aligned(tile) == x && lv_obj_get_y_aligned(tile) == y) return;

    lv_anim_t ax;
    lv_anim_init(&ax);
    lv_anim_set_var(&ax, tile);
    lv_anim_set_exec_cb(&ax, anim_set_x);
    lv_anim_set_values(&ax, lv_obj_get_x_aligned(tile), x);
    lv_anim_set_duration(&ax, kSnapDurMs);
    lv_anim_set_path_cb(&ax, lv_anim_path_ease_out);
    lv_anim_start(&ax);

    lv_anim_t ay;
    lv_anim_init(&ay);
    lv_anim_set_var(&ay, tile);
    lv_anim_set_exec_cb(&ay, anim_set_y);
    lv_anim_set_values(&ay, lv_obj_get_y_aligned(tile), y);
    lv_anim_set_duration(&ay, kSnapDurMs);
    lv_anim_set_path_cb(&ay, lv_anim_path_ease_out);
    lv_anim_start(&ay);
}

void relayout_all(bool animate) {
    for (int slot = 0; slot < s_tile_n; slot++) {
        // The actively-dragged tile follows the finger; don't animate it out from under the drag.
        // Its user_data/slot is still refreshed so it lands on the right slot after release/exit.
        if (animate && slot == s_drag_slot) {
            lv_obj_set_user_data(s_tiles[slot], (void *)(intptr_t)slot);
            continue;
        }
        tile_apply_pos(slot, animate);
    }
}

// -------------------------------------------------------------- pages (slide + dots)
void dots_update(void) {
    if (!s_dots) return;
    const NvTheme *th = nv_theme_get();
    const int n = (int)lv_obj_get_child_count(s_dots);
    for (int i = 0; i < n; i++) {
        lv_obj_t *d = lv_obj_get_child(s_dots, i);
        lv_obj_set_style_bg_color(d, i == s_page ? th->accent : th->text_dim, 0);
        lv_obj_set_style_bg_opa(d, i == s_page ? LV_OPA_COVER : LV_OPA_40, 0);
    }
}

// Slide the strip to a page (x-position animation — layer-free, same as tile snapping).
void strip_goto(int page, bool animate) {
    if (!s_strip) return;
    if (page < 0) page = 0;
    if (page > s_pages - 1) page = s_pages - 1;
    if (page != s_page) {
        nv_config_set_int("lpage", page);   // resume here next boot
        if (animate) nv_audio_click();      // page-turn tick (honors the key-click pref)
    }
    s_page = page;
    const int target = -page * LV_HOR_RES;
    lv_anim_delete(s_strip, anim_set_x);
    if (!animate) {
        lv_obj_set_x(s_strip, target);
    } else {
        lv_anim_t a;
        lv_anim_init(&a);
        lv_anim_set_var(&a, s_strip);
        lv_anim_set_exec_cb(&a, anim_set_x);
        lv_anim_set_values(&a, lv_obj_get_x_aligned(s_strip), target);
        lv_anim_set_duration(&a, kPageAnimMs);
        lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
        lv_anim_start(&a);
    }
    dots_update();
}

void dot_clicked(lv_event_t *e) {
    strip_goto((int)(intptr_t)lv_event_get_user_data(e), true);
}

// Highlight the armed folder-merge target (accent border only — layer-free).
void merge_mark(int slot, bool on) {
    if (slot < 0 || slot >= kMaxEntries || !s_tiles[slot]) return;
    lv_obj_set_style_border_width(s_tiles[slot], on ? 2 : 0, 0);
    lv_obj_set_style_border_color(s_tiles[slot], nv_theme_get()->accent, 0);
}

// -------------------------------------------------------------- drag plate (rearrange cue)
// While a drag/merge is in progress every tile shows an opaque plate behind its icon. Two jobs:
// it's the "you can rearrange now" cue (replacing the old jiggle wobble the P4 renderer couldn't
// keep up with), and it turns the per-frame wallpaper recomposite under each moving tile into a
// cheap solid fill — the SW renderer can't afford the PSRAM wallpaper reblit on the visible tiles
// at 30 fps. Shown only during an active drag; cleared the instant it ends. Icons at rest sit
// transparent over the wallpaper, no box.
void plate_all(bool on) {
    const NvTheme *th = nv_theme_get();
    for (int slot = 0; slot < s_tile_n; slot++) {
        lv_obj_t *t = s_tiles[slot];
        if (!t) continue;
        if (on) {
            lv_obj_set_style_bg_color(t, th->surface, 0);
            lv_obj_set_style_bg_opa(t, LV_OPA_COVER, 0);
            // The dragged tile is held down (LV_STATE_PRESSED); override the faint press
            // highlight so its box stays solid like the rest.
            lv_obj_set_style_bg_color(t, th->surface, LV_STATE_PRESSED);
            lv_obj_set_style_bg_opa(t, LV_OPA_COVER, LV_STATE_PRESSED);
        } else {
            lv_obj_set_style_bg_opa(t, LV_OPA_TRANSP, 0);       // transparent over the wallpaper
            lv_obj_set_style_bg_color(t, th->text_strong, LV_STATE_PRESSED);  // restore the
            lv_obj_set_style_bg_opa(t, LV_OPA_10, LV_STATE_PRESSED);          // normal tap highlight
        }
    }
}

// -------------------------------------------------------------- edit-mode enter / exit
// Edit mode itself is now invisible (no wobble, no box): the box appears per-drag via plate_all()
// and clears on release, so tiles at rest look identical to normal home.
void enter_edit_mode(void) {
    if (s_launcher_edit) return;
    s_launcher_edit = true;
    NV_LOGI(TAG, "launcher edit mode ON");
}

void exit_edit_mode(void) {
    if (!s_launcher_edit) return;
    s_launcher_edit = false;
    s_drag_slot = -1;
    merge_mark(s_merge_slot, false);
    s_merge_slot = -1;
    s_edge_since = 0;
    s_reorder_pending = -1;
    s_reorder_since   = 0;
    plate_all(false);  // clear any lingering drag plate + restore the normal tap highlight
    for (int slot = 0; slot < s_tile_n; slot++) {
        lv_obj_t *tile = s_tiles[slot];
        if (!tile) continue;
        lv_anim_delete(tile, anim_set_x);  // kill any in-flight snap so the set_pos below wins
        lv_anim_delete(tile, anim_set_y);
    }
    relayout_all(false);  // snap any half-dragged tile back to its exact slot, no anim
    order_save();
    NV_LOGI(TAG, "launcher edit mode OFF (order saved)");
}

// -------------------------------------------------------------- reorder (drag-insert)
void commit_reorder(int from, int to) {
    if (from == to) {
        relayout_all(true);  // settle
        return;
    }
    int app = s_order[from];
    lv_obj_t *tile = s_tiles[from];
    if (to > from) {
        for (int i = from; i < to; i++) {
            s_order[i] = s_order[i + 1];
            s_tiles[i] = s_tiles[i + 1];
        }
    } else {  // to < from
        for (int i = from; i > to; i--) {
            s_order[i] = s_order[i - 1];
            s_tiles[i] = s_tiles[i - 1];
        }
    }
    s_order[to] = app;
    s_tiles[to] = tile;
    relayout_all(true);  // animate every tile (incl. shifted neighbors) to its new slot
}

// -------------------------------------------------------------- folder merge (drag app onto tile)
int s_pending_merge_from = -1, s_pending_merge_to = -1;

// A drop on slot t merges INTO it iff the dragged tile's center sits close to t's center
// (otherwise it's a plain reorder) and the model allows it: only apps can be dropped; the
// target may be an app (new folder — needs a free record) or a folder with room left.
int merge_target(int from, lv_obj_t *tile) {
    if (entry_is_folder(s_order[from])) return -1;          // folders don't nest
    const int cx = lv_obj_get_x_aligned(tile) + kTileW / 2;
    const int cy = lv_obj_get_y_aligned(tile) + kTileH / 2;
    const int t = nearest_slot(lv_obj_get_x_aligned(tile), lv_obj_get_y_aligned(tile));
    if (t == from || t < 0 || t >= s_tile_n) return -1;
    int tx, ty;
    slot_xy(t, &tx, &ty);
    if (LV_ABS(cx - (tx + kTileW / 2)) > kTileW / 2) return -1;   // generous centre zone: hovering
    if (LV_ABS(cy - (ty + kTileH / 2)) > kTileH / 2) return -1;   // a tile leans toward merge

    const Folder *fo = entry_folder(s_order[t]);
    if (fo) return fo->n < kFolderCap ? t : -1;
    for (int f = 0; f < kMaxFolders; f++)
        if (!s_folders[f].n) return t;                      // a free record exists
    return -1;
}

// Deferred via lv_async_call: mutates the model then rebuilds the launcher. The drop event
// fired on a tile this rebuild deletes, so doing it inline would free the object mid-event.
void merge_apply_async(void *) {
    const int from = s_pending_merge_from, to = s_pending_merge_to;
    s_pending_merge_from = s_pending_merge_to = -1;
    if (from < 0 || to < 0 || from >= s_tile_n || to >= s_tile_n || from == to) return;
    const int app = s_order[from];
    if (entry_is_folder(app)) return;
    Folder *fo = entry_folder(s_order[to]);
    if (!fo) {                                   // app onto app: allocate a folder record
        int f = 0;
        while (f < kMaxFolders && s_folders[f].n) f++;
        if (f == kMaxFolders) return;
        fo = &s_folders[f];
        lv_snprintf(fo->name, sizeof fo->name, "%s", nv_tr(NV_STR_FOLDER));
        fo->mem[0] = s_order[to];
        fo->n = 1;
        s_order[to] = kEntFolder + f;
    }
    if (fo->n >= kFolderCap) return;             // guard (merge_target already vetoed this)
    fo->mem[fo->n++] = app;
    for (int i = from; i < s_tile_n - 1; i++) s_order[i] = s_order[i + 1];  // close the gap
    s_tile_n--;
    order_save();
    rebuild_launcher();                          // exits edit mode; pages recount
    NV_LOGI(TAG, "folder merge: app %d -> '%s' (%d members)", app, fo->name, fo->n);
}

// -------------------------------------------------------------- tile events (launch/drag/reorder)
void tile_event_cb(lv_event_t *e) {
    lv_obj_t *tile = lv_event_get_target_obj(e);
    int slot = (int)(intptr_t)lv_obj_get_user_data(tile);

    switch (lv_event_get_code(e)) {
        case LV_EVENT_SHORT_CLICKED: {
            if (s_launcher_edit) return;  // edit mode: tiles only drag, never launch
            const int ent = s_order[slot];
            if (entry_is_folder(ent)) folder_open(ent - kEntFolder);
            else                      open_app(nv_app_at(ent));
            break;
        }

        case LV_EVENT_LONG_PRESSED: {
            if (!s_launcher_edit) enter_edit_mode();
            s_drag_slot = slot;                     // this tile is now the drag subject
            plate_all(true);                        // drag started: opaque box behind every icon
            s_edge_since = 0;
            s_reorder_pending = -1;
            s_reorder_since   = 0;
            lv_indev_t *ind = lv_indev_active();
            if (ind) lv_indev_reset_long_press(ind);
            break;
        }

        case LV_EVENT_PRESSING: {
            if (!s_launcher_edit || s_drag_slot != slot) return;
            lv_indev_t *ind = lv_indev_active();
            if (!ind) return;
            lv_point_t v;
            lv_indev_get_vect(ind, &v);
            if (v.x || v.y)   // PRESSING keeps firing while the finger is still — needed below
                lv_obj_set_pos(tile, lv_obj_get_x_aligned(tile) + v.x,
                                     lv_obj_get_y_aligned(tile) + v.y);

            // Drag-to-edge page flip: hold the finger inside a screen-edge zone to slide to
            // the neighbor page. The tile lives in strip coords, so after the flip it shifts
            // one screen-width to stay under the finger.
            lv_point_t p;
            lv_indev_get_point(ind, &p);
            const int dir = (p.x < kEdgeFlipPx) ? -1
                          : (p.x > LV_HOR_RES - kEdgeFlipPx) ? 1 : 0;
            if (dir && (dir > 0 ? s_page < s_pages - 1 : s_page > 0)) {
                if (!s_edge_since) {
                    s_edge_since = lv_tick_get();
                } else if (lv_tick_elaps(s_edge_since) > kEdgeFlipMs) {
                    s_edge_since = 0;
                    strip_goto(s_page + dir, true);
                    lv_obj_set_x(tile, lv_obj_get_x_aligned(tile) + dir * LV_HOR_RES);
                }
            } else {
                s_edge_since = 0;
            }

            // Merge beats reorder. Hovering a tile's centre arms a folder-merge instantly and
            // suppresses reorder; a plain reorder only fires once the finger has DWELLED over a
            // new slot (kReorderDwellMs). Without the dwell the target darts away to fill the
            // vacated slot the moment you approach it, so folders were nearly impossible to form.
            const int mt = merge_target(slot, tile);
            if (mt != s_merge_slot) {
                merge_mark(s_merge_slot, false);
                s_merge_slot = mt;
                merge_mark(mt, true);
            }
            if (mt >= 0) {                       // over a centre: merge armed, cancel pending reorder
                s_reorder_pending = -1;
                s_reorder_since   = 0;
                break;
            }
            const int t = nearest_slot(lv_obj_get_x_aligned(tile), lv_obj_get_y_aligned(tile));
            if (t == slot) {                     // back over its own slot: nothing pending
                s_reorder_pending = -1;
                s_reorder_since   = 0;
                break;
            }
            if (s_reorder_pending != t) {        // newly hovering slot t: start the dwell timer
                s_reorder_pending = t;
                s_reorder_since   = lv_tick_get();
                break;
            }
            if (lv_tick_elaps(s_reorder_since) >= kReorderDwellMs) {
                s_reorder_pending = -1;
                s_reorder_since   = 0;
                // Mark the tile's NEW slot first so relayout skips animating it (finger owns it),
                // then permute; shifted neighbours animate, the drag keeps following.
                s_drag_slot = t;
                commit_reorder(slot, t);
            }
            break;
        }

        case LV_EVENT_RELEASED:
        case LV_EVENT_PRESS_LOST: {  // treat a lost press like a release: settle into a slot
            if (s_drag_slot != slot) return;
            s_edge_since = 0;
            if (s_merge_slot >= 0) {                 // drop-on-tile: merge into a folder
                const int mt = s_merge_slot;
                merge_mark(mt, false);
                s_merge_slot = -1;
                s_drag_slot = -1;
                s_pending_merge_from = slot;
                s_pending_merge_to = mt;
                if (lv_async_call(merge_apply_async, nullptr) != LV_RESULT_OK) {
                    s_pending_merge_from = s_pending_merge_to = -1;
                    commit_reorder(slot, slot);      // settle in place instead
                    plate_all(false);                // drag aborted — clear the plates
                }
                break;
            }
            int t = nearest_slot(lv_obj_get_x_aligned(tile), lv_obj_get_y_aligned(tile));
            s_drag_slot = -1;         // clear first so the dropped tile animates into its slot
            commit_reorder(slot, t);  // t==slot after live-reorder -> just settles into place
            plate_all(false);         // drag over — icons back to transparent over the wallpaper
            break;  // stay in edit mode; exit via empty-tap / swipe-up / back
        }

        default: break;
    }
}

// -------------------------------------------------------------- launcher empty-space tap
void launcher_bg_clicked(lv_event_t *e) {
    lv_obj_t *t = lv_event_get_target_obj(e);
    if (s_launcher_edit && (t == s_launcher || t == s_strip)) exit_edit_mode();
}

// (go_home removed: swipe-up-to-home is gone by design — a bottom-up swipe must never close
//  an app. Home is reached via Back; edit-mode exit is launcher-local. See nv_gesture.h.)

// -------------------------------------------------------------- gestures (launcher-local)
// Home-screen only: any decisive swipe on the launcher exits edit mode. Attached to the
// launcher itself (which does NOT bubble further), never to the screen — per the nv_gesture
// standard there is no blanket screen-level gesture handler anymore, so nothing an app or
// widget does can reach one.
void launcher_gesture(lv_event_t *) {
    if (s_drag_slot >= 0) return;   // a tile is being dragged: RELEASED owns the outcome
    if (s_launcher_edit) { exit_edit_mode(); return; }
    // Horizontal swipe = page navigation; swipe DOWN anywhere on the launcher = search
    // (the TOP edge strip still owns the shade — a shade swipe must START on the bezel).
    lv_indev_t *ind = lv_indev_active();
    if (!ind) return;
    const lv_dir_t d = lv_indev_get_gesture_dir(ind);
    if (d == LV_DIR_LEFT)        strip_goto(s_page + 1, true);
    else if (d == LV_DIR_RIGHT)  strip_goto(s_page - 1, true);
    else if (d == LV_DIR_BOTTOM) search_open(nullptr);
}

// -------------------------------------------------------------- build
void build_status_bar(lv_obj_t *scr) {
    lv_obj_t *bar = lv_obj_create(scr);
    s_statusbar = bar;
    lv_obj_remove_style_all(bar);
    lv_obj_set_size(bar, LV_HOR_RES, kStatusH);
    lv_obj_align(bar, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_bg_color(bar, nv_theme_get()->surface, 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_hor(bar, kSp3, 0);   // 12
    lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(bar, LV_OBJ_FLAG_CLICKABLE);  // receive the swipe-down gesture
    lv_obj_add_event_cb(bar, status_gesture, LV_EVENT_GESTURE, nullptr);

    const NvTheme *th = nv_theme_get();

    // Left cluster: time (focal) + localized date.
    lv_obj_t *left = lv_obj_create(bar);
    lv_obj_remove_style_all(left);
    lv_obj_set_size(left, LV_SIZE_CONTENT, lv_pct(100));
    lv_obj_align(left, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_set_flex_flow(left, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(left, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(left, kSp2, 0);   // 8
    lv_obj_clear_flag(left, LV_OBJ_FLAG_SCROLLABLE);

    s_clock = lv_label_create(left);
    lv_obj_set_style_text_color(s_clock, th->text, 0);

    s_date = lv_label_create(left);
    lv_obj_set_style_text_color(s_date, th->text_dim, 0);

    // Right cluster: SSID · Wi-Fi · SD · notification bell (no heap HUD, no fake battery — this
    // board has no fuel-gauge IC, so a battery glyph would be a lie).
    lv_obj_t *right = lv_obj_create(bar);
    lv_obj_remove_style_all(right);
    lv_obj_set_size(right, LV_SIZE_CONTENT, lv_pct(100));
    lv_obj_align(right, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_set_flex_flow(right, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(right, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(right, kSp3, 0);   // 12: airier than the old debug cluster
    lv_obj_clear_flag(right, LV_OBJ_FLAG_SCROLLABLE);

    s_wifi_ssid = lv_label_create(right);   // sits just left of the glyph; filled by status_tick
    lv_label_set_text(s_wifi_ssid, "");
    lv_obj_set_style_text_color(s_wifi_ssid, th->text_dim, 0);
    lv_obj_add_flag(s_wifi_ssid, LV_OBJ_FLAG_HIDDEN);

    s_wifi_ico = lv_label_create(right);
    lv_label_set_text(s_wifi_ico, LV_SYMBOL_WIFI);
    lv_obj_set_style_text_color(s_wifi_ico, th->text_dim, 0);

    s_sd_ico = lv_label_create(right);
    lv_label_set_text(s_sd_ico, LV_SYMBOL_SD_CARD);
    lv_obj_set_style_text_color(s_sd_ico, th->text_dim, 0);
    if (!nv_sd_is_mounted()) lv_obj_add_flag(s_sd_ico, LV_OBJ_FLAG_HIDDEN);

    s_bell = lv_label_create(right);        // unread-notification badge, rightmost; accent tint
    lv_obj_set_style_text_color(s_bell, th->accent, 0);
    lv_obj_add_flag(s_bell, LV_OBJ_FLAG_HIDDEN);
    update_bell();                          // restore state across theme/lang rebuilds

    lv_timer_create(status_tick, 1000, nullptr);
    status_tick(nullptr);
}

// -------------------------------------------------------------- launcher search
// A full-screen search overlay (parented on the screen so the IME can dock over it, like the
// Wi-Fi password sheet). Live-filters the app registry by name; a result tap launches through the
// normal open_app path. The overlay delete is deferred (the tap fires on one of its descendants).
lv_obj_t *s_search = nullptr;
lv_obj_t *s_search_ta = nullptr;
lv_obj_t *s_search_list = nullptr;
lv_obj_t *s_search_empty = nullptr;
bool      s_search_pending = false;

char lc_ascii(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c; }
bool ci_contains(const char *hay, const char *needle) {
    if (!needle[0]) return true;
    for (const char *h = hay; *h; h++) {
        const char *a = h, *b = needle;
        while (*a && *b && lc_ascii(*a) == lc_ascii(*b)) { a++; b++; }
        if (!*b) return true;
    }
    return false;
}

void search_close_apply(void *) {
    s_search_pending = false;
    nv_gesture_set_edge_enabled(NV_GESTURE_EDGE_LEFT, false);   // restore home state (back strip off)
    if (s_search) { lv_obj_delete(s_search); s_search = nullptr; }
    s_search_ta = s_search_list = s_search_empty = nullptr;
}
void search_close_deferred(void) {
    if (s_search_pending || !s_search) return;
    nv_ime_hide();
    if (lv_async_call(search_close_apply, nullptr) == LV_RESULT_OK) s_search_pending = true;
}
bool search_is_open(void) { return s_search != nullptr; }
void search_launch_cb(lv_event_t *e) {
    const NvApp *a = static_cast<const NvApp *>(lv_event_get_user_data(e));
    search_close_deferred();     // drop the overlay off this event; the app draws over it meanwhile
    if (a) open_app(a);
}
void search_rebuild(void) {
    if (!s_search_list) return;
    lv_obj_clean(s_search_list);
    const NvTheme *th = nv_theme_get();
    char q[40] = "";
    if (s_search_ta) lv_snprintf(q, sizeof q, "%s", lv_textarea_get_text(s_search_ta));
    int shown = 0;
    for (int i = 0; i < nv_app_count(); i++) {
        const NvApp *a = nv_app_at(i);
        if (!a) continue;
        const char *name = app_label(a);
        if (!ci_contains(name, q)) continue;
        lv_obj_t *row = lv_obj_create(s_search_list);   // icon + name, whole row tappable
        lv_obj_remove_style_all(row);
        lv_obj_set_size(row, lv_pct(100), LV_SIZE_CONTENT);
        lv_obj_set_style_min_height(row, 60, 0);
        lv_obj_set_style_radius(row, kRadSm, 0);
        lv_obj_set_style_bg_color(row, th->surface, 0);
        lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(row, th->surface2, LV_STATE_PRESSED);
        lv_obj_set_style_pad_hor(row, kSp4, 0);
        lv_obj_set_style_pad_column(row, kSp4, 0);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_event_cb(row, search_launch_cb, LV_EVENT_CLICKED, (void *)a);
        lv_obj_t *ic = lv_image_create(row);
        lv_image_set_src(ic, a->icon);
        lv_image_set_scale(ic, 160);   // ~62% of the 80px icon -> ~50px
        lv_obj_clear_flag(ic, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_t *l = lv_label_create(row);
        lv_label_set_text(l, name);
        lv_obj_set_style_text_color(l, th->text, 0);
        shown++;
    }
    if (s_search_empty) {
        if (shown) lv_obj_add_flag(s_search_empty, LV_OBJ_FLAG_HIDDEN);
        else       lv_obj_remove_flag(s_search_empty, LV_OBJ_FLAG_HIDDEN);
    }
}
void search_text_cb(lv_event_t *) { search_rebuild(); }

void search_open(lv_event_t *) {
    if (s_search) return;
    s_search_pending = false;
    const NvTheme *th = nv_theme_get();
    s_search = lv_obj_create(lv_screen_active());
    lv_obj_remove_style_all(s_search);
    lv_obj_set_size(s_search, lv_pct(100), lv_pct(100));
    apply_wallpaper(s_search);
    lv_obj_clear_flag(s_search, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(s_search, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(s_search, kSp4, 0);
    lv_obj_set_style_pad_row(s_search, kSp3, 0);

    // Field is a DIRECT child of the overlay (no header wrapper). This matters: when the keyboard
    // opens, the IME pads the field's PARENT by the keyboard height — so the parent must be the
    // overlay column, which shrinks the results list to sit above the keyboard. (With a wrapper
    // row as parent, the padding grew the header and shoved the list down behind the keyboard.)
    // No close button either: dismiss is the standard Back gesture (swipe-right from left edge).
    s_search_ta = nv_kit_textarea_ex(s_search, nv_tr(NV_STR_SEARCH), true, NV_IME_TEXT, NV_IME_RET_DEFAULT);
    lv_obj_set_width(s_search_ta, lv_pct(100));
    lv_obj_add_event_cb(s_search_ta, search_text_cb, LV_EVENT_VALUE_CHANGED, nullptr);

    s_search_list = nv_kit_scroll_column(s_search);
    lv_obj_set_flex_grow(s_search_list, 1);

    s_search_empty = lv_label_create(s_search);
    lv_label_set_text(s_search_empty, nv_tr(NV_STR_NO_RESULTS));
    lv_obj_set_style_text_color(s_search_empty, th->text_dim, 0);
    lv_obj_add_flag(s_search_empty, LV_OBJ_FLAG_HIDDEN);

    search_rebuild();   // list all apps until the user types

    // Back gesture: enable the left-edge strip (home leaves it off) and raise all strips above
    // this fresh overlay so the swipe-right lands on the strip, not the list.
    nv_gesture_set_edge_enabled(NV_GESTURE_EDGE_LEFT, true);
    nv_gesture_raise();

    // Pop the keyboard automatically — deferred one tick so the overlay layout is settled before
    // field_shift_apply() measures it. Same effect as tapping the field.
    lv_async_call([](void *) {
        if (s_search_ta) lv_obj_send_event(s_search_ta, LV_EVENT_FOCUSED, nullptr);
    }, nullptr);
}

// -------------------------------------------------------------- folder overlay
// Modal folder view (screen child, like the search overlay): rename field + member grid.
// Tap = launch, long-press = pull the member back onto the grid (a folder that drops to one
// member dissolves in place). Both act on close, which is deferred — the trigger event fires
// on a descendant of the overlay being deleted.
lv_obj_t *s_fold    = nullptr;   // scrim (overlay root)
lv_obj_t *s_fold_ta = nullptr;   // rename textarea
int       s_fold_id      = -1;
bool      s_fold_pending = false;
int       s_fold_launch  = -1;   // app registry index to open once the overlay is gone
int       s_fold_evict   = -1;   // member index to pull out once the overlay is gone

void folder_close_apply(void *) {
    s_fold_pending = false;
    const int f = s_fold_id;
    if (s_fold) {
        // Persist a rename typed in the overlay before the textarea dies.
        if (s_fold_ta && f >= 0 && s_folders[f].n) {
            lv_snprintf(s_folders[f].name, sizeof s_folders[f].name, "%s",
                        lv_textarea_get_text(s_fold_ta));
            if (!s_folders[f].name[0])
                lv_snprintf(s_folders[f].name, sizeof s_folders[f].name, "%s",
                            nv_tr(NV_STR_FOLDER));
        }
        lv_obj_delete(s_fold);
        s_fold = nullptr;
        s_fold_ta = nullptr;
    }
    s_fold_id = -1;

    if (f >= 0 && s_fold_evict >= 0) {           // long-press: pull one member out
        Folder *fo = &s_folders[f];
        const int m = s_fold_evict;
        s_fold_evict = -1;
        if (m < fo->n) {
            const int app = fo->mem[m];
            for (int i = m; i < fo->n - 1; i++) fo->mem[i] = fo->mem[i + 1];
            fo->n--;
            if (fo->n == 1) {                    // folders need >= 2: dissolve in place
                for (int i = 0; i < s_tile_n; i++)
                    if (s_order[i] == kEntFolder + f) { s_order[i] = fo->mem[0]; break; }
                fo->n = 0;
            }
            if (s_tile_n < kMaxEntries) s_order[s_tile_n++] = app;   // evicted app -> last slot
        }
    }
    s_fold_evict = -1;

    if (f >= 0) {
        order_save();          // rename and/or eviction
        rebuild_launcher();    // one code path for label, face and page-count updates
    }

    const int a = s_fold_launch;
    s_fold_launch = -1;
    if (a >= 0) open_app(nv_app_at(a));
}

void folder_close_deferred(void) {
    if (s_fold_pending || !s_fold) return;
    nv_ime_hide();
    if (lv_async_call(folder_close_apply, nullptr) == LV_RESULT_OK) s_fold_pending = true;
}

// Theme/lang/rotation refresh: drop the overlay with NO side effects (no rename persist, no
// launch) — the rebuild that follows recreates everything from the persisted model.
void folder_discard(void) {
    if (!s_fold) return;
    lv_obj_delete(s_fold);
    s_fold = nullptr;
    s_fold_ta = nullptr;
    s_fold_id = -1;
    s_fold_launch = -1;
    s_fold_evict = -1;
    s_fold_pending = false;
}

void fold_member_cb(lv_event_t *e) {
    const int m = (int)(intptr_t)lv_event_get_user_data(e);
    if (s_fold_id < 0 || m >= s_folders[s_fold_id].n) return;
    if (lv_event_get_code(e) == LV_EVENT_SHORT_CLICKED) {
        s_fold_launch = s_folders[s_fold_id].mem[m];
    } else {  // LV_EVENT_LONG_PRESSED
        s_fold_evict = m;
    }
    folder_close_deferred();
}

void fold_scrim_cb(lv_event_t *e) {
    if (lv_event_get_target_obj(e) == s_fold) folder_close_deferred();
}

void folder_open(int f) {
    if (s_fold || f < 0 || f >= kMaxFolders || !s_folders[f].n) return;
    s_fold_pending = false;
    s_fold_launch = -1;
    s_fold_evict = -1;
    s_fold_id = f;
    const Folder *fo = &s_folders[f];
    const NvTheme *th = nv_theme_get();

    s_fold = lv_obj_create(lv_screen_active());          // scrim: tap outside closes
    lv_obj_remove_style_all(s_fold);
    lv_obj_set_size(s_fold, LV_HOR_RES, LV_VER_RES);
    lv_obj_set_style_bg_color(s_fold, th->scrim, 0);
    lv_obj_set_style_bg_opa(s_fold, LV_OPA_60, 0);
    lv_obj_add_flag(s_fold, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(s_fold, LV_OBJ_FLAG_SCROLLABLE);
    nv_gesture_isolate(s_fold);
    lv_obj_add_event_cb(s_fold, fold_scrim_cb, LV_EVENT_CLICKED, nullptr);

    // Panel: rename field on top (stays clear of the IME) + 3-wide member grid.
    lv_obj_t *panel = lv_obj_create(s_fold);
    lv_obj_remove_style_all(panel);
    lv_obj_set_size(panel, 3 * (kTileW + kSp3) + 2 * kSp4, LV_SIZE_CONTENT);   // 476
    lv_obj_align(panel, LV_ALIGN_TOP_MID, 0, 60);
    lv_obj_set_style_bg_color(panel, th->surface, 0);
    lv_obj_set_style_bg_opa(panel, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(panel, kRadMd, 0);
    lv_obj_set_style_border_width(panel, 1, 0);
    lv_obj_set_style_border_color(panel, th->surface3, 0);
    lv_obj_set_style_pad_all(panel, kSp4, 0);
    lv_obj_set_style_pad_row(panel, kSp3, 0);
    lv_obj_set_flex_flow(panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_remove_flag(panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(panel, LV_OBJ_FLAG_CLICKABLE);       // swallow taps: don't hit the scrim

    s_fold_ta = nv_kit_textarea(panel, nv_tr(NV_STR_FOLDER), true);
    lv_textarea_set_text(s_fold_ta, fo->name);
    lv_textarea_set_max_length(s_fold_ta, (uint32_t)(sizeof fo->name - 1));
    lv_obj_set_width(s_fold_ta, lv_pct(100));

    lv_obj_t *grid = lv_obj_create(panel);
    lv_obj_remove_style_all(grid);
    lv_obj_set_size(grid, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(grid, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_gap(grid, kSp3, 0);
    lv_obj_set_flex_align(grid, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_remove_flag(grid, LV_OBJ_FLAG_SCROLLABLE);

    for (int m = 0; m < fo->n; m++) {
        const NvApp *a = nv_app_at(fo->mem[m]);
        if (!a) continue;
        lv_obj_t *tile = lv_obj_create(grid);
        lv_obj_remove_style_all(tile);
        lv_obj_set_size(tile, kTileW, kTileH);
        lv_obj_set_style_radius(tile, kRadMd, 0);
        lv_obj_set_style_bg_opa(tile, LV_OPA_TRANSP, 0);
        lv_obj_set_style_bg_color(tile, th->text_strong, LV_STATE_PRESSED);
        lv_obj_set_style_bg_opa(tile, LV_OPA_10, LV_STATE_PRESSED);
        lv_obj_add_flag(tile, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_remove_flag(tile, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_event_cb(tile, fold_member_cb, LV_EVENT_SHORT_CLICKED, (void *)(intptr_t)m);
        lv_obj_add_event_cb(tile, fold_member_cb, LV_EVENT_LONG_PRESSED,  (void *)(intptr_t)m);

        lv_obj_t *img = lv_image_create(tile);
        lv_image_set_src(img, a->icon);
        lv_obj_align(img, LV_ALIGN_TOP_MID, 0, kIconY);

        lv_obj_t *lbl = lv_label_create(tile);
        lv_label_set_text(lbl, app_label(a));
        lv_obj_set_width(lbl, kTileW);
        lv_label_set_long_mode(lbl, LV_LABEL_LONG_MODE_DOTS);
        lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_font(lbl, &nv_font_14, 0);
        lv_obj_set_style_text_color(lbl, th->text_strong, 0);
        lv_obj_align(lbl, LV_ALIGN_TOP_MID, 0, kLabelY);
    }
    nv_gesture_raise();   // strips stay top-most (overlay must not eat edge gestures)
}

// Folder tile face: rounded plate + up to 4 mini member icons. Image scale only — the sw
// renderer draws scaled images directly (no off-screen layer; same call the search list uses).
void tile_build_folder_face(lv_obj_t *tile, const Folder *fo) {
    const NvTheme *th = nv_theme_get();
    lv_obj_t *plate = lv_obj_create(tile);
    lv_obj_remove_style_all(plate);
    lv_obj_set_size(plate, 84, 84);
    lv_obj_align(plate, LV_ALIGN_TOP_MID, 0, -2);
    lv_obj_set_style_radius(plate, 20, 0);
    lv_obj_set_style_bg_color(plate, th->surface2, 0);
    lv_obj_set_style_bg_opa(plate, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(plate, 1, 0);
    lv_obj_set_style_border_color(plate, th->surface3, 0);
    lv_obj_remove_flag(plate, (lv_obj_flag_t)(LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE));
    for (int m = 0; m < fo->n && m < 4; m++) {
        const NvApp *a = nv_app_at(fo->mem[m]);
        if (!a) continue;
        lv_obj_t *mi = lv_image_create(plate);
        lv_image_set_src(mi, a->icon);
        lv_image_set_scale(mi, 96);   // 80px icon -> 30px mini
        lv_obj_align(mi, LV_ALIGN_CENTER, (m % 2) ? 19 : -19, (m / 2) ? 19 : -19);
        lv_obj_remove_flag(mi, LV_OBJ_FLAG_CLICKABLE);
    }
}

void build_launcher(lv_obj_t *scr) {
    s_launcher = lv_obj_create(scr);
    lv_obj_remove_style_all(s_launcher);
    lv_obj_set_size(s_launcher, LV_HOR_RES, LV_VER_RES - kStatusH);
    lv_obj_align(s_launcher, LV_ALIGN_TOP_MID, 0, kStatusH);
    apply_wallpaper(s_launcher);   // themed gradient backdrop (was a flat fill)
    lv_obj_set_style_pad_all(s_launcher, 0, 0);                       // positions are absolute
    lv_obj_remove_flag(s_launcher, LV_OBJ_FLAG_SCROLLABLE);           // manual grid, not scrolled
    lv_obj_add_flag(s_launcher, LV_OBJ_FLAG_CLICKABLE);               // receive empty-space tap
    nv_gesture_isolate(s_launcher);                                   // handle swipes here, never bubble
    lv_obj_add_event_cb(s_launcher, launcher_bg_clicked, LV_EVENT_CLICKED, nullptr);
    lv_obj_add_event_cb(s_launcher, launcher_gesture, LV_EVENT_GESTURE, nullptr);

    grid_compute();   // orientation-dependent geometry (6x3 / 3x6)
    order_load();     // builds s_order[]/s_folders and s_tile_n; recounts s_pages

    wall_attach(s_launcher);   // SD wallpaper (decode-once PSRAM cache); gradient otherwise

    // Page strip: one screen-width per page, slides horizontally (x animation, layer-free).
    // Created BEFORE the search pill and dots so those stay on top of it in z-order.
    s_strip = lv_obj_create(s_launcher);
    lv_obj_remove_style_all(s_strip);
    lv_obj_set_size(s_strip, s_pages * LV_HOR_RES, LV_VER_RES - kStatusH);
    lv_obj_set_pos(s_strip, -s_page * LV_HOR_RES, 0);
    lv_obj_add_flag(s_strip, (lv_obj_flag_t)(LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_GESTURE_BUBBLE));
    lv_obj_remove_flag(s_strip, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(s_strip, launcher_bg_clicked, LV_EVENT_CLICKED, nullptr);

    // Search bar pill at the top (grid origin padT leaves room). Tap -> search overlay.
    {
        const NvTheme *th = nv_theme_get();
        lv_obj_t *sb = lv_obj_create(s_launcher);
        lv_obj_remove_style_all(sb);
        lv_obj_set_size(sb, 560, 44);                 // grid padT 64 leaves 10px below
        lv_obj_align(sb, LV_ALIGN_TOP_MID, 0, 10);
        lv_obj_set_style_radius(sb, 22, 0);
        lv_obj_set_style_bg_color(sb, th->surface, 0);
        lv_obj_set_style_bg_opa(sb, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(sb, th->surface2, LV_STATE_PRESSED);
        lv_obj_set_style_border_width(sb, 1, 0);
        lv_obj_set_style_border_color(sb, th->surface3, 0);
        lv_obj_set_style_pad_hor(sb, kSp4, 0);
        lv_obj_set_flex_flow(sb, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(sb, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_add_flag(sb, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_clear_flag(sb, (lv_obj_flag_t)(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_GESTURE_BUBBLE));
        lv_obj_add_event_cb(sb, search_open, LV_EVENT_CLICKED, nullptr);
        lv_obj_t *sl = lv_label_create(sb);
        lv_label_set_text_fmt(sl, LV_SYMBOL_LIST "  %s", nv_tr(NV_STR_SEARCH));
        lv_obj_set_style_text_color(sl, th->text_dim, 0);
    }

    for (int slot = 0; slot < s_tile_n; slot++) {
        const int ent = s_order[slot];
        const Folder *fo = entry_folder(ent);
        const NvApp *a = fo ? nullptr : nv_app_at(ent);

        lv_obj_t *tile = lv_obj_create(s_strip);
        lv_obj_remove_style_all(tile);
        lv_obj_set_size(tile, kTileW, kTileH);                        // 136 x 122
        lv_obj_set_style_bg_opa(tile, LV_OPA_TRANSP, 0);
        lv_obj_set_style_radius(tile, kRadMd, 0);                     // rounded press highlight
        lv_obj_set_style_border_width(tile, 0, 0);
        lv_obj_set_style_shadow_width(tile, 0, 0);
        lv_obj_set_style_outline_width(tile, 0, 0);
        lv_obj_set_style_pad_all(tile, 0, 0);
        // Press feedback: a faint rounded highlight behind the icon+label (layer-free; a
        // transform_scale dip would force an off-screen draw layer that stalls the P4 renderer).
        lv_obj_set_style_bg_color(tile, nv_theme_get()->text_strong, LV_STATE_PRESSED);
        lv_obj_set_style_bg_opa(tile, LV_OPA_10, LV_STATE_PRESSED);
        lv_obj_set_align(tile, LV_ALIGN_TOP_LEFT);                    // manual positioning anchor
        lv_obj_add_flag(tile, LV_OBJ_FLAG_CLICKABLE);                 // click/long-press/gesture
        lv_obj_remove_flag(tile, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(tile, LV_OBJ_FLAG_GESTURE_BUBBLE);           // swipe-up bubbles to launcher

        if (fo) {
            tile_build_folder_face(tile, fo);
        } else {
            lv_obj_t *img = lv_image_create(tile);
            lv_image_set_src(img, a->icon);
            lv_obj_align(img, LV_ALIGN_TOP_MID, 0, kIconY);          // y=0
        }

        lv_obj_t *lbl = lv_label_create(tile);
        lv_label_set_text(lbl, fo ? fo->name : app_label(a));
        lv_obj_set_width(lbl, kTileW);                               // 136
        lv_label_set_long_mode(lbl, LV_LABEL_LONG_MODE_DOTS);  // canonical name in LVGL 9.2
        lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_font(lbl, &nv_font_14, 0);  // pinned: fixed 136x122 tile grid
        lv_obj_set_style_text_color(lbl, nv_theme_get()->text_strong, 0);  // legibility over bg
        lv_obj_align(lbl, LV_ALIGN_TOP_MID, 0, kLabelY);            // y=92

        lv_obj_set_user_data(tile, (void *)(intptr_t)slot);
        s_tiles[slot] = tile;
        tile_apply_pos(slot, false);                                // set_pos to slot_xy, no anim

        // Single handler resolves the entry via s_order[slot] at event time (order is dynamic).
        lv_obj_add_event_cb(tile, tile_event_cb, LV_EVENT_SHORT_CLICKED, nullptr);
        lv_obj_add_event_cb(tile, tile_event_cb, LV_EVENT_LONG_PRESSED,  nullptr);
        lv_obj_add_event_cb(tile, tile_event_cb, LV_EVENT_PRESSING,      nullptr);
        lv_obj_add_event_cb(tile, tile_event_cb, LV_EVENT_RELEASED,      nullptr);
        lv_obj_add_event_cb(tile, tile_event_cb, LV_EVENT_PRESS_LOST,    nullptr);
    }

    // Page dots: centered just above the dock, one tappable dot per page. Hidden with one page.
    s_dots = lv_obj_create(s_launcher);
    lv_obj_remove_style_all(s_dots);
    lv_obj_set_size(s_dots, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_align(s_dots, LV_ALIGN_BOTTOM_MID, 0, -(kDockMarg + kDockH + 10));
    lv_obj_set_flex_flow(s_dots, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(s_dots, 12, 0);
    lv_obj_remove_flag(s_dots, LV_OBJ_FLAG_SCROLLABLE);
    if (s_pages < 2) lv_obj_add_flag(s_dots, LV_OBJ_FLAG_HIDDEN);
    for (int p = 0; p < s_pages; p++) {
        lv_obj_t *d = lv_obj_create(s_dots);
        lv_obj_remove_style_all(d);
        lv_obj_set_size(d, 10, 10);
        lv_obj_set_style_radius(d, LV_RADIUS_CIRCLE, 0);
        lv_obj_add_flag(d, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_ext_click_area(d, 10);   // keep the tap target near kTouchMin
        lv_obj_add_event_cb(d, dot_clicked, LV_EVENT_CLICKED, (void *)(intptr_t)p);
    }
    dots_update();

    build_dock();   // usage-ranked favorites pill, above the dots' band
}

// Delete + rebuild the whole launcher subtree (used by folder edits and the theme/lang/
// rotation refresh). order_load() re-reads the persisted model, so positions are preserved;
// edit mode always ends (its tiles were deleted).
void rebuild_launcher(void) {
    const bool app_open = (s_app != nullptr);   // launcher stays hidden behind an open app
    if (s_launcher) {
        lv_obj_delete(s_launcher);
        s_launcher = nullptr;
    }
    s_strip = nullptr;
    s_dots = nullptr;
    s_dock = nullptr;
    for (int i = 0; i < kMaxEntries; i++) s_tiles[i] = nullptr;
    s_launcher_edit = false;
    s_drag_slot = -1;
    s_merge_slot = -1;
    s_edge_since = 0;
    build_launcher(lv_screen_active());
    if (app_open) lv_obj_add_flag(s_launcher, LV_OBJ_FLAG_HIDDEN);
    nv_gesture_raise();   // strips must stay above the fresh subtree
}

// -------------------------------------------------------------- live UI re-render (lang + theme)
// One async rebuild serves BOTH a language change (build-code re-reads nv_tr) and a theme
// change (build-code re-reads nv_theme_get). It rebuilds every dynamic surface — launcher,
// shade, and the open app — plus re-applies the screen-level inherited bg + default font so a
// mode / accent / font-scale change takes effect on the root before children inherit from it.
// Runs from lv_async_call (next LVGL loop), NOT inline from the change event: that event can
// fire from inside a Settings row's click handler, whose object graph is about to be rebuilt
// here — doing it synchronously would free the object still unwinding on the stack (UAF).
bool s_refresh_pending = false;  // coalesce a lang+theme burst (or repeats) into one rebuild

void ui_refresh_async(void *) {
    s_refresh_pending = false;
    lv_obj_t *scr = lv_screen_active();

    // Hide + unbind the IME before rebuilding: the open app's content (which may hold the
    // bound textarea) is about to be cleaned, and the keyboard survives on the screen.
    nv_ime_hide();

    // Tear down the search + folder overlays if open: the rebuilt launcher/shade are appended
    // as newer screen children and would bury these still-live overlays (z-order inversion +
    // stale theme). Safe to delete directly here — this runs from lv_async, not from the
    // overlays' own events. folder_discard() drops pending launch/evict side effects too.
    if (s_search) search_close_apply(nullptr);
    folder_discard();

    // Re-apply screen-level inherited styles first so a mode / font-scale change is live on the
    // root before children rebuild and inherit from it.
    const NvTheme *th = nv_theme_get();
    apply_wallpaper(scr);
    lv_obj_set_style_text_font(scr, th->font_default, 0);

    // Status bar: re-color + re-size in place (do NOT rebuild — its 1 Hz status_tick timer
    // must not duplicate). The width matters after a display rotation.
    if (s_statusbar) {
        lv_obj_set_width(s_statusbar, LV_HOR_RES);
        lv_obj_set_style_bg_color(s_statusbar, th->surface, 0);
        if (s_clock)    lv_obj_set_style_text_color(s_clock, th->text, 0);
        if (s_date)     lv_obj_set_style_text_color(s_date, th->text_dim, 0);
        if (s_heap)     lv_obj_set_style_text_color(s_heap, th->text_dim, 0);
        if (s_bell)     lv_obj_set_style_text_color(s_bell, th->accent, 0);
        if (s_sd_ico)   lv_obj_set_style_text_color(s_sd_ico, th->text_dim, 0);
        if (s_wifi_ico)  lv_obj_set_style_text_color(s_wifi_ico, th->text_dim, 0);  // tick re-tints
        if (s_wifi_ssid) lv_obj_set_style_text_color(s_wifi_ssid, th->text_dim, 0); // tick re-tints
    }

    // Launcher: delete + rebuild so tile labels/geometry pick up the new language/theme/
    // orientation. order_load() re-reads the persisted model, so positions are preserved.
    rebuild_launcher();   // keeps the launcher hidden itself while an app is open

    // Shade: delete + rebuild the whole subtree (panel is a child of the scrim, so deleting the
    // scrim takes the panel with it). Reset the open flag so a mid-open refresh can't leave a
    // stuck-open state; build_shade re-creates the scrim hidden == closed.
    if (s_shade_scrim) {
        lv_obj_delete(s_shade_scrim);
        s_shade_scrim = nullptr;
        s_shade = nullptr;
    }
    s_shade_open = false;
    build_shade(scr);

    // Open app: re-size the fixed chrome first (matters after a rotation; the alignments are
    // persistent, only explicit sizes go stale), then re-render the content in place.
    if (s_app) {
        lv_obj_set_size(s_app, LV_HOR_RES, LV_VER_RES - kStatusH);
        if (s_app_hdr)   lv_obj_set_width(s_app_hdr, LV_HOR_RES);
        if (s_app_title) lv_obj_set_width(s_app_title, LV_HOR_RES - 2 * kSp4);
        if (s_app_content)
            lv_obj_set_size(s_app_content, LV_HOR_RES,
                            LV_VER_RES - kStatusH - kHeaderH - kHomeInset);
    }
    // s_app_cur may be NULL (home) or cleared if the app was closed between the event and
    // this async callback — guard and skip.
    if (s_app_cur && s_app_content) {
        lv_obj_clean(s_app_content);      // fires sub-page LV_EVENT_DELETE cleanups
        nv_ui_set_back(nullptr);
        nv_ui_set_title(app_label(s_app_cur));
        if (s_app_cur->build) s_app_cur->build(s_app_content);
    }

    // IME: re-apply theme colors and the language-specific key map (it lives on the screen,
    // so it was not rebuilt above; it re-raises itself to the foreground on its next show).
    nv_ime_retheme();
    nv_ime_relayout();

    // Keep the edge strips top-most after all rebuilds (home or in-app), uniformly.
    // (The IME re-raises itself on its next show; strips and keyboard occupy disjoint regions.)
    nv_gesture_raise();
    NV_LOGI(TAG, "UI refreshed (async)");
}

void on_ui_invalidate(nv_event_t, const void *, void *) {
    // Defer + coalesce the rebuild; see ui_refresh_async(). One deferred rebuild covers a
    // lang+theme burst in the same loop. Passing NULL keeps no stale pointers.
    if (s_refresh_pending) return;
    // Only latch if scheduling succeeded, else a failed lv_async_call would wedge all future
    // refreshes (flag stuck true, callback never runs to clear it).
    if (lv_async_call(ui_refresh_async, nullptr) == LV_RESULT_OK) s_refresh_pending = true;
}

}  // namespace

// ================================================================= app-host API (public C)
void nv_ui_set_title(const char *text) {
    if (s_app_title) lv_label_set_text(s_app_title, text);
}
void nv_ui_set_back(void (*handler)(void)) { s_app_back = handler; }
lv_obj_t *nv_ui_app_content(void) { return s_app_content; }
const NvApp *nv_ui_current_app(void) { return s_app_cur; }
bool nv_ui_shade_is_open(void) { return s_shade_open; }

// Enable/disable the notification-shade open gesture (top-edge + status-bar swipe-down). The video
// player turns it OFF while running so a swipe can't pull the shade down over the film.
void nv_ui_set_shade_gesture_enabled(bool on) {
    s_shade_gesture_on = on;
    if (!on && s_shade_open) close_shade();   // close it if already open when we disable
}

int nv_app_unregister(const char *id) {
    if (!id) return 0;
    int r = -1;
    for (int i = 0; i < g_app_n; i++)
        if (g_apps[i] && g_apps[i]->id && strcmp(g_apps[i]->id, id) == 0) { r = i; break; }
    if (r < 0) return 0;

    for (int i = r; i < g_app_n - 1; i++) g_apps[i] = g_apps[i + 1];
    g_app_n--;

    // recents hold registry indices — drop the removed one and shift the higher ones down.
    int w = 0;
    for (int i = 0; i < s_recents_n; i++) {
        int v = s_recents[i];
        if (v == r) continue;
        s_recents[w++] = v > r ? v - 1 : v;
    }
    s_recents_n = w;

    rebuild_launcher();   // live: tiles + smart dock re-derive from the registry (dock keys by id)
    return 1;
}

void nv_ui_open_app(const NvApp *app) { if (app) open_app(app); }
const NvApp *nv_ui_find_app(const char *id) {
    if (!id) return nullptr;
    for (int i = 0; i < nv_app_count(); i++) {
        const NvApp *a = nv_app_at(i);
        if (a && a->id && strcmp(a->id, id) == 0) return a;
    }
    return nullptr;
}

// Legacy wrapper — toasts are a system service now (nv_notify). Existing callers keep working;
// new code should call nv_toast()/nv_notify_post() with an explicit severity.
void nv_ui_toast(const char *msg) { nv_toast(NV_NOTE_INFO, msg); }

// ================================================================= remote UI automation
// A synthetic pointer input device lets an off-device caller (the web server) drive the UI
// headlessly to capture screenshots of any app/state. Created lazily on first use (LVGL thread).
// The read_cb reports our injected point/state; a tap sets pressed then a one-shot timer releases
// it, so LVGL resolves a real press->release->CLICKED without the caller blocking.
namespace {
lv_indev_t *s_auto_indev = nullptr;
lv_point_t  s_auto_pt = {0, 0};
bool        s_auto_pressed = false;

void auto_read_cb(lv_indev_t *, lv_indev_data_t *data) {
    data->point = s_auto_pt;
    data->state = s_auto_pressed ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
}
void auto_release_cb(lv_timer_t *t) { s_auto_pressed = false; lv_timer_delete(t); }
void auto_ensure(void) {
    if (s_auto_indev) return;
    s_auto_indev = lv_indev_create();
    lv_indev_set_type(s_auto_indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(s_auto_indev, auto_read_cb);
}
}  // namespace

bool nv_ui_open_app_id(const char *id) {
    const NvApp *a = nv_ui_find_app(id);
    if (!a) return false;
    if (s_app) close_app();     // solo-mode: leave any current app before opening the next
    open_app(a);
    return s_app_cur == a;
}

void nv_ui_go_home(void) {
    if (search_is_open()) search_close_deferred();
    if (s_shade_open) close_shade();
    recents_close();
    if (s_app) close_app();
}

void nv_ui_tap(int x, int y) {
    auto_ensure();
    s_auto_pt.x = (lv_coord_t)x;
    s_auto_pt.y = (lv_coord_t)y;
    s_auto_pressed = true;
    lv_timer_create(auto_release_cb, 80, nullptr);   // ~80 ms press window -> resolves to a click
}

const char *nv_ui_current_app_id(void) {
    return (s_app_cur && s_app_cur->id) ? s_app_cur->id : "";
}

// ================================================================= entry
// ================================================================= screen sleep (idle blank)
// Battery/panel saver: after "scr_timeout" seconds with no touch, blank the backlight and park a
// full-screen catcher on the top layer. The first tap turns the light back on AND is swallowed by
// the catcher, so waking never activates whatever happened to be under the finger. 0 = never.
// The timeout is cached (NVS read only on change) — the 1s tick stays allocation- and IO-free.
namespace {
int       s_sleep_s   = 0;         // cached "scr_timeout" (seconds; 0 = never)
bool      s_asleep    = false;
lv_obj_t *s_wake_catch = nullptr;  // top-layer full-screen tap catcher while asleep

// ================================================================= lock screen + PIN
// Lock/PIN overlay lives on lv_layer_top (above apps, shade, IME — the same layer the wake
// catcher uses). A 4-digit PIN when set, else a plain Unlock button. Deleting the lock or the
// set-PIN modal from a handler that fired on one of their own descendants is deferred via
// lv_async_call (delete-ancestor-mid-event is illegal in LVGL).
void lock_hide_deferred(void);       // fwd
void pinmodal_close_deferred(void);  // fwd

// ---- reusable numeric PIN pad ----
struct PinPad {
    char      entry[6];
    int       len;
    int       mode;        // 0 = verify (unlock), 1 = set
    int       stage;       // set-mode: 0 = first entry, 1 = confirm re-entry
    char      first[6];    // set-mode: the first entry, held until confirmed
    lv_obj_t *dots[4];
    lv_obj_t *msg;
};

void pin_dots_refresh(PinPad *p) {
    const NvTheme *th = nv_theme_get();
    for (int i = 0; i < 4; i++) {
        const bool filled = i < p->len;
        // Filled = solid accent disc; empty = a hollow ring — the modern PIN look.
        lv_obj_set_style_bg_opa(p->dots[i], filled ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
        lv_obj_set_style_bg_color(p->dots[i], th->accent, 0);
        lv_obj_set_style_border_width(p->dots[i], filled ? 0 : 2, 0);
        lv_obj_set_style_border_color(p->dots[i], th->text_dim, 0);
    }
}
// Neutral prompt for the current mode/stage, in the dim color (clears any stale WRONG_PIN red).
void pin_prompt(PinPad *p) {
    if (!p->msg) return;
    nv_str_id_t id = (p->mode == 1) ? (p->stage == 0 ? NV_STR_SET_PIN : NV_STR_CONFIRM_PIN)
                                    : NV_STR_ENTER_PIN;
    lv_label_set_text(p->msg, nv_tr(id));
    lv_obj_set_style_text_color(p->msg, nv_theme_get()->text_dim, 0);
}
void pin_error(PinPad *p) {
    const NvTheme *th = nv_theme_get();
    if (p->msg) {
        lv_label_set_text(p->msg, nv_tr(NV_STR_WRONG_PIN));
        lv_obj_set_style_text_color(p->msg, th->danger, 0);
    }
    for (int i = 0; i < 4; i++)   // flash the empty rings red (cleared on the next digit)
        lv_obj_set_style_border_color(p->dots[i], th->danger, 0);
    nv_audio_alert();             // low error tone (silent when muted)
}
void pin_free_cb(lv_event_t *e) {
    PinPad *p = static_cast<PinPad *>(lv_obj_get_user_data(lv_event_get_target_obj(e)));
    if (p) lv_free(p);
}
void pin_commit(PinPad *p) {
    if (p->mode == 1) {                            // set: require a matching re-entry
        if (p->stage == 0) {                       // stash the first entry, ask for confirmation
            lv_strcpy(p->first, p->entry);
            p->stage = 1; p->len = 0; p->entry[0] = '\0';
            pin_dots_refresh(p);
            pin_prompt(p);                         // -> "Confirm PIN"
            return;
        }
        if (strcmp(p->first, p->entry) == 0) {     // confirmed: persist + arm the lock
            nv_config_set_str("lockpin", p->entry);
            nv_config_set_bool("lock_en", true);
            nv_toast(NV_NOTE_OK, nv_tr(NV_STR_PIN_SAVED));
            pinmodal_close_deferred();
        } else {                                   // mismatch: start over, no partial state stored
            p->stage = 0; p->first[0] = '\0'; p->len = 0; p->entry[0] = '\0';
            pin_dots_refresh(p);
            pin_error(p);
        }
        return;
    }
    char stored[6];                                // verify against the saved PIN
    nv_config_get_str("lockpin", "", stored, sizeof stored);
    if (strcmp(stored, p->entry) == 0) {
        lock_hide_deferred();
    } else {
        p->len = 0; p->entry[0] = '\0';
        pin_dots_refresh(p);
        pin_error(p);
    }
}
void pin_key_cb(lv_event_t *e) {
    lv_obj_t *btn = lv_event_get_target_obj(e);
    // btn -> grid -> pad; the pad carries the PinPad* (one active pad per subtree, no shared state)
    PinPad *p = static_cast<PinPad *>(lv_obj_get_user_data(lv_obj_get_parent(lv_obj_get_parent(btn))));
    if (!p) return;
    nv_audio_click();   // tactile feedback (honors the key-click pref; silent when off/muted)
    const char k = (char)(intptr_t)lv_event_get_user_data(e);
    if (k == '<') { if (p->len > 0) { p->entry[--p->len] = '\0'; pin_dots_refresh(p); } return; }
    if (p->len >= 4) return;
    if (p->len == 0) pin_prompt(p);   // first digit of a fresh entry clears any stale error
    p->entry[p->len++] = k; p->entry[p->len] = '\0';
    pin_dots_refresh(p);
    if (p->len == 4) pin_commit(p);
}

// Modern circular PIN pad: 4 ring/fill dots, a prompt line, and a 3x4 grid of round keys with
// large glyphs. Fully self-contained (heap PinPad* freed on delete); reused by the lock screen
// (verify) and the Settings set-PIN modal (set + confirm).
constexpr int kPinKey = 74;    // circular key diameter
constexpr int kPinGap = 18;    // gap between keys

lv_obj_t *build_pin_pad(lv_obj_t *parent, int mode) {
    const NvTheme *th = nv_theme_get();
    PinPad *p = static_cast<PinPad *>(lv_malloc(sizeof(PinPad)));
    if (!p) return nullptr;
    p->len = 0; p->entry[0] = '\0'; p->mode = mode; p->msg = nullptr;
    p->stage = 0; p->first[0] = '\0';

    lv_obj_t *pad = lv_obj_create(parent);
    lv_obj_remove_style_all(pad);
    lv_obj_set_size(pad, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(pad, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(pad, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(pad, kSp4, 0);
    lv_obj_clear_flag(pad, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_user_data(pad, p);
    lv_obj_add_event_cb(pad, pin_free_cb, LV_EVENT_DELETE, nullptr);

    p->msg = lv_label_create(pad);   // prompt above the dots ("Enter PIN" / "Confirm PIN")
    pin_prompt(p);

    lv_obj_t *dots = lv_obj_create(pad);        // 4 entry dots (ring when empty, disc when filled)
    lv_obj_remove_style_all(dots);
    lv_obj_set_size(dots, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(dots, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(dots, kSp4, 0);
    lv_obj_clear_flag(dots, LV_OBJ_FLAG_SCROLLABLE);
    for (int i = 0; i < 4; i++) {
        lv_obj_t *d = lv_obj_create(dots);
        lv_obj_remove_style_all(d);
        lv_obj_set_size(d, 16, 16);
        lv_obj_set_style_radius(d, LV_RADIUS_CIRCLE, 0);
        lv_obj_clear_flag(d, LV_OBJ_FLAG_CLICKABLE);
        p->dots[i] = d;
    }

    lv_obj_t *grid = lv_obj_create(pad);        // 3-column round keypad via row-wrap
    lv_obj_remove_style_all(grid);
    lv_obj_set_size(grid, 3 * kPinKey + 2 * kPinGap, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(grid, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_gap(grid, kPinGap, 0);
    lv_obj_set_flex_align(grid, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(grid, LV_OBJ_FLAG_SCROLLABLE);
    struct Key { const char *lbl; char k; };
    static const Key keys[12] = {
        {"1",'1'},{"2",'2'},{"3",'3'},
        {"4",'4'},{"5",'5'},{"6",'6'},
        {"7",'7'},{"8",'8'},{"9",'9'},
        {"", 0}, {"0",'0'}, {LV_SYMBOL_BACKSPACE,'<'},
    };
    for (int i = 0; i < 12; i++) {
        if (keys[i].k == 0) {                   // blank spacer keeps 0 centered on the last row
            lv_obj_t *sp = lv_obj_create(grid);
            lv_obj_remove_style_all(sp);
            lv_obj_set_size(sp, kPinKey, kPinKey);
            lv_obj_clear_flag(sp, (lv_obj_flag_t)(LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE));
            continue;
        }
        const bool is_back = (keys[i].k == '<');
        lv_obj_t *b = lv_button_create(grid);
        lv_obj_set_size(b, kPinKey, kPinKey);
        lv_obj_set_style_radius(b, LV_RADIUS_CIRCLE, 0);
        // Digits: translucent surface disc. Backspace: borderless (reads as a plain glyph).
        lv_obj_set_style_bg_color(b, th->surface2, 0);
        lv_obj_set_style_bg_opa(b, is_back ? LV_OPA_TRANSP : LV_OPA_50, 0);
        lv_obj_set_style_bg_opa(b, LV_OPA_COVER, LV_STATE_PRESSED);
        lv_obj_set_style_bg_color(b, th->accent, LV_STATE_PRESSED);
        lv_obj_set_style_text_color(b, th->text_strong, 0);
        lv_obj_set_style_text_color(b, th->on_primary, LV_STATE_PRESSED);
        if (!is_back) lv_obj_set_style_text_font(b, &lv_font_montserrat_28, 0);  // big ASCII digit
        lv_obj_add_event_cb(b, pin_key_cb, LV_EVENT_CLICKED, (void *)(intptr_t)keys[i].k);
        lv_obj_t *l = lv_label_create(b);
        lv_label_set_text(l, keys[i].lbl);
        lv_obj_center(l);
    }
    pin_dots_refresh(p);
    return pad;
}

// ---- lock screen ----
lv_obj_t  *s_lock = nullptr;
lv_obj_t  *s_lock_clock = nullptr;
lv_obj_t  *s_lock_date  = nullptr;
lv_obj_t  *s_lock_notif = nullptr;   // unread-notification count line (hidden at 0)
lv_timer_t *s_lock_timer = nullptr;
bool       s_lock_close_pending = false;

void lock_tick(lv_timer_t *) {
    if (!s_lock_clock || s_asleep) return;   // no point formatting while the panel is blanked
    char b[24];
    nv_time_format(b, sizeof b, nv_time_is_24h() ? "%H:%M" : "%I:%M %p");
    lv_label_set_text(s_lock_clock, b);
    if (s_lock_date) {
        struct tm t; nv_time_now(&t);
        lv_label_set_text_fmt(s_lock_date, "%s %d %s", nv_i18n_wday_short(t.tm_wday), t.tm_mday,
                              nv_i18n_month_short(t.tm_mon));
    }
    if (s_lock_notif) {   // live unread badge under the date
        const int u = nv_notify_unread();
        if (u > 0) {
            lv_label_set_text_fmt(s_lock_notif, LV_SYMBOL_BELL "  %d", u);
            lv_obj_remove_flag(s_lock_notif, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(s_lock_notif, LV_OBJ_FLAG_HIDDEN);
        }
    }
}
void lock_apply_close(void *) {
    s_lock_close_pending = false;
    if (s_lock_timer) { lv_timer_delete(s_lock_timer); s_lock_timer = nullptr; }
    if (s_lock) { lv_obj_delete(s_lock); s_lock = nullptr; }
    s_lock_clock = s_lock_date = s_lock_notif = nullptr;
}
void lock_hide_deferred(void) {
    if (s_lock_close_pending || !s_lock) return;
    if (lv_async_call(lock_apply_close, nullptr) == LV_RESULT_OK) s_lock_close_pending = true;
}
void lock_unlock_cb(lv_event_t *) { lock_hide_deferred(); }

void lock_show(void) {
    if (s_lock) return;
    s_lock_close_pending = false;
    nv_ime_hide();   // drop any on-screen keyboard so it can't linger under/around the lock
    const NvTheme *th = nv_theme_get();
    s_lock = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(s_lock);
    lv_obj_set_size(s_lock, lv_pct(100), lv_pct(100));
    apply_wallpaper(s_lock);
    lv_obj_add_flag(s_lock, LV_OBJ_FLAG_CLICKABLE);        // swallow taps to whatever is beneath
    lv_obj_clear_flag(s_lock, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(s_lock, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_lock, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(s_lock, 24, 0);   // airy gap between the clock hero and the keypad

    // Hero: big clock + date, tight together at the top third.
    lv_obj_t *hero = lv_obj_create(s_lock);
    lv_obj_remove_style_all(hero);
    lv_obj_set_size(hero, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(hero, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(hero, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(hero, kSp2, 0);
    lv_obj_clear_flag(hero, LV_OBJ_FLAG_SCROLLABLE);
    s_lock_clock = lv_label_create(hero);
    lv_obj_set_style_text_font(s_lock_clock, &lv_font_montserrat_48, 0);   // large hero clock
    lv_obj_set_style_text_color(s_lock_clock, th->text_strong, 0);
    s_lock_date = lv_label_create(hero);
    lv_obj_set_style_text_font(s_lock_date, &nv_font_20, 0);
    lv_obj_set_style_text_color(s_lock_date, th->text_dim, 0);
    s_lock_notif = lv_label_create(hero);   // unread badge (filled by lock_tick; hidden at 0)
    lv_obj_set_style_text_color(s_lock_notif, th->accent, 0);
    lv_obj_add_flag(s_lock_notif, LV_OBJ_FLAG_HIDDEN);

    char pin[6];
    nv_config_get_str("lockpin", "", pin, sizeof pin);
    if (pin[0]) {
        build_pin_pad(s_lock, 0);
    } else {
        // No PIN: a hint + a clear pill button (deliberate unlock, not a stray tap).
        lv_obj_t *hint = lv_label_create(s_lock);
        lv_label_set_text_fmt(hint, LV_SYMBOL_UP "  %s", nv_tr(NV_STR_UNLOCK));
        lv_obj_set_style_text_color(hint, th->text_dim, 0);
        lv_obj_t *btn = nv_kit_button(s_lock, nv_tr(NV_STR_UNLOCK), true);
        lv_obj_set_width(btn, 220);
        lv_obj_add_event_cb(btn, lock_unlock_cb, LV_EVENT_CLICKED, nullptr);
    }
    lock_tick(nullptr);
    s_lock_timer = lv_timer_create(lock_tick, 1000, nullptr);
}

// ---- set-PIN modal (Settings) ----
lv_obj_t *s_pinmodal = nullptr;
bool      s_pinmodal_pending = false;

void pinmodal_apply_close(void *) {
    s_pinmodal_pending = false;
    if (s_pinmodal) { lv_obj_delete(s_pinmodal); s_pinmodal = nullptr; }
}
void pinmodal_close_deferred(void) {
    if (s_pinmodal_pending || !s_pinmodal) return;
    if (lv_async_call(pinmodal_apply_close, nullptr) == LV_RESULT_OK) s_pinmodal_pending = true;
}
void pin_set_show(void) {
    if (s_pinmodal) return;
    s_pinmodal_pending = false;
    const NvTheme *th = nv_theme_get();
    s_pinmodal = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(s_pinmodal);
    lv_obj_set_size(s_pinmodal, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_color(s_pinmodal, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_pinmodal, LV_OPA_50, 0);
    lv_obj_add_flag(s_pinmodal, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(s_pinmodal, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(s_pinmodal, [](lv_event_t *e) {   // tap the scrim (outside the card) cancels
        if (lv_event_get_target_obj(e) == lv_event_get_current_target_obj(e)) pinmodal_close_deferred();
    }, LV_EVENT_CLICKED, nullptr);

    lv_obj_t *card = lv_obj_create(s_pinmodal);
    lv_obj_remove_style_all(card);
    lv_obj_set_size(card, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_center(card);
    lv_obj_set_style_bg_color(card, th->surface, 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(card, kRadMd, 0);
    lv_obj_set_style_pad_all(card, kSp4, 0);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(card, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    build_pin_pad(card, 1);
}

void screen_wake(lv_event_t *) {
    if (!s_asleep) return;
    s_asleep = false;
    nv_hal_backlight_set(nv_config_get_int("brightness", 90));
    lv_display_trigger_activity(nullptr);           // restart the idle clock
    if (s_wake_catch) { lv_obj_delete(s_wake_catch); s_wake_catch = nullptr; }
}

void screen_sleep_now(void) {
    if (s_asleep) return;
    s_asleep = true;
    if (nv_config_get_bool("lock_en", false)) lock_show();   // arm the lock UNDER the wake catch,
                                                             // so the wake tap reveals the lock
    s_wake_catch = lv_obj_create(lv_layer_top());   // above screen children (apps, shade, IME)
    lv_obj_remove_style_all(s_wake_catch);
    lv_obj_set_size(s_wake_catch, lv_pct(100), lv_pct(100));
    lv_obj_add_flag(s_wake_catch, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_wake_catch, screen_wake, LV_EVENT_PRESSED, nullptr);
    nv_hal_backlight_set(0);
}

// ---- USB display (Second Screen) system integration ------------------------------
// nv_usb publishes NV_EV_USB_DISPLAY from its USB tasks; the subscriber only flips these
// flags and the 1s housekeeping timer below does the UI work on the LVGL thread.
volatile bool s_usb_conn_evt = false;    // mount edge -> wake the screen + toast
volatile bool s_usb_stream_req = false;  // PC streams with no consumer -> auto-open the app

void on_usb_display(nv_event_t, const void *d, void *) {
    auto *e = static_cast<const nv_usb_display_ev_t *>(d);
    if (!e->mounted) { s_usb_conn_evt = false; s_usb_stream_req = false; return; }
    if (e->streaming_unclaimed) s_usb_stream_req = true;
    else                        s_usb_conn_evt = true;
}

void usb_display_tick(void) {
    if (s_usb_conn_evt) {
        s_usb_conn_evt = false;
        if (s_asleep) screen_wake(nullptr);   // plugging a monitor cable wakes the panel
        lv_display_trigger_activity(nullptr);
        nv_toast(NV_NOTE_OK, nv_tr(NV_STR_SS_PC_CONNECTED));
    }
    if (s_usb_stream_req) {
        s_usb_stream_req = false;
        // Auto-open Second Screen when the PC starts extending its desktop here — unless
        // disabled, locked, asleep, or something else is in the foreground.
        if (nv_config_get_bool("ss_auto", true) &&
            !s_app && !s_lock && !s_asleep && !s_shade_open) {
            for (int i = 0; i < nv_app_count(); i++) {
                const NvApp *a = nv_app_at(i);
                if (a && a->id && strcmp(a->id, "secondscreen") == 0) { open_app(a); break; }
            }
        } else {
            nv_notify_post(NV_NOTE_INFO, "USB", nv_tr(NV_STR_SS_PC_CONNECTED));
        }
    }
}

void sleep_tick(lv_timer_t *) {
    usb_display_tick();
    if (s_sleep_s <= 0 || s_asleep) return;
    if (lv_display_get_inactive_time(nullptr) > (uint32_t)s_sleep_s * 1000u)
        screen_sleep_now();
}

// Re-cache on any settings write (cheap int store; safe from any publisher thread).
void on_sleep_cfg(nv_event_t, const void *, void *) {
    s_sleep_s = nv_config_get_int("scr_timeout", 0);
}
}  // namespace

// Public lock API (thin wrappers over the anonymous-namespace implementation above).
void nv_ui_lock(void)        { lock_show(); }
bool nv_ui_is_locked(void)   { return s_lock != nullptr; }
void nv_ui_set_pin_flow(void){ pin_set_show(); }

void nv_ui_start(void) {
    if (!lvgl_port_lock(2000)) {
        NV_LOGE(TAG, "could not lock LVGL to build SystemUI");
        return;
    }
    // Restore the persisted orientation FIRST — everything below measures LV_HOR_RES/
    // LV_VER_RES (grid, status bar, shade, gesture strips).
    if (nv_config_get_int("rotation", 0) == 90)
        lv_display_set_rotation(lv_display_get_default(), LV_DISPLAY_ROTATION_90);

    lv_obj_t *scr = lv_screen_active();
    const NvTheme *th = nv_theme_get();
    apply_wallpaper(scr);
    lv_obj_set_style_text_font(scr, th->font_default, 0);  // inherited default: accents + font-scale
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    // NO screen-level gesture handler — per the nv_gesture standard, system gestures live
    // exclusively on the bezel edge strips below; content swipes can never trigger them.

    build_launcher(scr);
    build_status_bar(scr);
    build_shade(scr);

    // Centralized gesture standard: TOP edge = shade open/close; LEFT edge = in-app back
    // (starts hidden — home has no back). BOTTOM/RIGHT stay unassigned: no strip exists, so a
    // bottom-up swipe is always plain content interaction and can never close an app.
    nv_gesture_init(scr);
    nv_gesture_set_edge(NV_GESTURE_EDGE_TOP,    top_edge_cb,    nullptr);
    nv_gesture_set_edge(NV_GESTURE_EDGE_LEFT,   left_edge_cb,   nullptr);
    nv_gesture_set_edge(NV_GESTURE_EDGE_BOTTOM, bottom_edge_cb, nullptr);   // swipe up = home
    nv_gesture_set_edge_enabled(NV_GESTURE_EDGE_LEFT, false);
    // BOTTOM strip stays enabled everywhere: at home it opens Recents, in an app it goes home.
    nv_gesture_set_edge_enabled(NV_GESTURE_EDGE_BOTTOM, true);

    // IME last, so the shared keyboard sits above the launcher/shade/app plane (top-most).
    nv_ime_init(scr);
    // Re-raise the strips above the keyboard so the edge gestures survive (they occupy
    // disjoint screen regions, but keep the invariant "strips are top-most" uniform).
    nv_gesture_raise();

    // Live re-render on language OR theme change (handler defers to lv_async_call — see
    // ui_refresh_async). Both topics share one coalesced rebuild.
    nv_event_subscribe(NV_EV_LANG_CHANGED,  on_ui_invalidate, nullptr);
    nv_event_subscribe(NV_EV_THEME_CHANGED, on_ui_invalidate, nullptr);

    // Notification service -> SystemUI: badge + live shade list on every post/clear.
    nv_notify_set_listener(on_notify_changed);

    // Post-update notice: first boot on a new firmware posts a system notification, so the
    // user sees the OTA landed (and with which version) without opening Settings.
    const esp_app_desc_t *ad = esp_app_get_description();
    char lastv[36] = "";
    nv_config_get_str("last_ver", "", lastv, sizeof lastv);
    if (strcmp(lastv, ad->version) != 0) {
        nv_config_set_str("last_ver", ad->version);
        if (lastv[0]) {  // skip the very first boot ever (nothing was "updated")
            char m[64];
            lv_snprintf(m, sizeof m, nv_tr(NV_STR_UPDATED_TO), ad->version);
            nv_notify_post(NV_NOTE_OK, "NucleoOS", m);
        }
    }

    // Screen sleep: cached timeout + 1s idle watcher (see the screen-sleep section above).
    s_sleep_s = nv_config_get_int("scr_timeout", 0);
    nv_event_subscribe(NV_EV_SETTINGS_CHANGED, on_sleep_cfg, nullptr);
    lv_timer_create(sleep_tick, 1000, nullptr);

    // USB display link (Second Screen): wake + toast on connect, auto-open on demand.
    nv_event_subscribe(NV_EV_USB_DISPLAY, on_usb_display, nullptr);

    // Lock on startup: raise the lock immediately when enabled AND a PIN exists (a boot lock with
    // no PIN would be a pointless swipe-away, so require the PIN).
    if (nv_config_get_bool("lock_boot", false) && nv_config_get_bool("lock_en", false)) {
        char pin[6];
        nv_config_get_str("lockpin", "", pin, sizeof pin);
        if (pin[0]) lock_show();
    }

    lvgl_port_unlock();
    NV_LOGI(TAG, "SystemUI up: %d apps + shade + gestures", nv_app_count());
}
