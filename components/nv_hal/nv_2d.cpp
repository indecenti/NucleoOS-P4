// nv_2d — one lock for the P4's 2D engines (PPA + JPEG codec). See nv_2d.h.
#include "nv_2d.h"
#include "nv_log.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "nv_2d";

namespace {

// Longer than any single job (a full-panel PPA scale on this revision ~200 ms, a large JPEG decode
// ~150 ms), short enough that a wedged engine degrades one caller instead of freezing the others.
constexpr TickType_t kWait = pdMS_TO_TICKS(2000);

SemaphoreHandle_t lock_handle(void) {
    static StaticSemaphore_t buf;
    static SemaphoreHandle_t h = xSemaphoreCreateMutexStatic(&buf);   // thread-safe local static init
    return h;
}

struct Guard {
    bool held;
    explicit Guard(const char *what) : held(xSemaphoreTake(lock_handle(), kWait) == pdTRUE) {
        if (!held) NV_LOGW(TAG, "%s: 2D engines busy for %lu ms, skipped", what,
                           (unsigned long)pdTICKS_TO_MS(kWait));
    }
    ~Guard() { if (held) xSemaphoreGive(lock_handle()); }
};

}  // namespace

esp_err_t nv_2d_srm(ppa_client_handle_t client, const ppa_srm_oper_config_t *op) {
    Guard g("ppa srm");
    return g.held ? ppa_do_scale_rotate_mirror(client, op) : ESP_ERR_TIMEOUT;
}

esp_err_t nv_2d_jpeg_decode(jpeg_decoder_handle_t engine, const jpeg_decode_cfg_t *cfg, const uint8_t *in,
                            uint32_t in_len, uint8_t *out, uint32_t out_cap, uint32_t *out_size) {
    Guard g("jpeg decode");
    return g.held ? jpeg_decoder_process(engine, cfg, in, in_len, out, out_cap, out_size) : ESP_ERR_TIMEOUT;
}

esp_err_t nv_2d_jpeg_encode(jpeg_encoder_handle_t engine, const jpeg_encode_cfg_t *cfg, const uint8_t *in,
                            uint32_t in_len, uint8_t *out, uint32_t out_cap, uint32_t *out_size) {
    Guard g("jpeg encode");
    return g.held ? jpeg_encoder_process(engine, cfg, in, in_len, out, out_cap, out_size) : ESP_ERR_TIMEOUT;
}
