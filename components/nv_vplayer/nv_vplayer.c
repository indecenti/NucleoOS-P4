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
#include "nv_sd.h"     // removal-safe fopen/fclose: card pull mid-decode must not free the volume under us
#include "nv_log.h"
#include "nv_audio.h"

#include "driver/jpeg_decode.h"
#include "driver/ppa.h"
#include "esp_cache.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_h264_dec_sw.h"   // software H.264 decoder (dual-core, P4-optimized lib)
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

static const char *TAG = "vplayer";

#define VP_MAXW    1280
#define VP_MAXH    720
#define VP_RING    3
#define VP_IN_CAP  (512 * 1024)      // one compressed JPEG frame

typedef enum { VP_CMD_OPEN, VP_CMD_PAUSE, VP_CMD_RESUME, VP_CMD_STOP, VP_CMD_RELEASE } vp_cmd_t;
typedef struct { vp_cmd_t cmd; char path[256]; } vp_msg_t;

static jpeg_decoder_handle_t s_dec = NULL;
static uint8_t  *s_in = NULL;   size_t s_in_cap = 0;
static uint8_t  *s_annex = NULL;   size_t s_annex_cap = 0;   // Annex-B assembly (H.264/MP4)
static uint8_t  *s_ring[VP_RING] = {0};   size_t s_ring_len = 0;
static int       s_widx = 0;
static ppa_client_handle_t s_vp_ppa = NULL;   // cached SRM client (registered once, not per frame)

static volatile int      s_cur = -1;       // published ring index
static volatile int      s_w = 0, s_h = 0;
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
void nv_mpeg1_set_int_budget(size_t bytes);    // nv_mpeg1_impl.c: internal-SRAM budget for frame planes
static volatile int      s_fps10 = 0;         // measured decode rate x10 (EMA)
static int64_t           s_last_pub_us = 0;

// ---- seek + audio-track state (shared across the active play_* function and the audio task) -------
static volatile int  s_vseek_ms = -1;      // set by nv_vplayer_seek(); consumed+cleared by play_avi/play_mp4
static volatile int  s_audio_seek_ms = -1; // mirrored for the audio task; consumed+cleared there
static volatile bool s_has_audio = false;  // true while an MP4 clip's AAC track is actively playing
static volatile bool s_audio_stop_flag = false;  // video loop -> audio task: wind down now
static volatile bool s_audio_running = false;    // audio task: true from start until just before exit
static volatile int  s_audio_pos_ms = 0;         // audio clock (backlog-corrected), the A/V sync master

// Update the measured decode fps (EMA) — called on every published frame.
static void note_frame(void){
    int64_t now = esp_timer_get_time();
    if (s_last_pub_us) {
        int64_t dt = now - s_last_pub_us;
        if (dt > 0) {
            int inst = (int)(10000000 / dt);                 // fps x10
            s_fps10 = s_fps10 ? (s_fps10 * 3 + inst) / 4 : inst;
        }
    }
    s_last_pub_us = now;
}

static QueueHandle_t s_q    = NULL;
static TaskHandle_t  s_task = NULL;
static SemaphoreHandle_t s_release_sem = NULL;   // vp_task -> release(): "risorse liberate"

// ---------------------------------------------------------------- little-endian file reads
static bool rdu32(FILE *f, uint32_t *v){ uint8_t b[4]; if (fread(b,1,4,f)!=4) return false; *v=(uint32_t)b[0]|((uint32_t)b[1]<<8)|((uint32_t)b[2]<<16)|((uint32_t)b[3]<<24); return true; }

// ---------------------------------------------------------------- resources
// Shared buffers (input JPEG/NAL scratch + Annex-B assembly + the RGB565/I420 display ring). The
// ring is DMA-capable (jpeg allocator) so both the JPEG decoder DMA and PPA can touch it.
static bool ensure_ring(void){
    if (!s_in) {
        jpeg_decode_memory_alloc_cfg_t im = { .buffer_direction = JPEG_DEC_ALLOC_INPUT_BUFFER };
        s_in = (uint8_t *)jpeg_alloc_decoder_mem(VP_IN_CAP, &im, &s_in_cap);
        if (!s_in) { NV_LOGE(TAG,"input buffer alloc failed"); return false; }
    }
    if (!s_annex) {
        s_annex = (uint8_t *)heap_caps_malloc(VP_IN_CAP + 4096, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!s_annex) { NV_LOGE(TAG,"annex buffer alloc failed"); return false; }
        s_annex_cap = VP_IN_CAP + 4096;
    }
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

// ---------------------------------------------------------------- one decoded frame -> ring
static void decode_publish(const uint8_t *jpg, uint32_t len){
    jpeg_decode_picture_info_t info;
    if (jpeg_decoder_get_info(jpg, len, &info) != ESP_OK) return;
    if (info.width == 0 || info.height == 0 || info.width > VP_MAXW || info.height > VP_MAXH) return;
    jpeg_decode_cfg_t cfg = {
        .output_format = JPEG_DECODE_OUT_FORMAT_RGB565,
        .rgb_order = JPEG_DEC_RGB_ELEMENT_ORDER_RGB,   // for an LVGL canvas; flip to BGR if swapped
        .conv_std = JPEG_YUV_RGB_CONV_STD_BT601,
    };
    uint32_t outsz = 0;
    if (jpeg_decoder_process(s_dec, &cfg, jpg, len, s_ring[s_widx], s_ring_len, &outsz) != ESP_OK) return;
    s_w = (int)info.width; s_h = (int)info.height;
    s_cur = s_widx; s_gen++;
    s_widx = (s_widx + 1) % VP_RING;
    note_frame();
    if (s_frame_cb) s_frame_cb(s_ring[s_cur], s_w, s_h);   // frame-accurate direct blit (MJPEG=RGB565)
}

// ---------------------------------------------------------------- AVI: locate movi + fps
static bool avi_locate(FILE *f, long *movi_pos, long *movi_end, uint32_t *uspf, uint32_t *total){
    uint8_t hdr[12];
    if (fread(hdr,1,12,f)!=12) return false;
    if (memcmp(hdr,"RIFF",4) || memcmp(hdr+8,"AVI ",4)) return false;
    *uspf=0; *total=0; *movi_pos=0; *movi_end=0;
    for (;;) {
        uint8_t cc[4]; uint32_t sz;
        if (fread(cc,1,4,f)!=4) break;
        if (!rdu32(f,&sz)) break;
        long data = ftell(f);
        long next = data + sz + (long)(sz & 1);
        if (memcmp(cc,"LIST",4)==0) {
            uint8_t lt[4];
            if (fread(lt,1,4,f)!=4) break;
            if (memcmp(lt,"movi",4)==0) {
                *movi_pos = ftell(f);
                *movi_end = data + sz;
                fseek(f, *movi_pos, SEEK_SET);
                return true;
            }
            long lend = data + sz;                     // scan hdrl for avih
            while (ftell(f) + 8 <= lend) {
                uint8_t scc[4]; uint32_t ssz;
                if (fread(scc,1,4,f)!=4) break;
                if (!rdu32(f,&ssz)) break;
                long sdata = ftell(f);
                if (memcmp(scc,"avih",4)==0) {
                    uint32_t v=0; rdu32(f,&v); *uspf=v;
                    fseek(f, sdata+16, SEEK_SET); uint32_t t=0; rdu32(f,&t); *total=t;
                }
                fseek(f, sdata + ssz + (long)(ssz & 1), SEEK_SET);
            }
        }
        fseek(f, next, SEEK_SET);
    }
    return (*movi_pos != 0);
}

// ---------------------------------------------------------------- AVI: idx1 seek index
// Every entry is a video frame in our content (our own MJPEG recordings never carry an audio
// stream), so idx1 order == frame order 1:1 and seeking is frame-accurate. The dwChunkOffset base
// is ambiguous across encoders (relative to the 'movi' payload vs the 'movi' LIST's own fourcc) —
// probed once by checking which base makes entry 0 land on a real video/audio chunk id. Entries are
// read straight off the file stream into small fixed local buffers (bounded fread, checked return),
// so no extra hardening is needed here beyond the existing return-value checks.
typedef struct { uint32_t offset; uint32_t size; } vp_avi_entry_t;

static bool avi_build_index(FILE *f, long movi_pos, long movi_end, long fsz, vp_avi_entry_t **out, uint32_t *out_n){
    fseek(f, movi_end, SEEK_SET);
    for (int guard = 0; guard < 8; guard++) {   // idx1 is normally the very next top-level chunk
        long pos = ftell(f);
        if (pos + 8 > fsz) return false;
        uint8_t cc[4]; uint32_t sz;
        if (fread(cc,1,4,f)!=4 || !rdu32(f,&sz)) return false;
        if (memcmp(cc,"idx1",4)==0) {
            uint32_t n = sz / 16;
            if (n == 0 || n > 200000) return false;   // memory-exhaustion guard on an attacker-set size
            vp_avi_entry_t *arr = (vp_avi_entry_t *)heap_caps_malloc((size_t)n * sizeof(vp_avi_entry_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            if (!arr) return false;
            uint32_t got = 0;
            for (uint32_t k = 0; k < n; k++) {
                uint8_t e[16];
                if (fread(e,1,16,f)!=16) break;
                uint32_t off = (uint32_t)e[8]|((uint32_t)e[9]<<8)|((uint32_t)e[10]<<16)|((uint32_t)e[11]<<24);
                uint32_t esz = (uint32_t)e[12]|((uint32_t)e[13]<<8)|((uint32_t)e[14]<<16)|((uint32_t)e[15]<<24);
                arr[got].offset = off; arr[got].size = esz; got++;
            }
            if (got == 0) { heap_caps_free(arr); return false; }
            long base = movi_pos;
            uint8_t peek[4];
            bool ok = (fseek(f, base + (long)arr[0].offset, SEEK_SET)==0) && fread(peek,1,4,f)==4 &&
                      ((peek[2]=='d' && (peek[3]=='c'||peek[3]=='b')) || (peek[2]=='w' && peek[3]=='b'));
            if (!ok) {
                base = movi_pos - 4;
                ok = (fseek(f, base + (long)arr[0].offset, SEEK_SET)==0) && fread(peek,1,4,f)==4 &&
                     ((peek[2]=='d' && (peek[3]=='c'||peek[3]=='b')) || (peek[2]=='w' && peek[3]=='b'));
            }
            if (!ok) { heap_caps_free(arr); return false; }
            for (uint32_t k=0;k<got;k++) arr[k].offset += (uint32_t)base;   // normalize to absolute file offsets
            *out = arr; *out_n = got;
            return true;
        }
        fseek(f, pos + 8 + sz + (long)(sz & 1), SEEK_SET);
    }
    return false;
}

// drain commands; returns true if the current clip should stop (STOP or a new OPEN, requeued)
static bool poll_cmd(void){
    vp_msg_t m;
    while (xQueueReceive(s_q, &m, 0) == pdTRUE) {
        switch (m.cmd) {
            case VP_CMD_STOP:    return true;
            case VP_CMD_OPEN:    xQueueSendToFront(s_q, &m, 0); return true;
            case VP_CMD_RELEASE: xQueueSendToFront(s_q, &m, 0); return true;  // esci dal play, il top-loop libera
            case VP_CMD_PAUSE:   s_state = NV_VP_PAUSED;  break;
            case VP_CMD_RESUME:  s_state = NV_VP_PLAYING; break;
        }
    }
    return false;
}

// ---------------------------------------------------------------- play one AVI file
static void play_avi(const char *path){
    if (!ensure_engine()) { s_state = NV_VP_ERROR; return; }
    FILE *f = nv_sd_fopen(path, "rb");
    if (!f) { NV_LOGW(TAG,"open fail: %s", path); s_state = NV_VP_ERROR; return; }

    long movi_pos=0, movi_end=0; uint32_t uspf=0, total=0;
    if (!avi_locate(f, &movi_pos, &movi_end, &uspf, &total)) { nv_sd_fclose(f); NV_LOGW(TAG,"not an AVI"); s_state = NV_VP_ERROR; return; }

    int period = uspf ? (int)(uspf/1000) : 66;   // ms/frame
    s_period_ms = period;
    if (period < 15) period = 15; else if (period > 250) period = 250;
    s_pos_ms = 0; s_dur_ms = (total && uspf) ? (int)((uint64_t)total * uspf / 1000ull) : 0;
    s_cur = -1; s_gen = 0; s_widx = 0; s_eot = false;
    s_is_yuv = false;   // MJPEG path publishes RGB565
    s_has_audio = false;   // AVI is video-only (our recordings never captured a mic track)
    s_vseek_ms = -1;
    s_fps10 = 0; s_last_pub_us = 0;
    s_playing = true;
    s_state = NV_VP_PLAYING;

    fseek(f,0,SEEK_END); long fsz = ftell(f);
    vp_avi_entry_t *idx = NULL; uint32_t idx_n = 0;
    bool have_idx = avi_build_index(f, movi_pos, movi_end, fsz, &idx, &idx_n);

    TickType_t next = xTaskGetTickCount();
    uint32_t frame_no = 0;
    bool stopped = false;

    fseek(f, movi_pos, SEEK_SET);
    while (!stopped && ftell(f) < movi_end) {
        if (poll_cmd()) { stopped = true; break; }
        if (s_state == NV_VP_PAUSED) { vTaskDelay(pdMS_TO_TICKS(30)); continue; }

        if (s_vseek_ms >= 0) {
            int want = s_vseek_ms; s_vseek_ms = -1;
            uint32_t target = period ? (uint32_t)want / (uint32_t)period : 0;
            if (have_idx && target < idx_n) {
                fseek(f, idx[target].offset, SEEK_SET);
                frame_no = target;
            } else if (!have_idx) {   // no index: linear scan from the start, counting video frames
                fseek(f, movi_pos, SEEK_SET);
                uint32_t fn = 0;
                while (fn < target && ftell(f) < movi_end) {
                    uint8_t scc[4]; uint32_t ssz;
                    if (fread(scc,1,4,f)!=4 || !rdu32(f,&ssz)) break;
                    long sdata = ftell(f);
                    if ((scc[2]=='d') && (scc[3]=='c'||scc[3]=='b')) fn++;
                    fseek(f, sdata + ssz + (long)(ssz & 1), SEEK_SET);
                }
                frame_no = fn;
            }
            s_pos_ms = (int)(frame_no * (uint32_t)period);
            next = xTaskGetTickCount();
        }

        uint8_t cc[4]; uint32_t sz;
        if (fread(cc,1,4,f)!=4) break;
        if (!rdu32(f,&sz)) break;
        long data = ftell(f);
        long nxt = data + sz + (long)(sz & 1);

        const bool video = (cc[2]=='d') && (cc[3]=='c' || cc[3]=='b');   // 00dc / 00db
        if (video && sz >= 2 && sz <= s_in_cap) {
            if (fread(s_in, 1, sz, f) == sz) {
                decode_publish(s_in, sz);
                frame_no++;
                s_pos_ms = (int)(frame_no * (uint32_t)period);
                vTaskDelayUntil(&next, pdMS_TO_TICKS(period));
            }
        }
        fseek(f, nxt, SEEK_SET);
    }

    if (idx) heap_caps_free(idx);
    nv_sd_fclose(f);
    if (!stopped) { s_eot = true; s_state = NV_VP_STOPPED; }
    else if (s_state != NV_VP_ERROR) s_state = NV_VP_STOPPED;
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
    esp_cache_msync(s_ring[s_widx], s_ring_len, ESP_CACHE_MSYNC_FLAG_DIR_C2M);
    s_w = w; s_h = h;
    s_cur = s_widx; s_gen++;
    s_widx = (s_widx + 1) % VP_RING;
    note_frame();
}

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

static const uint8_t k_sc[4] = {0,0,0,1};

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
    if (nsamp == 0 || nsamp > 2000000) return false;   // also a memory-exhaustion guard

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

    if (!in_bounds(B, stts.payload, 12)) return;
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
    if (xTaskCreate(audio_task, "vpaudio", 6144, ctx, 5, &s_audio_task) != pdPASS) {
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
static void play_mp4(const char *path, esp_h264_dec_handle_t *dec_ptr, esp_h264_dec_param_handle_t *param_ptr){
    FILE *f = nv_sd_fopen(path, "rb");
    if (!f) { s_state = NV_VP_ERROR; return; }
    fseek(f,0,SEEK_END); long fsz = ftell(f); fseek(f,0,SEEK_SET);

    // locate the moov box (top-level scan)
    long moov_off = 0; uint32_t moov_sz = 0; long p = 0; uint8_t hb[8];
    while (p + 8 <= fsz) {
        fseek(f, p, SEEK_SET); if (fread(hb,1,8,f)!=8) break;
        uint32_t bs = be32(hb); if (bs < 8) break;
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
        while (s_state == NV_VP_PAUSED) { if (poll_cmd()) { stop = true; break; } vTaskDelay(pdMS_TO_TICKS(30)); }
        if (stop) break;

        if (s_vseek_ms >= 0) {
            int want = s_vseek_ms; s_vseek_ms = -1;
            i = seek_video_index(&V, want);
            s_audio_seek_ms = want;
            need_params = true;
            esp_h264_dec_close(*dec_ptr); esp_h264_dec_del(*dec_ptr);
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
            if (nl == 0 || q + nl > ssz) break;
            if (al + 4 + nl > s_annex_cap) break;
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

    esp_h264_dec_close(dec);
    esp_h264_dec_del(dec);
    if (s_state != NV_VP_ERROR) s_state = NV_VP_STOPPED;
    s_playing = false;
}

// ---------------------------------------------------------------- MPEG-1 (pl_mpeg, software)
static inline uint16_t vp_rgb565(int r, int g, int b){
    if (r < 0) r = 0; else if (r > 255) r = 255;
    if (g < 0) g = 0; else if (g > 255) g = 255;
    if (b < 0) b = 0; else if (b > 255) b = 255;
    return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}
// YUV 4:2:0 planes -> packed RGB565 (dst stride = w px). Mirrors pl_mpeg's own 2x2-block convert
// (frame->*.width are the padded plane strides; frame->width/height the active picture size).
// Forced to -O3 (the file is -Os): this per-pixel loop is on the playback hot path.
__attribute__((optimize("O3")))
static void mpeg1_to_565(plm_frame_t *fr, uint16_t *dst, int w){
    int cols = (int)fr->width >> 1, rows = (int)fr->height >> 1;
    int yw = (int)fr->y.width, cw = (int)fr->cb.width;
    for (int row = 0; row < rows; row++){
        int c = row * cw, yi = row * 2 * yw, di = row * 2 * w;
        for (int col = 0; col < cols; col++){
            int cr = fr->cr.data[c] - 128;
            int cb = fr->cb.data[c] - 128;
            int r = (cr * 104597) >> 16;
            int g = (cb * 25674 + cr * 53278) >> 16;
            int b = (cb * 132201) >> 16;
            int y;
            y = ((fr->y.data[yi]      - 16) * 76309) >> 16; dst[di]     = vp_rgb565(y+r, y-g, y+b);
            y = ((fr->y.data[yi+1]    - 16) * 76309) >> 16; dst[di+1]   = vp_rgb565(y+r, y-g, y+b);
            y = ((fr->y.data[yi+yw]   - 16) * 76309) >> 16; dst[di+w]   = vp_rgb565(y+r, y-g, y+b);
            y = ((fr->y.data[yi+yw+1] - 16) * 76309) >> 16; dst[di+w+1] = vp_rgb565(y+r, y-g, y+b);
            c += 1; yi += 2; di += 2;
        }
    }
}

static int16_t *s_ampcm = NULL;   // MP2 float->int16 scratch (1152 stereo samples)

// pl_mpeg fires this per decoded frame: convert YUV->RGB565 into the ring and publish.
static void mpeg1_video_cb(plm_t *plm, plm_frame_t *frame, void *user){
    (void)plm; (void)user;
    mpeg1_to_565(frame, (uint16_t *)s_ring[s_widx], (int)frame->width);
    esp_cache_msync(s_ring[s_widx], s_ring_len, ESP_CACHE_MSYNC_FLAG_DIR_C2M);
    s_w = (int)frame->width; s_h = (int)frame->height;
    s_cur = s_widx; s_gen++;
    s_widx = (s_widx + 1) % VP_RING;
    note_frame();
    if (s_frame_cb) s_frame_cb(s_ring[s_cur], s_w, s_h);   // frame-accurate direct blit (RGB565)
    s_pos_ms = (int)(frame->time * 1000.0);       // presentation time
    s_audio_pos_ms = s_pos_ms;                     // nv_vplayer_pos_ms() reads this when has_audio
}
// pl_mpeg fires this per MP2 audio frame (1152 stereo float samples in [-1,1]): -> int16 -> nv_audio.
// pcm_write blocks when the sink ring is full, which paces the whole decode loop to audio real-time.
static volatile uint32_t s_adbg = 0;   // audio frames fed (diagnostic)
static int s_arate = 48000;             // active audio sample rate (for the backlog gate)
static void mpeg1_audio_cb(plm_t *plm, plm_samples_t *s, void *user){
    (void)plm; (void)user;
    if (!s_ampcm) return;
    // Backlog SAFETY gate (not the primary buffer control — that's plm_set_audio_lead_time): pl_mpeg
    // decodes audio+video in ONE task and nv_audio_pcm_write BLOCKS only when the ~11 s sink ring is
    // full. We target ~1 s of buffered audio (the lead) so a heavy video frame can't drain the ring to
    // an underrun; this gate at ~2 s is just a ceiling well under the ring so pcm_write never blocks.
    if (nv_audio_pcm_backlog() > (size_t)s_arate * 8) return;   // ~2 s (48 kHz stereo 16-bit)
    const int n = (int)s->count * 2;   // interleaved stereo
    for (int i = 0; i < n; i++) {
        float v = s->interleaved[i];
        if (v > 1.0f) v = 1.0f; else if (v < -1.0f) v = -1.0f;
        s_ampcm[i] = (int16_t)(v * 32767.0f);
    }
    nv_audio_pcm_write(s_ampcm, (size_t)n * sizeof(int16_t));
    if ((s_adbg++ % 400) == 0) NV_LOGI(TAG, "mpeg1: audio fed %u frames", (unsigned)s_adbg);
}

// Software MPEG-1 playback (pl_mpeg). Publishes RGB565 (s_is_yuv=false) so it rides the SAME render
// path as MJPEG — deliberately NOT the I420->PPA-YUV420 path that hangs the H.264 case. Audio (MP2)
// is decoded and pushed to nv_audio (routes to the USB card / ES8311 like music). plm_decode() is
// driven by a real esp_timer clock so audio + video stay in sync; the per-call elapsed is clamped so
// a slow stretch degrades video gracefully instead of blocking the task.
static void play_mpeg1(const char *path){
    if (!ensure_ring()) { s_state = NV_VP_ERROR; return; }
    // (Tried luma planes in internal SRAM — measured NO decode gain: the path is compute-bound, not
    // PSRAM-latency-bound. Left at 0 so we don't hold scarce internal SRAM for nothing.)
    nv_mpeg1_set_int_budget(0);
    plm_t *plm = plm_create_with_filename(path);
    if (!plm) { NV_LOGW(TAG,"mpeg1: open failed %s", path); s_err_reason = "MPEG-1: apertura fallita"; s_state = NV_VP_ERROR; return; }
    int w = plm_get_width(plm), h = plm_get_height(plm);
    double fr = plm_get_framerate(plm);
    s_period_ms = (fr > 0) ? (int)(1000.0 / fr + 0.5) : 42;
    if (w <= 0 || h <= 0 || w > VP_MAXW || h > VP_MAXH) {
        NV_LOGW(TAG,"mpeg1: bad/oversized resolution %dx%d", w, h);
        s_err_reason = "MPEG-1: risoluzione non supportata"; plm_destroy(plm); s_state = NV_VP_ERROR; return;
    }

    const bool has_audio = plm_get_num_audio_streams(plm) > 0;
    NV_LOGI(TAG, "mpeg1: %dx%d @ %.1f fps, audio=%s", w, h, fr, has_audio ? "yes" : "no");
    plm_set_video_decode_callback(plm, mpeg1_video_cb, NULL);
    if (has_audio) {
        plm_set_audio_enabled(plm, 1);
        plm_set_audio_stream(plm, 0);
        plm_set_audio_lead_time(plm, 1.0);   // pre-decode ~1 s of audio ahead -> cushion that absorbs
                                             // video-decode spikes on the shared core (kills the USB
                                             // underrun stutter). Ring is ~11 s so this never blocks.
        plm_set_audio_decode_callback(plm, mpeg1_audio_cb, NULL);
        s_ampcm = (int16_t *)heap_caps_malloc(PLM_AUDIO_SAMPLES_PER_FRAME * 2 * sizeof(int16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        s_arate = plm_get_samplerate(plm);
        nv_audio_pcm_begin(s_arate, 2, 16);
    } else {
        plm_set_audio_enabled(plm, 0);
    }

    s_cur = -1; s_gen = 0; s_widx = 0; s_pos_ms = 0; s_eot = false;
    s_fps10 = 0; s_last_pub_us = 0; s_dec_err_logged = false; s_err_reason = "";
    s_dur_ms = (int)(plm_get_duration(plm) * 1000.0);
    s_has_audio = has_audio;
    s_is_yuv = false;   // RGB565 output -> working render path
    s_playing = true; s_state = NV_VP_PLAYING;

    // Pacing model: decode one frame-time's worth of stream per iteration. When there's an audio
    // track, mpeg1_audio_cb's nv_audio_pcm_write BLOCKS once the ~3 s sink ring is full — that block
    // is what paces the whole loop to audio real-time (audio is the master clock, video rides along
    // in sync because plm_decode interleaves both by PTS). No wall clock needed; that earlier
    // esp_timer approach starved the audio ring (underruns) and left pos stuck. Without audio we pace
    // the video ourselves to the source framerate.
    // Real-time pacing: advance pl_mpeg's internal clock by the ACTUAL elapsed wall time each pass,
    // so it decodes exactly real-time's worth of audio+video (no racing). plm_decode interleaves both
    // by PTS, so they stay in sync; a ~5 ms loop keeps the granularity fine. The earlier "fixed tick"
    // approach let self->time race, which starved audio and pinned fps at the raw decode ceiling.
    s_adbg = 0;
    int64_t last = esp_timer_get_time();
    bool stop = false;
    while (!stop) {
        if (poll_cmd()) break;
        while (s_state == NV_VP_PAUSED) { if (poll_cmd()) { stop = true; break; } vTaskDelay(pdMS_TO_TICKS(30)); last = esp_timer_get_time(); }
        if (stop) break;
        if (s_vseek_ms >= 0) { double t = s_vseek_ms / 1000.0; s_vseek_ms = -1; plm_seek(plm, t, 0); if (has_audio) nv_audio_pcm_flush(); last = esp_timer_get_time(); }
        int64_t now = esp_timer_get_time();
        double el = (now - last) / 1e6; last = now;
        if (el > 0.25) el = 0.25;   // after a hitch, cap the catch-up burst
        plm_decode(plm, el);
        if (plm_has_ended(plm)) { stop = true; s_eot = true; break; }
        vTaskDelay(pdMS_TO_TICKS(5));
    }

    if (has_audio) { nv_audio_pcm_end(); }
    if (s_ampcm) { heap_caps_free(s_ampcm); s_ampcm = NULL; }
    s_has_audio = false;
    plm_destroy(plm);
    if (s_state != NV_VP_ERROR) s_state = NV_VP_STOPPED;
    s_playing = false;
}

// ---------------------------------------------------------------- resource teardown
// SOLO vp_task tocca s_ring/s_in/s_annex/s_dec/s_vp_ppa: nessun free cross-thread, nessun lock.
// release() manda VP_CMD_RELEASE e aspetta il semaforo -> la corsa open/close che corrompeva l'heap
// (decode su buffer/engine gia' liberati -> freeze) e' impossibile.
static void free_resources(void){
    for (int i = 0; i < VP_RING; i++) { if (s_ring[i]) { free(s_ring[i]); s_ring[i] = NULL; } }
    if (s_in)    { free(s_in);  s_in = NULL; s_in_cap = 0; }
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
            else                                                       play_h264(m.path);  // .mp4 / .h264 (SW decode)
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
    // 16 KB stack: pl_mpeg's IDCT/motion-comp call chain needs more headroom than the 6 KB the
    // HW-JPEG / h264 paths used. Pinned to core 1 (APP): keeps the heavy SW decode off core 0 where
    // Wi-Fi/LWIP + the LVGL tick live, so the UI and network stay responsive during playback.
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
int nv_vplayer_pos_ms(void){ return s_has_audio ? s_audio_pos_ms : s_pos_ms; }
int nv_vplayer_dur_ms(void){ return s_dur_ms; }
int nv_vplayer_fps10(void){ return s_fps10; }   // measured decode rate x10
int nv_vplayer_period_ms(void){ return s_period_ms; }

const char *nv_vplayer_err_reason(void){ return s_err_reason ? s_err_reason : ""; }

void nv_vplayer_set_frame_cb(nv_vp_frame_cb_t cb){ s_frame_cb = cb; }

bool nv_vplayer_took_eot(void){ if (!s_eot) return false; s_eot = false; return true; }

const uint8_t *nv_vplayer_frame(int *w, int *h, uint32_t *generation){
    int c = s_cur;
    if (c < 0) return NULL;
    if (w) *w = s_w;
    if (h) *h = s_h;
    if (generation) *generation = s_gen;
    return s_ring[c];
}

static int s_aspect = NV_VP_FIT;   // Nv_vp_aspect_t; applied by nv_vplayer_render()
void nv_vplayer_set_aspect(nv_vp_aspect_t mode){
    if (mode < NV_VP_FIT || mode > NV_VP_ZOOM) return;
    s_aspect = (int)mode;
}

bool nv_vplayer_render(uint8_t *dst, int dw, int dh){
    int c = s_cur, w = s_w, h = s_h;
    if (c < 0 || !dst || dw <= 0 || dh <= 0 || w <= 0 || h <= 0) return false;
    uint8_t *src = s_ring[c];
    esp_cache_msync(src, s_ring_len, ESP_CACHE_MSYNC_FLAG_DIR_M2C);   // buf is 64B-aligned

    if (!s_vp_ppa) {   // register the SRM client once and reuse it every frame
        ppa_client_config_t cc = { .oper_type = PPA_OPERATION_SRM };
        if (ppa_register_client(&cc, &s_vp_ppa) != ESP_OK) { s_vp_ppa = NULL; return false; }
    }

    ppa_srm_oper_config_t op = {0};
    op.in.buffer  = src;  op.in.pic_w = w;  op.in.pic_h = h;
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
    return strcasecmp(d,".avi")==0 || strcasecmp(d,".mp4")==0 || strcasecmp(d,".h264")==0 ||
           strcasecmp(d,".mpg")==0 || strcasecmp(d,".mpeg")==0 || strcasecmp(d,".m1v")==0;
}
