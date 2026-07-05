// nv_sd — microSD mount + hot-plug monitor. See nv_sd.h.
#include "nv_sd.h"
#include "nv_pins.h"
#include "nv_log.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_heap_caps.h"

#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "driver/sdmmc_host.h"
#include "esp_ldo_regulator.h"
#include "esp_log.h"

#include <stdio.h>   // FILE / fopen / fclose for the removal-safe wrappers

static const char *TAG = "sd";
#define NV_SD_MOUNT_POINT "/sdcard"

// How long a teardown waits for in-flight file sessions to finish before giving up and DEFERRING
// (leaving the card mounted, retried on the next monitor poll). A dead-bus fread/fwrite errors out
// well within an SDMMC command timeout (<1s), so the loop closes its handle and drains long before
// this; the ceiling only bounds a pathologically stuck consumer so the monitor never wedges.
#define NV_SD_DRAIN_MS 3000

namespace {

sdmmc_card_t             *s_card    = nullptr;
esp_ldo_channel_handle_t  s_ldo     = nullptr;
SemaphoreHandle_t         s_lock    = nullptr;   // serializes mount/unmount
volatile bool             s_mounted = false;     // word-size: read lock-free by is_mounted()
volatile uint32_t         s_gen     = 0;
bool                      s_monitor = false;      // monitor task started

// Removal-safe I/O refcount. s_busy counts open file sessions; s_unmounting gates NEW sessions off
// while a teardown drains the existing ones. Guarded by its own mutex, DISTINCT from s_lock so a
// consumer ending its session never contends on the mount/unmount serializer (no lock inversion
// with the monitor, which holds s_lock across the whole drain).
SemaphoreHandle_t         s_busy_lock  = nullptr;
int                       s_busy       = 0;
bool                      s_unmounting = false;

void lock(void)   { if (s_lock) xSemaphoreTake(s_lock, portMAX_DELAY); }
void unlock(void) { if (s_lock) xSemaphoreGive(s_lock); }

void busy_lock(void)   { if (s_busy_lock) xSemaphoreTake(s_busy_lock, portMAX_DELAY); }
void busy_unlock(void) { if (s_busy_lock) xSemaphoreGive(s_busy_lock); }

// The SD I/O rail on the ESP32-P4 is fed by on-chip LDO VO4. Acquire once, keep powered.
void power_on(void) {
    if (s_ldo) return;
    esp_ldo_channel_config_t ldo = {};
    ldo.chan_id = 4;
    ldo.voltage_mv = 3300;
    esp_ldo_acquire_channel(&ldo, &s_ldo);
}

// Mount attempt (caller holds lock). Quiet: the monitor silences the low-level SDMMC/FAT tags,
// so a "no card" retry doesn't spam the log; we log only real state transitions.
bool do_mount(void) {
    if (s_mounted) return true;
    power_on();

    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.slot = SDMMC_HOST_SLOT_0;

    sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
    slot.width = 4;
    slot.clk = (gpio_num_t)NV_PIN_SD_CLK;
    slot.cmd = (gpio_num_t)NV_PIN_SD_CMD;
    slot.d0  = (gpio_num_t)NV_PIN_SD_D0;
    slot.d1  = (gpio_num_t)NV_PIN_SD_D1;
    slot.d2  = (gpio_num_t)NV_PIN_SD_D2;
    slot.d3  = (gpio_num_t)NV_PIN_SD_D3;
    slot.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;

    esp_vfs_fat_sdmmc_mount_config_t mcfg = {};
    mcfg.format_if_mount_failed = false;   // NEVER auto-format a user's card
    mcfg.max_files = 8;
    mcfg.allocation_unit_size = 16 * 1024;

    const esp_err_t err =
        esp_vfs_fat_sdmmc_mount(NV_SD_MOUNT_POINT, &host, &slot, &mcfg, &s_card);
    if (err != ESP_OK) {
        s_card = nullptr;
        return false;
    }

    s_mounted = true;
    s_gen = s_gen + 1;   // not ++: volatile increment is deprecated in C++20
    const uint64_t mb =
        ((uint64_t)s_card->csd.capacity * s_card->csd.sector_size) / (1024 * 1024);
    NV_LOGI(TAG, "SD mounted at %s (%llu MB)", NV_SD_MOUNT_POINT, mb);
    return true;
}

// Tear down the FATFS volume — but ONLY once every in-flight file session has drained, so we never
// free the card object under an active fread/fwrite. Caller holds s_lock. Returns true if actually
// unmounted, false if deferred because handles are still open (the monitor retries next poll).
bool do_unmount(void) {
    if (!s_mounted) return false;

    // Gate new sessions off first. An fopen that already passed the gate keeps s_busy>0 and holds
    // the volume registered until it closes; anyone arriving now gets NULL and backs off.
    busy_lock();
    s_unmounting = true;
    busy_unlock();

    // Wait (bounded) for the counted sessions to reach zero.
    const TickType_t start = xTaskGetTickCount();
    for (;;) {
        busy_lock();
        const int busy = s_busy;
        busy_unlock();
        if (busy == 0) break;
        if ((xTaskGetTickCount() - start) >= pdMS_TO_TICKS(NV_SD_DRAIN_MS)) {
            busy_lock();
            s_unmounting = false;   // give up: stay mounted, reopen the gate, retry on next poll
            busy_unlock();
            NV_LOGW(TAG, "SD unmount deferred: %d open handle(s) still draining", busy);
            return false;
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }

    // Drained, and s_unmounting still blocks new sessions -> safe to release the volume.
    esp_vfs_fat_sdcard_unmount(NV_SD_MOUNT_POINT, s_card);
    s_card = nullptr;
    s_mounted = false;
    s_gen = s_gen + 1;   // not ++: volatile increment is deprecated in C++20

    busy_lock();
    s_unmounting = false;
    busy_unlock();
    NV_LOGI(TAG, "SD unmounted (card removed)");
    return true;
}

// Hot-plug poll: mount when a card appears, unmount when it disappears (debounced).
void monitor_task(void *) {
    int miss = 0;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(1500));
        if (!s_mounted) {
            lock();
            if (do_mount()) miss = 0;   // card inserted -> came online
            unlock();
        } else {
            // Ping the card (CMD13). Two consecutive misses => really gone (survive bus blips).
            const esp_err_t st = s_card ? sdmmc_get_status(s_card) : ESP_FAIL;
            if (st != ESP_OK) {
                if (++miss >= 2) {
                    lock(); const bool gone = do_unmount(); unlock();
                    // Reset only on a real teardown; if it was DEFERRED (handles still open) keep
                    // miss latched so the very next poll re-attempts instead of re-arming from 0.
                    if (gone) miss = 0;
                }
            } else {
                miss = 0;
            }
        }
    }
}

void start_monitor(void) {
    if (s_monitor) return;
    s_monitor = true;
    // We report SD state ourselves; silence the chatty low-level tags so repeated "no card"
    // probe attempts don't flood the log.
    esp_log_level_set("sdmmc_common", ESP_LOG_NONE);
    esp_log_level_set("sdmmc_sd",     ESP_LOG_NONE);
    esp_log_level_set("sdmmc_req",    ESP_LOG_NONE);
    esp_log_level_set("vfs_fat",      ESP_LOG_NONE);
    xTaskCreateWithCaps(monitor_task, "sd_mon", 4096, nullptr, 5, nullptr,
                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);   // stack in PSRAM
}

}  // namespace

bool nv_sd_mount(void) {
    if (!s_lock)      s_lock      = xSemaphoreCreateMutex();
    if (!s_busy_lock) s_busy_lock = xSemaphoreCreateMutex();
    lock();
    const bool ok = do_mount();
    unlock();
    if (!ok) NV_LOGW(TAG, "no SD card at boot — monitor will mount on insert");
    start_monitor();   // hot-plug from boot onward
    return ok;
}

void nv_sd_unmount(void) {
    lock(); do_unmount(); unlock();
}

bool nv_sd_is_mounted(void) { return s_mounted; }

uint32_t nv_sd_generation(void) { return s_gen; }

bool nv_sd_info(uint64_t *total_bytes, uint64_t *free_bytes) {
    if (!s_mounted) return false;
    uint64_t total = 0, freeb = 0;
    lock();
    const esp_err_t err = s_mounted ? esp_vfs_fat_info(NV_SD_MOUNT_POINT, &total, &freeb) : ESP_FAIL;
    unlock();
    if (err != ESP_OK) return false;
    if (total_bytes) *total_bytes = total;
    if (free_bytes)  *free_bytes  = freeb;
    return true;
}

const char *nv_sd_mount_point(void) { return NV_SD_MOUNT_POINT; }

// --- Removal-safe file sessions ------------------------------------------------------------------

bool nv_sd_session_begin(void) {
    if (!s_busy_lock) return false;
    busy_lock();
    const bool ok = s_mounted && !s_unmounting;   // no card, or a teardown draining -> refuse
    if (ok) s_busy++;
    busy_unlock();
    return ok;
}

void nv_sd_session_end(void) {
    if (!s_busy_lock) return;
    busy_lock();
    if (s_busy > 0) s_busy--;   // clamp: a stray end() must never drive the count negative
    busy_unlock();
}

FILE *nv_sd_fopen(const char *path, const char *mode) {
    if (!nv_sd_session_begin()) return nullptr;   // card gone / removal draining -> looks like fopen fail
    FILE *f = fopen(path, mode);
    if (!f) nv_sd_session_end();                  // no handle to carry the session -> release it now
    return f;
}

int nv_sd_fclose(FILE *f) {
    if (!f) return 0;
    const int r = fclose(f);
    nv_sd_session_end();
    return r;
}
