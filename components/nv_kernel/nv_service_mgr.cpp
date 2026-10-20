#include "nv_service_mgr.h"

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nv_log.h"
#include "nv_mem_attr.h"

static const char *TAG = "svc";

// The table is append-only: a slot is written completely before s_count publishes it, and never
// changes afterwards, so readers only need the lock to read s_count. Probes run without the lock
// (they call into other subsystems, which must never wait on this one).
namespace {

constexpr int kMaxServices = 24;

NV_PSRAM_BSS const nv_service_def_t *s_svc[kMaxServices];   // cold table
int s_count = 0;
SemaphoreHandle_t s_mtx = nullptr;

int count_now(void) {
    if (!s_mtx) return 0;
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    const int n = s_count;
    xSemaphoreGive(s_mtx);
    return n;
}

}  // namespace

void nv_service_mgr_init(void) {
    if (s_mtx) return;
    s_mtx = xSemaphoreCreateMutex();
    NV_LOGI(TAG, "service registry init");
}

nv_service_id_t nv_service_register(const nv_service_def_t *def) {
    if (!def || !def->name || !def->probe || !s_mtx) return NV_SERVICE_INVALID;
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    for (int i = 0; i < s_count; i++) {
        if (!strcmp(s_svc[i]->name, def->name)) {
            xSemaphoreGive(s_mtx);
            return i;
        }
    }
    if (s_count >= kMaxServices) {
        xSemaphoreGive(s_mtx);
        NV_LOGE(TAG, "service table full (%d): '%s' not registered", kMaxServices, def->name);
        return NV_SERVICE_INVALID;
    }
    const nv_service_id_t id = s_count;
    s_svc[id] = def;
    s_count++;   // publish the slot only after it is written
    xSemaphoreGive(s_mtx);
    NV_LOGI(TAG, "registered '%s' (id %d)", def->name, id);
    return id;
}

int nv_service_count(void) { return count_now(); }

const char *nv_service_name(nv_service_id_t id) {
    return (id >= 0 && id < count_now()) ? s_svc[id]->name : "?";
}

nv_service_state_t nv_service_state(nv_service_id_t id, char *detail, size_t n) {
    char scratch[2];
    if (!detail || n == 0) { detail = scratch; n = sizeof scratch; }
    detail[0] = '\0';
    if (id < 0 || id >= count_now()) return NV_SVC_STOPPED;
    const nv_service_state_t st = s_svc[id]->probe(detail, n);
    detail[n - 1] = '\0';   // a probe that filled the whole buffer still leaves a string
    return st;
}
