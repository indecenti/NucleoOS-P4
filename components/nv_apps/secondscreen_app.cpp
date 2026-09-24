// secondscreen_app — "Second Screen": the board as a display for a computer or a phone.
//
// The app is a connection WIZARD on top of the nv_secondscreen engine (components/
// nv_secondscreen): pick the device (Windows / Mac / Linux / Android / other) -> the methods for
// it, best first -> a step list whose steps tick themselves from live checks (USB mode, cable,
// frames, network, sender connected, VNC state). When a step can't complete, the wizard offers
// the next method ("cascade"). While the page is open the engine listens on every passive
// transport at once (USB + NucleoCast + VNC reverse), so a device that starts streaming simply
// takes over the panel: the wizard is guidance, never a gate.
//
// Overlays: approval of a new network sender, the PAUSED card (after the left-edge swipe), the
// Options sheet (auto-open, ask-before-connect, USB personality).
#include "apps_internal.h"
#include "secondscreen_strings.h"

#include "nv_app.h"
#include "nv_icons.h"
#include "nv_i18n.h"
#include "nv_fonts.h"
#include "nv_theme.h"
#include "nv_config.h"
#include "nv_ui.h"
#include "nv_ui_kit.h"
#include "nv_notify.h"
#include "nv_log.h"
#include "nv_ss.h"
#include "nv_ss_links.h"
#include "nv_event_bus.h"
#include "ss_icons.h"

#include "lvgl.h"
#include "esp_system.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

void ss_show_options(void);

namespace {

constexpr const char *TAG = "secondscreen";

// ------------------------------------------------------------------ model
enum Os { OS_WIN, OS_MAC, OS_LINUX, OS_ANDROID, OS_OTHER, OS_N };
enum Method { M_USB, M_CAST, M_MACVNC, M_VNC, M_ANDROID };

struct OsDef {
    SsStr name, best;
    const lv_image_dsc_t *icon;
    Method methods[3];
    int n;
};
const OsDef kOs[OS_N] = {
    {S_OS_WIN, S_BEST_WIN, &ss_ic_win, {M_USB, M_CAST, M_VNC}, 3},
    {S_OS_MAC, S_BEST_MAC, &ss_ic_apple, {M_MACVNC, M_CAST, M_CAST}, 2},
    {S_OS_LINUX, S_BEST_LINUX, &ss_ic_linux, {M_CAST, M_VNC, M_VNC}, 2},
    {S_OS_ANDROID, S_BEST_ANDROID, &ss_ic_android, {M_ANDROID, M_ANDROID, M_ANDROID}, 1},
    {S_OS_OTHER, S_BEST_OTHER, &ss_ic_web, {M_CAST, M_CAST, M_CAST}, 1},
};

struct MethodDef {
    SsStr title, desc;
    const lv_image_dsc_t *icon;
    SsStr badges[3];
    int nb;
};
const MethodDef kMethod[] = {
    /* M_USB     */ {S_M_USB, S_M_USB_D, &ss_ic_usb, {S_B_EXT, S_B_TOUCH, S_B_ONCE}, 3},
    /* M_CAST    */ {S_M_CAST, S_M_CAST_D, &ss_ic_cast, {S_B_MIRROR, S_B_NOINST, S_B_NOINST}, 2},
    /* M_MACVNC  */ {S_M_MACVNC, S_M_MACVNC_D, &ss_ic_vnc, {S_B_MIRROR, S_B_TOUCH, S_B_NOINST}, 3},
    /* M_VNC     */ {S_M_VNC, S_M_VNC_D, &ss_ic_vnc, {S_B_MIRROR, S_B_TOUCH, S_B_ONCE}, 3},
    /* M_ANDROID */ {S_M_ANDROID, S_M_ANDROID_D, &ss_ic_android, {S_B_MIRROR, S_B_TOUCH, S_B_ONCE}, 3},
};

enum StepSt { ST_TODO, ST_OK, ST_FAIL };
enum StepKind {
    K_USB_MODE, K_USB_CABLE, K_USB_DRIVER, K_USB_EXTEND,
    K_NET, K_CAST_PAGE, K_CAST_SHARE,
    K_MAC_SHARING, K_VNC_SERVER, K_DROID_APP, K_VNC_CONNECT,
};
struct Step { StepKind kind; SsStr title; StepSt st; };

// Live snapshot of every transport, refreshed on each UI tick.
struct Live {
    nv_ss_status_t ss;
    nv_ss_usb_info_t usb;
    nv_ss_cast_info_t cast;
    nv_ss_vnc_info_t vnc;
};
Live L;

// ------------------------------------------------------------------ UI state (LVGL thread)
enum Page { P_HOME, P_METHODS, P_STEPS };
lv_obj_t *s_root = nullptr;       // app content
lv_obj_t *s_page = nullptr;       // current page container (rebuilt on navigation)
lv_obj_t *s_overlay = nullptr;    // approval / paused / options sheet (one at a time)
int s_overlay_kind = 0;           // 0 none, 1 approval, 2 paused, 3 options
lv_timer_t *s_tick = nullptr;
Page s_pg = P_HOME;
Os s_os = OS_WIN;
Method s_method = M_USB;
int s_focus = -1;                 // step focused by the user (-1 = follow the first open step)
int s_shown_step = -2;            // step whose detail panel is built
uint32_t s_shown_sig = 0;         // detail panel signature (rebuild when it changes)
uint32_t s_last_gen = 0;
bool s_toast_armed = false;
uint32_t s_usb_idle_since = 0;    // tick when "mounted, no frames" started (USB driver check)
uint32_t s_ack = 0;               // manual steps the user confirmed ("Done, next"), bit = step index
struct PendingRecent { bool on; char host[48]; uint16_t port; char name[48]; char user[64]; char pw[64]; };
PendingRecent s_pend = {};        // connection typed in the form, saved to Recents once it works

// widgets refreshed in place
lv_obj_t *s_home_net = nullptr, *s_home_usb = nullptr, *s_home_dot = nullptr, *s_home_quick = nullptr;
lv_obj_t *s_sug = nullptr, *s_sug_lbl = nullptr, *s_sug_btn = nullptr, *s_sug_btn_lbl = nullptr;
int s_sug_kind = -1;              // which suggestion the banner shows (-1 none)
lv_obj_t *s_meth_status[3] = {};
lv_obj_t *s_step_icon[5] = {}, *s_step_lbl[5] = {};
lv_obj_t *s_detail = nullptr;
lv_obj_t *s_detail_live = nullptr;    // dynamic status line inside the detail panel
lv_obj_t *s_next_btn = nullptr, *s_next_lbl = nullptr;
lv_obj_t *s_vnc_list = nullptr;       // discovered servers container
int s_vnc_list_n = -1;
lv_obj_t *s_ta_host = nullptr, *s_ta_pass = nullptr, *s_ta_user = nullptr;
lv_obj_t *s_pause_stats = nullptr;
lv_obj_t *s_focused_ta = nullptr;   // form field with the on-screen keyboard
int32_t s_kb_h = 0;                 // docked keyboard height while visible

const NvTheme *th(void) { return nv_theme_get(); }

// ------------------------------------------------------------------ small widget helpers
// Plain layout container. NOT clickable: LVGL v9 hit-tests clickable objects only, so a clickable
// inner row/column would swallow the tap meant for its card (pressable() opts a box back in).
lv_obj_t *box(lv_obj_t *parent) {
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_CLICKABLE);
    return o;
}

lv_obj_t *col(lv_obj_t *parent, int gap) {
    lv_obj_t *o = box(parent);
    lv_obj_set_flex_flow(o, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(o, gap, 0);
    return o;
}

lv_obj_t *row(lv_obj_t *parent, int gap) {
    lv_obj_t *o = box(parent);
    lv_obj_set_flex_flow(o, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(o, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(o, gap, 0);
    return o;
}

lv_obj_t *label(lv_obj_t *parent, const char *txt, const lv_font_t *f, lv_color_t c) {
    lv_obj_t *l = lv_label_create(parent);
    lv_label_set_text(l, txt);
    lv_obj_set_style_text_font(l, f, 0);
    lv_obj_set_style_text_color(l, c, 0);
    return l;
}

lv_obj_t *wrap_label(lv_obj_t *parent, const char *txt, const lv_font_t *f, lv_color_t c, int32_t w) {
    lv_obj_t *l = label(parent, txt, f, c);
    lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(l, w);
    return l;
}

lv_obj_t *icon(lv_obj_t *parent, const lv_image_dsc_t *img, lv_color_t c) {
    lv_obj_t *i = lv_image_create(parent);
    lv_image_set_src(i, img);
    lv_obj_set_style_image_recolor(i, c, 0);
    lv_obj_set_style_image_recolor_opa(i, LV_OPA_COVER, 0);
    return i;
}

lv_obj_t *card(lv_obj_t *parent) {
    lv_obj_t *c = box(parent);
    lv_obj_set_style_bg_color(c, th()->surface, 0);
    lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(c, NV_RAD_MD, 0);
    lv_obj_set_style_border_color(c, th()->divider, 0);
    lv_obj_set_style_border_width(c, 1, 0);
    lv_obj_set_style_pad_all(c, NV_SP_4, 0);
    return c;
}

// Pressed feedback without draw layers (the P4 renderer hangs on shadows/transforms/opacity).
void pressable(lv_obj_t *o) {
    lv_obj_add_flag(o, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_color(o, th()->surface2, LV_STATE_PRESSED);
    lv_obj_set_style_border_color(o, th()->accent, LV_STATE_PRESSED);
}

lv_obj_t *badge(lv_obj_t *parent, const char *txt, bool strong) {
    lv_obj_t *b = box(parent);
    lv_obj_set_size(b, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_hor(b, 8, 0);
    lv_obj_set_style_pad_ver(b, 2, 0);
    lv_obj_set_style_radius(b, 10, 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(b, strong ? th()->success_solid : th()->surface3, 0);
    label(b, txt, &nv_font_14, strong ? th()->on_primary : th()->text);
    return b;
}

lv_obj_t *qr(lv_obj_t *parent, const char *text, int size) {
    lv_obj_t *q = lv_qrcode_create(parent);
    lv_qrcode_set_size(q, size);
    lv_qrcode_set_dark_color(q, lv_color_black());
    lv_qrcode_set_light_color(q, lv_color_white());
    lv_qrcode_update(q, text, strlen(text));
    lv_obj_set_style_border_color(q, lv_color_white(), 0);   // quiet zone for phone cameras
    lv_obj_set_style_border_width(q, 6, 0);
    return q;
}

void set_text(lv_obj_t *l, const char *t) { if (l) nv_kit_label_set(l, t); }

// Restart from a timer so the button's click event unwinds cleanly first.
void reboot_now(void) {
    lv_timer_create([](lv_timer_t *) { esp_restart(); }, 300, nullptr);
}

// NVS key for a remembered VNC password ("ss_vp" + hash of host:port; keys are <= 15 chars).
void pw_key(const char *hostport, char *key, size_t n) {
    uint32_t h = 5381;
    for (const char *p = hostport; *p; p++) h = h * 33 + (uint8_t)*p;
    snprintf(key, n, "ss_vp%08lx", (unsigned long)h);
}

// ------------------------------------------------------------------ live checks
void refresh_live(void) {
    nv_ss_get_status(&L.ss);
    nv_ss_usb_info(&L.usb);
    nv_ss_cast_info(&L.cast);
    nv_ss_vnc_info(&L.vnc);
    // "Mounted but silent" timer for the USB driver check.
    if (L.usb.device_mode && L.usb.mounted && L.usb.frames == 0) {
        if (!s_usb_idle_since) s_usb_idle_since = lv_tick_get() | 1;
    } else {
        s_usb_idle_since = 0;
    }
}

// Mounted for a while and never a frame: the driver is missing or the host isn't Windows.
bool usb_driver_suspect(void) { return s_usb_idle_since && lv_tick_elaps(s_usb_idle_since) > 12000; }

bool live_by(nv_ss_src_t src) { return L.ss.src == src && L.ss.mode != NV_SS_IDLE; }

int build_steps(Method m, Step *out) {
    int n = 0;
    auto add = [&](StepKind k, SsStr t, bool ok, bool fail) {
        out[n++] = {k, t, ok ? ST_OK : (fail ? ST_FAIL : ST_TODO)};
    };
    const bool net = L.cast.net_up;
    switch (m) {
    case M_USB: {
        const bool streaming = live_by(NV_SS_SRC_USB) || L.usb.frames > 0;
        add(K_USB_MODE, S_U1, L.usb.device_mode, false);
        add(K_USB_CABLE, S_U2, L.usb.device_mode && L.usb.mounted, false);
        add(K_USB_DRIVER, S_U3, L.usb.mounted && streaming, usb_driver_suspect());
        add(K_USB_EXTEND, S_U4, live_by(NV_SS_SRC_USB), false);
        break;
    }
    case M_CAST:
        add(K_NET, S_C1, net, false);
        add(K_CAST_PAGE, S_C2, L.cast.client || live_by(NV_SS_SRC_CAST), false);
        add(K_CAST_SHARE, S_C3, live_by(NV_SS_SRC_CAST), false);
        break;
    case M_MACVNC:
    case M_VNC:
    case M_ANDROID: {
        const bool running = L.vnc.state == NV_SS_VNC_RUNNING;
        const bool acked = running || (s_ack & 2u);   // step 2 can't be checked from here: user confirms
        add(K_NET, S_C1, net, false);
        if (m == M_MACVNC) add(K_MAC_SHARING, S_V_MAC1, acked, false);
        else if (m == M_VNC) add(K_VNC_SERVER, S_V_SRV, acked, false);
        else add(K_DROID_APP, S_A1, acked, false);
        add(K_VNC_CONNECT, S_V_CONN, running, L.vnc.state == NV_SS_VNC_FAILED);
        break;
    }
    }
    return n;
}

// Status word for a method row on the METHODS page.
SsStr method_status(Method m, lv_color_t *c) {
    Step st[5];
    const int n = build_steps(m, st);
    int ok = 0;
    bool fail = false;
    for (int i = 0; i < n; i++) { ok += st[i].st == ST_OK; fail |= st[i].st == ST_FAIL; }
    if (ok == n) { *c = th()->success; return S_ST_LIVE; }
    if (fail) { *c = th()->danger; return S_ST_FAIL; }
    if (ok == n - 1) { *c = th()->accent; return S_ST_READY; }
    *c = th()->text_dim;
    return S_ST_ACTION;
}

// ------------------------------------------------------------------ navigation
void go(Page p);
void vnc_status_text(char *b, size_t n);
void build_home(void);
void build_methods(void);
void build_steps_page(void);

void clear_page_refs(void) {
    s_home_net = s_home_usb = s_home_dot = s_home_quick = nullptr;
    s_sug = s_sug_lbl = s_sug_btn = s_sug_btn_lbl = nullptr;
    s_sug_kind = -1;
    memset(s_meth_status, 0, sizeof s_meth_status);
    memset(s_step_icon, 0, sizeof s_step_icon);
    memset(s_step_lbl, 0, sizeof s_step_lbl);
    s_detail = s_detail_live = s_next_btn = s_next_lbl = s_vnc_list = nullptr;
    s_ta_host = s_ta_pass = s_ta_user = nullptr;
    s_focused_ta = nullptr;
    s_vnc_list_n = -1;
    s_shown_step = -2;
    s_shown_sig = 0;
}

void go(Page p) {
    if (!s_root) return;
    nv_ime_hide();
    s_pg = p;
    if (s_page) lv_obj_delete(s_page);
    clear_page_refs();
    s_page = box(s_root);
    lv_obj_set_size(s_page, lv_pct(100), lv_pct(100));
    lv_obj_set_style_pad_all(s_page, NV_SP_4, 0);
    refresh_live();
    switch (p) {
    case P_HOME: build_home(); break;
    case P_METHODS: build_methods(); break;
    case P_STEPS: build_steps_page(); break;
    }
    if (s_overlay) lv_obj_move_foreground(s_overlay);
}

void back_click(lv_event_t *) {
    s_focus = -1;
    go(s_pg == P_STEPS ? P_METHODS : P_HOME);
}

lv_obj_t *page_header(const char *title, const char *sub, bool back) {
    lv_obj_t *h = row(s_page, NV_SP_3);
    lv_obj_set_size(h, lv_pct(100), LV_SIZE_CONTENT);
    if (back) {
        lv_obj_t *b = box(h);
        lv_obj_set_size(b, 48, 48);
        lv_obj_set_style_radius(b, 24, 0);
        lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(b, th()->surface2, 0);
        pressable(b);
        lv_obj_center(icon(b, &ss_ic_back, th()->text_strong));
        lv_obj_add_event_cb(b, back_click, LV_EVENT_CLICKED, nullptr);
    }
    lv_obj_t *t = col(h, 2);
    lv_obj_set_flex_grow(t, 1);
    lv_obj_set_height(t, LV_SIZE_CONTENT);
    label(t, title, &nv_font_28, th()->text_strong);
    if (sub) wrap_label(t, sub, &nv_font_14, th()->text_dim, lv_pct(100));
    return h;
}

// ------------------------------------------------------------------ HOME
void home_status_text(char *net, size_t nn, char *usb, size_t un, lv_color_t *dot) {
    if (L.cast.net_up)
        snprintf(net, nn, ss_tr(strcmp(L.cast.iface, "eth") == 0 ? S_NET_ETH : S_NET_WIFI), L.cast.ip);
    else
        snprintf(net, nn, "%s", ss_tr(S_NET_NONE));
    const SsStr u = !L.usb.device_mode ? S_USB_HOST : live_by(NV_SS_SRC_USB) ? S_USB_LIVE
                  : L.usb.mounted ? S_USB_PC : S_USB_WAIT;
    snprintf(usb, un, "%s", ss_tr(u));
    *dot = (L.cast.net_up || L.usb.mounted) ? th()->success : th()->text_dim;
}

void recent_click(lv_event_t *e) {
    const char *hp = (const char *)lv_event_get_user_data(e);   // "host:port"
    char host[48];
    snprintf(host, sizeof host, "%s", hp);
    uint16_t port = 5900;
    char *c = strrchr(host, ':');
    if (c) { *c = 0; port = (uint16_t)atoi(c + 1); }
    char key[20], pw[64], user[64];
    pw_key(hp, key, sizeof key);
    nv_config_get_str(key, "", pw, sizeof pw);
    key[4] = 'u';   // "ss_vu..." = the Mac user for Apple authentication
    nv_config_get_str(key, "", user, sizeof user);
    if (nv_ss_vnc_connect(host, port, user, pw)) nv_toast(NV_NOTE_INFO, ss_tr(S_V_CONNECTING));
}

void free_user_data(lv_event_t *e) { lv_free(lv_event_get_user_data(e)); }

// Home suggestion: the most useful next action given what the transports report right now.
enum Sug { SUG_NONE = -1, SUG_USB_NODRV, SUG_CAST_WAIT, SUG_VNC_FAIL, SUG_NO_NET };
Sug current_suggestion(void) {
    if (L.ss.mode == NV_SS_LIVE) return SUG_NONE;
    if (usb_driver_suspect()) return SUG_USB_NODRV;
    if (L.cast.client && !live_by(NV_SS_SRC_CAST) && !L.cast.pending) return SUG_CAST_WAIT;
    if (L.vnc.state == NV_SS_VNC_FAILED) return SUG_VNC_FAIL;
    if (!L.cast.net_up && !L.usb.mounted) return SUG_NO_NET;
    return SUG_NONE;
}

void sug_click(lv_event_t *) {
    switch (s_sug_kind) {
    case SUG_USB_NODRV: s_os = OS_WIN; s_method = M_USB; s_focus = 2; go(P_STEPS); break;
    case SUG_CAST_WAIT: s_os = OS_OTHER; s_method = M_CAST; s_focus = -1; go(P_STEPS); break;
    case SUG_VNC_FAIL: {
        // Retry the last server (first recent entry)
        char recent[200];
        nv_config_get_str("ss_vnc_recent", "", recent, sizeof recent);
        char *sep = strpbrk(recent, "|;");
        if (sep) *sep = 0;
        if (recent[0]) {
            char key[20], pw[64], user[64], host[48];
            pw_key(recent, key, sizeof key);
            nv_config_get_str(key, "", pw, sizeof pw);
            key[4] = 'u';
            nv_config_get_str(key, "", user, sizeof user);
            snprintf(host, sizeof host, "%.47s", recent);
            uint16_t port = 5900;
            char *c = strrchr(host, ':');
            if (c) { *c = 0; port = (uint16_t)atoi(c + 1); }
            nv_ss_vnc_connect(host, port, user, pw);
        }
        break;
    }
    case SUG_NO_NET: nv_ui_open_app_id_async("settings"); break;
    default: break;
    }
}

void build_home(void) {
    lv_obj_set_flex_flow(s_page, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s_page, NV_SP_4, 0);

    // Hero: icon + title/subtitle + live status chips
    lv_obj_t *hero = row(s_page, NV_SP_4);
    lv_obj_set_size(hero, lv_pct(100), LV_SIZE_CONTENT);
    icon(hero, &ss_ic_screen, th()->accent);
    lv_obj_t *tc = col(hero, 4);
    lv_obj_set_flex_grow(tc, 1);
    lv_obj_set_height(tc, LV_SIZE_CONTENT);
    label(tc, ss_tr(S_HOME_TITLE), &nv_font_28, th()->text_strong);
    wrap_label(tc, ss_tr(S_HOME_SUB), &nv_font_14, th()->text_dim, lv_pct(100));
    lv_obj_t *chips = row(tc, NV_SP_3);
    lv_obj_set_size(chips, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_style_pad_top(chips, 4, 0);
    s_home_dot = box(chips);
    lv_obj_set_size(s_home_dot, 10, 10);
    lv_obj_set_style_radius(s_home_dot, 5, 0);
    lv_obj_set_style_bg_opa(s_home_dot, LV_OPA_COVER, 0);
    s_home_net = label(chips, "", &nv_font_14, th()->text);
    label(chips, "|", &nv_font_14, th()->divider);
    s_home_usb = label(chips, "", &nv_font_14, th()->text);

    // Smart suggestion banner (the cascade without entering a wizard): hidden until needed.
    s_sug = box(s_page);
    lv_obj_set_size(s_sug, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_style_radius(s_sug, NV_RAD_MD, 0);
    lv_obj_set_style_bg_opa(s_sug, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(s_sug, th()->surface2, 0);
    lv_obj_set_style_border_width(s_sug, 1, 0);
    lv_obj_set_style_border_color(s_sug, th()->accent, 0);
    lv_obj_set_style_pad_all(s_sug, NV_SP_3, 0);
    lv_obj_set_flex_flow(s_sug, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(s_sug, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(s_sug, NV_SP_3, 0);
    icon(s_sug, &ss_ic_warn, th()->accent);
    s_sug_lbl = wrap_label(s_sug, "", &nv_font_14, th()->text, 1);
    lv_obj_set_flex_grow(s_sug_lbl, 1);
    s_sug_btn = nv_kit_button(s_sug, "", true);
    s_sug_btn_lbl = lv_obj_get_child(s_sug_btn, 0);
    lv_obj_add_event_cb(s_sug_btn, sug_click, LV_EVENT_CLICKED, nullptr);
    lv_obj_add_flag(s_sug, LV_OBJ_FLAG_HIDDEN);

    // Device cards
    lv_obj_t *grid = row(s_page, NV_SP_3);
    lv_obj_set_size(grid, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_align(grid, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    for (int i = 0; i < OS_N; i++) {
        lv_obj_t *c = card(grid);
        lv_obj_set_size(c, 182, 190);
        lv_obj_set_flex_flow(c, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(c, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_row(c, 8, 0);
        pressable(c);
        icon(c, kOs[i].icon, th()->text_strong);
        label(c, ss_tr(kOs[i].name), &nv_font_20, th()->text_strong);
        lv_obj_t *b = label(c, ss_tr(kOs[i].best), &nv_font_14, th()->accent);
        lv_obj_set_style_text_align(b, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_add_event_cb(c, [](lv_event_t *e) {
            s_os = (Os)(intptr_t)lv_event_get_user_data(e);
            go(P_METHODS);
        }, LV_EVENT_CLICKED, (void *)(intptr_t)i);
    }

    // Bottom bar: recent VNC servers + options
    lv_obj_t *bar = row(s_page, NV_SP_3);
    lv_obj_set_size(bar, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_t *rl = row(bar, NV_SP_2);
    lv_obj_set_flex_grow(rl, 1);
    lv_obj_set_height(rl, LV_SIZE_CONTENT);
    char recent[200];
    nv_config_get_str("ss_vnc_recent", "", recent, sizeof recent);
    if (recent[0]) {
        label(rl, ss_tr(S_RECENT), &nv_font_14, th()->text_dim);
        // "host:port|name;host:port|name" (newest first, max 3)
        char *save = nullptr;
        int k = 0;
        for (char *it = strtok_r(recent, ";", &save); it && k < 3; it = strtok_r(nullptr, ";", &save), k++) {
            char *sep = strchr(it, '|');
            const char *name = sep ? sep + 1 : it;
            if (sep) *sep = 0;
            lv_obj_t *chip = box(rl);
            lv_obj_set_size(chip, LV_SIZE_CONTENT, 44);
            lv_obj_set_style_pad_hor(chip, 14, 0);
            lv_obj_set_style_radius(chip, 22, 0);
            lv_obj_set_style_bg_opa(chip, LV_OPA_COVER, 0);
            lv_obj_set_style_bg_color(chip, th()->surface2, 0);
            pressable(chip);
            lv_obj_t *cr = row(chip, 6);
            lv_obj_set_size(cr, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
            lv_obj_center(cr);
            icon(cr, &ss_ic_vnc, th()->accent);
            label(cr, name[0] ? name : it, &nv_font_14, th()->text);
            char *hp = lv_strdup(it);
            lv_obj_add_event_cb(chip, recent_click, LV_EVENT_CLICKED, hp);
            lv_obj_add_event_cb(chip, free_user_data, LV_EVENT_DELETE, hp);
        }
    }
    // quick start line (address filled by refresh_home)
    lv_obj_t *q = row(rl, NV_SP_2);
    lv_obj_set_size(q, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    if (!recent[0]) {
        icon(q, &ss_ic_browser, th()->accent);
        s_home_quick = label(q, "", &nv_font_14, th()->text_dim);
    }
    lv_obj_t *ob = nv_kit_button(bar, ss_tr(S_OPTIONS), false);
    lv_obj_add_event_cb(ob, [](lv_event_t *) { ss_show_options(); }, LV_EVENT_CLICKED, nullptr);
}

void refresh_suggestion(void) {
    if (!s_sug) return;
    const Sug k = current_suggestion();
    char b[200] = "";
    switch (k) {
    case SUG_USB_NODRV: snprintf(b, sizeof b, "%s", ss_tr(S_SUG_USB_NODRV)); break;
    case SUG_CAST_WAIT: snprintf(b, sizeof b, ss_tr(S_SUG_CAST_WAIT), L.cast.client_label); break;
    case SUG_VNC_FAIL: {
        char e[120];
        vnc_status_text(e, sizeof e);
        snprintf(b, sizeof b, ss_tr(S_SUG_VNC_FAIL), e);
        break;
    }
    case SUG_NO_NET: snprintf(b, sizeof b, "%s", ss_tr(S_SUG_NO_NET)); break;
    default: break;
    }
    if (k == SUG_NONE) {
        if (!lv_obj_has_flag(s_sug, LV_OBJ_FLAG_HIDDEN)) lv_obj_add_flag(s_sug, LV_OBJ_FLAG_HIDDEN);
        s_sug_kind = -1;
        return;
    }
    set_text(s_sug_lbl, b);
    if (k != s_sug_kind) {
        s_sug_kind = k;
        const SsStr btn = k == SUG_USB_NODRV ? S_SUG_DRIVER : k == SUG_VNC_FAIL ? S_SUG_RETRY
                        : k == SUG_NO_NET ? S_C1_BTN : S_SUG_OPEN;
        if (s_sug_btn_lbl) lv_label_set_text(s_sug_btn_lbl, ss_tr(btn));
        lv_obj_remove_flag(s_sug, LV_OBJ_FLAG_HIDDEN);
    }
}

void refresh_home(void) {
    refresh_suggestion();
    if (!s_home_net) return;
    char net[64], usb[48];
    lv_color_t dot;
    home_status_text(net, sizeof net, usb, sizeof usb, &dot);
    set_text(s_home_net, net);
    set_text(s_home_usb, usb);
    if (s_home_quick) {
        char q[128];
        if (L.cast.net_up) snprintf(q, sizeof q, ss_tr(S_QUICK), L.cast.ip);
        else q[0] = 0;
        set_text(s_home_quick, q);
    }
    nv_kit_bg_color(s_home_dot, dot);
}

// ------------------------------------------------------------------ METHODS
void build_methods(void) {
    lv_obj_set_flex_flow(s_page, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s_page, NV_SP_3, 0);
    char t[64];
    snprintf(t, sizeof t, ss_tr(S_METHODS_TITLE), ss_tr(kOs[s_os].name));
    page_header(t, ss_tr(S_METHODS_SUB), true);

    const OsDef &o = kOs[s_os];
    for (int i = 0; i < o.n; i++) {
        const MethodDef &m = kMethod[o.methods[i]];
        lv_obj_t *c = card(s_page);
        lv_obj_set_size(c, lv_pct(100), LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(c, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(c, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_column(c, NV_SP_4, 0);
        pressable(c);
        if (i == 0) lv_obj_set_style_border_color(c, th()->success_solid, 0);
        icon(c, m.icon, i == 0 ? th()->success : th()->accent);
        lv_obj_t *tc = col(c, 4);
        lv_obj_set_flex_grow(tc, 1);
        lv_obj_set_height(tc, LV_SIZE_CONTENT);
        lv_obj_t *tr = row(tc, NV_SP_2);
        lv_obj_set_size(tr, lv_pct(100), LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(tr, LV_FLEX_FLOW_ROW_WRAP);
        lv_obj_set_style_pad_row(tr, 4, 0);
        label(tr, ss_tr(m.title), &nv_font_20, th()->text_strong);
        if (i == 0) badge(tr, ss_tr(S_B_BEST), true);
        for (int b = 0; b < m.nb; b++) badge(tr, ss_tr(m.badges[b]), false);
        wrap_label(tc, ss_tr(m.desc), &nv_font_14, th()->text_dim, lv_pct(100));
        s_meth_status[i] = label(c, "", &nv_font_14, th()->text_dim);
        icon(c, &ss_ic_chev, th()->text_dim);
        lv_obj_add_event_cb(c, [](lv_event_t *e) {
            s_method = (Method)(intptr_t)lv_event_get_user_data(e);
            s_focus = -1;
            s_ack = 0;
            go(P_STEPS);
        }, LV_EVENT_CLICKED, (void *)(intptr_t)o.methods[i]);
    }
}

void refresh_methods(void) {
    const OsDef &o = kOs[s_os];
    for (int i = 0; i < o.n; i++) {
        if (!s_meth_status[i]) continue;
        lv_color_t c;
        const SsStr s = method_status(o.methods[i], &c);
        set_text(s_meth_status[i], ss_tr(s));
        nv_kit_text_color(s_meth_status[i], c);
    }
}

// ------------------------------------------------------------------ STEPS
int next_method_index(void) {   // index in the OS list of the method after s_method, -1 = none
    const OsDef &o = kOs[s_os];
    for (int i = 0; i < o.n; i++)
        if (o.methods[i] == s_method) return i + 1 < o.n ? i + 1 : -1;
    return -1;
}

void detail_clear(void) {
    if (!s_detail) return;
    lv_obj_clean(s_detail);
    s_detail_live = s_vnc_list = nullptr;
    s_ta_host = s_ta_pass = s_ta_user = nullptr;
    s_focused_ta = nullptr;
    s_vnc_list_n = -1;
}

void detail_title(const char *t, int idx, int n) {
    char b[32];
    snprintf(b, sizeof b, ss_tr(S_STEP_OF), idx + 1, n);
    label(s_detail, b, &nv_font_14, th()->accent);
    label(s_detail, t, &nv_font_20, th()->text_strong);
}

lv_obj_t *detail_text(const char *t) {
    return wrap_label(s_detail, t, &nv_font_14, th()->text, lv_pct(100));
}

// Address + QR block for the PC-side portal.
void detail_address(const char *path) {
    char url[64];
    if (L.cast.net_up) snprintf(url, sizeof url, "http://%s:%d%s", L.cast.ip, NV_SS_CAST_PORT, path);
    else snprintf(url, sizeof url, "%s", ss_tr(S_NET_NONE));
    lv_obj_t *r = row(s_detail, NV_SP_4);
    lv_obj_set_size(r, lv_pct(100), LV_SIZE_CONTENT);
    if (L.cast.net_up) qr(r, url, 120);
    lv_obj_t *u = label(r, url, &nv_font_20, th()->text_strong);
    lv_obj_set_flex_grow(u, 1);
    lv_label_set_long_mode(u, LV_LABEL_LONG_WRAP);
}

void save_recent(const char *host, uint16_t port, const char *name, const char *user, const char *pw) {
    char hp[56];
    snprintf(hp, sizeof hp, "%s:%u", host, (unsigned)port);
    char cur[200], out[260];
    nv_config_get_str("ss_vnc_recent", "", cur, sizeof cur);
    snprintf(out, sizeof out, "%s|%.40s", hp, name && name[0] ? name : host);
    int k = 1;
    char *save = nullptr;
    const size_t hl = strlen(hp);
    for (char *it = strtok_r(cur, ";", &save); it && k < 3; it = strtok_r(nullptr, ";", &save)) {
        if (strncmp(it, hp, hl) == 0 && (it[hl] == '|' || !it[hl])) continue;   // same host: moved to front
        const size_t l = strlen(out);
        snprintf(out + l, sizeof out - l, ";%s", it);
        k++;
    }
    out[199] = 0;   // fits the reader's buffer
    nv_config_set_str("ss_vnc_recent", out);
    char key[20];
    pw_key(hp, key, sizeof key);
    nv_config_set_str(key, pw ? pw : "");
    key[4] = 'u';
    nv_config_set_str(key, user ? user : "");
}

void vnc_connect_from_form(const char *name) {
    if (!s_ta_host) return;
    char host[48];
    snprintf(host, sizeof host, "%s", lv_textarea_get_text(s_ta_host));
    char pw[64], user[64];
    snprintf(pw, sizeof pw, "%s", s_ta_pass ? lv_textarea_get_text(s_ta_pass) : "");
    snprintf(user, sizeof user, "%s", s_ta_user ? lv_textarea_get_text(s_ta_user) : "");
    uint16_t port = 5900;
    char *c = strrchr(host, ':');
    if (c) { *c = 0; port = (uint16_t)atoi(c + 1); if (!port) port = 5900; }
    if (!host[0]) return;
    nv_ime_hide();
    if (nv_ss_vnc_connect(host, port, user, pw)) {
        s_pend.on = true;
        snprintf(s_pend.host, sizeof s_pend.host, "%s", host);
        s_pend.port = port;
        snprintf(s_pend.name, sizeof s_pend.name, "%s", name ? name : "");
        snprintf(s_pend.user, sizeof s_pend.user, "%s", user);
        snprintf(s_pend.pw, sizeof s_pend.pw, "%s", pw);
    }
}

void vnc_submit(lv_obj_t *, void *) { vnc_connect_from_form(nullptr); }

void apply_kb_pad(void *) {
    if (!s_page) return;
    lv_obj_set_style_pad_bottom(s_page, NV_SP_4 + s_kb_h, 0);
    lv_obj_update_layout(s_page);
    if (s_focused_ta && lv_obj_is_valid(s_focused_ta)) lv_obj_scroll_to_view_recursive(s_focused_ta, LV_ANIM_ON);
}

// NV_EV_IME_VISIBILITY is published by the keyboard code on the LVGL thread: defer the relayout
// until the keyboard's own focus handling has finished.
void on_ime(nv_event_t, const void *d, void *) {
    const auto *v = static_cast<const nv_ime_visibility_t *>(d);
    s_kb_h = (v && v->visible) ? v->height : 0;
    lv_async_call(apply_kb_pad, nullptr);
}

lv_obj_t *form_field(lv_obj_t *parent, const char *ph, nv_ime_type_t type, nv_ime_return_t ret, int32_t w) {
    lv_obj_t *ta = nv_kit_textarea_ex(parent, ph, true, type, ret);
    lv_obj_set_width(ta, w);
    lv_obj_add_event_cb(ta, [](lv_event_t *e) {
        s_focused_ta = (lv_obj_t *)lv_event_get_target(e);
        if (s_kb_h) lv_async_call(apply_kb_pad, nullptr);
    }, LV_EVENT_FOCUSED, nullptr);
    return ta;
}

void discovered_click(lv_event_t *e) {
    // Prefill the form with the tapped server and connect (droidVNC-NG needs no password by
    // default; with one, the error line asks for it and the user types it in the field).
    const char *hp = (const char *)lv_event_get_user_data(e);   // "host:port|name"
    char host[64];
    snprintf(host, sizeof host, "%s", hp);
    char *sep = strchr(host, '|');
    const char *name = "";
    if (sep) { *sep = 0; name = sep + 1; }
    if (s_ta_host) lv_textarea_set_text(s_ta_host, host);
    vnc_connect_from_form(name);
}

void build_vnc_connect(void) {
    detail_text(ss_tr(S_V_CONN_D));
    // Form first (the on-screen keyboard covers the lower ~42 %): one column so the keyboard's
    // Next key walks host -> user -> password and Go on the last field connects.
    lv_obj_t *fr = row(s_detail, NV_SP_3);
    lv_obj_set_size(fr, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_align(fr, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);
    lv_obj_t *fc = col(fr, NV_SP_2);
    lv_obj_set_size(fc, 300, LV_SIZE_CONTENT);
    s_ta_host = form_field(fc, ss_tr(S_V_HOST), NV_IME_URL, NV_IME_RET_NEXT, lv_pct(100));
    if (s_method == M_MACVNC)   // Apple authentication: the Mac account instead of a VNC password
        s_ta_user = form_field(fc, ss_tr(S_V_USER), NV_IME_EMAIL, NV_IME_RET_NEXT, lv_pct(100));
    s_ta_pass = form_field(fc, ss_tr(S_V_PASS), NV_IME_PASSWORD, NV_IME_RET_GO, lv_pct(100));
    lv_obj_t *cb = nv_kit_button(fr, ss_tr(S_V_CONNECT), true);
    lv_obj_add_event_cb(cb, [](lv_event_t *) { vnc_connect_from_form(nullptr); }, LV_EVENT_CLICKED, nullptr);
    nv_ime_set_submit_cb(vnc_submit, nullptr);
    s_detail_live = wrap_label(s_detail, "", &nv_font_14, th()->accent, lv_pct(100));
    if (s_method == M_MACVNC) wrap_label(s_detail, ss_tr(S_V_MAC_ALT), &nv_font_14, th()->text_dim, lv_pct(100));

    // devices found on the network (mDNS _rfb._tcp)
    lv_obj_t *hr = row(s_detail, NV_SP_2);
    lv_obj_set_size(hr, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_t *fl = label(hr, ss_tr(S_V_FOUND), &nv_font_14, th()->text_dim);
    lv_obj_set_flex_grow(fl, 1);
    lv_obj_t *again = box(hr);
    lv_obj_set_size(again, 44, 44);
    lv_obj_set_style_radius(again, 22, 0);
    pressable(again);
    lv_obj_center(icon(again, &ss_ic_refresh, th()->accent));
    lv_obj_add_event_cb(again, [](lv_event_t *) { nv_ss_vnc_discover(); s_vnc_list_n = -1; }, LV_EVENT_CLICKED, nullptr);
    s_vnc_list = col(s_detail, NV_SP_2);
    lv_obj_set_size(s_vnc_list, lv_pct(100), LV_SIZE_CONTENT);

    if (L.cast.net_up) {
        char b[96];
        snprintf(b, sizeof b, "%s  %s:%d", ss_tr(S_V_REVERSE), L.cast.ip, NV_SS_VNC_LISTEN_PORT);
        wrap_label(s_detail, b, &nv_font_14, th()->text_dim, lv_pct(100));
    }
    nv_ss_vnc_discover();
}

void refresh_vnc_list(void) {
    if (!s_vnc_list) return;
    nv_ss_vnc_server_t sv[6];
    const int n = nv_ss_vnc_discovered(sv, 6);
    const bool busy = nv_ss_vnc_discovering();
    const int sig = n * 2 + (busy ? 1 : 0);
    if (sig == s_vnc_list_n) return;
    s_vnc_list_n = sig;
    lv_obj_clean(s_vnc_list);
    if (n == 0) {
        label(s_vnc_list, ss_tr(busy ? S_V_SEARCHING : S_V_NONE), &nv_font_14, th()->text_dim);
        return;
    }
    for (int i = 0; i < n; i++) {
        lv_obj_t *it = box(s_vnc_list);
        lv_obj_set_size(it, lv_pct(100), 46);
        lv_obj_set_style_radius(it, NV_RAD_SM, 0);
        lv_obj_set_style_bg_opa(it, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(it, th()->surface2, 0);
        lv_obj_set_style_pad_hor(it, 12, 0);
        pressable(it);
        lv_obj_t *r = row(it, NV_SP_2);
        lv_obj_set_size(r, lv_pct(100), LV_SIZE_CONTENT);
        lv_obj_align(r, LV_ALIGN_LEFT_MID, 0, 0);
        icon(r, &ss_ic_vnc, th()->accent);
        char b[112];
        snprintf(b, sizeof b, "%.47s  (%.39s)", sv[i].name[0] ? sv[i].name : sv[i].host, sv[i].host);
        label(r, b, &nv_font_14, th()->text);
        char *hp = (char *)lv_malloc(112);
        snprintf(hp, 112, "%.39s:%u|%.47s", sv[i].host, (unsigned)sv[i].port, sv[i].name);
        lv_obj_add_event_cb(it, discovered_click, LV_EVENT_CLICKED, hp);
        lv_obj_add_event_cb(it, free_user_data, LV_EVENT_DELETE, hp);
    }
}

void vnc_status_text(char *b, size_t n) {
    b[0] = 0;
    switch (L.vnc.state) {
    case NV_SS_VNC_CONNECTING: snprintf(b, n, "%s %s", ss_tr(S_V_CONNECTING), L.vnc.host); break;
    case NV_SS_VNC_RUNNING:
        snprintf(b, n, ss_tr(S_LIVE_WITH), L.vnc.desktop[0] ? L.vnc.desktop : L.vnc.host);
        break;
    case NV_SS_VNC_FAILED: {
        static const SsStr map[] = {S_V_ERR_CLOSED, S_V_ERR_CONNECT, S_V_ERR_PROTOCOL, S_V_ERR_SECURITY,
                                    S_V_ERR_AUTH, S_V_ERR_NEEDPW, S_V_ERR_CLOSED, S_V_ERR_MEMORY};
        const int e = (int)L.vnc.err;
        snprintf(b, n, "%s%s%s", ss_tr(map[e >= 0 && e < 8 ? e : 0]), L.vnc.detail[0] ? "  " : "", L.vnc.detail);
        break;
    }
    default: break;
    }
}

// Steps the board can't verify (a setting on the other device): the user confirms and moves on.
void done_next_button(int idx) {
    lv_obj_t *b = nv_kit_button(s_detail, ss_tr(S_DONE_NEXT), true);
    lv_obj_add_event_cb(b, [](lv_event_t *e) {
        s_ack |= 1u << (int)(intptr_t)lv_event_get_user_data(e);
        s_focus = -1;   // follow the next open step
        s_shown_step = -2;
    }, LV_EVENT_CLICKED, (void *)(intptr_t)idx);
}

void build_detail(const Step *st, int n, int idx) {
    detail_clear();
    const Step &s = st[idx];
    detail_title(ss_tr(s.title), idx, n);
    switch (s.kind) {
    case K_USB_MODE:
        detail_text(ss_tr(S_U1_D));
        if (L.usb.device_mode_saved && !L.usb.device_mode) {
            label(s_detail, ss_tr(S_U1_PENDING), &nv_font_14, th()->accent);
            lv_obj_t *b = nv_kit_button(s_detail, ss_tr(S_REBOOT), true);
            lv_obj_add_event_cb(b, [](lv_event_t *) {
                nv_config_set_str("ss_resume", "usb");
                reboot_now();
            }, LV_EVENT_CLICKED, nullptr);
        } else if (!L.usb.device_mode) {
            lv_obj_t *b = nv_kit_button(s_detail, ss_tr(S_U1_BTN), true);
            lv_obj_add_event_cb(b, [](lv_event_t *) {
                nv_ss_usb_set_device_mode(true);
                nv_config_set_str("ss_resume", "usb");   // reopen the wizard at the cable step after boot
                reboot_now();
            }, LV_EVENT_CLICKED, nullptr);
        }
        break;
    case K_USB_CABLE:
        detail_text(ss_tr(S_U2_D));
        icon(s_detail, &ss_ic_usb, th()->accent);
        break;
    case K_USB_DRIVER:
        detail_text(ss_tr(S_U3_D));
        detail_address("/driver/windows");
        s_detail_live = wrap_label(s_detail, "", &nv_font_14, th()->danger, lv_pct(100));
        break;
    case K_USB_EXTEND:
        detail_text(ss_tr(S_U4_D));
        wrap_label(s_detail, ss_tr(S_SWIPE_HINT), &nv_font_14, th()->text_dim, lv_pct(100));
        break;
    case K_NET: {
        detail_text(ss_tr(S_C1_D));
        char net[64], usb[48];
        lv_color_t dot;
        home_status_text(net, sizeof net, usb, sizeof usb, &dot);
        label(s_detail, net, &nv_font_20, L.cast.net_up ? th()->success : th()->text_dim);
        lv_obj_t *b = nv_kit_button(s_detail, ss_tr(S_C1_BTN), !L.cast.net_up);
        lv_obj_add_event_cb(b, [](lv_event_t *) { nv_ui_open_app_id_async("settings"); }, LV_EVENT_CLICKED, nullptr);
        break;
    }
    case K_CAST_PAGE:
        detail_text(ss_tr(S_C2_D));
        detail_address("");
        break;
    case K_CAST_SHARE:
        detail_text(ss_tr(S_C3_D));
        s_detail_live = wrap_label(s_detail, "", &nv_font_14, th()->accent, lv_pct(100));
        wrap_label(s_detail, ss_tr(S_SWIPE_HINT), &nv_font_14, th()->text_dim, lv_pct(100));
        break;
    case K_MAC_SHARING:
        detail_text(ss_tr(S_V_MAC1_D));
        done_next_button(idx);
        break;
    case K_VNC_SERVER:
        detail_text(ss_tr(s_os == OS_WIN ? S_V_SRV_WIN : S_V_SRV_LINUX));
        done_next_button(idx);
        break;
    case K_DROID_APP:
        detail_text(ss_tr(S_A1_D));
        qr(s_detail, "https://play.google.com/store/apps/details?id=net.christianbeier.droidvnc_ng", 110);
        done_next_button(idx);
        break;
    case K_VNC_CONNECT:
        build_vnc_connect();
        break;
    }
}

void step_click(lv_event_t *e) {
    s_focus = (int)(intptr_t)lv_event_get_user_data(e);
    s_shown_step = -2;   // force a detail rebuild on the next tick
}

void next_click(lv_event_t *) {
    const int ni = next_method_index();
    if (ni < 0) { go(P_HOME); return; }
    s_method = kOs[s_os].methods[ni];
    s_focus = -1;
    s_ack = 0;
    go(P_STEPS);
}

void build_steps_page(void) {
    lv_obj_set_flex_flow(s_page, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s_page, NV_SP_3, 0);
    const MethodDef &m = kMethod[s_method];
    char t[80];
    snprintf(t, sizeof t, "%s  ·  %s", ss_tr(kOs[s_os].name), ss_tr(m.title));
    page_header(t, nullptr, true);

    lv_obj_t *body = row(s_page, NV_SP_4);
    lv_obj_set_width(body, lv_pct(100));
    lv_obj_set_flex_grow(body, 1);
    lv_obj_set_flex_align(body, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);

    // left: step list + cascade button
    lv_obj_t *left = col(body, NV_SP_2);
    lv_obj_set_size(left, 330, lv_pct(100));
    Step st[5];
    const int n = build_steps(s_method, st);
    for (int i = 0; i < n; i++) {
        lv_obj_t *it = box(left);
        lv_obj_set_size(it, lv_pct(100), 52);
        lv_obj_set_style_radius(it, NV_RAD_SM, 0);
        lv_obj_set_style_bg_opa(it, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(it, th()->surface, 0);
        lv_obj_set_style_pad_hor(it, 12, 0);
        pressable(it);
        lv_obj_t *r = row(it, NV_SP_3);
        lv_obj_set_size(r, lv_pct(100), LV_SIZE_CONTENT);
        lv_obj_align(r, LV_ALIGN_LEFT_MID, 0, 0);
        s_step_icon[i] = icon(r, &ss_ic_todo, th()->text_dim);
        s_step_lbl[i] = label(r, ss_tr(st[i].title), &nv_font_14, th()->text);
        lv_obj_set_flex_grow(s_step_lbl[i], 1);
        lv_label_set_long_mode(s_step_lbl[i], LV_LABEL_LONG_DOT);
        lv_obj_add_event_cb(it, step_click, LV_EVENT_CLICKED, (void *)(intptr_t)i);
    }
    lv_obj_t *sp = box(left);
    lv_obj_set_flex_grow(sp, 1);
    lv_obj_set_width(sp, 1);
    s_next_btn = box(left);
    lv_obj_set_size(s_next_btn, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_style_radius(s_next_btn, NV_RAD_SM, 0);
    lv_obj_set_style_bg_opa(s_next_btn, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(s_next_btn, th()->surface2, 0);
    lv_obj_set_style_pad_all(s_next_btn, 12, 0);
    pressable(s_next_btn);
    s_next_lbl = wrap_label(s_next_btn, "", &nv_font_14, th()->text, lv_pct(100));
    lv_obj_add_event_cb(s_next_btn, next_click, LV_EVENT_CLICKED, nullptr);

    // right: detail card
    s_detail = card(body);
    lv_obj_set_flex_grow(s_detail, 1);
    lv_obj_set_height(s_detail, lv_pct(100));
    lv_obj_set_flex_flow(s_detail, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s_detail, NV_SP_3, 0);
    lv_obj_add_flag(s_detail, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_detail, LV_OBJ_FLAG_CLICKABLE);   // drag target for scrolling long steps
    lv_obj_set_scroll_dir(s_detail, LV_DIR_VER);
}

void refresh_steps(void) {
    if (!s_detail) return;
    Step st[5];
    const int n = build_steps(s_method, st);
    int first_open = n - 1;
    for (int i = 0; i < n; i++) if (st[i].st != ST_OK) { first_open = i; break; }
    const int focus = (s_focus >= 0 && s_focus < n) ? s_focus : first_open;

    bool any_fail = false;
    for (int i = 0; i < n; i++) {
        const lv_image_dsc_t *img = st[i].st == ST_OK ? &ss_ic_ok : st[i].st == ST_FAIL ? &ss_ic_fail
                                  : i == first_open ? &ss_ic_cur : &ss_ic_todo;
        const lv_color_t c = st[i].st == ST_OK ? th()->success : st[i].st == ST_FAIL ? th()->danger
                           : i == first_open ? th()->accent : th()->text_dim;
        if (s_step_icon[i]) {
            if (lv_image_get_src(s_step_icon[i]) != (const void *)img) lv_image_set_src(s_step_icon[i], img);
            lv_obj_set_style_image_recolor(s_step_icon[i], c, 0);
        }
        if (s_step_lbl[i]) nv_kit_text_color(s_step_lbl[i], i == focus ? th()->text_strong : th()->text);
        any_fail |= st[i].st == ST_FAIL;
    }

    // Detail panel: rebuild when the focused step or its inputs change, not every tick (the VNC
    // form keeps its typed text: it only rebuilds on a focus change).
    const uint32_t sig = (uint32_t)focus * 131u + (uint32_t)st[focus].st * 7u + (L.cast.net_up ? 1u : 0u) +
                         (L.usb.device_mode_saved ? 33u : 0u) + (L.usb.device_mode ? 55u : 0u);
    if (focus != s_shown_step || (sig != s_shown_sig && st[focus].kind != K_VNC_CONNECT)) {
        s_shown_step = focus;
        s_shown_sig = sig;
        build_detail(st, n, focus);
    }

    if (s_detail_live) {
        char b[160] = "";
        const StepKind k = st[focus].kind;
        if (k == K_USB_DRIVER && st[focus].st == ST_FAIL) snprintf(b, sizeof b, "%s", ss_tr(S_U3_FAIL));
        if (k == K_CAST_SHARE && L.cast.client) snprintf(b, sizeof b, ss_tr(S_C3_WAIT), L.cast.client_label);
        if (k == K_VNC_CONNECT) {
            vnc_status_text(b, sizeof b);
            nv_kit_text_color(s_detail_live, L.vnc.state == NV_SS_VNC_FAILED ? th()->danger : th()->accent);
        }
        if (k == K_USB_DRIVER || k == K_CAST_SHARE || k == K_VNC_CONNECT) set_text(s_detail_live, b);
    }
    if (st[focus].kind == K_VNC_CONNECT) refresh_vnc_list();

    // Cascade button: the next method (highlighted when this one failed).
    if (s_next_lbl) {
        const int ni = next_method_index();
        char b[120];
        if (ni >= 0)
            snprintf(b, sizeof b, "%s  %s", ss_tr(any_fail ? S_NEXT_NOW : S_NEXT),
                     ss_tr(kMethod[kOs[s_os].methods[ni]].title));
        else
            snprintf(b, sizeof b, "%s", ss_tr(S_OTHER_DEVICE));
        set_text(s_next_lbl, b);
        nv_kit_bg_color(s_next_btn, any_fail ? th()->primary : th()->surface2);
        nv_kit_text_color(s_next_lbl, any_fail ? th()->on_primary : th()->text);
    }
}

// ------------------------------------------------------------------ overlays
void overlay_close(void) {
    if (s_overlay) { lv_obj_delete(s_overlay); s_overlay = nullptr; }
    s_overlay_kind = 0;
    s_pause_stats = nullptr;
}

void overlay_close_async(void) {
    // Buttons live inside the overlay: delete it after the click event has unwound.
    lv_async_call([](void *) { overlay_close(); }, nullptr);
}

lv_obj_t *overlay_sheet(int kind, int32_t w) {
    overlay_close();
    s_overlay_kind = kind;
    s_overlay = box(s_root);
    lv_obj_set_size(s_overlay, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_color(s_overlay, th()->scrim, 0);
    lv_obj_set_style_bg_opa(s_overlay, LV_OPA_70, 0);
    lv_obj_add_flag(s_overlay, LV_OBJ_FLAG_CLICKABLE);   // swallow taps behind the sheet
    lv_obj_t *c = card(s_overlay);
    lv_obj_set_size(c, w, LV_SIZE_CONTENT);
    lv_obj_center(c);
    lv_obj_set_flex_flow(c, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(c, NV_SP_3, 0);
    lv_obj_set_style_pad_all(c, NV_SP_5, 0);
    return c;
}

lv_obj_t *button_row(lv_obj_t *parent) {
    lv_obj_t *br = row(parent, NV_SP_2);
    lv_obj_set_size(br, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_align(br, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    return br;
}

void show_approval(void) {
    lv_obj_t *c = overlay_sheet(1, 600);
    lv_obj_t *h = row(c, NV_SP_3);
    lv_obj_set_size(h, lv_pct(100), LV_SIZE_CONTENT);
    icon(h, &ss_ic_shield, th()->accent);
    label(h, ss_tr(S_ASK_T), &nv_font_20, th()->text_strong);
    wrap_label(c, L.cast.client_label, &nv_font_20, th()->text_strong, lv_pct(100));
    wrap_label(c, ss_tr(S_ASK_D), &nv_font_14, th()->text, lv_pct(100));
    lv_obj_t *br = button_row(c);
    lv_obj_t *d = nv_kit_button(br, ss_tr(S_ASK_DENY), false);
    lv_obj_add_event_cb(d, [](lv_event_t *) { nv_ss_cast_answer(false, false); overlay_close_async(); }, LV_EVENT_CLICKED, nullptr);
    lv_obj_t *o = nv_kit_button(br, ss_tr(S_ASK_ONCE), false);
    lv_obj_add_event_cb(o, [](lv_event_t *) { nv_ss_cast_answer(true, false); overlay_close_async(); }, LV_EVENT_CLICKED, nullptr);
    lv_obj_t *a = nv_kit_button(br, ss_tr(S_ASK_ALWAYS), true);
    lv_obj_add_event_cb(a, [](lv_event_t *) { nv_ss_cast_answer(true, true); overlay_close_async(); }, LV_EVENT_CLICKED, nullptr);
}

void show_paused(void) {
    lv_obj_t *c = overlay_sheet(2, 620);
    lv_obj_t *h = row(c, NV_SP_3);
    lv_obj_set_size(h, lv_pct(100), LV_SIZE_CONTENT);
    icon(h, &ss_ic_swipe, th()->accent);
    lv_obj_t *tc = col(h, 2);
    lv_obj_set_flex_grow(tc, 1);
    lv_obj_set_height(tc, LV_SIZE_CONTENT);
    label(tc, ss_tr(S_PAUSED), &nv_font_28, th()->text_strong);
    char b[96];
    snprintf(b, sizeof b, "%s  ·  %s", L.ss.peer, nv_ss_src_name(L.ss.src));
    label(tc, b, &nv_font_14, th()->text_dim);
    wrap_label(c, ss_tr(S_PAUSED_D), &nv_font_14, th()->text, lv_pct(100));
    s_pause_stats = label(c, "", &nv_font_14, th()->text_dim);
    lv_obj_t *br = button_row(c);
    lv_obj_t *d = nv_kit_button(br, ss_tr(S_DISCONNECT), false);
    lv_obj_add_event_cb(d, [](lv_event_t *) { nv_ss_disconnect(); overlay_close_async(); }, LV_EVENT_CLICKED, nullptr);
    lv_obj_t *r = nv_kit_button(br, ss_tr(S_RESUME), true);
    lv_obj_add_event_cb(r, [](lv_event_t *) { overlay_close_async(); nv_ss_resume(); }, LV_EVENT_CLICKED, nullptr);
}

void options_usb_pc(lv_event_t *) { nv_ss_usb_set_device_mode(true); lv_async_call([](void *) { ss_show_options(); }, nullptr); }
void options_usb_acc(lv_event_t *) { nv_ss_usb_set_device_mode(false); lv_async_call([](void *) { ss_show_options(); }, nullptr); }

}  // namespace

// Options sheet. Global so the home page's lambda can reach it before its definition.
void ss_show_options(void) {
    if (!s_root) return;
    refresh_live();
    lv_obj_t *c = overlay_sheet(3, 660);
    label(c, ss_tr(S_OPTIONS), &nv_font_20, th()->text_strong);
    nv_kit_switch_row(c, ss_tr(S_OPT_AUTO), nv_config_get_bool("ss_auto", true), [](lv_event_t *e) {
        nv_config_set_bool("ss_auto", lv_obj_has_state((lv_obj_t *)lv_event_get_target(e), LV_STATE_CHECKED));
    });
    nv_kit_switch_row(c, ss_tr(S_OPT_ASK), nv_ss_cast_ask_enabled(), [](lv_event_t *e) {
        nv_ss_cast_set_ask(lv_obj_has_state((lv_obj_t *)lv_event_get_target(e), LV_STATE_CHECKED));
    });
    lv_obj_t *fb = nv_kit_button(c, ss_tr(S_OPT_FORGET), false);
    lv_obj_add_event_cb(fb, [](lv_event_t *) {
        nv_ss_cast_forget_all();
        nv_toast(NV_NOTE_OK, ss_tr(S_OPT_FORGOT));
    }, LV_EVENT_CLICKED, nullptr);
    lv_obj_t *fr = nv_kit_button(c, ss_tr(S_OPT_FORGET_RECENT), false);
    lv_obj_add_event_cb(fr, [](lv_event_t *) {
        char recent[200];
        nv_config_get_str("ss_vnc_recent", "", recent, sizeof recent);
        char *save = nullptr;   // clear the remembered passwords/users of every listed server too
        for (char *it = strtok_r(recent, ";", &save); it; it = strtok_r(nullptr, ";", &save)) {
            char *sep = strchr(it, '|');
            if (sep) *sep = 0;
            char key[20];
            pw_key(it, key, sizeof key);
            nv_config_set_str(key, "");
            key[4] = 'u';
            nv_config_set_str(key, "");
        }
        nv_config_set_str("ss_vnc_recent", "");
        nv_toast(NV_NOTE_OK, ss_tr(S_OPT_FORGOT));
    }, LV_EVENT_CLICKED, nullptr);

    label(c, ss_tr(S_OPT_USB), &nv_font_14, th()->text_dim);
    lv_obj_t *ur = row(c, NV_SP_2);
    lv_obj_set_size(ur, lv_pct(100), LV_SIZE_CONTENT);
    const bool dev = L.usb.device_mode_saved;
    lv_obj_t *pc = nv_kit_button(ur, ss_tr(S_OPT_USB_PC), dev);
    lv_obj_t *acc = nv_kit_button(ur, ss_tr(S_OPT_USB_ACC), !dev);
    lv_obj_add_event_cb(pc, options_usb_pc, LV_EVENT_CLICKED, nullptr);
    lv_obj_add_event_cb(acc, options_usb_acc, LV_EVENT_CLICKED, nullptr);
    lv_obj_t *br = button_row(c);
    if (L.usb.device_mode_saved != L.usb.device_mode) {
        label(br, ss_tr(S_OPT_REBOOT_NOTE), &nv_font_14, th()->accent);
        lv_obj_t *rb = nv_kit_button(br, ss_tr(S_REBOOT), true);
        lv_obj_add_event_cb(rb, [](lv_event_t *) { reboot_now(); }, LV_EVENT_CLICKED, nullptr);
    }
    lv_obj_t *done = nv_kit_button(br, ss_tr(S_DONE), L.usb.device_mode_saved == L.usb.device_mode);
    lv_obj_add_event_cb(done, [](lv_event_t *) { overlay_close_async(); }, LV_EVENT_CLICKED, nullptr);
}

namespace {

// ------------------------------------------------------------------ tick
void tick_cb(lv_timer_t *) {
    if (!s_root) return;
    refresh_live();

    // a connection typed in the form made it to the desktop: remember it (and forget failures)
    if (s_pend.on && L.vnc.state == NV_SS_VNC_RUNNING) {
        save_recent(s_pend.host, s_pend.port, s_pend.name, s_pend.user, s_pend.pw);
        memset(&s_pend, 0, sizeof s_pend);
    } else if (s_pend.on && L.vnc.state == NV_SS_VNC_FAILED) {
        memset(&s_pend, 0, sizeof s_pend);
    }

    // session end -> toast with the reason
    if (L.ss.generation != s_last_gen) {
        s_last_gen = L.ss.generation;
        if (L.ss.mode != NV_SS_IDLE) {
            s_toast_armed = true;
        } else if (s_toast_armed && L.ss.last_reason[0]) {
            s_toast_armed = false;
            char b[96];
            snprintf(b, sizeof b, "%s: %s", ss_tr(S_ENDED), L.ss.last_reason);
            nv_toast(NV_NOTE_INFO, b);
        }
    }

    // Overlays driven by engine/transport state. Approval wins; the options sheet stays until
    // the user closes it.
    if (L.cast.pending) {
        if (s_overlay_kind != 1) show_approval();
    } else {
        if (s_overlay_kind == 1) overlay_close();
        if (L.ss.mode == NV_SS_PAUSED) {
            if (s_overlay_kind == 0) show_paused();
        } else if (s_overlay_kind == 2) {
            overlay_close();
        }
    }
    if (s_overlay_kind == 2 && s_pause_stats) {
        char b[96];
        snprintf(b, sizeof b, "%u s  ·  %u", (unsigned)L.ss.session_s, (unsigned)L.ss.updates);
        set_text(s_pause_stats, b);
    }

    switch (s_pg) {
    case P_HOME: refresh_home(); break;
    case P_METHODS: refresh_methods(); break;
    case P_STEPS: refresh_steps(); break;
    }
}

void page_deleted(lv_event_t *) {
    if (s_tick) { lv_timer_delete(s_tick); s_tick = nullptr; }
    nv_event_unsubscribe(NV_EV_IME_VISIBILITY, on_ime, nullptr);
    s_kb_h = 0;
    nv_ime_set_submit_cb(nullptr, nullptr);
    nv_ss_vnc_set_listen(false);
    nv_ss_close();
    s_root = s_page = s_overlay = nullptr;
    s_overlay_kind = 0;
    s_pause_stats = nullptr;
    clear_page_refs();
}

void ss_build(lv_obj_t *content) {
    lv_obj_set_style_bg_color(content, th()->bg, 0);
    lv_obj_set_style_bg_opa(content, LV_OPA_COVER, 0);
    s_root = box(content);
    lv_obj_set_size(s_root, lv_pct(100), lv_pct(100));
    lv_obj_add_event_cb(s_root, page_deleted, LV_EVENT_DELETE, nullptr);

    if (!nv_ss_open()) NV_LOGE(TAG, "engine open failed (decode buffers)");
    nv_event_subscribe(NV_EV_IME_VISIBILITY, on_ime, nullptr);
    nv_ss_vnc_set_listen(true);

    // Came back from the "switch to PC mode" restart: resume the USB wizard where it was.
    char resume[8];
    nv_config_get_str("ss_resume", "", resume, sizeof resume);
    s_pg = P_HOME;
    if (strcmp(resume, "usb") == 0) {
        nv_config_set_str("ss_resume", "");
        s_os = OS_WIN;
        s_method = M_USB;
        s_pg = P_STEPS;
    }
    s_focus = -1;
    s_last_gen = 0;
    s_toast_armed = false;
    s_usb_idle_since = 0;
    go(s_pg);
    s_tick = lv_timer_create(tick_cb, 400, nullptr);
    tick_cb(nullptr);
}

const NvApp kScreenApp = {"secondscreen", "Second Screen", &nv_icon_screen, 4u << 20,
                          ss_build, NV_STR_APP_SCREEN, nullptr};

}  // namespace

void secondscreen_app_register(void) {
    nv_ss_init();   // transports listen from boot (network sender -> auto-open)
    nv_app_register(&kScreenApp);
}
