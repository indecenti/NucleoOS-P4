// ESP-SR WakeNet backend (CONFIG_NV_WAKE_ESP_SR). Single microphone, so WakeNet runs directly on the
// 16 kHz stream (no AFE): lighter, and enough for a quiet room at arm's length.
// The models come from the "model" flash partition that ESP-SR's build fills with the wake words
// selected in menuconfig (ESP Speech Recognition > Load Multiple Wake Words).
#include "wake_core.h"
#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "esp_wn_iface.h"
#include "esp_wn_models.h"
#include "model_path.h"

static const char *TAG = "nv_wake_sr";
static srmodel_list_t *s_models;
static const esp_wn_iface_t *s_wn;
static model_iface_data_t *s_md;

// What to say for the common models (otherwise the model name without its prefix).
static void label_of(const char *name, char *out, size_t cap)
{
    static const struct { const char *key, *label; } K[] = {
        {"hiesp", "Hi ESP"}, {"alexa", "Alexa"}, {"jarvis", "Jarvis"}, {"computer", "Computer"},
        {"heywillow", "Hey Willow"}, {"sophia", "Sophia"}, {"mycroft", "Hey Mycroft"},
        {"hilexin", "Hi Lexin"}, {"nihaoxiaozhi", "Ni Hao Xiao Zhi"}, {"xiaoaitongxue", "Xiao Ai Tong Xue"},
    };
    for (size_t i = 0; i < sizeof K / sizeof K[0]; i++) if (strstr(name, K[i].key)) { snprintf(out, cap, "%s", K[i].label); return; }
    const char *u = strchr(name, '_');
    snprintf(out, cap, "%s", u ? u + 1 : name);
}

static int sr_list(char names[][32], char labels[][32], int max)
{
    if (!s_models) s_models = esp_srmodel_init("model");
    if (!s_models) return 0;
    int n = 0;
    for (int i = 0; i < s_models->num && n < max; i++) {
        const char *m = s_models->model_name[i];
        if (strncmp(m, ESP_WN_PREFIX, strlen(ESP_WN_PREFIX))) continue;   // WakeNet models only
        snprintf(names[n], 32, "%s", m);
        label_of(m, labels[n], 32);
        n++;
    }
    return n;
}

static bool sr_open(const char *name, int sens)
{
    s_wn = (const esp_wn_iface_t *)esp_wn_handle_from_name(name);
    if (!s_wn) { ESP_LOGW(TAG, "no WakeNet handle for %s", name); return false; }
    // DET_MODE_95 is the more eager mode: only for the "high" sensitivity.
    s_md = s_wn->create(name, sens >= 2 ? DET_MODE_95 : DET_MODE_90);
    if (!s_md) { ESP_LOGW(TAG, "create failed for %s", name); return false; }
    return true;
}

static int  sr_chunk(void) { return s_wn && s_md ? s_wn->get_samp_chunksize(s_md) : 0; }
static bool sr_detect(const int16_t *pcm) { return s_wn->detect(s_md, (int16_t *)pcm) > 0; }
static void sr_close(void)
{
    if (s_wn && s_md) s_wn->destroy(s_md);
    s_md = NULL; s_wn = NULL;
}

const wake_backend_t g_wake_esp_sr = { sr_list, sr_open, sr_chunk, sr_detect, sr_close };
