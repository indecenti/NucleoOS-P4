// amy_port.c - AMY synth engine compiled into the Synth app as ONE translation unit, plus the
// platform glue AMY expects from its i2s.c / MIDI driver layer (none of which exists here: the app
// pulls blocks itself with amy_execute_deltas + amy_render + amy_fill_buffer and feeds nv_audio).
//
// AMY: https://github.com/shorepine/amy (MIT, see amy/LICENSE), vendored at commit f555bc7. Only
// the engine sources are in amy/ (no miniaudio, i2s, MIDI device drivers, sequencer threads).
#define AMY_NO_MINIAUDIO           // no audio device layer: the app pulls blocks itself
#pragma clang diagnostic ignored "-Wunused-parameter"
#pragma clang diagnostic ignored "-Wunused-function"
#pragma clang diagnostic ignored "-Wunused-variable"
#pragma clang diagnostic ignored "-Wunused-but-set-variable"
#pragma clang diagnostic ignored "-Wsign-compare"
#pragma clang diagnostic ignored "-Wmissing-field-initializers"
#pragma clang diagnostic ignored "-Wimplicit-fallthrough"
#pragma clang diagnostic ignored "-Wtype-limits"
#pragma clang diagnostic ignored "-Wformat"
#pragma clang diagnostic ignored "-Wshift-negative-value"
#pragma clang diagnostic ignored "-Wunused-const-variable"
#pragma clang diagnostic ignored "-Wpointer-sign"
#pragma clang diagnostic ignored "-Wparentheses-equality"
#pragma clang diagnostic ignored "-Wmisleading-indentation"

#include "amy/algorithms.c"
#include "amy/amy.c"
#include "amy/amy_midi.c"
#include "amy/api.c"
#include "amy/custom.c"
#include "amy/cv_trigger.c"
#include "amy/delay.c"
#include "amy/envelope.c"
#include "amy/examples.c"
#include "amy/filters.c"
#include "amy/instrument.c"
#include "amy/interp_partials.c"
#include "amy/log2_exp2.c"
#include "amy/midi_mappings.c"
#include "amy/note_output.c"
#include "amy/oscillators.c"
#include "amy/parse.c"
#include "amy/patches.c"
#include "amy/pcm.c"
#include "amy/sequencer.c"
#include "amy/transfer.c"

// ---- platform layer (normally i2s.c + the per-platform MIDI driver) ------------------------------
void run_midi(void) {}
void stop_midi(void) {}
void delay_ms(uint32_t ms) { (void)ms; }
void amy_platform_init(void) {}
void amy_platform_deinit(void) {}
void amy_update_tasks(void) { amy_execute_deltas(); }
int16_t *amy_render_audio(void) {
    amy_render(0, AMY_OSCS, 0);
    return amy_fill_buffer();
}
size_t amy_i2s_write(const uint8_t *buffer, size_t nbytes) { (void)buffer; return nbytes; }

// Wire string of built-in patch n (the app reads the patch's own EQ from it).
const char *synth_patch_cmd(int n) {
    return (n >= 0 && n < _PATCHES_NUM_BUILTIN) ? patch_commands[n] : "";
}
