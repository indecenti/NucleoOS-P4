// nv_wake device glue: mic tap -> stream buffer -> wake task (decimate + detect) -> handler.
// The detector backend is ESP-SR WakeNet when CONFIG_NV_WAKE_ESP_SR is set (wake_esp_sr.c), none
// otherwise; the state machine and the end-of-question detector live in wake_core.c (host-tested).
#include "nv_wake.h"
#include "wake_core.h"
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/stream_buffer.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "sdkconfig.h"
#include "nv_audio.h"
#include "nv_config.h"
#include "nv_mem_attr.h"

static const char *TAG = "nv_wake";

#if CONFIG_NV_WAKE_ESP_SR
extern const wake_backend_t g_wake_esp_sr;
#define BACKEND (&g_wake_esp_sr)
#else
#define BACKEND NULL
#endif

NV_PSRAM_BSS static wake_core_t s_w;   // ~0.9 KB of names/state: PSRAM (task context only)
static SemaphoreHandle_t s_mx;                 // guards s_w between the wake task and the API
static StreamBufferHandle_t s_sb;              // mic tap -> wake task (raw 48 kHz samples)
static TaskHandle_t s_task;
static void (*s_handler)(void);
static volatile bool s_reconfig;

#define SB_BYTES   (48000 * 2 / 2)             // ~0.5 s of 48 kHz mono: the tap never blocks
#define READ_SAMP  1536                         // 32 ms

static int64_t now_ms(void) { return esp_timer_get_time() / 1000; }

// On the mic worker task: copy and go (never block the capture loop).
static void tap(const int16_t *pcm, int n, void *ctx)
{
    (void)ctx;
    if (s_sb) xStreamBufferSend(s_sb, pcm, (size_t)n * sizeof(int16_t), 0);
}

static void apply_settings_locked(void)
{
    char word[32];
    nv_config_get_str("wake.word", "", word, sizeof word);
    s_w.mic_ok = nv_audio_mic_ready();
    wake_core_config(&s_w, nv_config_get_bool("wake.on", false), word, nv_config_get_int("wake.sens", 1));
    if (s_w.opened) {
        if (!nv_audio_listen_active()) nv_audio_listen_start(tap, NULL);
        ESP_LOGI(TAG, "listening for '%s' (sensitivity %d)", s_w.word, s_w.sens);
    } else {
        nv_audio_listen_stop();
        if (s_w.state == NV_WAKE_UNAVAILABLE) ESP_LOGW(TAG, "unavailable: %s", s_w.reason_en);
    }
}

static void wake_task(void *arg)
{
    (void)arg;
    NV_PSRAM_BSS static int16_t buf[READ_SAMP];   // 3 KB: PSRAM (cached reads are fine at 32 ms chunks)
    for (;;) {
        if (s_reconfig) {
            s_reconfig = false;
            xSemaphoreTake(s_mx, portMAX_DELAY);
            apply_settings_locked();
            xSemaphoreGive(s_mx);
            xStreamBufferReset(s_sb);
        }
        const size_t got = xStreamBufferReceive(s_sb, buf, sizeof buf, pdMS_TO_TICKS(200));
        bool heard = false;
        xSemaphoreTake(s_mx, portMAX_DELAY);
        wake_core_tick(&s_w, now_ms());
        if (got) {
            // Our own speaker would trigger us (TTS, music, chimes): pause while anything plays.
            const bool busy = nv_audio_pcm_owner() >= 0 || nv_audio_rec_active();
            heard = wake_core_feed(&s_w, buf, (int)(got / sizeof(int16_t)), busy);
        }
        xSemaphoreGive(s_mx);
        if (heard) {
            ESP_LOGI(TAG, "wake word '%s' heard", s_w.word);
            if (s_handler) s_handler();
        }
    }
}

void nv_wake_init(void)
{
    if (s_task) return;
    s_mx = xSemaphoreCreateMutex();
    s_sb = xStreamBufferCreateWithCaps(SB_BYTES, sizeof(int16_t) * 64, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_w.be = BACKEND;
    if (!s_mx || !s_sb) { ESP_LOGE(TAG, "out of memory"); return; }
    s_reconfig = true;
    // WakeNet inference wants a roomy stack; PSRAM keeps it off the scarce internal SRAM.
    if (xTaskCreateWithCaps(wake_task, "nv_wake", 16 * 1024, NULL, 4, &s_task,
                            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        s_task = NULL;
        ESP_LOGE(TAG, "task create failed");
    }
}

void nv_wake_set_enabled(bool on) { nv_config_set_bool("wake.on", on); s_reconfig = true; }
void nv_wake_set_sensitivity(int level)
{
    nv_config_set_int("wake.sens", level < 0 ? 0 : level > 2 ? 2 : level);
    s_reconfig = true;
}

bool nv_wake_set_word(const char *model)
{
    if (!model) return false;
    bool ok = false;
    if (s_mx) xSemaphoreTake(s_mx, portMAX_DELAY);
    for (int i = 0; i < s_w.nwords; i++) if (!strcmp(s_w.words[i], model)) ok = true;
    if (s_mx) xSemaphoreGive(s_mx);
    if (!ok) return false;
    nv_config_set_str("wake.word", model);
    s_reconfig = true;
    return true;
}

void nv_wake_set_handler(void (*fn)(void)) { s_handler = fn; }

const char *nv_wake_state_name(nv_wake_state_t s)
{
    switch (s) {
        case NV_WAKE_OFF: return "off";
        case NV_WAKE_UNAVAILABLE: return "unavailable";
        case NV_WAKE_LISTENING: return "listening";
        case NV_WAKE_HEARD: return "heard";
        case NV_WAKE_PAUSED: return "paused";
    }
    return "?";
}

void nv_wake_status(nv_wake_status_t *o, bool en)
{
    memset(o, 0, sizeof *o);
    if (!s_mx) {   // never initialized
        o->state = NV_WAKE_UNAVAILABLE;
        snprintf(o->reason, sizeof o->reason, "%s", en ? "not started" : "non avviato");
        return;
    }
    xSemaphoreTake(s_mx, portMAX_DELAY);
    o->enabled = nv_config_get_bool("wake.on", false);
    o->state = s_reconfig ? (o->enabled ? NV_WAKE_LISTENING : NV_WAKE_OFF) : s_w.state;
    o->available = s_w.be && s_w.mic_ok && s_w.nwords > 0;
    o->sensitivity = nv_config_get_int("wake.sens", 1);
    o->nwords = s_w.nwords;
    for (int i = 0; i < s_w.nwords; i++) {
        snprintf(o->words[i], sizeof o->words[i], "%s", s_w.words[i]);
        snprintf(o->labels[i], sizeof o->labels[i], "%s", s_w.labels[i]);
        if (!strcmp(s_w.words[i], s_w.word)) snprintf(o->label, sizeof o->label, "%s", s_w.labels[i]);
    }
    snprintf(o->word, sizeof o->word, "%s", s_w.word);
    o->triggers = s_w.triggers;
    o->last_ago_s = s_w.triggers ? (int32_t)((now_ms() - s_w.last_ms) / 1000) : -1;
    if (o->state == NV_WAKE_UNAVAILABLE)
        snprintf(o->reason, sizeof o->reason, "%s", en ? s_w.reason_en : s_w.reason_it);
    else if (o->state == NV_WAKE_PAUSED)
        snprintf(o->reason, sizeof o->reason, "%s", en ? "the microphone or speaker is busy" : "microfono o altoparlante occupati");
    xSemaphoreGive(s_mx);
}
