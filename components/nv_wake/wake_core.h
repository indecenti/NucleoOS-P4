// nv_wake internals shared by the core (host-tested) and the device glue.
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "nv_wake.h"

#ifdef __cplusplus
extern "C" {
#endif

// A wake-word detector backend (ESP-SR WakeNet on the device, a fake in the host tests).
typedef struct {
    int  (*list)(char names[][32], char labels[][32], int max);   // installed wake words
    bool (*open)(const char *name, int sensitivity);               // load one; false on failure
    int  (*chunk)(void);                                           // 16 kHz samples per detect()
    bool (*detect)(const int16_t *pcm16k);                         // true = wake word heard
    void (*close)(void);
} wake_backend_t;

// 48 kHz -> 16 kHz decimator (low-pass FIR + keep every 3rd sample), streaming across calls.
typedef struct { int16_t hist[16]; int phase; } wake_decim_t;
void wake_decim_reset(wake_decim_t *d);
int  wake_decim_run(wake_decim_t *d, const int16_t *in48, int n, int16_t *out16);   // returns samples out

typedef struct {
    const wake_backend_t *be;
    bool enabled, opened, mic_ok;
    int  sens;
    char word[32];
    int  nwords;
    char words[NV_WAKE_MAX_WORDS][32], labels[NV_WAKE_MAX_WORDS][32];
    nv_wake_state_t state;
    uint32_t triggers;
    int64_t  last_ms, now_ms;               // clock in ms (the caller advances now_ms)
    int      cool_ms;                       // HEARD -> LISTENING after this
    wake_decim_t dec;
    int16_t *frame; int fill, chunk;        // detector frame being assembled
    char reason_it[96], reason_en[96];
} wake_core_t;

// Apply the settings (enabled/word/sensitivity): (re)opens the detector as needed.
void wake_core_config(wake_core_t *w, bool enabled, const char *word, int sens);
// Feed raw 48 kHz mic samples. `busy` = the mic/speaker is in use for something else (paused).
// Returns true exactly when the wake word was just heard.
bool wake_core_feed(wake_core_t *w, const int16_t *pcm48, int n, bool busy);
void wake_core_tick(wake_core_t *w, int64_t now_ms);
void wake_core_free(wake_core_t *w);

#ifdef __cplusplus
}
#endif
