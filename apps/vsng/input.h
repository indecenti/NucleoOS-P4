/* input.h - one set of analogue controls from every device NucleoOS offers: gamepads (nv_pad_state),
 * keyboard (nv_kbd_state), mouse (nv_mouse_read) and touch. Header-only, shared by the apps.
 *
 *   move_x/move_y  -256..256  left stick / WASD-arrows (ramped) / touch stick (left half)
 *   look_x/look_y  per-frame look delta in "pixels": right stick / mouse / touch drag (right half)
 *   climb          -256..256  LB-RB / R-F / wheel / two fingers
 *   boost          0..256     RT / Shift / left mouse button / tap-and-hold right half
 *   pause_edge, confirm_edge, back_edge  one frame each
 *
 * The last device that moved is in_last (touch drawing, hints). Screen coordinates are landscape:
 * the canvas is quarter-turned, so touch arrives upright and is turned back here with g_h.
 * Rules (keyboard-support skill): arrows/WASD move, Enter/Space confirm, Esc pause/back.
 */
#ifndef VS_INPUT_H
#define VS_INPUT_H

#include "nucleo_sdk.h"

enum { IN_TOUCH = 0, IN_KEYS = 1, IN_PAD = 2, IN_MOUSE = 3 };

typedef struct {
    int move_x, move_y, look_x, look_y, climb, boost;
    int pause_edge, confirm_edge, back_edge;
    int last;                            /* IN_* */
    /* touch stick, for drawing: active, origin and knob in screen px */
    int stick_on, stick_x0, stick_y0, stick_x, stick_y;
    /* private */
    unsigned char keys[8], prev_keys[8];
    int nk, pnk;
    int kx, ky;                          /* ramped keyboard axes */
    unsigned pad_prev;
    int t_down, t_prev, t_x0, t_y0, t_lx, t_ly, t_left, t_two, t_ty2, t_hold_ms;
    int mouse_seen;
} in_state;

static inline int in_key(const in_state *s, int usage)
{
    int i;
    for (i = 1; i <= s->nk; i++) if (s->keys[i] == usage) return 1;
    return 0;
}
static inline int in_key_edge(const in_state *s, int usage)
{
    int i, was = 0;
    for (i = 1; i <= s->pnk; i++) if (s->prev_keys[i] == usage) was = 1;
    return in_key(s, usage) && !was;
}
static inline int in_clamp(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

/* radial dead zone, then rescale: -32768..32767 -> -256..256 */
static inline void in_stick(int x, int y, int *ox, int *oy)
{
    int m = (x < 0 ? -x : x) > (y < 0 ? -y : y) ? (x < 0 ? -x : x) : (y < 0 ? -y : y);
    const int dz = 5000;
    if (m < dz) { *ox = *oy = 0; return; }
    *ox = (int)((long long)x * (m - dz) / m * 256 / (32768 - dz));
    *oy = (int)((long long)y * (m - dz) / m * 256 / (32768 - dz));
}

/* screen_w/h: landscape size; dt: ms since the last call */
static inline void in_poll(in_state *s, int screen_w, int screen_h, int dt)
{
    int i, tx, ty, down, n, kx = 0, ky = 0;
    nv_pad_state_t ps;
    nv_mouse_t m;

    s->move_x = s->move_y = s->look_x = s->look_y = s->climb = s->boost = 0;
    s->pause_edge = s->confirm_edge = s->back_edge = 0;

    /* keyboard: digital keys ramp to full in ~150 ms so turns are smooth */
    __builtin_memcpy(s->prev_keys, s->keys, sizeof s->keys);
    s->pnk = s->nk;
    s->nk = nv_kbd_state(s->keys, sizeof s->keys);
    if (s->nk < 0) s->nk = 0;
    if (in_key(s, 0x50) || in_key(s, 0x04)) kx -= 256;                 /* left / A */
    if (in_key(s, 0x4F) || in_key(s, 0x07)) kx += 256;                 /* right / D */
    if (in_key(s, 0x52) || in_key(s, 0x1A)) ky -= 256;                 /* up / W (y grows down) */
    if (in_key(s, 0x51) || in_key(s, 0x16)) ky += 256;                 /* down / S */
    s->kx += in_clamp(kx - s->kx, -dt * 2, dt * 2);
    s->ky += in_clamp(ky - s->ky, -dt * 2, dt * 2);
    if (s->kx || s->ky) { s->move_x = s->kx; s->move_y = s->ky; }
    if (in_key(s, 0x15)) s->climb = 256;                                /* R */
    if (in_key(s, 0x09)) s->climb = -256;                               /* F */
    if (in_key(s, 0x08)) s->look_x = dt / 2;                            /* E: look right */
    if (in_key(s, 0x14)) s->look_x = -dt / 2;                           /* Q: look left */
    if (in_key(s, 0x4B)) s->look_y = -dt / 3;                           /* PgUp */
    if (in_key(s, 0x4E)) s->look_y = dt / 3;                            /* PgDn */
    if (s->keys[0] & 0x22) s->boost = 256;                              /* Shift */
    if (in_key_edge(s, 0x29)) { s->pause_edge = 1; s->back_edge = 1; }  /* Esc */
    if (in_key_edge(s, 0x28) || in_key_edge(s, 0x2C)) s->confirm_edge = 1;
    if (s->nk > 0) s->last = IN_KEYS;

    /* gamepads: every controller adds in (two players can share one plane...) */
    n = nv_pad_count();
    for (i = 0; i < n && i < 4; i++) {
        int lx, ly, rx, ry;
        unsigned b;
        if (nv_pad_state(i, &ps, sizeof ps) <= 0) continue;
        b = ps.buttons;
        in_stick(ps.lx, ps.ly, &lx, &ly);
        in_stick(ps.rx, ps.ry, &rx, &ry);
        if (b & NV_PADB_LEFT) lx = -256;
        if (b & NV_PADB_RIGHT) lx = 256;
        if (b & NV_PADB_UP) ly = -256;
        if (b & NV_PADB_DOWN) ly = 256;
        if (lx || ly) { s->move_x = lx; s->move_y = ly; s->last = IN_PAD; }
        if (rx || ry) { s->look_x += rx * dt / 256; s->look_y += ry * dt / 384; s->last = IN_PAD; }
        if (b & NV_PADB_RB) s->climb = 256;
        if (b & NV_PADB_LB) s->climb = -256;
        if (b & (NV_PADB_RT | NV_PADB_X)) s->boost = 256;
        if (i == 0) {
            unsigned e = b & ~s->pad_prev;
            if (e & (NV_PADB_START | NV_PADB_B)) s->pause_edge = 1;
            if (e & NV_PADB_B) s->back_edge = 1;
            if (e & NV_PADB_A) s->confirm_edge = 1;
            if (e) s->last = IN_PAD;
            s->pad_prev = b;
        }
    }

    /* mouse: captured on the first call (the OS pointer hides) - look like a flight sim */
    if (nv_mouse_read(&m, sizeof m) == 1 && (m.dx || m.dy || m.wheel || m.buttons || s->mouse_seen)) {
        if (m.dx || m.dy || m.wheel || m.buttons) { s->mouse_seen = 1; s->last = IN_MOUSE; }
        s->look_x += m.dx;
        s->look_y += m.dy;
        if (m.wheel) s->climb = m.wheel > 0 ? 256 : -256;
        if (m.buttons & NV_MOUSE_LEFT) s->boost = 256;
        if (m.buttons & NV_MOUSE_RIGHT) s->move_y = -256;               /* right button: forward */
    }

    /* touch: dynamic stick where the left thumb lands, look-drag on the right, two fingers climb */
    down = nv_touch(&tx, &ty);
    { int X = ty, Y = screen_h - 1 - tx; tx = X; ty = Y; }              /* upright -> screen */
    s->stick_on = 0;
    if (down) {
        int two = nv_touch_count() >= 2;
        if (!s->t_prev) {
            s->t_x0 = tx; s->t_y0 = ty; s->t_lx = tx; s->t_ly = ty;
            s->t_left = tx < screen_w / 2; s->t_hold_ms = 0; s->t_ty2 = ty;
        }
        s->last = IN_TOUCH;
        s->t_hold_ms += dt;
        if (two) {
            s->climb = in_clamp((s->t_ty2 - ty) * 4, -256, 256);
        } else if (s->t_left) {
            int r = screen_h / 6;
            s->move_x = in_clamp((tx - s->t_x0) * 256 / r, -256, 256);
            s->move_y = in_clamp((ty - s->t_y0) * 256 / r, -256, 256);
            s->stick_on = 1;
            s->stick_x0 = s->t_x0; s->stick_y0 = s->t_y0;
            s->stick_x = s->t_x0 + s->move_x * r / 256; s->stick_y = s->t_y0 + s->move_y * r / 256;
        } else {
            s->look_x += tx - s->t_lx;
            s->look_y += ty - s->t_ly;
            if (s->t_hold_ms > 250 && (tx - s->t_x0) * (tx - s->t_x0) + (ty - s->t_y0) * (ty - s->t_y0) < 400)
                s->boost = 256;                                         /* hold still on the right */
        }
        s->t_lx = tx; s->t_ly = ty;
    } else if (s->t_prev && s->t_hold_ms < 250 &&
               (s->t_lx - s->t_x0) * (s->t_lx - s->t_x0) + (s->t_ly - s->t_y0) * (s->t_ly - s->t_y0) < 400) {
        s->confirm_edge = 1;                                            /* a tap */
    }
    s->t_prev = down;
}

#endif
