// nv_wasm_w4 — WASM-4 fantasy-console compatibility for nv_wasm (private to the component).
//
// A WASM-4 cart (https://wasm4.org) is a .wasm module that imports its 64 KB memory and a small
// "env" drawing/sound API, and exports update() (called 60 times a second) plus an optional
// start(). The console state lives at fixed addresses in that memory: palette, draw colors,
// gamepads, mouse, system flags and a 160x160 2-bit framebuffer. nv_wasm runs the frame loop on
// its worker; this module supplies the env imports (the vendored WASM-4 rasterizer and APU in
// w4/), the input mapping (touch gamepad + mouse) and the upscaled render into the game canvas.
//
// Canvas layout (the game view is a 1024x600 canvas at the panel origin): the 160x160 screen is
// drawn 3x (480x480) centred; a D-pad sits in the left margin and the X / Z buttons in the right.
#pragma once

#include <stddef.h>
#include <stdint.h>
#include "wasm_export.h"

constexpr int kW4CanvasW = 1024, kW4CanvasH = 600;
constexpr int kW4Scale = 3, kW4Side = 160 * kW4Scale;                 // 480 px
constexpr int kW4X0 = (kW4CanvasW - kW4Side) / 2, kW4Y0 = (kW4CanvasH - kW4Side) / 2;

// Registers the "env" imports carts use and starts the (idle) audio task. Once, after
// wasm_runtime_full_init. The imports only act for the cart bound by nv_w4_begin().
bool nv_w4_init(void);

// Carts built with the official (wasi-sdk reactor) template export _initialize but IMPORT their
// memory, as the WASM-4 spec requires; with LIBC_WASI enabled, WAMR's interpreter loader rejects
// any _initialize/_start module that doesn't export memory. Call on the mutable .wasm bytes before
// wasm_runtime_load: renames those two exports in place (same length) to kW4Init / kW4Start, which
// the frame loop runs itself. No-op for anything that isn't a well-formed .wasm (e.g. app.aot).
void nv_w4_prepare_module(uint8_t *buf, uint32_t len);
constexpr const char *kW4Init  = "__w4initial";   // was "_initialize"
constexpr const char *kW4Start = "__w4st";        // was "_start"

// Binds a freshly instantiated cart: checks its memory is exactly 64 KB, sets the console's
// power-on state (palette, draw colors, mouse), loads its save disk and opens the audio stream.
bool nv_w4_begin(wasm_module_inst_t inst, const char *app_id, char *err, size_t err_n);
void nv_w4_end(void);

// Touches in canvas coordinates -> GAMEPAD1 + MOUSE_* in cart memory. Call before update().
void nv_w4_input(const int *xs, const int *ys, int n);

// Frame bracket around the cart's update(): clears the framebuffer (unless the cart set
// SYSTEM_PRESERVE_FRAMEBUFFER, or this is the first frame, which runs start() instead), then
// advances the APU envelope clock.
void nv_w4_frame_begin(bool first);
void nv_w4_frame_end(void);

// Draw into the 1024x600 RGB565 canvas. Each returns true (and the rect to re-blit, x/y/w/h) when
// it changed pixels: the game area only when the framebuffer or palette changed since the last
// call, the gamepad art on the first call and when the cart toggles its overlay flag.
bool nv_w4_render(uint16_t *canvas, bool force, int rect[4]);
bool nv_w4_render_overlay(uint16_t *canvas, bool force, int rect[4]);
