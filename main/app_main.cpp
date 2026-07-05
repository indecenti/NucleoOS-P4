// NucleoOS Anima — boot entry (Phase 1: kernel core wired + demo).
// Kernel: log · event bus · service manager · memory broker (solo-mode).
// HAL: JD9165 display + GT911 touch + SD + LVGL home.
#include <cstddef>
#include <cstdint>
#include <cstdio>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_chip_info.h"

#include "nv_log.h"
#include "nv_crash.h"
#include "nv_event_bus.h"
#include "nv_config.h"
#include "nv_time.h"
#include "nv_i18n.h"
#include "nv_theme.h"
#include "nv_service_mgr.h"
#include "nv_memory_broker.h"
#include "nv_hal.h"
#include "nv_rtc.h"
#include "nv_audio.h"
#include "nv_sd.h"
#include "nv_backup.h"
#include "nv_wifi.h"
#include "nv_eth.h"
#include "nv_ui.h"
#include "nv_ime.h"   // USB-HID keyboard sink -> IME injection
#include "nv_notify.h"
#include "esp_lvgl_port.h"
#include "nv_apps.h"
#include "nv_ota.h"
#include "nv_keydeck.h"
#include "nv_web.h"
#include "nv_wasm.h"
#include "nv_tts.h"
#include "nv_usb.h"
#include "nv_usb_audio.h"
#include "nv_hid_host.h"
#include "nv_camera.h"

static const char *TAG = "boot";

// --- event subscribers (demo) -------------------------------------------------
static void on_service_evt(nv_event_t, const void *d, void *) {
    auto *e = static_cast<const nv_service_event_t *>(d);
    NV_LOGI("evt", "service '%s' -> state %d", e->name, static_cast<int>(e->state));
}
static void on_lowmem_evt(nv_event_t, const void *d, void *) {
    auto *free_int = static_cast<const size_t *>(d);
    NV_LOGW("evt", "LOW MEMORY event: %u KB free", static_cast<unsigned>(*free_int / 1024));
}

// --- demo services (no-op lifecycle; service mgr logs the transitions) ---------
static bool demo_ok(void) { return true; }

static void kernel_core_demo(void) {
    static const nv_service_def_t wifi_def{"wifi", demo_ok, demo_ok, demo_ok, demo_ok, false};
    static const nv_service_def_t audio_def{"audio", demo_ok, demo_ok, demo_ok, demo_ok, false};
    static const nv_service_def_t display_def{"display", demo_ok, demo_ok, nullptr, nullptr, true};

    const nv_service_id_t sid_wifi = nv_service_register(&wifi_def);
    const nv_service_id_t sid_audio = nv_service_register(&audio_def);
    const nv_service_id_t sid_disp = nv_service_register(&display_def);

    nv_service_acquire(sid_wifi);
    nv_service_acquire(sid_audio);
    nv_service_acquire(sid_disp);

    NV_LOGI(TAG, "--- broker demo: launch heavy app (4MB), keep display ---");
    const nv_service_id_t keep[] = {sid_disp};
    (void)nv_mem_request(4 * 1024 * 1024, keep, 1);  // suspends wifi + audio, keeps display

    vTaskDelay(pdMS_TO_TICKS(300));  // pretend the app runs

    nv_mem_release();  // resumes wifi + audio
    NV_LOGI(TAG, "--- broker demo done ---");
}

extern "C" void app_main(void) {
    nv_log_init();

    NV_LOGI(TAG, "========================================");
    NV_LOGI(TAG, "  NucleoOS Anima  -  ESP32-P4  -  Phase 1");
    NV_LOGI(TAG, "========================================");

    esp_chip_info_t chip;
    esp_chip_info(&chip);
    NV_LOGI(TAG, "chip: %d core(s), silicon rev v%d.%d",
            chip.cores, chip.revision / 100, chip.revision % 100);

    // Kernel core.
    nv_event_init();
    nv_event_subscribe(NV_EV_SERVICE_STATE, on_service_evt, nullptr);
    nv_event_subscribe(NV_EV_LOW_MEMORY, on_lowmem_evt, nullptr);
    nv_config_init();
    nv_sd_mount();     // early + non-fatal: settings restore reads the card before UI config is read
    nv_backup_init();  // if NVS was wiped, restore prefs from the SD backup; then auto-back-up
    nv_i18n_init();    // load saved language before any UI string is resolved
    nv_theme_init();   // compose the active theme (mode/accent/font-scale) before any UI is built
    nv_service_mgr_init();
    nv_mem_broker_init();

    // Mark the running image valid EARLY — before the Wi-Fi/HAL bring-up that can occasionally
    // fault on the (older) C6 esp-hosted co-processor. If mark-valid ran only at the end and an
    // early fault rebooted us first, the bootloader would roll a freshly-OTA'd image back to the
    // previous slot. Confirming validity here makes OTA updates stick across resets.
    nv_ota_init();

    nv_wifi_init(); // Wi-Fi service (radio stays lazy until Settings enables it)
    nv_eth_init();  // wired Ethernet (IP101): plug & play, non-fatal without a cable/PHY
    nv_time_init(); // system clock: seed from build time + SNTP (auto-syncs once online)

    if (nv_hal_init()) {
        nv_hal_backlight_set(nv_config_get_int("brightness", 90));  // restore saved brightness
        nv_rtc_sync();           // I2C bus is up now — seed clock from RX8130 + persist on sync
        nv_audio_init();         // ES8311 DAC over I2S (shares the I2C bus); applies saved volume
        nv_tts_init("it");       // OS-wide offline voice (voice packs on SD /sdcard/data/tts/<lang>)
        nv_wasm_seed_demo();     // write the bundled demo WASM app to SD BEFORE the registry scan
        nv_wasm_seed_tanks();    // self-install the built-in Nucleo Tanks game to /sdcard/apps
        nv_apps_register_all();  // populate the app registry (incl. WASM tiles) before the launcher
        nv_ui_start();           // SystemUI: status bar + launcher + shade + gestures
        nv_keydeck_init();       // remote keyboard + telemetry (idles until Wi-Fi is up)
        nv_web_init();           // web console (idles until Wi-Fi is up; http://nucleov2.local)
        // One OTG-HS controller, two personalities. HOST (default): a USB speaker/soundbar on the
        // Type-C becomes the system output (nv_audio auto-routes). DEVICE: PC second-screen.
        // Flip with the terminal's `usb host|device` command; a reboot applies it.
        if (nv_config_get_bool("usbhost", true)) {
            nv_usb_audio_init();  // UAC host: hot-plug USB audio on the OTG-HS Type-C
            nv_hid_host_init();    // + keyboard/mouse (directly or behind a hub)
            // nv_hal can't call the IME directly (nv_ui depends on nv_hal) — wire it here.
            nv_hid_host_set_sink([](const char *s) { nv_ime_inject_text(s); },
                                [](int k) { nv_ime_inject_key((nv_ime_remote_key_t)k); });
        } else
            nv_usb_init();        // USB extend-screen device on the OTG-HS Type-C (PC second screen)

        // Surface a previous-boot crash (core dump found in flash): warn notification + details
        // in Diagnostics. Posting UI from app_main requires the LVGL lock (keydeck pattern).
        nv_crash_info_t ci;
        if (nv_crash_get(&ci) && lvgl_port_lock(2000)) {
            char m[96];
            snprintf(m, sizeof m, nv_tr(NV_STR_CRASH_NOTIF_FMT), ci.task, (unsigned)ci.pc);
            nv_notify_post(NV_NOTE_WARN, "System", m);
            lvgl_port_unlock();
        }
        if (nv_config_get_bool("chime", true))
            nv_audio_chime();    // brief startup confirmation tone (silent without a speaker)

        // Camera bring-up is confirmed working (continuous capture verified on HW). The boot
        // self-test is off by default now; set "cam_selftest"=true to re-run the continuity probe.
        if (nv_config_get_bool("cam_selftest", false)) {
            if (nv_camera_start()) {
                vTaskDelay(pdMS_TO_TICKS(1000));
                unsigned f1 = (unsigned)nv_camera_frames();
                vTaskDelay(pdMS_TO_TICKS(2000));
                unsigned f2 = (unsigned)nv_camera_frames();
                NV_LOGI(TAG, "CAM self-test: %u frames @1s, %u @3s (%s)",
                        f1, f2, (f2 > f1 + 5) ? "CONTINUOUS" : "STALLED");
                nv_camera_stop();
            }
        }

        // WASM runtime boot self-test (quiet): exported-function call + host-import path.
        // Non-fatal on failure; installed apps run on demand through the Apps manager.
        int wr = 0; char werr[128] = "";
        if (nv_wasm_run_demo(7, 5, &wr, werr, sizeof werr))
            NV_LOGI(TAG, "WASM self-test OK: 7+5=%d", wr);
        else
            NV_LOGE(TAG, "WASM self-test failed: %s", werr);
        werr[0] = '\0';
        if (!nv_wasm_run_app(werr, sizeof werr))
            NV_LOGE(TAG, "WASM host-import self-test failed: %s", werr);
    } else {
        NV_LOGE(TAG, "HAL init failed — running headless");
    }

    // Hands-free OTA: on boot, auto-check the saved manifest URL and self-update if a new build is
    // offered (runs on its own task; waits for Wi-Fi). Enabled by default; "ota_auto"=false opts out.
    if (nv_config_get_bool("ota_auto", true)) {
        char ota_url[256];
        nv_config_get_str("ota_url", "http://192.168.0.216:8080/manifest.json",
                          ota_url, sizeof(ota_url));
        nv_ota_boot_autoupdate(ota_url);
    }

    kernel_core_demo();

    NV_LOGI(TAG, "boot complete; entering heartbeat");

    uint32_t tick = 0;
    for (;;) {
        const size_t sram = nv_mem_free_internal();
        const size_t psram = nv_mem_free_psram();
        const size_t frag = nv_mem_largest_internal();
        NV_LOGI(TAG, "[hb %lu] free SRAM=%u KB (largest %u KB)  PSRAM=%u KB",
                static_cast<unsigned long>(tick++),
                static_cast<unsigned>(sram / 1024),
                static_cast<unsigned>(frag / 1024),
                static_cast<unsigned>(psram / 1024));
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}
