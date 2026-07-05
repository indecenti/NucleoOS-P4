#include "nv_config.h"

#include "nvs.h"
#include "nvs_flash.h"

#include "nv_event_bus.h"
#include "nv_log.h"

#include <string.h>

static const char *TAG = "cfg";
static const char *NS = "nvcfg";
static bool s_ready = false;

void nv_config_init(void) {
    esp_err_t e = nvs_flash_init();
    if (e == ESP_ERR_NVS_NO_FREE_PAGES || e == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        e = nvs_flash_init();
    }
    s_ready = (e == ESP_OK);
    NV_LOGI(TAG, "config store %s", s_ready ? "ready" : "FAILED");
}

int nv_config_get_int(const char *key, int def) {
    if (!s_ready) return def;
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) return def;
    int32_t v = def;
    nvs_get_i32(h, key, &v);  // leaves v = def if key absent
    nvs_close(h);
    return v;
}

void nv_config_set_int(const char *key, int value) {
    if (!s_ready) return;
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) return;
    if (nvs_set_i32(h, key, value) == ESP_OK) nvs_commit(h);
    nvs_close(h);
    nv_event_publish(NV_EV_SETTINGS_CHANGED, key);  // live-apply
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
    nvs_handle_t h;
    if (s_ready && nvs_open(NS, NVS_READONLY, &h) == ESP_OK) {
        size_t cap = n;
        esp_err_t e = nvs_get_str(h, key, out, &cap);   // fills out (NUL-terminated) on success
        nvs_close(h);
        if (e == ESP_OK) return;
    }
    strncpy(out, d, n - 1);   // key absent / store down -> default
    out[n - 1] = '\0';
}

void nv_config_set_str(const char *key, const char *value) {
    if (!s_ready) return;
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) return;
    if (nvs_set_str(h, key, value ? value : "") == ESP_OK) nvs_commit(h);
    nvs_close(h);
    nv_event_publish(NV_EV_SETTINGS_CHANGED, key);  // live-apply / SD backup
}
