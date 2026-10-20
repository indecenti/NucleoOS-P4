// sim_dmd — the DMD app's Popper menu (components/nv_apps/dmd_menu.cpp, unchanged) on the PC: real LVGL,
// real NucleoOS fonts and strings, real dot renderer, real answers of the bridge (data/, made on the
// cabinet by tools/popper_bridge/sim_data.py). A touch and a clock are simulated; every scene is saved as a
// picture in out/ (RGB565 like the panel, converted to PNG by run.py). No board is touched.
//
//   make -C tests/host/sim_dmd && python tests/host/sim_dmd/run.py
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <algorithm>
#include <vector>

#include "cJSON.h"
#include "dmd_menu.h"
#include "dmd_panel.h"
#include "lvgl.h"
#include "nv_dmd.h"
#include "nv_gesture.h"
#include "nv_i18n.h"
#include "nv_notify.h"
#include "nv_ui.h"
#include "nv_ui_focus.h"

static const int W = 1024, H = 600;
static int64_t g_now_us = 1000000;
static std::string g_data = "data";
static std::string g_out = "out";

// ------------------------------------------------------------------ the stubs the menu uses
extern "C" int64_t esp_timer_get_time(void) { return g_now_us; }
static std::map<std::string, int> g_cfg;
extern "C" int nv_config_get_int(const char *k, int d) { auto it = g_cfg.find(k); return it == g_cfg.end() ? d : it->second; }
extern "C" void nv_config_set_int(const char *k, int v) { g_cfg[k] = v; }
static std::map<std::string, std::string> g_cfg_s;
// no JPEG decoder here (the fake bridge sends the RGB565 pictures sim_data.py made)
#include "gallery_jpeg_hw.h"
extern "C" bool gallery_jpeg_hw_decode_mem(const uint8_t *, size_t, gallery_raster_t *out) { *out = {}; return false; }
extern "C" void gallery_jpeg_hw_free(gallery_raster_t *r) { free(r->px); *r = {}; }
extern "C" void nv_config_get_str(const char *k, const char *d, char *out, size_t n) {
    auto it = g_cfg_s.find(k);
    snprintf(out, n, "%s", it == g_cfg_s.end() ? d : it->second.c_str());
}
extern "C" void nv_config_set_str(const char *k, const char *v) { g_cfg_s[k] = v; }
extern "C" void nv_event_publish(int, const void *) {}
extern "C" void nv_toast(nv_note_kind_t kind, const char *msg) { printf("  [toast %d] %s\n", (int)kind, msg); }
extern "C" void nv_focus_prefer(lv_obj_t *) {}
static std::vector<uint8_t> g_state;
extern "C" bool nv_ui_state_save(const void *d, size_t n) { g_state.assign((const uint8_t *)d, (const uint8_t *)d + n); return true; }
extern "C" size_t nv_ui_state_load(void *out, size_t cap) {
    if (g_state.empty() || g_state.size() > cap) return 0;
    memcpy(out, g_state.data(), g_state.size());
    return g_state.size();
}

// nv_gesture_bind as on the board: the gesture is consumed on obj, cb fires for that direction
struct Bind { lv_dir_t dir; nv_gesture_dir_cb_t cb; void *user; };
static void gesture_cb(lv_event_t *e) {
    Bind *b = (Bind *)lv_event_get_user_data(e);
    if (lv_indev_get_gesture_dir(lv_indev_active()) == b->dir) b->cb(b->user);
}
extern "C" void nv_gesture_bind(lv_obj_t *obj, lv_dir_t dir, nv_gesture_dir_cb_t cb, void *user) {
    lv_obj_add_flag(obj, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_add_event_cb(obj, gesture_cb, LV_EVENT_GESTURE, new Bind{dir, cb, user});
}
extern "C" void nv_gesture_isolate(lv_obj_t *plane) { lv_obj_clear_flag(plane, LV_OBJ_FLAG_GESTURE_BUBBLE); }

// ------------------------------------------------------------------ the bridge, answering from data/
struct Msg { uint8_t type; std::vector<uint8_t> data; };
static std::vector<Msg> g_q;
static bool g_bridge = true;
static int g_requests = 0, g_images = 0, g_same = 0;
static std::map<std::string, int> g_kinds;
static std::map<std::string, int> g_same_req;   // requests per picture: one asked again and again is a loop

static bool load(const std::string &name, Msg &m) {
    FILE *f = fopen((g_data + "/" + name).c_str(), "rb");
    if (!f) return false;
    std::vector<uint8_t> all;
    uint8_t buf[65536];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) all.insert(all.end(), buf, buf + n);
    fclose(f);
    if (all.empty()) return false;
    m.type = all[0];
    m.data.assign(all.begin() + 1, all.end());
    return true;
}
static void push_json(uint8_t type, const char *json) {
    Msg m{type, std::vector<uint8_t>(json, json + strlen(json))};
    g_q.push_back(m);
}
static void push_file(const std::string &name) {
    Msg m;
    if (load(name, m)) g_q.push_back(m);
}

extern "C" bool nv_dmd_bridge_send(uint8_t type, const char *json) {
    if (!g_bridge || type != 'Q') return false;
    g_requests++;
    cJSON *o = cJSON_Parse(json);
    if (!o) return true;
    const char *q = cJSON_GetStringValue(cJSON_GetObjectItem(o, "q"));
    {
        const char *kd = cJSON_GetStringValue(cJSON_GetObjectItem(o, "kind"));
        const bool hv = cJSON_GetObjectItem(o, "have") != nullptr;
        g_kinds[std::string(q) + (kd ? std::string(":") + kd : "") + (hv ? "+have" : "")]++;
    }
    const int id = cJSON_GetObjectItem(o, "id") ? cJSON_GetObjectItem(o, "id")->valueint : 0;
    char name[128];
    Msg m;
    if (!strcmp(q, "list")) {
        snprintf(name, sizeof name, "list_%s", cJSON_GetStringValue(cJSON_GetObjectItem(o, "list")));
        push_file(name);
    } else if (!strcmp(q, "table")) {
        snprintf(name, sizeof name, "table_%d", id);
        if (load(name, m)) g_q.push_back(m);
    } else if (!strcmp(q, "random")) {
        push_file("table_767");
    } else if (!strcmp(q, "image")) {
        const char *kind = cJSON_GetStringValue(cJSON_GetObjectItem(o, "kind"));
        const int w = cJSON_GetObjectItem(o, "w")->valueint;
        if (!strcmp(kind, "media")) snprintf(name, sizeof name, "img_media_%d_%s", id, cJSON_GetStringValue(cJSON_GetObjectItem(o, "screen")));
        else snprintf(name, sizeof name, "img_%s_%d_%d", kind, id, w);
        const char *have = cJSON_GetStringValue(cJSON_GetObjectItem(o, "have"));
        g_same_req[name]++;
        if (!load(name, m) && (!strcmp(kind, "fulldmd") || !strcmp(kind, "b2sgrill") || !strcmp(kind, "gamebg"))) {
            snprintf(name, sizeof name, "img_%s_%d", kind, id);   // recorded at a game's start: no width
            if (strcmp(kind, "gamebg")) load(name, m);
            else { snprintf(name, sizeof name, "img_gamebg_%d_512", id); load(name, m); }
        }
        if (!m.data.empty() || load(name, m)) {
            // the board's card copy: still the same bytes? (the key in the picture's JSON header)
            std::string hdr(m.data.begin() + 2, m.data.begin() + 2 + (m.data[0] | m.data[1] << 8));
            const size_t kp = hdr.find("\"k\": \"");
            const std::string key = kp == std::string::npos ? "" : hdr.substr(kp + 6, 8);
            if (have && key == have) {
                char k[160];
                snprintf(k, sizeof k, "{\"id\":%d,\"kind\":\"%s\",\"k\":\"%s\"}", id, kind, have);
                push_json('K', k);
                g_same++;
            } else {
                g_q.push_back(m);
                g_images++;
            }
        } else {
            char e[160];
            const char *scr = cJSON_GetStringValue(cJSON_GetObjectItem(o, "screen"));   // as the bridge: with the screen
            if (getenv("SIM_E_NOSCREEN")) scr = "";   // an incomplete answer: the board must not ask in a loop
            snprintf(e, sizeof e, "{\"q\":\"image\",\"id\":%d,\"kind\":\"%s\",\"screen\":\"%s\",\"err\":\"no image\"}", id, kind,
                     scr ? scr : "");
            push_json('E', e);
        }
    } else if (!strcmp(q, "b2sinfo")) {   // as the bridge read the cabinet's files (Dracula: a dual backglass)
        char r[300];
        snprintf(r, sizeof r, "{\"q\":\"b2sinfo\",\"id\":%d,\"b2s\":true,\"bw\":%d,\"bh\":%d,\"grill\":%d,\"aw\":1920,"
                 "\"ah\":1080,\"dual\":%s,\"dmdtype\":3,\"mode\":1,\"s2\":0,\"s2dmd\":false}", id,
                 id == 784 ? 2000 : 3000, id == 784 ? 1900 : 2915, id == 784 ? 330 : 820, id == 784 ? "true" : "false");
        push_json('R', r);
    } else if (!strcmp(q, "dual")) {
        char r[120];
        snprintf(r, sizeof r, "{\"q\":\"dual\",\"id\":%d,\"mode\":%d,\"ok\":true,\"err\":\"\"}", id,
                 cJSON_GetObjectItem(o, "mode") ? cJSON_GetObjectItem(o, "mode")->valueint : 1);
        push_json('R', r);
    } else if (!strcmp(q, "launch") || !strcmp(q, "exit")) {
        char r[96];
        snprintf(r, sizeof r, "{\"q\":\"%s\",\"id\":%d,\"ok\":true,\"err\":\"\"}", q, id);
        push_json('R', r);
    }
    cJSON_Delete(o);
    return true;
}
extern "C" bool nv_dmd_bridge_take(uint8_t *type, uint8_t **data, uint32_t *len) {
    if (g_q.empty()) return false;
    Msg m = g_q.front();
    g_q.erase(g_q.begin());
    *type = m.type;
    *len = (uint32_t)m.data.size();
    *data = (uint8_t *)malloc(m.data.size() + 1);
    memcpy(*data, m.data.data(), m.data.size());
    (*data)[m.data.size()] = 0;
    return true;
}
extern "C" void nv_dmd_bridge_free(uint8_t *data) { free(data); }
extern "C" bool nv_dmd_show(const uint32_t *, int, int, const char *) { return false; }   // no DMD in the simulator
extern "C" void nv_dmd_get_status(nv_dmd_status_t *st) {
    memset(st, 0, sizeof *st);
    st->bridge = g_bridge;
    st->fmt = "-";
}

// ------------------------------------------------------------------ display, touch, clock
static uint16_t g_fb[W * H];
static void flush_cb(lv_display_t *d, const lv_area_t *a, uint8_t *px) {
    const int w = a->x2 - a->x1 + 1;
    for (int y = a->y1; y <= a->y2; y++) memcpy(&g_fb[y * W + a->x1], px + (size_t)(y - a->y1) * w * 2, (size_t)w * 2);
    lv_display_flush_ready(d);
}
static lv_point_t g_pt;
static bool g_pressed;
static void touch_cb(lv_indev_t *, lv_indev_data_t *d) {
    d->point = g_pt;
    d->state = g_pressed ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
}
static uint32_t tick_cb(void) { return (uint32_t)(g_now_us / 1000); }

// time passes: the menu's 10 Hz tick and LVGL, in 10 ms steps
static void run(int ms) {
    for (int t = 0; t < ms; t += 10) {
        g_now_us += 10000;
        if ((g_now_us / 10000) % 10 == 0) dmd_menu_tick(true);
        lv_timer_handler();
    }
}
static void tap(int x, int y) {
    g_pt = {x, y};
    g_pressed = true;
    run(60);
    g_pressed = false;
    run(200);
}
static void drag(int x0, int y0, int x1, int y1, int ms) {
    g_pt = {x0, y0};
    g_pressed = true;
    run(30);
    const int steps = ms / 10;
    for (int i = 1; i <= steps; i++) {
        g_pt = {x0 + (x1 - x0) * i / steps, y0 + (y1 - y0) * i / steps};
        run(10);
    }
    g_pressed = false;
    run(400);
}
static void shot(const char *name) {
    lv_obj_invalidate(lv_screen_active());
    lv_refr_now(nullptr);
    std::string p = g_out + "/" + name + ".rgb565";
    FILE *f = fopen(p.c_str(), "wb");
    fwrite(g_fb, 2, W * H, f);
    fclose(f);
    printf("shot %s\n", name);
}
// the app's per-table fit (dmd_app.cpp, not built here)
static std::map<int, int> g_table_fit;
int dmd_app_table_fit(int game_id) { auto it = g_table_fit.find(game_id); return it == g_table_fit.end() ? -1 : it->second; }
void dmd_app_set_table_fit(int game_id, int fit) { g_table_fit[game_id] = fit; printf("  [fit] table %d -> %d\n", game_id, fit); }
static bool g_half = false;   // a 128x16 DMD (the composition scenes)
bool dmd_app_dmd_half(void) { return g_half; }
bool dmd_app_table_rebuild(int) { return true; }
void dmd_app_set_table_rebuild(int, bool) {}
int dmd_app_fit_suggested(bool has_art, bool has_grill) { return has_art ? 7 : has_grill ? 8 : 1; }
static void settings_cb(void) { printf("  [settings]\n"); }
// the app's dots, its DMD and its geometry (dmd_app.cpp, not built here: the same rules as its geom_for)
static int g_dot[1000];
int dmd_app_table_dot(int g) { return g > 0 && g < 1000 ? g_dot[g] : 0; }
void dmd_app_set_table_dot(int g, int d) { if (g > 0 && g < 1000) g_dot[g] = d & 7; printf("  [dot] table %d -> %d\n", g, d); }
void dmd_app_dmd_size(int *w, int *h) { *w = 128; *h = 32; }
bool dmd_app_snapshot(uint32_t *dst, size_t cap, int *w, int *h) {
    if (cap < 128 * 32) return false;
    dmd_text(dst, 128, 32, "PUSH START", 0xFF5820);
    *w = 128;
    *h = 32;
    return true;
}
static int g_fit_all = 1;
int dmd_app_fit_all(void) { return g_fit_all; }
void dmd_app_set_fit_all(int fit) { g_fit_all = fit; printf("  [fit] every table -> %d\n", fit); }
void dmd_app_look(int *shape, bool *glow, int *bright, int *rot) { *shape = 0; *glow = false; *bright = 100; *rot = 0; }
bool dmd_app_fit_geom(int fit, int dot, const dmd_rect_t *wp, dmd_geom_t *g) {
    dmd_rect_t win{};
    if (fit == 9) {   // automatic: from the running game's pictures (as the app)
        dmd_rect_t w{};
        fit = dmd_app_fit_suggested(dmd_menu_game_full(&w) != nullptr, dmd_menu_game_grill(&w) != nullptr);
    }
    if (wp) win = *wp;
    else if (fit == 7 && !dmd_menu_game_full(&win)) return false;
    else if (fit == 8 && !dmd_menu_game_grill(&win)) return false;
    if ((fit == 7 || fit == 8) && (win.w <= 0 || win.h <= 0)) return false;
    if (fit == 3) return false;
    const int m = (dot >> 1) & 3, fill = dot & 1 ? DMD_FIT_SQUARE : DMD_FIT_FILL;
    const int gw = 128 + 2 * m, gh = (fit == 6 ? 64 : 32) + 2 * m;
    bool ok;
    if (fit == 7 || fit == 8) {
        const dmd_rect_t in = dmd_inset(win);
        ok = dmd_geom(gw, gh, 0, fill, in.w, in.h, g);
        g->ox += in.x;
        g->oy += in.y;
    } else if (fit == 6) {
        ok = dmd_geom(gw, gh, 0, fill, 1024, 600, g);
    } else if (fit == 4 || fit == 5) {
        ok = dmd_geom(gw, gh, 0, fill, 1024, 320, g);
        g->oy = fit == 5 ? (600 - g->gh) / 2 : 600 - g->gh;
    } else {
        ok = dmd_geom(gw, gh, 0, fit == 2 || fit == 1 ? DMD_FIT_SQUARE : fill, 1024, 600, g);
        if (fit == 2) g->oy = 600 - g->gh;
    }
    return ok;
}
// the device's fallbacks (dmd_app.cpp fit_shown, rotation 0): a missing picture, a DMD too tall to rebuild
int dmd_app_fit_shown(int fit, bool full, bool art, int w, int h) {
    if (fit == 4 || fit == 7 || fit == 8) return full ? fit : art ? 2 : 0;
    if (fit == 2 || fit == 3) return art ? fit : 1;
    if (fit == 6) return 2 * h <= DMD_MAX_H && w * 2 * h <= DMD_MAX_PX ? 6 : 1;
    return fit == 9 ? 1 : fit;
}
bool dmd_app_fit_geom_wh(int fit, int dot, const dmd_rect_t *win, int, int, dmd_geom_t *g) {
    dmd_rect_t w{};
    if (win) w = *win;
    if ((fit == 7 || fit == 8) && (w.w <= 0 || w.h <= 0)) return false;
    return dmd_app_fit_geom(fit, dot, &w, g);
}
int dmd_app_art_h(int, int, int) { return 600 - 256; }
void dmd_app_rebuild_rows(int, int h, int *r0, int *n) { *r0 = 0; *n = h; }
bool dmd_app_fit_rect(int fit, int dot, const dmd_rect_t *win, dmd_rect_t *out) {
    static dmd_geom_t g;
    const bool ok = dmd_app_fit_geom(fit, dot, win, &g);
    if (ok) *out = dmd_rect_t{g.ox, g.oy, g.gw, g.gh};
    return ok;
}

// ------------------------------------------------------------------ the composition screen, real pictures
static void comp_scenes(void) {
    g_half = true;
    lv_obj_t *root = lv_obj_create(lv_screen_active());
    lv_obj_remove_style_all(root);
    lv_obj_set_size(root, W, H);
    lv_obj_set_style_bg_color(root, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);
    g_q.clear();
    g_bridge = true;
    dmd_menu_set_theme(0);
    dmd_menu_build(root, settings_cb);
    push_file("hello");
    push_json('H', "{\"bridge\":\"1.1\",\"popper\":true,\"tables\":817,\"game\":null,\"ip\":\"192.168.1.60\",\"web\":80,\"s2\":[1366,0,1280,1024]}");
    run(4000);
    const int ids[2] = {811, 784};
    for (int id : ids) {
        char j[160], n[64];
        snprintf(j, sizeof j, "{\"game\":\"start\",\"id\":%d,\"name\":\"%s\",\"dmd\":true}", id, id == 811 ? "Teenage Mutant Ninja Turtles" : "Bram Stoker's Dracula");
        push_json('G', j);
        snprintf(n, sizeof n, "img_fulldmd_%d", id);
        push_file(n);
        snprintf(n, sizeof n, "img_b2sgrill_%d", id);
        push_file(n);
        snprintf(n, sizeof n, "img_gamebg_%d_512", id);
        push_file(n);
        run(2500);
        dmd_menu_quick_open();
        run(400);
        tap(512, 428);
        run(800);
        snprintf(n, sizeof n, "comp_%d_a", id);
        shot(n);
        // (the quick action opens on this screen's page)
        tap(150, 72);                      // the monitor's page
        run(400);
        snprintf(n, sizeof n, "comp_%d_a2_monitor", id);
        shot(n);
        tap(id == 811 ? 700 : 560, 138 + 238 + 60);   // a second-screen composition
        run(500);
        tap(400, 72);                      // this screen again
        run(300);
        tap(316 + 158 + 75, 138 + 136);    // original dots
        run(400);
        tap(316 + 340 + 2 * 62 + 27, 138 + 136);   // a margin of 2
        run(500);
        snprintf(n, sizeof n, "comp_%d_b", id);
        shot(n);
        dmd_menu_back();
        run(300);
        push_json('G', "{\"game\":\"end\"}");
        run(1500);
    }
}
// ------------------------------------------------------------------ the settings panel and the layout for every table
static const int32_t kSimColors[7] = {-1, 0xFF4500, 0xFF0000, 0xFFC000, 0x20FF40, 0x00C8FF, 0xFFFFFF};   // as dmd_app.cpp
static void port_field(lv_obj_t *row) {   // the app puts the system's number field here
    lv_obj_t *l = lv_label_create(row);
    lv_obj_set_style_text_font(l, &nv_font_20, 0);
    lv_obj_set_style_text_color(l, lv_color_white(), 0);
    lv_label_set_text(l, "9090");
    lv_obj_center(l);
    lv_obj_set_style_border_width(row, 2, 0);
    lv_obj_set_style_border_color(row, lv_color_hex(0x26343C), 0);
    lv_obj_set_style_radius(row, 8, 0);
}
static lv_obj_t *g_panel_root;
static void panel_layout_cb(void) {   // as the app: the composition for every table, the card then updated
    dmd_comp_open_all(g_panel_root, [] { dmd_panel_layout(dmd_app_fit_all(), -1); });
}
static void panel_scenes(void) {
    lv_obj_t *root = lv_obj_create(lv_screen_active());
    lv_obj_remove_style_all(root);
    lv_obj_set_size(root, W, H);
    g_panel_root = root;
    static DmdPanelIo io;
    io.theme = 1;
    io.color = 1;
    io.bright = 100;
    io.colors = kSimColors;
    io.ncolors = 7;
    io.boot = true;
    io.layout = panel_layout_cb;
    io.make_port = port_field;
    lv_obj_t *resume = nullptr;
    lv_obj_t *panel = dmd_panel_build(root, &io, &resume);
    lv_obj_remove_flag(panel, LV_OBJ_FLAG_HIDDEN);
    dmd_panel_status("USB  \xC2\xB7  ZeDMD 128x32  \xC2\xB7  59.8 fotogrammi/s  \xC2\xB7  Attack from Mars", "Wi-Fi:  ws://192.168.1.50:9090/dmd");
    dmd_panel_layout(dmd_app_fit_all(), -1);
    run(300);
    shot("panel_01");
    lv_obj_scroll_to_y(panel, 200, LV_ANIM_OFF);
    run(100);
    shot("panel_02_scrolled");
    lv_obj_scroll_to_y(panel, 0, LV_ANIM_OFF);
    run(100);
    panel_layout_cb();   // "choose with previews"
    run(400);
    shot("layout_01_all");
    tap(20 + 0 * 198 + 90, 276 + 60);   // the first card: the full-screen DMD
    run(300);
    shot("layout_02_fullscreen");
    tap(20 + 3 * 198 + 90, 276 + 60);   // the stretched DMD
    run(300);
    shot("layout_03_stretched");
    tap(20 + 1 * 198 + 90, 276 + 60);   // the classic DMD
    run(300);
    shot("layout_04_classic");
    dmd_comp_close();
    run(300);
    shot("panel_03_after");
    lv_obj_t *first = nullptr;   // the question before leaving the app (dmd_app.cpp leave_guard)
    lv_obj_t *ask = dmd_panel_ask(root, nv_tr(NV_STR_DMDX_LEAVE_TITLE), nv_tr(NV_STR_DMDX_LEAVE_GAME), nv_tr(NV_STR_DMDX_STAY),
                                  nv_tr(NV_STR_DMDX_LEAVE_OK), nullptr, nullptr, &first);
    run(300);
    shot("panel_04_leave");
    lv_obj_delete(ask);
}

// ------------------------------------------------------------------ a table's screens from the menu (no game)
static void screens_scenes(void) {
    g_half = false;
    lv_obj_t *root = lv_obj_create(lv_screen_active());
    lv_obj_remove_style_all(root);
    lv_obj_set_size(root, W, H);
    lv_obj_set_style_bg_color(root, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);
    g_q.clear();
    g_bridge = true;
    dmd_menu_set_theme(0);
    dmd_menu_build(root, settings_cb);
    push_file("hello");
    push_json('H', "{\"bridge\":\"1.1\",\"popper\":true,\"tables\":817,\"game\":null,\"ip\":\"192.168.1.60\",\"web\":80,\"s2\":[1366,0,1280,1024]}");
    run(4000);
    shot("screens_00_card");
    const int ids[2] = {811, 784};
    for (int id : ids) {
        char n[64];
        dmd_comp_open_table(root, id, id == 811 ? "Teenage Mutant Ninja Turtles" : "Bram Stoker's Dracula");
        run(3000);   // its pictures: from the bridge, the screen rebuilt as they come
        snprintf(n, sizeof n, "screens_%d_a_monitor", id);
        shot(n);
        tap(956, 242);   // a DMD on the monitor too: yes
        run(400);
        snprintf(n, sizeof n, "screens_%d_b_monitor_dmd", id);
        shot(n);
        tap(852, 242);   // and back: no
        run(300);
        tap(340, 72);         // this screen's page
        run(400);
        snprintf(n, sizeof n, "screens_%d_c_panel", id);
        shot(n);
        dmd_comp_close();
        run(300);
    }
}

// where to tap in the "go to" card (from its shot; overridable: FIND=x,y LETTER=x,y ALL=x,y)
static int g_find_x = 600, g_find_y = 200, g_letter_x = 420, g_letter_y = 330, g_all_x = 300, g_all_y = 200;
static void xy(const char *env, int *x, int *y) { const char *v = getenv(env); if (v) sscanf(v, "%d,%d", x, y); }

// ------------------------------------------------------------------ scenes
static void scenes(int theme, const char *tag) {
    char n[64];
    lv_obj_t *root = lv_obj_create(lv_screen_active());
    lv_obj_remove_style_all(root);
    lv_obj_set_size(root, W, H);
    lv_obj_set_style_bg_color(root, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);
    g_q.clear();
    g_bridge = true;
    dmd_menu_set_theme(theme);
    dmd_menu_build(root, settings_cb);
    push_file("hello");
    run(6000);   // list, cards, wheels, backglass
#define SHOT(x) (snprintf(n, sizeof n, "%s_%s", tag, x), shot(n))
    SHOT("01_menu");
    drag(700, 520, 300, 520, 250);   // drag the bar by ~3 tables
    run(3000);
    SHOT("02_bar_dragged");
    drag(900, 520, 300, 520, 60);    // a flick: far along the list
    run(3000);
    SHOT("02b_flick");
    drag(800, 250, 300, 250, 150);   // swipe the stage: the next one
    run(2500);
    SHOT("03_swiped");
    tap(560, 32);                    // "All"
    run(4000);
    SHOT("04_all");
    tap(885, 32);                    // "go to": systems and letters
    run(600);
    SHOT("04b_find");
    tap(g_find_x, g_find_y);         // a system (MAME)
    run(3000);
    SHOT("04c_filtered");
    tap(885, 32);
    run(600);
    SHOT("04d_find_filtered");
    tap(g_letter_x, g_letter_y);     // a letter
    run(3000);
    SHOT("04e_letter");
    tap(885, 32);
    run(600);
    tap(g_all_x, g_all_y);           // all systems again
    run(3000);
    tap(140, 32);                    // back to "Recent"
    run(3000);
    // instructions of the selected table, if it has them (the help button)
    for (int k = 0; k < 6; k++) { drag(700, 520, 552, 520, 120); run(1500); }
    SHOT("05_card");
    tap(937, 32);                    // "surprise me": Attack from Mars here, it has instructions
    run(2500);
    tap(635, 338);                   // "Instructions"
    run(1500);
    SHOT("05b_help");
    drag(512, 520, 512, 120, 300);   // scroll the page
    run(800);
    SHOT("05c_help_scrolled");
    tap(512, 300);                   // a tap closes it
    run(500);
    push_file("show_767");           // the player moved in Popper: the showcase
    run(4000);
    SHOT("06_showcase");
    run(8000);
    SHOT("07_showcase_next");
    tap(500, 300);                   // a touch: the menu on that table
    run(2500);
    SHOT("08_after_showcase");
    // Popper's selection on entries that are not tables, and on a table without horizontal pictures
    push_json('S', "{\"id\":0,\"nav\":\"back\",\"name\":\"Go Back\"}");
    run(800);
    SHOT("08b_nav_back");
    push_json('S', "{\"id\":0,\"nav\":\"exit\",\"name\":\"Exit\"}");
    run(800);
    SHOT("08c_nav_exit");
    push_json('S', "{\"id\":0,\"nav\":\"playlist\",\"name\":\"Visual Pinball X\"}");
    run(800);
    SHOT("08d_nav_playlist");
    push_json('S', "{\"id\":767,\"name\":\"Attack from Mars (Bally 1995)\",\"media\":[]}");
    run(1500);
    SHOT("08e_nav_table");
    push_json('S', "{\"idle\":true}");   // Popper in its attract mode again: the menu
    run(800);
    SHOT("08f_idle_menu");
    // a VPX game starts: the "in game" page (no DMD frames here)
    push_json('G', "{\"game\":\"start\",\"id\":767,\"name\":\"Attack from Mars\",\"dmd\":true}");
    run(1500);
    SHOT("09_ingame_vpx");
    dmd_menu_quick_open();
    run(500);
    SHOT("10_quick");
    tap(512, 428);                   // "Fit for this table": the per-table choice
    run(600);
    SHOT("10b_fit");
    dmd_menu_back();                 // closed again, the quick actions back for the next steps
    dmd_menu_quick_open();
    run(500);
    tap(352, 330);                   // the tables, during the game
    run(2000);
    SHOT("11_browse_ingame");
    tap(660, 520);                   // another table, then Play: "close the game and start this one?"
    run(1500);
    tap(448, 345);
    run(800);
    SHOT("11b_confirm");
    tap(512, 560);                   // outside the card: cancelled
    run(500);
    push_json('G', "{\"game\":\"end\"}");
    run(2000);
    // an FX2 game: the bridge keeps the port, the game's showcase
    {
        Msg m;
        load("show_676", m);
        std::string sc(m.data.begin(), m.data.end());
        std::string g = "{\"game\":\"start\",\"id\":676,\"name\":\"Star Wars: Return of the Jedi\",\"dmd\":false,\"showcase\":" + sc + "}";
        push_json('G', g.c_str());
    }
    run(4000);
    SHOT("12_ingame_fx2");
    dmd_menu_quick_open();
    run(500);
    SHOT("13_quick_fx2");
    push_json('G', "{\"game\":\"end\"}");
    run(1500);
    tap(470, 32);                    // favourites (empty on the cabinet)
    run(2500);
    SHOT("14_fav");
    tap(140, 32);
    run(2500);
    run(125000);                     // two minutes: the attract mode
    SHOT("15_attract");
    lv_obj_delete(root);
    dmd_menu_destroy();
}

// The selection under a finger while lists come and go (a game ended, the stats changed): a tap on a
// wheel must land on the wheel that was under the finger.
static std::string reordered(const char *file, int from) {   // the list with item `from` moved first
    Msg m;
    load(file, m);
    std::string j(m.data.begin(), m.data.end());
    cJSON *o = cJSON_Parse(j.c_str());
    cJSON *items = cJSON_GetObjectItem(o, "items");
    cJSON *it = cJSON_DetachItemFromArray(items, from);
    cJSON_InsertItemInArray(items, 0, it);
    char *t = cJSON_PrintUnformatted(o);
    std::string r(t);
    free(t);
    cJSON_Delete(o);
    return r;
}
static void sel_scenes(void) {
    lv_obj_t *root = lv_obj_create(lv_screen_active());
    lv_obj_remove_style_all(root);
    lv_obj_set_size(root, W, H);
    g_q.clear();
    g_bridge = true;
    dmd_menu_set_theme(1);
    dmd_menu_build(root, settings_cb);
    push_file("hello");
    run(6000);
    printf("-- a: tap +1 at rest\n");
    tap(660, 520);
    run(1000);
    printf("-- b: tap +1, a reordered list arrives during the slide\n");
    g_pt = {660, 520};
    g_pressed = true;
    run(60);
    g_pressed = false;
    run(60);
    push_json('L', reordered("list_recent", 2).c_str());   // the touched table moves first
    run(1500);
    printf("-- c: a reordered list arrives, then a tap -3\n");
    push_json('L', reordered("list_recent", 7).c_str());
    run(300);
    tap(66, 520);
    run(1500);
    printf("-- d: a game starts and ends, the list again (reordered), a tap +1 right after\n");
    push_json('G', "{\"game\":\"start\",\"id\":767,\"name\":\"Attack from Mars\",\"dmd\":true}");
    run(2000);
    push_json('G', "{\"game\":\"end\"}");
    run(100);
    tap(660, 520);
    run(1500);
    printf("-- e: attract mode running, a tap -3\n");
    run(130000);
    run(7900);
    tap(66, 520);
    run(1500);
    shot("sel_end");
}

int main(int argc, char **argv) {
    if (argc > 1) g_data = argv[1];
    if (argc > 2) g_out = argv[2];
    lv_init();
    lv_tick_set_cb(tick_cb);
    lv_display_t *d = lv_display_create(W, H);
    static uint16_t buf[W * H];
    lv_display_set_buffers(d, buf, nullptr, sizeof buf, LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(d, flush_cb);
    lv_indev_t *in = lv_indev_create();
    lv_indev_set_type(in, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(in, touch_cb);
    nv_config_set_int("lang", NV_LANG_IT);
    nv_i18n_init();
    lv_obj_set_style_bg_color(lv_screen_active(), lv_color_black(), 0);
    xy("FIND", &g_find_x, &g_find_y);
    xy("LETTER", &g_letter_x, &g_letter_y);
    xy("ALL", &g_all_x, &g_all_y);
    const char *only = argc > 3 ? argv[3] : "";
    if (!strcmp(only, "sel")) { sel_scenes(); return 0; }
    if (!strcmp(only, "comp")) { comp_scenes(); return 0; }
    if (!strcmp(only, "panel")) { panel_scenes(); return 0; }
    if (!strcmp(only, "screens")) { screens_scenes(); return 0; }
    if (!*only || !strcmp(only, "cab")) scenes(0, "cab");
    if (!*only || !strcmp(only, "dmd")) scenes(1, "dmd");
    printf("bridge requests: %d, pictures sent: %d, card copies confirmed: %d\n", g_requests, g_images, g_same);
    for (auto &kv : g_kinds) printf("  %-22s %d\n", kv.first.c_str(), kv.second);
    {
        std::vector<std::pair<int, std::string>> v;
        for (auto &kv : g_same_req) v.push_back({kv.second, kv.first});
        std::sort(v.rbegin(), v.rend());
        printf("pictures asked: %zu different; most asked:", v.size());
        for (size_t i = 0; i < v.size() && i < 6; i++) printf(" %s x%d", v[i].second.c_str(), v[i].first);
        printf("\n");
    }
    return 0;
}
