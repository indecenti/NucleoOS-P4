// nv_crash — read back the flash core dump left by a previous panic. See nv_crash.h.
#include "nv_crash.h"
#include "nv_log.h"

#include <cstring>
#include "esp_app_desc.h"
#include "esp_core_dump.h"
#include "esp_flash.h"

static const char *TAG = "crash";

namespace {
bool            s_checked = false;
bool            s_present = false;
nv_crash_info_t s_info{};

// The RISC-V panic handler stores the raw exception code for faults, and for panic interrupts
// the CPU interrupt number (CLIC offset already removed): 24 = interrupt watchdog (bit 12 set when
// it was CPU1 that stopped ticking), 25 = cache error, 26 = memory protection, 27 = stack guard.
const char *reason_of(uint32_t mcause) {
    switch (mcause) {
    case 0:  return "insn_misaligned";
    case 1:  return "insn_access_fault";
    case 2:  return "illegal_insn";
    case 3:  return "breakpoint";
    case 4:  return "load_misaligned";
    case 5:  return "load_fault";
    case 6:  return "store_misaligned";
    case 7:  return "store_fault";
    case 8: case 9: case 11: return "ecall";
    case 24: return "int_wdt_cpu0";
    case 24 | (1u << 12): return "int_wdt_cpu1";
    case 25: return "cache_err";
    case 26: return "memprot";
    case 27: return "stack_guard";
    default: return "unknown";
    }
}
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
                s_info.mcause = sum.ex_info.mcause;
                s_info.mtval = sum.ex_info.mtval;
                s_info.ra = sum.ex_info.ra;
                s_info.sp = sum.ex_info.sp;
                strlcpy(s_info.reason, reason_of(sum.ex_info.mcause), sizeof s_info.reason);
                strlcpy(s_info.elf_sha, (const char *)sum.app_elf_sha256, sizeof s_info.elf_sha);
                char mine[sizeof s_info.elf_sha];
                esp_app_get_elf_sha256(mine, sizeof mine);
                const size_t n = strlen(s_info.elf_sha);
                s_info.this_build = n > 0 && strncmp(mine, s_info.elf_sha, n) == 0;
                size_t addr = 0, size = 0;
                if (esp_core_dump_image_get(&addr, &size) == ESP_OK)
                    s_info.size = (uint32_t)size;
                NV_LOGW(TAG, "previous boot crashed: %s, task '%s' @ PC 0x%08lx (dump %lu bytes, elf %s%s)",
                        s_info.reason, s_info.task, (unsigned long)s_info.pc, (unsigned long)s_info.size,
                        s_info.elf_sha, s_info.this_build ? " = this build" : ", older build");
            }
        }
    }
    if (s_present && out) *out = s_info;
    return s_present;
}

int nv_crash_read(uint32_t off, void *buf, size_t len) {
    if (!nv_crash_get(nullptr) || !buf) return -1;
    size_t addr = 0, size = 0;
    if (esp_core_dump_image_get(&addr, &size) != ESP_OK) return -1;
    if (off >= size) return 0;
    if (len > size - off) len = size - off;
    return esp_flash_read(nullptr, buf, addr + off, len) == ESP_OK ? (int)len : -1;
}

void nv_crash_erase(void) {
    if (esp_core_dump_image_erase() == ESP_OK) {
        s_present = false;
        NV_LOGI(TAG, "core dump erased");
    } else {
        NV_LOGW(TAG, "core dump erase failed");
    }
}
