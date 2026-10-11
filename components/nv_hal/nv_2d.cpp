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

bool IRAM_ATTR mcp_done(async_memcpy_handle_t, async_memcpy_event_t *, void *arg) {
    BaseType_t hp = pdFALSE;
    xSemaphoreGiveFromISR(arg ? (SemaphoreHandle_t)arg : s_mcp_done, &hp);
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
        cfg.backlog = 4;                       // nv_2d_copy_start: two tiles per core in flight
        cfg.dma_burst_size = 64;
        s_mcp_done = xSemaphoreCreateBinary();
        if (!s_mcp_done || esp_async_memcpy_install_gdma_axi(&cfg, &s_mcp) != ESP_OK) {
            s_mcp = nullptr;
            if (s_mcp_done) { vSemaphoreDelete(s_mcp_done); s_mcp_done = nullptr; }
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

esp_err_t nv_2d_copy_start(void *dst, const void *src, size_t n, void *done) {
    if (!dst || !src || !n || !done) return ESP_ERR_INVALID_ARG;
    if (s_mcp_dead || !s_mcp) {
        // Not installed yet: one blocking copy installs the channel (and does this copy).
        if (s_mcp_dead) return ESP_ERR_NOT_SUPPORTED;
        const esp_err_t e = nv_2d_copy(dst, src, n, 50);
        if (e == ESP_OK) xSemaphoreGive((SemaphoreHandle_t)done);
        return e;
    }
    if (xSemaphoreTake(copy_lock(), pdMS_TO_TICKS(50)) != pdTRUE) return ESP_ERR_TIMEOUT;
    const esp_err_t e = s_mcp_dead ? ESP_ERR_NOT_SUPPORTED
                                   : esp_async_memcpy(s_mcp, dst, const_cast<void *>(src), n, mcp_done, done);
    xSemaphoreGive(copy_lock());
    return e;
}

esp_err_t nv_2d_copy_wait(void *done, uint32_t timeout_ms) {
    if (xSemaphoreTake((SemaphoreHandle_t)done, pdMS_TO_TICKS(timeout_ms)) == pdTRUE) return ESP_OK;
    s_mcp_dead = true;                         // may still land later: never reuse the channel
    NV_LOGW(TAG, "async DMA copy timed out: CPU copies from now on");
    return ESP_ERR_TIMEOUT;
}

// ---- PPA (bounded) -------------------------------------------------------------------------------
namespace {
// A full-panel scale takes ~200 ms on this revision; the 2D-DMA interrupt is also held off while the
// flash is written (CONFIG_DMA2D_ISR_IRAM_SAFE is off), so leave room for a long erase.
constexpr TickType_t kPpaJob = pdMS_TO_TICKS(1500);
SemaphoreHandle_t s_ppa_done = nullptr;   // one job at a time (the 2D lock): one semaphore
volatile bool     s_ppa_dead = false;     // a job never finished: the PPA is retired until restart
volatile bool     s_ppa_drill = false;

// Not in IRAM, like the 2D-DMA interrupt that calls it.
bool ppa_done(ppa_client_handle_t, ppa_event_data_t *, void *sem) {
    BaseType_t hp = pdFALSE;
    xSemaphoreGiveFromISR((SemaphoreHandle_t)sem, &hp);
    return hp == pdTRUE;
}
}  // namespace

esp_err_t nv_2d_srm(ppa_client_handle_t client, const ppa_srm_oper_config_t *op) {
    if (!client || !op) return ESP_ERR_INVALID_ARG;
    if (s_ppa_dead) return ESP_ERR_NOT_SUPPORTED;
    Guard g("ppa srm");
    if (!g.held) return ESP_ERR_TIMEOUT;
    if (s_ppa_dead) return ESP_ERR_NOT_SUPPORTED;
    if (!s_ppa_done && !(s_ppa_done = xSemaphoreCreateBinary())) return ESP_ERR_NO_MEM;
    // The driver's BLOCKING mode waits on the job forever: run it NON_BLOCKING and wait here.
    ppa_event_callbacks_t cbs = {};
    cbs.on_trans_done = ppa_done;
    ppa_client_register_event_callbacks(client, &cbs);   // a pointer store; clients are ours alone
    ppa_srm_oper_config_t o = *op;
    o.mode = PPA_TRANS_MODE_NON_BLOCKING;
    o.user_data = s_ppa_done;
    xSemaphoreTake(s_ppa_done, 0);
    if (s_ppa_drill) {
        s_ppa_drill = false;
        NV_LOGW(TAG, "PPA drill: this job is not submitted, its completion never comes");
    } else {
        const esp_err_t e = ppa_do_scale_rotate_mirror(client, &o);
        if (e != ESP_OK) return e;
    }
    if (xSemaphoreTake(s_ppa_done, kPpaJob) == pdTRUE) return ESP_OK;
    // The stuck job keeps the engine's queue: every later job would wait behind it. There is no
    // abort in the driver, so stop using the PPA; the job may still land in its output buffer.
    s_ppa_dead = true;
    NV_LOGE(TAG, "PPA job %lux%lu -> %lux%lu did not finish in %lu ms: PPA retired until restart",
            (unsigned long)op->in.block_w, (unsigned long)op->in.block_h, (unsigned long)op->out.pic_w,
            (unsigned long)op->out.pic_h, (unsigned long)pdTICKS_TO_MS(kPpaJob));
    return ESP_ERR_TIMEOUT;
}

bool nv_2d_ppa_ok(void) { return !s_ppa_dead; }
void nv_2d_ppa_drill(void) { if (!s_ppa_dead) s_ppa_drill = true; }

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
