// nv_vplayer — video playback engine for NucleoOS Anima. Decoded RGB565 frames go through a
// lock-free 3-buffer ring; each one is published at its presentation time on a per-clip media clock
// (wall clock, slaved to the audio track when there is one), and a frame that can no longer make its
// time is dropped — playback holds real time instead of turning into slow motion.
//
// Formats:
//   .avi              Motion-JPEG on the P4 HARDWARE JPEG decoder (camera recordings; up to 1280x720),
//                     optional PCM audio track (8/16-bit, mono/stereo). A reader task streams frames
//                     from the card with sector-aligned DMA reads in parallel with the HW decode.
//   .mpg .mpeg .m1v   MPEG-1 + MP2 (pl_mpeg, software): video decode alone on core 1; audio decode,
//                     YUV->RGB565 and presentation on core 0.
//   .mp4 .h264        only with CONFIG_NV_VPLAYER_H264 (off: unverified path).
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

// True while the current clip's audio track is being played (AVI PCM, MPEG-1 MP2, MP4 AAC).
bool nv_vplayer_has_audio(void);

nv_vp_state_t nv_vplayer_state(void);
int  nv_vplayer_pos_ms(void);   // the clip's presentation clock (audio-slaved when it has audio)
int  nv_vplayer_dur_ms(void);
int  nv_vplayer_fps10(void);   // measured presentation framerate x10 (0 until frames flow)
// Frames shown / skipped to hold the clock since the clip opened (diagnostics).
void nv_vplayer_stats(uint32_t *shown, uint32_t *dropped);
int  nv_vplayer_period_ms(void);   // SOURCE frame period in ms (for an even-paced display task); 0 if unknown

// Human-readable cause when state == NV_VP_ERROR (e.g. "Profilo H.264 High non supportato — serve
// Baseline"). "" when there is no error. Valid to call any time; the pointer is a static string.
const char *nv_vplayer_err_reason(void);

// True and self-clears when the clip reached its end since the last poll (for UI auto-stop/loop).
bool nv_vplayer_took_eot(void);

// Latest published frame (RGB565). Returns the buffer and fills the visible w/h, the row pitch in
// pixels (>= w: the HW JPEG decoder pads rows to whole MCUs) and a generation counter that bumps on
// every new frame; NULL before the first frame. The buffer is valid until two further frames
// publish — consume it promptly (a PPA blit is ~ms). Frames are already in memory (no cache sync
// needed before a DMA read).
const uint8_t *nv_vplayer_frame(int *w, int *h, int *pitch, uint32_t *generation);

// Same, and reserve the frame until nv_vplayer_frame_release(): the decoder never writes into a
// reserved ring slot, so a slow blit can't tear. One holder at a time (the display task).
const uint8_t *nv_vplayer_frame_acquire(int *w, int *h, int *pitch, uint32_t *generation);
void nv_vplayer_frame_release(void);

// Block until the next frame is published (true) or `timeout_ms` passes (false). One waiter: the
// display task that blits frames to the panel the moment they're due.
bool nv_vplayer_wait_frame(int timeout_ms);

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
