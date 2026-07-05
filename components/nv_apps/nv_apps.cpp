// nv_apps — registers every native app into the SystemUI registry (launcher order).
// Real apps live in their own .cpp and register via apps_internal.h; the "coming soon"
// tiles are declared here as build-less descriptors.
#include "nv_apps.h"
#include "apps_internal.h"

#include "nv_app.h"
#include "nv_icons.h"
#include "nv_i18n.h"

void nv_apps_register_all(void) {
    settings_app_register();
    files_app_register();
    anima_app_register();         // offline assistant (nv_anima engine: L0/L1/HDC + online tiers)
    diagnostics_app_register();
    sysmon_app_register();        // System Monitor (task manager: CPU/SRAM/PSRAM/tasks/services)
    apps_app_register();          // WASM app manager (installed apps on SD)
    apps_register_wasm();         // installed WASM apps as tiles — early, so they fit the 15 slots
    terminal_app_register();      // local command console (heap/ps/log/i2c/ls/cat/reboot)
    secondscreen_app_register();  // USB extended display (nv_usb transport)
    gallery_app_register();
    music_app_register();         // audio player (nv_media: WAV/MP3/AAC/FLAC/M4A)
    video_app_register();         // video player (nv_vplayer: MJPEG/AVI, HW JPEG decode)
    recorder_app_register();      // voice recorder (mic -> WAV via nv_audio)
    camera_app_register();
    calculator_app_register();
    notes_app_register();
    tasks_app_register();
}
