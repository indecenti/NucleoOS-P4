// dmd_comp — the composition of both screens for the running table: what the PC's second screen shows (the
// backglass, with its grill and the DMD in the housing, the FullDMD art with the DMD in its window, dmdext's
// virtual DMD) and how this panel shows the DMD (its fit, original dots, a margin of unlit dots). Every
// choice is previewed with the table's own pictures and the DMD's last frame, drawn here at thumbnail size.
// The second screen's choice is sent to the bridge as rectangles (dmd_s2_layout): at once when the cable
// is the bridge's, else as soon as it is again (B2S and dmdext read them when the table starts; "restart"
// quits it and starts it again). LVGL thread.
#include "dmd_menu.h"

#include "dmd_core.h"
#include "nv_config.h"
#include "nv_fonts.h"
#include "nv_i18n.h"
#include "nv_log.h"
#include "nv_notify.h"

#include "esp_heap_caps.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>

extern "C" void lv_image_cache_drop(const void *src);   // LVGL 9 (not in the public header), as dmd_menu.cpp

namespace {

constexpr int kScrW = 1024, kScrH = 600;
constexpr int kFits = 10;                     // "as every table" + the panel's nine fits
constexpr int kS2TW = 128, kPTW = 150, kPTH = 88;   // thumbnails (the second screen's: its shape)
constexpr int kBigS2W = 200, kBigPW = 272, kBigH = 160;
constexpr uint32_t kAccent = 0x2FE0FF, kInk = 0xFFFFFF, kDim = 0x7F929E, kCard = 0x0C1418;

struct Pic {                       // a preview drawn here: its pixels and LVGL's description of them
    uint16_t *px = nullptr;
    int w = 0, h = 0;
    lv_image_dsc_t dsc{};
};

struct Card {
    lv_obj_t *obj = nullptr, *img = nullptr;
    Pic pic;
    int what = -1;                 // the composition (second screen) or the fit + 1 (panel; 0 = every table)
};

struct Ui {
    lv_obj_t *scr = nullptr, *big_s2 = nullptr, *big_p = nullptr, *note = nullptr, *restart = nullptr;
    lv_obj_t *dots[2] = {}, *marg[4] = {}, *rb[2] = {}, *rb_row = nullptr;
    Pic s2pic, ppic;
    Card s2[DMD_S2_N], fit[kFits];
    int gid = 0;
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
    uint32_t *frame = nullptr;             // the DMD's last frame, and with a margin around it
    uint32_t *framem = nullptr;
    int fw = 0, fh = 0;
    bool changed = false;          // the second screen's composition changed while open
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

lv_color_t hx(uint32_t c) { return lv_color_hex(c); }

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

// A picture of the table (RGB565, 1024 wide) or a part of it into r.
void put_img(Pic &p, const lv_image_dsc_t *d, dmd_rect_t src, dmd_rect_t r) {
    if (!d || d->header.cf != LV_COLOR_FORMAT_RGB565 || !d->data) return;
    const int stride = d->header.stride ? (int)d->header.stride / 2 : (int)d->header.w;
    dmd_blit565(p.px, p.w, p.h, r, (const uint16_t *)d->data, stride, src);
}

// The DMD's frame into r, with `m` rows and columns of unlit dots around it (the panel's margin).
void put_dmd(Pic &p, dmd_rect_t r, int m, bool x2) {
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
    (void)x2;   // the rebuilt rows: the same picture, taller (a thumbnail shows no difference)
    dmd_blit_frame(p.px, p.w, p.h, r, px, w, h);
}

dmd_rect_t scaled(dmd_rect_t r, int num, int den) {
    return dmd_rect_t{r.x * num / den, r.y * num / den, r.w * num / den, r.h * num / den};
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

// The second screen with composition `mode`, into p (its shape). As it came: the backglass alone.
void draw_s2(Pic &p, int mode) {
    Ui &u = *s_ui;
    memset(p.px, 0, (size_t)p.w * (size_t)p.h * 2);
    const dmd_s2_in_t in = s2_in();
    dmd_s2_out_t o;
    if (!dmd_s2_layout(mode == DMD_S2_KEEP ? DMD_S2_BG : mode, &in, &o)) return;
    const int num = p.w, den = u.s2w;
    if (u.b2s && mode != DMD_S2_ART) {
        const dmd_rect_t src{u.box.x, u.box.y, u.box.w, o.grill ? u.box.h : u.box.h - u.grill};
        put_img(p, u.b2s, src, scaled(o.bg, num, den));
    }
    if (o.art.w && u.art) put_img(p, u.art, dmd_rect_t{0, 0, kScrW, kScrH}, scaled(o.art, num, den));
    if (o.dmd.w && mode != DMD_S2_KEEP) put_dmd(p, scaled(o.dmd, num, den), 0, false);
}

// This panel with fit `fit` (-1: as every table) and the table's dots, into p (1024x600's shape).
void draw_panel(Pic &p, int fit, int dot) {
    Ui &u = *s_ui;
    memset(p.px, 0, (size_t)p.w * (size_t)p.h * 2);
    if (fit < 0) fit = dmd_app_fit_all();
    const int num = p.w, den = kScrW;
    const dmd_rect_t all{0, 0, kScrW, kScrH};
    if (fit == 2 || fit == 3) {   // the backbox / the backglass only: Popper's backglass
        if (u.bg) put_img(p, u.bg, dmd_rect_t{0, 0, (int)u.bg->header.w, (int)u.bg->header.h},
                          scaled(fit == 3 ? all : dmd_rect_t{0, 0, kScrW, kScrH - 256}, num, den));
        if (fit == 3) return;
    } else if (fit == 4 && u.art) {   // the art above the band
        put_img(p, u.art, dmd_rect_t{0, 0, kScrW, u.win.y > 0 ? u.win.y : 280}, scaled(dmd_rect_t{0, 0, kScrW, kScrH - 320}, num, den));
    } else if (fit == 7 && u.art) {
        put_img(p, u.art, all, scaled(all, num, den));
    } else if (fit == 8 && u.b2s) {
        put_img(p, u.b2s, all, scaled(all, num, den));
    }
    dmd_rect_t r;
    if (dmd_app_fit_rect(fit, dot, &r)) put_dmd(p, scaled(r, num, den), (dot >> 1) & 3, fit == 6);
}

void refresh(lv_obj_t *img) {
    if (!img) return;
    const void *src = lv_image_get_src(img);
    if (src) lv_image_cache_drop(src);
    lv_obj_invalidate(img);
}

int cur_s2(void) {
    char k[24];
    snprintf(k, sizeof k, "dmds2_%d", s_ui->gid);
    const int v = nv_config_get_int(k, 0) - 1;
    return v > 0 && v < DMD_S2_N ? v : DMD_S2_KEEP;
}

void mark(Card *cards, int n, int sel) {
    for (int i = 0; i < n; i++) {
        if (!cards[i].obj) continue;
        const bool on = cards[i].what == sel;
        lv_obj_set_style_border_color(cards[i].obj, hx(on ? kAccent : 0x26343C), 0);
        lv_obj_set_style_border_width(cards[i].obj, on ? 3 : 1, 0);
        lv_obj_set_style_bg_color(cards[i].obj, hx(on ? 0x0E3440 : kCard), 0);
    }
}

void pills_mark(lv_obj_t **p, int n, int sel) {
    for (int i = 0; i < n; i++) {
        if (!p[i]) continue;
        lv_obj_set_style_bg_color(p[i], hx(i == sel ? 0x0E5A6C : kCard), 0);
        lv_obj_set_style_border_color(p[i], hx(i == sel ? kAccent : 0x26343C), 0);
    }
}

void redraw_panel(void) {
    Ui &u = *s_ui;
    const int dot = dmd_app_table_dot(u.gid);
    for (Card &c : u.fit)
        if (c.obj && c.pic.px) {
            draw_panel(c.pic, c.what - 1, dot);
            refresh(c.img);
        }
    draw_panel(u.ppic, dmd_app_table_fit(u.gid), dot);
    refresh(u.big_p);
    mark(u.fit, kFits, dmd_app_table_fit(u.gid) + 1);
    pills_mark(u.dots, 2, dot & 1);
    pills_mark(u.marg, 4, (dot >> 1) & 3);
    pills_mark(u.rb, 2, dmd_app_table_rebuild(u.gid) ? 0 : 1);
}

void redraw_s2(void) {
    Ui &u = *s_ui;
    const int cur = cur_s2();
    if (u.s2pic.px) {
        draw_s2(u.s2pic, cur);
        refresh(u.big_s2);
    }
    mark(u.s2, DMD_S2_N, cur);
}

// ---- the second screen's compositions: kept per table, sent when the cable is the bridge's
// nv_config: "dmds2_<id>" the composition + 1, "dmds2j_<id>" the request for the bridge, "dmd_s2q" the
// tables whose request is still to be sent (ids, comma-separated)

void queue_edit(int gid, bool add) {
    char q[160], out[160] = "";
    nv_config_get_str("dmd_s2q", "", q, sizeof q);
    size_t o = 0;
    for (char *t = strtok(q, ","); t; t = strtok(nullptr, ",")) {
        const int id = atoi(t);
        if (id <= 0 || id == gid) continue;
        o += (size_t)snprintf(out + o, sizeof out - o, "%s%d", o ? "," : "", id);
        if (o >= sizeof out) return;
    }
    if (add) snprintf(out + o, sizeof out - o, "%s%d", o ? "," : "", gid);
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
        if (o.art.w) snprintf(art, sizeof art, "[%d,%d,%d,%d]", o.art.x, o.art.y, o.art.w, o.art.h);
        if (o.dmd.w) snprintf(dmd, sizeof dmd, "[%d,%d,%d,%d]", o.dmd.x, o.dmd.y, o.dmd.w, o.dmd.h);
        snprintf(j, sizeof j, "{\"q\":\"s2\",\"id\":%d,\"mode\":%d,\"bg\":[%d,%d,%d,%d],\"grill\":%s,\"art\":%s,\"dmd\":%s}",
                 u.gid, mode, o.bg.x, o.bg.y, o.bg.w, o.bg.h, o.grill ? "true" : "false", art, dmd);
    } else {
        snprintf(j, sizeof j, "{\"q\":\"s2\",\"id\":%d,\"mode\":0}", u.gid);
    }
    snprintf(k, sizeof k, "dmds2j_%d", u.gid);
    nv_config_set_str(k, j);
    queue_edit(u.gid, true);
    nv_log_write(NV_LOG_INFO, "dmd", "second screen of %d: %s", u.gid, j);
    u.changed = true;
    dmd_comp_flush();
    if (u.restart) lv_obj_remove_flag(u.restart, LV_OBJ_FLAG_HIDDEN);
    if (u.note) lv_label_set_text(u.note, nv_tr(NV_STR_DMDC_NEXT));
}

// ---- events

void close_cb(lv_event_t *) { dmd_comp_close(); }

void s2_cb(lv_event_t *e) {
    if (!s_ui) return;
    const int mode = (int)(intptr_t)lv_event_get_user_data(e);
    if (mode == cur_s2()) return;
    choose_s2(mode);
    redraw_s2();
}

void fit_cb(lv_event_t *e) {
    if (!s_ui) return;
    dmd_app_set_table_fit(s_ui->gid, (int)(intptr_t)lv_event_get_user_data(e) - 1);
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

lv_obj_t *label(lv_obj_t *parent, const lv_font_t *f, uint32_t c, const char *t, int x, int y) {
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, f, 0);
    lv_obj_set_style_text_color(l, hx(c), 0);
    lv_label_set_text(l, t);
    lv_obj_set_pos(l, x, y);
    return l;
}

lv_obj_t *box(lv_obj_t *parent) {
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    return o;
}

lv_obj_t *pill(lv_obj_t *parent, const char *t, int x, int y, int w, lv_event_cb_t cb, void *user) {
    lv_obj_t *b = box(parent);
    lv_obj_set_pos(b, x, y);
    lv_obj_set_size(b, w, 40);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(b, hx(kCard), 0);
    lv_obj_set_style_border_width(b, 2, 0);
    lv_obj_set_style_border_color(b, hx(0x26343C), 0);
    lv_obj_set_style_radius(b, 8, 0);
    lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, user);
    lv_obj_t *l = lv_label_create(b);
    lv_obj_set_style_text_font(l, &nv_font_20, 0);
    lv_obj_set_style_text_color(l, hx(kInk), 0);
    lv_label_set_text(l, t);
    lv_obj_update_layout(l);
    if (lv_obj_get_width(l) > w - 16) lv_obj_set_style_text_font(l, &nv_font_14, 0);   // a long name: smaller
    lv_obj_set_width(l, LV_SIZE_CONTENT);
    lv_obj_center(l);
    return b;
}

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

void gather(Ui &u) {
    u.b2s = dmd_menu_game_b2s(&u.box, &u.grill, &u.housing);
    u.art = dmd_menu_game_full(&u.win);
    if (u.art) dmd_menu_game_full_shape(&u.aw, &u.ah);
    u.bg = dmd_menu_game_art(kScrH - 256);
    u.s2ok = dmd_menu_s2(&u.s2w, &u.s2h);
    int w = 0, h = 0;
    dmd_app_dmd_size(&w, &h);
    if (w > 0 && h > 0) {
        u.frame = (uint32_t *)heap_caps_malloc((size_t)w * (size_t)h * 4, MALLOC_CAP_SPIRAM);
        u.framem = (uint32_t *)heap_caps_malloc((size_t)(w + 6) * (size_t)(h + 6) * 4, MALLOC_CAP_SPIRAM);
        if (u.frame && !dmd_app_snapshot(u.frame, (size_t)w * (size_t)h, &u.fw, &u.fh)) {
            heap_caps_free(u.frame);
            u.frame = nullptr;
        }
    }
}

}  // namespace

bool dmd_comp_open(lv_obj_t *root) {
    const int gid = dmd_menu_game_id();
    if (!root || gid <= 0) return false;
    if (s_ui) dmd_comp_close();
    s_ui = new (std::nothrow) Ui();
    if (!s_ui) return false;
    Ui &u = *s_ui;
    u.gid = gid;
    gather(u);
    u.scr = box(root);
    lv_obj_set_size(u.scr, kScrW, kScrH);
    lv_obj_set_style_bg_opa(u.scr, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(u.scr, hx(0x000000), 0);
    lv_obj_add_flag(u.scr, LV_OBJ_FLAG_CLICKABLE);   // nothing under it gets the taps
    lv_obj_add_event_cb(u.scr, scr_deleted_cb, LV_EVENT_DELETE, s_ui);
    lv_obj_move_foreground(u.scr);

    char title[120];
    snprintf(title, sizeof title, "%s  \xC2\xB7  %s", nv_tr(NV_STR_DMDC_TITLE), dmd_menu_game());
    lv_obj_t *t = label(u.scr, &nv_font_28, kAccent, title, 20, 10);
    lv_obj_set_width(t, 880);
    lv_label_set_long_mode(t, LV_LABEL_LONG_DOT);
    pill(u.scr, LV_SYMBOL_CLOSE, 948, 8, 60, close_cb, nullptr);

    // the two screens as chosen now
    label(u.scr, &nv_font_14, kDim, nv_tr(NV_STR_DMDC_S2), 20, 56);
    label(u.scr, &nv_font_14, kDim, nv_tr(NV_STR_DMDC_PANEL), 20 + kBigS2W + 16, 56);
    const int s2h = u.s2ok ? kBigS2W * u.s2h / u.s2w : kBigH;
    if (u.s2ok && pic_alloc(u.s2pic, kBigS2W, s2h > kBigH ? kBigH : s2h)) {
        u.big_s2 = lv_image_create(u.scr);
        lv_image_set_src(u.big_s2, &u.s2pic.dsc);
        lv_obj_set_pos(u.big_s2, 20, 76);
    } else {
        lv_obj_t *l = label(u.scr, &nv_font_14, kDim, nv_tr(NV_STR_DMDC_NO_S2), 20, 120);
        lv_obj_set_width(l, kBigS2W);
        lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
    }
    if (pic_alloc(u.ppic, kBigPW, kBigH)) {
        u.big_p = lv_image_create(u.scr);
        lv_image_set_src(u.big_p, &u.ppic.dsc);
        lv_obj_set_pos(u.big_p, 20 + kBigS2W + 16, 76);
    }

    // this panel's dots: original or fitted, a margin; a 128x16 DMD rebuilt or as it is
    const int cx = 528;
    label(u.scr, &nv_font_14, kDim, nv_tr(NV_STR_DMDC_DOTS), cx, 56);
    u.dots[0] = pill(u.scr, nv_tr(NV_STR_DMDC_DOTS_FIT), cx, 76, 160, dots_cb, (void *)(intptr_t)0);
    u.dots[1] = pill(u.scr, nv_tr(NV_STR_DMDC_DOTS_ORIG), cx + 168, 76, 160, dots_cb, (void *)(intptr_t)1);
    label(u.scr, &nv_font_14, kDim, nv_tr(NV_STR_DMDC_MARGIN), cx, 122);
    for (int i = 0; i < 4; i++) {
        char n[4];
        snprintf(n, sizeof n, "%d", i);
        u.marg[i] = pill(u.scr, n, cx + i * 70, 142, 62, dots_cb, (void *)(intptr_t)(2 + i));
    }
    if (dmd_app_dmd_half()) {
        u.rb[0] = pill(u.scr, nv_tr(NV_STR_DMDQ_RB_ON), cx, 192, 232, rb_cb, nullptr);
        u.rb[1] = pill(u.scr, nv_tr(NV_STR_DMDQ_RB_OFF), cx + 240, 192, 232, rb_cb, (void *)1);
    }
    // the second screen changed: from the next game, or now (the table quits and starts again)
    u.restart = pill(u.scr, nv_tr(NV_STR_DMDC_RESTART), cx + 288, 142, 188, restart_cb, nullptr);
    lv_obj_add_flag(u.restart, LV_OBJ_FLAG_HIDDEN);
    if (lv_obj_get_child_count(u.restart)) {
        lv_obj_t *l = lv_obj_get_child(u.restart, 0);
        lv_obj_set_style_text_font(l, &nv_font_14, 0);
    }
    u.note = label(u.scr, &nv_font_14, kDim, "", cx, 238);
    lv_obj_set_width(u.note, 476);
    lv_label_set_long_mode(u.note, LV_LABEL_LONG_DOT);

    // the second screen's compositions this table can have
    char s2names[400], *s2n[DMD_S2_N] = {};
    snprintf(s2names, sizeof s2names, "%s", nv_tr(NV_STR_DMDC_S2_NAMES));
    split(s2names, s2n, DMD_S2_N);
    label(u.scr, &nv_font_20, kInk, nv_tr(NV_STR_DMDC_S2), 20, 242);
    if (u.s2ok) {
        const int th = kS2TW * u.s2h / u.s2w;
        lv_obj_t *row = strip(u.scr, 270, th + 56);
        const dmd_s2_in_t in = s2_in();
        for (int m = 0; m < DMD_S2_N; m++)
            if (dmd_s2_avail(m, &in) && card(u.s2[m], row, kS2TW, th, s2n[m] ? s2n[m] : "", m, s2_cb)) draw_s2(u.s2[m].pic, m);
    } else {
        label(u.scr, &nv_font_14, kDim, nv_tr(NV_STR_DMDC_NO_S2), 20, 280);
    }

    // this panel's fits this table can have
    char fits[240], *fn[kFits] = {(char *)nv_tr(NV_STR_DMDQ_FIT_ALL)};
    snprintf(fits, sizeof fits, "%s", nv_tr(NV_STR_DMDQ_FIT_NAMES));
    split(fits, fn + 1, kFits - 1);
    const int prow = u.s2ok ? 270 + kS2TW * u.s2h / u.s2w + 56 : 312;
    label(u.scr, &nv_font_20, kInk, nv_tr(NV_STR_DMDC_PANEL), 20, prow);
    lv_obj_t *row = strip(u.scr, prow + 24, kPTH + 56);
    for (int i = 0; i < kFits; i++) {
        const int fit = i - 1;
        const bool ok = fit == 4 || fit == 7 ? u.art != nullptr
                        : fit == 8           ? u.b2s && u.housing.w > 0
                        : fit == 2 || fit == 3 ? u.bg != nullptr
                                               : true;
        if (ok) card(u.fit[i], row, kPTW, kPTH, fn[i] ? fn[i] : "", i, fit_cb);
    }
    redraw_s2();
    redraw_panel();
    return true;
}

namespace {
void ui_free(Ui *u) {
    for (Card &c : u->s2) pic_free(c.pic);
    for (Card &c : u->fit) pic_free(c.pic);
    pic_free(u->s2pic);
    pic_free(u->ppic);
    heap_caps_free(u->frame);
    heap_caps_free(u->framem);
    delete u;
}
}  // namespace

void dmd_comp_close(void) {
    if (!s_ui) return;
    Ui *u = s_ui;
    s_ui = nullptr;
    if (u->scr && lv_obj_is_valid(u->scr)) {   // its images go first: then their pictures
        lv_obj_remove_event_cb_with_user_data(u->scr, scr_deleted_cb, u);
        lv_obj_delete(u->scr);
    }
    ui_free(u);
}

bool dmd_comp_is_open(void) { return s_ui != nullptr; }

void dmd_comp_flush(void) {
    char q[160];
    nv_config_get_str("dmd_s2q", "", q, sizeof q);
    for (char *t = strtok(q, ","); t; t = strtok(nullptr, ",")) {
        const int id = atoi(t);
        if (id <= 0) continue;
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
