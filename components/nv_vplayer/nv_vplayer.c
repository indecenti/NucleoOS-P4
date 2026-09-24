// nv_vplayer — see header. MJPEG/AVI engine on the P4 hardware JPEG decoder, H.264/MP4 on the
// esp_h264 software decoder. MP4 also carries an AAC audio track (esp_audio_codec simple decoder)
// played through nv_audio, with the video loop pacing itself against the audio clock, and real
// seek (MP4: nearest keyframe via stss; AVI: frame-accurate via the idx1 index).
//
// SECURITY: files played here are reachable over the LAN with no auth (/api/media|video/play write
// arbitrary files via /api/fs/write, then play them) — every box/table field below is
// attacker-controlled. All array counts (stsc/stco/stsz/stss entries) are clamped to what actually
// fits inside the loaded moov buffer BEFORE they drive any indexed read, and every fixed-offset
// field read is bounds-checked first — mirrors the discipline already applied to the pre-existing
// MP4 avcC/stsz/stco reads (see the VP_NEED-style checks folded into in_bounds()/build_sample_table
// below) rather than trusting declared sizes.
#include "nv_vplayer.h"
#include "sdkconfig.h"
#include "nv_sd.h"     // removal-safe fopen/fclose: card pull mid-decode must not free the volume under us
#include "nv_log.h"
#include "nv_audio.h"

#include "driver/jpeg_decode.h"
#include "driver/ppa.h"
#include "esp_cache.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#if CONFIG_NV_VPLAYER_H264
#include "esp_h264_dec_sw.h"   // software H.264 decoder (dual-core, P4-optimized lib)
#endif
#include "pl_mpeg.h"           // MPEG-1 software decoder (declarations; impl in nv_mpeg1_impl.c)

#include "esp_audio_simple_dec.h"
#include "esp_audio_simple_dec_default.h"
#include "esp_audio_dec_default.h"   // core codec layer — must be registered too (see ensure_audio_codecs)
#include "esp_aac_dec.h"             // esp_aac_dec_cfg_t (raw, no-ADTS AAC access-units from MP4 mp4a/esds)

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"

#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <sys/stat.h>

static const char *TAG = "vplayer";

#define VP_MAXW    1280
#define VP_MAXH    736               // 720p plus the decoder's MCU row padding
#define VP_RING    3
#define VP_IN_CAP  (512 * 1024)      // one compressed JPEG frame
#define VP_ISLOTS  3                 // AVI read-ahead slots: the reader fills, the decoder drains
#define VP_SECT    512
// SDMMC reads straight into the caller's PSRAM buffer (multi-block DMA) only when the pointer AND
// length are cache-line aligned and the file position is sector aligned; anything else falls back
// to one 512-byte sector per command through a bounce buffer (sdmmc_cmd.c) — ~10x slower. So every
// bulk read below is issued on sector boundaries into a 128-byte-aligned slot, and the payload is
// used in place at slot + (offset & 511) (the JPEG decoder has no input alignment rule).
#define VP_ALIGN   128
#define VP_SLOT_CAP (VP_IN_CAP + 2 * VP_SECT)

typedef enum { VP_CMD_OPEN, VP_CMD_PAUSE, VP_CMD_RESUME, VP_CMD_STOP, VP_CMD_RELEASE } vp_cmd_t;
typedef struct { vp_cmd_t cmd; char path[256]; } vp_msg_t;

static jpeg_decoder_handle_t s_dec = NULL;
static uint8_t  *s_islot[VP_ISLOTS] = {0};            // aligned sector-read buffers (AVI pipeline)
static uint8_t  *s_in = NULL;   size_t s_in_cap = 0;  // = s_islot[0] for the single-buffer paths
static uint8_t  *s_annex = NULL;   size_t s_annex_cap = 0;   // Annex-B assembly (H.264/MP4)
static uint8_t  *s_ring[VP_RING] = {0};   size_t s_ring_len = 0;
static int       s_widx = 0;
static ppa_client_handle_t s_vp_ppa = NULL;   // cached SRM client (registered once, not per frame)
static size_t    s_cache_align = 64;          // PSRAM cache line (queried once)

static volatile int      s_cur = -1;       // published ring index
static volatile int      s_w = 0, s_h = 0, s_pitch = 0;   // published frame: visible size + row pitch (px)
static volatile uint32_t s_gen = 0;
static volatile nv_vp_state_t s_state = NV_VP_STOPPED;
static volatile int      s_pos_ms = 0, s_dur_ms = 0;
static volatile int      s_period_ms = 0;   // source frame period (even-pace hint for the display task)
static volatile bool     s_eot = false;
static bool              s_dec_err_logged = false;   // one-shot per open: silence repeat decode-error spam
static const char       *s_err_reason = "";           // human-readable cause shown by the UI on NV_VP_ERROR
                                                       // (static string literals only -> pointer store is atomic)
static volatile bool     s_playing = false;   // true while inside a play_* (release waits on it)
static volatile bool     s_is_yuv = false;    // published frames are I420 (H.264) vs RGB565 (MJPEG)
static nv_vp_frame_cb_t  s_frame_cb = NULL;    // decode-thread frame-ready hook (RGB565 paths)
static SemaphoreHandle_t s_frame_sem = NULL;   // given on every publish -> nv_vplayer_wait_frame()
void nv_mpeg1_set_int_budget(size_t bytes);    // nv_mpeg1_impl.c: internal-SRAM budget for frame planes
int  nv_plm_seek_audio(plm_t *self, double time);   // nv_mpeg1_impl.c: audio-only seek (plm_seek needs video)
static volatile int      s_fps10 = 0;         // measured presentation rate x10 (EMA)
static int64_t           s_last_pub_us = 0;
static volatile uint32_t s_drops = 0;         // frames skipped to hold the clock (late decode / slow card)
static volatile uint32_t s_shown = 0;         // frames published this clip
static volatile uint32_t s_t_rd_us = 0, s_n_rd = 0;     // stage timing (diagnostics): card reads
static volatile uint32_t s_t_dec_us = 0, s_n_dec = 0;   //   and HW JPEG decodes since the last report

// ---- seek + audio-track state (shared across the active play_* function and the audio task) -------
static volatile int  s_vseek_ms = -1;      // set by nv_vplayer_seek(); consumed+cleared by the play_* loop
static volatile int  s_audio_seek_ms = -1; // mirrored for the audio task; consumed+cleared there
static volatile bool s_has_audio = false;  // true while the clip's audio track is actively playing
static volatile bool s_audio_stop_flag = false;  // video loop -> audio task: wind down now
static volatile bool s_audio_running = false;    // audio task: true from start until just before exit
static volatile bool s_audio_live = false;       // audio task owns the sink and is producing a clock
static volatile int  s_audio_pos_ms = 0;         // audio clock (backlog-corrected), the A/V sync master
// Every audio task belongs to one clip: it only touches the shared audio state (and keeps running)
// while s_audio_gen still carries the value it was started with, so a task that outlives its clip
// (stuck in a sink call) can't steer the next clip's clock or be mistaken for its audio.
static volatile uint32_t s_audio_gen = 0;
// Seek handshake: the video side bumps s_seek_seq after posting a seek; the audio task copies the
// value it has caught up with into s_audio_seq. Until they match, an audio position computed
// before the seek is stale and must neither be stored nor followed.
static volatile uint32_t s_seek_seq = 0, s_audio_seq = 0;
#define AUDIO_MINE(g) (!s_audio_stop_flag && (g) == s_audio_gen)
// Ring slot the display task is blitting (-1 none): the producers never write into it.
static volatile int  s_hold = -1;

// ---- presentation clock ---------------------------------------------------------------------------
// One media clock per clip: a wall clock (esp_timer) that pause freezes and seek re-bases, slaved to
// the audio clock while an audio track plays. The audio clock itself is too coarse to pace video (the
// feeder drains the ring in 20-80 ms bursts), so the wall clock is nudged toward it instead.
static portMUX_TYPE s_clk_mux = portMUX_INITIALIZER_UNLOCKED;   // 64-bit clock state, touched from both cores
static int64_t s_clk_t0_us = 0;       // esp_timer value at media time 0
static int64_t s_clk_pause_us = 0;    // esp_timer at pause start, 0 = running
static volatile bool s_clk_on = false; // a clocked path (AVI / MPEG-1) is running: pos = the clock

static int clk_ms(void){
    const int64_t now_us = esp_timer_get_time();
    taskENTER_CRITICAL(&s_clk_mux);
    const int64_t now = s_clk_pause_us ? s_clk_pause_us : now_us;
    const int ms = (int)((now - s_clk_t0_us) / 1000);
    taskEXIT_CRITICAL(&s_clk_mux);
    return ms;
}
static void clk_set(int ms){
    const int64_t now = esp_timer_get_time();
    taskENTER_CRITICAL(&s_clk_mux);
    s_clk_t0_us = now - (int64_t)ms * 1000;
    if (s_clk_pause_us) s_clk_pause_us = now;
    taskEXIT_CRITICAL(&s_clk_mux);
}
static void clk_pause(bool on){
    const int64_t now = esp_timer_get_time();
    taskENTER_CRITICAL(&s_clk_mux);
    if (on && !s_clk_pause_us) s_clk_pause_us = now;
    else if (!on && s_clk_pause_us) { s_clk_t0_us += now - s_clk_pause_us; s_clk_pause_us = 0; }
    taskEXIT_CRITICAL(&s_clk_mux);
}
// Pull the wall clock onto the audio clock: jump when far off (start/seek/sink hiccup), else slew.
// Ignored while the audio hasn't caught up with the latest seek (its position is from before it).
static void clk_follow_audio(void){
    if (!s_audio_live || s_audio_seq != s_seek_seq) return;
    const int a = s_audio_pos_ms;
    const int64_t now = esp_timer_get_time();
    taskENTER_CRITICAL(&s_clk_mux);
    if (!s_clk_pause_us) {
        const int diff = a - (int)((now - s_clk_t0_us) / 1000);   // >0: audio ahead of the picture
        if (diff > 120 || diff < -120) s_clk_t0_us -= (int64_t)diff * 1000;
        else                           s_clk_t0_us -= (int64_t)diff * 1000 / 8;
    }
    taskEXIT_CRITICAL(&s_clk_mux);
}
// Post a seek to the clip's audio task (see s_seek_seq).
static void audio_seek_post(int ms){
    s_audio_pos_ms = ms;
    __atomic_store_n(&s_audio_seek_ms, ms, __ATOMIC_RELEASE);
    __atomic_add_fetch(&s_seek_seq, 1, __ATOMIC_ACQ_REL);
}
// Audio clock update from the audio task: only for its own clip and only once it has caught up
// with the latest seek.
static void audio_pos_store(uint32_t gen, uint64_t written, size_t backlog, uint32_t out_bps){
    if (gen != s_audio_gen || s_audio_seq != s_seek_seq || !out_bps) return;
    const int64_t heard = (int64_t)written - (int64_t)backlog;
    s_audio_pos_ms = heard > 0 ? (int)(heard * 1000 / out_bps) : 0;
}
// Open the sink for a clip's audio, but never park on it: another stream (a paused music track)
// may own it for minutes. Retry in short bounded waits so stop/next stay instant; after ~3 s play
// the clip silent on the wall clock.
static bool audio_begin(int rate, int ch, uint32_t gen){
    for (int tries = 0; tries < 15 && AUDIO_MINE(gen); tries++)
        if (nv_audio_pcm_begin_timeout(rate, ch, 16, 200)) return true;
    if (AUDIO_MINE(gen)) NV_LOGW(TAG, "audio sink busy or unavailable: playing silent");
    return false;
}
// Wait until the display task is not blitting ring slot `slot` (it holds one frame for ~1-45 ms).
static void ring_wait_free(int slot){
    for (int i = 0; i < 200 && s_hold == slot; i++) vTaskDelay(1);
}

// Update the measured presentation fps (EMA) — called on every published frame.
static void note_frame(void){
    int64_t now = esp_timer_get_time();
    if (s_last_pub_us) {
        int64_t dt = now - s_last_pub_us;
        if (dt > 0) {
            int inst = (int)(10000000 / dt);                 // fps x10
            s_fps10 = s_fps10 ? (s_fps10 * 7 + inst) / 8 : inst;
        }
    }
    s_last_pub_us = now;
    s_shown++;
}

// Make ring slot `slot` (w x h visible, `pitch` px per row) the current frame and wake the display.
static void publish(int slot, int w, int h, int pitch){
    s_w = w; s_h = h; s_pitch = pitch;
    s_cur = slot; s_gen++;
    note_frame();
    if (s_frame_sem) xSemaphoreGive(s_frame_sem);
    if (s_frame_cb) s_frame_cb(s_ring[slot], w, h);
}

static QueueHandle_t s_q    = NULL;
static TaskHandle_t  s_task = NULL;
static SemaphoreHandle_t s_release_sem = NULL;   // vp_task -> release(): "risorse liberate"

// ---------------------------------------------------------------- resources
// Shared buffers (aligned sector-read slots + Annex-B assembly + the RGB565/I420 display ring). The
// ring is DMA-capable (jpeg allocator) so both the JPEG decoder DMA and PPA can touch it.
static bool ensure_ring(void){
#ifdef CONFIG_CACHE_L2_CACHE_LINE_SIZE
    s_cache_align = CONFIG_CACHE_L2_CACHE_LINE_SIZE;   // what the driver's alignment checks use for PSRAM
#else
    s_cache_align = 128;
#endif
    for (int i = 0; i < VP_ISLOTS; i++) {
        if (s_islot[i]) continue;
        s_islot[i] = (uint8_t *)heap_caps_aligned_calloc(VP_ALIGN, 1, VP_SLOT_CAP, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!s_islot[i]) { NV_LOGE(TAG,"input slot %d alloc failed", i); return false; }
    }
    s_in = s_islot[0]; s_in_cap = VP_IN_CAP;
#if CONFIG_NV_VPLAYER_H264
    if (!s_annex) {   // Annex-B assembly buffer: only the H.264 path uses it (516 KB PSRAM)
        s_annex = (uint8_t *)heap_caps_malloc(VP_IN_CAP + 4096, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!s_annex) { NV_LOGE(TAG,"annex buffer alloc failed"); return false; }
        s_annex_cap = VP_IN_CAP + 4096;
    }
#endif
    if (!s_ring[0]) {
        jpeg_decode_memory_alloc_cfg_t om = { .buffer_direction = JPEG_DEC_ALLOC_OUTPUT_BUFFER };
        for (int i=0;i<VP_RING;i++) {
            size_t got=0;
            s_ring[i] = (uint8_t *)jpeg_alloc_decoder_mem((size_t)VP_MAXW*VP_MAXH*2, &om, &got);
            if (!s_ring[i]) { NV_LOGE(TAG,"ring %d alloc failed",i); return false; }
            s_ring_len = got;
        }
    }
    return true;
}
static bool ensure_engine(void){   // MJPEG needs the HW JPEG decoder on top of the ring
    if (!ensure_ring()) return false;
    if (!s_dec) {
        jpeg_decode_engine_cfg_t eng = { .intr_priority = 0, .timeout_ms = 100 };
        if (jpeg_new_decoder_engine(&eng, &s_dec) != ESP_OK) { s_dec = NULL; NV_LOGE(TAG,"HW JPEG decoder unavailable"); return false; }
    }
    return true;
}

// ---------------------------------------------------------------- one JPEG -> ring slot
// The decoder writes whole MCUs (16 px for 4:2:0/4:2:2, 8 for 4:4:4/grey; rows likewise for 4:2:0),
// so a frame sits in the slot with a row pitch >= its width. Geometry is parsed once per clip (an
// MJPEG stream keeps its size) and re-parsed only if a frame's output size says it changed. The
// output length handed to the driver is the exact frame, not the whole slot: the driver invalidates
// the cache over that length on every frame (it was 1.9 MB per frame).
static int      s_jw = 0, s_jh = 0, s_jpitch = 0, s_jrows = 0;
static uint32_t s_jneed = 0;
static bool jpeg_geometry(const uint8_t *jpg, uint32_t len){
    jpeg_decode_picture_info_t info;
    if (jpeg_decoder_get_info(jpg, len, &info) != ESP_OK) return false;
    const int w = (int)info.width, h = (int)info.height;
    if (w <= 0 || h <= 0 || w > VP_MAXW) return false;
    const int mcu_w = (info.sample_method == JPEG_DOWN_SAMPLING_YUV444 ||
                       info.sample_method == JPEG_DOWN_SAMPLING_GRAY) ? 8 : 16;
    const int mcu_h = (info.sample_method == JPEG_DOWN_SAMPLING_YUV420) ? 16 : 8;
    const int pitch = (w + mcu_w - 1) / mcu_w * mcu_w;
    const int rows  = (h + mcu_h - 1) / mcu_h * mcu_h;
    const uint32_t need = (uint32_t)((((size_t)pitch * rows * 2) + s_cache_align - 1) & ~(s_cache_align - 1));
    if (need > s_ring_len) return false;
    s_jw = w; s_jh = h; s_jpitch = pitch; s_jrows = rows; s_jneed = need;
    return true;
}
static bool jpeg_decode_slot(int slot, const uint8_t *jpg, uint32_t len){
    if (!s_jneed && !jpeg_geometry(jpg, len)) return false;
    jpeg_decode_cfg_t cfg = {
        .output_format = JPEG_DECODE_OUT_FORMAT_RGB565,
        // BGR = RGB565 low byte first ("small endian" in jpeg_types.h), what LVGL and the panel
        // blit expect. RGB (high byte first) played every MJPEG clip as coloured static.
        .rgb_order = JPEG_DEC_RGB_ELEMENT_ORDER_BGR,
        .conv_std = JPEG_YUV_RGB_CONV_STD_BT601,
    };
    uint32_t outsz = 0;
    if (jpeg_decoder_process(s_dec, &cfg, jpg, len, s_ring[slot], s_jneed, &outsz) != ESP_OK) {
        // a bigger frame than the clip's first one: re-read the geometry and try once more
        if (!jpeg_geometry(jpg, len)) return false;
        if (jpeg_decoder_process(s_dec, &cfg, jpg, len, s_ring[slot], s_jneed, &outsz) != ESP_OK) return false;
    }
    if (outsz != (uint32_t)s_jpitch * s_jrows * 2 && !jpeg_geometry(jpg, len)) return false;
    return true;
}


// ---------------------------------------------------------------- aligned sector reads
// Read [off, off+len) as whole sectors into the aligned `buf`; returns the payload pointer inside it
// (NULL on a short read / oversize). With the FILE unbuffered (_IONBF) this is one f_read that FATFS
// hands to the SD driver as multi-block DMA straight into `buf` (see VP_ALIGN).
// Positioned read straight through the file descriptor. NOT stdio: on an unbuffered FILE every
// fread first walks (and locks) every open stream to flush line-buffered output — measured ~600 ms
// per 44 KB frame against 3.5 ms for the same read through read().
static size_t rd_at(FILE *f, long off, void *dst, size_t n){
    const int fd = fileno(f);
    if (fd < 0 || lseek(fd, (off_t)off, SEEK_SET) != (off_t)off) return 0;
    size_t got = 0;
    while (got < n) {
        const ssize_t r = read(fd, (uint8_t *)dst + got, n - got);
        if (r <= 0) break;
        got += (size_t)r;
    }
    return got;
}
static long file_size(FILE *f){
    struct stat st;
    return (fstat(fileno(f), &st) == 0) ? (long)st.st_size : 0;
}
// Big cache-aligned stdio buffer for the pl_mpeg streams: its reads are 4 KB freads, and the default
// 1 KB FILE buffer turned each into read()s of two sectors through the SD bounce path, hundreds of
// commands a second inside the decode thread. With 64 KB the card sees one multi-block DMA per 64 KB.
#define VP_STDIO_BUF (64 * 1024)
static char *stdio_big_buffer(FILE *f){
    char *b = (char *)heap_caps_aligned_alloc(VP_ALIGN, VP_STDIO_BUF, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (b && setvbuf(f, b, _IOFBF, VP_STDIO_BUF) != 0) { heap_caps_free(b); b = NULL; }
    return b;   // free it only after the FILE is closed
}

static const uint8_t *read_span(FILE *f, uint8_t *buf, size_t cap, uint32_t off, uint32_t len){
    const uint32_t a0 = off & ~(uint32_t)(VP_SECT - 1);
    const uint64_t a1 = ((uint64_t)off + len + VP_SECT - 1) & ~(uint64_t)(VP_SECT - 1);
    const size_t n = (size_t)(a1 - a0);
    if (!len || n > cap) return NULL;
    const size_t got = rd_at(f, (long)a0, buf, n);    // the last sector may run past EOF: short is fine
    if (got < (size_t)(off - a0) + len) return NULL;
    return buf + (off - a0);
}

// ---------------------------------------------------------------- AVI: header
// Everything read here is untrusted (files reach the SD over the LAN with no auth): every chunk
// stride is bounded by its parent and the file size, so a hostile size can neither wrap the `long`
// arithmetic nor park the scan on one spot (see avi_index_scan).
typedef struct {
    long     movi_pos;            // first byte after the 'movi' fourcc
    long     movi_end;            // end of the movi payload (EOF for an unfinished recording)
    long     fsz;
    uint32_t uspf, total, vw, vh; // avih
    uint32_t v_scale, v_rate;     // video strh time base: fps = rate / scale
    uint32_t v_fcc;               // video compression fourcc (strh handler or BITMAPINFOHEADER)
    int      v_stream, a_stream;  // stream numbers (the "00" in "00dc"); -1 = none
    uint16_t a_tag, a_ch, a_bits, a_align;
    uint32_t a_rate, a_bps;       // audio sample rate, bytes/second
    int      nstreams;
    long     v_indx, a_indx;      // OpenDML super index payload offsets ('indx' in the strl), 0 = none
    uint32_t v_indx_sz, a_indx_sz;
} vp_avi_t;

static uint32_t le32(const uint8_t *p){ return (uint32_t)p[0]|((uint32_t)p[1]<<8)|((uint32_t)p[2]<<16)|((uint32_t)p[3]<<24); }
static uint16_t le16(const uint8_t *p){ return (uint16_t)(p[0]|(p[1]<<8)); }
#define FCC(a,b,c,d) ((uint32_t)(a)|((uint32_t)(b)<<8)|((uint32_t)(c)<<16)|((uint32_t)(d)<<24))

// One stream list ('strl'): strh + strf.
static void avi_parse_strl(FILE *f, long p, long end, vp_avi_t *A){
    const int sn = A->nstreams++;
    uint32_t type = 0, handler = 0, scale = 0, rate = 0;
    long indx = 0; uint32_t indx_sz = 0;
    while (p + 8 <= end) {
        uint8_t h[48] = {0};
        const size_t want = 8 + (sizeof h - 8);
        if (rd_at(f, p, h, want) < 8) return;
        const uint32_t sz = le32(h + 4);
        const int64_t nx = (int64_t)p + 8 + sz + (sz & 1);
        if (nx <= p || nx > end) return;
        uint8_t b[40] = {0};
        memcpy(b, h + 8, sz < sizeof b ? sz : sizeof b);
        if (!memcmp(h, "strh", 4) && sz >= 28) {
            type = le32(b); handler = le32(b + 4); scale = le32(b + 20); rate = le32(b + 24);
        } else if (!memcmp(h, "strf", 4)) {
            if (type == FCC('v','i','d','s') && A->v_stream < 0 && sz >= 20) {
                A->v_stream = sn; A->v_scale = scale; A->v_rate = rate;
                A->v_fcc = le32(b + 16) ? le32(b + 16) : handler;
                if (!A->vw) { A->vw = le32(b + 4); A->vh = le32(b + 8); }
            } else if (type == FCC('a','u','d','s') && A->a_stream < 0 && sz >= 16) {
                A->a_stream = sn;
                A->a_tag = le16(b); A->a_ch = le16(b + 2); A->a_rate = le32(b + 4);
                A->a_bps = le32(b + 8); A->a_align = le16(b + 12); A->a_bits = le16(b + 14);
            }
        } else if (!memcmp(h, "indx", 4)) {
            indx = p + 8; indx_sz = sz;
        }
        p = (long)nx;
    }
    // 'indx' may come after 'strf': attach it once the stream's role is known
    if (indx && A->v_stream == sn) { A->v_indx = indx; A->v_indx_sz = indx_sz; }
    if (indx && A->a_stream == sn) { A->a_indx = indx; A->a_indx_sz = indx_sz; }
}

static bool avi_probe(FILE *f, vp_avi_t *A){
    memset(A, 0, sizeof *A);
    A->v_stream = A->a_stream = -1;
    uint8_t hdr[12];
    A->fsz = file_size(f);
    if (rd_at(f, 0, hdr, 12) != 12 || memcmp(hdr, "RIFF", 4) || memcmp(hdr + 8, "AVI ", 4)) return false;
    long p = 12;
    while (p + 12 <= A->fsz) {
        uint8_t h[12];
        if (rd_at(f, p, h, 12) != 12) break;
        const uint32_t sz = le32(h + 4);
        if (!memcmp(h, "LIST", 4) && !memcmp(h + 8, "movi", 4)) {
            A->movi_pos = p + 12;
            // A recording cut off before its stop (power loss, card pulled) still has the size 0 the
            // recorder writes up front: play to EOF.
            const int64_t e = (int64_t)p + 8 + sz;
            A->movi_end = (sz < 4 || e > A->fsz) ? A->fsz : (long)e;
            return A->v_stream >= 0;
        }
        const int64_t nx = (int64_t)p + 8 + sz + (sz & 1);
        if (nx <= p || nx > A->fsz) break;
        if (!memcmp(h, "LIST", 4) && !memcmp(h + 8, "hdrl", 4)) {
            long q = p + 12;
            while (q + 8 <= nx) {
                uint8_t c[12];
                if (rd_at(f, q, c, 12) < 8) break;
                const uint32_t csz = le32(c + 4);
                const int64_t cn = (int64_t)q + 8 + csz + (csz & 1);
                if (cn <= q || cn > nx) break;
                if (!memcmp(c, "avih", 4) && csz >= 40) {
                    uint8_t a[40];
                    if (rd_at(f, q + 8, a, 40) == 40) {
                        A->uspf = le32(a); A->total = le32(a + 16);
                        A->vw = le32(a + 32); A->vh = le32(a + 36);
                    }
                } else if (!memcmp(c, "LIST", 4) && !memcmp(c + 8, "strl", 4)) {
                    avi_parse_strl(f, q + 12, (long)cn, A);
                }
                q = (long)cn;
            }
        }
        p = (long)nx;
    }
    return false;
}

// ---------------------------------------------------------------- AVI: index
typedef struct { uint32_t off, size; } vp_ent_t;   // absolute payload offset + payload size

// "00dc"/"00db" (video) and "01wb" (audio) chunk ids for a stream number.
static bool is_ck(const uint8_t *id, int stream, char t0, char t1a, char t1b){
    if (stream < 0 || stream > 99) return false;
    return id[0] == '0' + stream / 10 && id[1] == '0' + stream % 10 && id[2] == t0 && (id[3] == t1a || id[3] == t1b);
}

// OpenDML (AVI 2.0, what ffmpeg writes past 1 GB): the stream's 'indx' super index lists 'ix##'
// standard indexes spread over the RIFF-AVI and RIFF-AVIX parts; each gives a 64-bit base plus
// 32-bit offsets to chunk PAYLOADS. idx1 only covers the first ~1 GB part, so this is preferred.
// All counts and offsets are bounded by the file size (a FAT32 file stays below 4 GB).
static bool avi_index_odml(FILE *f, const vp_avi_t *A, long indx, uint32_t indx_sz, vp_ent_t **out, uint32_t *out_n){
    uint8_t h[24];
    if (!indx || indx_sz < 24 || rd_at(f, indx, h, 24) != 24) return false;
    if (le16(h) != 4 || h[3] != 0) return false;                 // wLongsPerEntry 4, AVI_INDEX_OF_INDEXES
    uint32_t nsup = le32(h + 4);
    if (nsup > (indx_sz - 24) / 16) nsup = (indx_sz - 24) / 16;
    if (nsup == 0 || nsup > 4096) return false;
    uint32_t cap = 0, n = 0;
    vp_ent_t *tbl = NULL;
    uint8_t *buf = NULL; size_t bcap = 0;
    bool ok = true;
    for (uint32_t s = 0; s < nsup && ok; s++) {
        uint8_t e[16];
        if (rd_at(f, indx + 24 + (long)s * 16, e, 16) != 16) { ok = false; break; }
        const uint64_t ixo = (uint64_t)le32(e) | ((uint64_t)le32(e + 4) << 32);
        if (ixo + 32 > (uint64_t)A->fsz) { ok = false; break; }
        uint8_t ih[32];
        if (rd_at(f, (long)ixo, ih, 32) != 32 || memcmp(ih, "ix", 2) != 0) { ok = false; break; }
        const uint32_t isz = le32(ih + 4);
        if (le16(ih + 8) != 2 || ih[11] != 1) { ok = false; break; }   // 2 longs/entry, AVI_INDEX_OF_CHUNKS
        uint32_t cnt = le32(ih + 12);
        if (isz < 24 || cnt > (isz - 24) / 8) cnt = isz >= 24 ? (isz - 24) / 8 : 0;
        const uint64_t base = (uint64_t)le32(ih + 20) | ((uint64_t)le32(ih + 24) << 32);
        if (!cnt) continue;
        if (n + cnt > 200000) { ok = false; break; }
        if (n + cnt > cap) {
            uint32_t nc = cap ? cap : 4096;
            while (nc < n + cnt) nc *= 2;
            vp_ent_t *g = (vp_ent_t *)heap_caps_realloc(tbl, (size_t)nc * sizeof(vp_ent_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            if (!g) { ok = false; break; }
            tbl = g; cap = nc;
        }
        const size_t need = (size_t)cnt * 8 + 2 * VP_SECT;
        if (need > bcap) {
            if (buf) heap_caps_free(buf);
            buf = (uint8_t *)heap_caps_aligned_calloc(VP_ALIGN, 1, need, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            bcap = buf ? need : 0;
            if (!buf) { ok = false; break; }
        }
        const uint8_t *raw = read_span(f, buf, bcap, (uint32_t)(ixo + 32), cnt * 8);
        if (!raw) { ok = false; break; }
        for (uint32_t k = 0; k < cnt; k++) {
            const uint64_t off = base + le32(raw + k * 8);
            uint32_t size = le32(raw + k * 8 + 4) & 0x7FFFFFFFu;     // bit 31 = not a keyframe
            if (off >= (uint64_t)A->fsz) size = 0;
            else if (off + size > (uint64_t)A->fsz) size = (uint32_t)((uint64_t)A->fsz - off);
            tbl[n].off = (uint32_t)off; tbl[n].size = size; n++;
        }
    }
    if (buf) heap_caps_free(buf);
    if (!ok || !n) { if (tbl) heap_caps_free(tbl); return false; }
    *out = tbl; *out_n = n;
    return true;
}

// idx1 -> separate video / audio tables. The entry offset base is ambiguous across muxers (the
// 'movi' fourcc, the byte after it, or the file start): take the one whose first entry lands on a
// chunk carrying that entry's own id.
static bool avi_index_idx1(FILE *f, const vp_avi_t *A, vp_ent_t **V, uint32_t *vn, vp_ent_t **Au, uint32_t *an){
    long p = A->movi_end + (A->movi_end & 1);
    for (int guard = 0; guard < 8 && p + 8 <= A->fsz; guard++) {   // idx1 is normally right after movi
        uint8_t h[8];
        if (rd_at(f, p, h, 8) != 8) return false;
        const uint32_t sz = le32(h + 4);
        if (memcmp(h, "idx1", 4) != 0) {
            const int64_t nx = (int64_t)p + 8 + sz + (sz & 1);
            if (nx <= p || nx > A->fsz) return false;
            p = (long)nx;
            continue;
        }
        uint32_t n = sz / 16;
        if ((int64_t)p + 8 + (int64_t)n * 16 > A->fsz) n = (uint32_t)((A->fsz - p - 8) / 16);
        if (n == 0 || n > 400000) return false;          // memory guard on an attacker-set size
        // sector-aligned bulk read (a 1 h clip's idx1 is ~2 MB: unaligned it went 512 B at a time)
        const size_t rcap = (size_t)n * 16 + 2 * VP_SECT;
        uint8_t *rbuf = (uint8_t *)heap_caps_aligned_calloc(VP_ALIGN, 1, rcap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!rbuf) return false;
        const uint8_t *raw = read_span(f, rbuf, rcap, (uint32_t)(p + 8), n * 16);
        if (!raw) { heap_caps_free(rbuf); return false; }
        uint32_t nv = 0, na = 0;
        for (uint32_t k = 0; k < n; k++) {
            if (is_ck(raw + k*16, A->v_stream, 'd', 'c', 'b')) nv++;
            else if (is_ck(raw + k*16, A->a_stream, 'w', 'b', 'b')) na++;
        }
        vp_ent_t *va = nv ? (vp_ent_t *)heap_caps_malloc((size_t)nv * sizeof(vp_ent_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) : NULL;
        vp_ent_t *aa = na ? (vp_ent_t *)heap_caps_malloc((size_t)na * sizeof(vp_ent_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) : NULL;
        if (!va || (na && !aa)) { heap_caps_free(rbuf); if (va) heap_caps_free(va); if (aa) heap_caps_free(aa); return false; }
        // base probe on the first entry of either stream
        long base = -1;
        for (uint32_t k = 0; k < n && base < 0; k++) {
            const uint8_t *e = raw + k*16;
            if (!is_ck(e, A->v_stream, 'd', 'c', 'b') && !is_ck(e, A->a_stream, 'w', 'b', 'b')) continue;
            const long cand[3] = { A->movi_pos - 4, A->movi_pos, 0 };
            for (int c = 0; c < 3; c++) {
                uint8_t id[4];
                const int64_t at = (int64_t)cand[c] + le32(e + 8);
                if (at < 0 || at + 8 > A->fsz) continue;
                if (rd_at(f, (long)at, id, 4) == 4 && !memcmp(id, e, 4)) { base = cand[c]; break; }
            }
            break;
        }
        if (base < 0) { heap_caps_free(rbuf); heap_caps_free(va); if (aa) heap_caps_free(aa); return false; }
        uint32_t iv = 0, ia = 0;
        for (uint32_t k = 0; k < n; k++) {
            const uint8_t *e = raw + k*16;
            const int64_t off = (int64_t)base + le32(e + 8) + 8;      // payload, past the chunk header
            uint32_t size = le32(e + 12);
            if (off < 0 || off > A->fsz) size = 0;                   // hostile offset: unreadable entry
            else if (off + size > A->fsz) size = (uint32_t)(A->fsz - off);
            if (is_ck(e, A->v_stream, 'd', 'c', 'b'))      { va[iv].off = (uint32_t)off; va[iv].size = size; iv++; }
            else if (is_ck(e, A->a_stream, 'w', 'b', 'b')) { aa[ia].off = (uint32_t)off; aa[ia].size = size; ia++; }
        }
        heap_caps_free(rbuf);
        *V = va; *vn = iv; *Au = aa; *an = ia;
        return iv > 0;
    }
    return false;
}

// No idx1 (a recording cut off before its stop): walk the movi chunk headers. One header read per
// chunk; STOP/OPEN in the queue abort it (a long unindexed file must not wedge the player).
static bool avi_index_scan(FILE *f, const vp_avi_t *A, vp_ent_t **V, uint32_t *vn){
    uint32_t cap = 4096, n = 0;
    vp_ent_t *va = (vp_ent_t *)heap_caps_malloc(cap * sizeof(vp_ent_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!va) return false;
    long p = A->movi_pos;
    while (p + 8 <= A->movi_end) {
        uint8_t h[12];
        if (rd_at(f, p, h, 8) != 8) break;
        const uint32_t sz = le32(h + 4);
        if (!memcmp(h, "LIST", 4)) { p += 12; continue; }        // 'rec ' groups: step inside
        const int64_t nx = (int64_t)p + 8 + sz + (sz & 1);
        if (nx <= p || nx > A->movi_end + 1) break;               // torn tail of an unfinished file
        if (is_ck(h, A->v_stream, 'd', 'c', 'b')) {
            if (n == cap) {
                if (cap >= 200000) break;
                vp_ent_t *g = (vp_ent_t *)heap_caps_realloc(va, (size_t)cap * 2 * sizeof(vp_ent_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
                if (!g) break;
                va = g; cap *= 2;
            }
            va[n].off = (uint32_t)(p + 8); va[n].size = sz; n++;
        }
        p = (long)nx;
        if ((n & 255) == 0) {                                     // user moved on? (not pause/resume)
            vp_msg_t pk;
            if (xQueuePeek(s_q, &pk, 0) == pdTRUE && pk.cmd != VP_CMD_PAUSE && pk.cmd != VP_CMD_RESUME) break;
        }
    }
    if (!n) { heap_caps_free(va); return false; }
    *V = va; *vn = n;
    return true;
}

// drain commands; returns true if the current clip should stop (STOP or a new OPEN, requeued).
// Pause/resume also freeze the presentation clock and the audio ring (a true, audible pause).
static bool poll_cmd(void){
    vp_msg_t m;
    while (xQueueReceive(s_q, &m, 0) == pdTRUE) {
        switch (m.cmd) {
            case VP_CMD_STOP:    return true;
            case VP_CMD_OPEN:    xQueueSendToFront(s_q, &m, 0); return true;
            case VP_CMD_RELEASE: xQueueSendToFront(s_q, &m, 0); return true;  // esci dal play, il top-loop libera
            case VP_CMD_PAUSE:
                if (s_state == NV_VP_PLAYING) { s_state = NV_VP_PAUSED; clk_pause(true); if (s_audio_live) nv_audio_pcm_pause(true); }
                break;
            case VP_CMD_RESUME:
                if (s_state == NV_VP_PAUSED) { s_state = NV_VP_PLAYING; clk_pause(false); if (s_audio_live) nv_audio_pcm_pause(false); }
                break;
        }
    }
    return false;
}

// Stop the clip's audio task: unfreeze + flush the ring so a blocked pcm_write returns at once.
static void audio_stop_and_wait(void){
    if (!s_audio_running) return;
    s_audio_stop_flag = true;
    // Only touch the sink if our task actually owns it: while it is still waiting to begin, the
    // live stream is someone else's (a paused music track must not be unpaused and flushed).
    if (s_audio_live) { nv_audio_pcm_pause(false); nv_audio_pcm_flush(); }
    for (int i = 0; s_audio_running; i++) {
        if (i == 600) NV_LOGW(TAG, "audio task slow to stop");
        if (i >= 2000) { NV_LOGE(TAG, "audio task stuck: left to exit on its own"); break; }   // gen retires it
        vTaskDelay(pdMS_TO_TICKS(5));
    }
}

// ---------------------------------------------------------------- audio: to the sink's 48 kHz stereo
// The sink (ES8311 I2S or the USB card) really runs at 48 kHz stereo 16-bit whatever pcm_begin was
// asked: a 22 kHz mono track was consumed 4.35x too fast (chipmunk sound, clock racing). So every
// video audio track is converted here: mono -> both channels, any rate -> 48 kHz by linear
// interpolation, with the phase and the last input frame carried across chunks.
#define VP_OUT_RATE 48000
typedef struct { uint32_t step; int32_t pos; int16_t last_l, last_r; } vp_rs_t;   // 16.16 fixed point
static void rs_reset(vp_rs_t *r, int in_rate){
    r->step = (uint32_t)(((uint64_t)in_rate << 16) / VP_OUT_RATE);
    r->pos = -65536; r->last_l = r->last_r = 0;
}
// `n` interleaved frames of `ch` channels in -> 48 kHz stereo frames out (returns the count).
static size_t rs_run(vp_rs_t *r, const int16_t *in, size_t n, int ch, int16_t *out, size_t out_cap){
    if (!n) return 0;
    size_t o = 0;
    const int32_t end = (int32_t)(n - 1) << 16;
    while (r->pos < end && o < out_cap) {
        const int32_t k = r->pos >> 16;                  // -1 .. n-2 (x[-1] = last frame of before)
        const int32_t f = r->pos & 0xFFFF;
        int32_t l0, r0, l1, r1;
        if (k < 0) { l0 = r->last_l; r0 = r->last_r; }
        else       { l0 = in[k * ch]; r0 = in[k * ch + ch - 1]; }
        l1 = in[(k + 1) * ch]; r1 = in[(k + 1) * ch + ch - 1];
        out[o * 2]     = (int16_t)(l0 + (((l1 - l0) * (f >> 1)) >> 15));   // 17-bit x 15-bit: no overflow
        out[o * 2 + 1] = (int16_t)(r0 + (((r1 - r0) * (f >> 1)) >> 15));
        o++;
        r->pos += (int32_t)r->step;
    }
    r->pos -= (int32_t)n << 16;
    r->last_l = in[(n - 1) * ch]; r->last_r = in[(n - 1) * ch + ch - 1];
    return o;
}

// ---------------------------------------------------------------- AVI: PCM audio track
// Own task + own FILE: reads the '01wb' chunks in order and streams them into nv_audio, whose ring
// (full = blocked write) paces it; the backlog-corrected byte count is the clip's master clock.
typedef struct {
    char      path[256];
    vp_ent_t *ents; uint32_t n;
    uint32_t  rate, bps; uint16_t ch, bits, align;
    uint32_t  gen;
} vp_avi_audio_t;

static void avi_audio_task(void *arg){
    vp_avi_audio_t *C = (vp_avi_audio_t *)arg;
    const uint32_t gen = C->gen;
    const size_t cap = 64 * 1024 + 2 * VP_SECT;
    uint8_t  *buf  = (uint8_t *)heap_caps_aligned_calloc(VP_ALIGN, 1, cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    uint32_t *cum  = (uint32_t *)heap_caps_malloc((size_t)C->n * 4, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    // Tracks that aren't 48 kHz 16-bit stereo go through the converter in blocks of VP_RS_IN frames.
    const bool convert = !(C->rate == VP_OUT_RATE && C->ch == 2 && C->bits == 16);
    enum { VP_RS_IN = 4096 };
    const size_t rs_cap = (size_t)VP_RS_IN * VP_OUT_RATE / 8000 + 4;   // worst case: 8 kHz in
    int16_t  *wide = convert ? (int16_t *)heap_caps_malloc((size_t)VP_RS_IN * 2 * 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) : NULL;
    int16_t  *rso  = convert ? (int16_t *)heap_caps_malloc(rs_cap * 4, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) : NULL;
    vp_rs_t   rs; rs_reset(&rs, C->rate);
    FILE *fa = nv_sd_fopen(C->path, "rb");
    bool begun = false;
    if (buf && cum && fa && (!convert || (wide && rso))) {
        setvbuf(fa, NULL, _IONBF, 0);
        uint32_t acc = 0;
        for (uint32_t i = 0; i < C->n; i++) { cum[i] = acc; acc += C->ents[i].size; }
        begun = audio_begin(VP_OUT_RATE, 2, gen);
    }
    const uint32_t out_bps = VP_OUT_RATE * 2 * 2;          // what reaches the sink: 48 kHz stereo 16-bit
    const size_t   lead = (size_t)out_bps * 3 / 2;         // audio queued ahead of the ear
    // One sample frame (all channels): every read, skip and write stays a whole number of them —
    // one stray byte (odd chunk, bogus block-align) would shift the stream into full-scale noise.
    const uint32_t fsz = (uint32_t)C->ch * (C->bits / 8);
    const uint32_t align = (C->align >= fsz && C->align % fsz == 0) ? C->align : fsz;
    uint64_t written = 0;                                  // output bytes handed to the ring
    uint32_t i = 0, skip = 0;
    if (begun && gen == s_audio_gen) { s_audio_pos_ms = 0; s_audio_live = true; }
    // One loop for the whole life of the clip: a short clip fits the ring completely, and a seek
    // that arrives after the last chunk was written (or after the tail played out) must still
    // restart the audio — the task only leaves on stop.
    bool active = begun;                                   // false once the sink died: stay, silent
    while (begun && AUDIO_MINE(gen)) {
        if (!active) { vTaskDelay(pdMS_TO_TICKS(20)); continue; }
        const uint32_t q = s_seek_seq;
        const int want = __atomic_exchange_n(&s_audio_seek_ms, -1, __ATOMIC_ACQ_REL);
        if (want >= 0) {
            uint64_t src = (uint64_t)want * C->bps / 1000;
            src -= src % align;
            uint32_t lo = 0, hi = C->n;                     // last chunk with cum <= src
            while (hi - lo > 1) { const uint32_t mid = (lo + hi) / 2; if (cum[mid] <= src) lo = mid; else hi = mid; }
            i = lo; skip = (src > cum[lo]) ? (uint32_t)(src - cum[lo]) : 0;
            if (skip >= C->ents[i].size) skip = 0;
            skip -= skip % fsz;
            rs_reset(&rs, C->rate);
            nv_audio_pcm_flush();
            written = (uint64_t)want * out_bps / 1000;
            // let the feeder act on the flush before measuring: the old backlog would read as
            // "seconds behind" and drag the picture clock back
            for (int t = 0; t < 40 && nv_audio_pcm_backlog() > 0 && AUDIO_MINE(gen); t++) vTaskDelay(pdMS_TO_TICKS(5));
            if (gen == s_audio_gen) { s_audio_pos_ms = want; s_audio_live = true; }
        }
        s_audio_seq = q;
        if (i >= C->n) {                                    // all written: play out the tail, then idle
            const size_t bl = nv_audio_pcm_backlog();
            audio_pos_store(gen, written, bl, out_bps);
            if (!bl && gen == s_audio_gen) s_audio_live = false;   // picture back on the wall clock
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        // Keep ~1.5 s queued, not the whole ~11 s ring: filling it at every start and seek was a
        // burst of card reads that starved the picture (the frame drops right after opening).
        if (nv_audio_pcm_backlog() > lead) {
            audio_pos_store(gen, written, nv_audio_pcm_backlog(), out_bps);
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        const vp_ent_t *e = &C->ents[i];
        uint32_t off = e->off + skip, left = (e->size > skip) ? e->size - skip : 0;
        left -= left % fsz;
        skip = 0;
        while (left && AUDIO_MINE(gen) && s_audio_seek_ms < 0) {
            uint32_t take = left < 64 * 1024 ? left : 64 * 1024;
            if (convert && take > VP_RS_IN * fsz) take = VP_RS_IN * fsz;
            take -= take % fsz;
            const uint8_t *p = take ? read_span(fa, buf, cap, off, take) : NULL;
            if (!p) { left = 0; break; }
            const void *pcm = p; size_t bytes = take;
            if (convert) {
                const int16_t *in16 = (const int16_t *)p;
                if (C->bits == 8) {                        // unsigned 8-bit -> signed 16-bit
                    for (uint32_t k = 0; k < take; k++) wide[k] = (int16_t)(((int)p[k] - 128) << 8);
                    in16 = wide;
                }
                const size_t frames = take / fsz;
                pcm = rso; bytes = rs_run(&rs, in16, frames, C->ch, rso, rs_cap) * 4;
                if (!bytes) { off += take; left -= take; continue; }
            }
            if (nv_audio_pcm_write(pcm, bytes) < 0) {       // sink died: picture on the wall clock
                active = false;
                if (gen == s_audio_gen) s_audio_live = false;
                break;
            }
            written += bytes;
            audio_pos_store(gen, written, nv_audio_pcm_backlog(), out_bps);
            off += take; left -= take;
        }
        if (s_audio_seek_ms < 0) i++;
    }
    if (gen == s_audio_gen) s_audio_live = false;
    if (begun) { nv_audio_pcm_flush(); nv_audio_pcm_end(); }   // same task as the begin (mutex)
    if (fa) nv_sd_fclose(fa);
    if (buf) heap_caps_free(buf);
    if (cum) heap_caps_free(cum);
    if (wide) heap_caps_free(wide);
    if (rso) heap_caps_free(rso);
    heap_caps_free(C->ents);
    heap_caps_free(C);
    if (gen == s_audio_gen) s_audio_running = false;
    vTaskDelete(NULL);
}

// ---------------------------------------------------------------- AVI: reader task
// Reads frames ahead into the aligned slots while the decoder task is busy with the HW JPEG engine,
// so SD time and decode time overlap instead of adding up. It reads the frame due NOW, skipping
// whatever the clock has already passed (every MJPEG frame is a keyframe), so a card or a bitrate
// too slow for real time costs frame rate, never slow motion.
typedef struct { int slot; uint32_t k; uint32_t off; uint32_t len; uint32_t gen; } vp_rmsg_t;
#define VP_EOS 0xFFFFFFFFu

static QueueHandle_t     s_rfree = NULL, s_rfull = NULL;
static volatile uint32_t s_rd_gen = 0, s_rd_next = 0;
static volatile bool     s_rd_run = false, s_rd_alive = false;
static FILE             *s_rd_f = NULL;
static const vp_ent_t   *s_rd_idx = NULL;
static uint32_t          s_rd_n = 0;
static int64_t           s_v_period_us = 66666;

static inline int frame_ms(uint32_t k){ return (int)((int64_t)k * s_v_period_us / 1000); }
static inline uint32_t frame_at(int ms){ return ms <= 0 ? 0 : (uint32_t)((int64_t)ms * 1000 / s_v_period_us); }

static void avi_reader_task(void *arg){
    (void)arg;
    uint32_t gen = __atomic_load_n(&s_rd_gen, __ATOMIC_ACQUIRE), next = s_rd_next;
    bool eos_sent = false;
    while (s_rd_run) {
        int slot;
        if (xQueueReceive(s_rfree, &slot, pdMS_TO_TICKS(50)) != pdTRUE) continue;
        const uint32_t g = __atomic_load_n(&s_rd_gen, __ATOMIC_ACQUIRE);
        if (g != gen) { gen = g; next = s_rd_next; eos_sent = false; }
        if (!s_clk_pause_us && next < s_rd_n) {
            const uint32_t due = frame_at(clk_ms());
            if (due > next + 1 && due < s_rd_n) { s_drops += due - next; next = due; }
        }
        vp_rmsg_t m = { .slot = slot, .k = next, .off = 0, .len = 0, .gen = gen };
        if (next >= s_rd_n) {
            if (eos_sent) { xQueueSend(s_rfree, &slot, 0); vTaskDelay(pdMS_TO_TICKS(20)); continue; }
            m.k = VP_EOS; eos_sent = true;
        } else {
            const vp_ent_t *e = &s_rd_idx[next];
            const int64_t t0 = esp_timer_get_time();
            const uint8_t *p = (e->size >= 2 && e->size <= VP_IN_CAP)
                             ? read_span(s_rd_f, s_islot[slot], VP_SLOT_CAP, e->off, e->size) : NULL;
            s_t_rd_us += (uint32_t)(esp_timer_get_time() - t0); s_n_rd++;
            if (p) { m.off = (uint32_t)(p - s_islot[slot]); m.len = e->size; }
            next++;
        }
        xQueueSend(s_rfull, &m, portMAX_DELAY);   // never blocks: the queue holds every slot
    }
    s_rd_alive = false;
    vTaskDelete(NULL);
}

// ---------------------------------------------------------------- play one AVI file
static void play_avi(const char *path){
    s_err_reason = ""; s_dec_err_logged = false;
    s_audio_gen++;                       // retire any audio task a previous clip left behind
    if (!ensure_engine()) { s_err_reason = "Decoder JPEG hardware non disponibile"; s_state = NV_VP_ERROR; return; }
    FILE *f = nv_sd_fopen(path, "rb");
    if (!f) { NV_LOGW(TAG,"open fail: %s", path); s_err_reason = "Impossibile aprire il file"; s_state = NV_VP_ERROR; return; }
    setvbuf(f, NULL, _IONBF, 0);   // bulk reads go straight to the card (read_span); set before any I/O

    vp_avi_t A;
    if (!avi_probe(f, &A)) {
        nv_sd_fclose(f); NV_LOGW(TAG,"not a playable AVI: %s", path);
        s_err_reason = "AVI non valido o senza traccia video"; s_state = NV_VP_ERROR; return;
    }
    const uint32_t fcc = A.v_fcc;
    const bool mjpeg = fcc == FCC('M','J','P','G') || fcc == FCC('m','j','p','g') || fcc == FCC('A','V','I','1') ||
                       fcc == FCC('J','P','E','G') || fcc == FCC('j','p','e','g') || fcc == FCC('M','J','P','A') ||
                       fcc == FCC('d','m','b','1') || fcc == 0;
    if (!mjpeg) {
        nv_sd_fclose(f);
        NV_LOGW(TAG, "avi: video codec %.4s is not MJPEG", (const char *)&fcc);
        s_err_reason = "AVI: codec video non supportato (serve Motion-JPEG)"; s_state = NV_VP_ERROR; return;
    }

    vp_ent_t *vidx = NULL, *aidx = NULL; uint32_t vn = 0, an = 0;
    const char *how = "odml";
    bool indexed = avi_index_odml(f, &A, A.v_indx, A.v_indx_sz, &vidx, &vn);
    if (indexed && A.a_indx && !avi_index_odml(f, &A, A.a_indx, A.a_indx_sz, &aidx, &an)) { aidx = NULL; an = 0; }
    if (!indexed) { how = "idx1"; indexed = avi_index_idx1(f, &A, &vidx, &vn, &aidx, &an); }
    if (!indexed) how = "scanned";
    if (!indexed && !avi_index_scan(f, &A, &vidx, &vn)) {
        nv_sd_fclose(f);
        s_err_reason = "AVI senza fotogrammi leggibili"; s_state = NV_VP_ERROR; return;
    }
    // Frame period: the stream's own rate/scale, else avih; the camera writes the MEASURED rate.
    int64_t per = 0;
    if (A.v_rate && A.v_scale) per = (int64_t)A.v_scale * 1000000 / A.v_rate;
    if (per < 1000 || per > 5000000) per = A.uspf ? A.uspf : 66666;
    if (per < 1000) per = 1000; else if (per > 5000000) per = 5000000;
    s_v_period_us = per;
    s_period_ms = (int)(per / 1000);
    s_dur_ms = frame_ms(vn);

    const bool pcm = aidx && an && A.a_tag == 1 && (A.a_bits == 16 || A.a_bits == 8) &&
                     A.a_ch >= 1 && A.a_ch <= 2 && A.a_rate >= 8000 && A.a_rate <= 96000;
    NV_LOGI(TAG, "avi: %ux%u, %u frames @ %d.%03d fps (%s), audio=%s",
            (unsigned)A.vw, (unsigned)A.vh, (unsigned)vn, (int)(1000000 / per), (int)((1000000000 / per) % 1000),
            how, pcm ? "pcm" : (A.a_stream >= 0 ? "unsupported" : "none"));

    s_cur = -1; s_gen = 0; s_widx = 0; s_eot = false; s_pos_ms = 0;
    s_is_yuv = false;
    s_vseek_ms = -1; s_audio_seek_ms = -1;
    s_fps10 = 0; s_last_pub_us = 0; s_drops = 0; s_shown = 0;
    s_jneed = 0;                                  // re-read the geometry from this clip's first frame
    s_audio_live = false; s_audio_stop_flag = false; s_audio_pos_ms = 0;
    s_clk_pause_us = 0; clk_set(0); s_clk_on = true;
    s_playing = true;
    s_state = NV_VP_PLAYING;

    // audio first: it owns the clock as soon as the sink takes data
    s_has_audio = false;
    if (pcm) {
        vp_avi_audio_t *C = (vp_avi_audio_t *)heap_caps_malloc(sizeof *C, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (C) {
            strncpy(C->path, path, sizeof C->path - 1); C->path[sizeof C->path - 1] = '\0';
            C->ents = aidx; C->n = an; C->rate = A.a_rate; C->bps = A.a_bps ? A.a_bps : A.a_rate * A.a_ch * (A.a_bits / 8);
            C->ch = A.a_ch; C->bits = A.a_bits; C->align = A.a_align;
            C->gen = s_audio_gen;
            s_audio_seq = s_seek_seq;
            s_audio_running = true;
            if (xTaskCreatePinnedToCore(avi_audio_task, "vpaudio", 6144, C, 5, NULL, 0) == pdPASS) {
                aidx = NULL; s_has_audio = true;
            } else { s_audio_running = false; heap_caps_free(C); }
        }
    }
    if (aidx) { heap_caps_free(aidx); aidx = NULL; }

    // reader pipeline
    s_rfree = xQueueCreate(VP_ISLOTS, sizeof(int));
    s_rfull = xQueueCreate(VP_ISLOTS, sizeof(vp_rmsg_t));
    bool stop = false;
    if (!s_rfree || !s_rfull) { s_err_reason = "Memoria insufficiente"; s_state = NV_VP_ERROR; stop = true; }
    uint32_t gen = 1;
    if (!stop) {
        for (int i = 0; i < VP_ISLOTS; i++) xQueueSend(s_rfree, &i, 0);
        s_rd_f = f; s_rd_idx = vidx; s_rd_n = vn;
        s_rd_next = 0; __atomic_store_n(&s_rd_gen, gen, __ATOMIC_RELEASE);
        s_rd_run = true; s_rd_alive = true;
        if (xTaskCreatePinnedToCore(avi_reader_task, "vpread", 4096, NULL, 5, NULL, 0) != pdPASS) {
            s_rd_alive = false; s_err_reason = "Memoria insufficiente"; s_state = NV_VP_ERROR; stop = true;
        }
    }

    int  pend_slot = -1; uint32_t pend_k = 0;   // decoded ring slot waiting for its presentation time
    bool want_one = true;                        // show a frame even while paused (open / after seek)
    bool at_eos = false;
    uint32_t bad = 0;
    while (!stop) {
        if (poll_cmd()) { stop = true; break; }

        if (s_vseek_ms >= 0) {
            const int want = s_vseek_ms; s_vseek_ms = -1;
            uint32_t k = frame_at(want);
            if (k >= vn) k = vn ? vn - 1 : 0;
            gen++;
            s_rd_next = k; __atomic_store_n(&s_rd_gen, gen, __ATOMIC_RELEASE);
            pend_slot = -1; at_eos = false; want_one = true;
            clk_set(frame_ms(k)); s_pos_ms = frame_ms(k);
            if (s_has_audio) audio_seek_post(frame_ms(k));
            continue;
        }

        clk_follow_audio();
        const int now = clk_ms();
        if (pend_slot >= 0) {
            const int due = frame_ms(pend_k);
            if (due > now + 1 && !(want_one && s_state == NV_VP_PAUSED)) {
                const int w = due - now;
                vTaskDelay(pdMS_TO_TICKS(w < 20 ? w : 20));
                continue;
            }
            publish(pend_slot, s_jw, s_jh, s_jpitch);
            s_pos_ms = due; pend_slot = -1; want_one = false;
            continue;
        }
        if (s_state == NV_VP_PAUSED && !want_one) { vTaskDelay(pdMS_TO_TICKS(20)); continue; }
        if (at_eos) {
            // the last frame is up: finish once its display time has run out (and the audio with it)
            if (now >= s_dur_ms && !s_audio_live) break;
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }

        vp_rmsg_t m;
        if (xQueueReceive(s_rfull, &m, pdMS_TO_TICKS(20)) != pdTRUE) continue;
        if (m.gen != gen) { xQueueSend(s_rfree, &m.slot, 0); continue; }
        if (m.k == VP_EOS) { xQueueSend(s_rfree, &m.slot, 0); at_eos = true; continue; }
        // Late: when the next frame is already read AND already due, this one would only flash
        // past — drop it and let the newer one take the slot.
        if (!want_one && m.k + 1 < vn && frame_ms(m.k + 1) <= now && uxQueueMessagesWaiting(s_rfull) > 0) {
            xQueueSend(s_rfree, &m.slot, 0); s_drops++; continue;
        }
        if (!m.len) {                                   // unreadable / zero-size ("drop") entry
            xQueueSend(s_rfree, &m.slot, 0);
            if (++bad > 60 && !s_shown) { s_err_reason = "AVI illeggibile"; s_state = NV_VP_ERROR; break; }
            continue;
        }
        const int slot = s_widx;
        ring_wait_free(slot);                           // never under the display's blit
        const int64_t td = esp_timer_get_time();
        const bool ok = jpeg_decode_slot(slot, s_islot[m.slot] + m.off, m.len);
        s_t_dec_us += (uint32_t)(esp_timer_get_time() - td); s_n_dec++;
        xQueueSend(s_rfree, &m.slot, 0);
        if (s_n_dec >= 300) {   // ~10 s of 30 fps
            NV_LOGI(TAG, "avi: avg read %u us (%u), decode %u us (%u), shown %u dropped %u",
                    (unsigned)(s_n_rd ? s_t_rd_us / s_n_rd : 0), (unsigned)s_n_rd,
                    (unsigned)(s_t_dec_us / s_n_dec), (unsigned)s_n_dec, (unsigned)s_shown, (unsigned)s_drops);
            s_t_rd_us = s_n_rd = s_t_dec_us = s_n_dec = 0;
        }
        if (!ok) {
            if (!s_dec_err_logged) { NV_LOGW(TAG, "avi: frame %u did not decode", (unsigned)m.k); s_dec_err_logged = true; }
            if (++bad > 60 && !s_shown) { s_err_reason = "AVI: fotogrammi JPEG non decodificabili"; s_state = NV_VP_ERROR; break; }
            continue;
        }
        bad = 0;
        s_widx = (s_widx + 1) % VP_RING;
        pend_slot = slot; pend_k = m.k;
    }

    // teardown: reader first (it holds the FILE), then audio. No timeout on the join: the reader
    // only ever blocks in a card read, and freeing its queues/index under it would be worse.
    s_rd_run = false;
    for (int i = 0; s_rd_alive; i++) {
        if (i == 600) NV_LOGW(TAG, "avi: reader slow to stop (card?)");
        vTaskDelay(pdMS_TO_TICKS(5));
    }
    const bool ended = !stop && s_state != NV_VP_ERROR;
    audio_stop_and_wait();
    s_has_audio = false; s_audio_live = false;
    if (s_rfree) { vQueueDelete(s_rfree); s_rfree = NULL; }
    if (s_rfull) { vQueueDelete(s_rfull); s_rfull = NULL; }
    s_rd_f = NULL; s_rd_idx = NULL; s_rd_n = 0;
    if (vidx) heap_caps_free(vidx);
    nv_sd_fclose(f);
    NV_LOGI(TAG, "avi: shown %u frames, dropped %u", (unsigned)s_shown, (unsigned)s_drops);
    if (ended) { s_eot = true; s_state = NV_VP_STOPPED; }
    else if (s_state != NV_VP_ERROR) s_state = NV_VP_STOPPED;
    s_clk_on = false; s_clk_pause_us = 0;
    s_playing = false;
}

// ================================ H.264 software path ============================================
// The P4 has no HW H.264 DECODER, but esp_h264 ships a P4-asm-optimized software decoder that can
// use BOTH cores (CONFIG_ESP_H264_DUAL_TASK) with hot code in IRAM. It outputs I420; PPA converts
// I420->RGB565 (+ scale) in hardware in nv_vplayer_render(). Realtime only at low resolution.
static uint32_t be32(const uint8_t *p){ return ((uint32_t)p[0]<<24)|((uint32_t)p[1]<<16)|((uint32_t)p[2]<<8)|p[3]; }
static uint16_t be16(const uint8_t *p){ return (uint16_t)((p[0]<<8)|p[1]); }
static const uint8_t *find4(const uint8_t *h, size_t n, const char *t){
    if (n < 4) return NULL;
    for (size_t i=0;i+4<=n;i++) if (h[i]==(uint8_t)t[0]&&h[i+1]==(uint8_t)t[1]&&h[i+2]==(uint8_t)t[2]&&h[i+3]==(uint8_t)t[3]) return h+i;
    return NULL;
}

// Copy a decoded I420 frame into the ring and publish it (CPU write -> HW DMA read needs C2M sync).
static void publish_i420(const uint8_t *yuv, int w, int h){
    size_t need = (size_t)w * h * 3 / 2;
    if (!yuv || w<=0 || h<=0 || need > s_ring_len) return;
    memcpy(s_ring[s_widx], yuv, need);
    esp_cache_msync(s_ring[s_widx], (need + s_cache_align - 1) & ~(s_cache_align - 1), ESP_CACHE_MSYNC_FLAG_DIR_C2M);
    publish(s_widx, w, h, w);
    s_widx = (s_widx + 1) % VP_RING;
}

#if CONFIG_NV_VPLAYER_H264
// Feed one Annex-B packet through the decoder; publish every frame it yields.
static void feed_annexb(esp_h264_dec_handle_t dec, esp_h264_dec_param_handle_t param, uint8_t *buf, uint32_t len){
    esp_h264_dec_in_frame_t in = { .raw_data = { .buffer = buf, .len = len }, .consume = 0 };
    while (in.raw_data.len > 0) {
        esp_h264_dec_out_frame_t out = {0};
        if (esp_h264_dec_process(dec, &in, &out) != ESP_H264_ERR_OK) {
            if (!s_dec_err_logged) {
                NV_LOGW(TAG, "h264 decode error, dropping remaining stream (unsupported profile?)");
                s_dec_err_logged = true;
                if (!s_err_reason || !s_err_reason[0])
                    s_err_reason = "Decodifica H.264 fallita — profilo non supportato";
                s_state = NV_VP_ERROR;   // latch: sticky, unlike PLAYING/PAUSED -- lets the UI show
                                          // a clear message instead of a silent black+audio-only clip
            }
            break;
        }
        const uint32_t c = in.consume;
        if (out.out_size > 0 && out.outbuf) {
            esp_h264_resolution_t res = {0};
            if (esp_h264_dec_get_resolution(param, &res) == ESP_H264_ERR_OK)
                publish_i420(out.outbuf, res.width, res.height);
        }
        if (c == 0) break;
        const uint32_t adv = (c <= in.raw_data.len) ? c : in.raw_data.len;
        in.raw_data.buffer += adv; in.raw_data.len -= adv; in.consume = 0;
    }
}
static const uint8_t k_sc[4] = {0,0,0,1};   // Annex-B start code (MP4 path)
#endif  // CONFIG_NV_VPLAYER_H264

// ------------------------------------------------------------- MP4 box walking + sample tables
// Generic ISO-BMFF box reader. `tag` points at the 4-byte fourcc (box start is tag-4); `size` is the
// box's total length (incl. its own 8-byte header); `payload` is tag+4 (right after the fourcc).
typedef struct { const uint8_t *tag; uint32_t size; const uint8_t *payload; } vp_box_t;
typedef struct { const uint8_t *lo, *hi; } vp_bounds_t;   // the whole loaded moov buffer's span

static bool in_bounds(const vp_bounds_t *b, const uint8_t *p, long n){
    return p >= b->lo && n >= 0 && p + n <= b->hi;
}

static bool box_at(const uint8_t *p, const uint8_t *end, vp_box_t *out){
    if (p + 8 > end) return false;
    uint32_t sz = be32(p);
    if (sz == 1 || sz < 8) return false;         // 64-bit extended size not expected in our moov; bail safely
    if (p + sz > end) sz = (uint32_t)(end - p);   // clamp a truncated/oversized box to what we actually have
    out->tag = p + 4; out->size = sz; out->payload = p + 8;
    return true;
}
static bool find_child(const uint8_t *start, const uint8_t *end, const char *want, vp_box_t *out){
    const uint8_t *p = start;
    while (p < end) {
        vp_box_t b;
        if (!box_at(p, end, &b)) break;
        if (memcmp(b.tag, want, 4) == 0) { *out = b; return true; }
        p += b.size;
    }
    return false;
}

typedef struct { uint32_t offset, size; } vp_sample_t;

typedef struct {
    bool     present;
    uint32_t nsamp;
    vp_sample_t *tbl;         // PSRAM, nsamp entries — flat, chunk-interleaving already resolved
    uint32_t period_ms;       // ms/sample (CFR assumption: first stts run's delta)
    uint32_t *sync;           // PSRAM 0-based keyframe sample indices from stss; NULL = every sample is one
    uint32_t sync_count;
    const uint8_t *sps; uint16_t sps_len;   // video: point INTO the moov buffer (kept alive for the clip)
    const uint8_t *pps; uint16_t pps_len;
    uint8_t  nal_len_size;                   // video: avcC lengthSizeMinusOne+1 (1/2/3/4); 0 = unset
    int rate, channels, bits;                // audio only
} vp_track_t;

// Combine stsc (samples-per-chunk run-length) + stco/co64 (chunk byte offsets) + stsz (sample sizes)
// into one flat {offset,size} table — replacing the old "single contiguous chunk" assumption that
// only ever matched our own muxer's output, not a normal interleaved MP4 (like an ffmpeg remux).
// Every declared count is clamped to what actually fits in `B` BEFORE it drives an indexed read —
// an attacker-supplied entry_count can't push any access past the moov buffer.
static bool build_sample_table(const vp_bounds_t *B,
                                const uint8_t *stsc_p, uint32_t stsc_n,
                                const uint8_t *stco_p, bool use64, uint32_t stco_n,
                                const uint8_t *stsz_p, vp_sample_t **out_tbl, uint32_t *out_n){
    if (!in_bounds(B, stsz_p, 12)) return false;
    uint32_t fixed_size = be32(stsz_p + 4);
    uint32_t nsamp = be32(stsz_p + 8);
    const uint8_t *sizes = stsz_p + 12;
    if (!fixed_size) {
        uint32_t max_sizes = in_bounds(B, sizes, 0) ? (uint32_t)((B->hi - sizes) / 4) : 0;
        if (nsamp > max_sizes) nsamp = max_sizes;
    }
    // 200k samples = 1.6 MB of sample tables (~1.9 h at 30 fps). The old 2M cap let two 4-byte
    // header fields request 16 MB of PSRAM per track and starve LVGL/the reclaim broker.
    if (nsamp == 0 || nsamp > 200000) return false;

    { uint32_t max_stsc = in_bounds(B, stsc_p+8, 0) ? (uint32_t)((B->hi - (stsc_p+8)) / 12) : 0;
      if (stsc_n > max_stsc) stsc_n = max_stsc; }
    if (stsc_n == 0) return false;

    const long entry_sz = use64 ? 8 : 4;
    { uint32_t max_stco = in_bounds(B, stco_p+8, 0) ? (uint32_t)((B->hi - (stco_p+8)) / entry_sz) : 0;
      if (stco_n > max_stco) stco_n = max_stco; }
    if (stco_n == 0) return false;

    vp_sample_t *tbl = (vp_sample_t *)heap_caps_malloc((size_t)nsamp * sizeof(vp_sample_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!tbl) return false;

    uint32_t si = 0, entry_ix = 0;
    for (uint32_t c = 0; c < stco_n && si < nsamp; c++) {
        while (entry_ix + 1 < stsc_n && be32(stsc_p + 8 + (entry_ix+1)*12) <= c + 1) entry_ix++;
        uint32_t spc = be32(stsc_p + 8 + entry_ix*12 + 4);
        uint64_t off = use64 ? (((uint64_t)be32(stco_p+8+c*8) << 32) | be32(stco_p+8+c*8+4))
                             : be32(stco_p+8+c*4);
        for (uint32_t s = 0; s < spc && si < nsamp; s++) {
            uint32_t szv = fixed_size ? fixed_size : be32(sizes + si*4);
            tbl[si].offset = (uint32_t)off; tbl[si].size = szv;
            off += szv; si++;
        }
    }
    if (si == 0) { heap_caps_free(tbl); return false; }
    *out_tbl = tbl; *out_n = si;
    return true;
}

// Fill V (handler 'vide') or A (handler 'soun', mp4a/AAC only) from one <trak> box's contents.
static void parse_trak(const vp_bounds_t *B, const uint8_t *p, const uint8_t *end, vp_track_t *V, vp_track_t *A){
    vp_box_t mdia;
    if (!find_child(p, end, "mdia", &mdia)) return;
    const uint8_t *mp = mdia.payload, *me = mdia.payload + mdia.size - 8;
    if (me > B->hi) me = B->hi;

    vp_box_t hdlr, mdhd, minf;
    if (!find_child(mp, me, "hdlr", &hdlr)) return;
    if (!find_child(mp, me, "mdhd", &mdhd)) return;
    if (!find_child(mp, me, "minf", &minf)) return;

    if (!in_bounds(B, hdlr.payload, 12)) return;
    const bool is_vide = memcmp(hdlr.payload + 8, "vide", 4) == 0;
    const bool is_soun = memcmp(hdlr.payload + 8, "soun", 4) == 0;
    if (!is_vide && !is_soun) return;

    if (!in_bounds(B, mdhd.payload, 1)) return;
    const long ts_off = (mdhd.payload[0] == 1) ? 20 : 12;   // version 1 -> 64-bit create/modify times
    if (!in_bounds(B, mdhd.payload, ts_off + 4)) return;
    uint32_t timescale = be32(mdhd.payload + ts_off);

    vp_box_t stbl;
    const uint8_t *ip = minf.payload, *ie = minf.payload + minf.size - 8;
    if (ie > B->hi) ie = B->hi;
    if (!find_child(ip, ie, "stbl", &stbl)) return;
    const uint8_t *sp = stbl.payload, *se = stbl.payload + stbl.size - 8;
    if (se > B->hi) se = B->hi;

    vp_box_t stsd, stts, stsz, stsc, stco, co64b;
    bool has_stco = find_child(sp, se, "stco", &stco);
    bool has_co64 = !has_stco && find_child(sp, se, "co64", &co64b);
    if (!find_child(sp, se, "stsd", &stsd) || !find_child(sp, se, "stts", &stts) ||
        !find_child(sp, se, "stsz", &stsz) || !find_child(sp, se, "stsc", &stsc) ||
        (!has_stco && !has_co64)) return;

    if (!in_bounds(B, stts.payload, 16)) return;   // bytes 12-15 (the first run's delta) are read below
    uint32_t stts_n = be32(stts.payload + 4);
    uint32_t delta = stts_n ? be32(stts.payload + 8 + 4) : 0;   // first run's delta (CFR assumption)
    uint32_t period_ms = (timescale && delta) ? (uint32_t)((uint64_t)delta * 1000 / timescale) : 66;
    if (period_ms == 0) period_ms = 66;

    if (!in_bounds(B, stsc.payload, 8)) return;
    uint32_t stsc_n = be32(stsc.payload + 4);
    const uint8_t *stco_p = has_stco ? stco.payload : co64b.payload;
    if (!in_bounds(B, stco_p, 8)) return;
    uint32_t stco_n = be32(stco_p + 4);

    vp_sample_t *tbl = NULL; uint32_t nsamp = 0;
    if (!build_sample_table(B, stsc.payload, stsc_n, stco_p, has_co64, stco_n, stsz.payload, &tbl, &nsamp)) return;

    vp_track_t *T = is_vide ? V : A;
    T->present = true; T->nsamp = nsamp; T->tbl = tbl; T->period_ms = period_ms;

    if (!in_bounds(B, stsd.payload, 8)) return;   // no codec info -> track stays "present" but unusable
    const uint8_t *entry = stsd.payload + 8;      // first (only) sample entry
    const uint8_t *stsd_end = stsd.payload + stsd.size - 8;
    if (stsd_end > B->hi) stsd_end = B->hi;

    if (is_vide) {
        if (in_bounds(B, entry, (long)(stsd_end - entry))) {
            const uint8_t *av = find4(entry, (size_t)(stsd_end - entry), "avcC");
            if (av && in_bounds(B, av, 12)) {
                uint16_t sps_len = be16(av+10);
                const uint8_t *sps = av + 12;
                if (in_bounds(B, sps, (long)sps_len + 3)) {
                    const uint8_t *pp = sps + sps_len;
                    uint16_t pps_len = be16(pp+1);
                    const uint8_t *pps = pp + 3;
                    if (in_bounds(B, pps, pps_len)) {
                        T->sps = sps; T->sps_len = sps_len;
                        T->pps = pps; T->pps_len = pps_len;
                        // avcC payload[4] low 2 bits = lengthSizeMinusOne (av+4 is payload start,
                        // since av points at the 4-byte "avcC" tag). Most encoders use 4, but some
                        // remux/export tools use 1 or 2 -- assuming 4 unconditionally silently
                        // truncates every NAL and leaves the decoder starved (audio still plays,
                        // since it's an independent track/task -- exactly the "sound but no video"
                        // symptom this fixes).
                        T->nal_len_size = (uint8_t)((av[8] & 0x03) + 1);
                    }
                }
            }
        }
        vp_box_t stssb;
        if (find_child(sp, se, "stss", &stssb) && in_bounds(B, stssb.payload, 8)) {
            uint32_t n = be32(stssb.payload + 4);
            uint32_t max_n = (uint32_t)((B->hi - (stssb.payload+8)) / 4);
            if (n > max_n) n = max_n;
            if (n && n < 200000) {
                uint32_t *sync = (uint32_t *)heap_caps_malloc((size_t)n * 4, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
                if (sync) {
                    for (uint32_t k=0;k<n;k++) sync[k] = be32(stssb.payload + 8 + k*4) - 1;   // 1-based -> 0-based
                    T->sync = sync; T->sync_count = n;
                }
            }
        }
    } else if (in_bounds(B, entry, 36) && memcmp(entry + 4, "mp4a", 4) == 0) {   // audio: AAC (mp4a) only
        T->channels = be16(entry + 24);
        T->bits     = be16(entry + 26);
        T->rate     = (int)(be32(entry + 32) >> 16);
    }
}

static void parse_moov(const uint8_t *moov, uint32_t moov_sz, vp_track_t *V, vp_track_t *A){
    const vp_bounds_t B = { moov, moov + moov_sz };
    const uint8_t *p = moov + 8, *end = moov + moov_sz;   // skip moov's own 8-byte header
    while (p < end) {
        vp_box_t b;
        if (!box_at(p, end, &b)) break;
        if (memcmp(b.tag, "trak", 4) == 0) {
            const uint8_t *te = b.payload + b.size - 8;
            if (te > B.hi) te = B.hi;
            parse_trak(&B, b.payload, te, V, A);
        }
        p += b.size;
    }
}

// Nearest keyframe sample index <= the sample nearest pos_ms (CFR assumption via period_ms).
static uint32_t seek_video_index(vp_track_t *V, int want_ms){
    uint32_t target = V->period_ms ? (uint32_t)want_ms / V->period_ms : 0;
    if (V->nsamp && target >= V->nsamp) target = V->nsamp - 1;
    if (!V->sync || V->sync_count == 0) return target;   // no stss -> every sample is a keyframe
    uint32_t best = V->sync[0];
    for (uint32_t k = 0; k < V->sync_count && V->sync[k] <= target; k++) best = V->sync[k];
    return best;
}

// ------------------------------------------------------------- MP4 audio track: AAC -> nv_audio
// Runs as its own task with its own FILE* (independent cursor from the video loop's). MP4 samples
// are already exact, complete AAC access-units (no ADTS framing) — use_frame_dec=true so the simple
// decoder consumes one whole frame per call, no carry-tail buffering needed. Sample offset/size come
// from the same bounds-clamped table the video track uses; a bogus offset/size just fails the fseek
// or is skipped (size capped against the input buffer below), never an OOB write.
typedef struct { char path[300]; vp_track_t A; } vp_audio_ctx_t;

static TaskHandle_t s_audio_task = NULL;
static bool s_audio_codecs_registered = false;

static void ensure_audio_codecs(void){
    if (s_audio_codecs_registered) return;
    esp_audio_dec_register_default();
    esp_audio_simple_dec_register_default();
    s_audio_codecs_registered = true;
}

static void audio_ctx_free(vp_audio_ctx_t *ctx){
    if (!ctx) return;
    if (ctx->A.tbl) heap_caps_free(ctx->A.tbl);
    if (ctx->A.sync) heap_caps_free(ctx->A.sync);
    heap_caps_free(ctx);
}

static void audio_task(void *arg){
    vp_audio_ctx_t *ctx = (vp_audio_ctx_t *)arg;
    vp_track_t *A = &ctx->A;
    s_audio_running = true;
    s_audio_pos_ms = 0;

    FILE *fa = nv_sd_fopen(ctx->path, "rb");
    if (!fa) { audio_ctx_free(ctx); s_audio_running = false; vTaskDelete(NULL); return; }

    esp_aac_dec_cfg_t aac_cfg = {
        .sample_rate = A->rate, .channel = (uint8_t)A->channels, .bits_per_sample = 16,
        .no_adts_header = true, .aac_plus_enable = false,
    };
    esp_audio_simple_dec_cfg_t cfg = {
        .dec_type = ESP_AUDIO_SIMPLE_DEC_TYPE_AAC, .dec_cfg = &aac_cfg,
        .cfg_size = sizeof(aac_cfg), .use_frame_dec = true,
    };
    esp_audio_simple_dec_handle_t dec = NULL;
    if (esp_audio_simple_dec_open(&cfg, &dec) != ESP_AUDIO_ERR_OK || !dec) {
        nv_sd_fclose(fa); audio_ctx_free(ctx); s_audio_running = false; vTaskDelete(NULL); return;
    }

    uint8_t *inbuf = (uint8_t *)heap_caps_malloc(8192, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    uint32_t outcap = 16384;
    uint8_t *outbuf = (uint8_t *)heap_caps_malloc(outcap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    bool begun = false;
    uint64_t out_bytes = 0;
    int rate = A->rate, ch = A->channels, bits = 16;
    uint32_t si = 0;

    while (inbuf && outbuf && si < A->nsamp) {
        if (s_state == NV_VP_STOPPED || s_audio_stop_flag) break;
        bool give_up = false;
        while (s_state == NV_VP_PAUSED) {
            if (s_audio_stop_flag) { give_up = true; break; }
            vTaskDelay(pdMS_TO_TICKS(30));
        }
        if (give_up || s_audio_stop_flag) break;

        int want = s_audio_seek_ms;
        if (want >= 0) {
            uint32_t period = A->period_ms ? A->period_ms : 1;
            uint32_t ti = (uint32_t)want / period;
            if (A->nsamp && ti >= A->nsamp) ti = A->nsamp - 1;
            si = ti;
            nv_audio_pcm_flush();
            const uint32_t bps = (uint32_t)rate * ch * (bits/8);
            out_bytes = bps ? (uint64_t)si * period * bps / 1000ull : 0;
            s_audio_seek_ms = -1;
        }

        uint32_t ssz = A->tbl[si].size;
        if (ssz == 0 || ssz > 8192) { si++; continue; }
        fseek(fa, A->tbl[si].offset, SEEK_SET);
        if (fread(inbuf, 1, ssz, fa) != ssz) break;

        esp_audio_simple_dec_raw_t raw = { .buffer = inbuf, .len = ssz, .eos = (si + 1 >= A->nsamp), .consumed = 0 };
        esp_audio_simple_dec_out_t out = { .buffer = outbuf, .len = outcap };
        esp_audio_err_t e = esp_audio_simple_dec_process(dec, &raw, &out);
        if (e == ESP_AUDIO_ERR_BUFF_NOT_ENOUGH) {
            uint32_t need = out.needed_size ? out.needed_size : outcap * 2;
            uint8_t *nb = (uint8_t *)heap_caps_realloc(outbuf, need, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            if (nb) { outbuf = nb; outcap = need; }
            si++; continue;
        }
        if (e == ESP_AUDIO_ERR_OK && out.decoded_size > 0) {
            if (!begun) {
                esp_audio_simple_dec_info_t info = {0};
                if (esp_audio_simple_dec_get_info(dec, &info) == ESP_AUDIO_ERR_OK && info.sample_rate) {
                    rate = (int)info.sample_rate;
                    ch   = info.channel ? info.channel : ch;
                    bits = info.bits_per_sample ? info.bits_per_sample : 16;
                }
                begun = nv_audio_pcm_begin_as(rate, ch, bits, NV_PCM_MUSIC);
                if (!begun) break;
            }
            if (nv_audio_pcm_write(out.buffer, out.decoded_size) < 0) break;
            out_bytes += out.decoded_size;
            const uint32_t bps = (uint32_t)rate * ch * (bits/8);
            if (bps) {
                int pos = (int)((uint64_t)out_bytes * 1000ull / bps);
                pos -= (int)((uint64_t)nv_audio_pcm_backlog() * 1000ull / bps);
                s_audio_pos_ms = pos > 0 ? pos : 0;
            }
        }
        si++;
    }

    if (begun) nv_audio_pcm_end();
    esp_audio_simple_dec_close(dec);
    if (inbuf) heap_caps_free(inbuf);
    if (outbuf) heap_caps_free(outbuf);
    nv_sd_fclose(fa);
    audio_ctx_free(ctx);
    s_audio_running = false;
    vTaskDelete(NULL);
}

// Ownership: `A`'s tbl/sync arrays transfer to the audio task's ctx copy (freed there). The video
// loop's own V.tbl/V.sync stay owned by play_mp4 regardless of whether audio starts.
static void start_audio_task(const char *path, vp_track_t *A){
    vp_audio_ctx_t *ctx = (vp_audio_ctx_t *)heap_caps_malloc(sizeof(vp_audio_ctx_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!ctx) {
        if (A->tbl) heap_caps_free(A->tbl);
        if (A->sync) heap_caps_free(A->sync);
        return;
    }
    strncpy(ctx->path, path, sizeof ctx->path - 1); ctx->path[sizeof ctx->path - 1] = '\0';
    ctx->A = *A;
    s_audio_stop_flag = false;
    s_audio_seek_ms = -1;
    s_audio_pos_ms = 0;
    ensure_audio_codecs();
    // 20 KB: the esp_audio_codec AAC decoder needs ~20 KB of stack (vendor README); 6 KB
    // faulted the task on the first frame (the music player learned the same at 8 KB).
    if (xTaskCreate(audio_task, "vpaudio", 20480, ctx, 5, &s_audio_task) != pdPASS) {
        audio_ctx_free(ctx); s_audio_task = NULL;
    }
}
static void stop_audio_task_and_wait(void){
    if (!s_audio_task) return;
    s_audio_stop_flag = true;
    for (int i = 0; i < 200 && s_audio_running; i++) vTaskDelay(pdMS_TO_TICKS(5));   // up to ~1s grace
    s_audio_task = NULL;
}

// ------------------------------------------------------------- MP4 video (+ paired audio) playback
#if CONFIG_NV_VPLAYER_H264
static void play_mp4(const char *path, esp_h264_dec_handle_t *dec_ptr, esp_h264_dec_param_handle_t *param_ptr){
    FILE *f = nv_sd_fopen(path, "rb");
    if (!f) { s_state = NV_VP_ERROR; return; }
    fseek(f,0,SEEK_END); long fsz = ftell(f); fseek(f,0,SEEK_SET);

    // locate the moov box (top-level scan)
    long moov_off = 0; uint32_t moov_sz = 0; long p = 0; uint8_t hb[8];
    while (p + 8 <= fsz) {
        fseek(f, p, SEEK_SET); if (fread(hb,1,8,f)!=8) break;
        uint32_t bs = be32(hb); if (bs < 8 || (int64_t)p + bs > fsz) break;   // untrusted box size: no wrap-around scan
        if (memcmp(hb+4,"moov",4)==0) { moov_off = p; moov_sz = bs; break; }
        p += bs;
    }
    if (!moov_off || moov_sz < 16 || moov_sz > 4u*1024*1024) { nv_sd_fclose(f); NV_LOGW(TAG,"mp4: no moov"); s_state=NV_VP_ERROR; return; }
    uint8_t *moov = (uint8_t *)heap_caps_malloc(moov_sz, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!moov) { nv_sd_fclose(f); s_state=NV_VP_ERROR; return; }
    fseek(f, moov_off, SEEK_SET);
    if (fread(moov,1,moov_sz,f)!=moov_sz) { heap_caps_free(moov); nv_sd_fclose(f); s_state=NV_VP_ERROR; return; }

    vp_track_t V = {0}, A = {0};
    parse_moov(moov, moov_sz, &V, &A);
    if (!V.nal_len_size || V.nal_len_size > 4) V.nal_len_size = 4;   // avcC missing/unparsed: 4 is by far the common case

    if (!V.present || !V.tbl || !V.sps_len || !V.pps_len) {
        if (V.tbl) heap_caps_free(V.tbl);
        if (V.sync) heap_caps_free(V.sync);
        if (A.tbl) heap_caps_free(A.tbl);
        if (A.sync) heap_caps_free(A.sync);
        heap_caps_free(moov); nv_sd_fclose(f); NV_LOGW(TAG,"mp4: no usable video track"); s_state=NV_VP_ERROR; return;
    }

    // One-line diagnostic: SPS profile_idc (sps[1] -- sps[0] is the NAL header byte). 66=Baseline
    // (this decoder's sweet spot), 77=Main, 100=High -- most everyday H.264 exports are Main/High
    // with CABAC, which the h264bsd-based SW decoder here does not support (CAVLC/Baseline only).
    // A decode error right after this line for a non-66 profile confirms that limitation.
    const unsigned prof = (V.sps_len >= 2) ? V.sps[1] : 0;
    NV_LOGI(TAG, "mp4: SPS profile_idc=%u nal_len_size=%u", prof, V.nal_len_size);

    // Baseline gate: the only SW decoder shipped for the P4 is tinyh264/h264bsd (Constrained
    // Baseline, profile_idc 66 — CAVLC, no B-frames). openh264 in this component is ENCODER-ONLY on
    // P4 (libopenh264.a exports WelsCreateSVCEncoder but no WelsCreateDecoder). So Main(77)/High(100)
    // clips can't be decoded at all — reject up front with a clear reason instead of starting the
    // audio track over a permanently black canvas (h264bsd would just fail on the first slice).
    if (prof != 66) {
        NV_LOGW(TAG, "mp4: H.264 profile_idc=%u unsupported (baseline-only SW decoder) — refusing", prof);
        s_err_reason = (prof == 77) ? "Profilo H.264 Main non supportato — serve Baseline"
                     : (prof >= 100) ? "Profilo H.264 High non supportato — serve Baseline"
                                     : "Profilo H.264 non supportato — serve Baseline";
        if (V.tbl) heap_caps_free(V.tbl);
        if (V.sync) heap_caps_free(V.sync);
        if (A.tbl) heap_caps_free(A.tbl);
        if (A.sync) heap_caps_free(A.sync);
        heap_caps_free(moov); nv_sd_fclose(f);
        s_state = NV_VP_ERROR;
        return;
    }

    s_dur_ms = (int)(V.nsamp * V.period_ms);
    s_vseek_ms = -1;
    const bool audio_ok = A.present && A.tbl && A.rate > 0 && A.channels > 0;
    if (A.present && !audio_ok) {
        if (A.tbl) heap_caps_free(A.tbl);
        if (A.sync) heap_caps_free(A.sync);
    }
    s_has_audio = audio_ok;
    if (audio_ok) start_audio_task(path, &A);   // A.tbl/A.sync ownership moves to the audio task now

    TickType_t next = xTaskGetTickCount();
    bool stop = false;
    bool need_params = true;   // prefix SPS/PPS on the next fed sample: true for sample 0, and again after any seek
    uint32_t i = 0;
    while (i < V.nsamp && !stop) {
        if (poll_cmd()) { stop = true; break; }
        if (s_state == NV_VP_PAUSED) {
            while (s_state == NV_VP_PAUSED) { if (poll_cmd()) { stop = true; break; } vTaskDelay(pdMS_TO_TICKS(30)); }
            next = xTaskGetTickCount();   // re-anchor the pacer: no fast-forward burst after a pause
        }
        if (stop) break;

        if (s_vseek_ms >= 0) {
            int want = s_vseek_ms; s_vseek_ms = -1;
            i = seek_video_index(&V, want);
            s_audio_seek_ms = want;
            need_params = true;
            esp_h264_dec_close(*dec_ptr); esp_h264_dec_del(*dec_ptr);
            *dec_ptr = NULL;   // a failed re-open below must not leave a dangling handle for play_h264 to close again
            esp_h264_dec_cfg_sw_t rcfg = { .pic_type = ESP_H264_RAW_FMT_I420 };
            esp_h264_dec_handle_t ndec = NULL;
            if (esp_h264_dec_sw_new(&rcfg, &ndec) != ESP_H264_ERR_OK || !ndec || esp_h264_dec_open(ndec) != ESP_H264_ERR_OK) {
                NV_LOGW(TAG, "mp4: decoder re-open after seek failed"); stop = true; break;
            }
            *dec_ptr = ndec;
            esp_h264_dec_sw_get_param_hd(ndec, param_ptr);
            next = xTaskGetTickCount();
        }

        uint32_t ssz = V.tbl[i].size;
        if (ssz == 0 || ssz > s_in_cap) { i++; continue; }
        fseek(f, V.tbl[i].offset, SEEK_SET);
        if (fread(s_in,1,ssz,f)!=ssz) break;

        // assemble Annex-B: (SPS,PPS on the first sample / right after a seek) + each AVCC
        // length-prefixed NAL -> start code
        uint32_t al = 0;
        if (need_params && V.sps_len && V.pps_len && (size_t)(8+V.sps_len+V.pps_len) < s_annex_cap) {
            memcpy(s_annex+al,k_sc,4); al+=4; memcpy(s_annex+al,V.sps,V.sps_len); al+=V.sps_len;
            memcpy(s_annex+al,k_sc,4); al+=4; memcpy(s_annex+al,V.pps,V.pps_len); al+=V.pps_len;
            need_params = false;
        }
        // AVCC length-prefixed NAL -> start code. Prefix width comes from avcC's lengthSizeMinusOne
        // (V.nal_len_size), NOT a hardcoded 4 -- some remux/export tools use a 1- or 2-byte prefix.
        uint32_t q = 0;
        while (q + V.nal_len_size <= ssz) {
            uint32_t nl = 0;
            for (uint8_t k = 0; k < V.nal_len_size; k++) nl = (nl << 8) | s_in[q + k];
            q += V.nal_len_size;
            // Subtraction forms: `q + nl` / `al + 4 + nl` wrapped for a NAL length like 0xFFFFFFFE
            // and let memcpy run off the 516 KB annex buffer (remote: /api/video/play, no auth).
            if (nl == 0 || nl > ssz - q) break;
            if (al + 4 > s_annex_cap || nl > s_annex_cap - al - 4) break;
            memcpy(s_annex+al,k_sc,4); al+=4; memcpy(s_annex+al,s_in+q,nl); al+=nl;
            q += nl;
        }
        if (al && s_state != NV_VP_ERROR) {
            feed_annexb(*dec_ptr, *param_ptr, s_annex, al);
        } else if (al == 0 && !s_dec_err_logged) {
            NV_LOGW(TAG, "mp4: empty annexb at sample %u (bad NAL length prefix?)", (unsigned)i);
            s_dec_err_logged = true;
        }
        s_pos_ms = (int)((i+1) * V.period_ms);

        if (s_has_audio) {
            // audio is the master clock: ahead -> wait for it, behind -> catch up (no delay, drop-and-go)
            int diff = s_pos_ms - s_audio_pos_ms;
            if (diff > 80) vTaskDelay(pdMS_TO_TICKS(diff > 250 ? 250 : diff));
        } else {
            vTaskDelayUntil(&next, pdMS_TO_TICKS(V.period_ms));
        }
        i++;
    }

    if (s_has_audio) stop_audio_task_and_wait();
    s_has_audio = false;
    heap_caps_free(V.tbl); if (V.sync) heap_caps_free(V.sync);
    heap_caps_free(moov); nv_sd_fclose(f);
    if (!stop) s_eot = true;
}

static void play_raw_h264(const char *path, esp_h264_dec_handle_t dec, esp_h264_dec_param_handle_t param){
    FILE *f = nv_sd_fopen(path, "rb");
    if (!f) { s_state = NV_VP_ERROR; return; }
    s_dur_ms = 0;
    const int period = 66; uint32_t frame_no = 0;
    TickType_t next = xTaskGetTickCount();
    size_t fill = 0; bool stop = false;
    for (;;) {
        if (poll_cmd()) { stop = true; break; }
        while (s_state == NV_VP_PAUSED) { if (poll_cmd()) { stop = true; break; } vTaskDelay(pdMS_TO_TICKS(30)); }
        if (stop) break;
        size_t got = fread(s_in + fill, 1, s_in_cap - fill, f);
        size_t avail = fill + got;
        if (avail == 0) break;
        esp_h264_dec_in_frame_t in = { .raw_data = { .buffer = s_in, .len = (uint32_t)avail }, .consume = 0 };
        size_t consumed = 0;
        while (in.raw_data.len > 0) {
            esp_h264_dec_out_frame_t out = {0};
            if (esp_h264_dec_process(dec, &in, &out) != ESP_H264_ERR_OK) break;
            uint32_t c = in.consume;
            if (out.out_size > 0 && out.outbuf) {
                esp_h264_resolution_t res = {0};
                if (esp_h264_dec_get_resolution(param, &res) == ESP_H264_ERR_OK) publish_i420(out.outbuf, res.width, res.height);
                frame_no++; s_pos_ms = (int)(frame_no * (uint32_t)period);
                vTaskDelayUntil(&next, pdMS_TO_TICKS(period));
            }
            if (c == 0) break;
            uint32_t adv = (c <= in.raw_data.len) ? c : in.raw_data.len;
            in.raw_data.buffer += adv; in.raw_data.len -= adv; in.consume = 0; consumed += adv;
        }
        size_t leftover = avail - consumed;
        if (leftover && leftover < avail) memmove(s_in, s_in + consumed, leftover);
        fill = leftover;
        if (got == 0 && (leftover == 0 || consumed == 0)) break;   // EOF, no more progress
    }
    nv_sd_fclose(f);
    if (!stop) s_eot = true;
}

static void play_h264(const char *path){
    if (!ensure_ring()) { s_state = NV_VP_ERROR; return; }
    esp_h264_dec_cfg_sw_t cfg = { .pic_type = ESP_H264_RAW_FMT_I420 };
    esp_h264_dec_handle_t dec = NULL;
    if (esp_h264_dec_sw_new(&cfg, &dec) != ESP_H264_ERR_OK || !dec) { NV_LOGW(TAG,"h264 sw new failed"); s_state = NV_VP_ERROR; return; }
    if (esp_h264_dec_open(dec) != ESP_H264_ERR_OK) { esp_h264_dec_del(dec); s_state = NV_VP_ERROR; return; }
    esp_h264_dec_param_handle_t param = NULL;
    esp_h264_dec_sw_get_param_hd(dec, &param);

    s_cur = -1; s_gen = 0; s_widx = 0; s_pos_ms = 0; s_dur_ms = 0; s_eot = false;
    s_fps10 = 0; s_last_pub_us = 0; s_dec_err_logged = false; s_err_reason = "";
    s_is_yuv = true; s_playing = true; s_state = NV_VP_PLAYING;

    const char *ext = strrchr(path, '.');
    if (ext && strcasecmp(ext, ".mp4") == 0) play_mp4(path, &dec, &param);
    else                                     play_raw_h264(path, dec, param);

    if (dec) { esp_h264_dec_close(dec); esp_h264_dec_del(dec); }   // NULL after a failed seek re-open
    if (s_state != NV_VP_ERROR) s_state = NV_VP_STOPPED;
    s_playing = false;
}
#endif  // CONFIG_NV_VPLAYER_H264

// ---------------------------------------------------------------- MPEG-1 (pl_mpeg, software)
// Split across both cores. pl_mpeg's video decode (IDCT / motion comp / VLC) is the one job that
// can't be parallelised, so core 1 does nothing else: the MP2 audio track is decoded by a second,
// audio-only pl_mpeg instance on core 0, and the YUV->RGB565 conversion + frame presentation run on
// core 0 too. The decoder hands frames over through a few YUV slots, so it can run ahead of the
// clock and absorb the I-frame spikes instead of showing them as stutter. Frames are published
// RGB565 (s_is_yuv=false) to ride the same blit path as MJPEG — deliberately NOT the PPA-YUV420
// path that hung the H.264 case.
static inline uint16_t vp_rgb565(int r, int g, int b){
    if (r < 0) r = 0; else if (r > 255) r = 255;
    if (g < 0) g = 0; else if (g > 255) g = 255;
    if (b < 0) b = 0; else if (b > 255) b = 255;
    return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}
// YUV 4:2:0 planes -> packed RGB565 (dst stride = w px), BT.601 limited range, one chroma sample per
// 2x2 block. `ys`/`cs` are the padded plane strides; w/h the active picture size.
// Forced to -O3 (the file is -Os): this per-pixel loop is on the playback hot path.
__attribute__((optimize("O3")))
static void yuv420_to_565(const uint8_t *Y, int ys, const uint8_t *Cb, const uint8_t *Cr, int cs,
                          int w, int h, uint16_t *dst){
    const int cols = w >> 1, rows = h >> 1;
    for (int row = 0; row < rows; row++){
        const uint8_t *y0 = Y + row * 2 * ys, *y1 = y0 + ys;
        const uint8_t *cb = Cb + row * cs, *cr = Cr + row * cs;
        uint16_t *d0 = dst + row * 2 * w, *d1 = d0 + w;
        for (int col = 0; col < cols; col++){
            const int vr = cr[col] - 128, ub = cb[col] - 128;
            const int r = (vr * 104597) >> 16;
            const int g = (ub * 25674 + vr * 53278) >> 16;
            const int b = (ub * 132201) >> 16;
            int y;
            y = ((y0[0] - 16) * 76309) >> 16; d0[0] = vp_rgb565(y+r, y-g, y+b);
            y = ((y0[1] - 16) * 76309) >> 16; d0[1] = vp_rgb565(y+r, y-g, y+b);
            y = ((y1[0] - 16) * 76309) >> 16; d1[0] = vp_rgb565(y+r, y-g, y+b);
            y = ((y1[1] - 16) * 76309) >> 16; d1[1] = vp_rgb565(y+r, y-g, y+b);
            y0 += 2; y1 += 2; d0 += 2; d1 += 2;
        }
    }
}

#define VP_YSLOTS 4
typedef struct { int slot; int t_ms; uint32_t gen; bool first; } vp_ymsg_t;   // slot -1 = end of stream

static QueueHandle_t     s_yfree = NULL, s_yfull = NULL;
static uint8_t          *s_yslot[VP_YSLOTS] = {0};
static int               s_yw = 0, s_yh = 0;          // active picture
static int               s_ys = 0, s_yr = 0;          // luma stride / rows (padded to macroblocks)
static int               s_cs = 0, s_cr = 0;          // chroma stride / rows
static volatile uint32_t s_conv_gen = 0;
static volatile bool     s_conv_run = false, s_conv_alive = false, s_conv_eos = false;

static void mpeg1_free_yslots(void){
    for (int i = 0; i < VP_YSLOTS; i++) if (s_yslot[i]) { heap_caps_free(s_yslot[i]); s_yslot[i] = NULL; }
}

// Core 0: wait for each frame's presentation time, convert, publish. Owns clk_follow_audio().
static void mpeg1_conv_task(void *arg){
    (void)arg;
    while (s_conv_run) {
        vp_ymsg_t m;
        if (xQueueReceive(s_yfull, &m, pdMS_TO_TICKS(50)) != pdTRUE) { clk_follow_audio(); continue; }
        if (m.slot < 0) { if (m.gen == s_conv_gen) s_conv_eos = true; continue; }
        while (s_conv_run && m.gen == s_conv_gen) {
            clk_follow_audio();
            const int now = clk_ms();
            if (m.first && s_state == NV_VP_PAUSED) break;   // show the seek target even while paused
            if (m.t_ms <= now + 1) break;
            const int w = m.t_ms - now;
            vTaskDelay(pdMS_TO_TICKS(w < 10 ? w : 10));
        }
        if (!s_conv_run || m.gen != s_conv_gen) { xQueueSend(s_yfree, &m.slot, 0); continue; }
        // Late: the next frame is already decoded and due too — skip this one's conversion.
        if (!m.first && uxQueueMessagesWaiting(s_yfull) > 0 && clk_ms() > m.t_ms + s_period_ms) {
            s_drops++; xQueueSend(s_yfree, &m.slot, 0); continue;
        }
        const uint8_t *Y = s_yslot[m.slot];
        const uint8_t *Cb = Y + (size_t)s_ys * s_yr, *Cr = Cb + (size_t)s_cs * s_cr;
        const int slot = s_widx;
        ring_wait_free(slot);                           // never under the display's blit
        yuv420_to_565(Y, s_ys, Cb, Cr, s_cs, s_yw, s_yh, (uint16_t *)s_ring[slot]);
        xQueueSend(s_yfree, &m.slot, 0);
        const size_t bytes = ((size_t)s_yw * s_yh * 2 + s_cache_align - 1) & ~(s_cache_align - 1);
        esp_cache_msync(s_ring[slot], bytes, ESP_CACHE_MSYNC_FLAG_DIR_C2M);   // CPU wrote it, PPA reads it
        publish(slot, s_yw, s_yh, s_yw);
        s_widx = (s_widx + 1) % VP_RING;
        s_pos_ms = m.t_ms;
    }
    s_conv_alive = false;
    vTaskDelete(NULL);
}

// Core 0: the MP2 track through its own audio-only pl_mpeg instance (own FILE, own demux), paced by
// the nv_audio ring (a full ring blocks the write). Its backlog-corrected output is the clock.
typedef struct { uint32_t gen; char path[]; } vp_m1_audio_t;

static void mpeg1_audio_task(void *arg){
    vp_m1_audio_t *C = (vp_m1_audio_t *)arg;
    const uint32_t gen = C->gen;
    FILE *fa = nv_sd_fopen(C->path, "rb");
    char *fbuf = fa ? stdio_big_buffer(fa) : NULL;
    plm_t *pa = fa ? plm_create_with_file(fa, 0) : NULL;
    int16_t *pcm = (int16_t *)heap_caps_malloc(PLM_AUDIO_SAMPLES_PER_FRAME * 2 * sizeof(int16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    // MP2 at 44.1/32 kHz -> the sink's 48 kHz (see rs_run); 1152 frames in -> at most 1728 out
    int16_t *rso = (int16_t *)heap_caps_malloc((PLM_AUDIO_SAMPLES_PER_FRAME * VP_OUT_RATE / 32000 + 4) * 4, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    vp_rs_t rs;
    bool begun = false;
    int rate = 0;
    if (pa && pcm) {
        plm_set_video_enabled(pa, 0);
        plm_set_audio_enabled(pa, 1);
        plm_set_audio_stream(pa, 0);
        rate = plm_get_samplerate(pa);
        // A failed begin (dead USB sink -> "reopen failed") means the stream mutex is NOT ours:
        // pcm_end'ing it anyway killed whatever owned the sink and freed a mutex held by another
        // task (FreeRTOS priority-inheritance assert -> reboot). begin/end stay in this one task.
        if (rate > 0 && rso) begun = audio_begin(VP_OUT_RATE, 2, gen);
    }
    rs_reset(&rs, rate > 0 ? rate : VP_OUT_RATE);
    const bool convert = rate != VP_OUT_RATE;
    const uint32_t out_bps = VP_OUT_RATE * 4;
    uint64_t written = 0;
    bool drained = false;                 // decoder hit the end; only the ring's tail is left
    bool active = begun;                  // false once the sink died: stay (stoppable), silent
    if (begun && gen == s_audio_gen) { s_audio_pos_ms = 0; s_audio_live = true; }
    while (begun && AUDIO_MINE(gen)) {
        if (!active) { vTaskDelay(pdMS_TO_TICKS(20)); continue; }
        const uint32_t q = s_seek_seq;
        const int want = __atomic_exchange_n(&s_audio_seek_ms, -1, __ATOMIC_ACQ_REL);
        if (want >= 0) {
            nv_plm_seek_audio(pa, want / 1000.0);
            rs_reset(&rs, rate);
            drained = false;
            nv_audio_pcm_flush();
            written = (uint64_t)want * out_bps / 1000;
            for (int t = 0; t < 40 && nv_audio_pcm_backlog() > 0 && AUDIO_MINE(gen); t++) vTaskDelay(pdMS_TO_TICKS(5));
            if (gen == s_audio_gen) { s_audio_pos_ms = want; s_audio_live = true; }
        }
        s_audio_seq = q;
        if (drained) {                                    // play out the tail, then idle (seekable)
            const size_t bl = nv_audio_pcm_backlog();
            audio_pos_store(gen, written, bl, out_bps);
            if (!bl && gen == s_audio_gen) s_audio_live = false;
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        if (nv_audio_pcm_backlog() > (size_t)out_bps * 3 / 2) {   // ~1.5 s ahead is plenty (see AVI)
            audio_pos_store(gen, written, nv_audio_pcm_backlog(), out_bps);
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        plm_samples_t *s = plm_decode_audio(pa);
        if (!s) { if (plm_has_ended(pa)) drained = true; else vTaskDelay(pdMS_TO_TICKS(5)); continue; }
        const int n = (int)s->count * 2;   // interleaved stereo
        for (int i = 0; i < n; i++) {
            float v = s->interleaved[i];
            if (v > 1.0f) v = 1.0f; else if (v < -1.0f) v = -1.0f;
            pcm[i] = (int16_t)(v * 32767.0f);
        }
        const int16_t *outp = pcm;
        size_t outb = (size_t)n * sizeof(int16_t);
        if (convert) {
            outp = rso;
            outb = rs_run(&rs, pcm, (size_t)s->count, 2, rso, PLM_AUDIO_SAMPLES_PER_FRAME * VP_OUT_RATE / 32000 + 4) * 4;
        }
        if (outb && nv_audio_pcm_write(outp, outb) < 0) {
            active = false;
            if (gen == s_audio_gen) s_audio_live = false;
            continue;
        }
        written += outb;
        audio_pos_store(gen, written, nv_audio_pcm_backlog(), out_bps);
    }
    if (gen == s_audio_gen) s_audio_live = false;
    if (begun) { nv_audio_pcm_flush(); nv_audio_pcm_end(); }   // same task as the begin (mutex)
    if (pa) plm_destroy(pa);
    if (fa) nv_sd_fclose(fa);   // plm was created with close_when_done=0
    if (fbuf) heap_caps_free(fbuf);
    if (pcm) heap_caps_free(pcm);
    if (rso) heap_caps_free(rso);
    heap_caps_free(C);
    if (gen == s_audio_gen) s_audio_running = false;
    vTaskDelete(NULL);
}

// Copy a decoded frame's padded planes into a YUV slot and queue it for presentation.
static void mpeg1_post(int slot, const plm_frame_t *fr, uint32_t gen, bool first){
    uint8_t *d = s_yslot[slot];
    memcpy(d, fr->y.data, (size_t)s_ys * s_yr);            d += (size_t)s_ys * s_yr;
    memcpy(d, fr->cb.data, (size_t)s_cs * s_cr);           d += (size_t)s_cs * s_cr;
    memcpy(d, fr->cr.data, (size_t)s_cs * s_cr);
    vp_ymsg_t m = { .slot = slot, .t_ms = (int)(fr->time * 1000.0), .gen = gen, .first = first };
    xQueueSend(s_yfull, &m, portMAX_DELAY);   // never blocks: the queue holds every slot
}

static void play_mpeg1(const char *path){
    s_audio_gen++;                       // retire any audio task a previous clip left behind
    if (!ensure_ring()) { s_err_reason = "Memoria insufficiente"; s_state = NV_VP_ERROR; return; }
    // (Tried luma planes in internal SRAM — measured NO decode gain: the path is compute-bound, not
    // PSRAM-latency-bound. Left at 0 so we don't hold scarce internal SRAM for nothing.)
    nv_mpeg1_set_int_budget(0);
    // Removal-safe session (nv_sd_fopen) like every other path: plm's own fopen bypassed the drain
    // that protects a card pull mid-playback. close_when_done=0 -> we close it after plm_destroy.
    FILE *pf = nv_sd_fopen(path, "rb");
    char *pfbuf = pf ? stdio_big_buffer(pf) : NULL;
    plm_t *plm = pf ? plm_create_with_file(pf, 0) : NULL;
    if (!plm) { if (pf) nv_sd_fclose(pf); if (pfbuf) heap_caps_free(pfbuf); NV_LOGW(TAG,"mpeg1: open failed %s", path); s_err_reason = "MPEG-1: apertura fallita"; s_state = NV_VP_ERROR; return; }
    plm_set_audio_enabled(plm, 0);   // this instance is video-only; the audio task has its own
    const int w = plm_get_width(plm), h = plm_get_height(plm);
    const double fr = plm_get_framerate(plm);
    if (w <= 0 || h <= 0 || w > VP_MAXW || h > VP_MAXH) {
        NV_LOGW(TAG,"mpeg1: bad/oversized resolution %dx%d", w, h);
        s_err_reason = "MPEG-1: risoluzione non supportata"; plm_destroy(plm); nv_sd_fclose(pf); if (pfbuf) heap_caps_free(pfbuf); s_state = NV_VP_ERROR; return;
    }
    s_period_ms = (fr > 0) ? (int)(1000.0 / fr + 0.5) : 42;
    s_yw = w & ~1; s_yh = h & ~1;
    s_ys = (w + 15) & ~15; s_yr = (h + 15) & ~15;
    s_cs = s_ys / 2;      s_cr = s_yr / 2;
    const size_t yslot = (size_t)s_ys * s_yr + 2 * (size_t)s_cs * s_cr;
    bool ok = true;
    for (int i = 0; i < VP_YSLOTS && ok; i++)
        ok = (s_yslot[i] = (uint8_t *)heap_caps_malloc(yslot, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)) != NULL;
    s_yfree = xQueueCreate(VP_YSLOTS, sizeof(int));
    s_yfull = xQueueCreate(VP_YSLOTS + 1, sizeof(vp_ymsg_t));   // +1: room for the end marker
    if (!ok || !s_yfree || !s_yfull) {
        s_err_reason = "Memoria insufficiente";
        mpeg1_free_yslots();
        if (s_yfree) { vQueueDelete(s_yfree); s_yfree = NULL; }
        if (s_yfull) { vQueueDelete(s_yfull); s_yfull = NULL; }
        plm_destroy(plm); nv_sd_fclose(pf); if (pfbuf) heap_caps_free(pfbuf); s_state = NV_VP_ERROR; return;
    }
    for (int i = 0; i < VP_YSLOTS; i++) xQueueSend(s_yfree, &i, 0);

    const bool has_audio = plm_get_num_audio_streams(plm) > 0;
    NV_LOGI(TAG, "mpeg1: %dx%d @ %.2f fps, audio=%s", w, h, fr, has_audio ? "yes" : "no");

    s_cur = -1; s_gen = 0; s_widx = 0; s_pos_ms = 0; s_eot = false;
    s_fps10 = 0; s_last_pub_us = 0; s_drops = 0; s_shown = 0; s_dec_err_logged = false; s_err_reason = "";
    s_dur_ms = (int)(plm_get_duration(plm) * 1000.0);
    s_is_yuv = false;   // RGB565 output -> working render path
    s_vseek_ms = -1; s_audio_seek_ms = -1;
    s_audio_live = false; s_audio_stop_flag = false; s_audio_pos_ms = 0;
    s_clk_pause_us = 0; clk_set(0); s_clk_on = true;
    s_playing = true; s_state = NV_VP_PLAYING;

    s_has_audio = false;
    if (has_audio) {
        vp_m1_audio_t *ac = (vp_m1_audio_t *)heap_caps_malloc(sizeof(vp_m1_audio_t) + strlen(path) + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (ac) {
            strcpy(ac->path, path);
            ac->gen = s_audio_gen;
            s_audio_seq = s_seek_seq;
            s_audio_running = true;
            // prio 3: below the presenter (5) and the panel blit (5) — it has seconds of ring to spare
            if (xTaskCreatePinnedToCore(mpeg1_audio_task, "vpaudio", 6144, ac, 3, NULL, 0) == pdPASS) s_has_audio = true;
            else { s_audio_running = false; heap_caps_free(ac); }
        }
    }
    uint32_t gen = 1;
    s_conv_gen = gen; s_conv_eos = false; s_conv_run = true; s_conv_alive = true;
    bool stop = false;
    if (xTaskCreatePinnedToCore(mpeg1_conv_task, "vpconv", 3072, NULL, 5, NULL, 0) != pdPASS) {
        s_conv_alive = false; s_err_reason = "Memoria insufficiente"; s_state = NV_VP_ERROR; stop = true;
    }

    bool want_one = true, at_eos = false;
    int last_t = 0;
    uint32_t resyncs = 0;
    int64_t last_resync_us = 0;
    while (!stop) {
        if (poll_cmd()) { stop = true; break; }
        if (s_vseek_ms >= 0) {
            const int want = s_vseek_ms; s_vseek_ms = -1;
            s_conv_gen = ++gen;                                   // presenter drops what's queued
            plm_frame_t *f = plm_seek_frame(plm, want / 1000.0, 0);   // lands on the I-frame at/before
            const int t = f ? (int)(f->time * 1000.0) : want;
            clk_set(t); s_pos_ms = t;
            if (s_has_audio) audio_seek_post(t);
            at_eos = false; s_conv_eos = false; want_one = true;
            if (f) {
                int slot;
                if (xQueueReceive(s_yfree, &slot, pdMS_TO_TICKS(500)) == pdTRUE) {
                    mpeg1_post(slot, f, gen, true); want_one = false; last_t = t;
                }
            }
            continue;
        }
        if (at_eos) {
            if (s_conv_eos && clk_ms() >= last_t + s_period_ms && !s_audio_live) { s_eot = true; break; }
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        int slot;
        if (xQueueReceive(s_yfree, &slot, pdMS_TO_TICKS(20)) != pdTRUE) continue;   // presenter is behind us: good
        const int64_t td = esp_timer_get_time();
        plm_frame_t *f = plm_decode_video(plm);
        {
            const uint32_t us = (uint32_t)(esp_timer_get_time() - td);
            s_t_dec_us += us; s_n_dec++;
            if (us > s_t_rd_us) s_t_rd_us = us;   // (reused as "worst decode" here)
            if (s_n_dec >= 240) {   // ~10 s of 24 fps
                NV_LOGI(TAG, "mpeg1: decode avg %u us, worst %u us, lag %d ms, shown %u dropped %u",
                        (unsigned)(s_t_dec_us / s_n_dec), (unsigned)s_t_rd_us,
                        f ? clk_ms() - (int)(f->time * 1000.0) : 0, (unsigned)s_shown, (unsigned)s_drops);
                s_t_dec_us = s_n_dec = s_t_rd_us = 0;
            }
        }
        if (!f) {
            xQueueSend(s_yfree, &slot, 0);
            if (plm_has_ended(plm) && !at_eos)
                NV_LOGI(TAG, "mpeg1: video stream ended at %d ms (file pos %ld / %ld, shown %u)",
                        last_t, ftell(pf), file_size(pf), (unsigned)s_shown);
            if (plm_has_ended(plm)) {
                vp_ymsg_t m = { .slot = -1, .t_ms = 0, .gen = gen, .first = false };
                xQueueSend(s_yfull, &m, pdMS_TO_TICKS(100));
                at_eos = true;
            }
            continue;
        }
        // Can't keep up (P-frames can't be skipped): once the picture is ~0.6 s behind the clock,
        // jump to the I-frame nearest the clock instead of drifting further out of sync.
        // Rate-limited (a GOP longer than the jump could land behind us again); after a seek the
        // decoder sits on the new I-frame, so that frame is what gets shown.
        const int t = (int)(f->time * 1000.0);
        const int64_t now_us = esp_timer_get_time();
        if (!want_one && s_state == NV_VP_PLAYING && clk_ms() - t > 600 && now_us - last_resync_us > 2000000 &&
            clk_ms() + 1500 < s_dur_ms) {
            last_resync_us = now_us;
            plm_frame_t *g = plm_seek_frame(plm, (clk_ms() + 250) / 1000.0, 0);
            if (!g) { xQueueSend(s_yfree, &slot, 0); continue; }
            f = g; resyncs++;
        }
        last_t = (int)(f->time * 1000.0);
        mpeg1_post(slot, f, gen, want_one);
        want_one = false;
    }

    s_conv_run = false;
    for (int i = 0; s_conv_alive; i++) {               // no timeout: its queues/slots are freed next
        if (i == 600) NV_LOGW(TAG, "mpeg1: presenter slow to stop");
        vTaskDelay(pdMS_TO_TICKS(5));
    }
    audio_stop_and_wait();
    s_has_audio = false; s_audio_live = false;
    NV_LOGI(TAG, "mpeg1: shown %u frames, dropped %u, resyncs %u", (unsigned)s_shown, (unsigned)s_drops, (unsigned)resyncs);
    mpeg1_free_yslots();
    if (s_yfree) { vQueueDelete(s_yfree); s_yfree = NULL; }
    if (s_yfull) { vQueueDelete(s_yfull); s_yfull = NULL; }
    plm_destroy(plm);
    nv_sd_fclose(pf);   // plm was created with close_when_done=0
    if (pfbuf) heap_caps_free(pfbuf);
    if (s_state != NV_VP_ERROR) s_state = NV_VP_STOPPED;
    s_clk_on = false; s_clk_pause_us = 0;
    s_playing = false;
}

// ---------------------------------------------------------------- resource teardown
// SOLO vp_task tocca s_ring/s_in/s_annex/s_dec/s_vp_ppa: nessun free cross-thread, nessun lock.
// release() manda VP_CMD_RELEASE e aspetta il semaforo -> la corsa open/close che corrompeva l'heap
// (decode su buffer/engine gia' liberati -> freeze) e' impossibile.
static void free_resources(void){
    for (int i = 0; i < VP_RING; i++) { if (s_ring[i]) { free(s_ring[i]); s_ring[i] = NULL; } }
    for (int i = 0; i < VP_ISLOTS; i++) { if (s_islot[i]) { heap_caps_free(s_islot[i]); s_islot[i] = NULL; } }
    s_in = NULL; s_in_cap = 0;
    if (s_annex) { heap_caps_free(s_annex); s_annex = NULL; s_annex_cap = 0; }
    if (s_dec)   { jpeg_del_decoder_engine(s_dec); s_dec = NULL; }
    if (s_vp_ppa){ ppa_unregister_client(s_vp_ppa); s_vp_ppa = NULL; }
    s_cur = -1; s_ring_len = 0; s_widx = 0;
    s_state = NV_VP_STOPPED; s_playing = false;
}

// ---------------------------------------------------------------- task + API
static void vp_task(void *arg){
    (void)arg;
    vp_msg_t m;
    for (;;) {
        if (xQueueReceive(s_q, &m, portMAX_DELAY) != pdTRUE) continue;
        if (m.cmd == VP_CMD_OPEN) {
            const char *e = strrchr(m.path, '.');
            if (e && strcasecmp(e, ".avi") == 0)                       play_avi(m.path);   // MJPEG (HW JPEG decode)
            else if (e && (strcasecmp(e,".mpg")==0 || strcasecmp(e,".mpeg")==0 || strcasecmp(e,".m1v")==0))
                                                                       play_mpeg1(m.path); // MPEG-1 (SW pl_mpeg)
#if CONFIG_NV_VPLAYER_H264
            else                                                       play_h264(m.path);  // .mp4 / .h264 (SW decode)
#else
            else { s_err_reason = "Formato non supportato (usa MJPEG-AVI o MPEG-1)"; s_state = NV_VP_ERROR; }
#endif
        } else if (m.cmd == VP_CMD_RELEASE) {
            free_resources();                                    // teardown nel thread proprietario
            if (s_release_sem) xSemaphoreGive(s_release_sem);
        }
        // STOP/PAUSE/RESUME a task idle: nessun play attivo, niente da fare
    }
}

void nv_vplayer_init(void){
    if (s_task) return;
    s_q = xQueueCreate(4, sizeof(vp_msg_t));
    if (!s_q) { NV_LOGE(TAG,"queue oom"); return; }
    if (!s_release_sem) s_release_sem = xSemaphoreCreateBinary();
    if (!s_frame_sem) s_frame_sem = xSemaphoreCreateBinary();
    // 16 KB stack: pl_mpeg's IDCT/motion-comp call chain needs more headroom than the 6 KB the
    // HW-JPEG / h264 paths used. Core 1, below LVGL (prio 6, also core 1 but idle while a video
    // plays over the direct blit): the MPEG-1 decoder gets that core to itself, while its audio,
    // colour conversion and presentation, and the AVI card reader, run on core 0.
    if (xTaskCreatePinnedToCore(vp_task, "nvvplay", 16384, NULL, 5, &s_task, 1) != pdPASS) { s_task = NULL; NV_LOGE(TAG,"task create failed"); }
}

bool nv_vplayer_open(const char *path){
    if (!s_q || !path || !path[0]) return false;
    vp_msg_t m = { .cmd = VP_CMD_OPEN };
    strncpy(m.path, path, sizeof m.path - 1); m.path[sizeof m.path - 1] = '\0';
    s_eot = false; s_state = NV_VP_PLAYING;
    return xQueueSend(s_q, &m, pdMS_TO_TICKS(100)) == pdTRUE;
}

void nv_vplayer_pause(bool on){ if (!s_q) return; vp_msg_t m = { .cmd = on ? VP_CMD_PAUSE : VP_CMD_RESUME }; xQueueSend(s_q,&m,0); }
void nv_vplayer_stop(void){ if (!s_q) return; vp_msg_t m = { .cmd = VP_CMD_STOP }; s_state = NV_VP_STOPPED; xQueueSend(s_q,&m,0); }

bool nv_vplayer_seek(int pos_ms){
    if (s_state != NV_VP_PLAYING && s_state != NV_VP_PAUSED) return false;
    if (pos_ms < 0) pos_ms = 0;
    s_vseek_ms = pos_ms;
    return true;
}
bool nv_vplayer_has_audio(void){ return s_has_audio; }

nv_vp_state_t nv_vplayer_state(void){ return s_state; }
int nv_vplayer_pos_ms(void){
    if (!s_clk_on) return s_has_audio ? s_audio_pos_ms : s_pos_ms;
    int p = clk_ms();                      // the presentation clock (audio-slaved when there's audio)
    if (p < 0) p = 0; else if (s_dur_ms > 0 && p > s_dur_ms) p = s_dur_ms;
    return p;
}
int nv_vplayer_dur_ms(void){ return s_dur_ms; }
int nv_vplayer_fps10(void){ return s_fps10; }   // measured presentation rate x10
void nv_vplayer_stats(uint32_t *shown, uint32_t *dropped){ if (shown) *shown = s_shown; if (dropped) *dropped = s_drops; }
bool nv_vplayer_wait_frame(int timeout_ms){
    return s_frame_sem && xSemaphoreTake(s_frame_sem, pdMS_TO_TICKS(timeout_ms > 0 ? timeout_ms : 0)) == pdTRUE;
}
int nv_vplayer_period_ms(void){ return s_period_ms; }

const char *nv_vplayer_err_reason(void){ return s_err_reason ? s_err_reason : ""; }

void nv_vplayer_set_frame_cb(nv_vp_frame_cb_t cb){ s_frame_cb = cb; }

bool nv_vplayer_took_eot(void){ if (!s_eot) return false; s_eot = false; return true; }

const uint8_t *nv_vplayer_frame_acquire(int *w, int *h, int *pitch, uint32_t *generation){
    int c;
    // publish the hold, then confirm it is still the current frame (a producer that picked this
    // slot before seeing the hold would otherwise write into it while we blit)
    do { c = s_cur; s_hold = c; __sync_synchronize(); } while (c != s_cur);
    if (c < 0) { s_hold = -1; return NULL; }
    if (w) *w = s_w;
    if (h) *h = s_h;
    if (pitch) *pitch = s_pitch ? s_pitch : s_w;
    if (generation) *generation = s_gen;
    return s_ring[c];
}
void nv_vplayer_frame_release(void){ s_hold = -1; }

const uint8_t *nv_vplayer_frame(int *w, int *h, int *pitch, uint32_t *generation){
    int c = s_cur;
    if (c < 0) return NULL;
    if (w) *w = s_w;
    if (h) *h = s_h;
    if (pitch) *pitch = s_pitch ? s_pitch : s_w;
    if (generation) *generation = s_gen;
    return s_ring[c];
}

static int s_aspect = NV_VP_FIT;   // Nv_vp_aspect_t; applied by nv_vplayer_render()
void nv_vplayer_set_aspect(nv_vp_aspect_t mode){
    if (mode < NV_VP_FIT || mode > NV_VP_ZOOM) return;
    s_aspect = (int)mode;
}

bool nv_vplayer_render(uint8_t *dst, int dw, int dh){
    int c = s_cur, w = s_w, h = s_h, pitch = s_pitch ? s_pitch : s_w;
    if (c < 0 || !dst || dw <= 0 || dh <= 0 || w <= 0 || h <= 0) return false;
    uint8_t *src = s_ring[c];

    if (!s_vp_ppa) {   // register the SRM client once and reuse it every frame
        ppa_client_config_t cc = { .oper_type = PPA_OPERATION_SRM };
        if (ppa_register_client(&cc, &s_vp_ppa) != ESP_OK) { s_vp_ppa = NULL; return false; }
    }

    ppa_srm_oper_config_t op = {0};
    op.in.buffer  = src;  op.in.pic_w = s_is_yuv ? w : pitch;  op.in.pic_h = h;
    // H.264 frames arrive as I420 → PPA does the YUV420->RGB565 color convert (+ scale) in HW.
    op.in.srm_cm  = s_is_yuv ? PPA_SRM_COLOR_MODE_YUV420 : PPA_SRM_COLOR_MODE_RGB565;
    if (s_is_yuv) { op.in.yuv_range = PPA_COLOR_RANGE_LIMIT; op.in.yuv_std = PPA_COLOR_CONV_STD_RGB_YUV_BT601; }
    op.out.buffer = dst;  op.out.buffer_size = (uint32_t)dw * dh * 2;
    op.out.pic_w  = dw;   op.out.pic_h = dh;  op.out.srm_cm = PPA_SRM_COLOR_MODE_RGB565;
    op.mode = PPA_TRANS_MODE_BLOCKING;
    // All extents/offsets forced even (YUV420 requirement).
    if (s_aspect == NV_VP_STRETCH) {
        // Fill the target ignoring aspect ratio: independent per-axis scale, no borders.
        op.in.block_w = w;  op.in.block_h = h;
        op.scale_x = (float)dw / w;  op.scale_y = (float)dh / h;
    } else if (s_aspect == NV_VP_ZOOM) {
        // Fill the target preserving aspect ratio by cropping the input to the region that maps
        // onto dst (max-scale). The frame overflows on one axis; we crop it, centered.
        float sc = (float)dw / w; { float sy = (float)dh / h; if (sy > sc) sc = sy; }
        int cw = ((int)(dw / sc)) & ~1; if (cw > (w & ~1)) cw = w & ~1; if (cw < 2) cw = 2;
        int ch = ((int)(dh / sc)) & ~1; if (ch > (h & ~1)) ch = h & ~1; if (ch < 2) ch = 2;
        op.in.block_w = cw;  op.in.block_h = ch;
        op.in.block_offset_x = (uint32_t)(((w - cw) / 2) & ~1);
        op.in.block_offset_y = (uint32_t)(((h - ch) / 2) & ~1);
        op.scale_x = sc;  op.scale_y = sc;
    } else {
        // FIT (default): preserve aspect ratio, letterbox — center the scaled frame in the canvas.
        // Borders keep whatever the caller cleared the buffer to (the app clears to black).
        float sc = (float)dw / w; { float sy = (float)dh / h; if (sy < sc) sc = sy; }
        int tw = ((int)(w * sc)) & ~1; if (tw < 2) tw = 2;
        int th = ((int)(h * sc)) & ~1; if (th < 2) th = 2;
        op.in.block_w = w;  op.in.block_h = h;
        op.out.block_offset_x = (uint32_t)(((dw - tw) / 2) & ~1);
        op.out.block_offset_y = (uint32_t)(((dh - th) / 2) & ~1);
        op.scale_x = sc;  op.scale_y = sc;
    }
    if (ppa_do_scale_rotate_mirror(s_vp_ppa, &op) != ESP_OK) return false;
    // PPA (DMA) just wrote fresh pixels into `dst` (PSRAM) -- without this, the CPU/LVGL side can
    // still see the stale cache line from the last memset (i.e. black), even though the real frame
    // landed in memory. Mirrors the M2C sync already done for the ring buffer above.
    esp_cache_msync(dst, (size_t)dw * dh * 2, ESP_CACHE_MSYNC_FLAG_DIR_M2C);
    return true;
}

void nv_vplayer_release(void){
    // Nessun task: niente puo' star usando le risorse -> libera qui.
    if (!s_q || !s_task) { free_resources(); return; }

    // Fai uscire il play attivo (STOP) e chiedi il teardown DENTRO vp_task (RELEASE), poi aspetta
    // che l'abbia davvero fatto. vp_task e' l'unico a toccare i buffer/decoder -> zero corsa.
    if (s_release_sem) xSemaphoreTake(s_release_sem, 0);   // svuota eventuale give vecchia
    s_state = NV_VP_STOPPED;
    s_audio_stop_flag = true;                              // sblocca subito la task audio (pcm_write)
    vp_msg_t stop = { .cmd = VP_CMD_STOP };
    vp_msg_t rel  = { .cmd = VP_CMD_RELEASE };
    xQueueSend(s_q, &stop, pdMS_TO_TICKS(100));
    xQueueSend(s_q, &rel,  pdMS_TO_TICKS(100));
    // la task audio si auto-elimina; il video la attende gia' in stop_audio_task_and_wait prima di ritornare
    if (s_release_sem && xSemaphoreTake(s_release_sem, pdMS_TO_TICKS(4000)) != pdTRUE)
        NV_LOGE(TAG, "release: timeout teardown (task bloccata?) — risorse NON liberate");
}

bool nv_vplayer_is_video(const char *path){
    if (!path) return false;
    const char *d = strrchr(path, '.');
    if (!d) return false;
    return strcasecmp(d,".avi")==0 ||
#if CONFIG_NV_VPLAYER_H264
           strcasecmp(d,".mp4")==0 || strcasecmp(d,".h264")==0 ||
#endif
           strcasecmp(d,".mpg")==0 || strcasecmp(d,".mpeg")==0 || strcasecmp(d,".m1v")==0;
}
