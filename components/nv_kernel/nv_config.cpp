#include "nv_config.h"

#include "nvs.h"
#include "nvs_flash.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "esp_memory_utils.h"

#include "nv_event_bus.h"
#include "nv_log.h"

#include <string.h>

static const char *TAG = "cfg";
static const char *NS = "nvcfg";
static bool s_ready = false;

// ---- flash-safe execution ----------------------------------------------------------------------
// Every NVS access — READS included — ends in esp_flash_read/write, which disables the flash cache
// and asserts that the calling task's stack lives in internal DRAM (spi_flash/cache_utils.c,
// esp_task_stack_is_sane_cache_disabled). A task whose stack was moved to PSRAM (nv_bgwork, the
// ANIMA workers, keydeck, the audio feeders, ...) therefore aborts the moment it reads a setting.
// Rather than auditing every caller forever, this module detects a PSRAM stack and runs the
// operation synchronously on a small internal-stack helper task. Callers on internal stacks (LVGL,
// httpd, app_main, esp_timer) keep the direct path — no extra latency for the UI.
namespace {

struct Op {
    enum Kind { GET_I32, SET_I32, GET_STR, SET_STR } kind;
    const char *key;
    int32_t     ival;    // in: default (GET) / value (SET); out: result (GET)
    const char *sval;    // SET_STR value
    char       *out;     // GET_STR destination
    size_t      n;       // GET_STR capacity
    bool        ok;      // GET_STR: key found
};

SemaphoreHandle_t s_proxy_mtx = nullptr;   // one proxied op at a time
SemaphoreHandle_t s_proxy_done = nullptr;
QueueHandle_t     s_proxy_q = nullptr;

bool on_psram_stack(void) {
    int probe = 0;   // only its ADDRESS matters: it lives on the caller's stack
    return esp_ptr_external_ram(&probe);
}

void run_op(Op *op) {
    nvs_handle_t h;
    switch (op->kind) {
    case Op::GET_I32: {
        if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) return;
        nvs_get_i32(h, op->key, &op->ival);   // leaves ival = default when the key is absent
        nvs_close(h);
        break;
    }
    case Op::SET_I32: {
        if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) return;
        if (nvs_set_i32(h, op->key, op->ival) == ESP_OK) nvs_commit(h);
        nvs_close(h);
        break;
    }
    case Op::GET_STR: {
        if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) return;
        size_t cap = op->n;
        op->ok = nvs_get_str(h, op->key, op->out, &cap) == ESP_OK;   // NUL-terminated on success
        nvs_close(h);
        break;
    }
    case Op::SET_STR: {
        if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) return;
        if (nvs_set_str(h, op->key, op->sval ? op->sval : "") == ESP_OK) nvs_commit(h);
        nvs_close(h);
        break;
    }
    }
}

void proxy_task(void *) {
    Op *op = nullptr;
    for (;;) {
        if (xQueueReceive(s_proxy_q, &op, portMAX_DELAY) != pdTRUE) continue;
        run_op(op);
        xSemaphoreGive(s_proxy_done);
    }
}

// Run `op` where it is safe: inline on an internal stack, on the helper task from a PSRAM stack.
void exec_op(Op *op) {
    if (on_psram_stack() && s_proxy_q) {
        xSemaphoreTake(s_proxy_mtx, portMAX_DELAY);
        if (xQueueSend(s_proxy_q, &op, portMAX_DELAY) == pdTRUE)
            xSemaphoreTake(s_proxy_done, portMAX_DELAY);
        xSemaphoreGive(s_proxy_mtx);
        return;
    }
    run_op(op);
}

void proxy_init(void) {
    if (s_proxy_q) return;
    s_proxy_mtx = xSemaphoreCreateMutex();
    s_proxy_done = xSemaphoreCreateBinary();
    s_proxy_q = xQueueCreate(1, sizeof(Op *));
    // Internal stack on purpose (this is the whole point); 4 KB covers nvs_* + the flash driver.
    if (!s_proxy_mtx || !s_proxy_done || !s_proxy_q ||
        xTaskCreate(proxy_task, "nv_cfg", 4096, nullptr, 5, nullptr) != pdPASS) {
        NV_LOGE(TAG, "flash-safe proxy unavailable — settings from PSRAM-stack tasks will abort");
        if (s_proxy_q) vQueueDelete(s_proxy_q);
        s_proxy_q = nullptr;
    }
}

}  // namespace

void nv_config_init(void) {
    esp_err_t e = nvs_flash_init();
    if (e == ESP_ERR_NVS_NO_FREE_PAGES || e == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        e = nvs_flash_init();
    }
    s_ready = (e == ESP_OK);
    if (s_ready) proxy_init();
    NV_LOGI(TAG, "config store %s", s_ready ? "ready" : "FAILED");
}

int nv_config_get_int(const char *key, int def) {
    if (!s_ready || !key) return def;
    Op op = {Op::GET_I32, key, def, nullptr, nullptr, 0, false};
    exec_op(&op);
    return op.ival;
}

void nv_config_set_int(const char *key, int value) {
    if (!s_ready || !key) return;
    Op op = {Op::SET_I32, key, value, nullptr, nullptr, 0, false};
    exec_op(&op);
    nv_event_publish(NV_EV_SETTINGS_CHANGED, key);  // live-apply (runs on the caller's task)
}

bool nv_config_get_bool(const char *key, bool def) {
    return nv_config_get_int(key, def ? 1 : 0) != 0;
}

void nv_config_set_bool(const char *key, bool value) {
    nv_config_set_int(key, value ? 1 : 0);
}

void nv_config_get_str(const char *key, const char *def, char *out, size_t n) {
    if (!out || n == 0) return;
    const char *d = def ? def : "";
    if (s_ready && key) {
        Op op = {Op::GET_STR, key, 0, nullptr, out, n, false};
        exec_op(&op);
        if (op.ok) return;
    }
    strncpy(out, d, n - 1);   // key absent / store down -> default
    out[n - 1] = '\0';
}

void nv_config_set_str(const char *key, const char *value) {
    if (!s_ready || !key) return;
    Op op = {Op::SET_STR, key, 0, value, nullptr, 0, false};
    exec_op(&op);
    nv_event_publish(NV_EV_SETTINGS_CHANGED, key);  // live-apply / SD backup
}
