// nv_usb_audio — USB Audio Class HOST output on the OTG-HS Type-C: a standard USB speaker,
// soundbar (e.g. Dell AC511) or headset becomes the system speaker. nv_audio routes its
// output here automatically whenever a device is connected, falling back to the ES8311 DAC.
//
// Mutually exclusive with nv_usb (TinyUSB DEVICE mode, the second-screen feature): the P4 has
// ONE OTG-HS controller. app_main picks the mode from the persisted "usbhost" config key
// (default: host audio); the terminal's `usb host|device` command flips it (reboot applies).
//
// Power note: host mode must source 5V VBUS to the device. A bus-powered soundbar can draw
// ~500 mA at full volume — if it browns out, use a powered hub / externally powered OTG adapter.
#pragma once

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// Install the USB host stack + UAC class driver (background tasks). Idempotent.
bool nv_usb_audio_init(void);

// True while a UAC output device is connected with an active stream.
bool nv_usb_audio_present(void);

// (Re)start the device stream so it can accept PCM in this format. Mono sources are accepted
// on a stereo device (duplicated in write); a rate the device doesn't offer is linear-resampled
// in write() to its preferred rate (e.g. 44.1 kHz music on a 48 kHz-only soundbar). Returns
// false only for shapes the sink can't take (bits != 16, more channels than the device) —
// the caller then falls back to the ES8311 path.
bool nv_usb_audio_open(int sample_rate, int channels, int bits);

// Blocking PCM write (bytes of 16-bit samples, `src_ch` channels wide). Returns bytes
// consumed or -1. Mono is duplicated to the device channel count on the fly.
int nv_usb_audio_write(const void *pcm, size_t bytes, int src_ch);

void nv_usb_audio_set_volume(int percent);   // 0..100
void nv_usb_audio_set_mute(bool on);

// Diagnostics: how many USB devices the host bus has enumerated (0 = nothing plugged /
// no data connection — a charge-only adapter or the wrong Type-C port look like this).
int nv_usb_audio_bus_devices(void);

#ifdef __cplusplus
}
#endif
