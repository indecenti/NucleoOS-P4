// nv_wake core: decimation, state machine and end-of-question detection. Pure C (no ESP-IDF), so
// the host tests drive it with a fake detector; the device glue (nv_wake.c) only adds the mic tap,
// NVS settings and the UI hand-off.
#include "wake_core.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ---- 48 kHz -> 16 kHz ------------------------------------------------------------------------------
// 15-tap windowed-sinc low-pass (cutoff ~7 kHz, under the 8 kHz Nyquist of the 16 kHz output), Q15.
#define DEC_TAPS 15
static int32_t s_fir[DEC_TAPS];
static bool s_fir_ok;

static void fir_init(void)
{
    if (s_fir_ok) return;
    const double fc = 7000.0 / 48000.0;
    double h[DEC_TAPS], sum = 0;
    for (int i = 0; i < DEC_TAPS; i++) {
        const int m = i - DEC_TAPS / 2;
        const double sinc = m == 0 ? 2 * fc : sin(2 * M_PI * fc * m) / (M_PI * m);
        const double win = 0.54 - 0.46 * cos(2 * M_PI * i / (DEC_TAPS - 1));
        h[i] = sinc * win; sum += h[i];
    }
    for (int i = 0; i < DEC_TAPS; i++) s_fir[i] = (int32_t)lround(h[i] / sum * 32768.0);
    s_fir_ok = true;
}

void wake_decim_reset(wake_decim_t *d) { memset(d, 0, sizeof *d); fir_init(); }

int wake_decim_run(wake_decim_t *d, const int16_t *in, int n, int16_t *out)
{
    fir_init();
    int o = 0;
    for (int i = 0; i < n; i++) {
        memmove(d->hist, d->hist + 1, (DEC_TAPS - 1) * sizeof d->hist[0]);   // last DEC_TAPS inputs
        d->hist[DEC_TAPS - 1] = in[i];
        if (++d->phase < 3) continue;
        d->phase = 0;
        int64_t acc = 0;
        for (int k = 0; k < DEC_TAPS; k++) acc += (int64_t)s_fir[k] * d->hist[k];
        acc >>= 15;
        out[o++] = (int16_t)(acc > 32767 ? 32767 : acc < -32768 ? -32768 : acc);
    }
    return o;
}

// ---- state machine ----------------------------------------------------------------------------------
static void set_reason(wake_core_t *w, const char *it, const char *en)
{
    snprintf(w->reason_it, sizeof w->reason_it, "%s", it);
    snprintf(w->reason_en, sizeof w->reason_en, "%s", en);
}

static void close_det(wake_core_t *w)
{
    if (w->opened && w->be && w->be->close) w->be->close();
    w->opened = false;
    free(w->frame); w->frame = NULL; w->fill = w->chunk = 0;
}

void wake_core_config(wake_core_t *w, bool enabled, const char *word, int sens)
{
    if (sens < 0) sens = 0;
    if (sens > 2) sens = 2;
    const bool same = w->opened && enabled && sens == w->sens && word && !strcmp(word, w->word);
    w->enabled = enabled; w->sens = sens;
    if (w->cool_ms <= 0) w->cool_ms = 2500;
    w->nwords = (w->be && w->be->list) ? w->be->list(w->words, w->labels, NV_WAKE_MAX_WORDS) : 0;
    if (same) return;
    close_det(w);
    set_reason(w, "", "");
    if (!enabled) { w->state = NV_WAKE_OFF; return; }
    w->state = NV_WAKE_UNAVAILABLE;
    if (!w->be) { set_reason(w, "questa build non ha il riconoscimento della parola di attivazione (ESP-SR)",
                                "this build has no wake-word detector (ESP-SR)"); return; }
    if (!w->mic_ok) { set_reason(w, "microfono non disponibile", "microphone not available"); return; }
    if (w->nwords <= 0) { set_reason(w, "nessun modello di parola di attivazione installato",
                                     "no wake-word model installed"); return; }
    // the chosen word, else the first installed one
    int pick = 0;
    for (int i = 0; word && i < w->nwords; i++) if (!strcmp(w->words[i], word)) pick = i;
    snprintf(w->word, sizeof w->word, "%s", w->words[pick]);
    if (!w->be->open(w->word, sens)) { set_reason(w, "modello non caricato (memoria?)", "model failed to load (memory?)"); return; }
    w->chunk = w->be->chunk();
    w->frame = (w->chunk > 0 && w->chunk <= 8192) ? (int16_t *)malloc((size_t)w->chunk * sizeof(int16_t)) : NULL;
    if (!w->frame) { if (w->be->close) w->be->close(); set_reason(w, "memoria insufficiente", "out of memory"); return; }
    w->opened = true; w->fill = 0;
    wake_decim_reset(&w->dec);
    w->state = NV_WAKE_LISTENING;
}

void wake_core_tick(wake_core_t *w, int64_t now_ms)
{
    w->now_ms = now_ms;
    if (w->state == NV_WAKE_HEARD && now_ms - w->last_ms >= w->cool_ms) w->state = NV_WAKE_LISTENING;
}

bool wake_core_feed(wake_core_t *w, const int16_t *pcm48, int n, bool busy)
{
    if (!w->opened) return false;
    if (busy) {                                   // mic or speaker in use: drop, restart clean after
        if (w->state == NV_WAKE_LISTENING) w->state = NV_WAKE_PAUSED;
        w->fill = 0; wake_decim_reset(&w->dec);
        return false;
    }
    if (w->state == NV_WAKE_PAUSED) w->state = NV_WAKE_LISTENING;
    int16_t tmp[512];
    bool heard = false;
    while (n > 0) {
        const int take = n > 1536 ? 1536 : n;      // 1536 in -> 512 out
        const int got = wake_decim_run(&w->dec, pcm48, take, tmp);
        pcm48 += take; n -= take;
        for (int i = 0; i < got; i++) {
            w->frame[w->fill++] = tmp[i];
            if (w->fill < w->chunk) continue;
            w->fill = 0;
            if (w->state == NV_WAKE_LISTENING && w->be->detect(w->frame)) {
                w->state = NV_WAKE_HEARD; w->triggers++; w->last_ms = w->now_ms;
                heard = true;
            }
        }
    }
    return heard;
}

void wake_core_free(wake_core_t *w) { close_det(w); }

// ---- end of the question ----------------------------------------------------------------------------
// The level is nv_audio's meter (0..100 over -54..0 dBFS). The room's floor is learned in the first
// 300 ms; speech is anything clearly above it. Done after ~1.1 s of quiet once someone spoke.
void nv_vad_reset(nv_vad_state_t *v) { memset(v, 0, sizeof *v); v->floor = 100; }

nv_vad_t nv_vad_step(nv_vad_state_t *v, int level, int dt_ms)
{
    v->ms += dt_ms;
    if (v->ms <= 300 && level < v->floor) v->floor = level;
    int thr = (v->floor >= 100 ? level : v->floor) + 14;
    if (thr < 28) thr = 28;
    if (level >= thr) { v->speech_ms += dt_ms; v->quiet_ms = 0; if (v->speech_ms >= 150) v->spoke = true; }
    else v->quiet_ms += dt_ms;
    if (v->spoke && v->quiet_ms >= 1100) return NV_VAD_DONE;
    if (!v->spoke && v->ms >= 5000) return NV_VAD_NOTHING;
    if (v->ms >= 12000) return v->spoke ? NV_VAD_DONE : NV_VAD_NOTHING;
    return NV_VAD_GO;
}
