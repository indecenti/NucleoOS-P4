// nv_crash — read back the flash core dump left by a previous panic. See nv_crash.h.
#include "nv_crash.h"
#include "nv_log.h"

#include <cstring>
#include "esp_core_dump.h"

static const char *TAG = "crash";

namespace {
bool            s_checked = false;
bool            s_present = false;
nv_crash_info_t s_info{};
}  // namespace

bool nv_crash_get(nv_crash_info_t *out) {
    if (!s_checked) {
        s_checked = true;
        if (esp_core_dump_image_check() == ESP_OK) {
            esp_core_dump_summary_t sum{};
            if (esp_core_dump_get_summary(&sum) == ESP_OK) {
                s_present = true;
                strlcpy(s_info.task, sum.exc_task, sizeof s_info.task);
                s_info.pc = sum.exc_pc;
                size_t addr = 0, size = 0;
                if (esp_core_dump_image_get(&addr, &size) == ESP_OK)
                    s_info.size = (uint32_t)size;
                NV_LOGW(TAG, "previous boot crashed: task '%s' @ PC 0x%08lx (dump %lu bytes)",
                        s_info.task, (unsigned long)s_info.pc, (unsigned long)s_info.size);
            }
        }
    }
    if (s_present && out) *out = s_info;
    return s_present;
}

void nv_crash_erase(void) {
    if (esp_core_dump_image_erase() == ESP_OK) {
        s_present = false;
        NV_LOGI(TAG, "core dump erased");
    } else {
        NV_LOGW(TAG, "core dump erase failed");
    }
}
