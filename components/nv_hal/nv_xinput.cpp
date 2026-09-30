// nv_xinput — USB Xbox-protocol controllers -> nv_pad slots. See nv_xinput.h.
//
// Protocol knowledge and structure ported from:
//   - tusb_xinput (usb64), Copyright 2020 Ryan Wendland, MIT License: interface detection by
//     class/subclass/protocol, report layouts, LED / rumble packets, 360 wireless receiver.
//   - SDL2 SDL_hidapi_xboxone.c / SDL_hidapi_xbox360.c, Copyright (C) 1997-2026 Sam Lantinga,
//     zlib License (altered: rewritten for this driver): Share button and Elite paddle offsets.
//   - Linux xpad.c used as protocol documentation only (init packets for picky third-party
//     Xbox One pads, Guide-report ack, 360 wireless presence); no code copied.
//
// One usb_host client task ("xinput") does everything: client events, transfer completions and
// all submits run on it, so the per-pad state needs no locks. The only foreign entry point is the
// rumble hook (any task): it stores the request in the port and wakes the task, which sends it
// when the OUT endpoint is free (latest request wins, nothing blocks).
//
// Lifetime: each claimed interface is a Port with one IN and one OUT transfer. Teardown (device
// gone, or the IN endpoint keeps failing) detaches the nv_pad slot at once, halts + flushes the
// endpoints, waits for the transfers to come back, then releases the interface, frees the
// transfers and closes the device. A transfer that never comes back (3 s) is leaked and its
// Port retired for good, so a late completion can never hit a reused Port.
#include "nv_xinput.h"
#include "nv_pad.h"

#include "nv_log.h"
#include "nv_mem_attr.h"   // NV_PSRAM_BSS: port/device tables are cold, task-context only

#include "usb/usb_host.h"

#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <cstdint>
#include <cstring>

static const char *TAG = "xinput";

namespace {

constexpr int     kMaxDevs  = 4;
constexpr int     kMaxPorts = 8;            // a 360 receiver alone brings 4
constexpr size_t  kBufSize  = 64;           // full-speed interrupt MPS ceiling
constexpr int64_t kCloseUs  = 3000000;      // give up waiting for cancelled transfers after this
constexpr int     kMaxInErrors = 3;         // consecutive IN failures before we drop the pad

enum Type : uint8_t { T_NONE = 0, T_360, T_360W, T_ONE, T_OG };
enum PortState : uint8_t { P_FREE = 0, P_ACTIVE, P_CLOSING, P_RETIRED };

struct Dev {
    bool used;
    bool gone;                // DEV_GONE seen (or a transfer said NO_DEVICE)
    bool want_close;          // teardown requested from a callback
    bool closing;
    uint8_t addr;
    uint16_t vid, pid, bcd;
    usb_device_handle_t h;
    int64_t deadline_us;
    char product[40];
};

struct Port {
    volatile uint8_t state;   // PortState (read by the rumble hook on foreign tasks)
    uint8_t dev;
    uint8_t type;
    uint8_t iface, ep_in, ep_out;
    uint16_t mps_in;
    bool claimed;
    usb_transfer_t *xin, *xout;
    bool in_busy, out_busy;
    bool out_ep0;             // xout is on the control pipe (SET_INTERFACE)
    bool in_recover, out_recover;
    uint8_t in_errors;
    int8_t slot;              // nv_pad slot, -1 none
    uint8_t player;           // 0..3 for the ring LED
    // pending OUT work, sent in this priority order by pump_out()
    bool need_setif;          // One: SET_INTERFACE(1, 0) (PowerA needs it for Guide)
    uint8_t init_step;        // One: index into kOneInit; == size -> done
    bool need_ack;            // One: ack a Guide report
    uint8_t ack_seq;
    bool need_presence;       // 360W: ask the receiver whether a pad is linked
    bool need_led;            // 360 / 360W: player ring
    volatile uint32_t rumble_req;   // low | high << 16 (foreign writer)
    volatile uint8_t rumble_dirty;
    uint8_t gip_seq;          // One: GIP sequence counter (1..255)
    // decoded state
    uint8_t elite;            // 0 no, 1 Elite 1, 2 Elite 2
    uint8_t share;            // 0 no, 1 Series (SDL size table), 2 third party (len - 18)
    bool guide;               // One: Guide comes in its own report
    uint32_t paddles;         // NV_PADB_PADDLE* from the split Elite 2 report
    nv_pad_input_t st;
};

NV_PSRAM_BSS Dev  s_dev[kMaxDevs];
NV_PSRAM_BSS Port s_port[kMaxPorts];

usb_host_client_handle_t s_cl = nullptr;
bool s_started = false;
volatile int s_count = 0;

// Client events are only recorded in the callback (it runs inside handle_events) and acted on
// by the loop afterwards.
uint8_t s_new[8];
int s_n_new = 0;
bool s_rescan = false;

// ------------------------------------------------------------------ packets

// Xbox One init sequence (xpad order). vid/pid 0 = every pad. Byte 2 gets the running sequence.
struct InitPkt { uint16_t vid, pid; uint8_t len; uint8_t d[13]; };
const InitPkt kOneInit[] = {
    // Hori / Titanfall 2 pads: ack the identify request or the sticks stay dead
    {0x0e6f, 0x0165, 13, {0x01, 0x20, 0, 0x09, 0x00, 0x04, 0x20, 0x3a, 0x00, 0x00, 0x00, 0x80, 0x00}},
    {0x0f0d, 0x0067, 13, {0x01, 0x20, 0, 0x09, 0x00, 0x04, 0x20, 0x3a, 0x00, 0x00, 0x00, 0x80, 0x00}},
    // power on: every pad with 2015+ firmware
    {0, 0, 5, {0x05, 0x20, 0, 0x01, 0x00}},
    // One S / Elite 2 previously paired over Bluetooth
    {0x045e, 0x02ea, 5, {0x05, 0x20, 0, 0x0f, 0x06}},
    {0x045e, 0x0b00, 5, {0x05, 0x20, 0, 0x0f, 0x06}},
    // Elite 2: extended report (paddles)
    {0x045e, 0x0b00, 6, {0x4d, 0x10, 0, 0x02, 0x07, 0x00}},
    // Guide LED on + "authentication done": PDP and others wait for these before reporting
    {0, 0, 7, {0x0a, 0x20, 0, 0x03, 0x00, 0x01, 0x14}},
    {0, 0, 6, {0x06, 0x20, 0, 0x02, 0x01, 0x00}},
    // PowerA: a short rumble burst starts the reports, the second packet stops it at once
    {0x24c6, 0x541a, 13, {0x09, 0x00, 0, 0x09, 0x00, 0x0f, 0x00, 0x00, 0x1d, 0x1d, 0xff, 0x00, 0x00}},
    {0x24c6, 0x542a, 13, {0x09, 0x00, 0, 0x09, 0x00, 0x0f, 0x00, 0x00, 0x1d, 0x1d, 0xff, 0x00, 0x00}},
    {0x24c6, 0x543a, 13, {0x09, 0x00, 0, 0x09, 0x00, 0x0f, 0x00, 0x00, 0x1d, 0x1d, 0xff, 0x00, 0x00}},
    {0x24c6, 0x541a, 13, {0x09, 0x00, 0, 0x09, 0x00, 0x0f, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}},
    {0x24c6, 0x542a, 13, {0x09, 0x00, 0, 0x09, 0x00, 0x0f, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}},
    {0x24c6, 0x543a, 13, {0x09, 0x00, 0, 0x09, 0x00, 0x0f, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}},
};
constexpr uint8_t kOneInitN = sizeof kOneInit / sizeof kOneInit[0];

// Xbox One pads whose Share button sits at report[len - 18] (xpad MAP_SHARE_BUTTON list).
struct VidPid { uint16_t vid, pid; };
const VidPid kShareTail[] = {
    {0x03f0, 0x08b6}, {0x0b05, 0x1a38}, {0x0b05, 0x1c96}, {0x0b05, 0x1d04}, {0x0f0d, 0x01b2},
    {0x10f5, 0x7008}, {0x10f5, 0x7073}, {0x20d6, 0x2064}, {0x20d6, 0x400b}, {0x20d6, 0x890b},
    {0x2dc8, 0x200f}, {0x2e24, 0x0423}, {0x2e95, 0x0504}, {0x366c, 0x0005},
};

const char *type_name(uint8_t t) {
    switch (t) {
        case T_360:  return "Xbox 360 Controller";
        case T_360W: return "Xbox 360 Wireless Controller";
        case T_ONE:  return "Xbox One Controller";
        case T_OG:   return "Xbox Controller";
        default:     return "XInput Controller";
    }
}

inline int16_t rd16(const uint8_t *p) { return (int16_t)(uint16_t)(p[0] | p[1] << 8); }
inline uint16_t rdu16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
// XInput Y axes point up, nv_pad's point down. -(-32768) does not fit: clamp.
inline int16_t neg(int16_t v) { return v == INT16_MIN ? INT16_MAX : (int16_t)-v; }
inline int16_t trig8(uint8_t v) { return (int16_t)(v * 128 + (v >> 1)); }            // 0..255 -> 0..32767
inline int16_t trig10(uint16_t v) { if (v > 1023) v = 1023; return (int16_t)(v * 32 + (v >> 5)); }

void utf16_ascii(const usb_str_desc_t *sd, char *out, size_t cap) {
    out[0] = 0;
    if (!sd || sd->bLength < 2) return;
    const int n = (sd->bLength - 2) / 2;
    size_t k = 0;
    for (int i = 0; i < n && k + 1 < cap; i++) {
        const uint16_t c = sd->wData[i];
        out[k++] = (c >= 0x20 && c < 0x7F) ? (char)c : '?';
    }
    while (k && out[k - 1] == ' ') k--;
    out[k] = 0;
}

// ------------------------------------------------------------------ nv_pad glue

bool rumble_hook(void *ctx, uint16_t low, uint16_t high) {
    Port *p = (Port *)ctx;
    if (!p || p->state != P_ACTIVE || !p->ep_out || p->slot < 0) return false;
    __atomic_store_n(&p->rumble_req, (uint32_t)low | (uint32_t)high << 16, __ATOMIC_RELAXED);
    __atomic_store_n(&p->rumble_dirty, (uint8_t)1, __ATOMIC_RELEASE);
    if (s_cl) usb_host_client_unblock(s_cl);   // pump_out sends it on the client task
    return true;
}

void pad_attach(Port &p) {
    if (p.slot >= 0) return;
    const Dev &d = s_dev[p.dev];
    nv_pad_info_t info = {};
    info.source = NV_PAD_SRC_XINPUT;
    info.mapped = 1;
    info.battery = 255;
    info.rumble = p.ep_out ? 1 : 0;
    info.vid = d.vid;
    info.pid = d.pid;
    // Microsoft's own strings are just "Controller": name by protocol; clones keep their string.
    const char *name = type_name(p.type);
    if (d.vid == 0x045e) {
        if (d.pid == 0x0b12) name = "Xbox Series X|S Controller";
        else if (d.pid == 0x02e3) name = "Xbox Elite Controller";
        else if (d.pid == 0x0b00) name = "Xbox Elite Series 2";
    } else if (p.type != T_360W && strlen(d.product) >= 3) {
        name = d.product;
    }
    strlcpy(info.name, name, sizeof info.name);
    memset(&p.st, 0, sizeof p.st);
    p.guide = false;
    p.paddles = 0;
    const int slot = nv_pad_attach(&info);
    if (slot < 0) {
        NV_LOGW(TAG, "dev %u if %u: %s ignored, all %d pad slots taken", d.addr, p.iface, name, NV_PAD_MAX);
        return;
    }
    p.slot = (int8_t)slot;
    if (p.ep_out) nv_pad_set_rumble(slot, rumble_hook, &p);
    int player = nv_pad_count() - 1;   // newest pad = last in connection order
    p.player = (uint8_t)(player < 0 ? 0 : player > 3 ? 3 : player);
    if (p.type == T_360 || p.type == T_360W) p.need_led = true;
    NV_LOGI(TAG, "dev %u if %u: %s %04x:%04x -> pad slot %d (player %u)", d.addr, p.iface, name,
            d.vid, d.pid, slot, p.player + 1);
}

void pad_detach(Port &p) {
    if (p.slot < 0) return;
    nv_pad_detach(p.slot);
    NV_LOGI(TAG, "dev %u if %u: pad slot %d released", s_dev[p.dev].addr, p.iface, p.slot);
    p.slot = -1;
    p.need_led = false;
    __atomic_store_n(&p.rumble_dirty, (uint8_t)0, __ATOMIC_RELAXED);
}

inline void publish(Port &p) { if (p.slot >= 0) nv_pad_update(p.slot, &p.st); }

// ------------------------------------------------------------------ report decoding

// 360 wired / wireless payload: [0]=0x00 [1]=size [2..3]=buttons [4..5]=triggers [6..13]=sticks.
void decode_360(Port &p, const uint8_t *d) {
    const uint8_t b2 = d[2], b3 = d[3];
    uint32_t b = 0;
    if (b2 & 0x01) b |= NV_PADB_UP;
    if (b2 & 0x02) b |= NV_PADB_DOWN;
    if (b2 & 0x04) b |= NV_PADB_LEFT;
    if (b2 & 0x08) b |= NV_PADB_RIGHT;
    if (b2 & 0x10) b |= NV_PADB_START;
    if (b2 & 0x20) b |= NV_PADB_BACK;
    if (b2 & 0x40) b |= NV_PADB_LSTICK;
    if (b2 & 0x80) b |= NV_PADB_RSTICK;
    if (b3 & 0x01) b |= NV_PADB_LB;
    if (b3 & 0x02) b |= NV_PADB_RB;
    if (b3 & 0x04) b |= NV_PADB_GUIDE;
    if (b3 & 0x10) b |= NV_PADB_A;
    if (b3 & 0x20) b |= NV_PADB_B;
    if (b3 & 0x40) b |= NV_PADB_X;
    if (b3 & 0x80) b |= NV_PADB_Y;
    p.st.buttons = b;
    p.st.axis[NV_PADA_LT] = trig8(d[4]);
    p.st.axis[NV_PADA_RT] = trig8(d[5]);
    p.st.axis[NV_PADA_LX] = rd16(d + 6);
    p.st.axis[NV_PADA_LY] = neg(rd16(d + 8));
    p.st.axis[NV_PADA_RX] = rd16(d + 10);
    p.st.axis[NV_PADA_RY] = neg(rd16(d + 12));
    publish(p);
}

// Original Xbox: [0]=0x00 [1]=0x14 [2]=digital [4..9]=analog A B X Y Black White
// [10..11]=triggers [12..19]=sticks.
void decode_og(Port &p, const uint8_t *d) {
    const uint8_t b2 = d[2];
    uint32_t b = 0;
    if (b2 & 0x01) b |= NV_PADB_UP;
    if (b2 & 0x02) b |= NV_PADB_DOWN;
    if (b2 & 0x04) b |= NV_PADB_LEFT;
    if (b2 & 0x08) b |= NV_PADB_RIGHT;
    if (b2 & 0x10) b |= NV_PADB_START;
    if (b2 & 0x20) b |= NV_PADB_BACK;
    if (b2 & 0x40) b |= NV_PADB_LSTICK;
    if (b2 & 0x80) b |= NV_PADB_RSTICK;
    if (d[4] > 0x20) b |= NV_PADB_A;
    if (d[5] > 0x20) b |= NV_PADB_B;
    if (d[6] > 0x20) b |= NV_PADB_X;
    if (d[7] > 0x20) b |= NV_PADB_Y;
    if (d[8] > 0x20) b |= NV_PADB_RB;   // Black
    if (d[9] > 0x20) b |= NV_PADB_LB;   // White
    p.st.buttons = b;
    p.st.axis[NV_PADA_LT] = trig8(d[10]);
    p.st.axis[NV_PADA_RT] = trig8(d[11]);
    p.st.axis[NV_PADA_LX] = rd16(d + 12);
    p.st.axis[NV_PADA_LY] = neg(rd16(d + 14));
    p.st.axis[NV_PADA_RX] = rd16(d + 16);
    p.st.axis[NV_PADA_RY] = neg(rd16(d + 18));
    publish(p);
}

// Elite paddle bits -> NV_PADB_PADDLE1..4 (SDL order). Muted while the pad's own profile maps them.
uint32_t paddle_bits(uint8_t v, uint8_t b1, uint8_t b2, uint8_t b3, uint8_t b4) {
    uint32_t r = 0;
    if (v & b1) r |= NV_PADB_PADDLE1;
    if (v & b2) r |= NV_PADB_PADDLE2;
    if (v & b3) r |= NV_PADB_PADDLE3;
    if (v & b4) r |= NV_PADB_PADDLE4;
    return r;
}

// GIP input report 0x20: [4..5]=buttons [6..9]=triggers 0..1023 [10..17]=sticks, extras after.
void decode_one(Port &p, const uint8_t *d, int n) {
    const uint8_t b4 = d[4], b5 = d[5];
    uint32_t b = 0;
    if (b4 & 0x04) b |= NV_PADB_START;
    if (b4 & 0x08) b |= NV_PADB_BACK;
    if (b4 & 0x10) b |= NV_PADB_A;
    if (b4 & 0x20) b |= NV_PADB_B;
    if (b4 & 0x40) b |= NV_PADB_X;
    if (b4 & 0x80) b |= NV_PADB_Y;
    if (b5 & 0x01) b |= NV_PADB_UP;
    if (b5 & 0x02) b |= NV_PADB_DOWN;
    if (b5 & 0x04) b |= NV_PADB_LEFT;
    if (b5 & 0x08) b |= NV_PADB_RIGHT;
    if (b5 & 0x10) b |= NV_PADB_LB;
    if (b5 & 0x20) b |= NV_PADB_RB;
    if (b5 & 0x40) b |= NV_PADB_LSTICK;
    if (b5 & 0x80) b |= NV_PADB_RSTICK;
    if (p.guide) b |= NV_PADB_GUIDE;

    // Share (Series X|S and friends): position depends on the firmware's report size.
    int share_at = -1;
    if (p.share == 1) share_at = n < 48 ? 18 : n == 48 ? 22 : n == 50 ? 32 : n == 64 ? 46 : -1;
    else if (p.share == 2) share_at = n - 18;
    if (share_at >= 18 && share_at < n && (d[share_at] & 0x01)) b |= NV_PADB_MISC;

    // Elite paddles by report size (33: Elite 1; 38 / 50: Elite 2 fw 4.x / early 5.x). Later
    // Elite 2 firmware sends them in a separate 0x0C report (p.paddles).
    if (p.elite == 1 && n >= 33) {
        const bool remapped = memcmp(d + 4, d + 18, 2) != 0;
        if (!remapped) p.paddles = paddle_bits(d[32], 0x02, 0x08, 0x01, 0x04);
        else p.paddles = 0;
    } else if (p.elite == 2 && (n == 38 || n == 50)) {
        const int at = n == 38 ? 18 : 22;
        p.paddles = d[at + 1] ? 0 : paddle_bits(d[at], 0x01, 0x02, 0x04, 0x08);
    }
    b |= p.paddles;

    p.st.buttons = b;
    p.st.axis[NV_PADA_LT] = trig10(rdu16(d + 6));
    p.st.axis[NV_PADA_RT] = trig10(rdu16(d + 8));
    p.st.axis[NV_PADA_LX] = rd16(d + 10);
    p.st.axis[NV_PADA_LY] = neg(rd16(d + 12));
    p.st.axis[NV_PADA_RX] = rd16(d + 14);
    p.st.axis[NV_PADA_RY] = neg(rd16(d + 16));
    publish(p);
}

void on_report(Port &p, const uint8_t *d, int n) {
    switch (p.type) {
        case T_360:
            if (n >= 14 && d[0] == 0x00 && d[1] >= 0x13) decode_360(p, d);   // else LED/rumble status
            break;
        case T_OG:
            if (n >= 20 && d[0] == 0x00 && d[1] >= 0x14) decode_og(p, d);
            break;
        case T_360W:
            if (n < 2) break;
            if (d[0] & 0x08) {                           // link status: [1] bit 7 = pad present
                const bool present = (d[1] & 0x80) != 0;
                if (present && p.slot < 0) pad_attach(p);
                else if (!present && p.slot >= 0) pad_detach(p);
                break;
            }
            // Input: [1] bit 0 = pad data valid, 360 payload from byte 4.
            if (n >= 18 && (d[1] & 0x01) && d[4] == 0x00 && d[5] >= 0x13) {
                if (p.slot < 0) pad_attach(p);           // linked before we asked: data is proof
                decode_360(p, d + 4);
            }
            break;
        case T_ONE:
            if (n < 4) break;
            switch (d[0]) {
                case 0x20:                               // input
                    if (n >= 18) decode_one(p, d, n);
                    break;
                case 0x07:                               // Guide ("virtual key")
                    if (n < 5) break;
                    if (d[1] & 0x10) { p.need_ack = true; p.ack_seq = d[2]; }   // One S insists
                    p.guide = (d[4] & 0x03) != 0;
                    p.st.buttons = (p.st.buttons & ~(uint32_t)NV_PADB_GUIDE) | (p.guide ? NV_PADB_GUIDE : 0);
                    publish(p);
                    break;
                case 0x0c:                               // Elite 2 fw >= 5.11: paddles split out
                    if (p.elite == 2 && n >= 20) {
                        const uint32_t pad = d[19] ? 0 : paddle_bits(d[18], 0x01, 0x02, 0x04, 0x08);
                        if (pad != p.paddles) {
                            p.st.buttons = (p.st.buttons & ~p.paddles) | pad;
                            p.paddles = pad;
                            publish(p);
                        }
                    }
                    break;
                case 0x02:                               // announce: pad (re)started -> init again
                    if (p.init_step >= kOneInitN) {
                        p.init_step = 0;
                        NV_LOGI(TAG, "dev %u: GIP announce, re-init", s_dev[p.dev].addr);
                    }
                    break;
                default:
                    break;
            }
            break;
        default:
            break;
    }
}

// ------------------------------------------------------------------ transfers

void in_cb(usb_transfer_t *t);
void out_cb(usb_transfer_t *t);

bool submit_in(Port &p) {
    usb_transfer_t *t = p.xin;
    t->device_handle = s_dev[p.dev].h;
    t->bEndpointAddress = p.ep_in;
    t->num_bytes = p.mps_in;
    t->callback = in_cb;
    t->context = &p;
    t->timeout_ms = 0;
    if (usb_host_transfer_submit(t) != ESP_OK) return false;
    p.in_busy = true;
    return true;
}

void in_cb(usb_transfer_t *t) {
    Port &p = *(Port *)t->context;
    p.in_busy = false;
    if (p.state != P_ACTIVE) return;   // cancelled by teardown
    Dev &d = s_dev[p.dev];
    switch (t->status) {
        case USB_TRANSFER_STATUS_COMPLETED:
            p.in_errors = 0;
            on_report(p, t->data_buffer, t->actual_num_bytes);
            if (p.state == P_ACTIVE && !submit_in(p)) p.in_recover = true;   // pipe halted: loop fixes it
            break;
        case USB_TRANSFER_STATUS_NO_DEVICE:
            d.gone = true;
            d.want_close = true;
            break;
        case USB_TRANSFER_STATUS_CANCELED:
            break;
        case USB_TRANSFER_STATUS_STALL:
            NV_LOGW(TAG, "dev %u if %u: IN endpoint stalled -> drop", d.addr, p.iface);
            d.want_close = true;
            break;
        default:   // ERROR / OVERFLOW / TIMED_OUT: the pipe is halted, retry a few times
            if (++p.in_errors >= kMaxInErrors) {
                NV_LOGW(TAG, "dev %u if %u: IN keeps failing (status %d) -> drop", d.addr, p.iface, (int)t->status);
                d.want_close = true;
            } else {
                p.in_recover = true;
            }
            break;
    }
}

void out_cb(usb_transfer_t *t) {
    Port &p = *(Port *)t->context;
    p.out_busy = false;
    const bool ep0 = p.out_ep0;
    p.out_ep0 = false;
    if (p.state != P_ACTIVE) return;
    if (t->status == USB_TRANSFER_STATUS_NO_DEVICE) {
        s_dev[p.dev].gone = true;
        s_dev[p.dev].want_close = true;
    } else if (t->status != USB_TRANSFER_STATUS_COMPLETED && t->status != USB_TRANSFER_STATUS_CANCELED && !ep0) {
        p.out_recover = true;   // a lost LED/rumble packet is harmless; just unhalt the pipe
    }
}

bool submit_out(Port &p, const uint8_t *data, int len) {
    usb_transfer_t *t = p.xout;
    if (len > (int)t->data_buffer_size) return false;
    memcpy(t->data_buffer, data, (size_t)len);
    t->device_handle = s_dev[p.dev].h;
    t->bEndpointAddress = p.ep_out;
    t->num_bytes = len;
    t->callback = out_cb;
    t->context = &p;
    t->timeout_ms = 0;
    if (usb_host_transfer_submit(t) != ESP_OK) { p.out_recover = true; return false; }
    p.out_busy = true;
    return true;
}

// SET_INTERFACE(1, alt 0) on the control pipe, through the OUT transfer (idle at this point).
bool submit_setif(Port &p) {
    usb_transfer_t *t = p.xout;
    auto *s = (usb_setup_packet_t *)t->data_buffer;
    s->bmRequestType = 0x01;   // host->device, standard, interface
    s->bRequest = 0x0B;        // SET_INTERFACE
    s->wValue = 0;
    s->wIndex = 1;
    s->wLength = 0;
    t->num_bytes = sizeof(usb_setup_packet_t);
    t->device_handle = s_dev[p.dev].h;
    t->bEndpointAddress = 0;
    t->callback = out_cb;
    t->context = &p;
    t->timeout_ms = 1000;
    if (usb_host_transfer_submit_control(s_cl, t) != ESP_OK) return false;
    p.out_busy = true;
    p.out_ep0 = true;
    return true;
}

inline uint8_t next_seq(Port &p) {
    if (++p.gip_seq == 0) p.gip_seq = 1;
    return p.gip_seq;
}

void recover_ep(Port &p, uint8_t ep) {
    usb_device_handle_t h = s_dev[p.dev].h;
    usb_host_endpoint_halt(h, ep);   // no-op when the error already halted it
    usb_host_endpoint_flush(h, ep);
    usb_host_endpoint_clear(h, ep);
}

// Send the next pending OUT packet, if the endpoint is free. Client task only.
void pump_out(Port &p) {
    if (p.state != P_ACTIVE || !p.ep_out || !p.xout || p.out_busy) return;
    if (p.out_recover) {
        p.out_recover = false;
        recover_ep(p, p.ep_out);
    }
    const Dev &d = s_dev[p.dev];
    uint8_t pkt[16] = {};

    if (p.type == T_ONE) {
        if (p.need_setif) {
            p.need_setif = false;
            if (submit_setif(p)) return;
        }
        while (p.init_step < kOneInitN) {
            const InitPkt &ip = kOneInit[p.init_step++];
            if ((ip.vid && ip.vid != d.vid) || (ip.pid && ip.pid != d.pid)) continue;
            memcpy(pkt, ip.d, ip.len);
            pkt[2] = next_seq(p);
            if (submit_out(p, pkt, ip.len)) return;
        }
        if (p.need_ack) {
            p.need_ack = false;
            const uint8_t ack[13] = {0x01, 0x20, p.ack_seq, 0x09, 0x00, 0x07, 0x20, 0x02, 0, 0, 0, 0, 0};
            if (submit_out(p, ack, sizeof ack)) return;
        }
    }
    if (p.type == T_360W && p.need_presence) {
        p.need_presence = false;
        const uint8_t q[12] = {0x08, 0x00, 0x0F, 0xC0};
        if (submit_out(p, q, sizeof q)) return;
    }
    if (p.need_led && p.slot >= 0) {
        p.need_led = false;
        const uint8_t cmd = (uint8_t)(0x06 + p.player);   // 6..9: quadrant 1..4 on
        if (p.type == T_360) {
            const uint8_t led[3] = {0x01, 0x03, cmd};
            if (submit_out(p, led, sizeof led)) return;
        } else if (p.type == T_360W) {
            const uint8_t led[12] = {0x00, 0x00, 0x08, (uint8_t)(0x40 + cmd)};
            if (submit_out(p, led, sizeof led)) return;
        }
    }
    if (__atomic_load_n(&p.rumble_dirty, __ATOMIC_ACQUIRE) && p.slot >= 0) {
        __atomic_store_n(&p.rumble_dirty, (uint8_t)0, __ATOMIC_RELAXED);
        const uint32_t r = __atomic_load_n(&p.rumble_req, __ATOMIC_RELAXED);
        const uint16_t lo = (uint16_t)r, hi = (uint16_t)(r >> 16);
        int len = 0;
        switch (p.type) {
            case T_360:    // [3] big (low-frequency) motor, [4] small motor
                pkt[0] = 0x00; pkt[1] = 0x08; pkt[3] = lo >> 8; pkt[4] = hi >> 8;
                len = 8;
                break;
            case T_360W:
                pkt[0] = 0x00; pkt[1] = 0x01; pkt[2] = 0x0F; pkt[3] = 0xC0; pkt[5] = lo >> 8; pkt[6] = hi >> 8;
                len = 12;
                break;
            case T_ONE:    // GIP rumble: main motors 0..127, trigger motors off, long on-period
                pkt[0] = 0x09; pkt[1] = 0x00; pkt[2] = next_seq(p); pkt[3] = 0x09;
                pkt[4] = 0x00; pkt[5] = 0x0F; pkt[6] = 0x00; pkt[7] = 0x00;
                pkt[8] = lo >> 9; pkt[9] = hi >> 9; pkt[10] = 0xFF; pkt[11] = 0x00; pkt[12] = 0xFF;
                len = 13;
                break;
            case T_OG:
                pkt[0] = 0x00; pkt[1] = 0x06; pkt[2] = lo & 0xFF; pkt[3] = lo >> 8; pkt[4] = hi & 0xFF; pkt[5] = hi >> 8;
                len = 6;
                break;
            default:
                break;
        }
        if (len) submit_out(p, pkt, len);
    }
}

// ------------------------------------------------------------------ attach / teardown

struct Found { uint8_t iface, type, ep_in, ep_out; uint16_t mps_in; };

uint8_t classify(uint8_t cls, uint8_t sub, uint8_t proto) {
    if (cls == 0xFF && sub == 0x5D && proto == 0x01) return T_360;
    if (cls == 0xFF && sub == 0x5D && proto == 0x81) return T_360W;
    if (cls == 0xFF && sub == 0x47 && proto == 0xD0) return T_ONE;
    if (cls == 0x58 && sub == 0x42) return T_OG;
    return T_NONE;
}

// Interfaces we drive: every 360W data interface, else the first one of the device's type.
// Headset / audio / security interfaces never match, so they stay unclaimed.
int parse_config(const usb_config_desc_t *cd, Found *out, int max, bool *one_audio) {
    const uint8_t *p = (const uint8_t *)cd;
    const int total = cd->wTotalLength;
    int n = 0;
    uint8_t dev_type = T_NONE;
    Found cur = {};
    bool collecting = false;
    *one_audio = false;
    auto commit = [&]() {
        if (collecting && cur.ep_in && n < max) out[n++] = cur;
        collecting = false;
    };
    for (int off = 0; off + 2 <= total;) {
        const uint8_t len = p[off], type = p[off + 1];
        if (len < 2 || off + len > total) break;
        if (type == USB_B_DESCRIPTOR_TYPE_INTERFACE && len >= 9) {
            commit();
            const uint8_t num = p[off + 2], alt = p[off + 3];
            const uint8_t t = classify(p[off + 5], p[off + 6], p[off + 7]);
            if (num == 1 && p[off + 5] == 0xFF && p[off + 6] == 0x47) *one_audio = true;
            const bool first_of_kind = dev_type == T_NONE || (t == T_360W && dev_type == T_360W);
            if (alt == 0 && t != T_NONE && first_of_kind) {
                bool dup = false;   // same interface number twice (bogus descriptor)
                for (int i = 0; i < n; i++) dup |= out[i].iface == num;
                if (!dup) {
                    cur = {};
                    cur.iface = num;
                    cur.type = t;
                    collecting = true;
                    dev_type = t;
                }
            }
        } else if (type == USB_B_DESCRIPTOR_TYPE_ENDPOINT && len >= 7 && collecting) {
            const uint8_t addr = p[off + 2];
            if ((p[off + 3] & 0x03) == 0x03) {   // interrupt
                if (addr & 0x80) {
                    if (!cur.ep_in) { cur.ep_in = addr; cur.mps_in = (uint16_t)((p[off + 4] | p[off + 5] << 8) & 0x7FF); }
                } else if (!cur.ep_out) {
                    cur.ep_out = addr;
                }
            }
        }
        off += len;
    }
    commit();
    // A One / 360 wired device only ever gets its first matching interface.
    if (n > 1 && out[0].type != T_360W) n = 1;
    return n;
}

void port_free_resources(Port &p) {
    if (p.xin) usb_host_transfer_free(p.xin);
    if (p.xout) usb_host_transfer_free(p.xout);
    p.xin = p.xout = nullptr;
}

void dev_attach(uint8_t addr) {
    for (const Dev &d : s_dev) if (d.used && !d.gone && d.addr == addr) return;   // already ours
    usb_device_handle_t h = nullptr;
    if (usb_host_device_open(s_cl, addr, &h) != ESP_OK) return;
    const usb_config_desc_t *cd = nullptr;
    const usb_device_desc_t *dd = nullptr;
    Found f[4];
    bool one_audio = false;
    int nf = 0;
    if (usb_host_get_active_config_descriptor(h, &cd) == ESP_OK && cd &&
        usb_host_get_device_descriptor(h, &dd) == ESP_OK && dd)
        nf = parse_config(cd, f, 4, &one_audio);
    if (!nf) { usb_host_device_close(s_cl, h); return; }   // not ours (HID, storage, hub, audio...)

    int di = -1;
    for (int i = 0; i < kMaxDevs && di < 0; i++) if (!s_dev[i].used) di = i;
    if (di < 0) {
        NV_LOGW(TAG, "dev %u: too many XInput devices (max %d)", addr, kMaxDevs);
        usb_host_device_close(s_cl, h);
        return;
    }
    Dev &d = s_dev[di];
    memset(&d, 0, sizeof d);
    d.used = true;
    d.addr = addr;
    d.h = h;
    d.vid = dd->idVendor;
    d.pid = dd->idProduct;
    d.bcd = dd->bcdDevice;
    usb_device_info_t info = {};
    if (usb_host_device_info(h, &info) == ESP_OK) utf16_ascii(info.str_desc_product, d.product, sizeof d.product);
    if (info.parent.dev_hdl && info.speed != USB_SPEED_HIGH)
        NV_LOGW(TAG, "dev %u: full-speed pad behind a hub (no TT on the P4): may not work", addr);

    int ok = 0;
    for (int k = 0; k < nf; k++) {
        int pi = -1;
        for (int i = 0; i < kMaxPorts && pi < 0; i++) if (s_port[i].state == P_FREE) pi = i;
        if (pi < 0) { NV_LOGW(TAG, "dev %u: no free port (max %d)", addr, kMaxPorts); break; }
        Port &p = s_port[pi];
        memset((void *)&p, 0, sizeof p);
        p.dev = (uint8_t)di;
        p.type = f[k].type;
        p.iface = f[k].iface;
        p.ep_in = f[k].ep_in;
        p.ep_out = f[k].ep_out;
        p.mps_in = f[k].mps_in && f[k].mps_in <= kBufSize ? f[k].mps_in : (uint16_t)kBufSize;
        p.slot = -1;
        p.init_step = kOneInitN;
        if (usb_host_interface_claim(s_cl, h, p.iface, 0) != ESP_OK) {
            NV_LOGW(TAG, "dev %u: claim of interface %u failed", addr, p.iface);
            continue;
        }
        p.claimed = true;
        bool alloc = usb_host_transfer_alloc(kBufSize, 0, &p.xin) == ESP_OK;
        if (alloc && p.ep_out) alloc = usb_host_transfer_alloc(kBufSize, 0, &p.xout) == ESP_OK;
        if (!alloc) {
            NV_LOGE(TAG, "dev %u: transfer alloc failed (internal DMA RAM?)", addr);
            port_free_resources(p);
            usb_host_interface_release(s_cl, h, p.iface);
            continue;
        }
        p.state = P_ACTIVE;
        switch (p.type) {
            case T_360W:
                p.need_presence = true;   // the pad attaches when the receiver reports a link
                break;
            case T_ONE:
                if (d.vid == 0x045e && d.pid == 0x02e3) p.elite = 1;
                else if (d.vid == 0x045e && d.pid == 0x0b00) p.elite = 2;
                if (d.vid == 0x045e && d.pid == 0x0b12) p.share = 1;
                for (const VidPid &v : kShareTail) if (v.vid == d.vid && v.pid == d.pid) p.share = 2;
                p.need_setif = one_audio;
                p.init_step = 0;
                pad_attach(p);
                break;
            default:
                pad_attach(p);
                break;
        }
        if (!submit_in(p)) p.in_recover = true;
        ok++;
    }
    if (!ok) {
        usb_host_device_close(s_cl, h);
        d.used = false;
        return;
    }
    NV_LOGI(TAG, "dev %u: %s %04x:%04x \"%s\", %d interface(s)", addr, type_name(f[0].type), d.vid, d.pid,
            d.product, ok);
}

// Start tearing a device down: pads leave nv_pad now, transfers get cancelled, the loop finishes.
void dev_close_begin(int di) {
    Dev &d = s_dev[di];
    if (!d.used || d.closing) return;
    d.closing = true;
    d.deadline_us = esp_timer_get_time() + kCloseUs;
    for (Port &p : s_port) {
        if (p.state != P_ACTIVE || p.dev != di) continue;
        pad_detach(p);
        p.state = P_CLOSING;
        if (p.in_busy) { usb_host_endpoint_halt(d.h, p.ep_in); usb_host_endpoint_flush(d.h, p.ep_in); }
        if (p.out_busy && !p.out_ep0) { usb_host_endpoint_halt(d.h, p.ep_out); usb_host_endpoint_flush(d.h, p.ep_out); }
        // (a control transfer can't be flushed: it completes or fails on its own)
    }
    NV_LOGI(TAG, "dev %u: %s", d.addr, d.gone ? "unplugged" : "closing");
}

// Progress of closing devices; true while any is still waiting.
bool dev_close_progress(void) {
    bool pending = false;
    const int64_t now = esp_timer_get_time();
    for (int di = 0; di < kMaxDevs; di++) {
        Dev &d = s_dev[di];
        if (!d.used || !d.closing) continue;
        const bool late = now > d.deadline_us;
        bool waiting = false, leaked = false;
        for (Port &p : s_port) {
            if (p.dev != di || p.state != P_CLOSING) continue;
            if (p.in_busy || p.out_busy) {
                if (!late) { waiting = true; continue; }
                NV_LOGE(TAG, "dev %u if %u: transfer never returned -> leaked", d.addr, p.iface);
                p.state = P_RETIRED;   // its late completion must never hit a reused port
                leaked = true;
                continue;
            }
            if (p.claimed) {
                const esp_err_t r = usb_host_interface_release(s_cl, d.h, p.iface);
                if (r == ESP_OK || r == ESP_ERR_NOT_FOUND) p.claimed = false;
                else if (!late) { waiting = true; continue; }   // completions not yet drained
                else { leaked = true; p.state = P_RETIRED; continue; }
            }
            port_free_resources(p);
            p.state = P_FREE;
        }
        if (waiting) { pending = true; continue; }
        if (usb_host_device_close(s_cl, d.h) != ESP_OK || leaked)
            NV_LOGW(TAG, "dev %u: closed with leaked resources", d.addr);
        d.used = false;
    }
    return pending;
}

// ------------------------------------------------------------------ client task

void client_event_cb(const usb_host_client_event_msg_t *m, void *) {
    if (m->event == USB_HOST_CLIENT_EVENT_NEW_DEV) {
        if (s_n_new < (int)sizeof s_new) s_new[s_n_new++] = m->new_dev.address;
        else s_rescan = true;
    } else if (m->event == USB_HOST_CLIENT_EVENT_DEV_GONE) {
        for (Dev &d : s_dev)
            if (d.used && d.h == m->dev_gone.dev_hdl) { d.gone = true; d.want_close = true; }
    }
}

void queue_bus_scan(void) {
    uint8_t addrs[16];
    int n = 0;
    s_rescan = false;
    if (usb_host_device_addr_list_fill(sizeof addrs, addrs, &n) != ESP_OK) return;
    for (int i = 0; i < n && s_n_new < (int)sizeof s_new; i++) s_new[s_n_new++] = addrs[i];
    if (n > (int)sizeof s_new) s_rescan = true;
}

// Everything the callbacks asked for. Returns true when the loop should come back soon.
bool service(void) {
    if (s_rescan && s_n_new == 0) queue_bus_scan();
    while (s_n_new > 0) dev_attach(s_new[--s_n_new]);

    for (int di = 0; di < kMaxDevs; di++)
        if (s_dev[di].used && s_dev[di].want_close && !s_dev[di].closing) dev_close_begin(di);

    bool soon = false;
    int count = 0;
    for (Port &p : s_port) {
        if (p.state != P_ACTIVE) continue;
        if (p.in_recover && !p.in_busy) {
            p.in_recover = false;
            recover_ep(p, p.ep_in);
            if (!submit_in(p)) {
                NV_LOGW(TAG, "dev %u if %u: IN resubmit failed -> drop", s_dev[p.dev].addr, p.iface);
                s_dev[p.dev].want_close = true;
                soon = true;
            }
        }
        pump_out(p);
        // Work that could not go out now (endpoint busy): the completion wakes us, but a failed
        // submit has no completion — poll briefly instead of waiting a second.
        if (p.out_recover || (!p.out_busy && p.ep_out &&
                              (p.need_led || p.need_ack || p.need_presence || p.need_setif ||
                               p.init_step < kOneInitN)))
            soon = true;
        if (p.slot >= 0) count++;
    }
    s_count = count;
    return dev_close_progress() || soon;
}

void client_task(void *) {
    usb_host_client_config_t cfg = {};
    cfg.is_synchronous = false;
    cfg.max_num_event_msg = 8;
    cfg.async.client_event_callback = client_event_cb;
    cfg.async.callback_arg = nullptr;
    // usb_host_install is owned by nv_usb_audio: retry until the library is up.
    while (usb_host_client_register(&cfg, &s_cl) != ESP_OK) vTaskDelay(pdMS_TO_TICKS(500));
    // Devices enumerated before we registered never produce NEW_DEV for us: walk the bus once.
    queue_bus_scan();
    NV_LOGI(TAG, "XInput host ready (Xbox 360 / 360 Wireless / One / Series / original Xbox)");
    bool soon = true;
    for (;;) {
        usb_host_client_handle_events(s_cl, pdMS_TO_TICKS(soon ? 20 : 1000));
        soon = service();
    }
}

}  // namespace

// ------------------------------------------------------------------ public API

bool nv_xinput_init(void) {
    if (s_started) return true;
    // Forever daemon that never touches flash -> PSRAM stack (internal SRAM is scarce).
    if (xTaskCreateWithCaps(client_task, "xinput", 4096, nullptr, 5, nullptr,
                            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) return false;
    s_started = true;
    return true;
}

int nv_xinput_count(void) { return s_count; }
