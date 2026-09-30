// nv_pad_map — SDL GameController mappings for generic HID pads (see nv_pad.h): the compiled
// SDL_GameControllerDB (nv_paddb.c), user lines in /sdcard/data/pads.txt, a heuristic for the
// rest, and applying a mapping to a raw report. Plain C (no IDF): tested on the PC by
// tools/hidpad_test together with nv_hid_gamepad.c.
#include "nv_pad.h"
#include "nv_paddb.h"

#include "nv_log.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "pad";

#ifndef NV_PADS_TXT
#define NV_PADS_TXT "/sdcard/data/pads.txt"
#endif
uint32_t nv_pad_dirs(const nv_pad_input_t *in) {
    uint32_t d = in->buttons & (NV_PADB_UP | NV_PADB_DOWN | NV_PADB_LEFT | NV_PADB_RIGHT);
    const int dz = 13107;   // 40% of full deflection
    if (in->axis[NV_PADA_LX] < -dz) d |= NV_PADB_LEFT;
    if (in->axis[NV_PADA_LX] > dz)  d |= NV_PADB_RIGHT;
    if (in->axis[NV_PADA_LY] < -dz) d |= NV_PADB_UP;
    if (in->axis[NV_PADA_LY] > dz)  d |= NV_PADB_DOWN;
    return d;
}

// ---------------------------------------------------------------- SDL mapping strings

static const char *const kBtnNames[NV_PAD_N_BUTTONS - 2] = {
    "a", "b", "x", "y", "back", "guide", "start", "leftstick", "rightstick", "leftshoulder",
    "rightshoulder", "dpup", "dpdown", "dpleft", "dpright", "misc1", "paddle1", "paddle2", "paddle3",
    "paddle4", "touchpad" };
static const char *const kAxisNames[NV_PAD_N_AXES] = {
    "leftx", "lefty", "rightx", "righty", "lefttrigger", "righttrigger" };

// "b3" / "a2" / "+a2" / "-a1" / "a5~" / "h0.4" -> encoded source byte, 0xFF if unusable.
static uint8_t encode_source(const char *s, size_t n) {
    int half = 0;
    if (n && (*s == '+' || *s == '-')) { half = *s == '+' ? 1 : 2; s++; n--; }
    const bool inv = n && s[n - 1] == '~';
    if (inv) n--;
    if (n < 2) return 0xFF;
    char buf[8];
    if (n - 1 >= sizeof buf) return 0xFF;
    memcpy(buf, s + 1, n - 1);
    buf[n - 1] = 0;
    char *end;
    if (s[0] == 'b') {
        const long v = strtol(buf, &end, 10);
        return (*end || v < 0 || v > 63) ? 0xFF : (uint8_t)v;
    }
    if (s[0] == 'a') {
        const long v = strtol(buf, &end, 10);
        if (*end || v < 0 || v > 15) return 0xFF;
        const int mode = half ? half : (inv ? 3 : 0);
        return (uint8_t)(0x40 | mode << 4 | v);
    }
    if (s[0] == 'h') {
        const long h = strtol(buf, &end, 10);
        if (*end != '.' || h < 0 || h > 3) return 0xFF;
        const long m = strtol(end + 1, &end, 10);
        if (*end || (m != 1 && m != 2 && m != 4 && m != 8)) return 0xFF;
        return (uint8_t)(0x80 | h << 4 | m);
    }
    return 0xFF;
}

bool nv_pad_map_parse(const char *mapping, nv_pad_map_t *out) {
    memset(out, 0xFF, sizeof *out);
    bool face = false;
    for (const char *p = mapping; *p;) {
        const char *e = strchr(p, ',');
        const size_t n = e ? (size_t)(e - p) : strlen(p);
        const char *colon = memchr(p, ':', n);
        if (colon && colon > p && *p != '+' && *p != '-') {
            const size_t kn = (size_t)(colon - p);
            const uint8_t src = encode_source(colon + 1, n - kn - 1);
            if (src != 0xFF) {
                for (int i = 0; i < NV_PAD_N_BUTTONS - 2; i++)
                    if (strlen(kBtnNames[i]) == kn && !memcmp(p, kBtnNames[i], kn)) { out->btn[i] = src; if (i < 4) face = true; }
                for (int i = 0; i < NV_PAD_N_AXES; i++)
                    if (strlen(kAxisNames[i]) == kn && !memcmp(p, kAxisNames[i], kn)) out->axis[i] = src;
            }
        }
        if (!e) break;
        p = e + 1;
    }
    return face;
}

// SDL GUID (32 hex digits) -> bus, VID, PID. Same rules as tools/gen_paddb.py.
static bool guid_ids(const char *g, size_t n, uint8_t *bus, uint16_t *vid, uint16_t *pid) {
    if (n != 32) return false;
    uint8_t b[16];
    for (int i = 0; i < 16; i++) {
        unsigned v;
        char h[3] = { g[2 * i], g[2 * i + 1], 0 };
        if (!isxdigit((unsigned char)h[0]) || !isxdigit((unsigned char)h[1]) || sscanf(h, "%2x", &v) != 1) return false;
        b[i] = (uint8_t)v;
    }
    if (!memcmp(b + 10, "PIDVID", 6)) {
        *bus = NV_PAD_BUS_USB;
        *vid = (uint16_t)(b[0] | b[1] << 8);
        *pid = (uint16_t)(b[2] | b[3] << 8);
        return true;
    }
    if (b[6] || b[7] || b[10] || b[11]) return false;
    *bus = b[0];
    *vid = (uint16_t)(b[4] | b[5] << 8);
    *pid = (uint16_t)(b[8] | b[9] << 8);
    return *vid || *pid;
}

// A user line in /sdcard/data/pads.txt for this device (Bluetooth lines also match USB-less
// lookups and vice versa only when the bus is equal).
static bool user_lookup(uint8_t bus, uint16_t vid, uint16_t pid, nv_pad_map_t *out) {
    FILE *f = fopen(NV_PADS_TXT, "r");
    if (!f) return false;
    char *line = malloc(1024);
    bool found = false;
    while (line && !found && fgets(line, 1024, f)) {
        const char *c = strchr(line, ',');
        uint8_t b; uint16_t v, p;
        if (line[0] == '#' || !c || !guid_ids(line, (size_t)(c - line), &b, &v, &p)) continue;
        if (b != bus || v != vid || p != pid) continue;
        const char *m = strchr(c + 1, ',');                // skip the name
        if (!m) continue;
        line[strcspn(line, "\r\n")] = 0;
        found = nv_pad_map_parse(m + 1, out);
    }
    free(line);
    fclose(f);
    if (found) NV_LOGI(TAG, "%04x:%04x mapped from " NV_PADS_TXT, vid, pid);
    return found;
}

static bool db_lookup(const nv_paddb_row_t *rows, int n, uint16_t vid, uint16_t pid, nv_pad_map_t *out) {
    int lo = 0, hi = n - 1;
    const uint32_t key = (uint32_t)vid << 16 | pid;
    while (lo <= hi) {
        const int mid = (lo + hi) / 2;
        const uint32_t k = (uint32_t)rows[mid].vid << 16 | rows[mid].pid;
        if (k == key) {
            const uint8_t *m = nv_paddb.maps + (size_t)rows[mid].map * nv_paddb.width;
            memcpy(out->btn, m, sizeof out->btn);
            memcpy(out->axis, m + sizeof out->btn, sizeof out->axis);
            return true;
        }
        if (k < key) lo = mid + 1; else hi = mid - 1;
    }
    return false;
}

// Index of the raw axis with Linux ABS code `code`, 0xFF if absent.
static uint8_t axis_of(const uint8_t *codes, uint8_t n, uint8_t code) {
    for (uint8_t i = 0; i < n; i++) if (codes[i] == code) return i;
    return 0xFF;
}
#define AX(i, mode) ((i) == 0xFF ? 0xFF : (uint8_t)(0x40 | (mode) << 4 | (i)))

// A pad nobody has mapped: guess from what it has, the way most DirectInput pads are wired.
static void heuristic(const uint8_t *codes, uint8_t n_axes, uint8_t n_hats, uint8_t n_buttons, nv_pad_map_t *m) {
    memset(m, 0xFF, sizeof *m);
    enum { X = 0, Y = 1, Z = 2, RX = 3, RY = 4, RZ = 5, GAS = 9, BRAKE = 10 };
    const uint8_t gas = axis_of(codes, n_axes, GAS), brake = axis_of(codes, n_axes, BRAKE);
    const uint8_t z = axis_of(codes, n_axes, Z), rz = axis_of(codes, n_axes, RZ);
    const uint8_t rx = axis_of(codes, n_axes, RX), ry = axis_of(codes, n_axes, RY);
    m->axis[NV_PADA_LX] = AX(axis_of(codes, n_axes, X), 0);
    m->axis[NV_PADA_LY] = AX(axis_of(codes, n_axes, Y), 0);
    if (z != 0xFF && rz != 0xFF) {                 // right stick on Z / Rz (DualShock, most pads)
        m->axis[NV_PADA_RX] = AX(z, 0);
        m->axis[NV_PADA_RY] = AX(rz, 0);
        m->axis[NV_PADA_LT] = AX(rx, 0);
        m->axis[NV_PADA_RT] = AX(ry, 0);
    } else if (rx != 0xFF && ry != 0xFF) {         // right stick on Rx / Ry, triggers share Z
        m->axis[NV_PADA_RX] = AX(rx, 0);
        m->axis[NV_PADA_RY] = AX(ry, 0);
        m->axis[NV_PADA_LT] = AX(z, 1);
        m->axis[NV_PADA_RT] = AX(z, 2);
    }
    if (brake != 0xFF) m->axis[NV_PADA_LT] = AX(brake, 0);
    if (gas != 0xFF)   m->axis[NV_PADA_RT] = AX(gas, 0);
    if (n_hats) {
        m->btn[11] = 0x81; m->btn[12] = 0x84; m->btn[13] = 0x88; m->btn[14] = 0x82;
    }
    static const uint8_t kGeneric[] = { 0, 1, 2, 3, 8, 12, 9, 10, 11, 4, 5 };        // a b x y back guide start ls rs lb rb
    static const uint8_t kXboxBle[] = { 0, 1, 3, 4, 10, 12, 11, 13, 14, 6, 7 };      // Xbox Wireless over BLE
    const uint8_t *k = (gas != 0xFF && brake != 0xFF) ? kXboxBle : kGeneric;
    for (int i = 0; i < 11; i++) if (k[i] < n_buttons) m->btn[i] = k[i];
    if (k == kGeneric && m->axis[NV_PADA_LT] == 0xFF && n_buttons > 7) {   // digital L2 / R2
        m->axis[NV_PADA_LT] = 6;
        m->axis[NV_PADA_RT] = 7;
    }
}

bool nv_pad_map_lookup(uint8_t bus, uint16_t vid, uint16_t pid, const uint8_t *axis_codes,
                       uint8_t n_axes, uint8_t n_hats, uint8_t n_buttons, nv_pad_map_t *out) {
    if (user_lookup(bus, vid, pid, out)) return true;
    if (bus == NV_PAD_BUS_BT && db_lookup(nv_paddb.bt, nv_paddb.n_bt, vid, pid, out)) return true;
    if (db_lookup(nv_paddb.usb, nv_paddb.n_usb, vid, pid, out)) return true;
    heuristic(axis_codes, n_axes, n_hats, n_buttons, out);
    return false;
}

// ---------------------------------------------------------------- applying a mapping

static bool src_pressed(uint8_t s, const nv_hid_raw_t *r) {
    if (s == 0xFF) return false;
    switch (s >> 6) {
    case 0: return (r->buttons >> s) & 1;
    case 1: {
        const int v = r->axis[s & 15];
        switch ((s >> 4) & 3) {
        case 0: return v > 0;
        case 1: return v > 16384;
        case 2: return v < -16384;
        default: return v < 0;
        }
    }
    case 2: return (r->hat[(s >> 4) & 3] & (s & 15)) != 0;
    default: return false;
    }
}

// Source -> output range [omin, omax] (sticks -32768..32767, triggers 0..32767), as SDL scales it.
static int16_t src_axis(uint8_t s, const nv_hid_raw_t *r, int omin, int omax) {
    if (s == 0xFF) return omin < 0 ? 0 : (int16_t)omin;
    if ((s >> 6) != 1) return (int16_t)(src_pressed(s, r) ? omax : (omin < 0 ? 0 : omin));   // button: stick rests centred
    const int v = r->axis[s & 15];
    int imin, imax;
    switch ((s >> 4) & 3) {
    case 0:  imin = -32768; imax = 32767; break;
    case 1:  imin = 0;      imax = 32767; break;
    case 2:  imin = 0;      imax = -32768; break;
    default: imin = 32767;  imax = -32768; break;
    }
    int64_t t = (int64_t)(v - imin) * (omax - omin) / (imax - imin) + omin;
    if (t < omin) t = omin;
    if (t > omax) t = omax;
    return (int16_t)t;
}

void nv_pad_map_apply(const nv_pad_map_t *m, const nv_hid_raw_t *raw, nv_pad_input_t *out) {
    uint32_t b = 0;
    for (int i = 0; i < NV_PAD_N_BUTTONS - 2; i++)
        if (src_pressed(m->btn[i], raw)) b |= 1u << i;
    out->buttons = b;
    for (int i = 0; i < NV_PAD_N_AXES; i++) {
        const bool trig = i >= NV_PADA_LT;
        out->axis[i] = src_axis(m->axis[i], raw, trig ? 0 : -32768, 32767);
    }
}
