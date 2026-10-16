// dmd_menu — the touch menu of PinUP Popper tables (see dmd_menu.h, docs/DMD_POPPER.md).
//
// Data comes from tools/popper_bridge over the board's USB port (nv_dmd_bridge_*): lists of tables, a
// table's card, wheel / backglass / instruction images already sized by the bridge (converted once on the
// PC and cached there; wheels with their alpha plane). One image request is in flight at a time, the one
// the screen needs most first. The menu only shows while no table runs: when one starts (launched from
// here or from Popper), the bridge hands the port to dmdext and the app shows the DMD.
//
// Layout (Popper's): the selected table fills the screen — its backglass behind everything (enlarged and
// darkened here, on the board: the PC sends it at half size), its wheel large, its name, the facts and
// high scores, PLAY. Along the bottom the wheel bar: drag it, the wheel in the middle is the selected one,
// it goes round; a tap on a wheel brings it to the middle. A swipe on the stage moves one table (through
// nv_gesture_bind: the system edge gestures keep working). Idle, the bar turns by itself (attract mode).
//
// Two looks made for a cabinet (dmd_menu_set_theme):
//   CABINET — PinUP Popper's own default theme, as on the cabinet ("Cyan Dusk"): the wheel bar between two
//     chrome rails with a cyan light tube, the selected wheel inside a glass orb with a chrome ring (a lit
//     playfield insert), PLAY a lit cyan button; white type, nothing else coloured.
//   DMD — a dot-matrix display: titles, facts, lists and buttons drawn as dots in ColorDMD colours (plasma
//     orange titles, cyan, amber, magenta), the backglass seen through an LED grid, square bezels.
// No image transforms anywhere (the side wheels are shrunk and dimmed once, here): scaled images came out
// as noise on the board (cause not found: LVGL's PPA unit is off), and plain copies are cheaper anyway.
#include "dmd_menu.h"
#include "gallery_jpeg_hw.h"   // the P4's JPEG decoder: the bridge sends opaque pictures as JPEG

#include "dmd_core.h"   // the dot renderer

#include "nv_config.h"
#include "nv_dmd.h"
#include "nv_fonts.h"
#include "nv_gesture.h"
#include "nv_i18n.h"
#include "nv_log.h"
#include "nv_mem_attr.h"
#include "nv_notify.h"
#include "nv_ui.h"
#include "nv_ui_focus.h"
#include "nv_ui_scale.h"

#include "cJSON.h"
#include "miniz.h"   // ESP ROM tinfl: the bridge sends images zlib-compressed
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <sys/stat.h>

// The microSD cache: every picture as it came from the bridge (already compressed), and the last lists.
#ifndef DMD_CACHE_DIR
#define DMD_CACHE_DIR "/sdcard/data/dmd"
#endif

extern "C" void lv_image_cache_drop(const void *src);   // LVGL 9 (not in the public header), as apps_app.cpp
extern "C" const lv_image_dsc_t dmd_orb;                // dmd_orb.c (the cabinet theme's glass orb)

namespace {

// ---- layout (1024 x 600) ----
constexpr int kScrW = 1024, kScrH = 600;
constexpr int kTopH = 64;                       // lists, count, settings
constexpr int kBarY = 432, kBarH = kScrH - kBarY;   // the wheel bar
constexpr int kSlotStep = 148;                  // wheel bar: distance between wheels
constexpr int kSlots = 9;                       // wheels drawn: the selected one and 4 each side
constexpr int kBarImg = 136;                    // the middle wheel, px
constexpr int kSideImg = 96;                    // the others (shrunk and dimmed here, once)
constexpr int kBigImg = 280;                    // the stage's wheel
constexpr int kStageX = 36;                     // its left edge; the facts start right of it
constexpr int kColX = kStageX + kBigImg + 36;
constexpr int kColW = kScrW - kColX - 28;
constexpr int kBgW = 512, kBgH = 300;           // backglass as the bridge sends it (half the screen)
constexpr int kCacheN = 72;
// The instructions: Popper's cards are portrait pages saved on their side (for a rotated screen). The bridge
// turns them upright and fits them to the screen's width: a tall page, scrolled with a finger.
constexpr int kHelpH = 1824;
constexpr int kMaxItems = 1000;
constexpr int64_t kReqTimeoutUs = 4000000;      // an image request the bridge never answered
constexpr int64_t kRestUs = 280000;             // selection at rest this long: its card and backglass

const char *const kLists[] = {"recent", "new", "top", "fav", "all"};
constexpr int kNLists = 5;

constexpr int64_t kAttractUs = 120000000;       // no touch for 2 minutes: the attract mode
constexpr int64_t kAttractStepUs = 8000000;     // a table every 8 s
constexpr int64_t kLaunchTimeoutUs = 25000000;  // Play pressed, no game start from Popper: tell why

lv_color_t hx(uint32_t c) { return lv_color_hex(c); }

// ---- the two looks ----
struct Look {
    bool dots;                                  // titles, facts and labels as DMD dots
    uint32_t accent;                            // selection: tab, orb / marker
    uint32_t title, meta, stats, scores, fav, dim;
    uint32_t play, play2, play_ink;             // the main button (a gradient play -> play2)
    uint32_t second;                            // the other buttons
    uint32_t tab_on, tab_off;
    int bg_top, bg_bottom;                      // backglass brightness /256 at the top and at the bar
};
const Look kCabinet = {false, 0x2FE0FF, 0xFFFFFF, 0x2FE0FF, 0xC9D4DC, 0x9FB3BF, 0xFF5FA8, 0x7F929E,
                       0x5CEBFF, 0x0090B0, 0x001418, 0xC9D4DC, 0xFFFFFF, 0x8FA3AF, 92, 24};
const Look kDmdLook = {true, 0xFF6A00, 0xFF6A00, 0x18C8FF, 0xFFC400, 0xFF4FB0, 0xFF4FB0, 0x8A4A10,
                       0xFF6A00, 0xFF6A00, 0x000000, 0x18C8FF, 0xFFB000, 0x7A4E00, 150, 40};
NV_PSRAM_BSS int s_theme;                       // 0 cabinet, 1 DMD
const Look &L(void) { return s_theme ? kDmdLook : kCabinet; }

// For the dot font (ASCII only): accented Latin-1 letters lose the accent ("NOVITA", "PIU"), the other
// non-ASCII characters become spaces. Capitals.
void dot_fold(char *dst, size_t n, const char *src) {
    static const char kMap[64 + 1] =   // UTF-8 C3 80..BF -> ASCII
        "AAAAAAACEEEEIIII" "DNOOOOO OUUUUY S" "aaaaaaaceeeeiiii" "dnooooo ouuuuy y";
    size_t o = 0;
    for (const unsigned char *p = (const unsigned char *)src; *p && o + 1 < n; p++) {
        unsigned char c = *p;
        if (c == 0xC3 && p[1] >= 0x80 && p[1] <= 0xBF) c = (unsigned char)kMap[*++p - 0x80];
        else if (c >= 0x80) {   // another multi-byte character: one space for all of it
            while ((p[1] & 0xC0) == 0x80) p++;
            c = ' ';
        }
        if (c >= 'a' && c <= 'z') c = (unsigned char)(c - 32);
        dst[o++] = (char)c;
    }
    dst[o] = 0;
}

// The name to show: Popper's display names often carry "(Manufacturer Year)", MAME's region and date,
// "[...]" tags; they go (the facts are shown on their own). '_' becomes a space. Never empty.
void clean_name(char *dst, size_t n, const char *src) {
    size_t o = 0;
    for (const char *p = src; *p && o + 1 < n; p++) {
        if (*p == '(' || *p == '[') break;
        dst[o++] = *p == '_' ? ' ' : *p;
    }
    while (o && (dst[o - 1] == ' ' || dst[o - 1] == '-' || dst[o - 1] == ',')) o--;
    dst[o] = 0;
    if (!o) snprintf(dst, n, "%s", src);
}

// ------------------------------------------------------------------------------------------- dot text

// A text drawn as DMD dots (5x7 glyphs, round dots, the unlit ones faintly visible), as an RGB565 image.
struct DotText {
    uint16_t *px;
    lv_image_dsc_t dsc;
};

void dot_free(DotText &d) {
    if (!d.px) return;
    lv_image_cache_drop(&d.dsc);
    heap_caps_free(d.px);
    d = DotText{};
}

// False when the text does not fit max_w x max_h with dots of at least 2 px. bg: the colour around the
// dots (black, or a pressed button's fill).
bool dot_text(DotText &d, const char *text, uint32_t color, int max_w, int max_h, int max_pitch = 6, uint32_t bg = 0) {
    dot_free(d);
    char t[64];
    dot_fold(t, sizeof t, text);
    int lines = 1, len = 0, longest = 0;
    for (const char *q = t; *q; q++) {
        if (*q == '\n') { lines++; len = 0; continue; }
        if (++len > longest) longest = len;
    }
    if (!longest) return false;
    const int cols = longest * 6 - 1 + 2, rows = lines * 8 - 1 + 2;   // one unlit dot all around
    int pitch = max_w / cols;
    if (max_h / rows < pitch) pitch = max_h / rows;
    if (pitch > max_pitch) pitch = max_pitch;
    if (pitch < 2 || cols > DMD_MAX_W) return false;
    // geometry and masks (~18 KB) only for the length of the call
    uint32_t *src = (uint32_t *)heap_caps_malloc((size_t)cols * rows * 4, MALLOC_CAP_SPIRAM);
    dmd_geom_t *g = (dmd_geom_t *)heap_caps_malloc(sizeof(dmd_geom_t), MALLOC_CAP_SPIRAM);
    dmd_masks_t *mk = (dmd_masks_t *)heap_caps_malloc(sizeof(dmd_masks_t), MALLOC_CAP_SPIRAM);
    if (!src || !g || !mk) {
        heap_caps_free(src);
        heap_caps_free(g);
        heap_caps_free(mk);
        return false;
    }
    dmd_text(src, cols, rows, t, color);
    const uint32_t dim = ((color >> 16 & 0xFF) / 9) << 16 | ((color >> 8 & 0xFF) / 9) << 8 | (color & 0xFF) / 9;
    for (int i = 0; i < cols * rows; i++)
        if (!src[i]) src[i] = dim;   // unlit dots: a ninth of the colour
    const int w = cols * pitch, h = rows * pitch;
    bool ok = dmd_geom(cols, rows, 0, DMD_FIT_SQUARE, w, h, g);
    if (ok) {
        dmd_masks_build(mk, g, DMD_SHAPE_ROUND, !bg);
        d.px = (uint16_t *)heap_caps_aligned_alloc(64, (size_t)w * h * 2, MALLOC_CAP_SPIRAM);
        ok = d.px != nullptr;
    }
    if (ok) {
        dmd_render(d.px, w, g, mk, src, cols, rows, 0, 100, true, w);
        if (bg) {   // the gaps between the dots: the button's fill, not black
            const uint16_t b565 = (uint16_t)((bg >> 19 & 31) << 11 | (bg >> 10 & 63) << 5 | (bg >> 3 & 31));
            for (int i = 0; i < w * h; i++)
                if (!d.px[i]) d.px[i] = b565;
        }
        d.dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
        d.dsc.header.cf = LV_COLOR_FORMAT_RGB565;
        d.dsc.header.w = (uint32_t)w;
        d.dsc.header.h = (uint32_t)h;
        d.dsc.header.stride = (uint32_t)w * 2;
        d.dsc.data_size = (uint32_t)w * h * 2;
        d.dsc.data = (const uint8_t *)d.px;
    }
    heap_caps_free(src);
    heap_caps_free(g);
    heap_caps_free(mk);
    return ok;
}

bool dot_text_wrap(DotText &d, const char *text, uint32_t color, int w, int h, int max_pitch) {
    if (dot_text(d, text, color, w, h / 2 + 2, max_pitch)) return true;
    char two[64];
    snprintf(two, sizeof two, "%s", text);
    const int n = (int)strlen(two);
    int best = -1;
    for (int i = 0; i < n; i++)
        if (two[i] == ' ' && (best < 0 || abs(i - n / 2) < abs(best - n / 2))) best = i;
    if (best < 0) return false;
    two[best] = '\n';
    return dot_text(d, two, color, w, h, max_pitch);
}

// ------------------------------------------------------------------------------------------- state

struct Img {
    int id, w, h;                 // id 0 = empty slot
    char kind;                    // 'w' wheel, 'v' side wheel (made here), 'h' help, 'b' backglass,
                                  // 'g' game art source, 'm' showcase picture, 'f' FullDMD art
    int8_t sub;                   // 'm': which screen (index in kShowScreens)
    bool alpha;                   // RGB565A8: the alpha plane follows the pixels
    uint8_t *px;                  // 64-byte aligned (PSRAM)
    lv_image_dsc_t dsc;
    uint32_t used;                // LRU stamp
    bool has_win;                 // 'f' / 'r': the DMD's window (housing) was found in it
    dmd_rect_t win;
    char key[12];                 // the bridge's key for these bytes (source file + size + options)
};

struct Item {
    int id, year, plays, ago, secs, emu;
    bool fav;
    char name[56];
    char maker[28];
    char added[12];
};

// A button: its object, and (DMD look) its label as dots, plain and on the pressed fill.
struct Btn {
    lv_obj_t *obj, *img;
    DotText dot, dot_on;
};

NV_PSRAM_BSS Img s_cache[kCacheN];
NV_PSRAM_BSS int16_t s_rgrill[4];   // the last backglass-with-grill picture's grill: x, y, w, h
NV_PSRAM_BSS int s_rgrill_id;       // and whose (img_apply)
NV_PSRAM_BSS int16_t s_fart[2];     // the last FullDMD art's true size (the picture is stretched to 1024x600)
NV_PSRAM_BSS int s_fart_id;
NV_PSRAM_BSS int16_t s_s2[4];       // the PC's second screen (the backglass monitor): x, y, w, h (w 0: none)
NV_PSRAM_BSS uint32_t s_stamp;
NV_PSRAM_BSS Item *s_items;                   // the list on screen: s_all through the system filter (PSRAM)
NV_PSRAM_BSS int s_nitems;
NV_PSRAM_BSS Item *s_all;                     // the list as the bridge sent it (kMaxItems, PSRAM)
NV_PSRAM_BSS int s_nall;
struct Emu { int id; char name[24]; };
NV_PSRAM_BSS Emu s_emus[16];                  // Popper's systems (from the hello)
NV_PSRAM_BSS int s_nemus;
NV_PSRAM_BSS int s_filter;                    // the system shown (EMUID), 0 = all
const char *emu_name(int id);
NV_PSRAM_BSS lv_obj_t *s_find;                // "go to": systems and letters
NV_PSRAM_BSS lv_obj_t *s_find_card;
NV_PSRAM_BSS lv_obj_t *s_launch_bg, *s_launch_wheel, *s_launch_time;   // the "in game" / "starting" page
NV_PSRAM_BSS int s_drag_vx;                   // wheel bar: the finger's speed, px per 10 ms (fling)
NV_PSRAM_BSS int s_drag_lx;
NV_PSRAM_BSS int64_t s_drag_lus;
NV_PSRAM_BSS int s_list;                      // index in kLists
NV_PSRAM_BSS int s_sel;                       // selected item (the middle of the wheel bar)
NV_PSRAM_BSS int s_restore_id;                // table to select when the list comes (state restore)
NV_PSRAM_BSS int64_t s_sel_us;                // when the selection last changed
NV_PSRAM_BSS bool s_hello;                    // the bridge said hello since the menu was built
NV_PSRAM_BSS bool s_want_full;                // "Fit" is FullDMD: the bridge sends that art as a game starts
NV_PSRAM_BSS bool s_want_list;
NV_PSRAM_BSS bool s_visible;

// what the stage shows (the selected item, or the table "surprise me" picked)
NV_PSRAM_BSS int s_card_id;
NV_PSRAM_BSS bool s_card_asked;
NV_PSRAM_BSS char s_card_name[64];
NV_PSRAM_BSS bool s_random_pending;
NV_PSRAM_BSS bool s_want_help;
NV_PSRAM_BSS int s_bg_id;                     // backglass behind the stage (0: none)

// image requests
NV_PSRAM_BSS int s_wait_id;                   // in flight (0: none)
NV_PSRAM_BSS int64_t s_wait_us;
constexpr int kMissingN = 4096;   // wheels and backglasses of a long list ("Tutti": hundreds of MAME games)
NV_PSRAM_BSS uint32_t *s_missing;             // kMissingN (id << 2 | kind) the bridge has no image for (heap, PSRAM)
NV_PSRAM_BSS int s_nmissing;

NV_PSRAM_BSS char s_game[64];                 // the running table's name ("" = none)
NV_PSRAM_BSS int s_game_id;                   // and its id (0: unknown)
NV_PSRAM_BSS bool s_game_dmd;                 // its DMD goes through dmdext (a VPX table)
// The running table's backglass (backbox: above the DMD; or the whole screen), made here from the
// half-size image the bridge sends just before it hands the port to dmdext. The source stays in the
// cache, the picture is rebuilt for the height asked.
constexpr int kArtW = 1024, kArtSrcW = 512, kArtSrcH = 300;
NV_PSRAM_BSS uint16_t *s_artpx;
// During a game the USB port is dmdext's: the board talks to Popper's Web Remote over Wi-Fi instead
// (the bridge says where in its hello). Quit = pupEvent 15 (fEmuExit), tested on the cabinet.
NV_PSRAM_BSS char s_pc_ip[40];
NV_PSRAM_BSS int s_pc_web;
NV_PSRAM_BSS volatile int s_http_result;      // 0 idle / pending, 1 ok, -1 failed (set by the request task)
NV_PSRAM_BSS volatile bool s_http_busy;
NV_PSRAM_BSS char s_http_path[64];
NV_PSRAM_BSS int s_pending_launch;            // table to start once the running one has ended
// A quit asked (quit_request): Popper ignores one while a heavy table is still loading (seen: TMNT ~55 s
// after its launch), so it is asked once more after kQuitRetryUs if the game is still running.
NV_PSRAM_BSS int64_t s_quit_us;
NV_PSRAM_BSS int s_quit_n;
NV_PSRAM_BSS int64_t s_pending_us;
NV_PSRAM_BSS bool s_browse;                   // the tables browsed while a game runs (the DMD steps aside)
NV_PSRAM_BSS lv_obj_t *s_quick;               // in-game quick actions (a tap on the DMD)
NV_PSRAM_BSS lv_obj_t *s_quick_title, *s_quick_dots;
NV_PSRAM_BSS DotText s_dot_quick;
NV_PSRAM_BSS lv_obj_t *s_confirm;             // "X is running: quit it and start Y?"
NV_PSRAM_BSS lv_obj_t *s_confirm_txt;
NV_PSRAM_BSS lv_obj_t *s_resume;              // header: back to the DMD (browsing during a game)
NV_PSRAM_BSS lv_obj_t *s_launch_btns;         // in-game page: tables / quit
NV_PSRAM_BSS Btn s_resume_b;                 // header: back to the DMD (browsing during a game)
NV_PSRAM_BSS Btn s_q_resume, s_q_tables, s_q_quit, s_q_settings, s_c_ok, s_c_cancel, s_l_tables, s_l_quit;
// "Fit" for the running table only (dmd_app keeps the choice per table): a button in the quick actions
// opens a list, "as every table" then the panel's five fits
constexpr int kFitChoices = 10;   // "as every table" + the panel's nine
struct FitUi {
    Btn q, b[kFitChoices];
    Btn rb[2];                        // a 128x16 DMD: rebuilt / as it comes (only for those)
    lv_obj_t *tip;                    // "Suggested: ..."
};
NV_PSRAM_BSS FitUi *s_fitui;   // on the heap while the menu exists (static PSRAM is nearly full)
NV_PSRAM_BSS lv_obj_t *s_fitm;
NV_PSRAM_BSS lv_image_dsc_t s_art_dsc;
NV_PSRAM_BSS int s_art_id, s_art_h;
NV_PSRAM_BSS int64_t s_game_us;
NV_PSRAM_BSS void (*s_open_settings)(void);
NV_PSRAM_BSS lv_obj_t *s_root;

NV_PSRAM_BSS DotText s_dot_title, s_dot_name, s_dot_meta, s_dot_st1, s_dot_st2, s_dot_fav, s_dot_count;
NV_PSRAM_BSS DotText s_dot_launch, s_dot_launch_txt, s_dot_attract, s_dot_time;
NV_PSRAM_BSS DotText s_dot_find[40];          // DMD look: the "go to" keys
NV_PSRAM_BSS int s_nfind;
NV_PSRAM_BSS DotText s_dot_tab[kNLists + 1][2];   // DMD look: each tab off / on

NV_PSRAM_BSS lv_obj_t *s_menu;                // the whole menu (hidden while not allowed)
NV_PSRAM_BSS lv_obj_t *s_bg;                  // backglass, full screen, darkened
NV_PSRAM_BSS uint16_t *s_bgpx;                // its pixels (1024x600 RGB565, PSRAM)
NV_PSRAM_BSS lv_image_dsc_t s_bg_dsc;
NV_PSRAM_BSS lv_obj_t *s_tabs;
NV_PSRAM_BSS lv_obj_t *s_count;               // "12 / 75" (label, or dot image)
NV_PSRAM_BSS lv_obj_t *s_note;                // "loading" / "empty"
NV_PSRAM_BSS lv_obj_t *s_stage;
NV_PSRAM_BSS lv_obj_t *s_stage_img;
NV_PSRAM_BSS lv_obj_t *s_stage_title;         // label (cabinet look, or a name too long for dots)
NV_PSRAM_BSS lv_obj_t *s_stage_dots;          // the name as DMD dots
NV_PSRAM_BSS lv_obj_t *s_stage_meta;          // year - maker (a line: dots or label)
NV_PSRAM_BSS lv_obj_t *s_stage_fav;
NV_PSRAM_BSS lv_obj_t *s_stage_st1, *s_stage_st2;   // plays / last played (lines)
NV_PSRAM_BSS lv_obj_t *s_stage_scores;
NV_PSRAM_BSS Btn s_play, s_helpb, s_another, s_back;
NV_PSRAM_BSS lv_obj_t *s_bar;
NV_PSRAM_BSS lv_obj_t *s_slot[kSlots];
NV_PSRAM_BSS lv_obj_t *s_slot_ph[kSlots];     // a table without a wheel: its name in the slot
NV_PSRAM_BSS lv_obj_t *s_stage_ph;            // and on the stage
NV_PSRAM_BSS int s_slot_id[kSlots];           // table shown in each slot (0: none)
NV_PSRAM_BSS bool s_slot_big[kSlots];         // showing the middle (large) wheel
NV_PSRAM_BSS int s_drag;                      // wheel bar offset while dragged / animated, px
NV_PSRAM_BSS int s_press_x;
NV_PSRAM_BSS bool s_dragging;
NV_PSRAM_BSS bool s_animating;
NV_PSRAM_BSS int s_anim_n;                    // tables the running animation moves
NV_PSRAM_BSS int s_anim_id;                   // and the table it goes to (a list may change meanwhile)
NV_PSRAM_BSS lv_obj_t *s_help;                // the instruction card, full screen
NV_PSRAM_BSS lv_obj_t *s_help_img;
NV_PSRAM_BSS lv_obj_t *s_launch;              // "starting <table>" / the game running
NV_PSRAM_BSS lv_obj_t *s_launch_dots, *s_launch_name, *s_launch_txt;
NV_PSRAM_BSS bool s_launching;                // Play sent, no game start yet
NV_PSRAM_BSS int64_t s_launch_us;
NV_PSRAM_BSS bool s_attract;                  // idle: the bar turns by itself
NV_PSRAM_BSS lv_obj_t *s_attract_txt;
NV_PSRAM_BSS int64_t s_attract_us;
NV_PSRAM_BSS int64_t s_last_touch_us;
NV_PSRAM_BSS uint32_t s_ticks;
NV_PSRAM_BSS int s_touch_marker;              // user data of our input-device callbacks

int wrap(int i) { return s_nitems ? ((i % s_nitems) + s_nitems) % s_nitems : 0; }

// ------------------------------------------------------------------------------------------- images

// What the bridge has no picture for: the table, the kind, and for the showcase the screen. Each kind its own
// code: a table without a game backglass or a showcase screen keeps its wheel ('v' is made from 'w').
uint32_t miss_key(int id, char kind, int sub) {
    const uint32_t c = kind == 'b' ? 1 : kind == 'h' ? 2 : kind == 'g' ? 3 : kind == 'f' ? 4 : kind == 'r' ? 5
                     : kind == 'm' ? 8 + (uint32_t)(sub < 0 ? 0 : sub) : 0;
    return (uint32_t)id << 4 | c;
}
bool is_missing(int id, char kind, int sub = -1) {
    const uint32_t k = miss_key(id, kind, sub);
    if (!s_missing) return false;
    for (int i = 0; i < s_nmissing && i < kMissingN; i++)
        if (s_missing[i] == k) return true;
    return false;
}
void set_missing(int id, char kind, int sub = -1) {
    if (!id || !s_missing || is_missing(id, kind, sub)) return;
    s_missing[s_nmissing++ % kMissingN] = miss_key(id, kind, sub);
}

NV_PSRAM_BSS Img *s_zombie;                   // the copy a fresher picture replaces, until the screen moved on
bool s_store_nomem;                           // img_store failed for memory, not for the bytes (internal RAM)

Img *img_find(int id, char kind, int w) {
    for (Img &m : s_cache)   // (instructions: one per table, the bridge picks their width)
        if (m.id == id && m.kind == kind && (m.w == w || kind == 'h')) { m.used = ++s_stamp; return &m; }
    return nullptr;
}

void img_drop(Img &m) {
    if (!m.id) return;
    lv_image_cache_drop(&m.dsc);   // LVGL caches decoded images by source pointer
    heap_caps_free(m.px);
    m = Img{};
}

// ---- the showcase: Popper's selected table, its horizontal pictures one after the other
const char *const kShowScreens[] = {"BackGlass", "GameInfo", "GameHelp", "DMD", "Topper"};
constexpr int kShowN = 5;
constexpr int64_t kShowStepUs = 7000000;      // a picture every 7 s
NV_PSRAM_BSS int s_show_id;                   // its table (0: the showcase is off)
NV_PSRAM_BSS char s_show_name[64];
NV_PSRAM_BSS int8_t s_show_list[kShowN];      // the screens it has, in order
NV_PSRAM_BSS int s_show_n, s_show_at;         // how many; the one on screen (-1: none yet)
NV_PSRAM_BSS int64_t s_show_us;               // when it came
NV_PSRAM_BSS lv_obj_t *s_show;                // the view
NV_PSRAM_BSS lv_obj_t *s_show_img[2];         // the picture on screen and the one sliding in
NV_PSRAM_BSS int s_show_top;                  // which of the two is on top
NV_PSRAM_BSS lv_obj_t *s_show_name_lbl, *s_show_name_dots, *s_show_kind, *s_show_pips, *s_show_hint;
NV_PSRAM_BSS bool s_show_game;                // the showcase of the game being played (no DMD for it)
NV_PSRAM_BSS DotText s_dot_show;
// Popper's selection on something with no showcase: an entry that is not a table (Go Back, Exit, a
// playlist) or a table without horizontal pictures. A card says what it is, instead of the last table's
// pictures staying on screen.
lv_obj_t *s_nav, *s_nav_icon, *s_nav_sym, *s_nav_wheel, *s_nav_dots, *s_nav_lbl, *s_nav_sub;
DotText s_dot_nav;
int s_nav_id;                                 // the table on the card (0: a menu entry)

Img *show_find(int id, int sub) {
    for (Img &m : s_cache)
        if (m.id == id && m.kind == 'm' && m.sub == sub) { m.used = ++s_stamp; return &m; }
    return nullptr;
}

// On screen right now: never given away (an lv_image would draw freed memory).
bool img_in_use(const Img &m) {
    if (m.kind == 'h') return s_help && !lv_obj_has_flag(s_help, LV_OBJ_FLAG_HIDDEN);
    if (m.kind == 'g' || m.kind == 'f' || m.kind == 'r') return m.id == s_game_id;   // the running game's art
    if (m.kind == 'm') return m.id == s_show_id;   // the showcase's pictures
    if (m.kind == 'w' && m.w == kBigImg) return m.id == s_card_id || (m.id == s_nav_id && s_nav_id);
    if (m.kind == 'w' || m.kind == 'v')
        for (int i = 0; i < kSlots; i++)
            if (s_slot_id[i] == m.id) return true;
    return false;   // backglass: copied into s_bgpx
}

// The pictures together stay under this (PSRAM): a 1024x600 showcase picture is 1.2 MB once decoded, and a
// limit on the count alone let a few dozen of them eat the memory (screenshots failing, then pictures).
constexpr size_t kCacheBytes = 8u << 20;

size_t img_bytes(const Img &m) { return m.id ? (size_t)m.w * (size_t)m.h * (m.alpha ? 3 : 2) : 0; }

// The least recently used picture that can go (not on screen, not the running game's art), or none.
Img *img_lru(void) {
    Img *lru = nullptr;
    for (Img &m : s_cache)
        if (m.id > 0 && !img_in_use(m) && (!lru || m.used < lru->used)) lru = &m;
    return lru;
}

// Room for `need` more bytes under kCacheBytes: the least recently used pictures go first.
void img_room(size_t need) {
    for (;;) {
        size_t tot = 0;
        for (const Img &m : s_cache) tot += img_bytes(m);
        Img *lru = tot + need > kCacheBytes ? img_lru() : nullptr;
        if (!lru) return;
        img_drop(*lru);
    }
}

// Pixels for a picture; when the memory is short, older pictures make way.
uint8_t *img_alloc(size_t bytes) {
    for (;;) {
        if (void *px = heap_caps_aligned_alloc(64, bytes, MALLOC_CAP_SPIRAM)) return (uint8_t *)px;
        Img *lru = img_lru();
        if (!lru) return nullptr;
        img_drop(*lru);
    }
}

Img *img_slot(void) {   // a free slot, else the least recently used one not on screen
    Img *slot = nullptr;
    for (Img &m : s_cache) {
        if (!m.id) return &m;
        if (img_in_use(m)) continue;
        if (!slot || m.used < slot->used) slot = &m;
    }
    if (slot) img_drop(*slot);
    return slot;
}

void img_describe(Img &m) {
    m.used = ++s_stamp;
    m.dsc = lv_image_dsc_t{};
    m.dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
    m.dsc.header.cf = m.alpha ? LV_COLOR_FORMAT_RGB565A8 : LV_COLOR_FORMAT_RGB565;
    m.dsc.header.w = (uint32_t)m.w;
    m.dsc.header.h = (uint32_t)m.h;
    m.dsc.header.stride = (uint32_t)m.w * 2;   // RGB565A8: the colour plane's stride; the alpha plane follows
    m.dsc.data_size = (uint32_t)(m.w * m.h * (m.alpha ? 3 : 2));
    m.dsc.data = m.px;
}

// An 'I' message: u16 LE json_len, {"id","kind","w","h","fmt"}, then the pixels: zlib when fmt ends in
// "z"; "rgb565a8" = RGB565 then one alpha byte per pixel.
Img *img_store(const uint8_t *p, uint32_t n) {
    if (n < 2) return nullptr;
    const uint32_t jl = p[0] | p[1] << 8;
    if (jl > n - 2 || jl > 512) return nullptr;
    char js[513];
    memcpy(js, p + 2, jl);
    js[jl] = 0;
    cJSON *o = cJSON_Parse(js);
    if (!o) return nullptr;
    const int id = cJSON_GetObjectItem(o, "id") ? cJSON_GetObjectItem(o, "id")->valueint : 0;
    const int w = cJSON_GetObjectItem(o, "w") ? cJSON_GetObjectItem(o, "w")->valueint : 0;
    const int h = cJSON_GetObjectItem(o, "h") ? cJSON_GetObjectItem(o, "h")->valueint : 0;
    const cJSON *k = cJSON_GetObjectItem(o, "kind");
    const char *ks = cJSON_IsString(k) ? k->valuestring : "";
    const char kind = !strcmp(ks, "help")     ? 'h'
                      : !strcmp(ks, "backglass") ? 'b'
                      : !strcmp(ks, "gamebg")    ? 'g'
                      : !strcmp(ks, "media")     ? 'm'
                      : !strcmp(ks, "fulldmd")   ? 'f'
                      : !strcmp(ks, "b2sgrill")  ? 'r'
                                                 : 'w';
    int sub = -1;
    if (kind == 'm') {
        const cJSON *sc = cJSON_GetObjectItem(o, "screen");
        for (int i = 0; cJSON_IsString(sc) && i < kShowN; i++)
            if (!strcmp(sc->valuestring, kShowScreens[i])) sub = i;
    }
    if (kind == 'f') {   // its true shape: read by img_apply right after (0: an older bridge, 16:9 then)
        s_fart[0] = cJSON_IsNumber(cJSON_GetObjectItem(o, "aw")) ? (int16_t)cJSON_GetObjectItem(o, "aw")->valueint : 0;
        s_fart[1] = cJSON_IsNumber(cJSON_GetObjectItem(o, "ah")) ? (int16_t)cJSON_GetObjectItem(o, "ah")->valueint : 0;
    }
    if (kind == 'r') {   // where its grill is: read by img_apply right after (one such picture: the game's)
        const char *gk[4] = {"gx", "gy", "gw", "gh"};
        for (int i = 0; i < 4; i++)
            s_rgrill[i] = cJSON_IsNumber(cJSON_GetObjectItem(o, gk[i])) ? (int16_t)cJSON_GetObjectItem(o, gk[i])->valueint : 0;
    }
    const cJSON *f = cJSON_GetObjectItem(o, "fmt");
    char fs[16], key[12];
    snprintf(fs, sizeof fs, "%s", cJSON_IsString(f) ? f->valuestring : "rgb565");
    snprintf(key, sizeof key, "%s", cJSON_IsString(cJSON_GetObjectItem(o, "k")) ? cJSON_GetObjectItem(o, "k")->valuestring : "");
    cJSON_Delete(o);
    const bool alpha = !strncmp(fs, "rgb565a8", 8);
    const bool z = fs[0] && fs[strlen(fs) - 1] == 'z';
    const bool jpg = !strcmp(fs, "jpeg");
    const size_t bytes = (size_t)w * (size_t)h * (alpha ? 3 : 2);
    s_store_nomem = false;
    if (!id || w < 1 || h < 1 || w > 1024 || h > (kind == 'h' ? kHelpH : 600) || (!z && !jpg && n - 2 - jl < bytes))
        return nullptr;
    if (kind == 'm' && sub < 0) return nullptr;
    if (Img *old = kind == 'm' ? show_find(id, sub) : img_find(id, kind, w)) {
        if (!strcmp(old->key, key) && *key) { old->used = ++s_stamp; return old; }   // the same bytes
        if (img_in_use(*old)) {   // on screen: hidden from look-ups, dropped once the screen has the new one
            old->id = -old->id;
            old->used = ++s_stamp;
            s_zombie = old;
        } else {
            img_drop(*old);
        }
    }
    img_room(bytes);
    Img *slot = img_slot();
    if (!slot) { s_store_nomem = true; return nullptr; }
    if (jpg) {   // decoded by the hardware (~10 ms for 1024x600); its rows are whole 16-pixel blocks wide
        gallery_raster_t r = {};
        if (!gallery_jpeg_hw_decode_mem(p + 2 + jl, n - 2 - jl, &r) || r.w != w || r.h != h) {
            gallery_jpeg_hw_free(&r);
            // the decoder needs its output buffer in one piece: short of that it is memory, not the file
            s_store_nomem = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM) < bytes + (64u << 10);
            return nullptr;
        }
        if (r.pitch == w) {
            slot->px = r.px;   // the decoder's buffer is the picture
        } else if ((slot->px = img_alloc(bytes))) {
            for (int y = 0; y < h; y++) memcpy(slot->px + (size_t)y * w * 2, r.px + (size_t)y * r.pitch * 2, (size_t)w * 2);
            gallery_jpeg_hw_free(&r);
        } else {
            gallery_jpeg_hw_free(&r);
            s_store_nomem = true;
            return nullptr;
        }
    } else if (!(slot->px = img_alloc(bytes))) {
        s_store_nomem = true;
        return nullptr;
    } else if (z) {
        tinfl_decompressor *d = (tinfl_decompressor *)heap_caps_malloc(sizeof(tinfl_decompressor), MALLOC_CAP_SPIRAM);
        size_t in_sz = n - 2 - jl, out_sz = bytes;
        tinfl_status st = TINFL_STATUS_FAILED;
        if (d) {
            tinfl_init(d);
            st = tinfl_decompress(d, p + 2 + jl, &in_sz, slot->px, slot->px, &out_sz,
                                  TINFL_FLAG_PARSE_ZLIB_HEADER | TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF);
            heap_caps_free(d);
        }
        if (st != TINFL_STATUS_DONE || out_sz != bytes) {
            heap_caps_free(slot->px);
            *slot = Img{};
            return nullptr;
        }
    } else {
        memcpy(slot->px, p + 2 + jl, bytes);
    }
    slot->id = id;
    slot->kind = kind;
    slot->sub = (int8_t)sub;
    slot->w = w;
    slot->h = h;
    slot->alpha = alpha;
    snprintf(slot->key, sizeof slot->key, "%s", key);
    img_describe(*slot);
    return slot;
}

// A side wheel: the middle one shrunk to kSideImg (bilinear, colour and alpha) and dimmed to 55%, made
// once here instead of scaling on every frame.
Img *side_make(const Img &src) {
    if (Img *m = img_find(src.id, 'v', kSideImg)) return m;
    const int sw = src.w, sh = src.h, dw = kSideImg, dh = kSideImg * src.h / src.w;
    if (dh < 1) return nullptr;
    const Img copy = src;   // img_slot() may move things around; src stays (it is on screen)
    Img *slot = img_slot();
    if (!slot) return nullptr;
    const size_t n = (size_t)dw * dh;
    slot->px = (uint8_t *)heap_caps_aligned_alloc(64, n * (copy.alpha ? 3 : 2), MALLOC_CAP_SPIRAM);
    if (!slot->px) return nullptr;
    const uint16_t *sp = (const uint16_t *)copy.px;
    const uint8_t *sa = copy.alpha ? copy.px + (size_t)sw * sh * 2 : nullptr;
    uint16_t *dp = (uint16_t *)slot->px;
    uint8_t *da = copy.alpha ? slot->px + n * 2 : nullptr;
    for (int y = 0; y < dh; y++) {
        const int fy = ((2 * y + 1) * sh * 128) / dh - 128;   // source y, 1/256 px
        const int y0 = fy < 0 ? 0 : fy >> 8, y1 = y0 + 1 < sh ? y0 + 1 : y0, wy = fy < 0 ? 0 : fy & 255;
        for (int x = 0; x < dw; x++) {
            const int fx = ((2 * x + 1) * sw * 128) / dw - 128;
            const int x0 = fx < 0 ? 0 : fx >> 8, x1 = x0 + 1 < sw ? x0 + 1 : x0, wx = fx < 0 ? 0 : fx & 255;
            const int i00 = y0 * sw + x0, i01 = y0 * sw + x1, i10 = y1 * sw + x0, i11 = y1 * sw + x1;
            const int w00 = (256 - wx) * (256 - wy), w01 = wx * (256 - wy), w10 = (256 - wx) * wy, w11 = wx * wy;
            int c[3];
            const int shf[3] = {11, 5, 0}, msk[3] = {31, 63, 31};
            for (int j = 0; j < 3; j++)
                c[j] = (((sp[i00] >> shf[j] & msk[j]) * w00 + (sp[i01] >> shf[j] & msk[j]) * w01 +
                         (sp[i10] >> shf[j] & msk[j]) * w10 + (sp[i11] >> shf[j] & msk[j]) * w11) >> 16) * 140 >> 8;
            dp[y * dw + x] = (uint16_t)(c[0] << 11 | c[1] << 5 | c[2]);
            if (da) da[y * dw + x] = (uint8_t)((sa[i00] * w00 + sa[i01] * w01 + sa[i10] * w10 + sa[i11] * w11) >> 16);
        }
    }
    slot->id = copy.id;
    slot->kind = 'v';
    slot->w = dw;
    slot->h = dh;
    slot->alpha = copy.alpha;
    img_describe(*slot);
    return slot;
}

// The backglass behind the stage: the half-size image enlarged 2x (bilinear) and darkened, more towards
// the wheel bar, so text and wheels stay readable on any artwork; the DMD look sees it through an LED
// grid (every fourth row and every other column dark). All on the board, once per table.
inline uint16_t mix565(uint16_t a, uint16_t b) { return (uint16_t)((((a ^ b) & 0xF7DE) >> 1) + (a & b)); }
inline uint16_t dim565(uint16_t c, int f) {   // f/256
    const int r = (c >> 11) * f >> 8, g = (c >> 5 & 63) * f >> 8, b = (c & 31) * f >> 8;
    return (uint16_t)(r << 11 | g << 5 | b);
}
void bg_build(const Img &m) {
    if (!s_bgpx || m.w != kBgW || m.h != kBgH || m.alpha) return;
    const Look &lk = L();
    const uint16_t *src = (const uint16_t *)m.px;
    for (int y = 0; y < kScrH; y++) {
        const int sy = y >> 1, sy1 = sy + 1 < kBgH ? sy + 1 : sy;
        const uint16_t *r0 = src + sy * kBgW, *r1 = src + sy1 * kBgW;
        const int f = lk.bg_top + (lk.bg_bottom - lk.bg_top) * y / kScrH;
        const bool grid_row = lk.dots && (y & 3) == 3;
        const int fa = grid_row ? f / 5 : f, fb = lk.dots ? f / 5 : fa;
        uint16_t *d = s_bgpx + y * kScrW;
        for (int x = 0; x < kBgW; x++) {
            const int x1 = x + 1 < kBgW ? x + 1 : x;
            uint16_t a = r0[x], b = r0[x1];
            if (y & 1) { a = mix565(a, r1[x]); b = mix565(b, r1[x1]); }
            d[2 * x] = dim565(a, fa);
            d[2 * x + 1] = dim565(mix565(a, b), fb);
        }
    }
    lv_image_cache_drop(&s_bg_dsc);
    lv_image_set_src(s_bg, &s_bg_dsc);
    if (lv_obj_has_flag(s_bg, LV_OBJ_FLAG_HIDDEN)) {   // after another table's: it comes up softly
        lv_obj_remove_flag(s_bg, LV_OBJ_FLAG_HIDDEN);
        lv_obj_fade_in(s_bg, 180, 0);
    }
    lv_obj_invalidate(s_bg);
    s_bg_id = m.id;
}

// The ambient glow at a point: the 16x6 grid of cell averages, sampled bilinearly between the cells'
// centres (a soft wash, no blocks). gx, gy in 1/256 of a cell, clamped.
uint16_t amb_at(const uint16_t g[6][16], int gx, int gy) {
    gx = gx < 0 ? 0 : gx > 15 * 256 ? 15 * 256 : gx;
    gy = gy < 0 ? 0 : gy > 5 * 256 ? 5 * 256 : gy;
    const int x0 = gx >> 8, y0 = gy >> 8, x1 = x0 < 15 ? x0 + 1 : x0, y1 = y0 < 5 ? y0 + 1 : y0;
    const int wx = gx & 255, wy = gy & 255;
    const uint16_t c[4] = {g[y0][x0], g[y0][x1], g[y1][x0], g[y1][x1]};
    const int w[4] = {(256 - wx) * (256 - wy), wx * (256 - wy), (256 - wx) * wy, wx * wy};
    int r = 0, gg = 0, b = 0;
    for (int i = 0; i < 4; i++) {
        r += (c[i] >> 11) * w[i];
        gg += (c[i] >> 5 & 63) * w[i];
        b += (c[i] & 31) * w[i];
    }
    return (uint16_t)((r >> 16) << 11 | (gg >> 16) << 5 | (b >> 16));
}

// A picture centred on black (the bridge fits it, keeping its shape): the black bars around it become an
// ambient glow of its own colours (16x6 cell averages, darkened, darker away from it). In place, once.
void ambient_bars(uint16_t *px, int w, int h) {
    int x0 = 0, x1 = w - 1, y0 = 0, y1 = h - 1;
    auto col_lit = [&](int x) { for (int y = 0; y < h; y += 2) if (px[y * w + x]) return true; return false; };
    auto row_lit = [&](int y) { for (int x = 0; x < w; x += 2) if (px[y * w + x]) return true; return false; };
    while (x0 < x1 && !col_lit(x0)) x0++;
    while (x1 > x0 && !col_lit(x1)) x1--;
    while (y0 < y1 && !row_lit(y0)) y0++;
    while (y1 > y0 && !row_lit(y1)) y1--;
    if (x0 < 4 && x1 > w - 5 && y0 < 4 && y1 > h - 5) return;   // it fills the frame
    const int cw = x1 - x0 + 1, ch = y1 - y0 + 1;
    uint16_t amb[6][16];
    for (int gy = 0; gy < 6; gy++)
        for (int gx = 0; gx < 16; gx++) {
            uint32_t r = 0, g = 0, b = 0, n = 0;
            for (int y = y0 + gy * ch / 6; y < y0 + (gy + 1) * ch / 6; y += 3)
                for (int x = x0 + gx * cw / 16; x < x0 + (gx + 1) * cw / 16; x += 3) {
                    const uint16_t c = px[y * w + x];
                    r += c >> 11; g += c >> 5 & 63; b += c & 31; n++;
                }
            amb[gy][gx] = n ? (uint16_t)((r / n) << 11 | (g / n) << 5 | (b / n)) : 0;
        }
    for (int y = 0; y < h; y++) {
        const bool row_in = y >= y0 && y <= y1;
        const int gy = (y - y0) * 6 * 256 / ch - 128;
        for (int x = 0; x < w; x++) {
            if (row_in && x >= x0 && x <= x1) continue;
            const int gx = (x - x0) * 16 * 256 / cw - 128;
            const int dist = x < x0 ? x0 - x : x > x1 ? x - x1 : y < y0 ? y0 - y : y - y1;
            const int f = 105 - dist * 75 / (w / 2);
            px[y * w + x] = dim565(amb_at(amb, gx, gy), f < 30 ? 30 : f);
        }
    }
}

// The game art: the backglass (centred in its half-size source, black bars around it) fitted to
// kArtW x h, its height filled; the sides an ambient glow of its own colours (a 16x6 average, enlarged
// and darkened), like a video wall; the last rows fade to black where the DMD sits below (backbox).
void art_build(const Img &m, int h) {
    if (m.w != kArtSrcW || m.h != kArtSrcH || m.alpha || h < 16 || h > kScrH) return;
    if (!s_artpx) s_artpx = (uint16_t *)heap_caps_aligned_alloc(64, (size_t)kArtW * kScrH * 2, MALLOC_CAP_SPIRAM);
    if (!s_artpx) return;
    const uint16_t *src = (const uint16_t *)m.px;
    // the picture inside the source: its columns / rows with anything lit
    int x0 = 0, x1 = kArtSrcW - 1, y0 = 0, y1 = kArtSrcH - 1;
    auto col_lit = [&](int x) { for (int y = 0; y < kArtSrcH; y++) if (src[y * kArtSrcW + x]) return true; return false; };
    auto row_lit = [&](int y) { for (int x = 0; x < kArtSrcW; x++) if (src[y * kArtSrcW + x]) return true; return false; };
    while (x0 < x1 && !col_lit(x0)) x0++;
    while (x1 > x0 && !col_lit(x1)) x1--;
    while (y0 < y1 && !row_lit(y0)) y0++;
    while (y1 > y0 && !row_lit(y1)) y1--;
    const int cw = x1 - x0 + 1, ch = y1 - y0 + 1;
    int dw = cw * h / ch, dh = h;   // contain: the height filled
    if (dw > kArtW) { dw = kArtW; dh = ch * kArtW / cw; }
    const int dx0 = (kArtW - dw) / 2, dy0 = (h - dh) / 2;
    // ambient: 16 x 6 cell averages of the picture
    uint16_t amb[6][16];
    for (int gy = 0; gy < 6; gy++)
        for (int gx = 0; gx < 16; gx++) {
            uint32_t r = 0, g = 0, b = 0, n = 0;
            for (int y = y0 + gy * ch / 6; y < y0 + (gy + 1) * ch / 6; y += 2)
                for (int x = x0 + gx * cw / 16; x < x0 + (gx + 1) * cw / 16; x += 2) {
                    const uint16_t c = src[y * kArtSrcW + x];
                    r += c >> 11; g += c >> 5 & 63; b += c & 31; n++;
                }
            amb[gy][gx] = n ? (uint16_t)((r / n) << 11 | (g / n) << 5 | (b / n)) : 0;
        }
    for (int y = 0; y < h; y++) {
        uint16_t *d = s_artpx + (size_t)y * kArtW;
        const int fade = h < kScrH && y >= h - 48 ? 256 * (h - y) / 48 : 256;
        const int gy = y * 6 * 256 / h - 128;
        for (int x = 0; x < kArtW; x++) {
            uint16_t c;
            if (x >= dx0 && x < dx0 + dw && y >= dy0 && y < dy0 + dh) {   // the picture, bilinear
                const int fx = ((2 * (x - dx0) + 1) * cw * 128) / dw - 128, fy = ((2 * (y - dy0) + 1) * ch * 128) / dh - 128;
                const int sx = x0 + (fx < 0 ? 0 : fx >> 8), sy = y0 + (fy < 0 ? 0 : fy >> 8);
                const int sx1 = sx + 1 <= x1 ? sx + 1 : sx, sy1 = sy + 1 <= y1 ? sy + 1 : sy;
                const int wx = fx < 0 ? 0 : (fx & 255) >> 4, wy = fy < 0 ? 0 : (fy & 255) >> 4;   // 0..15
                const uint16_t a = src[sy * kArtSrcW + sx], b2 = src[sy * kArtSrcW + sx1];
                const uint16_t c2 = src[sy1 * kArtSrcW + sx], d2 = src[sy1 * kArtSrcW + sx1];
                const uint16_t top = wx < 5 ? a : wx > 10 ? b2 : mix565(a, b2);
                const uint16_t bot = wx < 5 ? c2 : wx > 10 ? d2 : mix565(c2, d2);
                c = wy < 5 ? top : wy > 10 ? bot : mix565(top, bot);
            } else {   // the ambient glow, darkened, a little darker away from the picture
                const int dist = x < dx0 ? dx0 - x : x - (dx0 + dw);
                const int f = 110 - (dist * 70) / (kArtW / 2 + 1);
                c = dim565(amb_at(amb, (x - dx0) * 16 * 256 / (dw > 0 ? dw : 1) - 128, gy), f < 30 ? 30 : f);
            }
            d[x] = fade < 256 ? dim565(c, fade) : c;
        }
    }
    lv_image_cache_drop(&s_art_dsc);
    s_art_dsc = lv_image_dsc_t{};
    s_art_dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
    s_art_dsc.header.cf = LV_COLOR_FORMAT_RGB565;
    s_art_dsc.header.w = kArtW;
    s_art_dsc.header.h = (uint32_t)h;
    s_art_dsc.header.stride = kArtW * 2;
    s_art_dsc.data_size = (uint32_t)kArtW * h * 2;
    s_art_dsc.data = (const uint8_t *)s_artpx;
    s_art_id = m.id;
    s_art_h = h;
}

void bg_clear(void) {
    if (s_bg) lv_obj_add_flag(s_bg, LV_OBJ_FLAG_HIDDEN);
    s_bg_id = 0;
}

// The bridge's request for a picture. have: the key of the copy on the microSD (the bridge answers 'K'
// when it still holds, without converting or sending anything).
// pf: a prefetch (a picture for the card, not for the screen): the bridge never drops it as out of date.
bool req_image(int id, char kind, int w, int h, int sub, const char *have, bool pf = false) {
    const char *ks = kind == 'h' ? "help" : kind == 'b' ? "backglass" : kind == 'm' ? "media" : "wheel";
    char q[320], extra[160] = "";
    if (kind == 'm') snprintf(extra, sizeof extra, ",\"screen\":\"%s\",\"box\":[32,18,960,480]", kShowScreens[sub < 0 ? 0 : sub]);
    if (kind == 'h') snprintf(extra, sizeof extra, ",\"rot\":90");
    if (kind != 'w') {   // opaque: as JPEG, small on the cable and the card, decoded by the hardware
        const size_t n = strlen(extra);
        snprintf(extra + n, sizeof extra - n, ",\"jpeg\":1");
    }
    if (have && *have) {
        const size_t n = strlen(extra);
        snprintf(extra + n, sizeof extra - n, ",\"have\":\"%s\"", have);
    }
    if (pf) {
        const size_t n = strlen(extra);
        snprintf(extra + n, sizeof extra - n, ",\"pf\":1");
    }
    snprintf(q, sizeof q, "{\"q\":\"image\",\"id\":%d,\"kind\":\"%s\",\"w\":%d,\"h\":%d,\"bg\":\"0x000000\",\"alpha\":%d%s}", id,
             ks, w, h, kind == 'w' ? 1 : 0, extra);
    if (!nv_dmd_bridge_send('Q', q)) return false;
    s_wait_id = id;
    s_wait_us = esp_timer_get_time();
    return true;
}
bool ask_image(int id, char kind, int w, int h) { return req_image(id, kind, w, h, -1, nullptr); }

// ---- the microSD cache
NV_PSRAM_BSS bool s_sd_ok;                    // the cache folder is there
struct Ver { int id, w, h; char kind; int8_t sub; char key[12]; };
NV_PSRAM_BSS Ver s_ver[64];                   // pictures shown from the card, to check with the bridge
NV_PSRAM_BSS int s_nver;
NV_PSRAM_BSS int s_pf;                        // prefetch: the next table of the list to look at
NV_PSRAM_BSS int s_pf_left;                   // tables left to look at in this pass

void sd_init(void) {
    mkdir("/sdcard/data", 0775);
    mkdir(DMD_CACHE_DIR, 0775);
    mkdir(DMD_CACHE_DIR "/img", 0775);
    struct stat st;
    s_sd_ok = stat(DMD_CACHE_DIR "/img", &st) == 0;
}
void sd_name(char *b, size_t n, int id, char kind, int w, int sub) {
    snprintf(b, n, DMD_CACHE_DIR "/img/%c%d_%d_%d.nvi", kind, id, kind == 'h' ? 0 : w, sub < 0 ? 0 : sub);
}
bool sd_has(int id, char kind, int w, int sub) {
    if (!s_sd_ok) return false;
    char nm[96];
    sd_name(nm, sizeof nm, id, kind, w, sub);
    struct stat st;
    return stat(nm, &st) == 0;
}
void sd_write(const char *name, const void *p, size_t n) {
    char tmp[104];
    snprintf(tmp, sizeof tmp, "%s.tmp", name);
    FILE *f = fopen(tmp, "wb");
    if (!f) return;
    const bool ok = fwrite(p, 1, n, f) == n;
    fclose(f);
    if (ok) { remove(name); rename(tmp, name); }
    else remove(tmp);
}
uint8_t *sd_read(const char *name, uint32_t *n) {
    FILE *f = fopen(name, "rb");
    if (!f) return nullptr;
    fseek(f, 0, SEEK_END);
    const long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *b = sz > 2 && sz < 4 * 1024 * 1024 ? (uint8_t *)heap_caps_malloc((size_t)sz + 1, MALLOC_CAP_SPIRAM) : nullptr;
    if (b && fread(b, 1, (size_t)sz, f) != (size_t)sz) { heap_caps_free(b); b = nullptr; }
    fclose(f);
    if (b) { b[sz] = 0; *n = (uint32_t)sz; }
    return b;
}

NV_PSRAM_BSS uint32_t s_list_hash;            // of the list applied last (an identical one is not re-applied)
NV_PSRAM_BSS int64_t s_list_us;               // when the list was asked last (refreshed every 5 minutes at rest)

uint32_t fnv(const char *p) {
    uint32_t h = 2166136261u;
    while (*p) h = (h ^ (uint8_t)*p++) * 16777619u;
    return h;
}
void list_file(char *b, size_t n, int list) { snprintf(b, n, DMD_CACHE_DIR "/list_%s.json", kLists[list]); }

void img_apply(Img *m);

// A picture from the card, at once (a few ms instead of the cable's tenths of a second); it is checked
// with the bridge later, when nothing else waits. False when the card does not have it.
bool sd_fetch(int id, char kind, int w, int h, int sub) {
    if (!s_sd_ok) return false;
    char nm[96];
    sd_name(nm, sizeof nm, id, kind, w, sub);
    uint32_t n = 0;
    uint8_t *b = sd_read(nm, &n);
    if (!b) return false;
    Img *m = img_store(b, n);
    heap_caps_free(b);
    if (!m) {   // unreadable: gone, the bridge sends it again (short of memory: the file is fine, it stays)
        if (!s_store_nomem) remove(nm);
        return false;
    }
    if (*m->key) {
        Ver &v = s_ver[s_nver++ % 64];
        v = Ver{id, w, h, kind, (int8_t)sub, {}};
        snprintf(v.key, sizeof v.key, "%s", m->key);
    }
    img_apply(m);
    return true;
}

bool want(int id, char kind, int w, int h) {
    if (!id || is_missing(id, kind) || img_find(id, kind, w)) return false;
    if (sd_fetch(id, kind, w, h, -1)) return false;   // here now: the next one
    return ask_image(id, kind, w, h);
}

// The next image the screen needs: the instructions asked for, the stage's wheel, the wheels on the bar
// from the middle outwards, the backglass once the selection rests, then wheels a little further out.
void pump_images(void) {
    const int64_t now = esp_timer_get_time();
    if (s_wait_id && now - s_wait_us < kReqTimeoutUs) return;
    s_wait_id = 0;
    if (s_want_help && s_card_id) {
        s_want_help = false;
        if (want(s_card_id, 'h', 1024, kHelpH)) return;
    }
    if (s_show_id) {   // the showcase: the picture on screen, then the next one
        for (int k = 0; k < 2 && k < s_show_n; k++) {
            const int sub = s_show_list[(s_show_at < 0 ? 0 : s_show_at + k) % s_show_n];
            if (is_missing(s_show_id, 'm', sub)) continue;   // (dropped from the list as the error came)
            if (!show_find(s_show_id, sub) && !sd_fetch(s_show_id, 'm', 1024, 600, sub)) {
                req_image(s_show_id, 'm', 1024, 600, sub, nullptr);
                return;
            }
        }
        return;   // nothing else while it runs: its pictures are big
    }
    if (want(s_card_id, 'w', kBigImg, kBigImg)) return;
    if (s_nitems)
        for (int d = 0; d <= kSlots / 2; d++)
            for (int sgn = 1; sgn >= -1; sgn -= 2) {
                if (want(s_items[wrap(s_sel + sgn * d)].id, 'w', kBarImg, kBarImg)) return;
                if (!d) break;
            }
    const bool rest = now - s_sel_us > kRestUs && !s_dragging && !s_animating;
    if (rest && s_card_id && s_bg_id != s_card_id) {
        if (Img *m = img_find(s_card_id, 'b', kBgW)) bg_build(*m);
        else if (!is_missing(s_card_id, 'b') && sd_fetch(s_card_id, 'b', kBgW, kBgH, -1)) {}
        else if (!is_missing(s_card_id, 'b') && ask_image(s_card_id, 'b', kBgW, kBgH)) return;
        else bg_clear();
    }
    if (rest && s_nitems)
        for (int d = kSlots / 2 + 1; d <= 8 && d < s_nitems; d++)
            for (int sgn = 1; sgn >= -1; sgn -= 2)
                if (want(s_items[wrap(s_sel + sgn * d)].id, 'w', kBarImg, kBarImg)) return;
    if (!rest) return;
    // pictures shown from the card: still what the bridge has? (it answers 'K' in a few bytes)
    if (s_nver > 0) {
        const Ver v = s_ver[--s_nver % 64];
        if (req_image(v.id, v.kind, v.w, v.h, v.sub, v.key)) return;
    }
    // the menu untouched for a while: the wheels and backglasses of the list, one at a time, onto the card
    // (then a table comes up at once, from the card; the PC converts each picture once, in idle time)
    if (s_sd_ok && s_nitems && s_pf_left > 0 && now - s_last_touch_us > 15000000) {
        // the same picture asked twice in a row and still not on the card (an answer that did not land):
        // this table is left for the next pass, never asked in a loop
        static uint32_t pf_last;   // (internal RAM: the PSRAM statics are nearly full)
        auto ask = [&](int id, char kind, int w, int h, int sub) -> int {   // 1 asked, 0 busy, -1 given up
            const uint32_t key = miss_key(id, kind, sub) ^ (uint32_t)w << 20;
            if (key == pf_last) { pf_last = 0; return -1; }
            if (!req_image(id, kind, w, h, sub, nullptr, true)) return 0;
            pf_last = key;
            return 1;
        };
        for (int k = 0; k < 8 && s_pf_left > 0; k++) {
            const int id = s_items[wrap(s_pf)].id;
            int r = 0;
            if (!is_missing(id, 'w') && !img_find(id, 'w', kBarImg) && !sd_has(id, 'w', kBarImg, -1))
                r = ask(id, 'w', kBarImg, kBarImg, -1);
            else if (!is_missing(id, 'w') && !img_find(id, 'w', kBigImg) && !sd_has(id, 'w', kBigImg, -1))
                r = ask(id, 'w', kBigImg, kBigImg, -1);   // the stage's: the table comes up whole
            else if (!is_missing(id, 'b') && !img_find(id, 'b', kBgW) && !sd_has(id, 'b', kBgW, -1))
                r = ask(id, 'b', kBgW, kBgH, -1);
            else if (!is_missing(id, 'm', 0) && !sd_has(id, 'm', 1024, 0))
                r = ask(id, 'm', 1024, 600, 0);   // the showcase's first picture: a joystick move shows it at once
            if (r > 0) return;
            s_pf++;   // this table is on the card (or the bridge is busy, or an answer never landed: next pass)
            s_pf_left--;
        }
    }
}

// ------------------------------------------------------------------------------------------- widgets

lv_obj_t *plain(lv_obj_t *parent) {
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_CLICKABLE);
    return o;
}

lv_obj_t *text(lv_obj_t *parent, const lv_font_t *font, uint32_t color) {
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, hx(color), 0);
    return l;
}

// A vertical (dir VER) or horizontal (HOR) two-colour gradient fill.
void grad(lv_obj_t *o, uint32_t a, uint32_t b, lv_grad_dir_t dir) {
    lv_obj_set_style_bg_color(o, hx(a), 0);
    lv_obj_set_style_bg_grad_color(o, hx(b), 0);
    lv_obj_set_style_bg_grad_dir(o, dir, 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
}

// A line: an image (dots) and a label, one of the two shown.
lv_obj_t *line_make(lv_obj_t *parent, const lv_font_t *font, uint32_t color, int w) {
    lv_obj_t *box = plain(parent);
    lv_obj_set_size(box, w, LV_SIZE_CONTENT);
    lv_obj_t *img = lv_image_create(box);
    lv_obj_clear_flag(img, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(img, LV_OBJ_FLAG_HIDDEN);
    lv_obj_t *l = text(box, font, color);
    lv_obj_set_width(l, w);
    lv_label_set_long_mode(l, LV_LABEL_LONG_DOT);
    lv_obj_add_flag(l, LV_OBJ_FLAG_HIDDEN);
    return box;
}

void line_set(lv_obj_t *box, DotText &d, const char *t, uint32_t color, int max_h, int max_pitch) {
    lv_obj_t *img = lv_obj_get_child(box, 0), *lbl = lv_obj_get_child(box, 1);
    if (!*t) {
        lv_obj_add_flag(img, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(lbl, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    if (L().dots && dot_text(d, t, color, lv_obj_get_width(box) > 0 ? lv_obj_get_width(box) : kColW, max_h, max_pitch)) {
        lv_image_set_src(img, &d.dsc);
        lv_obj_remove_flag(img, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(lbl, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_label_set_text(lbl, t);
        lv_obj_remove_flag(lbl, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(img, LV_OBJ_FLAG_HIDDEN);
    }
}

void btn_pressed_cb(lv_event_t *e) {   // DMD look: the dot label on the fill while pressed
    Btn *b = (Btn *)lv_event_get_user_data(e);
    if (!b->img || !b->dot_on.px) return;
    lv_image_set_src(b->img, lv_event_get_code(e) == LV_EVENT_PRESSED ? &b->dot_on.dsc : &b->dot.dsc);
}

// A button. Cabinet: a capsule — the main one a lit cyan insert (gradient, chrome rim), the others a chrome
// outline. DMD: a square bezel with the label in dots, filled with its colour while pressed.
// small: a header button, as tall as the tabs (its type the tabs' size)
void btn_make(Btn &b, lv_obj_t *parent, const char *sym, const char *label, bool main, lv_event_cb_t cb,
              bool small = false) {
    const Look &lk = L();
    const uint32_t col = main ? lk.play : lk.second;
    b.obj = lv_obj_create(parent);
    lv_obj_remove_style_all(b.obj);
    lv_obj_set_size(b.obj, LV_SIZE_CONTENT, small ? NV_TOUCH_MIN : main ? 60 : 52);
    lv_obj_set_style_pad_hor(b.obj, small ? 14 : main ? 32 : 22, 0);
    lv_obj_add_flag(b.obj, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(b.obj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(b.obj, cb, LV_EVENT_CLICKED, nullptr);
    b.img = nullptr;
    if (lk.dots) {
        lv_obj_set_style_radius(b.obj, 0, 0);
        lv_obj_set_style_border_width(b.obj, small ? 2 : 3, 0);
        lv_obj_set_style_border_color(b.obj, hx(col), 0);
        lv_obj_set_style_bg_color(b.obj, hx(0x000000), 0);
        lv_obj_set_style_bg_opa(b.obj, LV_OPA_70, 0);
        lv_obj_set_style_bg_color(b.obj, hx(col), LV_STATE_PRESSED);
        lv_obj_set_style_bg_opa(b.obj, LV_OPA_COVER, LV_STATE_PRESSED);
        const int h = small ? 18 : main ? 36 : 27, pitch = small ? 2 : 4;
        if (dot_text(b.dot, label, col, 420, h, pitch)) {
            dot_text(b.dot_on, label, 0x000000, 420, h, pitch, col);
            b.img = lv_image_create(b.obj);
            lv_obj_clear_flag(b.img, LV_OBJ_FLAG_CLICKABLE);
            lv_image_set_src(b.img, &b.dot.dsc);
            lv_obj_center(b.img);
            lv_obj_add_event_cb(b.obj, btn_pressed_cb, LV_EVENT_PRESSED, &b);
            lv_obj_add_event_cb(b.obj, btn_pressed_cb, LV_EVENT_RELEASED, &b);
            lv_obj_add_event_cb(b.obj, btn_pressed_cb, LV_EVENT_PRESS_LOST, &b);
            return;
        }
    }
    lv_obj_set_style_radius(b.obj, LV_RADIUS_CIRCLE, 0);
    if (main) {   // a lit insert
        grad(b.obj, lk.play, lk.play2, LV_GRAD_DIR_VER);
        lv_obj_set_style_border_width(b.obj, 2, 0);
        lv_obj_set_style_border_color(b.obj, hx(0xD6FBFF), 0);
        lv_obj_set_style_bg_color(b.obj, hx(0xFFFFFF), LV_STATE_PRESSED);
        lv_obj_set_style_bg_grad_color(b.obj, hx(0xBFF6FF), LV_STATE_PRESSED);
    } else {      // chrome outline
        lv_obj_set_style_border_width(b.obj, 2, 0);
        lv_obj_set_style_border_color(b.obj, hx(col), 0);
        lv_obj_set_style_border_opa(b.obj, LV_OPA_60, 0);
        lv_obj_set_style_bg_color(b.obj, hx(0x000000), 0);
        lv_obj_set_style_bg_opa(b.obj, LV_OPA_50, 0);
        lv_obj_set_style_bg_color(b.obj, hx(col), LV_STATE_PRESSED);
        lv_obj_set_style_bg_opa(b.obj, LV_OPA_30, LV_STATE_PRESSED);
    }
    char t[80];
    snprintf(t, sizeof t, "%s%s%s", sym ? sym : "", sym ? "   " : "", label);
    lv_obj_t *l = text(b.obj, main && !small ? &nv_font_28 : &nv_font_20, main ? lk.play_ink : col);
    lv_label_set_text(l, t);
    lv_obj_center(l);
}

void btn_free(Btn &b) {
    dot_free(b.dot);
    dot_free(b.dot_on);
    b.obj = b.img = nullptr;
}

// ------------------------------------------------------------------------------------------- Popper over Wi-Fi

void http_task(void *) {
    char host[40], path[64];
    snprintf(host, sizeof host, "%s", s_pc_ip);
    snprintf(path, sizeof path, "%s", s_http_path);
    int ok = -1;
    const int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd >= 0) {
        struct timeval tv = {3, 0};
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
        struct sockaddr_in a = {};
        a.sin_family = AF_INET;
        a.sin_port = htons((uint16_t)(s_pc_web > 0 ? s_pc_web : 80));
        if (inet_pton(AF_INET, host, &a.sin_addr) == 1 && connect(fd, (struct sockaddr *)&a, sizeof a) == 0) {
            char req[160];
            const int n = snprintf(req, sizeof req, "GET %s HTTP/1.0\r\nHost: %s\r\n\r\n", path, host);
            char resp[32] = {};
            if (send(fd, req, n, 0) == n && recv(fd, resp, sizeof resp - 1, 0) > 12 && !strncmp(resp + 9, "200", 3)) ok = 1;
        }
        close(fd);
    }
    s_http_result = ok;
    s_http_busy = false;
    vTaskDeleteWithCaps(nullptr);
}

// One GET to Popper's Web Remote, in the background (a few hundred ms over Wi-Fi). False if not possible.
bool popper_get(const char *path) {
    if (!s_pc_ip[0] || s_http_busy) return false;
    snprintf(s_http_path, sizeof s_http_path, "%s", path);
    s_http_result = 0;
    s_http_busy = true;
    if (xTaskCreatePinnedToCoreWithCaps(http_task, "dmdhttp", 4096, nullptr, 3, nullptr, 0, MALLOC_CAP_SPIRAM) != pdPASS) {
        s_http_busy = false;
        return false;
    }
    return true;
}

// ------------------------------------------------------------------------------------------- stage

void show_note(const char *t) {
    if (!s_note) return;
    if (t && *t) {
        lv_label_set_text(s_note, t);
        lv_obj_remove_flag(s_note, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_note, LV_OBJ_FLAG_HIDDEN);
    }
}

// "today" / "yesterday" / "3 days ago"
void rel_day(char *b, size_t n, int ago) {
    if (ago < 0) b[0] = 0;
    else if (ago == 0) snprintf(b, n, "%s", nv_tr(NV_STR_DMDM_TODAY));
    else if (ago == 1) snprintf(b, n, "%s", nv_tr(NV_STR_DMDM_YESTERDAY));
    else snprintf(b, n, nv_tr(NV_STR_DMDM_DAYS_FMT), ago);
}

// "2026-10-12" -> "12.10.2026"
void fmt_date(char *b, size_t n, const char *iso) {
    if (strlen(iso) >= 10) snprintf(b, n, "%.2s.%.2s.%.4s", iso + 8, iso + 5, iso);
    else snprintf(b, n, "%s", iso);
}

void fmt_time(char *b, size_t n, int secs) {
    if (secs >= 3600) snprintf(b, n, "%d h %02d min", secs / 3600, secs / 60 % 60);
    else if (secs < 60) snprintf(b, n, "<1 min");
    else snprintf(b, n, "%d min", secs / 60);
}

// A name in place of a missing wheel: capitals, on up to three lines, in the look's title colour.
void placeholder_set(lv_obj_t *ph, int id, bool show) {
    if (!ph) return;
    if (!show) { lv_obj_add_flag(ph, LV_OBJ_FLAG_HIDDEN); return; }
    const char *name = "";
    for (int i = 0; i < s_nitems; i++)
        if (s_items[i].id == id) { name = s_items[i].name; break; }
    if (id == s_card_id && !*name) name = s_card_name;
    char clean[64], up[64];
    clean_name(clean, sizeof clean, name);
    dot_fold(up, sizeof up, clean);
    lv_label_set_text(ph, up);
    lv_obj_remove_flag(ph, LV_OBJ_FLAG_HIDDEN);
}

void stage_wheel(void) {
    Img *m = img_find(s_card_id, 'w', kBigImg);
    lv_image_set_src(s_stage_img, m ? &m->dsc : nullptr);
    placeholder_set(s_stage_ph, s_card_id, !m && is_missing(s_card_id, 'w'));
}

// The stage for a table: name, facts, stats (scores and the help button come with the bridge's card).
void stage_show(int id, const char *name, int year, const char *maker, int plays, int secs, int ago, const char *last,
                bool fav, const char *added) {
    const Look &lk = L();
    s_card_id = id;
    // another table's backglass never stays behind this one: off now, this one's once the selection rests
    if (s_bg_id && s_bg_id != id) bg_clear();
    snprintf(s_card_name, sizeof s_card_name, "%s", name);
    char clean[64];
    clean_name(clean, sizeof clean, name);
    if (lk.dots && dot_text(s_dot_name, clean, lk.title, kColW, 54, 6)) {
        lv_image_set_src(s_stage_dots, &s_dot_name.dsc);
        lv_obj_remove_flag(s_stage_dots, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_stage_title, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_label_set_text(s_stage_title, clean);
        lv_obj_remove_flag(s_stage_title, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_stage_dots, LV_OBJ_FLAG_HIDDEN);
    }
    char meta[128];
    const char *sep = lk.dots ? "  -  " : "   \xC2\xB7   ";
    if (year && *maker) snprintf(meta, sizeof meta, "%s%s%d", maker, sep, year);
    else if (year) snprintf(meta, sizeof meta, "%d", year);
    else snprintf(meta, sizeof meta, "%s", maker);
    // the system, when the list mixes them (no filter): VPX, FX2 and MAME tables side by side
    int emu = 0;
    for (int i = 0; i < s_nitems; i++)
        if (s_items[i].id == id) { emu = s_items[i].emu; break; }
    if (emu && !s_filter && *emu_name(emu)) {
        const size_t n = strlen(meta);
        snprintf(meta + n, sizeof meta - n, "%s%s", n ? sep : "", emu_name(emu));
    }
    line_set(s_stage_meta, s_dot_meta, meta, lk.meta, 27, 3);
    char a[96] = "", b[96] = "";
    if (plays) {
        char tb[32], rel[40];
        fmt_time(tb, sizeof tb, secs);
        if (plays == 1) snprintf(a, sizeof a, nv_tr(NV_STR_DMDM_STATS1_FMT), tb);
        else snprintf(a, sizeof a, nv_tr(NV_STR_DMDM_STATS_FMT), plays, tb);
        rel_day(rel, sizeof rel, ago);
        if (rel[0]) snprintf(b, sizeof b, nv_tr(NV_STR_DMDM_LAST_REL_FMT), rel);
        else if (*last) snprintf(b, sizeof b, nv_tr(NV_STR_DMDM_LAST_FMT), last);
    } else {
        snprintf(a, sizeof a, "%s", nv_tr(NV_STR_DMDM_NEVER));
    }
    if (added && *added) {   // the "new" list: when it arrived
        char d[24];
        fmt_date(d, sizeof d, added);
        snprintf(b, sizeof b, nv_tr(NV_STR_DMDM_ADDED_FMT), d);
    }
    line_set(s_stage_st1, s_dot_st1, a, lk.stats, 24, 3);
    line_set(s_stage_st2, s_dot_st2, b, lk.stats, 24, 3);
    if (fav) lv_obj_remove_flag(s_stage_fav, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(s_stage_fav, LV_OBJ_FLAG_HIDDEN);
    lv_label_set_text(s_stage_scores, "");
    lv_obj_add_flag(s_helpb.obj, LV_OBJ_FLAG_HIDDEN);
    s_card_asked = false;
    stage_wheel();
    lv_obj_remove_flag(s_stage, LV_OBJ_FLAG_HIDDEN);
}

void stage_item(int i) {
    const Item &it = s_items[i];
    lv_obj_add_flag(s_another.obj, LV_OBJ_FLAG_HIDDEN);
    stage_show(it.id, it.name, it.year, it.maker, it.plays, it.secs, it.ago, "", it.fav, s_list == 1 ? it.added : "");
}

// ------------------------------------------------------------------------------------------- wheel bar

// Slot k (0..kSlots-1) shows item sel + k - kSlots/2. Its centre: the middle of the screen, plus its rank
// times the step, plus the drag. The one nearest the middle shows the large lit wheel, the others the
// small dimmed one (Popper's wheel bar).
void slot_src(int k, bool big) {
    const int id = s_slot_id[k];
    Img *m = nullptr;
    if (id) {
        if (big) m = img_find(id, 'w', kBarImg);
        else if (Img *w = img_find(id, 'w', kBarImg)) m = side_make(*w);
    }
    lv_image_set_src(s_slot[k], m ? &m->dsc : nullptr);
    s_slot_big[k] = big;
    placeholder_set(s_slot_ph[k], id, id && !m && is_missing(id, 'w'));
}

void bar_layout(void) {
    if (!s_bar) return;
    for (int k = 0; k < kSlots; k++) {
        lv_obj_t *o = s_slot[k];
        const int off = k - kSlots / 2;
        if (!s_nitems || (s_nitems < kSlots && (off < -(s_nitems - 1) / 2 || off > s_nitems / 2))) {
            lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);   // a short list: no wheel twice on screen
            if (s_slot_ph[k]) lv_obj_add_flag(s_slot_ph[k], LV_OBJ_FLAG_HIDDEN);
            continue;
        }
        lv_obj_remove_flag(o, LV_OBJ_FLAG_HIDDEN);
        const int cx = kScrW / 2 + off * kSlotStep + s_drag;
        const bool big = abs(cx - kScrW / 2) < kSlotStep / 2;
        if (big != s_slot_big[k]) slot_src(k, big);
        lv_obj_set_pos(o, cx - kBarImg / 2, (kBarH - kBarImg) / 2);
        if (s_slot_ph[k]) lv_obj_align(s_slot_ph[k], LV_ALIGN_TOP_LEFT, cx - (kBarImg - 20) / 2, kBarH / 2 - 24);
    }
}

void bar_fill(void) {
    for (int k = 0; k < kSlots; k++) {
        s_slot_id[k] = s_nitems ? s_items[wrap(s_sel + k - kSlots / 2)].id : 0;
        slot_src(k, k == kSlots / 2 && !s_drag);
    }
    bar_layout();
}

void update_count(void) {
    if (!s_count) return;
    char t[48] = "";
    if (s_nitems && s_filter) snprintf(t, sizeof t, "%.16s  %d / %d", emu_name(s_filter), s_sel + 1, s_nitems);
    else if (s_nitems) snprintf(t, sizeof t, "%d / %d", s_sel + 1, s_nitems);
    if (L().dots) {
        if (*t && dot_text(s_dot_count, t, L().meta, 260, 18, 2)) {
            lv_image_set_src(s_count, &s_dot_count.dsc);
            if (!s_browse) lv_obj_remove_flag(s_count, LV_OBJ_FLAG_HIDDEN);   // (in game: "back to the DMD" there)
        } else {
            lv_obj_add_flag(s_count, LV_OBJ_FLAG_HIDDEN);
        }
    } else {
        lv_label_set_text(s_count, t);
    }
}

void select_item(int i) {
    if (!s_nitems) return;
#ifdef DMD_SIM_TRACE
    printf("  [sel] %d -> %d %s\n", s_sel, wrap(i), s_items[wrap(i)].name);
#endif
    s_sel = wrap(i);
    s_sel_us = esp_timer_get_time();
    s_drag = 0;
    bar_fill();
    update_count();
    stage_item(s_sel);
}

void anim_exec(void *, int32_t v) {
    s_drag = (int)v;
    bar_layout();
}

// Where the running slide lands: the table it set off for, wherever a list that came meanwhile put it.
int anim_target(void) {
    for (int i = 0; s_anim_id && i < s_nitems; i++)
        if (s_items[i].id == s_anim_id) return i;
    return s_sel + s_anim_n;
}

void anim_done(lv_anim_t *) {
    s_animating = false;
    select_item(anim_target());
}

// Move the selection by n tables, sliding the bar from where the finger left it.
void bar_move(int n) {
    if (!s_nitems) return;
    if (n > 60) n = 60;
    if (n < -60) n = -60;
    if (n > kSlots / 2 || n < -kSlots / 2) {   // far: jump most of the way, slide the last wheels in
        const int sgn = n > 0 ? 1 : -1;
        lv_anim_delete(&s_drag, anim_exec);
        s_animating = false;
        select_item(s_sel + n - sgn * (kSlots / 2));
        n = sgn * (kSlots / 2);
    }
    if (!n && !s_drag) return;
    lv_anim_delete(&s_drag, anim_exec);
    s_anim_n = n;
    s_anim_id = s_items[wrap(s_sel + n)].id;
    s_animating = true;
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, &s_drag);
    lv_anim_set_exec_cb(&a, anim_exec);
    lv_anim_set_values(&a, s_drag, -n * kSlotStep);
    lv_anim_set_duration(&a, (uint32_t)(150 + 45 * abs(n)));
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_set_completed_cb(&a, anim_done);
    lv_anim_start(&a);
}

void bar_event(lv_event_t *e) {
    const lv_event_code_t code = lv_event_get_code(e);
    if (code != LV_EVENT_PRESSED && code != LV_EVENT_PRESSING && code != LV_EVENT_RELEASED && code != LV_EVENT_PRESS_LOST)
        return;
    lv_indev_t *in = lv_indev_active();
    if (!in) return;
    lv_point_t p;
    lv_indev_get_point(in, &p);
    if (code == LV_EVENT_PRESSED) {
        s_last_touch_us = esp_timer_get_time();
        if (s_animating) {   // a finger on a moving bar takes it where it is
            lv_anim_delete(&s_drag, anim_exec);
            s_animating = false;
            const int keep = s_drag + s_anim_n * kSlotStep;
            select_item(anim_target());
            s_drag = keep;
            bar_layout();
        }
        s_press_x = p.x - s_drag;
        s_dragging = true;
        s_drag_vx = 0;
        s_drag_lx = p.x;
        s_drag_lus = esp_timer_get_time();
    } else if (code == LV_EVENT_PRESSING && s_dragging) {
        int d = p.x - s_press_x;
        const int lim = (kSlots / 2) * kSlotStep;
        if (d > lim) d = lim;
        if (d < -lim) d = -lim;
        s_drag = d;
        bar_layout();
        // the finger's speed, smoothed: px per 10 ms
        const int64_t now = esp_timer_get_time();
        const int dt = (int)((now - s_drag_lus) / 10000);
        if (dt > 0) {
            s_drag_vx = (s_drag_vx + (p.x - s_drag_lx) / dt) / 2;
            s_drag_lx = p.x;
            s_drag_lus = now;
        }
    } else if (s_dragging) {   // released
        s_dragging = false;
        if (abs(s_drag) < 12) {   // a tap: that wheel to the middle
            const int dx = p.x - kScrW / 2;
            bar_move((dx + (dx >= 0 ? kSlotStep / 2 : -kSlotStep / 2)) / kSlotStep);
            return;
        }
        int n = (-s_drag + (s_drag < 0 ? kSlotStep / 2 : -kSlotStep / 2)) / kSlotStep;
        // a flick keeps the bar going: the faster, the further (a fast one crosses dozens of tables)
        // (a real flick only: over 3 px/ms at the release; never more than a third of the list round)
        if (esp_timer_get_time() - s_drag_lus < 80000 && abs(s_drag_vx) > 30) {
            const int lim = s_nitems / 3 > kSlots / 2 ? s_nitems / 3 : kSlots / 2;
            int extra = -(s_drag_vx > 0 ? 1 : -1) * (abs(s_drag_vx) - 25) * abs(s_drag_vx) / 50;
            if (extra > lim) extra = lim;
            if (extra < -lim) extra = -lim;
            n += extra;
        }
#ifdef DMD_SIM_TRACE
        printf("  [fling] drag %d vx %d dt %lld -> n %d\n", s_drag, s_drag_vx, (long long)(esp_timer_get_time() - s_drag_lus), n);
#endif
        bar_move(n);
    }
}

// Swipes on the stage (nv_gesture_bind: consumed there, never a system gesture).
void stage_next(void *) {
    s_last_touch_us = esp_timer_get_time();
    bar_move(1);
}
void stage_prev(void *) {
    s_last_touch_us = esp_timer_get_time();
    bar_move(-1);
}

void play_key(lv_event_t *e) {   // keyboard / remote: left and right move along the bar
    const uint32_t k = lv_event_get_key(e);
    if (k == LV_KEY_LEFT) bar_move(-1);
    else if (k == LV_KEY_RIGHT) bar_move(1);
}

// ------------------------------------------------------------------------------------------- lists

void tabs_restyle(void) {
    const Look &lk = L();
    for (uint32_t i = 0; i < lv_obj_get_child_count(s_tabs); i++) {
        lv_obj_t *p = lv_obj_get_child(s_tabs, (int32_t)i);
        const bool on = (int)i == s_list;
        if (lk.dots) {
            lv_obj_set_style_border_color(p, hx(on ? lk.tab_on : lk.tab_off), 0);
            lv_obj_set_style_bg_opa(p, on ? LV_OPA_20 : LV_OPA_TRANSP, 0);
            lv_obj_t *img = lv_obj_get_child(p, 0);
            if (img && s_dot_tab[i][on].px) lv_image_set_src(img, &s_dot_tab[i][on].dsc);
        } else {   // the selected list lights up like an insert: cyan rim, dark teal glass, white type
            lv_obj_set_style_border_opa(p, on ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
            lv_obj_set_style_bg_opa(p, on ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
            lv_obj_t *l = lv_obj_get_child(p, 0);
            if (l) lv_obj_set_style_text_color(l, hx(on ? lk.tab_on : lk.tab_off), 0);
        }
    }
}

void on_list(const char *json);

void ask_list(void) {
    s_list_us = esp_timer_get_time();
    if (!s_nall && s_sd_ok) {   // the last one from the card while the bridge answers
        char nm[96];
        list_file(nm, sizeof nm, s_list);
        uint32_t n = 0;
        if (uint8_t *b = sd_read(nm, &n)) {
            on_list((const char *)b);
            heap_caps_free(b);
        }
    }
    char q[80];
    snprintf(q, sizeof q, "{\"q\":\"list\",\"list\":\"%s\",\"limit\":%d}", kLists[s_list], kMaxItems);
    if (nv_dmd_bridge_send('Q', q)) {
        s_want_list = false;
        if (!s_nitems) show_note(nv_tr(NV_STR_DMDM_LOADING));
    }
}

void random_cb(lv_event_t *) {   // "surprise me": a random table on the stage
    s_random_pending = nv_dmd_bridge_send('Q', "{\"q\":\"random\"}");
}

// A square (DMD) or round (cabinet) icon button, in the header.
lv_obj_t *icon_btn(lv_obj_t *parent, const char *sym, uint32_t color, lv_event_cb_t cb) {
    const Look &lk = L();
    lv_obj_t *b = lv_obj_create(parent);
    lv_obj_remove_style_all(b);
    lv_obj_set_size(b, NV_TOUCH_MIN, NV_TOUCH_MIN);
    lv_obj_set_style_radius(b, lk.dots ? 0 : LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(b, 2, 0);
    lv_obj_set_style_border_color(b, hx(color), 0);
    lv_obj_set_style_border_opa(b, LV_OPA_70, 0);
    lv_obj_set_style_bg_color(b, hx(0x000000), 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_50, 0);
    lv_obj_set_style_bg_color(b, hx(color), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(b, LV_OPA_40, LV_STATE_PRESSED);
    lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, nullptr);
    lv_obj_t *l = text(b, &nv_font_20, color);
    lv_label_set_text(l, sym);
    lv_obj_center(l);
    return b;
}

void tab_cb(lv_event_t *e) {
    const int i = (int)(intptr_t)lv_event_get_user_data(e);
    if (i == s_list && s_nitems) { select_item(0); return; }   // the same list again: back to its first table
    s_list = i;
    s_restore_id = 0;
    s_nall = 0;          // the new list: from the card at once when it is there, then the bridge's
    s_list_hash = 0;
    tabs_restyle();
    ask_list();
}

void settings_cb(lv_event_t *) { if (s_open_settings) s_open_settings(); }
void list_apply(int keep_id);

// The initial a table is filed under: its first letter without accent, '#' for anything else.
char initial_of(const char *name) {
    char t[8];
    dot_fold(t, sizeof t, name);
    return t[0] >= 'A' && t[0] <= 'Z' ? t[0] : '#';
}

void find_close(void) {
    if (s_find) lv_obj_add_flag(s_find, LV_OBJ_FLAG_HIDDEN);
}
void find_close_cb(lv_event_t *e) {
    if (lv_event_get_target_obj(e) == s_find) find_close();
}
void find_swipe(void *) { find_close(); }

void find_letter_cb(lv_event_t *e) {
    const char c = (char)(intptr_t)lv_event_get_user_data(e);
    find_close();
    for (int i = 0; i < s_nitems; i++)
        if (initial_of(s_items[i].name) == c) { select_item(i); return; }
}

void find_system_cb(lv_event_t *e) {
    const int id = (int)(intptr_t)lv_event_get_user_data(e);
    find_close();
    if (id == s_filter) return;
    s_filter = id;
    list_apply(s_nitems ? s_items[s_sel].id : 0);
}

// A chip / key of the "go to" card: a capsule (cabinet) or a square bezel (DMD); `on` = the current one.
lv_obj_t *find_key(lv_obj_t *parent, const char *label, bool on, bool enabled, lv_event_cb_t cb, intptr_t user, int w) {
    const Look &lk = L();
    lv_obj_t *b = lv_obj_create(parent);
    lv_obj_remove_style_all(b);
    lv_obj_set_size(b, w, NV_TOUCH_MIN + 4);
    lv_obj_set_style_pad_hor(b, 14, 0);
    lv_obj_set_style_radius(b, lk.dots ? 0 : LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(b, 2, 0);
    lv_obj_set_style_border_color(b, hx(on ? lk.accent : lk.dots ? lk.tab_off : lk.second), 0);
    lv_obj_set_style_border_opa(b, on ? LV_OPA_COVER : enabled ? LV_OPA_60 : LV_OPA_20, 0);
    lv_obj_set_style_bg_color(b, hx(lk.accent), 0);
    lv_obj_set_style_bg_opa(b, on ? LV_OPA_30 : LV_OPA_TRANSP, 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_40, LV_STATE_PRESSED);
    const uint32_t ink = on ? (lk.dots ? lk.tab_on : 0xFFFFFF) : enabled ? (lk.dots ? lk.tab_on : lk.second) : lk.dim;
    lv_obj_t *l;
    if (lk.dots && s_nfind < 40 && dot_text(s_dot_find[s_nfind], label, enabled ? ink : lk.tab_off, 260, 20, 2)) {
        l = lv_image_create(b);
        lv_obj_clear_flag(l, LV_OBJ_FLAG_CLICKABLE);
        lv_image_set_src(l, &s_dot_find[s_nfind++].dsc);
    } else {
        l = text(b, &nv_font_20, ink);
        lv_label_set_text(l, label);
    }
    lv_obj_center(l);
    if (enabled) {
        lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, (void *)user);
    } else {
        lv_obj_set_style_text_opa(l, LV_OPA_40, 0);
    }
    return b;
}

// "Go to": the systems of this list (a filter) and the letters its tables start with (a jump). Built
// when opened: it always reflects the list on screen.
void find_open_cb(lv_event_t *) {
    if (!s_find) return;
    const Look &lk = L();
    lv_obj_clean(s_find_card);
    for (DotText &d : s_dot_find) dot_free(d);   // the keys' dots go with the keys
    s_nfind = 0;
    if (lk.dots && dot_text(s_dot_find[s_nfind], nv_tr(NV_STR_DMDF_TITLE), lk.title, 400, 36, 4)) {
        lv_obj_t *t = lv_image_create(s_find_card);
        lv_image_set_src(t, &s_dot_find[s_nfind++].dsc);
    } else {
        lv_obj_t *t = text(s_find_card, &nv_font_28, lk.title);
        lv_label_set_text(t, nv_tr(NV_STR_DMDF_TITLE));
    }
    // systems with at least one table in this list
    lv_obj_t *row = plain(s_find_card);
    lv_obj_set_size(row, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, 10, 0);
    lv_obj_set_style_pad_row(row, 10, 0);
    find_key(row, nv_tr(NV_STR_DMDF_ALL), s_filter == 0, true, find_system_cb, 0, LV_SIZE_CONTENT);
    for (int e = 0; e < s_nemus; e++) {
        int n = 0;
        for (int i = 0; i < s_nall && !n; i++) n += s_all[i].emu == s_emus[e].id;
        if (n) find_key(row, s_emus[e].name, s_filter == s_emus[e].id, true, find_system_cb, s_emus[e].id, LV_SIZE_CONTENT);
    }
    // letters: the ones nothing starts with are shown, but off
    bool has[27] = {};
    for (int i = 0; i < s_nitems; i++) {
        const char c = initial_of(s_items[i].name);
        has[c == '#' ? 0 : c - 'A' + 1] = true;
    }
    lv_obj_t *grid = plain(s_find_card);
    lv_obj_set_size(grid, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(grid, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(grid, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(grid, 8, 0);
    lv_obj_set_style_pad_row(grid, 8, 0);
    lv_obj_set_style_pad_top(grid, 8, 0);
    for (int k = 0; k < 27; k++) {
        const char c = k ? (char)('A' + k - 1) : '#';
        const char lbl[2] = {c, 0};
        lv_obj_t *key = find_key(grid, k ? lbl : "0-9", false, has[k], find_letter_cb, c, k ? 62 : 96);
        lv_obj_set_style_pad_hor(key, 0, 0);
    }
    lv_obj_remove_flag(s_find, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_find);
}

void close_help(void) {
    if (s_help) lv_obj_add_flag(s_help, LV_OBJ_FLAG_HIDDEN);
}

// "Playing for 12 min" under the in-game page's status (empty while starting). A VPX table that has not
// sent a single DMD frame yet is still loading (VPX opens the table, PinMAME boots): "Loading..." for up
// to a minute; past that, a table without a DMD (a 1980 alphanumeric) is simply being played.
void launch_time(void) {
    if (!s_launch_time) return;
    if (!s_game[0] || s_launching) { line_set(s_launch_time, s_dot_time, "", 0, 24, 3); return; }
    nv_dmd_status_t st;
    nv_dmd_get_status(&st);
    if (s_game_dmd && st.last_pc_us < s_game_us && esp_timer_get_time() - s_game_us < 60000000) {   // no DMD frame yet
        line_set(s_launch_time, s_dot_time, nv_tr(NV_STR_DMDM_LOADING), L().stats, 24, 3);
        return;
    }
    const int min = (int)((esp_timer_get_time() - s_game_us) / 60000000);
    char d[24], t[64];
    if (min >= 60) snprintf(d, sizeof d, "%d h %02d min", min / 60, min % 60);
    else snprintf(d, sizeof d, "%d min", min < 1 ? 1 : min);
    snprintf(t, sizeof t, nv_tr(NV_STR_DMDQ_PLAYING_FMT), d);
    line_set(s_launch_time, s_dot_time, t, L().meta, 24, 3);
}

// "Starting" / the game: the table's name, a blinking line. Until the DMD takes the screen.
void launch_show(const char *name, bool starting) {
    if (!s_launch) return;
    const Look &lk = L();
    char clean[64], t[128], up[96];
    clean_name(clean, sizeof clean, name);
    if (starting) snprintf(t, sizeof t, "%s...", nv_tr(NV_STR_DMDM_STARTING));
    else {
        snprintf(up, sizeof up, nv_tr(NV_STR_DMDM_INGAME_FMT), "");
        size_t n = strlen(up);
        while (n && (up[n - 1] == ' ' || up[n - 1] == ':')) up[--n] = 0;
        snprintf(t, sizeof t, "%s", up);
    }
    if (lk.dots && dot_text(s_dot_launch, clean, lk.title, kScrW - 80, 90)) {
        lv_image_set_src(s_launch_dots, &s_dot_launch.dsc);
        lv_obj_remove_flag(s_launch_dots, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_launch_name, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_label_set_text(s_launch_name, clean);
        lv_obj_remove_flag(s_launch_name, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_launch_dots, LV_OBJ_FLAG_HIDDEN);
    }
    // in game the time line says it all ("playing for 12 min"); the status line is for "starting"
    line_set(s_launch_txt, s_dot_launch_txt, starting ? t : "", lk.stats, 30, 3);
    // the picture behind: the game's own art (sent as it started), else the stage's backglass of it
    const int id = starting ? s_card_id : s_game_id;
    const lv_image_dsc_t *art = starting ? nullptr : dmd_menu_game_art(kScrH);
    if (!art && s_bgpx && s_bg_id && s_bg_id == id) art = &s_bg_dsc;
    lv_image_set_src(s_launch_bg, art);
    Img *w = id ? img_find(id, 'w', kBigImg) : nullptr;
    lv_image_set_src(s_launch_wheel, w ? &w->dsc : nullptr);
    if (w) lv_obj_remove_flag(s_launch_wheel, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(s_launch_wheel, LV_OBJ_FLAG_HIDDEN);
    launch_time();
    if (starting) { lv_obj_remove_flag(s_back.obj, LV_OBJ_FLAG_HIDDEN); lv_obj_add_flag(s_launch_btns, LV_OBJ_FLAG_HIDDEN); }
    else { lv_obj_add_flag(s_back.obj, LV_OBJ_FLAG_HIDDEN); lv_obj_remove_flag(s_launch_btns, LV_OBJ_FLAG_HIDDEN); }
    if (!starting && (s_browse || s_show_game)) return;   // browsing the tables, or the game's showcase
    lv_obj_remove_flag(s_launch, LV_OBJ_FLAG_HIDDEN);
}

void launch_hide(void) {
    if (s_launch) lv_obj_add_flag(s_launch, LV_OBJ_FLAG_HIDDEN);
}

void launch_cancel_cb(lv_event_t *) {
    s_launching = false;
    launch_hide();
}

void confirm_show(void);

void play_cb(lv_event_t *) {
    if (!s_card_id) return;
    if (s_game[0]) { confirm_show(); return; }   // another table runs: ask first
    char q[64];
    snprintf(q, sizeof q, "{\"q\":\"launch\",\"id\":%d}", s_card_id);
    if (nv_dmd_bridge_send('Q', q)) {
        s_launching = true;
        s_launch_us = esp_timer_get_time();
        launch_show(s_card_name, true);
    }
}

void another_cb(lv_event_t *) { s_random_pending = nv_dmd_bridge_send('Q', "{\"q\":\"random\"}"); }

// The page at the top of the view; a short one (a card that was already landscape) centred.
void help_show(Img *m) {
    lv_image_set_src(s_help_img, m ? &m->dsc : nullptr);
    lv_obj_align(s_help_img, m && m->h >= kScrH ? LV_ALIGN_TOP_MID : LV_ALIGN_CENTER, 0, 0);
    lv_obj_scroll_to_y(s_help, 0, LV_ANIM_OFF);
}

void help_cb(lv_event_t *) {
    if (!s_card_id) return;
    Img *m = img_find(s_card_id, 'h', 1024);
    help_show(m);
    if (!m) s_want_help = true;
    lv_obj_remove_flag(s_help, LV_OBJ_FLAG_HIDDEN);
    pump_images();
}

void help_close_cb(lv_event_t *) { close_help(); }

// ------------------------------------------------------------------ in game: quick actions, browsing

void quick_close(void) {
    if (s_quick) lv_obj_add_flag(s_quick, LV_OBJ_FLAG_HIDDEN);
}
void quick_close_cb(lv_event_t *e) {
    if (lv_event_get_target_obj(e) == s_quick) quick_close();   // a tap beside the card
}
void quick_swipe(void *) { quick_close(); }
void quick_resume_cb(lv_event_t *) { quick_close(); }

void browse_set(bool on) {
    s_browse = on;
    if (s_resume) {
        if (on) { lv_obj_remove_flag(s_resume, LV_OBJ_FLAG_HIDDEN); lv_obj_add_flag(s_count, LV_OBJ_FLAG_HIDDEN); }
        else { lv_obj_add_flag(s_resume, LV_OBJ_FLAG_HIDDEN); lv_obj_remove_flag(s_count, LV_OBJ_FLAG_HIDDEN); }
    }
    if (s_show && s_show_game) {
        if (on) lv_obj_add_flag(s_show, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_remove_flag(s_show, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    if (on) launch_hide();
    else if (s_game[0]) launch_show(s_game, false);
}
void quick_tables_cb(lv_event_t *) {
    quick_close();
    browse_set(true);
}
void resume_cb(lv_event_t *) { browse_set(false); }

// Popper closes the game: through the bridge on the cable when it is there (games without DMD keep it),
// else over Wi-Fi straight to Popper's Web Remote (dmdext has the cable).
bool quit_send(void) {
    nv_dmd_status_t st;
    nv_dmd_get_status(&st);
    if (st.bridge && nv_dmd_bridge_send('Q', "{\"q\":\"exit\"}")) return true;
    return popper_get("/pupkey/15");
}
bool quit_request(void) {
    if (!quit_send()) return false;
    s_quit_us = esp_timer_get_time();
    s_quit_n = 1;
    return true;
}
void quit_game(void) {
    if (quit_request()) nv_toast(NV_NOTE_INFO, nv_tr(NV_STR_DMDQ_QUITTING));
    else nv_toast(NV_NOTE_ERROR, nv_tr(NV_STR_DMDQ_NO_PC));
}
void quick_quit_cb(lv_event_t *) {
    quick_close();
    quit_game();
}
void quick_settings_cb(lv_event_t *) {
    quick_close();
    if (s_open_settings) s_open_settings();
}

void fitm_close(void) {
    if (s_fitm) lv_obj_add_flag(s_fitm, LV_OBJ_FLAG_HIDDEN);
}
void fitm_close_cb(lv_event_t *e) {
    if (lv_event_get_target_obj(e) == s_fitm) fitm_close();   // a tap beside the card
}
void fitm_swipe(void *) { fitm_close(); }
// the current choice lit (-1 = as every table: the first line)
void fitm_mark(void) {
    const int cur = s_game_id ? dmd_app_table_fit(s_game_id) : -1;
    if (!s_fitui) return;
    for (int i = 0; i < kFitChoices; i++) {
        if (!s_fitui->b[i].obj) continue;
        lv_obj_set_style_bg_color(s_fitui->b[i].obj, hx(L().accent), 0);
        lv_obj_set_style_bg_opa(s_fitui->b[i].obj, i == cur + 1 ? LV_OPA_40 : LV_OPA_TRANSP, 0);
    }
}
void fitm_rb_mark(void) {
    if (!s_fitui) return;
    const bool on = dmd_app_table_rebuild(s_game_id);
    for (int i = 0; i < 2; i++) {
        if (!s_fitui->rb[i].obj) continue;
        lv_obj_set_style_bg_color(s_fitui->rb[i].obj, hx(L().accent), 0);
        lv_obj_set_style_bg_opa(s_fitui->rb[i].obj, (i == 0) == on ? LV_OPA_40 : LV_OPA_TRANSP, 0);
    }
}
void fitm_rb_cb(lv_event_t *e) {
    lv_obj_t *t = lv_event_get_current_target_obj(e);
    if (s_fitui && s_game_id) dmd_app_set_table_rebuild(s_game_id, t == s_fitui->rb[0].obj);
    fitm_rb_mark();
}
void fitm_pick_cb(lv_event_t *e) {
    lv_obj_t *t = lv_event_get_current_target_obj(e);
    for (int i = 0; s_fitui && i < kFitChoices; i++)
        if (s_fitui->b[i].obj == t && s_game_id) dmd_app_set_table_fit(s_game_id, i - 1);
    fitm_mark();
    fitm_close();
}
// Only what this table can show: its FullDMD art and its backglass when it has them; the suggestion under
// the title (dmd_app_fit_suggested).
void fitm_prepare(void) {
    if (!s_fitui) return;
    dmd_rect_t win;
    const bool art = dmd_menu_game_full(&win) != nullptr;
    const bool grill = dmd_menu_game_grill(&win) != nullptr;
    const bool bg = dmd_menu_game_art(kScrH - 256) != nullptr;
    for (int i = 1; i < kFitChoices; i++) {   // i - 1: the fit (0 stretched, 1 classic, 2 backglass + DMD,
        const int fit = i - 1;                // 3 backglass only, 4 table art, 5 tall, 6 full screen)
        const bool ok = fit == 4 || fit == 7 ? art : fit == 8 ? grill : (fit == 2 || fit == 3) ? bg : true;   // 7: art
        // with window, 8: backglass with housing
        if (!s_fitui->b[i].obj) continue;
        if (ok) lv_obj_remove_flag(s_fitui->b[i].obj, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(s_fitui->b[i].obj, LV_OBJ_FLAG_HIDDEN);
    }
    const bool half = dmd_app_dmd_half();   // the rebuild switch: only where there is something to rebuild
    for (Btn &b : s_fitui->rb)
        if (b.obj) {
            if (half) lv_obj_remove_flag(b.obj, LV_OBJ_FLAG_HIDDEN);
            else lv_obj_add_flag(b.obj, LV_OBJ_FLAG_HIDDEN);
        }
    fitm_rb_mark();
    if (s_fitui->tip) {
        char names[200], *name[kFitChoices - 1] = {};
        snprintf(names, sizeof names, "%s", nv_tr(NV_STR_DMDQ_FIT_NAMES));
        int n = 0;
        for (char *q = names; n < kFitChoices - 1 && q && *q;) {
            name[n++] = q;
            q = strchr(q, '\n');
            if (q) *q++ = 0;
        }
        const int sug = dmd_app_fit_suggested(art, grill);
        lv_label_set_text_fmt(s_fitui->tip, nv_tr(NV_STR_DMDQ_FIT_TIP_FMT), sug >= 0 && sug < n && name[sug] ? name[sug] : "");
    }
}

void quick_fit_cb(lv_event_t *) {
    quick_close();
    if (s_game_id && s_root && dmd_comp_open(s_root)) return;   // the composition of both screens
    if (!s_fitm || !s_game_id) return;
    fitm_prepare();
    fitm_mark();
    lv_obj_remove_flag(s_fitm, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_fitm);
}

void confirm_close(void) {
    if (s_confirm) lv_obj_add_flag(s_confirm, LV_OBJ_FLAG_HIDDEN);
}
void confirm_show(void) {
    char a[64], b[64], t[220];
    clean_name(a, sizeof a, s_game);
    clean_name(b, sizeof b, s_card_name);
    snprintf(t, sizeof t, nv_tr(NV_STR_DMDQ_REPLACE_FMT), a, b);
    lv_label_set_text(s_confirm_txt, t);
    lv_obj_remove_flag(s_confirm, LV_OBJ_FLAG_HIDDEN);
}
void confirm_cancel_cb(lv_event_t *) { confirm_close(); }
// Quit the running table over Wi-Fi; the new one starts when the bridge reports the end (it has the
// USB port back by then).
void confirm_ok_cb(lv_event_t *) {
    confirm_close();
    if (!quit_request()) { nv_toast(NV_NOTE_ERROR, nv_tr(NV_STR_DMDQ_NO_PC)); return; }
    s_pending_launch = s_card_id;
    s_pending_us = esp_timer_get_time();
    browse_set(false);
    s_launching = true;
    s_launch_us = esp_timer_get_time();
    launch_show(s_card_name, true);
}

void show_close(void);

// The game is over (the bridge said so): a table waiting to start goes now.
void game_over(void) {
    dmd_comp_close();   // its pictures were the game's: gone with it
    if (s_show_game) show_close();
    s_game[0] = 0;
    s_game_id = 0;
    s_last_touch_us = esp_timer_get_time();   // the table just played stays: the attract mode waits again
    s_card_asked = false;   // its card again: new scores (and one asked during the game never got there)
    browse_set(false);
    quick_close();
    confirm_close();
    launch_hide();
    dmd_comp_flush();   // before a table starts again: its new composition first
    if (s_pending_launch) {
        char q[64];
        snprintf(q, sizeof q, "{\"q\":\"launch\",\"id\":%d}", s_pending_launch);
        s_pending_launch = 0;
        if (nv_dmd_bridge_send('Q', q)) {
            s_launching = true;
            s_launch_us = esp_timer_get_time();
            launch_show(s_card_name, true);
        }
    }
}

// A tile: a big icon over its name (the quick actions' secondary choices), all the same size.
void tile_make(Btn &b, lv_obj_t *parent, const char *sym, const char *label, lv_event_cb_t cb) {
    const Look &lk = L();
    const uint32_t col = lk.dots ? lk.second : lk.second;
    b.obj = lv_obj_create(parent);
    lv_obj_remove_style_all(b.obj);
    lv_obj_set_size(b.obj, 184, 112);
    lv_obj_set_style_radius(b.obj, lk.dots ? 0 : 18, 0);
    lv_obj_set_style_border_width(b.obj, 2, 0);
    lv_obj_set_style_border_color(b.obj, hx(col), 0);
    lv_obj_set_style_border_opa(b.obj, LV_OPA_60, 0);
    lv_obj_set_style_bg_color(b.obj, hx(col), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(b.obj, LV_OPA_30, LV_STATE_PRESSED);
    lv_obj_set_flex_flow(b.obj, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(b.obj, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(b.obj, 10, 0);
    lv_obj_add_flag(b.obj, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(b.obj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(b.obj, cb, LV_EVENT_CLICKED, nullptr);
    lv_obj_t *ic = text(b.obj, &nv_font_28, col);
    lv_label_set_text(ic, sym);
    b.img = nullptr;
    if (lk.dots && dot_text_wrap(b.dot, label, col, 172, 38, 2)) {
        b.img = lv_image_create(b.obj);
        lv_obj_clear_flag(b.img, LV_OBJ_FLAG_CLICKABLE);
        lv_image_set_src(b.img, &b.dot.dsc);
    } else {
        lv_obj_t *l = text(b.obj, &nv_font_20, lk.dots ? col : 0xFFFFFF);
        lv_obj_set_width(l, 168);
        lv_label_set_long_mode(l, LV_LABEL_LONG_DOT);
        lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
        lv_label_set_text(l, label);
    }
}

// A modal: a dark veil over everything, a card in the middle.
lv_obj_t *modal_make(lv_obj_t *root, lv_obj_t **card_out) {
    const Look &lk = L();
    lv_obj_t *veil = plain(root);
    lv_obj_set_size(veil, lv_pct(100), lv_pct(100));
    lv_obj_add_flag(veil, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_color(veil, hx(0x000000), 0);
    lv_obj_set_style_bg_opa(veil, LV_OPA_70, 0);
    lv_obj_add_flag(veil, LV_OBJ_FLAG_HIDDEN);
    lv_obj_t *card = plain(veil);
    lv_obj_set_size(card, 640, LV_SIZE_CONTENT);
    lv_obj_center(card);
    lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);   // taps on the card do not close it
    lv_obj_set_style_pad_all(card, 28, 0);
    lv_obj_set_style_pad_row(card, 16, 0);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(card, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_bg_color(card, hx(lk.dots ? 0x000000 : 0x061A20), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(card, lk.dots ? 3 : 2, 0);
    lv_obj_set_style_border_color(card, hx(lk.accent), 0);
    lv_obj_set_style_radius(card, lk.dots ? 0 : 24, 0);
    *card_out = card;
    return veil;
}

void quick_build(lv_obj_t *root) {
    const Look &lk = L();
    lv_obj_t *card;
    s_quick = modal_make(root, &card);
    lv_obj_add_event_cb(s_quick, quick_close_cb, LV_EVENT_CLICKED, nullptr);
    nv_gesture_bind(s_quick, LV_DIR_BOTTOM, quick_swipe, nullptr);
    s_quick_dots = lv_image_create(card);
    lv_obj_clear_flag(s_quick_dots, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(s_quick_dots, LV_OBJ_FLAG_HIDDEN);
    s_quick_title = text(card, &nv_font_28, lk.title);
    lv_obj_set_width(s_quick_title, 580);
    lv_label_set_long_mode(s_quick_title, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(s_quick_title, LV_TEXT_ALIGN_CENTER, 0);
    btn_make(s_q_resume, card, LV_SYMBOL_PLAY, nv_tr(NV_STR_DMD_RESUME), true, quick_resume_cb);
    lv_obj_set_width(s_q_resume.obj, 580);
    lv_obj_t *row = plain(card);
    lv_obj_set_size(row, 580, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    // centred with a fixed gap: three tiles fill the row, one or two (no game known) stay together
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, 14, 0);
    tile_make(s_q_tables, row, LV_SYMBOL_LIST, nv_tr(NV_STR_DMDQ_TABLES), quick_tables_cb);
    tile_make(s_q_quit, row, LV_SYMBOL_STOP, nv_tr(NV_STR_DMDQ_QUIT), quick_quit_cb);
    tile_make(s_q_settings, row, LV_SYMBOL_SETTINGS, nv_tr(NV_STR_DMDQ_SETTINGS), quick_settings_cb);
    if (!s_fitui) s_fitui = (FitUi *)heap_caps_calloc(1, sizeof(FitUi), MALLOC_CAP_SPIRAM);
    if (!s_fitui) return;
    btn_make(s_fitui->q, card, LV_SYMBOL_IMAGE, nv_tr(NV_STR_DMDQ_FIT), false, quick_fit_cb);
    lv_obj_set_width(s_fitui->q.obj, 580);
    // its list: as every table, then the five fits of the panel (same names)
    lv_obj_t *fcard;
    s_fitm = modal_make(root, &fcard);
    lv_obj_add_event_cb(s_fitm, fitm_close_cb, LV_EVENT_CLICKED, nullptr);
    nv_gesture_bind(s_fitm, LV_DIR_BOTTOM, fitm_swipe, nullptr);
    lv_obj_t *ft = text(fcard, &nv_font_28, lk.title);
    lv_obj_set_width(ft, 580);
    lv_obj_set_style_text_align(ft, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(ft, nv_tr(NV_STR_DMDQ_FIT));
    s_fitui->tip = text(fcard, &nv_font_20, lk.second);   // "Suggested: ..." (fitm_prepare)
    lv_obj_set_width(s_fitui->tip, 580);
    lv_obj_set_style_text_align(s_fitui->tip, LV_TEXT_ALIGN_CENTER, 0);
    char fits[200];
    snprintf(fits, sizeof fits, "%s", nv_tr(NV_STR_DMDQ_FIT_NAMES));
    char *names[kFitChoices] = {(char *)nv_tr(NV_STR_DMDQ_FIT_ALL)};
    int nn = 1;
    for (char *q = fits; nn < kFitChoices && q && *q;) {
        names[nn++] = q;
        q = strchr(q, '\n');
        if (q) *q++ = 0;
    }
    lv_obj_t *grid = plain(fcard);   // two columns: eight choices fit the panel's height
    lv_obj_set_size(grid, 580, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(grid, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(grid, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(grid, 12, 0);
    for (int i = 0; i < nn; i++) {
        btn_make(s_fitui->b[i], grid, i ? LV_SYMBOL_IMAGE : LV_SYMBOL_REFRESH, names[i], false, fitm_pick_cb, true);
        lv_obj_set_width(s_fitui->b[i].obj, 284);
    }
    btn_make(s_fitui->rb[0], grid, LV_SYMBOL_OK, nv_tr(NV_STR_DMDQ_RB_ON), false, fitm_rb_cb, true);
    lv_obj_set_width(s_fitui->rb[0].obj, 284);
    btn_make(s_fitui->rb[1], grid, LV_SYMBOL_CLOSE, nv_tr(NV_STR_DMDQ_RB_OFF), false, fitm_rb_cb, true);
    lv_obj_set_width(s_fitui->rb[1].obj, 284);

    s_find = modal_make(root, &s_find_card);
    lv_obj_set_width(s_find_card, 760);
    lv_obj_add_event_cb(s_find, find_close_cb, LV_EVENT_CLICKED, nullptr);
    nv_gesture_bind(s_find, LV_DIR_BOTTOM, find_swipe, nullptr);

    s_confirm = modal_make(root, &card);
    lv_obj_set_width(card, 776);
    lv_obj_add_event_cb(s_confirm, confirm_cancel_cb, LV_EVENT_CLICKED, nullptr);
    s_confirm_txt = text(card, &nv_font_20, lk.dots ? lk.stats : 0xFFFFFF);
    lv_obj_set_width(s_confirm_txt, 680);
    lv_label_set_long_mode(s_confirm_txt, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(s_confirm_txt, LV_TEXT_ALIGN_CENTER, 0);
    row = plain(card);   // (wide enough for the dot-matrix buttons; a long translation wraps to two rows)
    lv_obj_set_size(row, 720, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, 18, 0);
    lv_obj_set_style_pad_row(row, 12, 0);
    btn_make(s_c_ok, row, LV_SYMBOL_PLAY, nv_tr(NV_STR_DMDQ_REPLACE_OK), true, confirm_ok_cb);
    btn_make(s_c_cancel, row, nullptr, nv_tr(NV_STR_CANCEL), false, confirm_cancel_cb);
}

// ------------------------------------------------------------------ attract mode

void attract_stop(void) {
    s_attract = false;
    if (s_attract_txt) lv_obj_add_flag(s_attract_txt, LV_OBJ_FLAG_HIDDEN);
}

// Any press anywhere (the app keeps the display "active" itself, so LVGL's own idle time is useless here).
void touch_cb(lv_event_t *) {
    s_last_touch_us = esp_timer_get_time();
    if (s_attract) attract_stop();
}

// ------------------------------------------------------------------ the showcase

int jint(const cJSON *o, const char *k);
const char *jstr(const cJSON *o, const char *k);

const char *show_kind_label(int sub) {
    static const nv_str_id_t ids[kShowN] = {NV_STR_DMDS_BACKGLASS, NV_STR_DMDS_INFO, NV_STR_DMDS_HELP, NV_STR_DMDS_DMD,
                                            NV_STR_DMDS_TOPPER};
    return nv_tr(ids[sub]);
}

void show_pips(void) {   // where in the list: one dot per picture, the current one lit and longer
    const Look &lk = L();
    lv_obj_clean(s_show_pips);
    if (s_show_n < 2) return;
    for (int i = 0; i < s_show_n; i++) {
        lv_obj_t *d = plain(s_show_pips);
        const bool on = i == s_show_at;
        lv_obj_set_size(d, on ? 22 : 8, 8);
        lv_obj_set_style_radius(d, lk.dots ? 0 : 4, 0);
        lv_obj_set_style_bg_color(d, hx(on ? lk.accent : lk.dim), 0);
        lv_obj_set_style_bg_opa(d, LV_OPA_COVER, 0);
    }
}

void show_slide_exec(void *obj, int32_t v) { lv_obj_set_x((lv_obj_t *)obj, v); }

// Picture `at` of the list on screen: it slides in from the right over the previous one.
bool show_put(int at) {
    Img *m = show_find(s_show_id, s_show_list[at]);
    if (!m) return false;
    const int top = s_show_top ^ 1;
    lv_obj_t *in = s_show_img[top];
    lv_image_set_src(in, &m->dsc);
    lv_obj_move_to_index(in, 1);   // above the other picture, below the caption
    const bool first = s_show_at < 0;
    s_show_top = top;
    s_show_at = at;
    s_show_us = esp_timer_get_time();
    lv_label_set_text(s_show_kind, show_kind_label(s_show_list[at]));
    show_pips();
    if (first) { lv_obj_set_x(in, 0); return true; }
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, in);
    lv_anim_set_exec_cb(&a, show_slide_exec);
    lv_anim_set_values(&a, kScrW, 0);
    lv_anim_set_duration(&a, 420);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_start(&a);
    return true;
}

void show_close(void) {
    if (!s_show_id) return;
    s_show_id = 0;
    s_show_game = false;
    for (lv_obj_t *o : s_show_img) {
        lv_anim_delete(o, show_slide_exec);
        lv_image_set_src(o, nullptr);
    }
    if (s_show) lv_obj_add_flag(s_show, LV_OBJ_FLAG_HIDDEN);
}

// A tap: the menu, on the table being shown (with its card), as if chosen there.
void show_tap_cb(lv_event_t *) {
    if (s_show_game) { dmd_menu_quick_open(); return; }
    const int id = s_show_id;
    show_close();
    s_last_touch_us = esp_timer_get_time();
    for (int i = 0; i < s_nitems; i++)
        if (s_items[i].id == id) { select_item(i); return; }
    char q[48];   // not in this list: its card on the stage
    snprintf(q, sizeof q, "{\"q\":\"table\",\"id\":%d}", id);
    s_random_pending = nv_dmd_bridge_send('Q', q);
}

// Start the showcase of a table: media = the screens it has (JSON array). ingame: the game being played.
void show_start(int id, const char *name, const cJSON *media, bool ingame) {
    show_close();
    s_show_n = 0;
    const cJSON *it;
    cJSON_ArrayForEach(it, media) {
        for (int i = 0; cJSON_IsString(it) && i < kShowN && s_show_n < kShowN; i++)
            if (!strcmp(it->valuestring, kShowScreens[i])) s_show_list[s_show_n++] = (int8_t)i;
    }
    if (!s_show_n) return;
    s_show_id = id;
    s_show_at = -1;
    s_show_game = ingame;
    snprintf(s_show_name, sizeof s_show_name, "%s", name);
    char clean[64];
    clean_name(clean, sizeof clean, s_show_name);
    if (L().dots && dot_text(s_dot_show, clean, L().title, 640, 36, 4)) {
        lv_image_set_src(s_show_name_dots, &s_dot_show.dsc);
        lv_obj_remove_flag(s_show_name_dots, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_show_name_lbl, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_label_set_text(s_show_name_lbl, clean);
        lv_obj_remove_flag(s_show_name_lbl, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_show_name_dots, LV_OBJ_FLAG_HIDDEN);
    }
    // in game the line under the name says what a touch does there: the quick actions
    char hint[96];
    if (ingame) {
        char t[64];
        snprintf(t, sizeof t, nv_tr(NV_STR_DMDM_INGAME_FMT), "");
        size_t n = strlen(t);
        while (n && (t[n - 1] == ' ' || t[n - 1] == ':')) t[--n] = 0;
        snprintf(hint, sizeof hint, LV_SYMBOL_PLAY "  %s", t);
    } else {
        snprintf(hint, sizeof hint, "%s", nv_tr(NV_STR_DMDS_HINT));
    }
    lv_label_set_text(s_show_hint, hint);
    lv_label_set_text(s_show_kind, "");
    lv_obj_clean(s_show_pips);
    lv_obj_remove_flag(s_show, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_show);
    if (s_attract) attract_stop();
    s_wait_id = 0;   // its pictures go first
    if (!show_put(0)) pump_images();   // already here (shown a moment ago): at once
}

// 'S' from the bridge: the player moved in Popper with the cabinet's controls.
void nav_close(void);
void nav_show(const char *kind, const char *name, int id);

void on_showcase(const char *json) {
    cJSON *o = cJSON_Parse(json);
    if (!o) return;
    const int id = jint(o, "id");
    const cJSON *nav = cJSON_GetObjectItem(o, "nav");
    const cJSON *media = cJSON_GetObjectItem(o, "media");
    if (s_game[0]) {
        // a game runs: its own screen stays
    } else if (cJSON_IsTrue(cJSON_GetObjectItem(o, "idle"))) {   // Popper in its attract mode again: our menu
        show_close();
        nav_close();
    } else {
        // the player is at the cabinet: this first, ahead of a background fetch still on the cable (that
        // answer still lands, it is only not waited for), and no prefetch for a while
        s_wait_id = 0;
        s_last_touch_us = esp_timer_get_time();
        if (cJSON_IsString(nav)) {
            nav_show(nav->valuestring, jstr(o, "name"), 0);
        } else if (id && id != s_show_id) {
            nav_close();
            show_start(id, jstr(o, "name"), media, false);
            if (!s_show_id) nav_show("table", jstr(o, "name"), id);   // no horizontal picture: its wheel and name
        }
    }
    cJSON_Delete(o);
}

void show_build(void) {
    const Look &lk = L();
    s_show = plain(s_menu);
    lv_obj_set_size(s_show, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_color(s_show, hx(0x000000), 0);
    lv_obj_set_style_bg_opa(s_show, LV_OPA_COVER, 0);
    lv_obj_add_flag(s_show, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_show, show_tap_cb, LV_EVENT_CLICKED, nullptr);
    lv_obj_add_flag(s_show, LV_OBJ_FLAG_HIDDEN);
    for (lv_obj_t *&o : s_show_img) {
        o = lv_image_create(s_show);
        lv_obj_set_pos(o, 0, 0);
        lv_obj_clear_flag(o, LV_OBJ_FLAG_CLICKABLE);
    }
    s_show_top = 0;
    // the caption: a dark band at the bottom — name on the left, the kind of picture and where we are on
    // the right, a line under it that says a touch opens the menu
    lv_obj_t *band = plain(s_show);
    lv_obj_set_size(band, kScrW, 84);
    lv_obj_align(band, LV_ALIGN_BOTTOM_MID, 0, 0);
    grad(band, 0x000000, 0x000000, LV_GRAD_DIR_VER);
    lv_obj_set_style_bg_opa(band, LV_OPA_70, 0);
    if (lk.dots) {
        lv_obj_set_style_border_side(band, LV_BORDER_SIDE_TOP, 0);
        lv_obj_set_style_border_width(band, 2, 0);
        lv_obj_set_style_border_color(band, hx(lk.accent), 0);
    }
    s_show_name_dots = lv_image_create(band);
    lv_obj_clear_flag(s_show_name_dots, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_align(s_show_name_dots, LV_ALIGN_LEFT_MID, 24, -8);
    s_show_name_lbl = text(band, &nv_font_28, lk.title);
    lv_obj_set_width(s_show_name_lbl, 640);
    lv_label_set_long_mode(s_show_name_lbl, LV_LABEL_LONG_DOT);
    lv_obj_align(s_show_name_lbl, LV_ALIGN_LEFT_MID, 24, -10);
    s_show_hint = text(band, &nv_font_14, lk.dim);
    lv_obj_align(s_show_hint, LV_ALIGN_BOTTOM_LEFT, 24, -6);
    s_show_kind = text(band, &nv_font_20, lk.meta);
    lv_obj_align(s_show_kind, LV_ALIGN_RIGHT_MID, -24, -12);
    s_show_pips = plain(band);
    lv_obj_set_size(s_show_pips, LV_SIZE_CONTENT, 8);
    lv_obj_set_flex_flow(s_show_pips, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(s_show_pips, 6, 0);
    lv_obj_align(s_show_pips, LV_ALIGN_BOTTOM_RIGHT, -24, -12);
}

bool nav_visible(void) { return s_nav && !lv_obj_has_flag(s_nav, LV_OBJ_FLAG_HIDDEN); }

void nav_close(void) {
    s_nav_id = 0;
    if (!s_nav) return;
    lv_obj_add_flag(s_nav, LV_OBJ_FLAG_HIDDEN);
    lv_image_set_src(s_nav_wheel, nullptr);
}

// A tap: the menu (on the table, when the card is one).
void nav_tap_cb(lv_event_t *) {
    const int id = s_nav_id;
    nav_close();
    s_last_touch_us = esp_timer_get_time();
    for (int i = 0; id && i < s_nitems; i++)
        if (s_items[i].id == id) { select_item(i); return; }
}

void nav_build(void) {
    const Look &lk = L();
    s_nav = plain(s_menu);
    lv_obj_set_size(s_nav, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_color(s_nav, hx(0x000000), 0);
    lv_obj_set_style_bg_opa(s_nav, LV_OPA_COVER, 0);
    lv_obj_add_flag(s_nav, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_nav, nav_tap_cb, LV_EVENT_CLICKED, nullptr);
    lv_obj_set_flex_flow(s_nav, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_nav, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(s_nav, 22, 0);
    lv_obj_add_flag(s_nav, LV_OBJ_FLAG_HIDDEN);
    s_nav_wheel = lv_image_create(s_nav);   // a table: its wheel
    lv_obj_clear_flag(s_nav_wheel, LV_OBJ_FLAG_CLICKABLE);
    s_nav_icon = plain(s_nav);              // an entry: its sign in a ring (a square bezel in the DMD look)
    lv_obj_set_size(s_nav_icon, 72, 72);
    lv_obj_set_style_radius(s_nav_icon, lk.dots ? 0 : LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(s_nav_icon, 3, 0);
    lv_obj_set_style_border_color(s_nav_icon, hx(lk.accent), 0);
    lv_obj_set_style_bg_color(s_nav_icon, hx(lk.accent), 0);
    lv_obj_set_style_bg_opa(s_nav_icon, LV_OPA_10, 0);
    s_nav_sym = text(s_nav_icon, &nv_font_28, lk.accent);
    lv_obj_center(s_nav_sym);
    s_nav_dots = lv_image_create(s_nav);    // the name as big dots (DMD look)
    lv_obj_clear_flag(s_nav_dots, LV_OBJ_FLAG_CLICKABLE);
    s_nav_lbl = text(s_nav, &nv_font_28, lk.dots ? lk.title : 0xFFFFFF);
    lv_obj_set_width(s_nav_lbl, 900);
    lv_label_set_long_mode(s_nav_lbl, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(s_nav_lbl, LV_TEXT_ALIGN_CENTER, 0);
    s_nav_sub = text(s_nav, &nv_font_20, lk.dim);
}

// kind: "back", "exit", "playlist", "menu" (Popper's entries) or "table"; name: Popper's own label, or
// the table's name; id: the table (0 for an entry).
void nav_show(const char *kind, const char *name, int id) {
    if (!s_nav) return;
    show_close();
    attract_stop();
    const Look &lk = L();
    s_nav_id = id;
    const bool table = !strcmp(kind, "table");
    const bool back = !strcmp(kind, "back"), quit = !strcmp(kind, "exit"), list = !strcmp(kind, "playlist");
    char big[96], sub[128];
    if (table) {
        clean_name(big, sizeof big, name);
        snprintf(sub, sizeof sub, "%s", nv_tr(NV_STR_DMDS_HINT));
    } else if (back || quit) {   // the action in the board's language; Popper's own words under it
        snprintf(big, sizeof big, "%s", nv_tr(back ? NV_STR_DMDN_BACK : NV_STR_DMDN_EXIT));
        snprintf(sub, sizeof sub, nv_tr(NV_STR_DMDN_ON_FMT), *name ? name : big);
    } else {                     // a playlist / another entry: its name, and what it is
        snprintf(big, sizeof big, "%s", *name ? name : nv_tr(list ? NV_STR_DMDN_PLAYLIST : NV_STR_DMDN_MENU));
        snprintf(sub, sizeof sub, nv_tr(NV_STR_DMDN_ON_FMT), nv_tr(list ? NV_STR_DMDN_PLAYLIST : NV_STR_DMDN_MENU));
    }
    // a table: its wheel (memory, or the card); an entry: its sign
    Img *w = nullptr;
    if (table && !(w = img_find(id, 'w', kBigImg)) && sd_fetch(id, 'w', kBigImg, kBigImg, -1)) w = img_find(id, 'w', kBigImg);
    lv_image_set_src(s_nav_wheel, w ? &w->dsc : nullptr);
    if (w) lv_obj_remove_flag(s_nav_wheel, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(s_nav_wheel, LV_OBJ_FLAG_HIDDEN);
    if (table) {
        lv_obj_add_flag(s_nav_icon, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_label_set_text(s_nav_sym, back ? LV_SYMBOL_LEFT : quit ? LV_SYMBOL_POWER : list ? LV_SYMBOL_LIST : LV_SYMBOL_HOME);
        lv_obj_remove_flag(s_nav_icon, LV_OBJ_FLAG_HIDDEN);
    }
    if (lk.dots && dot_text(s_dot_nav, big, lk.title, 940, table ? 64 : 110, table ? 6 : 10)) {
        lv_image_set_src(s_nav_dots, &s_dot_nav.dsc);
        lv_obj_remove_flag(s_nav_dots, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_nav_lbl, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_label_set_text(s_nav_lbl, big);
        lv_obj_remove_flag(s_nav_lbl, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_nav_dots, LV_OBJ_FLAG_HIDDEN);
    }
    lv_label_set_text(s_nav_sub, sub);
    lv_obj_remove_flag(s_nav, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_nav);
    if (table && !w) pump_images();   // its wheel from the bridge
}

// ------------------------------------------------------------------ bridge messages

int jint(const cJSON *o, const char *k) {
    const cJSON *v = cJSON_GetObjectItem(o, k);
    return cJSON_IsNumber(v) ? v->valueint : 0;
}
const char *jstr(const cJSON *o, const char *k) {
    const cJSON *v = cJSON_GetObjectItem(o, k);
    return cJSON_IsString(v) ? v->valuestring : "";
}

void on_table(const char *json) {
    cJSON *o = cJSON_Parse(json);
    if (!o) return;
    const int id = jint(o, "id");
    const bool random = s_random_pending;
    s_random_pending = false;
    if (random) {   // "surprise me": the bar goes there if the table is in this list, else only the stage
        int at = -1;
        for (int i = 0; i < s_nitems; i++)
            if (s_items[i].id == id) { at = i; break; }
        if (at >= 0) select_item(at);
        else stage_show(id, jstr(o, "name"), jint(o, "year"), jstr(o, "maker"), jint(o, "plays"), jint(o, "secs"),
                        jint(o, "ago"), jstr(o, "last"), cJSON_IsTrue(cJSON_GetObjectItem(o, "fav")), "");
        s_card_asked = true;
        lv_obj_remove_flag(s_another.obj, LV_OBJ_FLAG_HIDDEN);
    }
    if (id != s_card_id) { cJSON_Delete(o); return; }   // the selection moved on meanwhile
    if (cJSON_IsTrue(cJSON_GetObjectItem(o, "fav"))) lv_obj_remove_flag(s_stage_fav, LV_OBJ_FLAG_HIDDEN);
    const char *sc = jstr(o, "scores");
    if (*sc) {   // the first lines only: the stage is not a scoreboard
        char sb[320];
        int n = 0, lines = 0;
        for (const char *p = sc; *p && n < (int)sizeof sb - 1; p++) {
            if (*p == '\n' && ++lines >= 4) break;
            sb[n++] = *p;
        }
        sb[n] = 0;
        lv_label_set_text(s_stage_scores, sb);
    }
    if (cJSON_IsTrue(cJSON_GetObjectItem(o, "help"))) lv_obj_remove_flag(s_helpb.obj, LV_OBJ_FLAG_HIDDEN);
    if (!cJSON_IsTrue(cJSON_GetObjectItem(o, "wheel"))) set_missing(id, 'w');
    cJSON_Delete(o);
}

const char *emu_name(int id) {
    for (int i = 0; i < s_nemus; i++)
        if (s_emus[i].id == id) return s_emus[i].name;
    return "";
}

// s_items = the tables of s_all on the chosen system; the selection stays on keep_id when it is there.
void list_apply(int keep_id) {
    if (!s_items || !s_all) return;
    if (s_animating) {   // a slide under way (a tap, a swipe): it ends on the table it was going to
        lv_anim_delete(&s_drag, anim_exec);
        s_animating = false;
        if (s_anim_id) keep_id = s_anim_id;
    }
    s_pf = 0;
    s_pf_left = s_nall;
    s_nitems = 0;
    for (int i = 0; i < s_nall; i++)
        if (!s_filter || s_all[i].emu == s_filter) s_items[s_nitems++] = s_all[i];
    int sel = 0;
    for (int i = 0; keep_id && i < s_nitems; i++)
        if (s_items[i].id == keep_id) { sel = i; break; }
    // nothing to show: why, in a few words
    show_note(s_nitems                    ? ""
              : s_nall                    ? nv_tr(NV_STR_DMDF_EMPTY)
              : s_list == 3               ? nv_tr(NV_STR_DMDM_EMPTY_FAV)
                                          : nv_tr(NV_STR_DMDM_EMPTY));
    if (s_nitems) select_item(sel);
    else {
        bar_fill();
        update_count();
        s_card_id = 0;
        lv_obj_add_flag(s_stage, LV_OBJ_FLAG_HIDDEN);
        bg_clear();
    }
}

void on_list(const char *json) {
    if (!s_items || !s_all) return;
    const uint32_t hsh = fnv(json);
    if (hsh == s_list_hash && s_nall) return;   // nothing changed: the screen stays as it is
    s_list_hash = hsh;
    if (s_sd_ok) {
        char nm[96];
        cJSON *o = cJSON_Parse(json);
        const char *which = o ? jstr(o, "list") : "";
        for (int i = 0; i < kNLists; i++)
            if (!strcmp(which, kLists[i])) { list_file(nm, sizeof nm, i); sd_write(nm, json, strlen(json)); }
        if (o) cJSON_Delete(o);
    }
    cJSON *o = cJSON_Parse(json);
    if (!o) return;
    const cJSON *items = cJSON_GetObjectItem(o, "items");
    const int keep_id = s_restore_id ? s_restore_id : s_nitems ? s_items[s_sel].id : 0;
    s_restore_id = 0;
    s_nall = 0;
    const cJSON *it;
    cJSON_ArrayForEach(it, items) {
        if (s_nall >= kMaxItems) break;
        Item &m = s_all[s_nall++];
        m.id = jint(it, "id");
        m.year = jint(it, "year");
        m.plays = jint(it, "plays");
        m.secs = jint(it, "secs");
        m.emu = jint(it, "emu");
        m.ago = cJSON_IsNumber(cJSON_GetObjectItem(it, "ago")) ? jint(it, "ago") : -1;
        m.fav = cJSON_IsTrue(cJSON_GetObjectItem(it, "fav"));
        snprintf(m.name, sizeof m.name, "%s", jstr(it, "name"));
        snprintf(m.maker, sizeof m.maker, "%s", jstr(it, "maker"));
        snprintf(m.added, sizeof m.added, "%s", jstr(it, "added"));
    }
    cJSON_Delete(o);
    list_apply(keep_id);   // the same list again (a game ended): stay on the table
}

// A stored picture goes where the screen wants it.
void img_apply(Img *m) {
    if (!m) return;
    if (m->kind == 'g') {   // the game art: the picture is made when the app asks for it
        s_art_id = 0;
    } else if (m->kind == 'f') {   // FullDMD: where the DMD goes, found once here
        s_fart_id = m->id;
        m->has_win = !m->alpha && dmd_find_window((const uint16_t *)m->px, m->w, m->h, &m->win);
        nv_log_write(NV_LOG_INFO, "dmd", "FullDMD art of %d: %dx%d, window %s %d,%d %dx%d", m->id, m->w, m->h,
                     m->has_win ? "at" : "not found", m->win.x, m->win.y, m->win.w, m->win.h);
    } else if (m->kind == 'r') {   // the backglass with its grill: the housing, searched in the grill only
        m->has_win = false;
        s_rgrill_id = m->id;
        // only in a grill: searched in a picture without one it finds dark art and black edges (tried on all
        // 68 backglasses of the cabinet: 1 housing right, 13 wrong)
        const int gx = s_rgrill[0], gy = s_rgrill[1], gw = s_rgrill[2], gh = s_rgrill[3];
        if (!m->alpha && gw >= 16 && gh >= 16 && gx >= 0 && gy >= 0 && gx + gw <= m->w && gy + gh <= m->h) {
            uint16_t *sub = (uint16_t *)heap_caps_malloc((size_t)gw * gh * 2, MALLOC_CAP_SPIRAM);
            if (sub) {
                const uint16_t *src = (const uint16_t *)m->px;
                for (int y = 0; y < gh; y++) memcpy(sub + (size_t)y * gw, src + (size_t)(gy + y) * m->w + gx, (size_t)gw * 2);
                if (dmd_find_housing(sub, gw, gh, &m->win)) {
                    m->win.x += gx;
                    m->win.y += gy;
                    m->has_win = true;
                }
                heap_caps_free(sub);
            }
        }
        nv_log_write(NV_LOG_INFO, "dmd", "backglass with grill of %d: grill %d,%d %dx%d, housing %s %d,%d %dx%d", m->id,
                     gx, gy, gw, gh, m->has_win ? "at" : "not found", m->win.x, m->win.y, m->win.w, m->win.h);
    } else if (m->kind == 'm') {
        ambient_bars((uint16_t *)m->px, m->w, m->h);
        if (m->id == s_show_id && s_show_at < 0 && m->sub == s_show_list[0]) show_put(0);
    } else if (m->kind == 'h') {
        if (s_card_id == m->id) help_show(m);
    } else if (m->kind == 'b') {
        if (s_card_id == m->id) bg_build(*m);
    } else if (m->w == kBigImg) {
        if (s_card_id == m->id) stage_wheel();
        if (s_nav_id == m->id && s_nav_wheel) {
            lv_image_set_src(s_nav_wheel, &m->dsc);
            lv_obj_remove_flag(s_nav_wheel, LV_OBJ_FLAG_HIDDEN);
        }
    } else {
        const int id = m->id;
        for (int k = 0; k < kSlots; k++)
            if (s_slot_id[k] == id) slot_src(k, s_slot_big[k]);
    }
    if (s_zombie) {   // a fresher copy replaced one on screen: every image still showing the old one moves
        const void *old = &s_zombie->dsc;
        lv_obj_t *objs[] = {s_stage_img, s_help_img, s_show_img[0], s_show_img[1], s_launch_wheel, s_nav_wheel};
        for (lv_obj_t *o : objs)
            if (o && lv_image_get_src(o) == old) lv_image_set_src(o, &m->dsc);
        for (int k = 0; k < kSlots; k++)
            if (s_slot[k] && lv_image_get_src(s_slot[k]) == old) lv_image_set_src(s_slot[k], &m->dsc);
        img_drop(*s_zombie);
        s_zombie = nullptr;
    }
}

// A picture's table, kind, width and showcase screen, from its header, without decoding it.
bool img_peek(const uint8_t *p, uint32_t n, int *id, char *kind, int *w, int *sub) {
    if (n < 2) return false;
    const uint32_t jl = p[0] | p[1] << 8;
    if (jl > n - 2 || jl > 512) return false;
    char js[513];
    memcpy(js, p + 2, jl);
    js[jl] = 0;
    cJSON *o = cJSON_Parse(js);
    if (!o) return false;
    const char *ks = cJSON_GetStringValue(cJSON_GetObjectItem(o, "kind"));
    ks = ks ? ks : "";
    *id = cJSON_IsNumber(cJSON_GetObjectItem(o, "id")) ? cJSON_GetObjectItem(o, "id")->valueint : 0;
    *w = cJSON_IsNumber(cJSON_GetObjectItem(o, "w")) ? cJSON_GetObjectItem(o, "w")->valueint : 0;
    *kind = !strcmp(ks, "wheel") ? 'w' : !strcmp(ks, "backglass") ? 'b' : !strcmp(ks, "help") ? 'h' : !strcmp(ks, "media") ? 'm' : 0;
    *sub = -1;
    const char *sc = cJSON_GetStringValue(cJSON_GetObjectItem(o, "screen"));
    for (int i = 0; *kind == 'm' && sc && i < kShowN; i++)
        if (!strcmp(sc, kShowScreens[i])) *sub = i;
    cJSON_Delete(o);
    return *id && *kind && (*kind != 'm' || *sub >= 0);
}

// Wanted on screen now (or already in memory, to be refreshed): else the picture only goes onto the card.
bool img_wanted(int id, char kind, int w, int sub) {
    if (kind == 'm') return id == s_show_id || show_find(id, sub);
    if (img_find(id, kind, w)) return true;
    if (kind == 'b' || kind == 'h') return id == s_card_id;
    if (id == s_card_id || id == s_nav_id) return true;   // a wheel: the stage's, the card's, the bar's
    for (int k = 0; k < kSlots; k++)
        if (s_slot_id[k] == id) return true;
    return false;
}

void on_image(const uint8_t *p, uint32_t n) {
    s_wait_id = 0;
    {
        // a picture not wanted on screen (a prefetch, or the selection moved on meanwhile): onto the card as
        // it came, not decoded (no 1.2 MB of pixels made, nothing else pushed out of the memory)
        int id = 0, w = 0, sub = -1;
        char kind = 0;
        if (s_sd_ok && img_peek(p, n, &id, &kind, &w, &sub) && !img_wanted(id, kind, w, sub)) {
            char nm[96];
            sd_name(nm, sizeof nm, id, kind, w, sub);
            sd_write(nm, p, n);
            pump_images();
            return;
        }
    }
    Img *m = img_store(p, n);
    if (m && s_sd_ok && (m->kind == 'w' || m->kind == 'b' || m->kind == 'h' || m->kind == 'm' || m->kind == 'f' ||
                         m->kind == 'r')) {
        char nm[96];   // as it came (compressed): read back next time instead of crossing the cable
        sd_name(nm, sizeof nm, m->id, m->kind, m->w, m->sub);
        sd_write(nm, p, n);
    }
    img_apply(m);
    pump_images();
}

void on_game(const char *json) {
    cJSON *o = cJSON_Parse(json);
    if (!o) return;
    if (!strcmp(jstr(o, "game"), "start")) {
        snprintf(s_game, sizeof s_game, "%s", jstr(o, "name"));
        s_game_id = jint(o, "id");
        s_game_us = esp_timer_get_time();
        s_game_dmd = !cJSON_IsFalse(cJSON_GetObjectItem(o, "dmd"));
        s_launching = false;   // it started
        show_close();
        nav_close();
        close_help();
        attract_stop();
        const cJSON *sc = cJSON_GetObjectItem(o, "showcase");
        const cJSON *dmd = cJSON_GetObjectItem(o, "dmd");
        if (cJSON_IsFalse(dmd) && cJSON_IsObject(sc) && s_game_id)   // no DMD for it: its pictures meanwhile
            show_start(s_game_id, s_game, cJSON_GetObjectItem(sc, "media"), true);
        if (!s_show_id) launch_show(s_game, false);
    } else {
        game_over();
        s_want_list = true;   // stats changed: the list again (the selection stays on the table)
    }
    cJSON_Delete(o);
}

void send_prefs(void) {
    nv_dmd_bridge_send('Q', s_want_full ? "{\"q\":\"prefs\",\"fulldmd\":true}" : "{\"q\":\"prefs\",\"fulldmd\":false}");
}

void handle(uint8_t type, uint8_t *data, uint32_t len) {
    switch (type) {
    case 'H': {
        cJSON *o = cJSON_Parse((const char *)data);
        if (o) {
            const cJSON *g = cJSON_GetObjectItem(o, "game");
            if (cJSON_IsObject(g)) {
                snprintf(s_game, sizeof s_game, "%s", jstr(g, "name"));
                s_game_id = jint(g, "id");
                launch_show(s_game, false);
            } else if (s_game[0]) {
                game_over();
            }
            const cJSON *emus = cJSON_GetObjectItem(o, "emus");
            if (cJSON_IsArray(emus)) {
                s_nemus = 0;
                const cJSON *e;
                cJSON_ArrayForEach(e, emus) {
                    if (s_nemus >= 16) break;
                    s_emus[s_nemus].id = jint(e, "id");
                    snprintf(s_emus[s_nemus].name, sizeof s_emus[0].name, "%s", jstr(e, "name"));
                    s_nemus++;
                }
            }
            // the PC's address, kept: after a restart the board can still close a game over Wi-Fi before
            // the bridge is back on the cable (dmdext has it while a table runs)
            const char *ip = jstr(o, "ip");
            if (*ip && strcmp(ip, s_pc_ip)) {
                snprintf(s_pc_ip, sizeof s_pc_ip, "%s", ip);
                nv_config_set_str("dmd_pc_ip", s_pc_ip);
            }
            const cJSON *s2 = cJSON_GetObjectItem(o, "s2");   // the backglass monitor (null: none)
            int16_t v[4] = {0, 0, 0, 0};
            for (int i = 0; cJSON_IsArray(s2) && i < 4; i++) {
                const cJSON *e = cJSON_GetArrayItem(s2, i);
                v[i] = cJSON_IsNumber(e) ? (int16_t)e->valueint : 0;
            }
            if (memcmp(v, s_s2, sizeof v)) {
                memcpy(s_s2, v, sizeof v);
                char t[40];
                snprintf(t, sizeof t, "%d,%d,%d,%d", v[0], v[1], v[2], v[3]);
                nv_config_set_str("dmd_s2", t);
            }
            if (jint(o, "web") && jint(o, "web") != s_pc_web) {
                s_pc_web = jint(o, "web");
                nv_config_set_int("dmd_pc_web", s_pc_web);
            }
            cJSON_Delete(o);
        }
        if (!s_hello || !s_nitems) s_want_list = true;
        s_hello = true;
        send_prefs();
        dmd_comp_flush();   // second-screen compositions chosen during a game (the cable is ours again)
        break;
    }
    case 'L': on_list((const char *)data); break;
    case 'T': on_table((const char *)data); break;
    case 'I': on_image(data, len); break;
    case 'G': on_game((const char *)data); break;
    case 'S': on_showcase((const char *)data); break;
    case 'K':   // the card's copy is still the bridge's: nothing to do
        s_wait_id = 0;
        pump_images();
        break;
    case 'R': {
        cJSON *o = cJSON_Parse((const char *)data);
        if (o && !strcmp(jstr(o, "q"), "exit")) {
            if (!cJSON_IsTrue(cJSON_GetObjectItem(o, "ok"))) nv_toast(NV_NOTE_ERROR, jstr(o, "err"));
        } else if (o && !strcmp(jstr(o, "q"), "s2")) {
            dmd_comp_on_result(jint(o, "id"), cJSON_IsTrue(cJSON_GetObjectItem(o, "ok")), jstr(o, "err"));
        } else if (o && !cJSON_IsTrue(cJSON_GetObjectItem(o, "ok"))) {
            s_launching = false;
            launch_hide();
            char t[200];
            snprintf(t, sizeof t, nv_tr(NV_STR_DMDM_LAUNCH_FAIL_FMT), jstr(o, "err"));
            nv_toast(NV_NOTE_ERROR, t);
        }
        if (o) cJSON_Delete(o);
        break;
    }
    case 'E': {
        cJSON *o = cJSON_Parse((const char *)data);
        if (o && !strcmp(jstr(o, "q"), "image")) {
            const char *k = jstr(o, "kind");
            const int mid = jint(o, "id");
            const char kc = !strcmp(k, "help")        ? 'h'
                            : !strcmp(k, "backglass") ? 'b'
                            : !strcmp(k, "gamebg")    ? 'g'
                            : !strcmp(k, "media")     ? 'm'
                            : !strcmp(k, "fulldmd")   ? 'f'
                            : !strcmp(k, "b2sgrill")  ? 'r'
                            : !strcmp(k, "wheel")     ? 'w'
                                                      : 0;
            int sub = -1;
            if (kc == 'm')
                for (int i = 0; i < kShowN; i++)
                    if (!strcmp(jstr(o, "screen"), kShowScreens[i])) sub = i;
            if (kc && (kc != 'm' || sub >= 0)) set_missing(mid, kc, sub);
            if (kc == 'm' && sub >= 0 && mid == s_show_id) {   // that screen out of the showcase
                int n = 0;
                for (int i = 0; i < s_show_n; i++)
                    if (s_show_list[i] != sub) s_show_list[n++] = s_show_list[i];
                s_show_n = n;
                if (!n) show_close();
                else if (s_show_at < 0) show_put(0);   // the next one, if it is here already
                else if (s_show_at >= n) s_show_at = 0;
            }
            if (kc == 'w') {
                if (mid == s_card_id) stage_wheel();
                for (int kk = 0; kk < kSlots; kk++)
                    if (s_slot_id[kk] == mid) slot_src(kk, s_slot_big[kk]);
            }
            s_wait_id = 0;
            pump_images();
        }
        if (o) cJSON_Delete(o);
        break;
    }
    default: break;
    }
}

void dots_free_all(void) {
    DotText *all[] = {&s_dot_title, &s_dot_name, &s_dot_meta, &s_dot_st1, &s_dot_st2, &s_dot_fav, &s_dot_count,
                      &s_dot_launch, &s_dot_launch_txt, &s_dot_attract, &s_dot_time, &s_dot_nav};
    for (DotText &d : s_dot_find) dot_free(d);
    s_nfind = 0;
    for (DotText *d : all) dot_free(*d);
    for (auto &t : s_dot_tab) { dot_free(t[0]); dot_free(t[1]); }
    btn_free(s_play);
    btn_free(s_helpb);
    btn_free(s_another);
    btn_free(s_back);
    btn_free(s_resume_b);
    dot_free(s_dot_show);
    dot_free(s_dot_quick);
    for (Btn *b : {&s_q_resume, &s_q_tables, &s_q_quit, &s_q_settings, &s_c_ok, &s_c_cancel, &s_l_tables, &s_l_quit}) btn_free(*b);
    if (s_fitui) {
        btn_free(s_fitui->q);
        for (Btn &b : s_fitui->b) btn_free(b);
        for (Btn &b : s_fitui->rb) btn_free(b);
        *s_fitui = FitUi{};
    }
}

// The wheel bar's furniture. Cabinet: Popper's — two chrome rails, a cyan light tube brightest in the
// middle, a glass orb with a chrome ring where the selected wheel sits. DMD: a dark band under an orange
// rule, a row of lit dots under the selected wheel.
void bar_dress(void) {
    const Look &lk = L();
    if (!lk.dots) {
        lv_obj_set_style_bg_color(s_bar, hx(0x000000), 0);
        lv_obj_set_style_bg_opa(s_bar, LV_OPA_60, 0);
        for (int half = 0; half < 2; half++) {   // the light tube: dark at the ends, teal in the middle
            lv_obj_t *t = plain(s_bar);
            lv_obj_set_size(t, kScrW / 2, kBarH - 44);
            lv_obj_set_pos(t, half * kScrW / 2, 22);
            grad(t, half ? 0x0B5866 : 0x000000, half ? 0x000000 : 0x0B5866, LV_GRAD_DIR_HOR);
            lv_obj_set_style_bg_opa(t, LV_OPA_70, 0);
        }
        for (int r = 0; r < 2; r++) {            // chrome rails
            lv_obj_t *rail = plain(s_bar);
            lv_obj_set_size(rail, kScrW, 8);
            lv_obj_set_pos(rail, 0, r ? kBarH - 16 : 8);
            lv_obj_set_style_radius(rail, 4, 0);
            grad(rail, 0xF2F6FA, 0x4A5660, LV_GRAD_DIR_VER);
        }
        // the glass orb: a chrome insert ring rendered with Qwen Image 2.1 (tools/icons/make_dmd_orb.py)
        lv_obj_t *orb = lv_image_create(s_bar);
        lv_image_set_src(orb, &dmd_orb);
        lv_obj_clear_flag(orb, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_pos(orb, kScrW / 2 - (int)dmd_orb.header.w / 2, (kBarH - (int)dmd_orb.header.h) / 2);
    } else {
        lv_obj_set_style_bg_color(s_bar, hx(0x000000), 0);
        lv_obj_set_style_bg_opa(s_bar, LV_OPA_70, 0);
        lv_obj_set_style_border_side(s_bar, LV_BORDER_SIDE_TOP, 0);
        lv_obj_set_style_border_width(s_bar, 2, 0);
        lv_obj_set_style_border_color(s_bar, hx(lk.accent), 0);
        lv_obj_set_style_border_opa(s_bar, LV_OPA_70, 0);
        for (int i = 0; i < 9; i++) {           // lit dots under the selected wheel
            lv_obj_t *dot = plain(s_bar);
            lv_obj_set_size(dot, 6, 6);
            lv_obj_set_pos(dot, kScrW / 2 - 4 * 11 - 3 + i * 11, kBarH - 13);
            lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
            lv_obj_set_style_bg_color(dot, hx(lk.accent), 0);
            lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
        }
    }
}

// Everything on screen, in the current look (rebuilt when the look changes; the data stays).
void build_ui(lv_obj_t *root) {
    const Look &lk = L();
    s_menu = plain(root);
    lv_obj_set_size(s_menu, lv_pct(100), lv_pct(100));
    grad(s_menu, 0x000000, lk.dots ? 0x160700 : 0x04161C, LV_GRAD_DIR_VER);   // before a backglass arrives
    lv_obj_add_flag(s_menu, LV_OBJ_FLAG_HIDDEN);

    s_bg_dsc = lv_image_dsc_t{};
    s_bg_dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
    s_bg_dsc.header.cf = LV_COLOR_FORMAT_RGB565;
    s_bg_dsc.header.w = kScrW;
    s_bg_dsc.header.h = kScrH;
    s_bg_dsc.header.stride = kScrW * 2;
    s_bg_dsc.data_size = (uint32_t)kScrW * kScrH * 2;
    s_bg_dsc.data = (const uint8_t *)s_bgpx;
    s_bg = lv_image_create(s_menu);
    lv_obj_set_pos(s_bg, 0, 0);
    lv_obj_clear_flag(s_bg, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(s_bg, LV_OBJ_FLAG_HIDDEN);
    s_bg_id = 0;

    // top: the five lists on the left; on the right the count (or, browsing during a game, back to the
    // DMD), "surprise me" and the DMD settings as two icon buttons. Nothing is ever cut: the lists take
    // their own width, a spacer the rest.
    lv_obj_t *hdr = plain(s_menu);
    lv_obj_set_pos(hdr, 16, 0);
    lv_obj_set_size(hdr, kScrW - 28, kTopH);
    lv_obj_set_flex_flow(hdr, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(hdr, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(hdr, 8, 0);
    s_tabs = plain(hdr);
    lv_obj_set_size(s_tabs, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(s_tabs, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(s_tabs, lk.dots ? 6 : 4, 0);
    const nv_str_id_t names[] = {NV_STR_DMDM_RECENT, NV_STR_DMDM_NEW, NV_STR_DMDM_TOP, NV_STR_DMDM_FAV, NV_STR_DMDM_ALL};
    for (int i = 0; i < kNLists; i++) {
        lv_obj_t *p = lv_obj_create(s_tabs);
        lv_obj_remove_style_all(p);
        lv_obj_set_size(p, LV_SIZE_CONTENT, NV_TOUCH_MIN);
        lv_obj_add_flag(p, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_clear_flag(p, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_event_cb(p, tab_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        if (lk.dots) {   // a square bezel, the name in dots (two images: off and on)
            lv_obj_set_style_pad_hor(p, 8, 0);
            lv_obj_set_style_border_width(p, 2, 0);
            lv_obj_set_style_bg_color(p, hx(lk.tab_on), 0);
            lv_obj_t *img = lv_image_create(p);
            lv_obj_clear_flag(img, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_center(img);
            dot_text(s_dot_tab[i][0], nv_tr(names[i]), lk.tab_off, 170, 18, 2);
            dot_text(s_dot_tab[i][1], nv_tr(names[i]), lk.tab_on, 170, 18, 2);
        } else {          // a capsule; the selected one lit (cyan rim, teal glass, white type)
            lv_obj_set_style_pad_hor(p, 18, 0);
            lv_obj_set_style_radius(p, LV_RADIUS_CIRCLE, 0);
            lv_obj_set_style_border_width(p, 2, 0);
            lv_obj_set_style_border_color(p, hx(lk.accent), 0);
            grad(p, 0x0E4A56, 0x041A20, LV_GRAD_DIR_VER);
            lv_obj_set_style_bg_color(p, hx(0x0E4A56), LV_STATE_PRESSED);
            lv_obj_set_style_bg_opa(p, LV_OPA_COVER, LV_STATE_PRESSED);
            lv_obj_t *l = text(p, &nv_font_20, lk.tab_off);
            lv_label_set_text(l, nv_tr(names[i]));
            lv_obj_center(l);
        }
    }
    tabs_restyle();
    lv_obj_t *spacer = plain(hdr);
    lv_obj_set_height(spacer, 1);
    lv_obj_set_flex_grow(spacer, 1);
    if (lk.dots) {
        s_count = lv_image_create(hdr);
        lv_obj_clear_flag(s_count, LV_OBJ_FLAG_CLICKABLE);
    } else {
        s_count = text(hdr, &nv_font_mono_17, lk.dim);
    }
    lv_obj_set_style_pad_hor(s_count, 6, 0);
    btn_make(s_resume_b, hdr, LV_SYMBOL_PLAY, nv_tr(NV_STR_DMD_RESUME), true, resume_cb, true);   // browsing in game
    s_resume = s_resume_b.obj;
    if (!s_browse) lv_obj_add_flag(s_resume, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(s_count, LV_OBJ_FLAG_HIDDEN);
    icon_btn(hdr, "A-Z", lk.dots ? lk.tab_on : lk.second, find_open_cb);   // go to: systems, letters
    icon_btn(hdr, LV_SYMBOL_SHUFFLE, lk.meta, random_cb);   // "surprise me"
    icon_btn(hdr, LV_SYMBOL_SETTINGS, lk.dots ? lk.tab_on : lk.second, settings_cb);   // the DMD panel

    // the stage: the selected table; swipes move along the list
    s_stage = plain(s_menu);
    lv_obj_set_pos(s_stage, 0, kTopH);
    lv_obj_set_size(s_stage, kScrW, kBarY - kTopH);
    nv_gesture_bind(s_stage, LV_DIR_LEFT, stage_next, nullptr);
    nv_gesture_bind(s_stage, LV_DIR_RIGHT, stage_prev, nullptr);
    lv_obj_add_flag(s_stage, LV_OBJ_FLAG_HIDDEN);
    s_stage_ph = text(s_stage, &nv_font_28, lk.dots ? lk.title : 0xFFFFFF);   // (behind the wheel)
    lv_obj_set_width(s_stage_ph, kBigImg - 40);
    lv_obj_set_style_text_align(s_stage_ph, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_line_space(s_stage_ph, 4, 0);
    lv_obj_align(s_stage_ph, LV_ALIGN_LEFT_MID, kStageX + 20, 0);
    lv_obj_add_flag(s_stage_ph, LV_OBJ_FLAG_HIDDEN);
    s_stage_img = lv_image_create(s_stage);
    lv_obj_set_size(s_stage_img, kBigImg, kBigImg);
    lv_obj_set_pos(s_stage_img, kStageX, (kBarY - kTopH - kBigImg) / 2);
    lv_image_set_inner_align(s_stage_img, LV_IMAGE_ALIGN_CENTER);
    lv_obj_clear_flag(s_stage_img, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_t *col = plain(s_stage);
    lv_obj_set_pos(col, kColX, 0);
    lv_obj_set_size(col, kColW, kBarY - kTopH);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(col, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(col, lk.dots ? 12 : 8, 0);
    s_stage_dots = lv_image_create(col);
    lv_obj_clear_flag(s_stage_dots, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(s_stage_dots, LV_OBJ_FLAG_HIDDEN);
    s_stage_title = text(col, &nv_font_28, lk.title);
    lv_obj_set_width(s_stage_title, kColW);
    lv_label_set_long_mode(s_stage_title, LV_LABEL_LONG_DOT);
    lv_obj_add_flag(s_stage_title, LV_OBJ_FLAG_HIDDEN);
    s_stage_meta = line_make(col, &nv_font_20, lk.meta, kColW);
    s_stage_st1 = line_make(col, &nv_font_20, lk.stats, kColW);
    s_stage_st2 = line_make(col, &nv_font_20, lk.stats, kColW);
    if (lk.dots && dot_text(s_dot_fav, nv_tr(NV_STR_DMDM_FAVOURITE), lk.fav, kColW, 21, 2)) {
        s_stage_fav = lv_image_create(col);
        lv_image_set_src(s_stage_fav, &s_dot_fav.dsc);
        lv_obj_clear_flag(s_stage_fav, LV_OBJ_FLAG_CLICKABLE);
    } else {
        s_stage_fav = text(col, &nv_font_20, lk.fav);
        char t[56];
        snprintf(t, sizeof t, LV_SYMBOL_OK "  %s", nv_tr(NV_STR_DMDM_FAVOURITE));
        lv_label_set_text(s_stage_fav, t);
    }
    lv_obj_add_flag(s_stage_fav, LV_OBJ_FLAG_HIDDEN);
    s_stage_scores = text(col, &nv_font_mono_17, lk.scores);
    lv_obj_t *btns = plain(col);
    lv_obj_set_size(btns, kColW, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(btns, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(btns, 14, 0);
    lv_obj_set_style_pad_top(btns, 6, 0);
    lv_obj_set_flex_align(btns, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    btn_make(s_play, btns, LV_SYMBOL_PLAY, nv_tr(NV_STR_DMDM_PLAY), true, play_cb);
    lv_obj_add_event_cb(s_play.obj, play_key, LV_EVENT_KEY, nullptr);
    nv_focus_prefer(s_play.obj);
    btn_make(s_helpb, btns, nullptr, nv_tr(NV_STR_DMDM_HELP), false, help_cb);
    lv_obj_add_flag(s_helpb.obj, LV_OBJ_FLAG_HIDDEN);
    btn_make(s_another, btns, LV_SYMBOL_SHUFFLE, nv_tr(NV_STR_DMDM_ANOTHER), false, another_cb);
    lv_obj_add_flag(s_another.obj, LV_OBJ_FLAG_HIDDEN);

    s_note = text(s_menu, &nv_font_28, lk.dots ? lk.stats : lk.dim);
    lv_obj_align(s_note, LV_ALIGN_CENTER, 0, -50);
    lv_obj_add_flag(s_note, LV_OBJ_FLAG_HIDDEN);

    // the wheel bar
    s_bar = plain(s_menu);
    lv_obj_set_pos(s_bar, 0, kBarY);
    lv_obj_set_size(s_bar, kScrW, kBarH);
    lv_obj_add_flag(s_bar, LV_OBJ_FLAG_CLICKABLE);
    nv_gesture_isolate(s_bar);   // dragging the bar is never a swipe for anyone else
    lv_obj_add_event_cb(s_bar, bar_event, LV_EVENT_ALL, nullptr);
    bar_dress();
    for (int k = 0; k < kSlots; k++) {
        s_slot[k] = lv_image_create(s_bar);
        lv_obj_set_size(s_slot[k], kBarImg, kBarImg);
        lv_image_set_inner_align(s_slot[k], LV_IMAGE_ALIGN_CENTER);
        lv_obj_clear_flag(s_slot[k], LV_OBJ_FLAG_CLICKABLE);
        s_slot_id[k] = 0;
        s_slot_big[k] = false;
        s_slot_ph[k] = text(s_bar, &nv_font_14, lk.dots ? lk.title : 0xDDE6EC);
        lv_obj_set_width(s_slot_ph[k], kBarImg - 20);
        lv_label_set_long_mode(s_slot_ph[k], LV_LABEL_LONG_DOT);
        lv_obj_set_height(s_slot_ph[k], 3 * lv_font_get_line_height(&nv_font_14));
        lv_obj_set_style_text_align(s_slot_ph[k], LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_add_flag(s_slot_ph[k], LV_OBJ_FLAG_HIDDEN);
    }

    // attract mode: the bar turns by itself, a blinking line asks for a touch
    if (lk.dots && dot_text(s_dot_attract, nv_tr(NV_STR_DMDM_TOUCH), lk.stats, 600, 30, 3)) {
        s_attract_txt = lv_image_create(s_menu);
        lv_image_set_src(s_attract_txt, &s_dot_attract.dsc);
    } else {
        s_attract_txt = text(s_menu, &nv_font_20, lk.title);
        lv_label_set_text(s_attract_txt, nv_tr(NV_STR_DMDM_TOUCH));
    }
    lv_obj_clear_flag(s_attract_txt, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_align(s_attract_txt, LV_ALIGN_BOTTOM_MID, 0, -(kBarH + 8));
    lv_obj_add_flag(s_attract_txt, LV_OBJ_FLAG_HIDDEN);

    show_build();
    nav_build();

    // "starting" / the game running
    // a page of its own (opaque): the table's backglass, darkened, behind its wheel, its name, what is
    // happening (starting / playing for 12 min) and what can be done
    s_launch = plain(s_menu);
    lv_obj_set_size(s_launch, lv_pct(100), lv_pct(100));
    lv_obj_add_flag(s_launch, LV_OBJ_FLAG_CLICKABLE);   // nothing behind it reacts
    lv_obj_set_style_bg_color(s_launch, hx(0x000000), 0);
    lv_obj_set_style_bg_opa(s_launch, LV_OPA_COVER, 0);
    lv_obj_add_flag(s_launch, LV_OBJ_FLAG_HIDDEN);
    s_launch_bg = lv_image_create(s_launch);
    lv_obj_set_pos(s_launch_bg, 0, 0);
    lv_obj_clear_flag(s_launch_bg, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_t *veil = plain(s_launch);
    lv_obj_set_size(veil, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_color(veil, hx(0x000000), 0);
    lv_obj_set_style_bg_opa(veil, LV_OPA_60, 0);
    lv_obj_t *lcol = plain(s_launch);
    lv_obj_set_size(lcol, lv_pct(100), lv_pct(100));
    lv_obj_set_flex_flow(lcol, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(lcol, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(lcol, 14, 0);
    s_launch_wheel = lv_image_create(lcol);
    lv_obj_set_size(s_launch_wheel, kBigImg - 40, kBigImg - 40);
    lv_image_set_inner_align(s_launch_wheel, LV_IMAGE_ALIGN_CENTER);
    lv_obj_clear_flag(s_launch_wheel, LV_OBJ_FLAG_CLICKABLE);
    s_launch_dots = lv_image_create(lcol);
    lv_obj_clear_flag(s_launch_dots, LV_OBJ_FLAG_CLICKABLE);
    s_launch_name = text(lcol, &nv_font_28, lk.title);
    s_launch_txt = line_make(lcol, &nv_font_20, lk.stats, kScrW - 80);
    lv_obj_set_style_text_align(lv_obj_get_child(s_launch_txt, 1), LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_align(lv_obj_get_child(s_launch_txt, 0), LV_ALIGN_TOP_MID, 0);
    s_launch_time = line_make(lcol, &nv_font_20, lk.dots ? lk.meta : lk.dim, kScrW - 80);
    lv_obj_set_style_text_align(lv_obj_get_child(s_launch_time, 1), LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_align(lv_obj_get_child(s_launch_time, 0), LV_ALIGN_TOP_MID, 0);
    btn_make(s_back, lcol, LV_SYMBOL_LEFT, nv_tr(NV_STR_DMDM_BACK), false, launch_cancel_cb);
    s_launch_btns = plain(lcol);   // in game (a table without DMD, or before its first frame)
    lv_obj_set_size(s_launch_btns, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(s_launch_btns, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(s_launch_btns, 18, 0);
    lv_obj_set_style_pad_top(s_launch_btns, 8, 0);
    tile_make(s_l_tables, s_launch_btns, LV_SYMBOL_LIST, nv_tr(NV_STR_DMDQ_TABLES), quick_tables_cb);
    tile_make(s_l_quit, s_launch_btns, LV_SYMBOL_STOP, nv_tr(NV_STR_DMDQ_QUIT), quick_quit_cb);

    // the instructions, full screen: an upright page as wide as the screen, scrolled with a finger; a tap
    // (or Back) closes it
    s_help = plain(root);
    lv_obj_set_size(s_help, lv_pct(100), lv_pct(100));
    lv_obj_add_flag(s_help, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(s_help, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(s_help, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(s_help, LV_SCROLLBAR_MODE_ACTIVE);
    lv_obj_set_style_bg_color(s_help, hx(0x000000), 0);
    lv_obj_set_style_bg_opa(s_help, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(s_help, hx(lk.accent), LV_PART_SCROLLBAR);
    lv_obj_set_style_width(s_help, 6, LV_PART_SCROLLBAR);
    lv_obj_add_flag(s_help, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(s_help, help_close_cb, LV_EVENT_CLICKED, nullptr);
    nv_gesture_isolate(s_help);   // its drags scroll the page
    s_help_img = lv_image_create(s_help);
    lv_obj_align(s_help_img, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_clear_flag(s_help_img, LV_OBJ_FLAG_CLICKABLE);
    quick_build(root);
}

}  // namespace

void dmd_menu_build(lv_obj_t *root, void (*open_settings)(void)) {
    s_open_settings = open_settings;
    s_root = root;
    if (!s_items) s_items = (Item *)heap_caps_calloc(kMaxItems, sizeof(Item), MALLOC_CAP_SPIRAM);
    if (!s_all) s_all = (Item *)heap_caps_calloc(kMaxItems, sizeof(Item), MALLOC_CAP_SPIRAM);
    sd_init();
    if (!s_missing) s_missing = (uint32_t *)heap_caps_calloc(kMissingN, sizeof(uint32_t), MALLOC_CAP_SPIRAM);
    s_list_hash = 0;
    s_nver = 0;
    s_zombie = nullptr;
    s_nall = 0;
    s_filter = 0;
    if (!s_bgpx) s_bgpx = (uint16_t *)heap_caps_aligned_alloc(64, (size_t)kScrW * kScrH * 2, MALLOC_CAP_SPIRAM);
    s_nitems = 0;
    if (!s_s2[2]) {   // the second screen from an earlier session
        char t[40];
        nv_config_get_str("dmd_s2", "", t, sizeof t);
        int v[4] = {0, 0, 0, 0};
        if (sscanf(t, "%d,%d,%d,%d", &v[0], &v[1], &v[2], &v[3]) == 4)
            for (int i = 0; i < 4; i++) s_s2[i] = (int16_t)v[i];
    }
    if (!s_pc_ip[0]) {   // the PC's address from an earlier session (the bridge says it again on its hello)
        nv_config_get_str("dmd_pc_ip", "", s_pc_ip, sizeof s_pc_ip);
        s_pc_web = nv_config_get_int("dmd_pc_web", 80);
    }
    // back where the user left it (NucleoOS keeps a suspended app's small state for the session)
    struct { int list, id; } st = {0, 0};
    if (nv_ui_state_load(&st, sizeof st) == sizeof st && st.list >= 0 && st.list < kNLists) {
        s_list = st.list;
        s_restore_id = st.id;
    } else {
        s_list = 0;
        s_restore_id = 0;
    }
    build_ui(root);

    // every press, on any input device: the attract mode's idle time
    s_last_touch_us = esp_timer_get_time();
    for (lv_indev_t *in = lv_indev_get_next(nullptr); in; in = lv_indev_get_next(in))
        lv_indev_add_event_cb(in, touch_cb, LV_EVENT_PRESSED, &s_touch_marker);

    s_hello = false;
    s_want_list = true;
    s_visible = false;
    s_launching = false;
    s_random_pending = false;
    s_want_help = false;
    s_attract = false;
    s_card_id = 0;
    s_sel = 0;
    s_drag = 0;
    s_dragging = s_animating = false;
    s_nmissing = 0;
    s_game[0] = 0;
    s_game_id = 0;
    s_wait_id = 0;
}

void dmd_menu_set_theme(int theme) {
    theme = theme ? 1 : 0;
    if (theme == s_theme) return;
    s_theme = theme;
    if (!s_menu || !s_root) return;
    // rebuild the screen in the new look, in the same place among the root's children; lists and
    // images stay (the side wheels are remade: they hold no look, but cost nothing to keep)
    lv_anim_delete(&s_drag, anim_exec);
    s_animating = s_dragging = false;
    s_drag = 0;
    show_close();
    const int32_t at = lv_obj_get_index(s_menu);
    const bool vis = s_visible, help = !lv_obj_has_flag(s_help, LV_OBJ_FLAG_HIDDEN);
    lv_obj_delete(s_help);
    lv_obj_delete(s_quick);
    lv_obj_delete(s_fitm);
    lv_obj_delete(s_find);
    lv_obj_delete(s_confirm);
    lv_obj_delete(s_menu);
    dots_free_all();
    build_ui(s_root);
    lv_obj_move_to_index(s_menu, at);       // below the app's panel, as before
    lv_obj_move_to_index(s_help, at + 1);
    lv_obj_move_to_index(s_quick, at + 2);
    lv_obj_move_to_index(s_find, at + 3);
    lv_obj_move_to_index(s_confirm, at + 4);
    lv_obj_move_to_index(s_fitm, at + 5);
    if (vis) lv_obj_remove_flag(s_menu, LV_OBJ_FLAG_HIDDEN);
    if (s_nitems) select_item(s_sel);
    else if (s_hello) show_note(nv_tr(NV_STR_DMDM_EMPTY));
    if (help) help_cb(nullptr);
    if (s_game[0]) launch_show(s_game, false);
    else if (s_launching) launch_show(s_card_name, true);
}

bool dmd_menu_tick(bool allowed) {
    // take what the bridge sent (a few per tick: images are large)
    uint8_t type, *data;
    uint32_t len;
    for (int i = 0; i < 6 && nv_dmd_bridge_take(&type, &data, &len); i++) {
        if (s_menu) handle(type, data, len);
        nv_dmd_bridge_free(data);
    }
    if (!s_menu) return false;
    nv_dmd_status_t st;
    nv_dmd_get_status(&st);
    const int64_t now = esp_timer_get_time();
    // a game whose end never came (the bridge stopped meanwhile): forgotten after 4 hours
    if (s_game[0] && !st.bridge && now - s_game_us > 4LL * 3600 * 1000000) { s_game[0] = 0; launch_hide(); }
    if (s_launching && !s_game[0] && !s_pending_launch && now - s_launch_us > kLaunchTimeoutUs) {   // Popper never started it
        s_launching = false;
        launch_hide();
        nv_toast(NV_NOTE_WARN, nv_tr(NV_STR_DMDM_TIMEOUT));
    }
    if (s_http_result < 0) {
        s_http_result = 0;
        s_pending_launch = 0;
        if (s_launching && s_game[0]) { s_launching = false; launch_show(s_game, false); }
        nv_toast(NV_NOTE_ERROR, nv_tr(NV_STR_DMDQ_NO_PC));
    }
    constexpr int64_t kQuitRetryUs = 15000000;
    if (s_quit_us && !s_game[0]) s_quit_us = 0;   // it ended
    if (s_quit_us && now - s_quit_us > kQuitRetryUs) {   // still running: once more, then no more
        if (s_quit_n < 2 && quit_send()) {
            s_quit_n++;
            s_quit_us = now;
            nv_log_write(NV_LOG_INFO, "dmd", "quit asked again (the game was still running)");
        } else {
            s_quit_us = 0;
        }
    }
    if (s_pending_launch && now - s_pending_us > 45000000) {   // the game never ended (a quit asked twice)
        s_pending_launch = 0;
        s_launching = false;
        if (s_game[0]) launch_show(s_game, false);
    }
    const bool show = allowed && ((st.bridge && s_hello) || s_game[0] || s_launching);
    // blinking lines (launch, attract): twice a second, only while shown
    if (++s_ticks % 5 == 0) {
        const lv_opa_t opa = ((s_ticks / 5) & 1) ? LV_OPA_COVER : LV_OPA_30;
        if (s_attract) lv_obj_set_style_opa(s_attract_txt, opa, 0);
        if (!lv_obj_has_flag(s_launch, LV_OBJ_FLAG_HIDDEN)) lv_obj_set_style_opa(s_launch_txt, opa, 0);
    }
    if (s_ticks % 10 == 0 && s_launch && !lv_obj_has_flag(s_launch, LV_OBJ_FLAG_HIDDEN)) launch_time();
    // the card of a table the selection rests on (scores, help)
    if (show && s_card_id && !s_card_asked && now - s_sel_us > kRestUs && !s_dragging && !s_animating) {
        char q[48];
        snprintf(q, sizeof q, "{\"q\":\"table\",\"id\":%d}", s_card_id);
        s_card_asked = nv_dmd_bridge_send('Q', q);
    }
    // the attract mode: nothing touched for a while, on the plain menu
    if (s_show_id && s_show_at < 0) show_put(0);
    if (s_show_id && show && s_show_n > 1 && s_show_at >= 0 && now - s_show_us > kShowStepUs) {
        if (!show_put((s_show_at + 1) % s_show_n)) s_show_us = now - kShowStepUs + 1000000;   // not here yet: soon
    }
    const bool plain_menu = show && s_nitems > 1 && !s_game[0] && !s_show_id && !nav_visible() && lv_obj_has_flag(s_help, LV_OBJ_FLAG_HIDDEN) &&
                            lv_obj_has_flag(s_launch, LV_OBJ_FLAG_HIDDEN);
    if (!plain_menu || now - s_last_touch_us < kAttractUs) {
        if (s_attract) attract_stop();
    } else if (!s_attract) {
        s_attract = true;
        s_attract_us = now;
        lv_obj_remove_flag(s_attract_txt, LV_OBJ_FLAG_HIDDEN);
    } else if (now - s_attract_us > kAttractStepUs && !s_animating) {
        s_attract_us = now;
        bar_move(1);
    }
    if (show != s_visible) {
        s_visible = show;
        if (show) lv_obj_remove_flag(s_menu, LV_OBJ_FLAG_HIDDEN);
        else { lv_obj_add_flag(s_menu, LV_OBJ_FLAG_HIDDEN); close_help(); }
    }
    if (show && !s_game[0] && s_nall && now - s_list_us > 300000000 && now - s_last_touch_us > 60000000) s_want_list = true;
    if (s_hello && s_want_list) ask_list();
    if (show) pump_images();
    return show;
}

bool dmd_menu_back(void) {
    if (s_find && !lv_obj_has_flag(s_find, LV_OBJ_FLAG_HIDDEN)) { find_close(); return true; }
    if (s_show_id) { show_close(); return true; }
    if (nav_visible()) { nav_close(); return true; }
    if (s_confirm && !lv_obj_has_flag(s_confirm, LV_OBJ_FLAG_HIDDEN)) { confirm_close(); return true; }
    if (s_quick && !lv_obj_has_flag(s_quick, LV_OBJ_FLAG_HIDDEN)) { quick_close(); return true; }
    if (s_fitm && !lv_obj_has_flag(s_fitm, LV_OBJ_FLAG_HIDDEN)) { fitm_close(); return true; }
    if (dmd_comp_is_open()) { dmd_comp_close(); return true; }
    if (s_browse) { browse_set(false); return true; }
    if (!s_visible) return false;
    if (s_help && !lv_obj_has_flag(s_help, LV_OBJ_FLAG_HIDDEN)) { close_help(); return true; }
    if (s_launching) { s_launching = false; launch_hide(); return true; }
    return false;
}

bool dmd_menu_in_game(void) { return s_game[0] != 0; }

bool dmd_menu_ready(void) {
    nv_dmd_status_t st;
    nv_dmd_get_status(&st);
    return s_menu && ((st.bridge && s_hello) || s_game[0] || s_launching);
}

const char *dmd_menu_game(void) { return s_game; }

void dmd_menu_quick_open(void) {
    if (!s_quick) return;
    char t[64];
    clean_name(t, sizeof t, s_game[0] ? s_game : "DMD");
    if (L().dots && dot_text(s_dot_quick, t, L().title, 580, 45, 5)) {
        lv_image_set_src(s_quick_dots, &s_dot_quick.dsc);
        lv_obj_remove_flag(s_quick_dots, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_quick_title, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_label_set_text(s_quick_title, t);
        lv_obj_remove_flag(s_quick_title, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_quick_dots, LV_OBJ_FLAG_HIDDEN);
    }
    const bool game = s_game[0] != 0;
    // dmdext drives the DMD but the bridge never said which table (it started during the game): still a
    // game, that can be closed (over Wi-Fi); the tables come back with the bridge once it is over
    nv_dmd_status_t st;
    nv_dmd_get_status(&st);
    const bool unknown = !game && st.usb && s_pc_ip[0];
    for (Btn *b : {&s_q_tables, &s_q_quit}) {
        if (game || (unknown && b == &s_q_quit)) lv_obj_remove_flag(b->obj, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(b->obj, LV_OBJ_FLAG_HIDDEN);
    }
    if (s_fitui && s_fitui->q.obj) {   // the per-table fit: only for a table whose DMD the board shows
        if (game && s_game_id && !s_show_game) lv_obj_remove_flag(s_fitui->q.obj, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(s_fitui->q.obj, LV_OBJ_FLAG_HIDDEN);
    }
    // the first button goes back to what was on screen: the DMD, or the game's pictures (no DMD for it)
    if (s_q_resume.obj) {
        const char *lbl = nv_tr(s_show_game ? NV_STR_DMDM_BACK : NV_STR_DMD_RESUME);
        if (s_q_resume.img) {   // DMD look: the dot label again
            dot_free(s_q_resume.dot);
            dot_free(s_q_resume.dot_on);
            if (dot_text(s_q_resume.dot, lbl, L().play, 420, 36, 4)) {
                dot_text(s_q_resume.dot_on, lbl, 0x000000, 420, 36, 4, L().play);
                lv_image_set_src(s_q_resume.img, &s_q_resume.dot.dsc);
            }
        } else if (lv_obj_get_child_count(s_q_resume.obj)) {
            lv_label_set_text_fmt(lv_obj_get_child(s_q_resume.obj, 0), "%s   %s", s_show_game ? LV_SYMBOL_LEFT : LV_SYMBOL_PLAY, lbl);
        }
    }
    lv_obj_remove_flag(s_quick, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_quick);
}

bool dmd_menu_covering(void) {
    return s_browse || (s_quick && !lv_obj_has_flag(s_quick, LV_OBJ_FLAG_HIDDEN)) ||
           (s_confirm && !lv_obj_has_flag(s_confirm, LV_OBJ_FLAG_HIDDEN)) ||
           (s_fitm && !lv_obj_has_flag(s_fitm, LV_OBJ_FLAG_HIDDEN)) || dmd_comp_is_open();
}

const lv_image_dsc_t *dmd_menu_game_art(int h) {
    if (!s_game_id) return nullptr;
    if (s_artpx && s_art_id == s_game_id && s_art_h == h) return &s_art_dsc;
    Img *m = img_find(s_game_id, 'g', kArtSrcW);
    if (!m) return nullptr;
    art_build(*m, h);
    return s_art_id == s_game_id && s_art_h == h ? &s_art_dsc : nullptr;
}

int dmd_menu_game_id(void) { return s_game[0] ? s_game_id : 0; }

const lv_image_dsc_t *dmd_menu_game_grill(dmd_rect_t *win) {
    if (!s_game_id) return nullptr;
    Img *m = img_find(s_game_id, 'r', 1024);
    static int s_sd_tried;   // the card once a game, when it did not come over the cable
    if (!m && s_sd_tried != s_game_id) {
        s_sd_tried = s_game_id;
        if (sd_fetch(s_game_id, 'r', 1024, 600, -1)) m = img_find(s_game_id, 'r', 1024);
    }
    if (!m || !m->has_win || m->h != 600) return nullptr;
    *win = m->win;
    return &m->dsc;
}

const lv_image_dsc_t *dmd_menu_game_full(dmd_rect_t *win) {
    if (!s_game_id) return nullptr;
    Img *m = img_find(s_game_id, 'f', 1024);
    static int s_sd_tried;   // the card is read once a game, when the art did not come over the cable
    if (!m && s_sd_tried != s_game_id) {
        s_sd_tried = s_game_id;
        if (sd_fetch(s_game_id, 'f', 1024, 600, -1)) m = img_find(s_game_id, 'f', 1024);
    }
    if (!m || !m->has_win || m->h != 600) return nullptr;
    *win = m->win;
    return &m->dsc;
}

const lv_image_dsc_t *dmd_menu_game_b2s(dmd_rect_t *box, int *grill, dmd_rect_t *housing) {
    if (!s_game_id) return nullptr;
    dmd_rect_t w{};
    dmd_menu_game_grill(&w);   // the card once a game, when it did not come over the cable
    Img *m = img_find(s_game_id, 'r', 1024);
    if (!m || m->h != 600 || m->alpha || s_rgrill_id != s_game_id) return nullptr;
    // the bridge fits the picture whole and centred, its grill at the bottom: the rest follows
    const int gx = s_rgrill[0], gy = s_rgrill[1], gw = s_rgrill[2], gh = s_rgrill[3];
    const int bottom = gy + gh, h = 2 * bottom - m->h;
    if (gw < 16 || h < 16 || gx < 0 || gx + gw > m->w || bottom > m->h) return nullptr;
    *box = dmd_rect_t{gx, m->h - bottom, gw, h};
    *grill = gh;
    *housing = m->has_win ? m->win : dmd_rect_t{0, 0, 0, 0};
    return &m->dsc;
}

void dmd_menu_game_full_shape(int *aw, int *ah) {
    const bool known = s_fart_id == s_game_id && s_fart[0] > 0 && s_fart[1] > 0;
    *aw = known ? s_fart[0] : 1920;
    *ah = known ? s_fart[1] : 1080;
}

bool dmd_menu_s2(int *w, int *h) {
    *w = s_s2[2];
    *h = s_s2[3];
    return s_s2[2] > 0 && s_s2[3] > 0;
}

// (never while dmdext has the cable: the bytes would reach it, not the bridge)
bool dmd_menu_send(const char *json) { return s_hello && !(s_game[0] && s_game_dmd) && nv_dmd_bridge_send('Q', json); }

void dmd_menu_quit_and_relaunch(void) {
    if (!s_game_id || !quit_request()) {
        nv_toast(NV_NOTE_ERROR, nv_tr(NV_STR_DMDQ_NO_PC));
        return;
    }
    s_pending_launch = s_game_id;
    s_pending_us = esp_timer_get_time();
    snprintf(s_card_name, sizeof s_card_name, "%s", s_game);
}

void dmd_menu_want_fulldmd(bool on) {
    if (on == s_want_full) return;
    s_want_full = on;
    if (s_hello) send_prefs();
}

void dmd_menu_destroy(void) {
    dmd_comp_close();
    // where the user was, for when the app comes back (a suspended task)
    struct { int list, id; } st = {s_list, s_nitems && s_items ? s_items[s_sel].id : 0};
    nv_ui_state_save(&st, sizeof st);
    lv_anim_delete(&s_drag, anim_exec);
    for (Img &m : s_cache) img_drop(m);
    dots_free_all();
    for (lv_indev_t *in = lv_indev_get_next(nullptr); in; in = lv_indev_get_next(in))
        lv_indev_remove_event_cb_with_user_data(in, touch_cb, &s_touch_marker);
    lv_image_cache_drop(&s_bg_dsc);
    heap_caps_free(s_bgpx);
    s_bgpx = nullptr;
    heap_caps_free(s_items);
    s_items = nullptr;
    heap_caps_free(s_all);
    s_all = nullptr;
    heap_caps_free(s_missing);
    s_missing = nullptr;
    s_nmissing = 0;
    s_nall = 0;
    lv_image_cache_drop(&s_art_dsc);
    heap_caps_free(s_artpx);
    s_artpx = nullptr;
    s_art_id = 0;
    s_menu = s_bg = s_tabs = s_count = s_note = s_stage = s_stage_img = s_stage_dots = s_stage_title = nullptr;
    s_stage_fav = s_stage_meta = s_stage_st1 = s_stage_st2 = s_stage_scores = nullptr;
    s_bar = nullptr;
    for (lv_obj_t *&o : s_slot) o = nullptr;
    for (lv_obj_t *&o : s_slot_ph) o = nullptr;
    s_stage_ph = nullptr;
    s_help = s_help_img = s_launch = s_launch_dots = s_launch_name = s_launch_txt = s_attract_txt = nullptr;
    show_close();
    s_show = s_show_name_lbl = s_show_name_dots = s_show_kind = s_show_pips = s_show_hint = nullptr;
    s_show_img[0] = s_show_img[1] = nullptr;
    s_nav = s_nav_icon = s_nav_sym = s_nav_wheel = s_nav_dots = s_nav_lbl = s_nav_sub = nullptr;
    s_nav_id = 0;
    s_find = s_find_card = s_launch_bg = s_launch_wheel = s_launch_time = nullptr;
    s_fitm = nullptr;
    heap_caps_free(s_fitui);
    s_fitui = nullptr;
    s_quick = s_quick_title = s_quick_dots = s_confirm = s_confirm_txt = s_resume = s_launch_btns = nullptr;
    s_browse = false;
    s_pending_launch = 0;
    s_root = nullptr;
    s_nitems = 0;
    s_visible = false;
    s_hello = false;
    s_attract = false;
    s_animating = s_dragging = false;
}
