// hidpad_test — PC unit test of the HID gamepad pipeline: components/nv_hal/nv_hid_gamepad.c (report
// descriptor parser + report decoder) and nv_pad_map.c (SDL GameController mappings: compiled DB,
// SDL mapping strings, heuristic, apply). Descriptors are written out item by item after the HID
// 1.11 spec, covering the shapes real pads use: no report ID + stick, a DualShock 4 (report ID, hat,
// Rx/Ry triggers), an Xbox Wireless Controller over BLE (16-bit sticks, Simulation-page triggers,
// 1-based hat with a null state), D-pad usages; a keyboard must be refused. Run (WSL/Linux):
//   gcc -O2 -Wall -Wextra -Itools/hidpad_test/shim -Icomponents/nv_hal/include -Icomponents/nv_hal
//       -DNV_PADS_TXT='"/nonexistent"' tools/hidpad_test/test.c components/nv_hal/nv_hid_gamepad.c
//       components/nv_hal/nv_pad_map.c components/nv_hal/nv_paddb.c -o /tmp/hidpad && /tmp/hidpad
#include "nv_hid_gamepad.h"

#include <stdio.h>
#include <string.h>

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

static nv_pad_input_t run(const nv_hid_pad_layout_t *l, const nv_pad_map_t *m, const uint8_t *r, size_t n) {
    nv_hid_raw_t raw;
    nv_pad_input_t in;
    memset(&in, 0xAA, sizeof in);
    CHECK(nv_hid_pad_decode(l, r, n, &raw));
    nv_pad_map_apply(m, &raw, &in);
    return in;
}

int main(void) {
    nv_hid_pad_layout_t l;
    nv_pad_map_t m;
    nv_hid_raw_t raw;

    // 1. Generic gamepad, no report ID: 8-bit X/Y 0..255, 10 buttons, 6 bits padding. Unknown VID:
    //    heuristic mapping (b0 = A, b1 = B, b8 = Back, b9 = Start).
    static const uint8_t generic[] = {
        0x05, 0x01, 0x09, 0x05, 0xa1, 0x01,               // Desktop / Gamepad / Application
        0x15, 0x00, 0x26, 0xff, 0x00,                     //   logical 0..255
        0x09, 0x30, 0x09, 0x31, 0x75, 0x08, 0x95, 0x02, 0x81, 0x02,   // X, Y: 2 x 8 bits
        0x05, 0x09, 0x19, 0x01, 0x29, 0x0a,               //   Button 1..10
        0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x0a, 0x81, 0x02,
        0x75, 0x06, 0x95, 0x01, 0x81, 0x03,               //   padding (constant)
        0xc0 };
    CHECK(nv_hid_pad_parse(generic, sizeof generic, &l));
    CHECK(l.report_id == 0 && l.n_buttons == 10 && l.n_axes == 2 && l.n_hats == 0);
    CHECK(l.axis[0].size == 8 && l.axis[1].off == 8 && l.axis_code[0] == 0 && l.axis_code[1] == 1);
    CHECK(!nv_hid_pad_map(NV_PAD_BUS_USB, 0x1234, 0x5678, &l, &m));   // not in the DB
    { const uint8_t r[] = { 0x80, 0x7f, 0x00, 0x00 }; nv_pad_input_t in = run(&l, &m, r, sizeof r);
      CHECK(in.buttons == 0 && nv_pad_dirs(&in) == 0 && in.axis[NV_PADA_LX] > -300 && in.axis[NV_PADA_LX] < 300); }
    { const uint8_t r[] = { 0x00, 0xff, 0x01, 0x00 }; nv_pad_input_t in = run(&l, &m, r, sizeof r);
      CHECK(nv_pad_dirs(&in) == (NV_PADB_LEFT | NV_PADB_DOWN) && in.buttons == NV_PADB_A);
      CHECK(in.axis[NV_PADA_LX] == -32768 && in.axis[NV_PADA_LY] == 32767 && in.axis[NV_PADA_RX] == 0 && in.axis[NV_PADA_LT] == 0); }
    { const uint8_t r[] = { 0xff, 0x00, 0x00, 0x02 }; nv_pad_input_t in = run(&l, &m, r, sizeof r);
      CHECK(nv_pad_dirs(&in) == (NV_PADB_RIGHT | NV_PADB_UP) && in.buttons == NV_PADB_START); }
    { const uint8_t r[] = { 0x50, 0xb0, 0x00, 0x00 }; nv_pad_input_t in = run(&l, &m, r, sizeof r);
      CHECK(nv_pad_dirs(&in) == 0); }                                                  // inside dead zone
    { const uint8_t r[] = { 0x80, 0x80 }; CHECK(nv_hid_pad_decode(&l, r, sizeof r, &raw)); CHECK(raw.buttons == 0); }  // short

    // 2. DualShock 4 (USB, report 1): X Y Z Rz sticks, 4-bit hat, 14 buttons, vendor bits, Rx Ry
    //    triggers. Mapped by the DB (Windows line: a = b1 cross, x = b0 square, triggers a3 / a4,
    //    right stick a2 / a5).
    static const uint8_t ds4[] = {
        0x05, 0x01, 0x09, 0x05, 0xa1, 0x01, 0x85, 0x01,
        0x09, 0x30, 0x09, 0x31, 0x09, 0x32, 0x09, 0x35, 0x15, 0x00, 0x26, 0xff, 0x00, 0x75, 0x08, 0x95, 0x04, 0x81, 0x02,
        0x09, 0x39, 0x15, 0x00, 0x25, 0x07, 0x35, 0x00, 0x46, 0x3b, 0x01, 0x65, 0x14, 0x75, 0x04, 0x95, 0x01, 0x81, 0x42,
        0x65, 0x00, 0x05, 0x09, 0x19, 0x01, 0x29, 0x0e, 0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x0e, 0x81, 0x02,
        0x06, 0x00, 0xff, 0x09, 0x20, 0x75, 0x06, 0x95, 0x01, 0x15, 0x00, 0x25, 0x7f, 0x81, 0x02,
        0x05, 0x01, 0x09, 0x33, 0x09, 0x34, 0x15, 0x00, 0x26, 0xff, 0x00, 0x75, 0x08, 0x95, 0x02, 0x81, 0x02,
        0x06, 0x00, 0xff, 0x09, 0x21, 0x95, 0x36, 0x81, 0x02,
        0xc0 };
    CHECK(nv_hid_pad_parse(ds4, sizeof ds4, &l));
    CHECK(l.report_id == 1 && l.n_axes == 6 && l.n_hats == 1 && l.n_buttons == 14);
    CHECK(l.axis_code[2] == 2 && l.axis_code[3] == 3 && l.axis_code[4] == 4 && l.axis_code[5] == 5);
    CHECK(nv_hid_pad_map(NV_PAD_BUS_USB, 0x054c, 0x05c4, &l, &m));
    //                       id    LX    LY    RX    RY    hat|btn  btn   cnt   L2    R2
    { const uint8_t r[] = { 0x01, 0x80, 0x80, 0x80, 0x80, 0x08,   0x00, 0x00, 0x00, 0x00 };
      nv_pad_input_t in = run(&l, &m, r, sizeof r);
      CHECK(in.buttons == 0 && in.axis[NV_PADA_LT] == 0 && in.axis[NV_PADA_RT] == 0); }       // hat 8 = centred
    { const uint8_t r[] = { 0x01, 0x80, 0x80, 0xff, 0x00, 0x20 | 0x02, 0x00, 0x00, 0xff, 0x00 };  // cross, hat right, L2
      nv_pad_input_t in = run(&l, &m, r, sizeof r);
      CHECK(in.buttons == (NV_PADB_A | NV_PADB_RIGHT));
      CHECK(in.axis[NV_PADA_RX] == 32767 && in.axis[NV_PADA_RY] == -32768 && in.axis[NV_PADA_LT] == 32767); }
    { const uint8_t r[] = { 0x01, 0x80, 0x80, 0x80, 0x80, 0x10 | 0x08, 0x20, 0x00, 0x00, 0x00 };  // square, options
      nv_pad_input_t in = run(&l, &m, r, sizeof r);
      CHECK(in.buttons == (NV_PADB_X | NV_PADB_START)); }
    { const uint8_t r[] = { 0x11, 0x80 }; CHECK(!nv_hid_pad_decode(&l, r, sizeof r, &raw)); }   // other report ID

    // 3. Xbox Wireless Controller over BLE: 16-bit X Y / Z Rz, Brake + Accelerator 10-bit
    //    (Simulation page), hat 1..8 with null state, 15 buttons. Mapped by the Bluetooth DB line
    //    (A b0, B b1, X b3, Y b4, LB b6, RB b7, View b10, Menu b11, Guide b12; RT a4, LT a5).
    static const uint8_t xbox_ble[] = {
        0x05, 0x01, 0x09, 0x05, 0xa1, 0x01, 0x85, 0x01,
        0x09, 0x01, 0xa1, 0x00, 0x09, 0x30, 0x09, 0x31, 0x15, 0x00, 0x27, 0xff, 0xff, 0x00, 0x00, 0x95, 0x02, 0x75, 0x10, 0x81, 0x02, 0xc0,
        0x09, 0x01, 0xa1, 0x00, 0x09, 0x32, 0x09, 0x35, 0x15, 0x00, 0x27, 0xff, 0xff, 0x00, 0x00, 0x95, 0x02, 0x75, 0x10, 0x81, 0x02, 0xc0,
        0x05, 0x02, 0x09, 0xc5, 0x15, 0x00, 0x26, 0xff, 0x03, 0x95, 0x01, 0x75, 0x0a, 0x81, 0x02,
        0x15, 0x00, 0x25, 0x00, 0x75, 0x06, 0x95, 0x01, 0x81, 0x03,
        0x05, 0x02, 0x09, 0xc4, 0x15, 0x00, 0x26, 0xff, 0x03, 0x95, 0x01, 0x75, 0x0a, 0x81, 0x02,
        0x15, 0x00, 0x25, 0x00, 0x75, 0x06, 0x95, 0x01, 0x81, 0x03,
        0x05, 0x01, 0x09, 0x39, 0x15, 0x01, 0x25, 0x08, 0x35, 0x00, 0x46, 0x3b, 0x01, 0x66, 0x14, 0x00, 0x75, 0x04, 0x95, 0x01, 0x81, 0x42,
        0x75, 0x04, 0x95, 0x01, 0x15, 0x00, 0x25, 0x00, 0x35, 0x00, 0x45, 0x00, 0x65, 0x00, 0x81, 0x03,
        0x05, 0x09, 0x19, 0x01, 0x29, 0x0f, 0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x0f, 0x81, 0x02,
        0x15, 0x00, 0x25, 0x00, 0x75, 0x01, 0x95, 0x01, 0x81, 0x03,
        0xc0 };
    CHECK(nv_hid_pad_parse(xbox_ble, sizeof xbox_ble, &l));
    CHECK(l.report_id == 1 && l.n_axes == 6 && l.n_hats == 1 && l.n_buttons == 15);
    CHECK(l.axis_code[3] == 5 && l.axis_code[4] == 9 && l.axis_code[5] == 10);   // X Y Z Rz Gas Brake
    CHECK(nv_hid_pad_map(NV_PAD_BUS_BT, 0x045e, 0x0b13, &l, &m));
    //   id  LX lo/hi   LY         RX         RY         brake(10b)+pad  accel+pad   hat|pad  buttons
    { const uint8_t r[] = { 0x01, 0xff, 0xff, 0x00, 0x80, 0x00, 0x80, 0x00, 0x80, 0xff, 0x03, 0x00, 0x00, 0x01, 0x01 | (1 << 3), 0x00 };
      nv_pad_input_t in = run(&l, &m, r, sizeof r);
      CHECK(in.axis[NV_PADA_LX] == 32767 && in.axis[NV_PADA_LY] == 0);
      CHECK(in.axis[NV_PADA_LT] == 32767 && in.axis[NV_PADA_RT] == 0);
      CHECK(in.buttons == (NV_PADB_UP | NV_PADB_A | NV_PADB_X)); }                      // hat 1 = up, b0 + b3
    { const uint8_t r[] = { 0x01, 0, 0x80, 0, 0x80, 0, 0x80, 0, 0x80, 0, 0, 0xff, 0x03, 0x00, 0x00, 0x08 };  // RT, Menu (b11)
      nv_pad_input_t in = run(&l, &m, r, sizeof r);
      CHECK(in.axis[NV_PADA_RT] == 32767 && in.buttons == NV_PADB_START); }
    // Same pad, unknown PID: the heuristic recognises the Xbox BLE shape.
    CHECK(!nv_hid_pad_map(NV_PAD_BUS_BT, 0x045e, 0xfff0, &l, &m));
    { const uint8_t r[] = { 0x01, 0, 0x80, 0, 0x80, 0, 0x80, 0, 0x80, 0, 0, 0, 0, 0x00, 0x10, 0x00 };   // b4 = Y
      nv_pad_input_t in = run(&l, &m, r, sizeof r);
      CHECK(in.buttons == NV_PADB_Y); }

    // 4. Report ID + 8-way hat + signed 8-bit stick (-127..127) + D-pad usages instead of a hat.
    static const uint8_t dpad[] = {
        0x05, 0x01, 0x09, 0x04, 0xa1, 0x01, 0x85, 0x03,   // Joystick, report 3
        0x15, 0x81, 0x25, 0x7f, 0x09, 0x30, 0x09, 0x31, 0x75, 0x08, 0x95, 0x02, 0x81, 0x02,
        0x15, 0x00, 0x25, 0x01, 0x09, 0x90, 0x09, 0x91, 0x09, 0x92, 0x09, 0x93, 0x75, 0x01, 0x95, 0x04, 0x81, 0x02,
        0x05, 0x09, 0x09, 0x01, 0x09, 0x02, 0x95, 0x02, 0x81, 0x02, 0x75, 0x02, 0x95, 0x01, 0x81, 0x03,
        0xc0 };
    CHECK(nv_hid_pad_parse(dpad, sizeof dpad, &l));
    CHECK(l.report_id == 3 && l.n_hats == 1 && !l.hat[0].size && l.dpad[0].size == 1 && l.n_buttons == 2);
    nv_hid_pad_map(NV_PAD_BUS_USB, 0x1234, 0x0001, &l, &m);
    { const uint8_t r[] = { 0x03, 0x81, 0x00, 0x01 | 0x04 | 0x20 };   // left, up + right D-pad, button 2
      nv_pad_input_t in = run(&l, &m, r, sizeof r);
      CHECK(in.buttons == (NV_PADB_UP | NV_PADB_RIGHT | NV_PADB_B) && in.axis[NV_PADA_LX] == -32768); }

    // 5. A boot keyboard is not a gamepad.
    static const uint8_t kbd[] = {
        0x05, 0x01, 0x09, 0x06, 0xa1, 0x01, 0x05, 0x07, 0x19, 0xe0, 0x29, 0xe7, 0x15, 0x00, 0x25, 0x01,
        0x75, 0x01, 0x95, 0x08, 0x81, 0x02, 0x95, 0x01, 0x75, 0x08, 0x81, 0x01, 0xc0 };
    CHECK(!nv_hid_pad_parse(kbd, sizeof kbd, &l));

    // 6. SDL mapping strings: half axes, inverted axes, hats, unknown keys.
    CHECK(nv_pad_map_parse("a:b1,b:b2,x:b0,y:b3,dpup:-a1,lefttrigger:+a2,righttrigger:a5~,leftx:a0,back:h0.8,crc:1234,platform:Windows", &m));
    CHECK(m.btn[0] == 1 && m.btn[11] == (0x40 | 2 << 4 | 1) && m.axis[NV_PADA_LT] == (0x40 | 1 << 4 | 2));
    CHECK(m.axis[NV_PADA_RT] == (0x40 | 3 << 4 | 5) && m.btn[4] == (0x80 | 8) && m.axis[NV_PADA_RY] == 0xFF);
    memset(&raw, 0, sizeof raw);
    raw.axis[1] = -30000; raw.axis[2] = 32767; raw.axis[5] = -32768; raw.hat[0] = 8;
    { nv_pad_input_t in; nv_pad_map_apply(&m, &raw, &in);
      CHECK(in.buttons == (NV_PADB_UP | NV_PADB_BACK) && in.axis[NV_PADA_LT] == 32767 && in.axis[NV_PADA_RT] == 32767); }
    CHECK(!nv_pad_map_parse("leftx:a0,lefty:a1", &m));                                  // no face buttons

    printf(fails ? "hidpad_test: %d FAILED\n" : "hidpad_test: all passed\n", fails);
    return fails != 0;
}
