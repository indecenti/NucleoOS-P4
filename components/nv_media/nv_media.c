// nv_media — audio playback engine (see header). A worker task decodes a file with the Espressif
// unified "simple decoder" (WAV/MP3/AAC/FLAC/M4A) and streams PCM to the ES8311 DAC through
// nv_audio's PCM path. The decoder consumes input of any size and asks for more via DATA_LACK;
// we carry any unconsumed tail to the front of the input buffer and refill from the file.
#include "nv_media.h"
#include "nv_audio.h"
#include "nv_sd.h"     // removal-safe fopen/fclose: never fread a track while the card is being torn down
#include "nv_log.h"

#include "esp_audio_simple_dec.h"
#include "esp_audio_simple_dec_default.h"
#include "esp_audio_dec_default.h"   // core codec layer — must be registered too (see init)
#include "nv_mp3dec.h"               // minimp3: MP3 goes through this (see header for why)
#include "esp_audio_types.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"

#include <stdio.h>
#include <string.h>
#include <strings.h>

static const char *TAG = "media";

typedef enum { CMD_PLAY, CMD_PAUSE, CMD_RESUME, CMD_STOP } cmd_kind_t;
typedef struct { cmd_kind_t cmd; char path[256]; } msg_t;

static QueueHandle_t s_q     = NULL;
static TaskHandle_t  s_task  = NULL;
static bool          s_registered = false;

static volatile nv_media_state_t s_state = NV_MEDIA_STOPPED;
static volatile int  s_pos_ms = 0;
static volatile int  s_dur_ms = 0;
static volatile bool s_eot    = false;

// Seek + track info (UI thread reads/writes the volatiles; the decode loop consumes).
static volatile int  s_seek_req  = -1;    // 0..1000 target position, -1 = none
static volatile bool s_seekable  = false; // frame-sync codecs only (MP3/AAC)
static volatile int  s_trk_rate  = 0, s_trk_ch = 0, s_trk_bits = 0;
static int           s_pos_base_ms = 0;   // time base after a seek (decode-loop private)

// ---------------------------------------------------------------- helpers

static esp_audio_simple_dec_type_t type_for(const char *path) {
    const char *d = strrchr(path, '.');
    if (!d) return ESP_AUDIO_SIMPLE_DEC_TYPE_NONE;
    if (!strcasecmp(d, ".mp3"))  return ESP_AUDIO_SIMPLE_DEC_TYPE_MP3;
    if (!strcasecmp(d, ".wav"))  return ESP_AUDIO_SIMPLE_DEC_TYPE_WAV;
    if (!strcasecmp(d, ".aac"))  return ESP_AUDIO_SIMPLE_DEC_TYPE_AAC;
    if (!strcasecmp(d, ".m4a"))  return ESP_AUDIO_SIMPLE_DEC_TYPE_M4A;
    if (!strcasecmp(d, ".flac")) return ESP_AUDIO_SIMPLE_DEC_TYPE_FLAC;
    return ESP_AUDIO_SIMPLE_DEC_TYPE_NONE;
}

// Drain any pending transport commands. Returns true if the caller should STOP the current track
// (STOP, or a new PLAY which is requeued for the task loop to pick up). Handles PAUSE/RESUME inline.
static bool poll_commands(void) {
    msg_t m;
    while (xQueueReceive(s_q, &m, 0) == pdTRUE) {
        switch (m.cmd) {
            case CMD_STOP:   return true;
            case CMD_PLAY:   xQueueSendToFront(s_q, &m, 0); return true;  // let the task restart
            case CMD_PAUSE:  s_state = NV_MEDIA_PAUSED;  nv_audio_pcm_pause(true);  break;
            case CMD_RESUME: s_state = NV_MEDIA_PLAYING; nv_audio_pcm_pause(false); break;
        }
    }
    return false;
}

static void play_file(const char *path);   // generic esp_audio path (fwd: WAV falls back to it)

// Decode is over but up to ring-depth seconds are still QUEUED. Wait them out WITHOUT
// deafening the transport: commands still poll (a tap interrupts instantly), pause keeps the
// tail frozen. Returns true when a command interrupted — the caller flushes and moves on.
// (Without this, a short file that fits the ring entirely — "test tone" — made pause/next
// dead for its whole duration inside pcm_end's blocking drain.)
static bool tail_wait(void) {
    for (;;) {
        if (poll_commands()) return true;
        if (nv_audio_pcm_backlog() == 0 && s_state != NV_MEDIA_PAUSED) return false;
        vTaskDelay(pdMS_TO_TICKS(30));
    }
}

// ---------------------------------------------------------------- WAV (native reader)
// WAV is raw PCM in a chunked container — no decoder needed at all. The esp_audio WAV path
// stack-faulted the media task on real hardware; this reader is ~zero stack, byte-exact on
// seek, and feeds the playout ring straight from SD.
static void play_file_wav(const char *path) {
    FILE *f = nv_sd_fopen(path, "rb");
    if (!f) { s_state = NV_MEDIA_ERROR; return; }

    uint8_t h[512];
    size_t n = fread(h, 1, sizeof h, f);
    uint32_t rate = 0, byterate = 0, data_off = 0, data_len = 0;
    uint16_t ch = 0, bits = 0, fmtcode = 0;
    if (n >= 44 && !memcmp(h, "RIFF", 4) && !memcmp(h + 8, "WAVE", 4)) {
        size_t off = 12;
        while (off + 8 <= n) {
            const uint32_t csz = h[off+4] | (h[off+5] << 8) | (h[off+6] << 16) | ((uint32_t)h[off+7] << 24);
            if (!memcmp(h + off, "fmt ", 4) && off + 24 <= n) {
                fmtcode  = h[off+8]  | (h[off+9]  << 8);
                ch       = h[off+10] | (h[off+11] << 8);
                rate     = h[off+12] | (h[off+13] << 8) | (h[off+14] << 16) | ((uint32_t)h[off+15] << 24);
                byterate = h[off+16] | (h[off+17] << 8) | (h[off+18] << 16) | ((uint32_t)h[off+19] << 24);
                bits     = h[off+22] | (h[off+23] << 8);
            }
            if (!memcmp(h + off, "data", 4)) { data_off = off + 8; data_len = csz; break; }
            if (csz > n) break;   // untrusted stride: a huge/overflow csz would wrap `off` to a
                                  // non-advancing value -> infinite loop -> WDT reset. Stop instead.
            off += 8 + csz + (csz & 1);
        }
    }
    // PCM 16-bit only here (the recorder's own format); anything exotic -> generic decoder.
    if (!data_off || !rate || !ch || bits != 16 || (fmtcode != 1 && fmtcode != 0xFFFE)) {
        nv_sd_fclose(f);
        play_file(path);
        return;
    }

    const uint32_t frame = (uint32_t)ch * 2;
    s_seekable = true;
    s_seek_req = -1;
    s_pos_base_ms = 0;
    s_trk_rate = (int)rate; s_trk_ch = ch; s_trk_bits = 16;
    s_dur_ms = byterate ? (int)((uint64_t)data_len * 1000ULL / byterate) : 0;
    s_pos_ms = 0;
    s_eot = false;

    const size_t CH = 8192;
    uint8_t *buf = heap_caps_malloc(CH, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) { nv_sd_fclose(f); s_state = NV_MEDIA_ERROR; return; }
    if (!nv_audio_pcm_begin((int)rate, ch, 16)) {
        heap_caps_free(buf); nv_sd_fclose(f); s_state = NV_MEDIA_ERROR; return;
    }
    s_state = NV_MEDIA_PLAYING;
    fseek(f, (long)data_off, SEEK_SET);

    bool stopped = false;
    uint64_t done = 0;
    while (done < data_len) {
        if (poll_commands()) { stopped = true; break; }
        if (s_state == NV_MEDIA_PAUSED) { vTaskDelay(pdMS_TO_TICKS(30)); continue; }
        const int sk = s_seek_req;
        if (sk >= 0) {
            s_seek_req = -1;
            nv_audio_pcm_flush();
            done = ((uint64_t)data_len * sk / 1000) / frame * frame;   // frame-aligned
            fseek(f, (long)(data_off + done), SEEK_SET);
        }
        size_t want = data_len - done < CH ? (size_t)(data_len - done) : CH;
        const size_t got = fread(buf, 1, want, f);
        if (!got) break;
        if (nv_audio_pcm_write(buf, got) < 0) { stopped = true; break; }
        done += got;
        s_pos_ms = (int)(done * 1000ULL / byterate);
    }
    if (!stopped && tail_wait()) stopped = true;   // audible tail, transport stays live
    if (stopped) nv_audio_pcm_flush();
    else s_eot = true;                             // real (heard) end of track
    nv_audio_pcm_end();
    heap_caps_free(buf);
    nv_sd_fclose(f);
    s_state = NV_MEDIA_STOPPED;
}

// ---------------------------------------------------------------- MP3 via minimp3
// Dedicated path (why: nv_mp3dec.h). Hot state internal, input window 16 KB in PSRAM,
// per-frame decode with native garbage resync, byte-proportional seek, PSRAM-ring backpressure.
static void play_file_mp3(const char *path) {
    FILE *f = nv_sd_fopen(path, "rb");
    if (!f) { NV_LOGW(TAG, "open fail: %s", path); s_state = NV_MEDIA_ERROR; return; }
    fseek(f, 0, SEEK_END);
    const long fsize = ftell(f);
    fseek(f, 0, SEEK_SET);

    long audio_start = 0;                       // skip a leading ID3v2 tag (cover art etc.)
    {
        uint8_t h[10];
        if (fread(h, 1, 10, f) == 10 && h[0] == 'I' && h[1] == 'D' && h[2] == '3')
            audio_start = 10 + (((long)(h[6] & 0x7F) << 21) | ((long)(h[7] & 0x7F) << 14) |
                                ((long)(h[8] & 0x7F) << 7)  |  (long)(h[9] & 0x7F));
        if (audio_start < 0 || audio_start >= fsize) audio_start = 0;
        fseek(f, audio_start, SEEK_SET);
    }
    const long audio_size = fsize - audio_start;

    const size_t IN = 16384;
    uint8_t *inbuf = heap_caps_malloc(IN, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    static int16_t s_pcm[NV_MP3_MAX_SAMPLES];   // internal .bss (4.6 KB): decoder's hot output
    if (!inbuf) { NV_LOGE(TAG, "mp3 buffer oom"); nv_sd_fclose(f); s_state = NV_MEDIA_ERROR; return; }

    nv_mp3dec_reset();
    s_seekable = true;
    s_seek_req = -1;
    s_pos_base_ms = 0;
    s_trk_rate = s_trk_ch = s_trk_bits = 0;
    s_dur_ms = nv_media_probe_dur_ms(path);     // Xing exact / CBR estimate, ID3-free
    s_pos_ms = 0;
    s_eot = false;
    s_state = NV_MEDIA_PLAYING;

    bool begun = false, stopped = false, errored = false, eof = false;
    int rate = 0, ch = 0;
    uint64_t out_bytes = 0;
    size_t fill = 0, off = 0;
    int64_t t_read = 0, t_dec = 0, t_push = 0, t0;
    uint64_t out_5s = 0;
    int64_t next_pipe_log = esp_timer_get_time() + 5000000;

    for (;;) {
        if (poll_commands()) { stopped = true; break; }
        if (s_state == NV_MEDIA_PAUSED) { vTaskDelay(pdMS_TO_TICKS(30)); continue; }

        const int sk = s_seek_req;
        if (sk >= 0) {
            s_seek_req = -1;
            if (audio_size > 0 && s_dur_ms > 0) {
                nv_audio_pcm_flush();           // drop pre-seek audio still queued
                fseek(f, audio_start + (long)((int64_t)audio_size * sk / 1000), SEEK_SET);
                fill = off = 0;
                eof = false;
                nv_mp3dec_reset();              // clear the bit reservoir
                out_bytes = 0;
                s_pos_base_ms = (int)((int64_t)s_dur_ms * sk / 1000);
                s_pos_ms = s_pos_base_ms;
            }
        }

        // Compact the window when the tail gets short, then refill from SD.
        if (off && (fill - off < 2048 || fill == IN)) {
            memmove(inbuf, inbuf + off, fill - off);
            fill -= off;
            off = 0;
        }
        if (!eof && fill < IN) {
            t0 = esp_timer_get_time();
            const size_t got = fread(inbuf + fill, 1, IN - fill, f);
            t_read += esp_timer_get_time() - t0;
            if (!got) eof = true; else fill += got;
        }
        if (fill == off) break;                 // out of data (EOT decided after the tail plays)

        int hz = 0, nch = 0, fb = 0;
        t0 = esp_timer_get_time();
        const int samples = nv_mp3dec_frame(inbuf + off, (int)(fill - off), s_pcm, &hz, &nch, &fb);
        t_dec += esp_timer_get_time() - t0;
        if (fb == 0) {                          // decoder wants more input
            if (eof) break;                     // trailing garbage: done decoding
            continue;
        }
        off += (size_t)fb;

        if (samples > 0 && hz && nch) {
            if (!begun) {
                rate = hz; ch = nch;
                if (!nv_audio_pcm_begin(rate, ch, 16)) { errored = stopped = true; break; }
                begun = true;
                s_trk_rate = rate; s_trk_ch = ch; s_trk_bits = 16;
            }
            const size_t bytes = (size_t)samples * nch * 2;
            t0 = esp_timer_get_time();
            if (nv_audio_pcm_write(s_pcm, bytes) < 0) { stopped = true; break; }
            t_push += esp_timer_get_time() - t0;
            out_5s += bytes;
            out_bytes += bytes;
            s_pos_ms = s_pos_base_ms + (int)(out_bytes * 1000ULL / ((uint64_t)rate * ch * 2));
        }

        if (esp_timer_get_time() >= next_pipe_log) {
            // Quiet sentinel: warn only when the producer runs under realtime (rate*ch*2 B/s).
            const unsigned need = rate ? (unsigned)rate * ch * 2 / 1024 : 0;
            const unsigned got_kbs = (unsigned)(out_5s / 5 / 1024);
            if (need && got_kbs + 4 < need)
                NV_LOGW(TAG, "mp3 UNDER REALTIME: out=%uKB/s (need %u) read=%ums dec=%ums push=%ums",
                        got_kbs, need, (unsigned)(t_read / 1000),
                        (unsigned)(t_dec / 1000), (unsigned)(t_push / 1000));
            t_read = t_dec = t_push = 0;
            out_5s = 0;
            next_pipe_log = esp_timer_get_time() + 5000000;
        }
    }

    NV_LOGI(TAG, "mp3 end: %s pos=%dms/%dms consumed=%ld/%ld",
            errored ? "ERROR" : stopped ? "stopped" : "eot",
            s_pos_ms, s_dur_ms, ftell(f) - audio_start, audio_size);

    if (begun) {
        if (!stopped && !errored && tail_wait()) stopped = true;   // audible tail, commands live
        if (stopped) nv_audio_pcm_flush();
        else if (!errored) s_eot = true;        // real (heard) end of track
        nv_audio_pcm_end();
    }
    nv_sd_fclose(f);
    heap_caps_free(inbuf);
    s_state = errored ? NV_MEDIA_ERROR : NV_MEDIA_STOPPED;
}

// ---------------------------------------------------------------- decode one file

static void play_file(const char *path) {
    const esp_audio_simple_dec_type_t ty = type_for(path);
    if (ty == ESP_AUDIO_SIMPLE_DEC_TYPE_NONE) { s_state = NV_MEDIA_ERROR; return; }
    // Byte-proportional seeking relies on the decoder re-syncing mid-stream: safe for the
    // frame-sync codecs, not for containers/lossless (WAV headers, FLAC frames, M4A atoms).
    s_seekable = (ty == ESP_AUDIO_SIMPLE_DEC_TYPE_MP3 || ty == ESP_AUDIO_SIMPLE_DEC_TYPE_AAC);
    s_seek_req = -1;
    s_pos_base_ms = 0;
    s_trk_rate = s_trk_ch = s_trk_bits = 0;

    FILE *f = nv_sd_fopen(path, "rb");
    if (!f) { NV_LOGW(TAG, "open fail: %s", path); s_state = NV_MEDIA_ERROR; return; }
    fseek(f, 0, SEEK_END);
    const long fsize = ftell(f);
    fseek(f, 0, SEEK_SET);

    // Skip a leading ID3v2 tag (DistroKid-style MP3s carry hundreds of KB of cover art):
    // feeding it to the decoder wastes time, risks false frame syncs, and inflates the
    // byte-based duration estimate. audio_start/audio_size are the REAL audio region.
    long audio_start = 0;
    {
        uint8_t h[10];
        if (fread(h, 1, 10, f) == 10 && h[0] == 'I' && h[1] == 'D' && h[2] == '3')
            audio_start = 10 + (((long)(h[6] & 0x7F) << 21) | ((long)(h[7] & 0x7F) << 14) |
                                ((long)(h[8] & 0x7F) << 7)  |  (long)(h[9] & 0x7F));
        if (audio_start < 0 || audio_start >= fsize) audio_start = 0;
        fseek(f, audio_start, SEEK_SET);
    }
    const long audio_size = fsize - audio_start;

    esp_audio_simple_dec_cfg_t cfg = { .dec_type = ty, .dec_cfg = NULL, .cfg_size = 0,
                                       .use_frame_dec = false };
    esp_audio_simple_dec_handle_t dec = NULL;
    const size_t int_b = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    const size_t psr_b = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    const esp_audio_err_t oe = esp_audio_simple_dec_open(&cfg, &dec);
    NV_LOGI(TAG, "dec state: internal %+d B, psram %+d B",
            (int)heap_caps_get_free_size(MALLOC_CAP_INTERNAL) - (int)int_b,
            (int)heap_caps_get_free_size(MALLOC_CAP_SPIRAM) - (int)psr_b);
    if (oe != ESP_AUDIO_ERR_OK || !dec) {
        NV_LOGW(TAG, "decoder open fail (%s) err=%d registered=%d free_int=%u largest=%u",
                esp_audio_simple_dec_get_name(ty), (int)oe, (int)s_registered,
                (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
        nv_sd_fclose(f); s_state = NV_MEDIA_ERROR; return;
    }

    const size_t IN = 4096;
    size_t OUT = 16384;
    uint8_t *inbuf  = heap_caps_malloc(IN,  MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    uint8_t *outbuf = heap_caps_malloc(OUT, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!inbuf || !outbuf) {
        NV_LOGE(TAG, "buffer oom");
        if (inbuf) heap_caps_free(inbuf);
        if (outbuf) heap_caps_free(outbuf);
        esp_audio_simple_dec_close(dec); nv_sd_fclose(f); s_state = NV_MEDIA_ERROR; return;
    }

    bool     begun = false, stopped = false, errored = false;
    int      rate = 0, ch = 0, bits = 0;
    uint64_t out_bytes = 0;
    size_t   fill = 0;   // leftover bytes carried at the front of inbuf
    int      badf = 0;   // corrupt-frame resyncs on this track (bounded)
    esp_audio_err_t last_e = ESP_AUDIO_ERR_OK;
    // Producer telemetry: where does the wall-clock go? (SD read vs decode vs ring write)
    int64_t  t_read = 0, t_dec = 0, t_push = 0, t0;
    uint64_t out_5s = 0;
    int64_t  next_pipe_log = esp_timer_get_time() + 5000000;

    s_pos_ms = 0; s_dur_ms = 0; s_eot = false;
    s_state = NV_MEDIA_PLAYING;

    for (;;) {
        if (poll_commands()) { stopped = true; break; }
        if (s_state == NV_MEDIA_PAUSED) { vTaskDelay(pdMS_TO_TICKS(30)); continue; }

        const int sk = s_seek_req;
        if (sk >= 0) {
            s_seek_req = -1;
            // Seek: fresh decoder + byte-proportional file offset; the MP3/AAC frame sync
            // realigns on the next frame boundary. Time restarts from the proportional base.
            if (begun && s_seekable && audio_size > 0 && s_dur_ms > 0) {
                nv_audio_pcm_flush();          // drop pre-seek audio still in the playout ring
                esp_audio_simple_dec_close(dec);
                dec = NULL;
                if (esp_audio_simple_dec_open(&cfg, &dec) != ESP_AUDIO_ERR_OK || !dec) { errored = true; break; }
                fseek(f, audio_start + (long)((int64_t)audio_size * sk / 1000), SEEK_SET);
                fill = 0;
                out_bytes = 0;
                s_pos_base_ms = (int)((int64_t)s_dur_ms * sk / 1000);
                s_pos_ms = s_pos_base_ms;
            }
        }

        // Buffer wedged (decoder consumed nothing with a FULL buffer — a run of garbage with
        // no frame sync): shift 2 bytes out and re-run, instead of mislabeling it as EOF.
        if (fill == IN) {
            memmove(inbuf, inbuf + 2, fill - 2);
            fill -= 2;
        }
        t0 = esp_timer_get_time();
        const size_t got = fread(inbuf + fill, 1, IN - fill, f);
        t_read += esp_timer_get_time() - t0;
        const size_t avail = fill + got;
        const bool   eof = (got == 0);

        if (esp_timer_get_time() >= next_pipe_log) {
            NV_LOGI(TAG, "pipe: out=%uKB/s read=%ums dec=%ums push=%ums",
                    (unsigned)(out_5s / 5 / 1024), (unsigned)(t_read / 1000),
                    (unsigned)(t_dec / 1000), (unsigned)(t_push / 1000));
            t_read = t_dec = t_push = 0;
            out_5s = 0;
            next_pipe_log = esp_timer_get_time() + 5000000;
        }

        esp_audio_simple_dec_raw_t raw = { .buffer = inbuf, .len = (uint32_t)avail,
                                           .eos = eof, .consumed = 0 };
        bool progressed = false;

        while (raw.len > 0) {
            esp_audio_simple_dec_out_t out = { .buffer = outbuf, .len = (uint32_t)OUT };
            t0 = esp_timer_get_time();
            const esp_audio_err_t e = esp_audio_simple_dec_process(dec, &raw, &out);
            t_dec += esp_timer_get_time() - t0;

            if (e == ESP_AUDIO_ERR_BUFF_NOT_ENOUGH) {
                const size_t need = out.needed_size ? out.needed_size : OUT * 2;
                uint8_t *nb = heap_caps_realloc(outbuf, need, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
                if (!nb) { errored = stopped = true; break; }
                outbuf = nb; OUT = need;
                continue;   // retry the same frame with a bigger buffer
            }
            if (e != ESP_AUDIO_ERR_OK && e != ESP_AUDIO_ERR_CONTINUE) {
                last_e = e;
                if (e == ESP_AUDIO_ERR_DATA_LACK) break;   // refill and keep going
                // Corrupt frame mid-stream (bit rot, tag remnants, bad rip): don't kill the
                // track — hop 2 bytes and let the frame sync realign. Bounded so a truly
                // broken file still ends instead of grinding forever.
                if (!eof && ++badf <= 128 && raw.len > 2) {
                    raw.buffer += 2;
                    raw.len -= 2;
                    progressed = true;
                    continue;
                }
                if (!eof) errored = true;
                break;
            }

            if (out.decoded_size > 0) {
                progressed = true;
                if (!begun) {
                    esp_audio_simple_dec_info_t info = {0};
                    if (esp_audio_simple_dec_get_info(dec, &info) == ESP_AUDIO_ERR_OK &&
                        info.sample_rate) {
                        rate = (int)info.sample_rate;
                        ch   = info.channel ? info.channel : 2;
                        bits = info.bits_per_sample ? info.bits_per_sample : 16;
                        if (!nv_audio_pcm_begin(rate, ch, bits)) { errored = stopped = true; break; }
                        begun = true;
                        s_trk_rate = rate; s_trk_ch = ch; s_trk_bits = bits;   // UI info
                        if (info.bitrate > 0 && audio_size > 0)   // tag bytes excluded (CBR seed;
                            s_dur_ms = (int)((uint64_t)audio_size * 8000ULL / info.bitrate);
                            // refined continuously below from real consumption (handles VBR)
                    }
                }
                if (begun) {
                    t0 = esp_timer_get_time();
                    if (nv_audio_pcm_write(out.buffer, out.decoded_size) < 0) { stopped = true; break; }
                    t_push += esp_timer_get_time() - t0;
                    out_5s += out.decoded_size;
                    out_bytes += out.decoded_size;
                    if (rate && ch && bits)
                        s_pos_ms = s_pos_base_ms +
                                   (int)(out_bytes * 1000ULL / ((uint64_t)rate * ch * (bits / 8)));
                }
            }

            if (raw.consumed > 0) {
                progressed = true;
                raw.buffer += raw.consumed;
                raw.len = (raw.consumed <= raw.len) ? raw.len - raw.consumed : 0;
                raw.consumed = 0;
            } else {
                break;   // decoder took nothing more from this buffer -> need a refill
            }
        }
        if (stopped) break;

        // Carry the unconsumed tail to the front of inbuf for the next read.
        fill = raw.len;
        if (fill > 0 && raw.buffer != inbuf) memmove(inbuf, raw.buffer, fill);

        // Refine the duration from REAL consumption (self-correcting, VBR-proof): after enough
        // audio, time-so-far scaled by total/consumed bytes converges on the true length.
        if (begun && s_pos_ms > 4000) {
            const long consumed = ftell(f) - audio_start - (long)fill;
            if (consumed > 96 * 1024) {
                const int est = (int)((int64_t)s_pos_ms * audio_size / consumed);
                if (est > 0) s_dur_ms = est;
            }
        }

        if (eof && (fill == 0 || !progressed)) break;   // EOT decided after the tail plays
    }

    NV_LOGI(TAG, "track end: %s pos=%dms/%dms consumed=%ld/%ld badframes=%d last_err=%d",
            errored ? "ERROR" : stopped ? "stopped" : "eot",
            s_pos_ms, s_dur_ms, ftell(f) - audio_start, audio_size, badf, (int)last_e);

    if (begun) {
        if (!stopped && !errored && tail_wait()) stopped = true;   // audible tail, commands live
        if (stopped) nv_audio_pcm_flush();     // user stop / track switch: cut NOW
        else if (!errored) s_eot = true;       // real (heard) end of track
        nv_audio_pcm_end();
    }
    esp_audio_simple_dec_close(dec);
    nv_sd_fclose(f);
    heap_caps_free(inbuf);
    heap_caps_free(outbuf);
    s_state = errored ? NV_MEDIA_ERROR : NV_MEDIA_STOPPED;
}

// ---------------------------------------------------------------- task + API

static void media_task(void *arg) {
    (void)arg;
    msg_t m;
    for (;;) {
        if (xQueueReceive(s_q, &m, portMAX_DELAY) != pdTRUE) continue;
        if (m.cmd == CMD_PLAY) {
            // MP3 -> minimp3; WAV -> native PCM reader; the rest -> esp_audio decoders.
            const esp_audio_simple_dec_type_t ty = type_for(m.path);
            if (ty == ESP_AUDIO_SIMPLE_DEC_TYPE_MP3)      play_file_mp3(m.path);
            else if (ty == ESP_AUDIO_SIMPLE_DEC_TYPE_WAV) play_file_wav(m.path);
            else                                          play_file(m.path);
        }
        // stray PAUSE/RESUME/STOP while idle are ignored
    }
}

void nv_media_init(void) {
    if (s_task) return;
    if (!s_registered) {
        // TWO layers must both be registered: esp_audio_dec (the actual MP3/AAC/FLAC codecs)
        // and esp_audio_simple_dec (the container/auto-detect wrapper). Registering only the
        // simple layer "succeeds" but every open then fails with AUDIO_DEC "not registered".
        const esp_audio_err_t core = esp_audio_dec_register_default();
        const esp_audio_err_t simp = esp_audio_simple_dec_register_default();
        if (core == ESP_AUDIO_ERR_OK && simp == ESP_AUDIO_ERR_OK) s_registered = true;
        else NV_LOGW(TAG, "decoder register failed (core=%d simple=%d)", (int)core, (int)simp);
    }
    s_q = xQueueCreate(4, sizeof(msg_t));
    if (!s_q) { NV_LOGE(TAG, "queue oom"); return; }
    // Pinned to core 1, above the UI/radio crowd on core 0: measured with wall-clock telemetry,
    // the decoder was preempted into producing 145 KB/s against a 188 KB/s (48 kHz stereo) target
    // ("pipe: dec=4975ms/5000ms") — the entire "audio lag". Priority 7 also outranks the feeder (6).
    // minimp3's ~21 KB decode scratch is patched to static .bss (see minimp3.h); WAV reads
    // natively. 12 KB covers the remaining esp_audio paths (AAC/FLAC), which proved stack-
    // hungrier than their formats suggest (WAV faulted even at 8 KB before going native).
    if (xTaskCreatePinnedToCore(media_task, "nvmedia", 12288, NULL, 7, &s_task, 1) != pdPASS) {
        NV_LOGE(TAG, "task create failed");
        s_task = NULL;
    }
}

// ---------------------------------------------------------------- header probe (durations)

// Read a track's duration from its headers only — no decode. MP3 (Xing/Info exact, else CBR
// estimate net of the ID3v2 tag), WAV (data/byterate), FLAC (STREAMINFO). 0 = unknown.
int nv_media_probe_dur_ms(const char *path) {
    FILE *f = nv_sd_fopen(path, "rb");
    if (!f) return 0;
    fseek(f, 0, SEEK_END);
    const long fsize = ftell(f);
    fseek(f, 0, SEEK_SET);

    uint8_t b[512];
    size_t n = fread(b, 1, sizeof b, f);
    int dur = 0;

    if (n >= 44 && !memcmp(b, "RIFF", 4) && !memcmp(b + 8, "WAVE", 4)) {
        // WAV: walk chunks for fmt (byte rate) + data (payload size).
        uint32_t byterate = 0, datasz = 0;
        size_t off = 12;
        while (off + 8 <= n) {
            const uint32_t csz = b[off+4] | (b[off+5] << 8) | (b[off+6] << 16) | ((uint32_t)b[off+7] << 24);
            if (!memcmp(b + off, "fmt ", 4) && off + 20 <= n)
                byterate = b[off+16] | (b[off+17] << 8) | (b[off+18] << 16) | ((uint32_t)b[off+19] << 24);
            if (!memcmp(b + off, "data", 4)) { datasz = csz; break; }
            if (csz > n) break;   // untrusted stride: reject wrap/overrun (non-advancing loop -> WDT)
            off += 8 + csz + (csz & 1);
        }
        if (byterate) dur = (int)((uint64_t)(datasz ? datasz : (uint32_t)fsize) * 1000ULL / byterate);
    } else if (n >= 42 && !memcmp(b, "fLaC", 4) && (b[4] & 0x7F) == 0) {
        // FLAC STREAMINFO: sample rate (20 bit) + total samples (36 bit) at fixed offsets.
        const uint8_t *si = b + 8;
        const uint32_t rate = ((uint32_t)si[10] << 12) | ((uint32_t)si[11] << 4) | (si[12] >> 4);
        const uint64_t total = (((uint64_t)si[13] & 0x0F) << 32) | ((uint64_t)si[14] << 24) |
                               ((uint64_t)si[15] << 16) | ((uint64_t)si[16] << 8) | si[17];
        if (rate && total) dur = (int)(total * 1000ULL / rate);
    } else if (n >= 10) {
        // MP3: skip ID3v2, then parse the first frame header.
        long audio_start = 0;
        if (b[0] == 'I' && b[1] == 'D' && b[2] == '3')
            audio_start = 10 + (((long)(b[6] & 0x7F) << 21) | ((long)(b[7] & 0x7F) << 14) |
                                ((long)(b[8] & 0x7F) << 7)  |  (long)(b[9] & 0x7F));
        if (audio_start > 0 && audio_start < fsize) { fseek(f, audio_start, SEEK_SET); n = fread(b, 1, sizeof b, f); }
        // find frame sync in the probe window
        size_t s = 0;
        while (s + 4 < n && !(b[s] == 0xFF && (b[s+1] & 0xE0) == 0xE0)) s++;
        if (s + 4 < n) {
            static const int kBr1[] = {0,32,40,48,56,64,80,96,112,128,160,192,224,256,320,0};
            static const int kBr2[] = {0,8,16,24,32,40,48,56,64,80,96,112,128,144,160,0};
            static const int kSr[]  = {44100, 48000, 32000, 0};
            const int ver = (b[s+1] >> 3) & 3;            // 3=MPEG1 2=MPEG2 0=MPEG2.5
            const int sri = (b[s+2] >> 2) & 3;
            const int bri = (b[s+2] >> 4) & 15;
            const bool mpeg1 = (ver == 3);
            int rate = kSr[sri];
            if (rate) { if (ver == 2) rate /= 2; else if (ver == 0) rate /= 4; }
            const int kbps = mpeg1 ? kBr1[bri] : kBr2[bri];
            const int spf  = mpeg1 ? 1152 : 576;
            const bool mono = ((b[s+3] >> 6) & 3) == 3;
            // Xing/Info (VBR): exact frame count lives inside the first frame.
            const size_t xo = s + 4 + (mpeg1 ? (mono ? 17 : 32) : (mono ? 9 : 17));
            if (rate && xo + 12 < n && (!memcmp(b + xo, "Xing", 4) || !memcmp(b + xo, "Info", 4)) && (b[xo+7] & 1)) {
                const uint32_t frames = ((uint32_t)b[xo+8] << 24) | ((uint32_t)b[xo+9] << 16) |
                                        ((uint32_t)b[xo+10] << 8) | b[xo+11];
                dur = (int)((uint64_t)frames * spf * 1000ULL / rate);
            } else if (kbps) {
                dur = (int)((uint64_t)(fsize - audio_start) * 8ULL / kbps);   // CBR: bytes/kbps = ms
            }
        }
    }
    nv_sd_fclose(f);
    return dur > 0 ? dur : 0;
}

bool nv_media_seekable(void) { return s_seekable; }

void nv_media_seek(int pct_x10) {
    if (pct_x10 < 0) pct_x10 = 0; else if (pct_x10 > 1000) pct_x10 = 1000;
    s_seek_req = pct_x10;
}

void nv_media_track_info(int *rate, int *ch, int *bits) {
    if (rate) *rate = s_trk_rate;
    if (ch)   *ch   = s_trk_ch;
    if (bits) *bits = s_trk_bits;
}

bool nv_media_play(const char *path) {
    if (!s_q || !path || !path[0]) return false;
    msg_t m = { .cmd = CMD_PLAY };
    strncpy(m.path, path, sizeof m.path - 1);
    m.path[sizeof m.path - 1] = '\0';
    s_eot = false;
    s_state = NV_MEDIA_PLAYING;   // optimistic; the task confirms/rolls back
    return xQueueSend(s_q, &m, pdMS_TO_TICKS(100)) == pdTRUE;
}

void nv_media_pause(bool on) {
    if (!s_q) return;
    msg_t m = { .cmd = on ? CMD_PAUSE : CMD_RESUME };
    xQueueSend(s_q, &m, 0);
}

void nv_media_stop(void) {
    if (!s_q) return;
    msg_t m = { .cmd = CMD_STOP };
    s_state = NV_MEDIA_STOPPED;   // optimistic
    xQueueSend(s_q, &m, 0);
}

nv_media_state_t nv_media_state(void) { return s_state; }
int nv_media_pos_ms(void) {
    // s_pos_ms tracks the DECODE head, which runs up to the playout ring ahead of the ear.
    // Subtract the queued backlog so the UI playhead is the AUDIBLE position.
    int pos = s_pos_ms;
    const int rate = s_trk_rate, ch = s_trk_ch, bits = s_trk_bits ? s_trk_bits : 16;
    if (rate && ch) {
        const uint32_t bps = (uint32_t)rate * ch * (bits / 8);
        if (bps) pos -= (int)((uint64_t)nv_audio_pcm_backlog() * 1000ULL / bps);
    }
    return pos > 0 ? pos : 0;
}
int  nv_media_dur_ms(void) { return s_dur_ms; }

bool nv_media_took_eot(void) {
    if (!s_eot) return false;
    s_eot = false;
    return true;
}

bool nv_media_is_audio(const char *path) {
    return type_for(path) != ESP_AUDIO_SIMPLE_DEC_TYPE_NONE;
}
