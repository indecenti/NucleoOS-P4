// opl_nv.c — Chocolate Doom's OPL API (opl.h) on a software OPL2: emu8950, no threads.
//
// Chocolate's i_oplmusic.c drives the chip through opl.h: register writes plus timed callbacks
// (one per MIDI event). Upstream runs the emulator on the SDL audio thread; here the single app
// thread pulls samples with nv_opl_render() from the audio pump (nv_sound.c) and the callbacks
// fire when the rendered sample clock reaches them, exactly like opl_sdl.c without the mutexes.
// The chip runs at its native rate (3.58 MHz / 72 = 49716 Hz, emu8950 has no rate converter
// then) and is box-filtered down to the output rate in 16.16 fixed point.
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "opl.h"
#include "opl_queue.h"
#include "emu8950.h"
#include "nv_doom.h"

#if EMU8950_LINEAR
void OPL_calc_buffer_linear(OPL *opl, int32_t *buffer, uint32_t nsamples);
#endif

#define OPL_CLOCK 3579552u
#define OPL_RATE  (OPL_CLOCK / 72u)     // 49716

static OPL *s_chip;
static opl_callback_queue_t *s_queue;
static uint64_t s_now_us;               // chip time, advanced by rendered samples
static uint64_t s_frac_us;              // sub-microsecond remainder (units of 1/OPL_RATE us)
static int s_paused;
static uint64_t s_pause_offset;
static int s_reg_addr;
static unsigned s_out_rate = 22050;
static uint32_t s_step;                 // chip samples per output sample, 16.16
static uint32_t s_pos;                  // fractional chip position, 16.16
static int16_t s_chipbuf[512];
static int s_chipbuf_n, s_chipbuf_i;

opl_init_result_t OPL_Init(unsigned int port_base) {
    (void)port_base;
    if (!s_chip) s_chip = OPL_new(OPL_CLOCK, OPL_RATE);
    if (!s_queue) s_queue = OPL_Queue_Create();
    if (!s_chip || !s_queue) return OPL_INIT_NONE;
    OPL_reset(s_chip);
    OPL_Queue_Clear(s_queue);
    s_now_us = s_frac_us = 0;
    s_paused = 0;
    s_pause_offset = 0;
    s_chipbuf_n = s_chipbuf_i = 0;
    s_pos = 0;
    OPL_SetSampleRate(s_out_rate);
    return OPL_INIT_OPL2;               // i_oplmusic then uses the 9 OPL2 voices only
}

void OPL_Shutdown(void) {
    if (s_queue) OPL_Queue_Clear(s_queue);
}

void OPL_SetSampleRate(unsigned int rate) {
    s_out_rate = rate ? rate : 22050;
    s_step = (uint32_t)(((uint64_t)OPL_RATE << 16) / s_out_rate);
}

void OPL_WritePort(opl_port_t port, unsigned int value) {
    if (port == OPL_REGISTER_PORT) s_reg_addr = (int)(value & 0xff);
    else if (port == OPL_REGISTER_PORT_OPL3) s_reg_addr = (int)((value & 0xff) | 0x100);
    else if (port == OPL_DATA_PORT) OPL_WriteRegister(s_reg_addr, (int)value);
}

unsigned int OPL_ReadPort(opl_port_t port) {
    (void)port;
    return 0;   // no timers emulated: the music player never polls the status
}

unsigned int OPL_ReadStatus(void) { return OPL_ReadPort(OPL_REGISTER_PORT); }

void OPL_WriteRegister(int reg, int value) {
    if (!s_chip || reg > 0xff) return;  // OPL2: the OPL3 bank does not exist
    OPL_writeReg(s_chip, (uint32_t)reg, (uint8_t)value);
}

opl_init_result_t OPL_Detect(void) { return s_chip ? OPL_INIT_OPL2 : OPL_INIT_NONE; }

void OPL_InitRegisters(int opl3) {
    (void)opl3;
    for (int r = OPL_REGS_TREMOLO; r <= OPL_REGS_TREMOLO + OPL_NUM_OPERATORS; ++r) OPL_WriteRegister(r, 0x00);
    for (int r = OPL_REGS_LEVEL; r <= OPL_REGS_LEVEL + OPL_NUM_OPERATORS; ++r) OPL_WriteRegister(r, 0x3f);
    for (int r = OPL_REGS_ATTACK; r <= OPL_REGS_WAVEFORM + OPL_NUM_OPERATORS; ++r) OPL_WriteRegister(r, 0x00);
    for (int r = 1; r < OPL_REGS_TREMOLO; ++r) OPL_WriteRegister(r, 0x00);
    OPL_WriteRegister(OPL_REG_TIMER_CTRL, 0x60);
    OPL_WriteRegister(OPL_REG_TIMER_CTRL, 0x80);
    OPL_WriteRegister(OPL_REG_WAVEFORM_ENABLE, 0x20);   // OPL2 waveforms
}

void OPL_SetCallback(uint64_t us, opl_callback_t callback, void *data) {
    if (s_queue) OPL_Queue_Push(s_queue, callback, data, s_now_us - s_pause_offset + us);
}

void OPL_AdjustCallbacks(float factor) {
    if (s_queue) OPL_Queue_AdjustCallbacks(s_queue, s_now_us - s_pause_offset, factor);
}

void OPL_ClearCallbacks(void) {
    if (s_queue) OPL_Queue_Clear(s_queue);
}

void OPL_Lock(void) {}
void OPL_Unlock(void) {}
void OPL_Delay(uint64_t us) { (void)us; }

void OPL_SetPaused(int paused) { s_paused = paused; }

// Fire every callback due at the current chip time.
static void run_callbacks(void) {
    opl_callback_t cb;
    void *data;
    while (!s_paused && !OPL_Queue_IsEmpty(s_queue) &&
           OPL_Queue_Peek(s_queue) <= s_now_us - s_pause_offset) {
        if (!OPL_Queue_Pop(s_queue, &cb, &data)) break;
        cb(data);
    }
}

// Render `n` chip samples into s_chipbuf, stopping early at the next callback so its register
// writes land on the right sample. Returns the number rendered (>= 1).
static int render_chip(int n) {
    run_callbacks();
    if (!s_paused && !OPL_Queue_IsEmpty(s_queue)) {
        const uint64_t next = OPL_Queue_Peek(s_queue) + s_pause_offset;
        if (next > s_now_us) {
            const uint64_t until = ((next - s_now_us) * OPL_RATE + 999999u) / 1000000u;
            if (until < (uint64_t)n) n = until ? (int)until : 1;
        } else {
            n = 1;
        }
    }
#if EMU8950_LINEAR
    // rp2040-doom's block renderer: one pass per slot over the whole block, silent slots skipped
    static int32_t lin[sizeof s_chipbuf / sizeof s_chipbuf[0]];
    OPL_calc_buffer_linear(s_chip, lin, (uint32_t)n);
    for (int i = 0; i < n; i++) {
        const int32_t v = lin[i] >> 1;
        s_chipbuf[i] = (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
    }
#else
    OPL_calc_buffer(s_chip, s_chipbuf, (uint32_t)n);
#endif
    // advance the clock by n / OPL_RATE seconds without drifting
    s_frac_us += (uint64_t)n * 1000000u;
    const uint64_t dt = s_frac_us / OPL_RATE;
    s_frac_us %= OPL_RATE;
    s_now_us += dt;
    if (s_paused) s_pause_offset += dt;  // paused: the queue stays frozen while the chip rings out
    return n;
}

// Mix `frames` output samples of music (mono) into `acc` (stereo int32, L/R interleaved).
void nv_opl_render(int32_t *acc, int frames, int volume_q8) {
    if (!s_chip || !s_queue) return;
    for (int i = 0; i < frames; i++) {
        // box filter: average the chip samples that fall into this output sample
        int32_t sum = 0;
        int cnt = 0;
        s_pos += s_step;
        while (s_pos >= 0x10000) {
            if (s_chipbuf_i >= s_chipbuf_n) {
                s_chipbuf_n = render_chip((int)(sizeof s_chipbuf / sizeof s_chipbuf[0]));
                s_chipbuf_i = 0;
            }
            sum += s_chipbuf[s_chipbuf_i++];
            cnt++;
            s_pos -= 0x10000;
        }
        if (cnt) {
            const int32_t v = (sum / cnt) * volume_q8 >> 8;
            acc[2 * i] += v;
            acc[2 * i + 1] += v;
        }
    }
}
