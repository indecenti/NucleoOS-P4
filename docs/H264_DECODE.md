# H.264 decoding on the ESP32-P4 — what is actually possible

Status: **decoding is possible and officially supported**; it is disabled in NucleoOS by choice.
This file records the evidence so the question does not get re-litigated from memory.

Verified 2026-09-10 against `managed_components/espressif__esp_h264` v1.3.6 and ESP-IDF v5.5.2.

## 1. What the silicon has

| Block | Direction | Availability on P4 |
| --- | --- | --- |
| H.264 hardware **encoder** | encode | Yes — used by the camera (`components/nv_camera/nv_camera.c:604`, `esp_h264_enc_hw_new`, RGB565_LE in, 1080p30) |
| H.264 hardware **decoder** | decode | **Does not exist.** No HW decoder block, no HAL, no registers. `esp_h264/hw/` is encoder-only. |
| JPEG codec | both | Yes (HW) — this is why MJPEG-AVI is the fast path |
| PPA / 2D-DMA | color convert + scale | Yes, including `YUV420 -> RGB565` (BT.601/709) |

So decoding is **software only**. That is a limit, not a blocker.

## 2. What the software decoder can do

`esp_h264` ships two prebuilt static libs per target. Symbol dump of the P4 ones:

- `sw/libs/esp32p4/libopenh264.a` — exports `WelsCreateSVCEncoder`, `WelsDestroySVCEncoder`,
  `WelsCreateVpInterface`. **No `WelsCreateDecoder`, no `ISVCDecoder`.** openh264 on P4 is
  **encoder-only**.
- `sw/libs/esp32p4/libtinyh264.a` — exports the full `h264bsd*` symbol set. This is the decoder.

tinyH264 = h264bsd. Per the component README:

- profile: **constrained baseline only** (profile_idc 66, CAVLC, no B-frames, 1 slice group)
- output raw format: **`ESP_H264_RAW_FMT_I420` only** (planar Y, then U, then V)
- SPS/PPS/LTR/MMCO/ref-list-modification supported
- dual-task decode supported (`CONFIG_ESP_H264_DUAL_TASK`, core + priority configurable)

Espressif's own P4 numbers (README "Performance", ESP32-P4):

| Resolution | mono task | dual task | Memory |
| --- | --- | --- | --- |
| 640x480 | 25 fps | **31 fps** | 2.5 MB |
| 1280x720 | 7 fps | 10 fps | 6.2 MB |

**Our own measurement contradicts that table.** On this board (2026-07-05, same session that
measured the MPEG-1 ladder) the H.264 baseline path ran at **~3.5 fps at 480x270** — an order of
magnitude below the README. Unresolved which side is right; plausible causes on our side: the run
predates `CONFIG_ESP_H264_DUAL_TASK=y` / `ESP_H264_DECODER_IRAM=y` being set, and it included the
SW YUV->RGB565 convert and SD read in the same loop. Espressif's number is decode-only.

So: treat "640x480 @ 31 fps" as an upper bound to be re-measured, not a fact. What is certain is
that ~1280x720 realtime is out of reach either way.

## 3. Why NucleoOS keeps it off

Gate: `CONFIG_NV_VPLAYER_H264`, `components/nv_vplayer/Kconfig`, `default n`, not set in
`sdkconfig`. With it off, `.mp4`/`.h264` are not recognised as video
(`nv_vplayer.c:1322` `nv_vplayer_is_video`) and open reports
`"Formato non supportato (usa MJPEG-AVI o MPEG-1)"` (`nv_vplayer.c:1178`).

Two independent reasons, of very different weight:

### 3a. Content reason (real, permanent)

Nearly every real-world `.mp4` is **High profile (100)** or Main (77), with CABAC and B-frames.
h264bsd rejects those on the first slice. Since openh264's decoder is not in the P4 lib, there is
no fallback. The player already gates on `profile_idc` (`nv_vplayer.c:823-827`) so a High-profile
file fails loudly instead of playing audio over a black canvas.
=> A user dropping a phone/YouTube MP4 on the SD card would see a failure ~always.
This is the reason the format is not advertised.

### 3b. Engineering reason (ours, fixable)

The I420 -> RGB565 render goes through PPA SRM with `PPA_SRM_COLOR_MODE_YUV420` input
(`nv_vplayer.c:1260`). On hardware this **hung the whole UI**, which is why the MPEG-1 path was
deliberately built to publish RGB565 instead (`nv_vplayer.c:1064-1066`, `1113`).

Mechanism of the hang, now identified:
`ppa_do_scale_rotate_mirror()` in `PPA_TRANS_MODE_BLOCKING` ends in
`xSemaphoreTake(trans_elm->sem, portMAX_DELAY)` —
`esp-idf/components/esp_driver_ppa/src/ppa_core.c:458-460`. The semaphore is only given from the
2D-DMA RX `on_recv_eof` ISR. If the transaction never raises EOF, the caller blocks **forever**,
with no timeout. `nv_vplayer_render()` runs on the UI side, so the whole OS freezes.

The driver's own argument checks (even `pic_w/h`, `block_w/h`, `block_offset_x/y` for YUV420 —
`ppa_srm.c:183-199`) return `ESP_ERR_INVALID_ARG`, not a hang, so the failing configuration is
something the driver does not validate: most likely the I420 plane layout / `in.buffer` alignment
expected by 2D-DMA in dscr-port mode, not the even-ness rules.

## 4. If we want it back

Ordered by cost:

1. **Drop PPA from the H.264 path.** Convert I420 -> RGB565 on the CPU and publish with
   `s_is_yuv = false`, so it rides the already-working MJPEG/MPEG-1 render path.
   `mpeg1_to_565()` (`nv_vplayer.c`) is already exactly this converter for planar I420 — it needs
   only a variant taking contiguous `Y|U|V` with stride = width instead of `plm_frame_t`.
   Removes the hang class entirely. Cost: CPU, on top of an already CPU-bound decode.
2. **Or fix the PPA call** with `PPA_TRANS_MODE_NON_BLOCKING` + a bounded wait on our own
   semaphore, so a bad transaction degrades to a dropped frame instead of a dead OS. Worth doing
   for the MJPEG path too, on principle.
3. **Ship a transcoder-side rule**: document "Constrained Baseline only" and provide the ffmpeg
   line (`-c:v libx264 -profile:v baseline -level 3.0 -x264-params cabac=0:bframes=0`), the same
   way MPEG-1 clips are prepared today.

Cost of turning the gate on, from the Kconfig help: ~22 KB internal SRAM (IRAM-placed decoder),
~85 KB flash, 516 KB PSRAM per open.

## 5. Bottom line

- "The P4 cannot decode H.264" is **wrong**.
- "The P4 has no H.264 hardware decoder" is right; software does ~640x480@31fps.
- "We cannot play normal MP4s" is right, and is the actual product reason the gate is off.
- The UI hang is our PPA usage, not a platform limit, and item 4.1 above sidesteps it.
