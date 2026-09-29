// nv_2d — one lock for the P4's 2D engines (PPA + JPEG codec). See nv_2d.h.
#include "nv_2d.h"
#include "nv_log.h"

#include "esp_async_memcpy.h"
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

// ---- DMA copy (AXI-GDMA async memcpy) -------------------------------------------------------------
async_memcpy_handle_t s_mcp = nullptr;
SemaphoreHandle_t     s_mcp_done = nullptr;
bool                  s_mcp_dead = false;   // a copy timed out once: never trust the channel again

SemaphoreHandle_t copy_lock(void) {
    static StaticSemaphore_t buf;
    static SemaphoreHandle_t h = xSemaphoreCreateMutexStatic(&buf);
    return h;
}

bool IRAM_ATTR mcp_done(async_memcpy_handle_t, async_memcpy_event_t *, void *) {
    BaseType_t hp = pdFALSE;
    xSemaphoreGiveFromISR(s_mcp_done, &hp);
    return hp == pdTRUE;
}

}  // namespace

esp_err_t nv_2d_copy(void *dst, const void *src, size_t n, uint32_t timeout_ms) {
    if (!dst || !src || !n) return ESP_ERR_INVALID_ARG;
    if (s_mcp_dead) return ESP_ERR_NOT_SUPPORTED;
    if (xSemaphoreTake(copy_lock(), pdMS_TO_TICKS(timeout_ms)) != pdTRUE) return ESP_ERR_TIMEOUT;
    esp_err_t e = ESP_OK;
    if (s_mcp_dead) {
        e = ESP_ERR_NOT_SUPPORTED;
    } else if (!s_mcp) {
        async_memcpy_config_t cfg = ASYNC_MEMCPY_DEFAULT_CONFIG();
        cfg.backlog = 1;
        cfg.dma_burst_size = 64;
        s_mcp_done = xSemaphoreCreateBinary();
        if (!s_mcp_done || esp_async_memcpy_install_gdma_axi(&cfg, &s_mcp) != ESP_OK) {
            s_mcp = nullptr;
            s_mcp_dead = true;
            e = ESP_ERR_NOT_SUPPORTED;
            NV_LOGW(TAG, "DMA copy unavailable: CPU copies from now on");
        }
    }
    if (e == ESP_OK) {
        xSemaphoreTake(s_mcp_done, 0);
        e = esp_async_memcpy(s_mcp, dst, const_cast<void *>(src), n, mcp_done, nullptr);
        if (e == ESP_OK && xSemaphoreTake(s_mcp_done, pdMS_TO_TICKS(timeout_ms)) != pdTRUE) {
            s_mcp_dead = true;   // may still complete into dst later: never reuse the channel
            e = ESP_ERR_TIMEOUT;
            NV_LOGW(TAG, "DMA copy of %u B timed out: CPU copies from now on", (unsigned)n);
        }
    }
    xSemaphoreGive(copy_lock());
    return e;
}

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
