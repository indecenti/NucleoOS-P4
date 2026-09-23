// nv_wallpaper — launcher wallpaper from any photo on the SD card.
//
// The launcher shows /sdcard/wallpaper.jpg when it is exactly panel-sized (1024x600, baseline).
// nv_wallpaper_set_from_file() turns any baseline JPEG into that file on the background worker:
// HW JPEG decode -> centre-crop to the panel aspect ("cover") -> high-quality downscale (2x2 box
// pre-reduction + bilinear) -> HW JPEG encode -> atomic replace -> live launcher reload. Toasts
// report progress and the result. It is also the system "Set as wallpaper" action (nv_open
// handler "sys.wallpaper", offered for image/jpeg by Files, the Gallery viewer, ...).
#pragma once
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// LVGL thread. Queue the conversion of a baseline JPEG. False when the file is missing, not a
// JPEG, or a conversion is already running (a toast says why).
bool nv_wallpaper_set_from_file(const char *path);
// A custom wallpaper file is present on the SD.
bool nv_wallpaper_is_set(void);
// LVGL thread. Remove the custom wallpaper and fall back to the theme gradient.
void nv_wallpaper_clear(void);
// Register the "sys.wallpaper" nv_open action (called once from nv_apps_register_all).
void nv_wallpaper_register(void);

#ifdef __cplusplus
}
#endif
