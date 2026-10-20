// dmd_comp — "Screens": how a table uses the cabinet's screens, chosen with previews of its own pictures.
// Two pages:
//   - the PC's monitor (the second screen): what B2S shows there (the backglass alone or with its grill, the
//     FullDMD art — the backglass file's second, smaller picture — or both), whether a DMD shows there too
//     (dmdext's virtual DMD; off: the DMD is only on this board) and, for a dual backglass, its version
//     (Authentic or Fantasy: B2S lights another set of pictures). Sent to the bridge, which writes them where
//     B2S and dmdext read them when the table starts. A Future Pinball table played with its backbox off has
//     Popper's backglass picture instead: where Popper shows it (its DMD under it) or the whole monitor, drawn
//     by the bridge, with the scores it reads from the game in both;
//   - this screen: its fit (Automatic: the one the table's pictures suit best), original or fitted dots, a
//     margin, with a 1:1 lens of the real dots.
// Opened from a table's card in the menu (any table, no game needed: its pictures are asked for as it opens),
// from the quick actions in a game (the running table), or from the settings panel for every table (this
// screen only: the monitor is chosen table by table). LVGL thread.
#include "dmd_menu.h"
#include "dmd_ui.h"

#include "dmd_core.h"
#include "nv_config.h"
#include "nv_fonts.h"
#include "nv_i18n.h"
#include "nv_log.h"
#include "nv_notify.h"

#include "esp_heap_caps.h"
#include "esp_timer.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>

extern "C" void lv_image_cache_drop(const void *src);   // LVGL 9 (not in the public header), as dmd_menu.cpp

namespace {

constexpr int kNFit = 10;                     // the panel's fits (dmd_app.cpp), 9 = automatic
constexpr int kFitAuto = 9;
constexpr int kFits = kNFit + 1;              // "as every table" + the fits
constexpr int kS2TW = 128, kPTW = 150, kPTH = 88;   // thumbnails (the monitor's: its shape)
constexpr int kBigS2W = 240, kBigPW = 272, kBigH = 160;
constexpr int kAllTW = 176, kAllTH = 103;     // the cards when choosing for every table (a grid)
constexpr int kTop = 138;                     // where a page starts (under the tabs and the pictures' lines)
constexpr size_t kQCap = 512;                 // "dmd_s2q": the tables waiting for the cable (~100 ids)
// The fits as the cards show them: the most useful first
constexpr int kOrder[kNFit] = {kFitAuto, 6, 1, 5, 0, 2, 3, 4, 7, 8};
constexpr uint16_t kStandIn = 0x2124;         // where a table's picture goes when there is none to show
using namespace dmdui;

struct Pic {                       // a preview drawn here: its pixels and LVGL's description of them
    uint16_t *px = nullptr;
    int w = 0, h = 0;
    lv_image_dsc_t dsc{};
};

struct Card {
    lv_obj_t *obj = nullptr, *img = nullptr;
    Pic pic;
    int what = -1;                 // the monitor's composition, or the fit + 1 (0 = as every table)
};

struct Info {                      // what the bridge said about tables' backglass files (the last few)
    DmdB2sInfo i[12];
    int n;
};
Info *s_info;                      // on the heap (the statics are nearly full)

struct Ui {
    lv_obj_t *scr = nullptr, *tab[2] = {}, *page[2] = {}, *pics = nullptr, *loading = nullptr;
    // the monitor's page
    lv_obj_t *big_s2 = nullptr, *s2name = nullptr, *s2desc = nullptr, *note = nullptr, *restart = nullptr;
    lv_obj_t *s2dmd[2] = {}, *s2dmd_why = nullptr, *dual[2] = {}, *dual_lbl = nullptr, *dual_note = nullptr;
    lv_obj_t *s2dmd_lbl = nullptr, *s2row = nullptr, *fp_lbl = nullptr, *fpfull[2] = {};   // (Future Pinball)
    // this screen's page
    lv_obj_t *big_p = nullptr, *fname = nullptr, *desc = nullptr;
    lv_obj_t *dots[2] = {}, *marg[4] = {}, *rb[2] = {};
    Pic s2pic, ppic;
    Card s2[DMD_S2_N], fit[kFits];
    int gid = 0;                   // the table (0: every table)
    int pid = 0;                   // whose pictures the previews show (every table: the running or selected one)
    char name[64] = "";
    bool game = false;             // it is the running game
    bool all = false;              // every table: this screen only
    int tab_on = 0;
    void (*on_close)(void) = nullptr;
    lv_obj_t *root = nullptr;
    int s2w = 0, s2h = 0;          // the PC's second screen
    bool s2ok = false;
    // the table's pictures
    const lv_image_dsc_t *b2s = nullptr;   // its backglass (1024x600, fitted whole) and where it is in it
    dmd_rect_t box{}, housing{};
    int grill = 0;
    const lv_image_dsc_t *art = nullptr;   // its FullDMD art (1024x600, stretched), the window, true shape
    dmd_rect_t win{};
    int aw = 0, ah = 0;
    const lv_image_dsc_t *bg = nullptr;    // Popper's backglass for the backbox (1024 x 344)
    uint32_t *frame = nullptr;             // the DMD's last frame, with a margin around it, its rebuilt rows
    uint32_t *framem = nullptr;            // (all sized from the snapshot: w x 2h with a margin of 3)
    uint32_t *frame2 = nullptr, *tmp2 = nullptr;
    int fw = 0, fh = 0;
    dmd_geom_t *geom = nullptr;            // a fit's geometry (~2 KB, PSRAM)
    int64_t info_us = 0;                   // the backglass file's description asked for (retried while lost)
    int info_tries = 0;
    bool info_failed = false;
};
Ui *s_ui = nullptr;                // on the heap while open (static PSRAM is nearly full)

void ui_free(Ui *u);
// The screen deleted with its parent (the menu rebuilt for another look, the app closed): its memory goes
// too, right after (never a dangling screen that keeps the DMD "covered").
void scr_deleted_cb(lv_event_t *e) {
    Ui *u = (Ui *)lv_event_get_user_data(e);
    if (!u || u != s_ui) return;
    u->scr = nullptr;
    s_ui = nullptr;
    lv_async_call([](void *p) { ui_free((Ui *)p); }, u);
}

bool pic_alloc(Pic &p, int w, int h) {
    p.px = (uint16_t *)heap_caps_calloc((size_t)w * (size_t)h, 2, MALLOC_CAP_SPIRAM);
    if (!p.px) return false;
    p.w = w;
    p.h = h;
    p.dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
    p.dsc.header.cf = LV_COLOR_FORMAT_RGB565;
    p.dsc.header.w = (uint32_t)w;
    p.dsc.header.h = (uint32_t)h;
    p.dsc.header.stride = (uint32_t)w * 2;
    p.dsc.data = (const uint8_t *)p.px;
    p.dsc.data_size = (uint32_t)w * (uint32_t)h * 2;
    return true;
}

void pic_free(Pic &p) {
    if (p.px) lv_image_cache_drop(&p.dsc);
    heap_caps_free(p.px);
    p = Pic{};
}

// "a\nb\nc" -> pointers into buf
int split(char *buf, char **out, int max) {
    int n = 0;
    for (char *q = buf; n < max && q && *q;) {
        out[n++] = q;
        q = strchr(q, '\n');
        if (q) *q++ = 0;
    }
    return n;
}

// The n-th line of a "\n" list into out.
void line_of(const char *lines, int n, char *out, size_t cap) {
    out[0] = 0;
    const char *q = lines;
    for (int i = 0; q && i < n; i++) {
        q = strchr(q, '\n');
        if (q) q++;
    }
    if (!q) return;
    const char *e = strchr(q, '\n');
    const size_t len = e ? (size_t)(e - q) : strlen(q);
    snprintf(out, cap, "%.*s", (int)(len < cap ? len : cap - 1), q);
}

const DmdB2sInfo *info_of(int id) {
    for (int i = 0; s_info && i < 12; i++)
        if (s_info->i[i].id == id && id) return &s_info->i[i];
    return nullptr;
}

// ---- pictures

// A picture of the table (RGB565, 1024 wide) or a part of it into r.
void put_img(Pic &p, const lv_image_dsc_t *d, dmd_rect_t src, dmd_rect_t r) {
    if (!d || d->header.cf != LV_COLOR_FORMAT_RGB565 || !d->data) return;
    const int stride = d->header.stride ? (int)d->header.stride / 2 : (int)d->header.w;
    dmd_blit565(p.px, p.w, p.h, r, (const uint16_t *)d->data, stride, src);
}

// The DMD's frame into r, with `m` rows and columns of unlit dots around it (the panel's margin).
void put_dmd(Pic &p, dmd_rect_t r, int m) {
    Ui &u = *s_ui;
    if (!u.frame) {
        dmd_fill565(p.px, p.w, p.h, r, 0x2945);
        return;
    }
    const uint32_t *px = u.frame;
    int w = u.fw, h = u.fh;
    if (m && u.framem) {
        const int mw = w + 2 * m;
        memset(u.framem, 0, (size_t)mw * (size_t)(h + 2 * m) * 4);
        for (int y = 0; y < h; y++) memcpy(u.framem + (size_t)(y + m) * mw + m, px + (size_t)y * w, (size_t)w * 4);
        px = u.framem;
        w = mw;
        h += 2 * m;
    }
    dmd_blit_frame(p.px, p.w, p.h, r, px, w, h);
}

dmd_rect_t scaled(dmd_rect_t r, int num, int den) {
    return dmd_rect_t{r.x * num / den, r.y * num / den, r.w * num / den, r.h * num / den};
}

// ---- the monitor

// The monitor's DMD: on (dmdext's virtual DMD there too) or off (only on this board, the default).
bool s2_dmd_on(void) {
    char k[24];
    snprintf(k, sizeof k, "dmds2d_%d", s_ui->gid);
    return nv_config_get_int(k, 0) != 0;
}

int cur_s2(void) {
    char k[24];
    snprintf(k, sizeof k, "dmds2_%d", s_ui->gid);
    const int v = nv_config_get_int(k, 0) - 1;
    return v > 0 && v < DMD_S2_N ? v : DMD_S2_KEEP;
}

dmd_s2_in_t s2_in(void) {
    Ui &u = *s_ui;
    dmd_s2_in_t in{};
    in.sw = u.s2w;
    in.sh = u.s2h;
    if (u.b2s) {
        in.bw = u.box.w;
        in.bh = u.box.h;
        in.grill = u.grill;
        if (u.housing.w) in.housing = dmd_rect_t{u.housing.x - u.box.x, u.housing.y - u.box.y, u.housing.w, u.housing.h};
    }
    if (u.art && u.win.w) {   // the window found in the stretched 1024x600 copy, in the art's true pixels
        in.aw = u.aw;
        in.ah = u.ah;
        in.window = dmd_rect_t{u.win.x * u.aw / kScrW, u.win.y * u.ah / kScrH, u.win.w * u.aw / kScrW, u.win.h * u.ah / kScrH};
    }
    in.dw = u.fw > 0 ? u.fw : 128;
    in.dh = u.fh > 0 ? u.fh : 32;
    return in;
}

// The monitor with composition `mode`, into p (its shape). As it came: the backglass alone.
void draw_s2(Pic &p, int mode) {
    Ui &u = *s_ui;
    memset(p.px, 0, (size_t)p.w * (size_t)p.h * 2);
    const dmd_s2_in_t in = s2_in();
    dmd_s2_out_t o;
    if (!dmd_s2_layout(mode == DMD_S2_KEEP ? DMD_S2_BG : mode, &in, &o)) {
        if (mode == DMD_S2_KEEP) dmd_fill565(p.px, p.w, p.h, dmd_rect_t{0, 0, p.w, p.h}, kStandIn);
        return;
    }
    const int num = p.w, den = u.s2w;
    if (u.b2s && mode != DMD_S2_ART) {
        const dmd_rect_t src{u.box.x, u.box.y, u.box.w, o.grill ? u.box.h : u.box.h - u.grill};
        put_img(p, u.b2s, src, scaled(o.bg, num, den));
    }
    if (o.art.w && u.art) put_img(p, u.art, dmd_rect_t{0, 0, kScrW, kScrH}, scaled(o.art, num, den));
    // the DMD there: only when it is on the monitor too ("backglass + DMD" is that by definition)
    if (o.dmd.w && mode != DMD_S2_KEEP && (s2_dmd_on() || mode == DMD_S2_BG_DMD)) put_dmd(p, scaled(o.dmd, num, den), 0);
}

// ---- this screen
// The previews follow the board: the fit it would really show (missing pictures, a rotated screen), the
// frame as it draws it (rebuilt rows), the geometry for the frame the previews have (not the size on screen).

// The table the previews are about is the one running: only its lit rows (a 128x16 DMD) are known.
bool live(void) { return s_ui->pid && s_ui->pid == dmd_menu_game_id(); }

// Automatic for this table: its pictures decide; without them a running table's DMD does (rebuilt when it
// lights only half its rows), any other table's is not known yet (classic here, decided when it runs).
int auto_of(bool *later) {
    Ui &u = *s_ui;
    const bool art = u.art && u.win.w, grill = u.b2s && u.housing.w;
    if (later) *later = !art && !grill && !live();
    return art || grill || live() ? dmd_app_fit_suggested(art, grill) : 1;
}

// The fit a choice means for this table: as every table, and automatic, resolved with its pictures.
int resolved(int fit) {
    if (fit < 0) fit = dmd_app_fit_all();
    if (fit == kFitAuto) fit = auto_of(nullptr);
    return fit;
}

// What the board shows for that choice (the fallbacks of dmd_app_fit_shown). For every table the cards
// illustrate each layout (a stand-in where this table has no picture: another one may), for a table they
// show what it will really get.
int shown(int choice) {
    Ui &u = *s_ui;
    const int fit = resolved(choice);
    const bool full = u.all || (fit == 8 ? u.b2s && u.housing.w : u.art && u.win.w);
    return dmd_app_fit_shown(fit, full, u.all || u.bg, u.fw > 0 ? u.fw : 128, u.fh > 0 ? u.fh : 32);
}

// The window / housing of a fit drawn in one, in this table's pictures (NULL: none).
const dmd_rect_t *win_of(int fit) {
    Ui &u = *s_ui;
    if (fit == 7) return u.art && u.win.w ? &u.win : nullptr;
    if (fit == 8) return u.b2s && u.housing.w ? &u.housing : nullptr;
    return nullptr;
}

// Where fit puts the preview's frame on the panel (fit as shown, the table's dots).
bool fit_rect(int fit, int dot, dmd_rect_t *r) {
    Ui &u = *s_ui;
    if (!u.geom || !dmd_app_fit_geom_wh(fit, dot, win_of(fit), u.fw, u.fh, u.geom)) return false;
    *r = dmd_rect_t{u.geom->ox, u.geom->oy, u.geom->gw, u.geom->gh};
    return true;
}

// The frame as the board draws it in fit (shown): the rebuilt DMD twice its rows, any other its own rows,
// a 128x16 DMD of the running table grown to the full height; the margin of unlit dots m around it. NULL
// when it cannot be made (the preview stays dark, as the board would not draw that fit).
const uint32_t *frame_for(int fit, int m, int *w, int *h) {
    Ui &u = *s_ui;
    if (!u.frame) return nullptr;
    const uint32_t *px = u.frame;
    *w = u.fw;
    *h = u.fh;
    int r0 = 0, n = u.fh;
    dmd_app_rebuild_rows(u.pid, u.fh, &r0, &n);
    if (fit == 6) {
        if (!u.frame2 || !u.tmp2 || !dmd_rebuild(u.frame, u.fw, u.fh, r0, n, u.frame2, u.tmp2)) return nullptr;
        px = u.frame2;
        *h = 2 * u.fh;
    } else if (n < u.fh && u.frame2 && u.tmp2 && dmd_rebuild_to(u.frame, u.fw, u.fh, r0, n, u.frame2, u.tmp2, u.fh)) {
        px = u.frame2;
    }
    if (m && u.framem) {   // (sized for twice the rows and a margin of 3)
        const int mw = *w + 2 * m;
        memset(u.framem, 0, (size_t)mw * (size_t)(*h + 2 * m) * 4);
        for (int y = 0; y < *h; y++) memcpy(u.framem + (size_t)(y + m) * mw + m, px + (size_t)y * *w, (size_t)*w * 4);
        px = u.framem;
        *w = mw;
        *h += 2 * m;
    }
    return px;
}

// The DMD's frame into r as fit draws it, with `m` rows and columns of unlit dots around it.
void put_dmd_fit(Pic &p, dmd_rect_t r, int fit, int m) {
    int w = 0, h = 0;
    const uint32_t *px = frame_for(fit, m, &w, &h);
    if (px) dmd_blit_frame(p.px, p.w, p.h, r, px, w, h);
    else dmd_fill565(p.px, p.w, p.h, r, 0x2945);
}

// This panel with choice `choice` (-1: as every table) and the table's dots, into p (1024x600's shape): the
// table's pictures as the board puts them, 1:1 like it (scaled to the thumbnail only).
void draw_panel(Pic &p, int choice, int dot) {
    Ui &u = *s_ui;
    memset(p.px, 0, (size_t)p.w * (size_t)p.h * 2);
    const int fit = shown(choice);
    const int num = p.w, den = kScrW;
    const dmd_rect_t all{0, 0, kScrW, kScrH};
    dmd_rect_t r{};
    bool dmd = fit != 3 && fit_rect(fit, dot, &r);
    if (fit == 2 || fit == 3) {   // Popper's backglass: above the DMD, or the whole screen
        const dmd_rect_t to = fit == 3 || !dmd ? all : dmd_rect_t{0, 0, kScrW, r.y};
        if (u.bg) put_img(p, u.bg, dmd_rect_t{0, 0, (int)u.bg->header.w, (int)u.bg->header.h}, scaled(to, num, den));
        else dmd_fill565(p.px, p.w, p.h, scaled(to, num, den), kStandIn);
    } else if (fit == 4) {   // the art's part above its window, at its size (the band below)
        const int h = u.art && u.win.y > 0 ? u.win.y : u.art ? kScrH : kScrH - 320;
        if (u.art) put_img(p, u.art, dmd_rect_t{0, 0, kScrW, h}, scaled(dmd_rect_t{0, 0, kScrW, h}, num, den));
        else dmd_fill565(p.px, p.w, p.h, scaled(dmd_rect_t{0, 0, kScrW, h}, num, den), kStandIn);
    } else if (fit == 7 || fit == 8) {
        if (win_of(fit)) {
            put_img(p, fit == 7 ? u.art : u.b2s, all, scaled(all, num, den));
        } else {   // (every table, no picture here) a picture with a dark window in it, the DMD inside
            dmd_fill565(p.px, p.w, p.h, scaled(all, num, den), kStandIn);
            dmd_fill565(p.px, p.w, p.h, scaled(dmd_rect_t{160, 300, 704, 200}, num, den), 0x0000);
            r = dmd_rect_t{176, 316, 672, 168};
            dmd = true;
        }
    }
    if (dmd) put_dmd_fit(p, scaled(r, num, den), fit, (dot >> 1) & 3);
}

// The dots as this panel really draws them, 1:1 and not scaled: the middle of the DMD in the choice's fit,
// cut to p's size. The difference between the fits that thumbnails cannot show: dot size, gaps between rows.
void draw_lens(Pic &p, int choice, int dot) {
    Ui &u = *s_ui;
    memset(p.px, 0, (size_t)p.w * (size_t)p.h * 2);
    const int fit = shown(choice);
    if (!u.frame || !u.geom || fit == 3) return;   // (backglass only: no dots)
    dmd_masks_t *mk = (dmd_masks_t *)heap_caps_malloc(sizeof(dmd_masks_t), MALLOC_CAP_SPIRAM);
    uint16_t *band = (uint16_t *)heap_caps_malloc((size_t)kScrW * (size_t)(p.h + 2 * DMD_PITCH_MAX) * 2, MALLOC_CAP_SPIRAM);
    int shape = 0, bright = 100, rot = 0;
    bool glow = false;
    dmd_app_look(&shape, &glow, &bright, &rot);
    dmd_geom_t *g = u.geom;
    int w = 0, h = 0;
    const uint32_t *px = nullptr;
    // the geometry and the frame from the same snapshot: the frame's rows are what the geometry expects
    if (mk && band && dmd_app_fit_geom_wh(fit, dot, win_of(fit), u.fw, u.fh, g)) px = frame_for(fit, (dot >> 1) & 3, &w, &h);
    if (px) {
        dmd_masks_build(mk, g, shape, glow);
        // the crop: centred on the DMD, inside the screen
        int x0 = g->ox + g->gw / 2 - p.w / 2, y0 = g->oy + g->gh / 2 - p.h / 2;
        x0 = x0 < 0 ? 0 : x0 > kScrW - p.w ? kScrW - p.w : x0;
        y0 = y0 < 0 ? 0 : y0 > kScrH - p.h ? kScrH - p.h : y0;
        // the dot rows covering it, drawn into a band (its row 0 = the first of them)
        int cy0 = 0, cy1 = g->rows;
        while (cy0 < g->rows && g->oy + dmd_cell_y(g, cy0 + 1) <= y0) cy0++;
        while (cy1 > cy0 && g->oy + dmd_cell_y(g, cy1 - 1) >= y0 + p.h) cy1--;
        const int top = g->oy + dmd_cell_y(g, cy0), bh = dmd_cell_y(g, cy1) - dmd_cell_y(g, cy0);
        if (cy1 > cy0 && bh <= p.h + 2 * DMD_PITCH_MAX) {
            memset(band, 0, (size_t)kScrW * (size_t)bh * 2);
            dmd_render_rows(band, kScrW, g, mk, px, w, h, rot, bright, true, kScrW, cy0, cy1, nullptr);
            for (int y = 0; y < p.h; y++) {
                const int by = y0 + y - top;
                if (by >= 0 && by < bh) memcpy(p.px + (size_t)y * p.w, band + (size_t)by * kScrW + x0, (size_t)p.w * 2);
            }
        }
    }
    heap_caps_free(mk);
    heap_caps_free(band);
}

void refresh(lv_obj_t *img) {
    if (!img) return;
    const void *src = lv_image_get_src(img);
    if (src) lv_image_cache_drop(src);
    lv_obj_invalidate(img);
}

void mark(Card *cards, int n, int sel) {
    for (int i = 0; i < n; i++) {
        if (!cards[i].obj) continue;
        const bool on = cards[i].what == sel;
        lv_obj_set_style_border_color(cards[i].obj, hx(on ? kAccent : kEdge), 0);
        lv_obj_set_style_border_width(cards[i].obj, on ? 3 : 1, 0);
        lv_obj_set_style_bg_color(cards[i].obj, hx(on ? 0x0E3440 : kCard), 0);
    }
}

// The fit chosen here: the table's own (-1: as every table), or the one for every table.
int cur_fit(void) { return s_ui->gid ? dmd_app_table_fit(s_ui->gid) : dmd_app_fit_all(); }

// The chosen fit's name and what it does; automatic and "as every table" say what they come to here.
void describe_fit(int fit) {
    Ui &u = *s_ui;
    char nm[160], t[480], d[240];
    const int choice = fit;
    if (fit < 0) {
        line_of(nv_tr(NV_STR_DMDQ_FIT_ALL), 0, nm, sizeof nm);
        const int all = dmd_app_fit_all();
        char an[64];
        line_of(nv_tr(NV_STR_DMDQ_FIT_NAMES), all, an, sizeof an);
        snprintf(t, sizeof t, nv_tr(NV_STR_DMDC_ALL_DESC_FMT), an);
    } else {
        line_of(nv_tr(NV_STR_DMDQ_FIT_NAMES), fit, nm, sizeof nm);
        line_of(nv_tr(NV_STR_DMDC_FIT_DESCS), fit, t, sizeof t);
    }
    // what it comes to here, after the name: Automatic resolved (or not before the table runs), a picture
    // missing, a rotated screen (the board then shows another fit: the previews show that one)
    const int chosen = choice < 0 ? dmd_app_fit_all() : choice;
    bool later = false;
    if (chosen == kFitAuto) auto_of(&later);
    const int sh = shown(choice);
    char an[96] = "";
    if (later) snprintf(an, sizeof an, "%s", nv_tr(NV_STR_DMDX_AUTO_LATER));
    else if (sh != chosen) line_of(nv_tr(NV_STR_DMDQ_FIT_NAMES), sh, an, sizeof an);
    if (an[0]) {
        snprintf(d, sizeof d, "%.80s  \xC2\xBB  %.90s", nm, an);
        snprintf(nm, sizeof nm, "%.159s", d);
    }
    int shape = 0, bright = 100, rot = 0;
    bool glow = false;
    dmd_app_look(&shape, &glow, &bright, &rot);
    const int res = resolved(choice);
    if (rot && sh == 1 && res != 1 && res != 0) {   // the rotation, not a missing picture, chose the classic DMD
        const size_t o = strlen(t);
        snprintf(t + o, sizeof t - o, "\n%s", nv_tr(NV_STR_DMDX_ROT_CLASSIC));
    }
    if (u.fname) lv_label_set_text(u.fname, nm);
    if (u.desc) lv_label_set_text(u.desc, t);
}

void redraw_panel(void) {
    Ui &u = *s_ui;
    const int dot = dmd_app_table_dot(u.gid);
    for (Card &c : u.fit)
        if (c.obj && c.pic.px) {
            draw_panel(c.pic, c.what - 1, dot);
            refresh(c.img);
        }
    const int fit = cur_fit();
    if (u.ppic.px) {
        draw_lens(u.ppic, fit, dot);
        refresh(u.big_p);
    }
    describe_fit(fit);
    mark(u.fit, kFits, fit + 1);
    pills_mark(u.dots, 2, dot & 1);
    pills_mark(u.marg, 4, (dot >> 1) & 3);
    pills_mark(u.rb, 2, dmd_app_table_rebuild(u.gid) ? 0 : 1);
}

// A Future Pinball table's monitor: Popper's backglass at the top with its DMD under it, or on the whole monitor.
void draw_fp(Pic &p, bool full) {
    Ui &u = *s_ui;
    memset(p.px, 0, (size_t)p.w * (size_t)p.h * 2);
    const int bh = full ? p.h : p.h * 750 / 1024;   // Popper's backglass screen: 1280x750 over its DMD
    const dmd_rect_t r{0, 0, p.w, bh};
    if (u.bg) put_img(p, u.bg, dmd_rect_t{0, 0, (int)u.bg->header.w, (int)u.bg->header.h}, r);
    else dmd_fill565(p.px, p.w, p.h, r, kStandIn);
    if (!full) dmd_fill565(p.px, p.w, p.h, dmd_rect_t{p.w / 4, bh + (p.h - bh) / 4, p.w / 2, (p.h - bh) / 2}, 0x2945);
}

void show(lv_obj_t *o, bool on) {
    if (!o) return;
    if (on) lv_obj_remove_flag(o, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
}

// A Future Pinball table: its two choices instead of B2S's compositions.
void redraw_fp(const DmdB2sInfo *in) {
    Ui &u = *s_ui;
    const int full = in->fpfull ? 1 : 0;
    if (u.s2pic.px) {
        draw_fp(u.s2pic, full);
        refresh(u.big_s2);
    }
    char t[400];
    if (u.s2name) {
        line_of(nv_tr(NV_STR_DMDC_FP_NAMES), full, t, sizeof t);
        lv_label_set_text(u.s2name, t);
    }
    if (u.s2desc) {
        line_of(nv_tr(NV_STR_DMDC_FP_DESCS), full, t, sizeof t);
        lv_label_set_text(u.s2desc, t);
    }
    lv_obj_t *b2s_objs[] = {u.s2dmd_lbl, u.s2dmd[0], u.s2dmd[1], u.s2dmd_why, u.dual_lbl, u.dual[0], u.dual[1],
                            u.dual_note, u.s2row};
    for (lv_obj_t *o : b2s_objs) show(o, false);
    lv_obj_t *fp_objs[] = {u.fp_lbl, u.fpfull[0], u.fpfull[1]};
    for (lv_obj_t *o : fp_objs) show(o, true);
    pills_mark(u.fpfull, 2, full);
}

// The monitor's choice: its name, what it does, the DMD there, the dual version; cards that need a DMD there
// only when it is.
void redraw_s2(void) {
    Ui &u = *s_ui;
    const DmdB2sInfo *fpi = info_of(u.gid);
    if (fpi && fpi->fp) {
        redraw_fp(fpi);
        return;
    }
    lv_obj_t *fp_objs[] = {u.fp_lbl, u.fpfull[0], u.fpfull[1]};
    for (lv_obj_t *o : fp_objs) show(o, false);
    lv_obj_t *b2s_objs[] = {u.s2dmd_lbl, u.s2dmd[0], u.s2dmd[1], u.s2row};
    for (lv_obj_t *o : b2s_objs) show(o, true);
    const int cur = cur_s2();
    if (u.s2pic.px) {
        draw_s2(u.s2pic, cur);
        refresh(u.big_s2);
    }
    for (Card &c : u.s2)
        if (c.obj && c.pic.px) {
            draw_s2(c.pic, c.what);
            refresh(c.img);
        }
    mark(u.s2, DMD_S2_N, cur);
    char t[400];
    if (u.s2name) {
        line_of(nv_tr(NV_STR_DMDC_S2_NAMES), cur, t, sizeof t);
        lv_label_set_text(u.s2name, t);
    }
    if (u.s2desc) {
        const DmdB2sInfo *in = info_of(u.gid);
        if (in && !in->b2s) snprintf(t, sizeof t, "%s", nv_tr(NV_STR_DMDC_NO_B2S));
        else line_of(nv_tr(NV_STR_DMDC_S2_DESCS), cur, t, sizeof t);
        lv_label_set_text(u.s2desc, t);
    }
    pills_mark(u.s2dmd, 2, s2_dmd_on() ? 1 : 0);
    // "as it came" writes nothing for the table: no DMD of ours to add there (the toggle would come back)
    const bool keep = cur == DMD_S2_KEEP;
    for (lv_obj_t *o : u.s2dmd) pill_enable(o, !keep);
    if (u.s2dmd_why) {
        if (keep) lv_obj_remove_flag(u.s2dmd_why, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(u.s2dmd_why, LV_OBJ_FLAG_HIDDEN);
    }
    const DmdB2sInfo *in = info_of(u.gid);
    const bool dual = in && in->dual;
    lv_obj_t *dual_objs[] = {u.dual_lbl, u.dual[0], u.dual[1], u.dual_note};
    for (lv_obj_t *o : dual_objs)
        if (o) {
            if (dual) lv_obj_remove_flag(o, LV_OBJ_FLAG_HIDDEN);
            else lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
        }
    if (dual) pills_mark(u.dual, 2, in->mode == 2 ? 1 : 0);
}

// "Pictures of this table: ..." from the bridge's answer and the pictures here.
void describe_pics(void) {
    Ui &u = *s_ui;
    if (!u.pics) return;
    const DmdB2sInfo *in = info_of(u.gid);
    char parts[4][120], list[400] = "", t[480];
    const char *names = nv_tr(NV_STR_DMDC_IMG_NAMES);
    for (int i = 0; i < 4; i++) line_of(names, i, parts[i], sizeof parts[i]);
    size_t o = 0;
    if (in && in->b2s && in->bw > 0) {
        char b[80];
        snprintf(b, sizeof b, parts[0], in->bw, in->bh);
        o += (size_t)snprintf(list + o, sizeof list - o, "%s%s%s", b, in->grill > 0 ? " " : "", in->grill > 0 ? parts[1] : "");
        if (in->aw > 0 && o < sizeof list) {
            snprintf(b, sizeof b, parts[2], in->aw, in->ah);
            o += (size_t)snprintf(list + o, sizeof list - o, "  \xC2\xB7  %s", b);
        }
        if (in->dual && o < sizeof list) snprintf(list + o, sizeof list - o, "  \xC2\xB7  %s", parts[3]);
        snprintf(t, sizeof t, nv_tr(NV_STR_DMDC_IMAGES_FMT), list);
    } else if (in && !in->b2s) {
        snprintf(t, sizeof t, "%s", nv_tr(NV_STR_DMDC_NO_B2S));
    } else {
        snprintf(t, sizeof t, "%s", u.game ? "" : nv_tr(u.info_failed ? NV_STR_DMDX_INFO_FAIL : NV_STR_DMDC_LOADING));
    }
    lv_label_set_text(u.pics, t);
}

// ---- the monitor's compositions: kept per table, sent when the cable is the bridge's
// nv_config: "dmds2_<id>" the composition + 1, "dmds2d_<id>" a DMD on the monitor too, "dmds2j_<id>" the
// request for the bridge, "dmd_s2q" the tables whose request is still to be sent (ids, comma-separated)

// The ids in the queue, in order (at most max).
int queue_ids(int *ids, int max) {
    char q[kQCap];
    nv_config_get_str("dmd_s2q", "", q, sizeof q);
    int n = 0;
    char *save = nullptr;
    for (char *t = strtok_r(q, ",", &save); t && n < max; t = strtok_r(nullptr, ",", &save)) {
        const int id = atoi(t);
        if (id > 0) ids[n++] = id;
    }
    return n;
}

void queue_edit(int gid, bool add) {
    int ids[130], n = 0, got[128];
    const int k = queue_ids(got, 128);
    for (int i = 0; i < k; i++)
        if (got[i] != gid) ids[n++] = got[i];
    if (add) ids[n++] = gid;   // the newest last
    // too many waiting (never seen: ~100 tables changed with no bridge): the oldest give way, logged
    char out[kQCap];
    for (int first = 0;; first++) {
        size_t o = 0;
        out[0] = 0;
        for (int i = first; i < n && o < sizeof out; i++) o += (size_t)snprintf(out + o, sizeof out - o, "%s%d", o ? "," : "", ids[i]);
        if (o < sizeof out) break;
        nv_log_write(NV_LOG_WARN, "dmd", "second screen of %d dropped from the queue (full)", ids[first]);
    }
    nv_config_set_str("dmd_s2q", out);
}

void choose_s2(int mode) {
    Ui &u = *s_ui;
    char k[24], j[320];
    snprintf(k, sizeof k, "dmds2_%d", u.gid);
    nv_config_set_int(k, mode + 1);
    const dmd_s2_in_t in = s2_in();
    dmd_s2_out_t o;
    if (mode != DMD_S2_KEEP && dmd_s2_layout(mode, &in, &o)) {
        char art[48] = "null", dmd[48] = "null";
        const bool dmd_there = s2_dmd_on() || mode == DMD_S2_BG_DMD;
        if (o.art.w) snprintf(art, sizeof art, "[%d,%d,%d,%d]", o.art.x, o.art.y, o.art.w, o.art.h);
        if (o.dmd.w && dmd_there) snprintf(dmd, sizeof dmd, "[%d,%d,%d,%d]", o.dmd.x, o.dmd.y, o.dmd.w, o.dmd.h);
        snprintf(j, sizeof j, "{\"q\":\"s2\",\"id\":%d,\"mode\":%d,\"bg\":[%d,%d,%d,%d],\"grill\":%s,\"art\":%s,\"dmd\":%s}",
                 u.gid, mode, o.bg.x, o.bg.y, o.bg.w, o.bg.h, o.grill ? "true" : "false", art, dmd);
    } else {
        snprintf(j, sizeof j, "{\"q\":\"s2\",\"id\":%d,\"mode\":0}", u.gid);
    }
    snprintf(k, sizeof k, "dmds2j_%d", u.gid);
    nv_config_set_str(k, j);
    queue_edit(u.gid, true);
    nv_log_write(NV_LOG_INFO, "dmd", "second screen of %d: %s", u.gid, j);
    dmd_comp_flush();
    if (u.game) {
        if (u.restart) lv_obj_remove_flag(u.restart, LV_OBJ_FLAG_HIDDEN);
        if (u.note) lv_label_set_text(u.note, nv_tr(NV_STR_DMDC_NEXT));
    }
}

// ---- events

void close_cb(lv_event_t *) { dmd_comp_close(); }

void tab_show(int t) {
    Ui &u = *s_ui;
    u.tab_on = t;
    for (int i = 0; i < 2; i++) {
        pill_mark(u.tab[i], i == t);
        if (!u.page[i]) continue;
        if (i == t) lv_obj_remove_flag(u.page[i], LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(u.page[i], LV_OBJ_FLAG_HIDDEN);
    }
}
void tab_cb(lv_event_t *e) {
    if (s_ui) tab_show((int)(intptr_t)lv_event_get_user_data(e));
}

void s2_cb(lv_event_t *e) {
    if (!s_ui) return;
    const int mode = (int)(intptr_t)lv_event_get_user_data(e);
    if (mode == cur_s2()) return;
    choose_s2(mode);
    redraw_s2();
}

void s2dmd_cb(lv_event_t *e) {
    if (!s_ui) return;
    const bool on = lv_event_get_user_data(e) != nullptr;
    if (on == s2_dmd_on()) return;
    char k[24];
    snprintf(k, sizeof k, "dmds2d_%d", s_ui->gid);
    nv_config_set_int(k, on ? 1 : 0);
    if (cur_s2() != DMD_S2_KEEP) choose_s2(cur_s2());   // written again with or without it
    redraw_s2();
}

// A Future Pinball table's backglass: asked of the bridge now (it answers with the table's description, which
// confirms it); from the next game.
void fpfull_cb(lv_event_t *e) {
    if (!s_ui) return;
    const int full = lv_event_get_user_data(e) ? 1 : 0;
    char j[80];
    snprintf(j, sizeof j, "{\"q\":\"fpfull\",\"id\":%d,\"full\":%d}", s_ui->gid, full);
    if (!dmd_menu_send(j)) {
        dmd_menu_send_fail_note();
        return;
    }
    nv_log_write(NV_LOG_INFO, "dmd", "Future Pinball backglass of %d: %s", s_ui->gid, full ? "full screen" : "as Popper");
    for (int i = 0; s_info && i < 12; i++)
        if (s_info->i[i].id == s_ui->gid) s_info->i[i].fpfull = full;
    if (s_ui->game && s_ui->note) lv_label_set_text(s_ui->note, nv_tr(NV_STR_DMDC_NEXT));
    redraw_s2();
}

void dual_cb(lv_event_t *e) {
    if (!s_ui) return;
    const int mode = lv_event_get_user_data(e) ? 2 : 1;
    char j[80];
    snprintf(j, sizeof j, "{\"q\":\"dual\",\"id\":%d,\"mode\":%d}", s_ui->gid, mode);
    // not queued (the version is B2S's own setting, asked for now): say the cable is busy, not "next game"
    if (!dmd_menu_send(j)) dmd_menu_send_fail_note();
}

void fit_cb(lv_event_t *e) {
    if (!s_ui) return;
    const int fit = (int)(intptr_t)lv_event_get_user_data(e) - 1;
    if (s_ui->gid) dmd_app_set_table_fit(s_ui->gid, fit);
    else dmd_app_set_fit_all(fit);
    redraw_panel();
}

void dots_cb(lv_event_t *e) {
    if (!s_ui) return;
    const int v = (int)(intptr_t)lv_event_get_user_data(e);
    const int dot = dmd_app_table_dot(s_ui->gid);
    dmd_app_set_table_dot(s_ui->gid, v < 2 ? (dot & ~1) | v : (dot & 1) | (v - 2) << 1);
    redraw_panel();
}

void rb_cb(lv_event_t *e) {
    if (!s_ui) return;
    dmd_app_set_table_rebuild(s_ui->gid, lv_event_get_user_data(e) == nullptr);
    redraw_panel();
}

void restart_cb(lv_event_t *) {
    dmd_comp_close();
    dmd_menu_quit_and_relaunch();
}

// ---- building

// A row of cards that scrolls sideways.
lv_obj_t *strip(lv_obj_t *parent, int y, int h) {
    lv_obj_t *s = box(parent);
    lv_obj_set_pos(s, 0, y);
    lv_obj_set_size(s, kScrW, h);
    lv_obj_add_flag(s, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(s, LV_DIR_HOR);
    lv_obj_set_scrollbar_mode(s, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_flex_flow(s, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_left(s, 16, 0);
    lv_obj_set_style_pad_right(s, 16, 0);
    lv_obj_set_style_pad_column(s, 12, 0);
    return s;
}

bool card(Card &c, lv_obj_t *row, int tw, int th, const char *name, int what, lv_event_cb_t cb) {
    if (!pic_alloc(c.pic, tw, th)) return false;
    c.what = what;
    c.obj = box(row);
    lv_obj_set_size(c.obj, tw + 10, th + 52);   // the name may take two lines
    lv_obj_set_style_bg_opa(c.obj, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(c.obj, 8, 0);
    lv_obj_add_flag(c.obj, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(c.obj, LV_OBJ_FLAG_SCROLL_ON_FOCUS);
    lv_obj_add_event_cb(c.obj, cb, LV_EVENT_CLICKED, (void *)(intptr_t)what);
    c.img = lv_image_create(c.obj);
    lv_image_set_src(c.img, &c.pic.dsc);
    lv_obj_set_pos(c.img, 5, 5);
    lv_obj_t *l = lv_label_create(c.obj);
    lv_obj_set_style_text_font(l, &nv_font_14, 0);
    lv_obj_set_style_text_color(l, hx(kInk), 0);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(l, tw);
    lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
    lv_label_set_text(l, name);
    lv_obj_set_pos(l, 5, th + 8);
    lv_obj_clear_flag(c.img, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(l, LV_OBJ_FLAG_CLICKABLE);
    return true;
}

lv_obj_t *page_box(lv_obj_t *scr) {
    lv_obj_t *p = box(scr);
    lv_obj_set_pos(p, 0, kTop);
    lv_obj_set_size(p, kScrW, kScrH - kTop);
    return p;
}

// Two pills (a choice of two) at x, y, each w wide; out[0], out[1].
void two(lv_obj_t *parent, const char *names, int x, int y, int w, lv_event_cb_t cb, lv_obj_t **out) {
    char a[48], b[48];
    line_of(names, 0, a, sizeof a);
    line_of(names, 1, b, sizeof b);
    out[0] = pill(parent, a, x, y, w, cb, nullptr);
    out[1] = pill(parent, b, x + w + 8, y, w, cb, (void *)1);
}

void gather(Ui &u) {
    u.b2s = dmd_menu_table_b2s(u.pid, &u.box, &u.grill, &u.housing);
    u.art = dmd_menu_table_full(u.pid, &u.win);
    if (u.art) dmd_menu_table_full_shape(u.pid, &u.aw, &u.ah);
    u.s2ok = dmd_menu_s2(&u.s2w, &u.s2h);
    u.geom = (dmd_geom_t *)heap_caps_malloc(sizeof(dmd_geom_t), MALLOC_CAP_SPIRAM);
    // the DMD's newest frame, whatever its size; nothing yet: a sample picture, so the dots can still be judged
    u.frame = (uint32_t *)heap_caps_malloc((size_t)DMD_MAX_PX * 4, MALLOC_CAP_SPIRAM);
    if (u.frame && !dmd_app_snapshot(u.frame, DMD_MAX_PX, &u.fw, &u.fh)) {
        dmd_text(u.frame, 128, 32, "DMD 128x32", 0xFF5820);
        u.fw = 128;
        u.fh = 32;
    }
    if (u.frame) {   // everything else from the frame the previews really have
        const size_t w = (size_t)u.fw, h = (size_t)u.fh;
        u.framem = (uint32_t *)heap_caps_malloc((w + 6) * (2 * h + 6) * 4, MALLOC_CAP_SPIRAM);
        u.frame2 = (uint32_t *)heap_caps_malloc(w * h * 2 * 4, MALLOC_CAP_SPIRAM);
        u.tmp2 = (uint32_t *)heap_caps_malloc(w * h * 2 * 4, MALLOC_CAP_SPIRAM);
    }
    // the backbox's backglass as tall as the board makes it (one picture kept by the menu: the same height)
    u.bg = dmd_menu_comp_art(u.pid, dmd_app_art_h(u.fw, u.fh, dmd_app_table_dot(u.gid)));
}

// The screen, its title and its close button: on top of everything under root.
bool open_screen(lv_obj_t *root, int gid, const char *what, int pid) {
    if (s_ui) dmd_comp_close();
    // PSRAM: a plain new under 16 KB would take internal SRAM (rules: memory tiers)
    void *mem = heap_caps_calloc(1, sizeof(Ui), MALLOC_CAP_SPIRAM);
    s_ui = mem ? new (mem) Ui() : nullptr;
    if (!s_ui) return false;
    Ui &u = *s_ui;
    u.gid = gid;
    u.pid = pid;
    u.root = root;
    snprintf(u.name, sizeof u.name, "%s", what ? what : "");
    gather(u);
    u.scr = box(root);
    lv_obj_set_size(u.scr, kScrW, kScrH);
    lv_obj_set_style_bg_opa(u.scr, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(u.scr, hx(0x000000), 0);
    lv_obj_add_flag(u.scr, LV_OBJ_FLAG_CLICKABLE);   // nothing under it gets the taps
    lv_obj_add_event_cb(u.scr, scr_deleted_cb, LV_EVENT_DELETE, s_ui);
    lv_obj_move_foreground(u.scr);
    char title[120];
    snprintf(title, sizeof title, "%s  \xC2\xB7  %s", nv_tr(NV_STR_DMDC_TITLE), u.name);
    lv_obj_t *t = label(u.scr, &nv_font_28, kAccent, title, 20, 10);
    lv_obj_set_width(t, kScrW - 20 - 76 - 12);
    lv_label_set_long_mode(t, LV_LABEL_LONG_DOT);
    pill(u.scr, LV_SYMBOL_CLOSE, kScrW - 76, 8, 60, close_cb, nullptr);
    return true;
}

// The fits this table can have (every table: every fit), in kOrder; "as every table" first for a table.
void fit_cards(Ui &u, lv_obj_t *row, int tw, int th) {
    char fits[480], *fn[kFits] = {(char *)nv_tr(NV_STR_DMDQ_FIT_ALL)};
    snprintf(fits, sizeof fits, "%s", nv_tr(NV_STR_DMDQ_FIT_NAMES));
    split(fits, fn + 1, kFits - 1);
    if (u.gid) card(u.fit[0], row, tw, th, fn[0] ? fn[0] : "", 0, fit_cb);
    for (int fit : kOrder) {
        const bool ok = !u.gid                  ? true
                        : fit == 4 || fit == 7 ? u.art != nullptr
                        : fit == 8             ? u.b2s && u.housing.w > 0
                        : fit == 2 || fit == 3 ? u.bg != nullptr
                                               : true;
        if (ok) card(u.fit[fit + 1], row, tw, th, fn[fit + 1] ? fn[fit + 1] : "", fit + 1, fit_cb);
    }
}

// The 1:1 lens of the chosen fit's dots, with its caption, at x, y (in parent).
void lens(Ui &u, lv_obj_t *parent, int x, int y) {
    label(parent, &nv_font_14, kDim, nv_tr(NV_STR_DMDC_LENS), x, y - 20);
    if (!pic_alloc(u.ppic, kBigPW, kBigH)) return;
    u.big_p = lv_image_create(parent);
    lv_image_set_src(u.big_p, &u.ppic.dsc);
    lv_obj_set_pos(u.big_p, x, y);
    lv_obj_set_style_border_width(u.big_p, 1, 0);
    lv_obj_set_style_border_color(u.big_p, hx(kEdge), 0);
}

// The monitor's page: its preview, what the choice does, a DMD there, the dual version, the choices.
void build_s2_page(Ui &u, lv_obj_t *pg) {
    if (!u.s2ok) {
        lv_obj_t *l = label(pg, &nv_font_20, kDim, nv_tr(NV_STR_DMDC_NO_S2), 20, 20);
        lv_obj_set_width(l, kScrW - 40);
        return;
    }
    const int bh = kBigS2W * u.s2h / u.s2w;
    if (pic_alloc(u.s2pic, kBigS2W, bh > 200 ? 200 : bh)) {
        u.big_s2 = lv_image_create(pg);
        lv_image_set_src(u.big_s2, &u.s2pic.dsc);
        lv_obj_set_pos(u.big_s2, 20, 0);
        lv_obj_set_style_border_width(u.big_s2, 1, 0);
        lv_obj_set_style_border_color(u.big_s2, hx(kEdge), 0);
    }
    const int x = 20 + kBigS2W + 24, w = kScrW - x - 20;
    u.s2name = label(pg, &nv_font_28, kInk, "", x, 0);
    u.s2desc = label(pg, &nv_font_14, kDim, "", x, 38);
    lv_obj_set_width(u.s2desc, w);
    lv_label_set_long_mode(u.s2desc, LV_LABEL_LONG_WRAP);
    const int yes_x = kScrW - 20 - 2 * 96 - 8, rs_x = kScrW - 20 - 220;
    // (the descriptions stop short of the choices on their right)
    lv_obj_set_height(u.s2desc, 18 * 3);
    lv_label_set_long_mode(u.s2desc, LV_LABEL_LONG_DOT);
    lv_obj_t *dl = u.s2dmd_lbl = label(pg, &nv_font_20, kInk, nv_tr(NV_STR_DMDC_S2_DMD), x, 98);
    lv_obj_set_width(dl, yes_x - x - 8);
    lv_label_set_long_mode(dl, LV_LABEL_LONG_DOT);
    // a Future Pinball table, backbox off: where its backglass goes (shown instead of what B2S has)
    const int fp_x = kScrW - 20 - 2 * 200 - 8;
    u.fp_lbl = label(pg, &nv_font_20, kInk, nv_tr(NV_STR_DMDC_FP), x, 98);
    lv_obj_set_width(u.fp_lbl, fp_x - x - 8);
    lv_label_set_long_mode(u.fp_lbl, LV_LABEL_LONG_DOT);
    two(pg, nv_tr(NV_STR_DMDC_FP_NAMES), fp_x, 90, 200, fpfull_cb, u.fpfull);
    lv_obj_t *fp_objs[] = {u.fp_lbl, u.fpfull[0], u.fpfull[1]};
    for (lv_obj_t *o : fp_objs) lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
    two(pg, nv_tr(NV_STR_DMDC_NOYES), yes_x, 90, 96, s2dmd_cb, u.s2dmd);
    u.s2dmd_why = label(pg, &nv_font_14, kDim, nv_tr(NV_STR_DMDX_S2_DMD_KEEP), x, 124);
    lv_obj_set_width(u.s2dmd_why, yes_x - x - 8);
    lv_label_set_long_mode(u.s2dmd_why, LV_LABEL_LONG_DOT);
    u.dual_lbl = label(pg, &nv_font_20, kInk, nv_tr(NV_STR_DMDC_DUAL), x, 146);
    lv_obj_set_width(u.dual_lbl, kScrW - 20 - 2 * 150 - 8 - x - 8);
    lv_label_set_long_mode(u.dual_lbl, LV_LABEL_LONG_DOT);
    two(pg, nv_tr(NV_STR_DMDC_DUAL_NAMES), kScrW - 20 - 2 * 150 - 8, 138, 150, dual_cb, u.dual);
    u.dual_note = label(pg, &nv_font_14, kDim, nv_tr(NV_STR_DMDC_DUAL_NOTE), x, 184);
    lv_obj_set_size(u.dual_note, rs_x - x - 12, 18 * 2);   // clear of "Restart the table now"
    lv_label_set_long_mode(u.dual_note, LV_LABEL_LONG_DOT);
    // in a game: the monitor changes from the next one (under the preview), or now (the table quits and
    // starts again)
    u.note = label(pg, &nv_font_14, kDim, "", 20, 202);
    lv_obj_set_size(u.note, kBigS2W, 18 * 2);
    lv_label_set_long_mode(u.note, LV_LABEL_LONG_DOT);
    u.restart = pill(pg, nv_tr(NV_STR_DMDC_RESTART), rs_x, 196, 220, restart_cb, nullptr);
    lv_obj_add_flag(u.restart, LV_OBJ_FLAG_HIDDEN);
    // the compositions this table can have
    char names[480], *n[DMD_S2_N] = {};
    snprintf(names, sizeof names, "%s", nv_tr(NV_STR_DMDC_S2_NAMES));
    split(names, n, DMD_S2_N);
    const int th = kS2TW * u.s2h / u.s2w;
    lv_obj_t *row = u.s2row = strip(pg, 238, th + 56);
    const dmd_s2_in_t in = s2_in();
    for (int m = 0; m < DMD_S2_N; m++)
        if (dmd_s2_avail(m, &in)) card(u.s2[m], row, kS2TW, th, n[m] ? n[m] : "", m, s2_cb);
}

// This screen's page for a table: the lens, what the fit does, its dots, the fits.
void build_panel_page(Ui &u, lv_obj_t *pg) {
    lens(u, pg, 20, 20);
    const int x = 20 + kBigPW + 24, w = kScrW - x - 20;
    u.fname = label(pg, &nv_font_28, kInk, "", x, 0);
    lv_obj_set_width(u.fname, w);
    lv_label_set_long_mode(u.fname, LV_LABEL_LONG_DOT);
    u.desc = label(pg, &nv_font_14, kDim, "", x, 38);
    lv_obj_set_size(u.desc, w, 18 * 3);   // (never under the choices below it)
    lv_label_set_long_mode(u.desc, LV_LABEL_LONG_DOT);
    label(pg, &nv_font_14, kDim, nv_tr(NV_STR_DMDX_DOT_SIZE), x, 96);
    u.dots[0] = pill(pg, nv_tr(NV_STR_DMDC_DOTS_FIT), x, 116, 150, dots_cb, (void *)(intptr_t)0);
    u.dots[1] = pill(pg, nv_tr(NV_STR_DMDC_DOTS_ORIG), x + 158, 116, 150, dots_cb, (void *)(intptr_t)1);
    label(pg, &nv_font_14, kDim, nv_tr(NV_STR_DMDC_MARGIN), x + 340, 96);
    for (int i = 0; i < 4; i++) {
        char nm[4];
        snprintf(nm, sizeof nm, "%d", i);
        u.marg[i] = pill(pg, nm, x + 340 + i * 62, 116, 54, dots_cb, (void *)(intptr_t)(2 + i));
    }
    if (u.game && dmd_app_dmd_half()) {   // a 128x16 DMD: rebuilt to the full height, or as it comes
        u.rb[0] = pill(pg, nv_tr(NV_STR_DMDQ_RB_ON), x, 164, 232, rb_cb, nullptr);
        u.rb[1] = pill(pg, nv_tr(NV_STR_DMDQ_RB_OFF), x + 240, 164, 232, rb_cb, (void *)1);
    }
    fit_cards(u, strip(pg, 222, kPTH + 56), kPTW, kPTH);
}

// A table's screens (the running game's, or any from the menu), on tab `tab` (0 the monitor, 1 this screen;
// with no second screen known: this screen).
bool open_table(lv_obj_t *root, int gid, const char *name, bool game, int tab) {
    if (!root || gid <= 0 || !open_screen(root, gid, name, gid)) return false;
    Ui &u = *s_ui;
    u.game = game;
    if (!info_of(gid)) {   // (in a game too: a game without dmdext leaves the cable to the bridge)
        char j[64];
        snprintf(j, sizeof j, "{\"q\":\"b2sinfo\",\"id\":%d}", gid);
        dmd_menu_send(j);
        u.info_us = esp_timer_get_time();
    }
    u.tab[0] = pill(u.scr, nv_tr(NV_STR_DMDC_S2), 20, 52, 0, tab_cb, (void *)0);
    lv_obj_update_layout(u.tab[0]);
    u.tab[1] = pill(u.scr, nv_tr(NV_STR_DMDC_PANEL), 20 + lv_obj_get_width(u.tab[0]) + 8, 52, 0, tab_cb, (void *)1);
    u.pics = label(u.scr, &nv_font_14, kDim, "", 20, 96);
    lv_obj_set_size(u.pics, kScrW - 40, 18 * 2);   // two lines at most
    lv_label_set_long_mode(u.pics, LV_LABEL_LONG_DOT);
    u.page[0] = page_box(u.scr);
    u.page[1] = page_box(u.scr);
    build_s2_page(u, u.page[0]);
    build_panel_page(u, u.page[1]);
    tab_show(u.s2ok ? tab : 1);
    describe_pics();
    redraw_s2();
    redraw_panel();
    return true;
}

void close_ui(bool user);

// A picture of the screen's table came: built again around it, once the current event is over (a picture
// read from the card arrives while the screen is still being built). Several in a row: one rebuild.
bool s_refresh_due;
void refresh_async(void *) {
    s_refresh_due = false;
    if (!s_ui) return;
    if (s_ui->all) {   // every table: the same screen again, with the pictures that came
        lv_obj_t *root = s_ui->root;
        void (*on_close)(void) = s_ui->on_close;
        const int pid = s_ui->pid;
        s_ui->on_close = nullptr;
        close_ui(false);
        dmd_comp_open_all(root, on_close);   // (it asks for the rest of the pictures)
        (void)pid;
        return;
    }
    lv_obj_t *root = s_ui->root;
    char name[64];
    snprintf(name, sizeof name, "%s", s_ui->name);
    const int id = s_ui->gid, tab = s_ui->tab_on;
    const bool game = s_ui->game;
    void (*on_close)(void) = s_ui->on_close;
    s_ui->on_close = nullptr;   // not closed for the user: no callback
    close_ui(false);
    if (open_table(root, id, name, game, tab)) {
        s_ui->on_close = on_close;
        dmd_menu_comp_fetch(id);   // the rest of its pictures, if any are still to come
    }
}

// The table's pictures asked for (the card, else the bridge): after the screen is built, never inside it.
bool fetch(bool ok, int gid) {
    if (ok) dmd_menu_comp_fetch(gid);
    return ok;
}

}  // namespace

bool dmd_comp_open(lv_obj_t *root) {
    const int gid = dmd_menu_game_id();
    // the quick action "this table's composition" is about the DMD here: this screen's page first
    return gid > 0 && fetch(open_table(root, gid, dmd_menu_game(), true, 1), gid);
}

bool dmd_comp_open_table(lv_obj_t *root, int id, const char *name) {
    return fetch(open_table(root, id, name, id == dmd_menu_game_id(), 0), id);
}

bool dmd_comp_open_all(lv_obj_t *root, void (*on_close)(void)) {
    // the previews with a real table's pictures: the running one, else the one selected in the menu
    const int pid = dmd_menu_game_id() ? dmd_menu_game_id() : dmd_menu_selected();
    if (!root || !open_screen(root, 0, nv_tr(NV_STR_DMDC_ALL_TABLES), pid)) return false;
    Ui &u = *s_ui;
    u.on_close = on_close;
    u.all = true;
    lens(u, u.scr, 20, 76);
    // the chosen fit: its name and what it does
    u.fname = label(u.scr, &nv_font_28, kInk, "", 20 + kBigPW + 24, 72);
    u.desc = label(u.scr, &nv_font_20, kDim, "", 20 + kBigPW + 24, 116);
    lv_obj_set_width(u.desc, kScrW - 20 - kBigPW - 24 - 20);
    lv_label_set_long_mode(u.desc, LV_LABEL_LONG_WRAP);
    lv_obj_t *n = label(u.scr, &nv_font_14, kDim, nv_tr(NV_STR_DMDC_S2_PER_TABLE), 20 + kBigPW + 24, 212);
    lv_obj_set_width(n, kScrW - 20 - kBigPW - 24 - 20);
    lv_label_set_long_mode(n, LV_LABEL_LONG_WRAP);
    // every fit as a card, in a grid (no scrolling: all of them in view)
    label(u.scr, &nv_font_20, kInk, nv_tr(NV_STR_DMDC_PANEL), 20, 248);
    lv_obj_t *grid = box(u.scr);
    lv_obj_set_pos(grid, 20, 276);
    lv_obj_set_size(grid, kScrW - 40, kScrH - 276);
    lv_obj_set_flex_flow(grid, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_column(grid, 12, 0);
    lv_obj_set_style_pad_row(grid, 8, 0);
    fit_cards(u, grid, kAllTW, kAllTH);
    redraw_panel();
    if (!s_refresh_due && pid) dmd_menu_comp_fetch(pid);
    return true;
}

void dmd_comp_refresh(int id) {
    if (!s_ui || s_ui->pid != id || s_refresh_due) return;
    s_refresh_due = true;
    lv_async_call(refresh_async, nullptr);
}

void dmd_comp_on_info(const DmdB2sInfo *info) {
    if (!info || !info->id) return;
    if (!s_info) s_info = (Info *)heap_caps_calloc(1, sizeof(Info), MALLOC_CAP_SPIRAM);
    if (!s_info) return;
    DmdB2sInfo *slot = nullptr;
    for (DmdB2sInfo &e : s_info->i)
        if (e.id == info->id) slot = &e;
    if (!slot) slot = &s_info->i[s_info->n++ % 12];
    *slot = *info;
    // the monitor as the bridge has written it: the truth, unless a newer choice is still waiting to be sent
    char k[24];
    int ids[128];
    const int nq = queue_ids(ids, 128);
    bool waiting = false;
    for (int i = 0; i < nq; i++)
        if (ids[i] == info->id) waiting = true;
    if (!waiting) {
        snprintf(k, sizeof k, "dmds2_%d", info->id);
        const int have = nv_config_get_int(k, 0), want = info->s2 > 0 ? info->s2 + 1 : 1;
        if (have != want && !(want == 1 && have == 0)) nv_config_set_int(k, want);
        snprintf(k, sizeof k, "dmds2d_%d", info->id);
        if ((nv_config_get_int(k, 0) != 0) != info->s2dmd) nv_config_set_int(k, info->s2dmd ? 1 : 0);
    }
    if (s_ui && s_ui->gid == info->id) {
        describe_pics();
        redraw_s2();
    }
}

void dmd_comp_on_dual(int id, int mode, bool ok, const char *err) {
    if (ok) {
        if (s_info)
            for (DmdB2sInfo &e : s_info->i)
                if (e.id == id) e.mode = mode;
        nv_toast(NV_NOTE_INFO, nv_tr(NV_STR_DMDC_DUAL_NOTE));
    } else {
        char t[200];
        snprintf(t, sizeof t, nv_tr(NV_STR_DMDC_FAIL_FMT), err ? err : "");
        nv_toast(NV_NOTE_ERROR, t);
    }
    if (s_ui && s_ui->gid == id) redraw_s2();
}

namespace {
void ui_free(Ui *u) {
    for (Card &c : u->s2) pic_free(c.pic);
    for (Card &c : u->fit) pic_free(c.pic);
    pic_free(u->s2pic);
    pic_free(u->ppic);
    heap_caps_free(u->frame);
    heap_caps_free(u->framem);
    heap_caps_free(u->frame2);
    heap_caps_free(u->tmp2);
    heap_caps_free(u->geom);
    u->~Ui();
    heap_caps_free(u);
}

// user: closed for good (its pictures are no longer kept nor asked for, even with a rebuild still due);
// else closed to be built again at once with the pictures that came (they must stay).
void close_ui(bool user) {
    if (!s_ui) return;
    Ui *u = s_ui;
    s_ui = nullptr;
    void (*on_close)(void) = u->on_close;
    if (u->scr && lv_obj_is_valid(u->scr)) {   // its images go first: then their pictures
        lv_obj_remove_event_cb_with_user_data(u->scr, scr_deleted_cb, u);
        lv_obj_delete(u->scr);
    }
    ui_free(u);
    if (user) dmd_menu_comp_fetch(0);
    if (on_close) on_close();
}
}  // namespace

void dmd_comp_close(void) { close_ui(true); }

// The app's screen goes (closed, rebuilt): the caller's on_close would run in the middle of its teardown.
void dmd_comp_abandon(void) {
    if (s_ui) s_ui->on_close = nullptr;
    close_ui(true);
}

bool dmd_comp_is_open(void) { return s_ui != nullptr; }

// While a table's screen waits for its backglass file's description: asked again every 4 s (a request
// refused while the cable was busy, an answer lost), three times, then it says the PC did not answer.
void dmd_comp_tick(void) {
    if (!s_ui) return;
    Ui &u = *s_ui;
    if (!u.gid || u.game || u.all || u.info_failed || info_of(u.gid)) return;
    const int64_t now = esp_timer_get_time();
    if (now - u.info_us < 4000000) return;
    u.info_us = now;
    if (++u.info_tries > 3) {
        u.info_failed = true;
        describe_pics();
        return;
    }
    char j[64];
    snprintf(j, sizeof j, "{\"q\":\"b2sinfo\",\"id\":%d}", u.gid);
    dmd_menu_send(j);
}

void dmd_comp_flush(void) {
    int ids[128];   // read once: queue_edit below rewrites the queue while this goes through it
    const int n = queue_ids(ids, 128);
    for (int i = 0; i < n; i++) {
        const int id = ids[i];
        char k[24], j[320];
        snprintf(k, sizeof k, "dmds2j_%d", id);
        nv_config_get_str(k, "", j, sizeof j);
        if (!j[0]) {
            queue_edit(id, false);
            continue;
        }
        if (!dmd_menu_send(j)) return;   // the cable is dmdext's: later
    }
}

void dmd_comp_on_result(int game_id, bool ok, const char *err) {
    if (game_id > 0) queue_edit(game_id, false);   // done, or never possible: not again
    if (ok) {
        nv_toast(NV_NOTE_INFO, nv_tr(NV_STR_DMDC_SAVED));
    } else {
        char t[200];
        snprintf(t, sizeof t, nv_tr(NV_STR_DMDC_FAIL_FMT), err ? err : "");
        nv_toast(NV_NOTE_ERROR, t);
    }
    nv_log_write(ok ? NV_LOG_INFO : NV_LOG_WARN, "dmd", "second screen of %d: %s %s", game_id, ok ? "written" : "not written:",
                 ok ? "" : (err ? err : ""));
}
