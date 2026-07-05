// nv_vplayer — video playback engine for NucleoOS Anima. A decode task pulls frames from a file
// and publishes the latest decoded RGB565 frame through a lock-free 3-buffer ring, so the UI
// always draws the freshest frame and naturally drops stale ones (smooth catch-up under load).
//
// v1: Motion-JPEG in AVI (.avi) + raw MJPEG (.mjpeg), decoded by the P4 HARDWARE JPEG decoder →
// smooth at full resolution. H.264/MP4 (software decode, low-res) is a planned second path.
#pragma once
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    NV_VP_STOPPED = 0,
    NV_VP_PLAYING,
    NV_VP_PAUSED,
    NV_VP_ERROR,
} nv_vp_state_t;

// Start the engine (idempotent). Allocates the HW JPEG decoder lazily on first open.
void nv_vplayer_init(void);

// Decode + play `path`. Replaces any current clip. Returns false if it can't start.
bool nv_vplayer_open(const char *path);

void nv_vplayer_pause(bool on);
void nv_vplayer_stop(void);

// Seek to `pos_ms` in the current clip. MP4: snaps to the nearest keyframe at or before pos_ms
// (GOP-granular, not frame-exact — avoids decode-and-discard cost). AVI/MJPEG: frame-accurate (every
// frame is independently decodable) when the file has an idx1 index, else a linear scan from the
// start. Returns false if nothing is open or the format has no index (rare, falls back gracefully).
bool nv_vplayer_seek(int pos_ms);

// True if the current MP4 clip has an AAC audio track being decoded/played (always false for .avi —
// AVI audio is out of scope, our own camera recordings never captured a mic track anyway).
bool nv_vplayer_has_audio(void);

nv_vp_state_t nv_vplayer_state(void);
int  nv_vplayer_pos_ms(void);   // audio-clock position when has_audio, else the frame-counted clock
int  nv_vplayer_dur_ms(void);
int  nv_vplayer_fps10(void);   // measured decode framerate x10 (0 until frames flow)
int  nv_vplayer_period_ms(void);   // SOURCE frame period in ms (for an even-paced display task); 0 if unknown

// Human-readable cause when state == NV_VP_ERROR (e.g. "Profilo H.264 High non supportato — serve
// Baseline"). "" when there is no error. Valid to call any time; the pointer is a static string.
const char *nv_vplayer_err_reason(void);

// True and self-clears when the clip reached its end since the last poll (for UI auto-stop/loop).
bool nv_vplayer_took_eot(void);

// Latest decoded frame (RGB565). Returns the buffer and fills w/h + a generation counter that
// bumps on every new frame; returns NULL before the first frame. The buffer is valid until two
// further frames decode — the caller (UI thread) must consume it promptly (a PPA blit is ~ms).
const uint8_t *nv_vplayer_frame(int *w, int *h, uint32_t *generation);

// Aspect / scaling mode applied by nv_vplayer_render():
//   FIT     — preserve aspect ratio, letterbox (black bars). Default, matches VLC "Fit".
//   STRETCH — fill the whole target, ignore aspect ratio (independent per-axis scale).
//   ZOOM    — fill the whole target preserving aspect ratio by cropping the overflow.
// The caller should clear its canvas buffer to black once when switching modes (FIT/ZOOM leave the
// previous frame's pixels outside the new blit rect otherwise). Changed live, applies next render.
typedef enum { NV_VP_FIT = 0, NV_VP_STRETCH, NV_VP_ZOOM } nv_vp_aspect_t;
void nv_vplayer_set_aspect(nv_vp_aspect_t mode);

// Frame-ready callback: invoked from the DECODE task the instant a new RGB565 frame is published
// (MJPEG + MPEG-1 paths only; not the I420/H.264 path). Lets the app blit straight to the panel
// frame-accurately (at the real 24/30 fps decode cadence) instead of polling on a slower UI timer —
// kills the beat-frequency judder between the video rate and the UI tick. The pointer is valid only
// for the duration of the call (the ring reuses it two frames later); do the blit, don't stash it.
// MUST be cleared (pass NULL) before teardown so no callback fires into a freed UI. Runs off the
// LVGL thread — the callee must NOT touch LVGL objects; read cached geometry instead.
typedef void (*nv_vp_frame_cb_t)(const uint8_t *rgb565, int w, int h);
void nv_vplayer_set_frame_cb(nv_vp_frame_cb_t cb);

// PPA-scale the latest frame into `dst` (RGB565, dst_w x dst_h). Returns false if no frame yet.
// Call from the UI thread on a timer — mirrors nv_camera_render().
bool nv_vplayer_render(uint8_t *dst, int dst_w, int dst_h);

// Stop playback and free the decoder + ~6 MB of frame buffers (call on app close to return RAM).
void nv_vplayer_release(void);

// Extension sniff: does this path look like a playable video?
bool nv_vplayer_is_video(const char *path);

#ifdef __cplusplus
}
#endif
