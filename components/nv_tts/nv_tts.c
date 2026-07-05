// nv_tts — OS-wide offline voice for NucleoV2, ported from G:\Nucleo (nucleo_tts).
//
// Concatenative TTS: a per-language dictionary on SD (/sdcard/data/tts/<lang>/index.bin +
// clips.pcm, format NTI1 = sorted slug->offset/len + one big mono 16-bit PCM blob @24kHz). Text is
// PLANNED (nucleo_tts_plan: normalize, expand numbers to cardinals, phrase/word match, spell
// fallback) into CLIP/PAUSE tokens, then a worker task STREAMS the clip PCM slices straight to the
// codec via nv_audio's PCM path — no temp WAV, RAM ~zero. Any native app calls nv_tts_say(); WASM
// apps reach it through the nv.speak host import.
#include "nv_tts.h"
#include "nucleo_tts.h"
#include "nucleo_tts_index.h"
#include "nv_audio.h"
#include "nv_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_heap_caps.h"
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

static const char *TAG = "nv_tts";
#define TTS_DIR      "/sdcard/data/tts"
#define TTS_MAX_TOK  64
#define TTS_SKIP_MS  150            // micro-pause in place of an uncovered word
#define TTS_MAX_SKIP 2              // tolerate this many uncovered words; more -> stay silent

static char s_lang[8] = "it";
static bool s_enabled = true;
static int  s_speed = 100;          // % (declared-rate tape-speed; 100 = natural)
static QueueHandle_t s_q = NULL;
static volatile uint32_t s_gen = 0; // bumped on every say; a running utterance aborts when it changes
                                    // (so a new speak SUPERSEDES the old one — no pile-up/overlap)

typedef struct { char text[96]; char lang[8]; } Utt;

static void idx_path(char *b, size_t n, const char *lang) { snprintf(b, n, "%s/%s/index.bin", TTS_DIR, lang); }
static void pcm_path(char *b, size_t n, const char *lang) { snprintf(b, n, "%s/%s/clips.pcm", TTS_DIR, lang); }

static bool have_lang(const char *lang) {
    char p[64]; idx_path(p, sizeof p, lang);
    struct stat st; return stat(p, &st) == 0 && st.st_size > 12;
}
static bool has_clip_cb(const char *slug, void *ud) {
    return ud && slug && slug[0] && tts_index_find((tts_index_t *)ud, slug, NULL, NULL);
}
static void write_silence(int ms, uint32_t rate) {
    int frames = (int)((int64_t)ms * rate / 1000);
    static int16_t zero[256];
    while (frames > 0) { int n = frames < 256 ? frames : 256; nv_audio_pcm_write(zero, (size_t)n * 2); frames -= n; }
}
// Returns 1 if the whole clip streamed, 0 if the sink cut us off (pcm_write < 0) — lets speak_now
// report a mid-word interruption instead of failing silently.
static int stream_clip(FILE *pcm, uint32_t off, uint32_t len, uint32_t my_gen) {
    if (fseek(pcm, off, SEEK_SET) != 0) return 0;
    static int16_t buf[512];
    uint32_t rem = len;
    while (rem > 0) {
        if (s_gen != my_gen) return 0;                  // superseded by a newer say -> stop this clip now
        size_t want = rem < sizeof buf ? rem : sizeof buf;
        size_t n = fread(buf, 1, want, pcm);
        if (n == 0) break;
        if (nv_audio_pcm_write(buf, n) < 0) return 0;   // sink died mid-clip -> word truncated
        rem -= (uint32_t)n;
    }
    return 1;
}

static void speak_now(const char *text, const char *lang, uint32_t my_gen) {
    if (!have_lang(lang)) { NV_LOGW(TAG, "say '%s': no pack for %s", text, lang); return; }
    char ip[64], pp[64]; idx_path(ip, sizeof ip, lang); pcm_path(pp, sizeof pp, lang);
    tts_index_t ix;
    if (!tts_index_open(&ix, ip)) { NV_LOGW(TAG, "index_open failed: %s", ip); return; }
    FILE *pcm = fopen(pp, "rb");
    if (!pcm) { NV_LOGW(TAG, "clips open failed: %s", pp); tts_index_close(&ix); return; }

    tts_token_t tok[TTS_MAX_TOK];
    int nt = nucleo_tts_plan(text, lang, has_clip_cb, &ix, tok, TTS_MAX_TOK);
    int skips = 0;
    for (int i = 0; i < nt; i++) if (tok[i].kind == TTS_TOK_UNKNOWN) skips++;
    NV_LOGI(TAG, "say '%s' (%s): %d tok, %d unknown, rate=%lu, tok0=%d/'%s'",
            text, lang, nt, skips, (unsigned long)ix.rate, nt ? tok[0].kind : -1, nt ? tok[0].slug : "");
    if (skips > TTS_MAX_SKIP) { fclose(pcm); tts_index_close(&ix); return; }   // too much missing -> stay quiet

    uint32_t rate = ix.rate ? ix.rate : 24000;
    uint32_t play_rate = rate * (uint32_t)s_speed / 100;                       // tape-speed via declared rate
    if (play_rate < 8000) play_rate = 8000; else if (play_rate > 48000) play_rate = 48000;
    nv_audio_voice_priority(true);   // voice owns the DAC: preempt game SFX, mute tones (no overlap)
    NV_LOGI(TAG, "say begin '%s': free SRAM=%uKB PSRAM=%uKB", text,
            (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
            (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));
    if (!nv_audio_pcm_begin_as((int)play_rate, 1, 16, NV_PCM_VOICE)) {
        NV_LOGW(TAG, "pcm_begin(%lu) failed", (unsigned long)play_rate);
        nv_audio_voice_priority(false); fclose(pcm); tts_index_close(&ix); return;
    }
    uint32_t sent = 0; int cut = 0;
    for (int i = 0; i < nt && !cut; i++) {
        if (s_gen != my_gen) { cut = 1; break; }            // a newer say arrived -> abort the rest
        if (tok[i].kind == TTS_TOK_CLIP) {
            uint32_t off, len;
            if (tts_index_find(&ix, tok[i].slug, &off, &len)) {
                if (!stream_clip(pcm, off, len, my_gen)) cut = 1;   // sink cut or superseded
                sent += len;
            }
        } else if (tok[i].kind == TTS_TOK_PAUSE) {
            write_silence(tok[i].ms > 0 ? tok[i].ms : TTS_SKIP_MS, play_rate);
        } else {                                                              // UNKNOWN -> micro gap
            write_silence(TTS_SKIP_MS, play_rate);
        }
    }
    nv_audio_pcm_end();
    nv_audio_voice_priority(false);   // release the channel back to sounds/tones
    if (cut) NV_LOGW(TAG, "say CUT SHORT '%s' after %lu bytes (sink error mid-word)", text, (unsigned long)sent);
    NV_LOGI(TAG, "say done: %lu pcm bytes @%lu", (unsigned long)sent, (unsigned long)play_rate);
    fclose(pcm);
    tts_index_close(&ix);
}

static void tts_task(void *) {
    Utt u;
    for (;;) {
        if (xQueueReceive(s_q, &u, portMAX_DELAY) != pdTRUE) continue;
        if (!s_enabled) continue;
        speak_now(u.text, u.lang[0] ? u.lang : s_lang, s_gen);
    }
}

// ---- public API ---------------------------------------------------------------------------------
bool nv_tts_init(const char *lang) {
    if (lang && lang[0]) snprintf(s_lang, sizeof s_lang, "%s", lang);
    if (!s_q) {
        s_q = xQueueCreate(3, sizeof(Utt));
        if (s_q) xTaskCreateWithCaps(tts_task, "nv_tts", 20 * 1024, NULL, 4, NULL, MALLOC_CAP_SPIRAM);
    }
    bool ok = have_lang(s_lang);
    NV_LOGI(TAG, "TTS %s (lang=%s, dir=%s)", ok ? "ready" : "no voice pack", s_lang, TTS_DIR);
    return ok;
}
bool nv_tts_available(void) { return have_lang(s_lang); }
bool nv_tts_say(const char *text, const char *lang) {
    if (!s_enabled || !s_q || !text || !text[0]) return false;
    Utt u; snprintf(u.text, sizeof u.text, "%s", text);
    snprintf(u.lang, sizeof u.lang, "%s", (lang && lang[0]) ? lang : s_lang);
    if (!have_lang(u.lang)) return false;
    // Supersede anything in flight: bump the generation (running utterance aborts), drop queued
    // older ones, and flush the audio ring so the current word stops NOW — then enqueue the new one.
    s_gen++;
    xQueueReset(s_q);
    nv_audio_pcm_flush();
    xQueueSend(s_q, &u, 0);
    return true;
}
void nv_tts_set_lang(const char *lang) { if (lang && lang[0]) snprintf(s_lang, sizeof s_lang, "%s", lang); }
void nv_tts_set_enabled(bool on) { s_enabled = on; }
bool nv_tts_enabled(void) { return s_enabled; }
void nv_tts_set_speed(int pct) { s_speed = nucleo_tts_speed_clamp(pct); }
int  nv_tts_speed(void) { return s_speed; }
void nv_tts_stop(void) { /* nv_audio has no fine-grained stop for the PCM stream; clips are short */ }
