// nv_wasm_snd — the game sound mixer (ABI v15). A WASM game used to get one sound at a time: every
// nv.sound streamed its WAV alone, so a jingle cut the splash before it and music could not play under
// the effects. This mixer runs in its own task, apart from the game loop: up to NV_WSND_VOICES voices
// at once — WAVs loaded into PSRAM (effects, loops) or streamed from the SD (music) — each with its
// own volume, pitch, loop and fade, summed through a soft limiter into one 48 kHz mono stream.
//
// The game only sends commands (play / set / stop): a slow frame can never starve the audio, and
// the mixer never waits for the game. Samples are decoded once per app (16-bit PCM WAV, mono or
// stereo, 8..48 kHz) and dropped when the app ends.
//
// Threads: the commands come from the WASM worker; the mixer task owns the output stream. The voice
// table is guarded by one mutex, held only for short copies (never across SD reads or audio writes).
#include "nv_wasm_snd.h"

#include <stdio.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nv_audio.h"
#include "nv_log.h"
#include "nv_mem_attr.h"   // NV_PSRAM_BSS

namespace {

const char *TAG = "wsnd";
constexpr int kRate = 48000;
constexpr int kBlock = 256;                       // samples per mix block (5.3 ms)
constexpr size_t kTargetBytes = kRate / 1000 * 70 * 2;   // keep ~70 ms queued: low latency, no gaps
constexpr int kStreamRing = 32768;                // samples buffered per streamed voice (~0.7 s at 48 kHz)
constexpr size_t kCacheBudget = 8u << 20;         // PSRAM for decoded samples

struct Sample {
    char      path[128];
    int16_t  *pcm;            // mono
    uint32_t  n, rate;
};
constexpr int kMaxSamples = 48;
Sample *s_smp = nullptr;                          // kMaxSamples, allocated (PSRAM) while an app uses the mixer
int    s_nsmp = 0;
size_t s_cache = 0;

struct Voice {                    // all zero at rest (.bss); nv_wsnd_play fills every field
    bool      on;
    uint16_t  gen;
    Sample   *smp;                // in-memory voice
    FILE     *f;                  // streamed voice
    long      data_off, data_end;
    uint16_t  ch;
    uint32_t  rate;
    int16_t  *ring;               // streamed: mono samples at the source rate
    uint32_t  rd, wr;             // ring indices (monotonic)
    bool      eof;
    uint64_t  pos;                // Q16 position in source samples
    uint32_t  step;               // Q16 source samples per output sample (rate x pitch)
    int       vol, vol_t, pitch;
    int       fade;               // > 0: fading out, samples left
    int       fade_len;
    bool      loop;
};
Voice *s_v = nullptr;                             // NV_WSND_VOICES, same lifetime as s_smp
SemaphoreHandle_t s_mx = nullptr;
TaskHandle_t s_task = nullptr;
volatile bool s_run = false, s_paused = false;
int s_master = 256;

// ---- WAV parsing -----------------------------------------------------------------------------------
// Positions f at the samples; fills rate, channels and the data size. PCM 16-bit, 1-2 ch only.
bool wav_open(FILE *f, uint32_t *rate, uint16_t *ch, uint32_t *bytes) {
    uint8_t riff[12];
    if (fread(riff, 1, 12, f) != 12 || memcmp(riff, "RIFF", 4) || memcmp(riff + 8, "WAVE", 4)) return false;
    uint16_t bits = 0;
    *rate = 0; *ch = 0;
    for (;;) {
        uint8_t ck[8];
        if (fread(ck, 1, 8, f) != 8) return false;
        const uint32_t sz = ck[4] | (ck[5] << 8) | (ck[6] << 16) | ((uint32_t)ck[7] << 24);
        if (!memcmp(ck, "fmt ", 4)) {
            uint8_t fm[16];
            if (sz < 16 || fread(fm, 1, 16, f) != 16) return false;
            if ((fm[0] | (fm[1] << 8)) != 1) return false;          // PCM only
            *ch = fm[2] | (fm[3] << 8);
            *rate = fm[4] | (fm[5] << 8) | (fm[6] << 16) | ((uint32_t)fm[7] << 24);
            bits = fm[14] | (fm[15] << 8);
            if (sz > 16) fseek(f, (long)(sz - 16 + (sz & 1)), SEEK_CUR);
        } else if (!memcmp(ck, "data", 4)) {
            *bytes = sz;
            return *ch >= 1 && *ch <= 2 && bits == 16 && *rate >= 8000 && *rate <= 48000;
        } else {
            fseek(f, (long)(sz + (sz & 1)), SEEK_CUR);
        }
    }
}

// Decode a whole WAV into the cache (mono, source rate). Called by the worker (play / preload).
Sample *sample_get(const char *path) {
    for (int i = 0; i < s_nsmp; i++) if (!strcmp(s_smp[i].path, path)) return &s_smp[i];
    if (s_nsmp >= kMaxSamples) return nullptr;
    FILE *f = fopen(path, "rb");
    if (!f) return nullptr;
    uint32_t rate, bytes; uint16_t ch;
    if (!wav_open(f, &rate, &ch, &bytes)) { fclose(f); return nullptr; }
    const uint32_t n = bytes / (2u * ch);
    if (!n || s_cache + n * 2 > kCacheBudget) { fclose(f); NV_LOGW(TAG, "no room for %s", path); return nullptr; }
    int16_t *pcm = (int16_t *)heap_caps_malloc((size_t)n * 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!pcm) { fclose(f); return nullptr; }
    if (ch == 1) {
        if (fread(pcm, 2, n, f) != n) { heap_caps_free(pcm); fclose(f); return nullptr; }
    } else {
        int16_t tmp[512];
        uint32_t done = 0;
        while (done < n) {
            const uint32_t k = n - done < 256 ? n - done : 256;
            if (fread(tmp, 4, k, f) != k) break;
            for (uint32_t i = 0; i < k; i++) pcm[done + i] = (int16_t)(((int)tmp[2 * i] + tmp[2 * i + 1]) / 2);
            done += k;
        }
        if (done < n) { heap_caps_free(pcm); fclose(f); return nullptr; }
    }
    fclose(f);
    Sample *s = &s_smp[s_nsmp++];
    snprintf(s->path, sizeof s->path, "%s", path);
    s->pcm = pcm; s->n = n; s->rate = rate;
    s_cache += (size_t)n * 2;
    return s;
}

uint32_t step_for(uint32_t rate, int pitch) {
    return (uint32_t)(((uint64_t)rate * (uint32_t)pitch << 16) / ((uint64_t)kRate * 256));
}
int handle_of(int i) { return (s_v[i].gen << 4) | i; }
Voice *voice_of(int h) {
    if (h < 0) return nullptr;
    const int i = h & 15;
    if (i >= NV_WSND_VOICES || !s_v[i].on || s_v[i].gen != (uint16_t)(h >> 4)) return nullptr;
    return &s_v[i];
}
void voice_free(Voice *v) {          // under s_mx; the FILE/ring are closed by the mixer (see reap)
    v->on = false;
}

// ---- the mixer task ---------------------------------------------------------------------------------
// Top up a streamed voice's ring from the SD (mixer task, outside the lock: only this task touches
// the stream fields of a voice that is on). At most `chunks` reads of 512 samples per call: a read
// can stall for tens of ms while the game loads assets from the same card, and the output queue
// must never wait behind a whole ring refill (the ring holds ~0.7 s, so small top-ups keep up).
// Returns true if it read something.
bool stream_fill(Voice *v, int chunks) {
    if (!v->f || v->eof) return false;
    int16_t tmp[1024];
    bool got = false;
    while (chunks-- > 0 && v->wr - v->rd < (uint32_t)(kStreamRing - 1024)) {
        long pos = ftell(v->f);
        if (pos >= v->data_end) {
            if (v->loop) { fseek(v->f, v->data_off, SEEK_SET); continue; }
            v->eof = true;
            return got;
        }
        const long left = (v->data_end - pos) / (2 * v->ch);
        const uint32_t k = (uint32_t)(left < 512 ? left : 512);
        if (v->ch == 1) {
            if (fread(tmp, 2, k, v->f) != k) { v->eof = true; return got; }
            for (uint32_t i = 0; i < k; i++) v->ring[(v->wr + i) % kStreamRing] = tmp[i];
        } else {
            if (fread(tmp, 4, k, v->f) != k) { v->eof = true; return got; }
            for (uint32_t i = 0; i < k; i++) v->ring[(v->wr + i) % kStreamRing] = (int16_t)(((int)tmp[2 * i] + tmp[2 * i + 1]) / 2);
        }
        v->wr += k;
        got = true;
    }
    return got;
}

void mix_task(void *) {
    int16_t out[kBlock];                              // on the task's (PSRAM) stack
    int32_t acc[kBlock];
    bool stream = false;
    while (s_run) {
        if (s_paused) {
            if (stream) { nv_audio_pcm_flush(); nv_audio_pcm_end(); stream = false; }
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        bool any = false;
        xSemaphoreTake(s_mx, portMAX_DELAY);
        for (int i = 0; i < NV_WSND_VOICES; i++) any |= s_v[i].on;
        xSemaphoreGive(s_mx);
        // Finished voices: close outside the lock.
        for (int i = 0; i < NV_WSND_VOICES; i++) {
            Voice *v = &s_v[i];
            if (!v->on && (v->f || v->ring)) {
                if (v->f) { fclose(v->f); v->f = nullptr; }
                if (v->ring) { heap_caps_free(v->ring); v->ring = nullptr; }
            }
        }
        if (!any) {                                    // silence: give the DAC back after a while
            static int idle = 0;
            if (stream && ++idle > 100) { nv_audio_pcm_end(); stream = false; idle = 0; }
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        if (!stream) {
            if (!nv_audio_pcm_begin_as(kRate, 1, 16, NV_PCM_SFX)) { vTaskDelay(pdMS_TO_TICKS(50)); continue; }
            stream = true;
        }
        // The output queue first: while it is short, mix. With margin, top the SD streams up a little
        // (a voice whose ring is about to run dry gets a read even then, or it would gap).
        const bool margin = nv_audio_pcm_backlog() > kTargetBytes;
        bool read = false;
        for (int i = 0; i < NV_WSND_VOICES; i++) {
            Voice *v = &s_v[i];
            if (!v->on || !v->f) continue;
            const bool dry = v->wr - v->rd < (uint32_t)(kBlock * 4);
            if (margin || dry) read |= stream_fill(v, margin ? 4 : 1);
        }
        if (margin) { if (!read) vTaskDelay(pdMS_TO_TICKS(3)); continue; }
        memset(acc, 0, sizeof acc);
        xSemaphoreTake(s_mx, portMAX_DELAY);
        for (int i = 0; i < NV_WSND_VOICES; i++) {
            Voice *v = &s_v[i];
            if (!v->on) continue;
            for (int k = 0; k < kBlock; k++) {
                // volume glides toward its target (no clicks), fade-outs ramp to zero
                if (v->vol != v->vol_t) v->vol += v->vol < v->vol_t ? 1 : -1;
                int g = v->vol;
                if (v->fade > 0) { g = g * v->fade / v->fade_len; if (--v->fade == 0) { voice_free(v); break; } }
                const uint32_t ip = (uint32_t)(v->pos >> 16), fr = (uint32_t)(v->pos & 0xFFFF) >> 1;
                int s0, s1;
                if (v->smp) {
                    if (ip + 1 >= v->smp->n) {
                        if (v->loop) { v->pos -= (uint64_t)v->smp->n << 16; continue; }
                        voice_free(v); break;
                    }
                    s0 = v->smp->pcm[ip]; s1 = v->smp->pcm[ip + 1];
                } else {
                    if (ip + 1 >= v->wr) { if (v->eof) { voice_free(v); } break; }   // underrun: wait for the SD
                    s0 = v->ring[ip % kStreamRing]; s1 = v->ring[(ip + 1) % kStreamRing];
                    v->rd = ip;
                }
                const int smp = s0 + (((s1 - s0) * (int)fr) >> 15);           // linear interpolation
                acc[k] += smp * g >> 8;
                v->pos += v->step;
            }
        }
        const int master = s_master;
        xSemaphoreGive(s_mx);
        for (int k = 0; k < kBlock; k++) {             // master gain, then a soft knee above ~50%
            int x = acc[k] * master >> 8;
            const int ax = x < 0 ? -x : x;
            if (ax > 16384) { const int over = ax - 16384; x = (x < 0 ? -1 : 1) * (16384 + over * 8192 / (over + 8192)); }
            out[k] = (int16_t)(x > 24576 ? 24576 : x < -24576 ? -24576 : x);   // peak cap: the amp browns out above
        }
        if (nv_audio_pcm_write(out, sizeof out) < 0) {     // the voice engine took the DAC: start again later
            nv_audio_pcm_end();
            stream = false;
            vTaskDelay(pdMS_TO_TICKS(30));
        }
    }
    if (stream) { nv_audio_pcm_flush(); nv_audio_pcm_end(); }
    s_task = nullptr;
    vTaskDelete(nullptr);
}

bool ensure_task(void) {
    if (!s_mx) s_mx = xSemaphoreCreateMutex();
    if (!s_mx) return false;
    if (!s_v) s_v = (Voice *)heap_caps_calloc(NV_WSND_VOICES, sizeof(Voice), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_smp) s_smp = (Sample *)heap_caps_calloc(kMaxSamples, sizeof(Sample), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_v || !s_smp) return false;
    if (s_task) return true;
    s_run = true;
    // Above the game worker so a busy game can't starve the sound; PSRAM stack (SD + audio only).
    if (xTaskCreatePinnedToCoreWithCaps(mix_task, "wsnd", 8192, nullptr, 6, &s_task, tskNO_AFFINITY,
                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        s_task = nullptr;
        s_run = false;
        return false;
    }
    return true;
}

int voice_alloc(void) {            // under s_mx: a free voice, else the quietest non-looping one
    int best = -1;
    for (int i = 0; i < NV_WSND_VOICES; i++) if (!s_v[i].on && !s_v[i].f && !s_v[i].ring) return i;
    for (int i = 0; i < NV_WSND_VOICES; i++)
        if (s_v[i].on && !s_v[i].loop && !s_v[i].f && (best < 0 || s_v[i].vol < s_v[best].vol)) best = i;
    return best;
}

}  // namespace

extern "C" {

int nv_wsnd_preload(const char *path) { return ensure_task() && sample_get(path) ? 0 : -1; }

int nv_wsnd_play(const char *path, int vol, int pitch, int flags) {
    if (!path || !ensure_task()) return -1;
    vol = vol < 0 ? 0 : vol > 512 ? 512 : vol;
    pitch = pitch < 32 ? 32 : pitch > 1024 ? 1024 : pitch;
    const bool stream = (flags & NV_WSND_STREAM) != 0;
    Sample *smp = nullptr;
    FILE *f = nullptr;
    uint32_t rate = 0, bytes = 0; uint16_t ch = 1;
    long off = 0;
    int16_t *ring = nullptr;
    if (stream) {
        f = fopen(path, "rb");
        if (!f || !wav_open(f, &rate, &ch, &bytes)) { if (f) fclose(f); return -1; }
        off = ftell(f);
        ring = (int16_t *)heap_caps_malloc(kStreamRing * 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!ring) { fclose(f); return -1; }
    } else {
        smp = sample_get(path);
        if (!smp) return -1;
        rate = smp->rate;
    }
    xSemaphoreTake(s_mx, portMAX_DELAY);
    const int i = voice_alloc();
    if (i < 0) { xSemaphoreGive(s_mx); if (f) fclose(f); if (ring) heap_caps_free(ring); return -1; }
    Voice *v = &s_v[i];
    FILE *old_f = v->f; int16_t *old_ring = v->ring;   // a stolen voice's stream (non-stream only here)
    v->gen++;
    v->smp = smp; v->f = f; v->ring = ring; v->ch = ch; v->rate = rate;
    v->data_off = off; v->data_end = off + (long)bytes;
    v->rd = v->wr = 0; v->eof = false; v->pos = 0;
    v->pitch = pitch; v->step = step_for(rate, pitch);
    v->vol = v->vol_t = vol;
    v->fade = v->fade_len = 0;
    v->loop = (flags & NV_WSND_LOOP) != 0;
    v->on = true;
    const int h = handle_of(i);
    xSemaphoreGive(s_mx);
    if (old_f) fclose(old_f);
    if (old_ring) heap_caps_free(old_ring);
    return h;
}

void nv_wsnd_set(int h, int vol, int pitch) {
    if (!s_mx || !s_v) return;
    xSemaphoreTake(s_mx, portMAX_DELAY);
    if (Voice *v = voice_of(h)) {
        if (vol >= 0) v->vol_t = vol > 512 ? 512 : vol;
        if (pitch > 0) { v->pitch = pitch < 32 ? 32 : pitch > 1024 ? 1024 : pitch; v->step = step_for(v->rate, v->pitch); }
    }
    xSemaphoreGive(s_mx);
}

void nv_wsnd_stop(int h, int fade_ms) {
    if (!s_mx || !s_v) return;
    xSemaphoreTake(s_mx, portMAX_DELAY);
    if (h < 0) {                                    // all voices
        for (int i = 0; i < NV_WSND_VOICES; i++)
            if (s_v[i].on) { s_v[i].fade_len = s_v[i].fade = fade_ms > 0 ? kRate / 1000 * fade_ms : 1; }
    } else if (Voice *v = voice_of(h)) {
        v->fade_len = v->fade = fade_ms > 0 ? kRate / 1000 * fade_ms : 1;
    }
    xSemaphoreGive(s_mx);
}

void nv_wsnd_master(int vol) { s_master = vol < 0 ? 0 : vol > 512 ? 512 : vol; }

void nv_wsnd_pause(bool on) { s_paused = on; }

bool nv_wsnd_active(void) { return s_task != nullptr; }

// App teardown: stop the task, free every voice and the sample cache.
void nv_wsnd_shutdown(void) {
    if (s_task) {
        s_run = false;
        for (int i = 0; i < 100 && s_task; i++) vTaskDelay(pdMS_TO_TICKS(5));
    }
    if (s_task) return;                             // still draining: leave the memory to the next call
    if (!s_v || !s_smp) return;
    for (int i = 0; i < NV_WSND_VOICES; i++) {
        Voice *v = &s_v[i];
        v->on = false;
        if (v->f) { fclose(v->f); v->f = nullptr; }
        if (v->ring) { heap_caps_free(v->ring); v->ring = nullptr; }
    }
    for (int i = 0; i < s_nsmp; i++) { heap_caps_free(s_smp[i].pcm); s_smp[i].pcm = nullptr; }
    s_nsmp = 0;
    s_cache = 0;
    s_master = 256;
    s_paused = false;
    heap_caps_free(s_v); s_v = nullptr;
    heap_caps_free(s_smp); s_smp = nullptr;
}

}  // extern "C"
