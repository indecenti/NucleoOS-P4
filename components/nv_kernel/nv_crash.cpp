// nv_crash — read back the flash core dump left by a previous panic. See nv_crash.h.
#include "nv_crash.h"
#include "nv_log.h"

#include <cstring>
#include "esp_app_desc.h"
#include "esp_attr.h"     // RTC_NOINIT_ATTR: the restart note survives the reset
#include "esp_core_dump.h"
#include "esp_flash.h"
#include "esp_system.h"   // esp_reset_reason
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "crash";

namespace {
bool            s_checked = false;
bool            s_present = false;
nv_crash_info_t s_info{};

constexpr uint32_t kRestartMagic = 0x52535452;   // "RSTR"
struct RestartStore {
    uint32_t          magic;
    nv_restart_note_t n;
    uint32_t          sum;
};
RTC_NOINIT_ATTR RestartStore s_rst;
EXT_RAM_BSS_ATTR bool              s_rst_checked;   // read once per boot: PSRAM, internal SRAM is the budget
EXT_RAM_BSS_ATTR bool              s_rst_have;
EXT_RAM_BSS_ATTR nv_restart_note_t s_rst_prev;

uint32_t rst_sum(const RestartStore &s) {
    const uint8_t *b = reinterpret_cast<const uint8_t *>(&s.n);
    uint32_t x = kRestartMagic;
    for (size_t i = 0; i < sizeof s.n; i++) x = (x ^ b[i]) * 16777619u;
    return x;
}

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

bool nv_crash_last_restart(nv_restart_note_t *out) {
    if (!s_rst_checked) {
        s_rst_checked = true;
        s_rst_have = esp_reset_reason() == ESP_RST_SW && s_rst.magic == kRestartMagic && s_rst.sum == rst_sum(s_rst);
        if (s_rst_have) {
            s_rst_prev = s_rst.n;
            s_rst_prev.task[sizeof s_rst_prev.task - 1] = '\0';
            NV_LOGI(TAG, "previous boot restarted by %s, pc 0x%08lx, after %lu s", s_rst_prev.task,
                    (unsigned long)s_rst_prev.pc, (unsigned long)s_rst_prev.uptime_s);
        }
        s_rst.magic = 0;   // one boot's answer: a later panic / power cycle must not repeat it
    }
    if (s_rst_have && out) *out = s_rst_prev;
    return s_rst_have;
}

// Linked in place of every esp_restart() (components/nv_kernel/CMakeLists.txt: --wrap).
extern "C" [[noreturn]] void __real_esp_restart(void);
extern "C" [[noreturn]] void __wrap_esp_restart(void) {
    RestartStore s{};
    // The return address is past the call, and esp_restart never returns: the compiler puts nothing
    // after the call, so that address is already the next function. 2 bytes back is inside the call
    // (4-byte or compressed), in the caller.
    s.n.pc = (uint32_t)(uintptr_t)__builtin_return_address(0) - 2;
    s.n.uptime_s = (uint32_t)(esp_timer_get_time() / 1000000);
    const char *t = xPortInIsrContext() ? "isr" : pcTaskGetName(nullptr);
    strncpy(s.n.task, t ? t : "?", sizeof s.n.task - 1);
    s.magic = kRestartMagic;
    s.sum = rst_sum(s);
    s_rst = s;
    __real_esp_restart();
}
