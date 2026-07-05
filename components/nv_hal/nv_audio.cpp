// nv_audio — ES8311 DAC output over I2S. See nv_audio.h. Uses the shared I2C bus for codec
// control and a worker task so UI callers never block on the I2S write.
#include "nv_audio.h"
#include "nv_usb_audio.h"   // hot-pluggable UAC sink: USB speaker wins over the ES8311 when present
#include "nv_hal.h"      // nv_hal_i2c_bus()
#include "nv_pins.h"
#include "nv_config.h"
#include "nv_log.h"

#include "driver/i2s_std.h"
#include "driver/i2c_master.h"   // i2c_master_probe (find the ES7210 address / skip cleanly)
#include "esp_attr.h"    // EXT_RAM_BSS_ATTR (cold statics -> PSRAM, hot SRAM stays free)
#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"
#include "es8311_codec.h"
#include "es7210_adc.h"
#include "esp_heap_caps.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"

#include <cmath>
#include <cstring>
#include <cstdio>   // WAV file writing (voice recorder)

static const char *TAG = "audio";

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr int kRate = 48000;                 // Hz
constexpr int kMaxMs = 300;
constexpr int kMaxSamples = kRate / 1000 * kMaxMs;   // 14400 -> 28.8KB scratch

// Tone scratch: sequential write (sine gen) + sequential read (codec write) — PSRAM-friendly.
// EXT_RAM_BSS_ATTR hands its 28.8 KB back to internal SRAM, where the MP3 decoder state lives.
EXT_RAM_BSS_ATTR int16_t s_scratch[kMaxSamples];

esp_codec_dev_handle_t s_spk = nullptr;
esp_codec_dev_handle_t s_mic = nullptr;
i2s_chan_handle_t s_tx = nullptr;
i2s_chan_handle_t s_rx = nullptr;
bool s_ready = false;
int  s_vol   = 60;
bool s_mute  = false;
bool s_keyclick = true;   // "keyclick" pref: gate the IME tick at the source

QueueHandle_t s_q = nullptr;
struct Snd { int freq; int ms; };

// PCM streaming (music/video). The single DAC is shared with the tone path, so a mutex serializes
// every codec open/close/write and `s_streaming` makes the tone task stand down while a stream runs.
SemaphoreHandle_t s_spk_lock = nullptr;
SemaphoreHandle_t s_pcm_lock = nullptr;   // stream-ownership: one PCM stream at a time (sfx vs voice vs music)
volatile bool     s_streaming = false;
volatile int      s_pcm_owner = 0;        // nv_pcm_owner_t of the live stream (music/sfx/voice)
volatile int      s_voice_prio = 0;       // >0: voice owns the channel — cancel SFX, mute tones
volatile bool     s_sfx_cancel = false;   // set while voice preempts: a live SFX stream must bail
bool s_route_usb = false;   // current output route (chosen at each out_open)
int  s_out_ch    = 1;       // channel width of the active stream (for the USB mono-dup)

// ---- PSRAM playout ring -------------------------------------------------------------------
// Decouples the decoder from the sink: pcm_write copies into a ~3 s PSRAM ring and returns;
// a dedicated feeder task drains it into the USB speaker / ES8311 at playback pace. SD read
// bursts, decoder warm-up or UI storms can no longer underrun the output ("lag al primo
// avvio"). Single-producer (nvmedia) / single-consumer (feeder) — indices only, no lock.
constexpr size_t kRingCap  = 2 * 1024 * 1024;   // ~11 s @ 48 kHz stereo — PSRAM is abundant
constexpr size_t kFeedChunk = 4608;              // ~24 ms of 48 kHz stereo per sink write
uint8_t *s_ring = nullptr;                        // PSRAM, allocated once at init
volatile size_t s_rd = 0, s_wr = 0;               // ring indices (bytes)
volatile bool   s_draining = false;               // pcm_end: play out what's left, then stop
volatile bool   s_feed_busy = false;              // feeder is inside a sink write
volatile bool   s_pcm_paused = false;             // freeze the FEEDER (the ring would otherwise
                                                  // keep playing ~11 s of buffered tail on pause)
// Pre-roll: at stream start the ring is empty and the feeder would drain it before the decoder has
// pushed anything — the "lag al primo avvio" the ring was meant to cure still bit MPEG-1 (measured
// underruns=162 in the first 5 s window). Gate the feeder: don't drain (and don't count underruns)
// until the ring first reaches s_prebuf_target bytes. Set per-stream in pcm_begin_as, cleared once
// reached. 0 = no pre-roll (already primed / short SFX where the ~50 ms would be perceptible).
volatile size_t s_prebuf_target = 0;
// Ring flush handshake. A mid-stream flush/seek/voice-preempt must NOT poke s_rd/s_wr from the
// caller thread: that races the feeder's pop (owns s_rd) and the decoder's push (owns s_wr), and a
// reset lost to a concurrent index write-back replays up to ~11 s of stale ring (previous track /
// word bleeds past the flush). Instead the caller bumps s_flush_req; the feeder — the sole writer
// of s_rd — drops the ring on its next turn and acks via s_flush_done.
volatile uint32_t s_flush_req  = 0;               // ++ by foreign flush callers (atomic RMW)
uint32_t          s_flush_done = 0;               // feeder ack; begin swallows stale reqs

size_t ring_avail(void) { size_t w = s_wr, r = s_rd; return w >= r ? w - r : kRingCap - r + w; }
size_t ring_free(void)  { return kRingCap - 1 - ring_avail(); }

// Direct index reset — ONLY safe at a stream boundary (begin/end), where s_streaming is off so
// neither the feeder nor the decoder is touching the indices. Never call this mid-stream; use the
// s_flush_req handshake (nv_audio_pcm_flush) instead.
void ring_reset(void) { s_rd = s_wr = 0; }

void ring_push(const void *src, size_t n) {       // caller guaranteed n <= ring_free()
    const uint8_t *p = (const uint8_t *)src;
    size_t w = s_wr;
    const size_t first = kRingCap - w < n ? kRingCap - w : n;
    memcpy(s_ring + w, p, first);
    if (n > first) memcpy(s_ring, p + first, n - first);
    s_wr = (w + n) % kRingCap;
}

size_t ring_pop(void *dst, size_t max) {
    size_t n = ring_avail();
    if (n > max) n = max;
    if (!n) return 0;
    uint8_t *p = (uint8_t *)dst;
    size_t r = s_rd;
    const size_t first = kRingCap - r < n ? kRingCap - r : n;
    memcpy(p, s_ring + r, first);
    if (n > first) memcpy(p + first, s_ring, n - first);
    s_rd = (r + n) % kRingCap;
    return n;
}

// (Re)open the output codec at a given format and re-apply the live volume. Lock held by caller.
bool spk_open(int rate, int ch, int bits) {
    if (!s_spk) return false;
    esp_codec_dev_close(s_spk);
    esp_codec_dev_sample_info_t fs = {};
    fs.bits_per_sample = (uint8_t)bits;
    fs.channel = (uint8_t)ch;
    fs.sample_rate = (uint32_t)rate;
    if (esp_codec_dev_open(s_spk, &fs) != ESP_OK) return false;
    esp_codec_dev_set_out_vol(s_spk, s_mute ? 0 : s_vol);
    return true;
}

// Route-aware open/write: a connected USB speaker takes the stream when it can run this format
// natively; anything else stays on the ES8311. Lock held by caller.
bool out_open(int rate, int ch, int bits) {
    s_out_ch = ch;
    const bool usb_there = nv_usb_audio_present();
    if (usb_there && nv_usb_audio_open(rate, ch, bits)) {
        s_route_usb = true;
        NV_LOGI(TAG, "out_open %d/%dch/%db -> USB", rate, ch, bits);
        return true;
    }
    s_route_usb = false;
    NV_LOGI(TAG, "out_open %d/%dch/%db -> ES8311 (usb %s)", rate, ch, bits,
            usb_there ? "REFUSED format" : "absent");
    return spk_open(rate, ch, bits);
}

int out_write(const void *pcm, size_t bytes) {
    if (s_route_usb) {
        if (nv_usb_audio_write(pcm, bytes, s_out_ch) >= 0) return (int)bytes;
        // A lone failed chunk must NOT dump the rest of the stream onto the (usually speaker-less)
        // ES8311 — that turned one transient USB blip into "the app has no audio". Only abandon the
        // route when the sink has actually gone unhealthy/disconnected; otherwise drop this ~24 ms
        // chunk (inaudible) and keep trying USB on the next one.
        if (nv_usb_audio_present()) return (int)bytes;
        s_route_usb = false;   // sink truly lost: finish the stream on the DAC
        NV_LOGW(TAG, "USB sink lost mid-stream -> ES8311 fallback");
    }
    if (!s_spk) return -1;
    return esp_codec_dev_write(s_spk, (void *)pcm, bytes) == ESP_OK ? (int)bytes : -1;
}

// Feeder: the ONLY sink writer while a stream runs. Pulls ~24 ms chunks from the PSRAM ring
// and lets the (blocking) sink write pace playback. Idles on a short poll when stopped.
void feeder_task(void *) {
    // chunk staging in PSRAM too — this task's writes copy into driver buffers anyway
    uint8_t *chunk = (uint8_t *)heap_caps_malloc(kFeedChunk, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!chunk) { NV_LOGE(TAG, "feeder oom"); vTaskDelete(nullptr); }
    // Streaming health telemetry: underruns (ring empty mid-stream = audible gap), the lowest
    // ring fill seen, and the slowest sink write. One line every ~5 s while streaming.
    uint32_t underruns = 0, worst_wr_ms = 0;
    size_t   min_fill = SIZE_MAX;
    int64_t  next_stat_us = 0;
    for (;;) {
        if (!s_streaming) { min_fill = SIZE_MAX; underruns = 0; worst_wr_ms = 0; next_stat_us = 0;
                            vTaskDelay(pdMS_TO_TICKS(10)); continue; }
        // Foreign-thread flush/seek/voice-preempt lands here: this task owns s_rd, so drop the ring
        // ourselves (s_rd = s_wr) rather than let the caller race the indices. Checked before the
        // pause gate so a seek while paused still cuts. s_wr may advance under us — that post-flush
        // data legitimately survives; s_rd only ever moves forward, never back into stale content.
        {
            const uint32_t freq = __atomic_load_n(&s_flush_req, __ATOMIC_ACQUIRE);
            if (freq != s_flush_done) { s_rd = s_wr; __atomic_store_n(&s_flush_done, freq, __ATOMIC_RELEASE); }
        }
        if (s_pcm_paused) { vTaskDelay(pdMS_TO_TICKS(20)); continue; }   // hold the ring intact
        // Pre-roll gate: wait for the ring to fill to the target before the first drain, so the sink
        // never starts on an empty buffer. Not an underrun — the stream just hasn't warmed up yet.
        // s_draining (pcm_end) bypasses it so a stream that ends before priming still flushes.
        if (s_prebuf_target) {
            if (!s_draining && ring_avail() < s_prebuf_target) { vTaskDelay(pdMS_TO_TICKS(5)); continue; }
            s_prebuf_target = 0;   // primed — drain normally from here on
        }
        const size_t avail = ring_avail();
        if (avail < min_fill) min_fill = avail;
        const size_t n = ring_pop(chunk, kFeedChunk);
        if (!n) {
            if (!s_draining) underruns++;
            vTaskDelay(pdMS_TO_TICKS(s_draining ? 2 : 5));
            continue;
        }
        s_feed_busy = true;
        const int64_t t0 = esp_timer_get_time();
        if (s_spk_lock) xSemaphoreTake(s_spk_lock, portMAX_DELAY);
        if (s_streaming) out_write(chunk, n);
        if (s_spk_lock) xSemaphoreGive(s_spk_lock);
        const uint32_t wr_ms = (uint32_t)((esp_timer_get_time() - t0) / 1000);
        if (wr_ms > worst_wr_ms) worst_wr_ms = wr_ms;
        s_feed_busy = false;
        if (!next_stat_us) next_stat_us = esp_timer_get_time() + 5000000;
        else if (esp_timer_get_time() >= next_stat_us) {
            // Quiet sentinel: one line ONLY when something is off (a gap or a >100 ms sink
            // stall). Healthy playback logs nothing — the counters still run every window.
            if (underruns || worst_wr_ms > 100)
                NV_LOGW(TAG, "stream: underruns=%u min_fill=%uKB worst_write=%ums route=%s",
                        (unsigned)underruns, (unsigned)(min_fill / 1024), (unsigned)worst_wr_ms,
                        s_route_usb ? "USB" : "ES8311");
            underruns = 0; worst_wr_ms = 0; min_fill = SIZE_MAX;
            next_stat_us = esp_timer_get_time() + 5000000;
        }
    }
}

// Generate a soft sine burst with a short attack/release so it doesn't click. Returns samples.
int gen_tone(int freq, int ms) {
    if (ms > kMaxMs) ms = kMaxMs;
    if (ms < 5) ms = 5;
    int n = kRate / 1000 * ms;
    if (n > kMaxSamples) n = kMaxSamples;
    const double amp  = 0.22 * 32767.0;      // moderate level; keep clear of clipping
    const double w    = 2.0 * kPi * (double)freq / kRate;
    const int    ramp = kRate / 1000 * 3;    // 3 ms fade in/out
    for (int i = 0; i < n; i++) {
        double env = 1.0;
        if (i < ramp)          env = (double)i / ramp;
        else if (i > n - ramp) env = (double)(n - i) / ramp;
        s_scratch[i] = (int16_t)(sin(w * i) * amp * env);
    }
    return n;
}

void audio_task(void *) {
    Snd s;
    for (;;) {
        if (xQueueReceive(s_q, &s, portMAX_DELAY) != pdTRUE) continue;
        if (!s_ready || s_mute || !s_spk) continue;
        if (s_voice_prio) continue;  // voice owns the channel: drop the beep entirely (no overlap)
        const int n = gen_tone(s.freq, s.ms);
        if (s_spk_lock) xSemaphoreTake(s_spk_lock, portMAX_DELAY);
        if (!s_streaming && !s_voice_prio) {   // stand down while a PCM stream / voice owns the output
            // Tones are 48 kHz mono; the USB sink duplicates to stereo itself. If the USB stream
            // idles at another rate the click shifts pitch imperceptibly — not worth a restart.
            if (!(nv_usb_audio_present() && nv_usb_audio_write(s_scratch, n * sizeof(int16_t), 1) >= 0) && s_spk)
                esp_codec_dev_write(s_spk, s_scratch, n * (int)sizeof(int16_t));
        }
        if (s_spk_lock) xSemaphoreGive(s_spk_lock);
    }
}

bool i2s_setup(void) {
    i2s_chan_config_t chan = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chan.auto_clear = true;
    if (i2s_new_channel(&chan, &s_tx, &s_rx) != ESP_OK) return false;   // full duplex (DAC + ADC)

    i2s_std_config_t std = {};
    std.clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(kRate);
    std.slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO);
    std.gpio_cfg.mclk = (gpio_num_t)NV_PIN_I2S_MCLK;
    std.gpio_cfg.bclk = (gpio_num_t)NV_PIN_I2S_BCLK;
    std.gpio_cfg.ws   = (gpio_num_t)NV_PIN_I2S_WS;
    std.gpio_cfg.dout = (gpio_num_t)NV_PIN_I2S_DOUT;
    std.gpio_cfg.din  = (gpio_num_t)NV_PIN_I2S_DIN;    // ES7210 capture (on-board MIC1)
    if (i2s_channel_init_std_mode(s_tx, &std) != ESP_OK) return false;
    if (i2s_channel_enable(s_tx) != ESP_OK) return false;
    if (i2s_channel_init_std_mode(s_rx, &std) != ESP_OK) { s_rx = nullptr; return true; }  // out-only
    if (i2s_channel_enable(s_rx) != ESP_OK) { s_rx = nullptr; }
    return true;
}

// ---------------------------------------------------------------- microphone (ES7210)
// Worker-task state machine: IDLE -> METER (until stopped) / REC -> PLAY -> back. The task is
// created lazily on first use and then sleeps on its command queue. Level is a rolling RMS
// mapped to 0..100; UI polls it from an LVGL timer.

constexpr int kMicChunkMs   = 32;
constexpr int kMicChunk     = kRate / 1000 * kMicChunkMs;   // samples per meter chunk
constexpr int kMicTestMaxMs = 5000;

enum class MicCmd { MeterStart, MeterStop, Test, RecStart, RecStop };

// Voice recorder (mic -> WAV on SD). The path is staged before RecStart is queued.
char              s_rec_path[256] = {0};
volatile bool     s_rec_run   = false;
volatile uint32_t s_rec_start = 0;

// Minimal little-endian WAV (PCM) writer — 44-byte header, sizes back-patched on stop.
void wav_wr16(FILE *f, uint16_t v) { uint8_t b[2] = {(uint8_t)v,(uint8_t)(v>>8)}; fwrite(b,1,2,f); }
void wav_wr32(FILE *f, uint32_t v) { uint8_t b[4] = {(uint8_t)v,(uint8_t)(v>>8),(uint8_t)(v>>16),(uint8_t)(v>>24)}; fwrite(b,1,4,f); }
void wav_header(FILE *f, uint32_t rate, uint16_t ch, uint16_t bits) {
    uint32_t br = rate*ch*bits/8; uint16_t ba = ch*bits/8;
    fwrite("RIFF",1,4,f); wav_wr32(f,0); fwrite("WAVE",1,4,f);
    fwrite("fmt ",1,4,f); wav_wr32(f,16); wav_wr16(f,1); wav_wr16(f,ch);
    wav_wr32(f,rate); wav_wr32(f,br); wav_wr16(f,ba); wav_wr16(f,bits);
    fwrite("data",1,4,f); wav_wr32(f,0);
}
void wav_patch(FILE *f, uint32_t data_bytes) {
    fseek(f,4,SEEK_SET);  wav_wr32(f,36+data_bytes);
    fseek(f,40,SEEK_SET); wav_wr32(f,data_bytes);
}

QueueHandle_t         s_mic_q     = nullptr;
TaskHandle_t          s_mic_task  = nullptr;
volatile nv_mic_state_t s_mic_state = NV_MIC_IDLE;
volatile int          s_mic_level = 0;
int                   s_mic_test_ms = 3000;
int16_t              *s_mic_buf   = nullptr;   // capture scratch + test recording (PSRAM)

int rms_to_level(const int16_t *p, int n) {
    if (n <= 0) return 0;
    uint64_t acc = 0;
    for (int i = 0; i < n; i++) acc += (int32_t)p[i] * p[i];
    const double rms = sqrt((double)(acc / (uint64_t)n));
    // ~ -54 dBFS floor .. 0 dBFS full: usable range for a visual meter
    double db = 20.0 * log10(rms / 32768.0 + 1e-9);
    double pct = (db + 54.0) * (100.0 / 54.0);
    if (pct < 0) pct = 0; else if (pct > 100) pct = 100;
    return (int)pct;
}

void mic_task(void *) {
    MicCmd cmd;
    for (;;) {
        if (xQueueReceive(s_mic_q, &cmd, portMAX_DELAY) != pdTRUE) continue;

        if (cmd == MicCmd::RecStart) {
            FILE *f = fopen(s_rec_path, "wb");
            if (f) {
                wav_header(f, kRate, 1, 16);
                s_mic_state = NV_MIC_REC;
                s_rec_run = true;
                uint32_t nsamp = 0;
                while (s_rec_run) {
                    MicCmd nx;   // stop is delivered on the same queue
                    while (xQueueReceive(s_mic_q, &nx, 0) == pdTRUE) {
                        if (nx == MicCmd::RecStop) { s_rec_run = false; break; }
                    }
                    if (!s_rec_run) break;
                    if (esp_codec_dev_read(s_mic, s_mic_buf, kMicChunk * sizeof(int16_t)) == ESP_OK) {
                        fwrite(s_mic_buf, sizeof(int16_t), kMicChunk, f);
                        nsamp += kMicChunk;
                        s_mic_level = rms_to_level(s_mic_buf, kMicChunk);
                    }
                }
                wav_patch(f, nsamp * (uint32_t)sizeof(int16_t));
                fclose(f);
            }
            s_rec_run = false;
            s_mic_level = 0;
            s_mic_state = NV_MIC_IDLE;
            continue;
        }

        if (cmd == MicCmd::MeterStart) {
            s_mic_state = NV_MIC_METER;
            while (s_mic_state == NV_MIC_METER) {
                // drain queued commands: Stop/Test break the loop; a stale duplicate
                // MeterStart (UI raced the state flip) is ignored, NOT treated as a stop
                MicCmd next;
                while (xQueueReceive(s_mic_q, &next, 0) == pdTRUE) {
                    if (next == MicCmd::Test)      { cmd = next; s_mic_state = NV_MIC_IDLE; break; }
                    if (next == MicCmd::MeterStop) { s_mic_state = NV_MIC_IDLE; break; }
                }
                if (s_mic_state != NV_MIC_METER) break;
                if (esp_codec_dev_read(s_mic, s_mic_buf, kMicChunk * sizeof(int16_t)) == ESP_OK)
                    s_mic_level = rms_to_level(s_mic_buf, kMicChunk);
            }
            s_mic_level = 0;
            if (cmd != MicCmd::Test) continue;
        }

        if (cmd == MicCmd::Test) {
            const int total = kRate / 1000 * s_mic_test_ms;
            s_mic_state = NV_MIC_REC;
            int got = 0;
            while (got < total) {
                int take = total - got < kMicChunk ? total - got : kMicChunk;
                if (esp_codec_dev_read(s_mic, s_mic_buf + got, take * sizeof(int16_t)) != ESP_OK)
                    break;
                s_mic_level = rms_to_level(s_mic_buf + got, take);
                got += take;
            }
            s_mic_level = 0;
            if (got > 0 && !s_mute) {
                s_mic_state = NV_MIC_PLAY;
                if (!(nv_usb_audio_present() && nv_usb_audio_write(s_mic_buf, got * sizeof(int16_t), 1) >= 0) && s_spk)
                    esp_codec_dev_write(s_spk, s_mic_buf, got * (int)sizeof(int16_t));
            }
            s_mic_state = NV_MIC_IDLE;
        }
    }
}

bool mic_worker_up(void) {
    if (s_mic_task) return true;
    if (!s_mic) return false;
    const int test_samples = kRate / 1000 * kMicTestMaxMs;
    s_mic_buf = (int16_t *)heap_caps_malloc(test_samples * sizeof(int16_t),
                                            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_mic_buf) { NV_LOGE(TAG, "mic buffer oom"); return false; }
    s_mic_q = xQueueCreate(4, sizeof(MicCmd));
    if (!s_mic_q ||
        xTaskCreate(mic_task, "nvmic", 4096, nullptr, 5, &s_mic_task) != pdPASS) {
        NV_LOGE(TAG, "mic task failed");
        s_mic_task = nullptr;
        return false;
    }
    return true;
}

void mic_setup(i2c_master_bus_handle_t bus, const audio_codec_data_if_t *data_if) {
    if (!s_rx) { NV_LOGW(TAG, "no I2S RX channel; mic disabled"); return; }

    // Probe the ES7210 before handing it to the codec driver: its AD0/AD1 pins pick one of
    // 0x40..0x43, and if the chip doesn't ACK at all the driver would otherwise spam repeated
    // "I2C write fail" / "Open fail" errors. Probing lets us pick the real address or skip quietly.
    uint8_t addr = 0;
    for (uint8_t a = 0x40; a <= 0x43; a++) {
        if (i2c_master_probe(bus, a, 20) == ESP_OK) { addr = a; break; }
    }
    if (!addr) { NV_LOGW(TAG, "no ES7210 on I2C 0x40-0x43; mic unavailable"); return; }

    audio_codec_i2c_cfg_t i2c_if = {};
    i2c_if.port = I2C_NUM_0;
    // The codec ctrl driver stores the 8-bit WRITE address and derives the 7-bit device address as
    // (addr >> 1). i2c_master_probe() gave us the 7-bit value, so shift it left: 0x40 -> 0x80
    // (== ES7210_CODEC_DEFAULT_ADDR). Passing the raw 7-bit form would register the device at 0x20.
    i2c_if.addr = (uint8_t)(addr << 1);
    i2c_if.bus_handle = bus;
    const audio_codec_ctrl_if_t *ctrl_if = audio_codec_new_i2c_ctrl(&i2c_if);
    if (!ctrl_if) { NV_LOGW(TAG, "es7210 ctrl if failed"); return; }

    es7210_codec_cfg_t es = {};
    es.ctrl_if      = ctrl_if;
    es.master_mode  = false;              // P4 is I2S master
    es.mic_selected = ES7210_SEL_MIC1;    // the on-board microphone
    const audio_codec_if_t *codec_if = es7210_codec_new(&es);
    if (!codec_if) { NV_LOGW(TAG, "es7210 init failed (mic unavailable)"); return; }

    esp_codec_dev_cfg_t dev = {};
    dev.dev_type = ESP_CODEC_DEV_TYPE_IN;
    dev.codec_if = codec_if;
    dev.data_if  = data_if;
    s_mic = esp_codec_dev_new(&dev);
    if (!s_mic) { NV_LOGW(TAG, "mic codec_dev new failed"); return; }

    esp_codec_dev_sample_info_t fs = {};
    fs.bits_per_sample = 16;
    fs.channel = 1;
    fs.sample_rate = kRate;
    if (esp_codec_dev_open(s_mic, &fs) != ESP_OK) {
        NV_LOGW(TAG, "mic open failed");
        s_mic = nullptr;
        return;
    }
    esp_codec_dev_set_in_gain(s_mic, 30.0);   // sensible on-board mic default
    NV_LOGI(TAG, "mic ready (ES7210, MIC1)");
}

}  // namespace

void nv_audio_init(void) {
    if (s_ready) return;
    i2c_master_bus_handle_t bus = nv_hal_i2c_bus();
    if (!bus) { NV_LOGW(TAG, "no I2C bus; audio disabled"); return; }
    if (!i2s_setup()) { NV_LOGE(TAG, "I2S init failed"); return; }

    audio_codec_i2s_cfg_t i2s_if = {};
    i2s_if.port = I2S_NUM_0;
    i2s_if.tx_handle = s_tx;
    i2s_if.rx_handle = s_rx;   // duplex: same port feeds the ES7210 capture device
    const audio_codec_data_if_t *data_if = audio_codec_new_i2s_data(&i2s_if);

    audio_codec_i2c_cfg_t i2c_if = {};
    i2c_if.port = I2C_NUM_0;
    i2c_if.addr = ES8311_CODEC_DEFAULT_ADDR;
    i2c_if.bus_handle = bus;                       // share the GT911/RTC bus (new i2c_master)
    const audio_codec_ctrl_if_t *ctrl_if = audio_codec_new_i2c_ctrl(&i2c_if);
    const audio_codec_gpio_if_t *gpio_if = audio_codec_new_gpio();
    if (!data_if || !ctrl_if || !gpio_if) { NV_LOGE(TAG, "codec interfaces failed"); return; }

    esp_codec_dev_hw_gain_t gain = {};
    gain.pa_voltage = 5.0;
    gain.codec_dac_voltage = 3.3;
    es8311_codec_cfg_t es = {};
    es.ctrl_if     = ctrl_if;
    es.gpio_if     = gpio_if;
    es.codec_mode  = ESP_CODEC_DEV_WORK_MODE_BOTH; // DAC (speaker) + ADC (on-board mic); no ES7210
    es.pa_pin      = NV_PIN_AUDIO_PA_EN;           // GPIO11 amplifier enable
    es.pa_reverted = false;
    es.master_mode = false;                        // P4 is I2S master, codec is slave
    es.use_mclk    = true;                         // MCLK (GPIO13) is wired
    es.hw_gain     = gain;
    const audio_codec_if_t *codec_if = es8311_codec_new(&es);
    if (!codec_if) { NV_LOGE(TAG, "es8311 init failed"); return; }

    esp_codec_dev_cfg_t dev = {};
    dev.dev_type = ESP_CODEC_DEV_TYPE_OUT;
    dev.codec_if = codec_if;
    dev.data_if  = data_if;
    s_spk = esp_codec_dev_new(&dev);
    if (!s_spk) { NV_LOGE(TAG, "codec_dev new failed"); return; }

    esp_codec_dev_sample_info_t fs = {};
    fs.bits_per_sample = 16;
    fs.channel = 1;
    fs.sample_rate = kRate;
    if (esp_codec_dev_open(s_spk, &fs) != ESP_OK) { NV_LOGE(TAG, "codec open failed"); return; }

    s_vol  = nv_config_get_int("volume", 60);
    s_mute = nv_config_get_bool("mute", false);
    s_keyclick = nv_config_get_bool("keyclick", true);
    esp_codec_dev_set_out_vol(s_spk, s_mute ? 0 : s_vol);

    s_spk_lock = xSemaphoreCreateMutex();
    s_pcm_lock = xSemaphoreCreateMutex();
    s_q = xQueueCreate(6, sizeof(Snd));
    if (!s_q || xTaskCreateWithCaps(audio_task, "nvaudio", 4096, nullptr, 5, nullptr,
                                    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {   // stack in PSRAM (internal SRAM is scarce)
        NV_LOGE(TAG, "audio task failed");
        return;
    }
    // PSRAM playout ring + feeder (see above). Priority above the decoder so the sink never
    // starves while nvmedia is busy on SD/decode.
    s_ring = (uint8_t *)heap_caps_malloc(kRingCap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_ring || xTaskCreateWithCaps(feeder_task, "nvfeed", 6144, nullptr, 6, nullptr,
                                       MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS)
        NV_LOGE(TAG, "playout ring init failed");
    // Capture path: the on-board mic feeds the ES8311's OWN ADC — an I2C scan shows only ES8311
    // @0x18 (NO ES7210 @0x40-0x43 on this board). Create a second codec_dev (IN) on the same chip.
    {
        esp_codec_dev_cfg_t midev = {};
        midev.dev_type = ESP_CODEC_DEV_TYPE_IN;
        midev.codec_if = codec_if;   // same ES8311 (opened in BOTH mode)
        midev.data_if  = data_if;    // same duplex I2S (DIN48 = ES8311 ADC out)
        s_mic = esp_codec_dev_new(&midev);
        if (s_mic) {
            esp_codec_dev_sample_info_t mfs = {};
            mfs.bits_per_sample = 16;
            mfs.channel = 1;
            mfs.sample_rate = kRate;
            if (esp_codec_dev_open(s_mic, &mfs) == ESP_OK) {
                esp_codec_dev_set_in_gain(s_mic, 30.0);
                NV_LOGI(TAG, "mic ready (ES8311 ADC, MIC1)");
            } else {
                NV_LOGW(TAG, "mic open failed"); s_mic = nullptr;
            }
        }
    }
    s_ready = true;
    NV_LOGI(TAG, "audio ready (ES8311, vol %d%s%s)", s_vol, s_mute ? ", muted" : "",
            s_mic ? ", mic" : "");
}

bool nv_audio_ready(void) { return s_ready; }

void nv_audio_set_volume(int percent) {
    if (percent < 0) percent = 0; else if (percent > 100) percent = 100;
    s_vol = percent;
    if (s_spk) esp_codec_dev_set_out_vol(s_spk, s_mute ? 0 : s_vol);
    nv_usb_audio_set_volume(s_vol);
}
void nv_audio_set_mute(bool on) {
    s_mute = on;
    if (s_spk) esp_codec_dev_set_out_vol(s_spk, s_mute ? 0 : s_vol);
    nv_usb_audio_set_mute(on);
}

void nv_audio_tone(int freq_hz, int ms) {
    if (!s_ready || s_mute || !s_q) return;
    Snd s = { freq_hz, ms };
    xQueueSend(s_q, &s, 0);   // drop if the queue is full (never block the caller)
}

void nv_audio_set_key_click(bool on) { s_keyclick = on; }

// ---- PCM streaming (contract in the header) -----------------------------------------------------

bool nv_audio_pcm_begin_as(int sample_rate, int channels, int bits, nv_pcm_owner_t owner) {
    if (!s_ready || !s_spk || !s_spk_lock) return false;
    if (channels < 1) channels = 1; else if (channels > 2) channels = 2;
    if (bits != 16 && bits != 32) bits = 16;
    if (sample_rate < 8000) sample_rate = 8000; else if (sample_rate > 96000) sample_rate = 96000;
    // Own the stream exclusively: a second begin (voice while a jingle streams, etc.) BLOCKS here
    // until the first pcm_end — so PCM streams play in sequence instead of clobbering each other's
    // ring/format. Voice raises priority first (nv_audio_voice_priority), which cancels a live SFX
    // stream so this begin doesn't wait out a whole jingle. Released in pcm_end.
    if (s_pcm_lock) xSemaphoreTake(s_pcm_lock, portMAX_DELAY);
    xSemaphoreTake(s_spk_lock, portMAX_DELAY);
    const bool ok = out_open(sample_rate, channels, bits);
    ring_reset();
    // Swallow any flush requested before this stream so the feeder doesn't nuke the fresh ring
    // (s_flush_req is monotonic and shared across streams).
    __atomic_store_n(&s_flush_done, __atomic_load_n(&s_flush_req, __ATOMIC_ACQUIRE), __ATOMIC_RELEASE);
    s_draining = false;
    s_pcm_paused = false;
    s_pcm_owner = owner;
    // Pre-roll ONLY for long streams (music/video). SFX (nv_sound, ~300 ms) and voice are short —
    // a 0.2 s gate there would be audible latency; they start immediately (target 0).
    s_prebuf_target = (owner == NV_PCM_MUSIC)
                        ? (size_t)sample_rate * channels * (bits / 8) / 5   // ~0.2 s
                        : 0;
    s_streaming = ok;                      // the feeder starts pulling from the ring (gated by pre-roll)
    xSemaphoreGive(s_spk_lock);
    if (!ok) {
        NV_LOGW(TAG, "pcm_begin: reopen failed (%d/%dch/%db)", sample_rate, channels, bits);
        if (s_pcm_lock) xSemaphoreGive(s_pcm_lock);   // no stream to end -> release now
    }
    return ok;
}
bool nv_audio_pcm_begin(int sample_rate, int channels, int bits) {
    return nv_audio_pcm_begin_as(sample_rate, channels, bits, NV_PCM_MUSIC);
}

int nv_audio_pcm_write(const void *pcm, size_t bytes) {
    if (!pcm || !bytes || !s_ring) return -1;
    // Copy into the PSRAM ring; a full ring blocks briefly — that IS the decoder's pacing.
    const uint8_t *p = (const uint8_t *)pcm;
    size_t left = bytes;
    int stuck = 0;
    while (left) {
        if (!s_streaming) return -1;                         // stream ended underneath us
        if (s_sfx_cancel && s_pcm_owner == NV_PCM_SFX) return -1;   // voice preempts this SFX: bail now
        const size_t f = ring_free();
        if (!f) {
            if (s_pcm_paused) { vTaskDelay(pdMS_TO_TICKS(20)); continue; }   // paused ≠ dead sink
            if (++stuck > 500) return -1;                    // sink dead for 5 s: give up
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        const size_t take = f < left ? f : left;
        ring_push(p, take);
        p += take;
        left -= take;
        stuck = 0;
    }
    return (int)bytes;
}

// Drop everything buffered (user stop / track switch / seek): playback cuts within one chunk.
// Signals the feeder instead of resetting the indices from this (foreign) thread — see the
// s_flush_req note above. Idempotent; safe to call when nothing is streaming (begin swallows it).
void nv_audio_pcm_flush(void) { __atomic_add_fetch(&s_flush_req, 1, __ATOMIC_RELEASE); }

// Freeze/unfreeze playback INCLUDING the buffered ring (true pause — the decoder's own pause
// alone would leave up to ring-depth seconds still playing).
void nv_audio_pcm_pause(bool on) { s_pcm_paused = on; }

// Bytes queued but not yet heard — lets the player show the REAL playhead (decoded-ahead
// minus this backlog) instead of the decode position, which runs up to the ring ahead.
size_t nv_audio_pcm_backlog(void) { return ring_avail(); }

void nv_audio_pcm_end(void) {
    if (!s_spk_lock) return;
    // Natural end: let the feeder play out the buffered tail (callers wanting an instant cut
    // call nv_audio_pcm_flush() first), then release the sink and restore the tone format.
    s_pcm_paused = false;                  // a paused stream must still drain and close
    s_draining = true;
    int guard = 0;
    while ((ring_avail() || s_feed_busy) && s_streaming && ++guard < 800)
        vTaskDelay(pdMS_TO_TICKS(10));                       // bounded: <= 8 s
    xSemaphoreTake(s_spk_lock, portMAX_DELAY);
    s_streaming = false;
    s_draining = false;
    s_prebuf_target = 0;
    s_pcm_owner = 0;
    ring_reset();
    out_open(kRate, 1, 16);   // default tone format (48 kHz mono) on the live route
    xSemaphoreGive(s_spk_lock);
    if (s_pcm_lock) xSemaphoreGive(s_pcm_lock);   // release stream ownership -> next begin can proceed
}

// Voice priority (ref-counted). Raising it hands the DAC to the voice engine: a live sound effect
// is cancelled (its _write returns <0, its worker bails and frees the channel), queued tones are
// dropped, and further tones are muted until the voice finishes. Music is never auto-cut.
void nv_audio_voice_priority(bool on) {
    if (on) {
        s_voice_prio++;
        if (s_pcm_owner == NV_PCM_SFX) {   // kick a playing jingle off the channel; music is spared
            s_sfx_cancel = true;
            nv_audio_pcm_flush();          // drop its buffered tail so it stops NOW
        }
        if (s_q) xQueueReset(s_q);         // discard pending beeps
    } else {
        if (--s_voice_prio <= 0) { s_voice_prio = 0; s_sfx_cancel = false; }
    }
}

void nv_audio_click(void) { if (s_keyclick) nv_audio_tone(2200, 18); }   // short soft tick
void nv_audio_chime(void) { nv_audio_tone(880, 90); }    // A5 confirmation
void nv_audio_alert(void) { nv_audio_tone(300, 180); }   // low error buzz

// ---- microphone API (contract in the header) ----------------------------------------------------

bool nv_audio_mic_ready(void) { return s_mic != nullptr; }

bool nv_audio_mic_meter_start(void) {
    if (!mic_worker_up()) return false;
    if (s_mic_state == NV_MIC_REC || s_mic_state == NV_MIC_PLAY) return false;
    if (s_mic_state == NV_MIC_METER) return true;
    const MicCmd c = MicCmd::MeterStart;
    return xQueueSend(s_mic_q, &c, 0) == pdTRUE;
}

void nv_audio_mic_meter_stop(void) {
    if (!s_mic_q || s_mic_state != NV_MIC_METER) return;
    const MicCmd c = MicCmd::MeterStop;
    xQueueSend(s_mic_q, &c, 0);
}

int nv_audio_mic_level(void) { return s_mic_level; }

bool nv_audio_mic_test_start(int ms) {
    if (!mic_worker_up()) return false;
    if (s_mic_state == NV_MIC_REC || s_mic_state == NV_MIC_PLAY) return false;
    if (ms < 500) ms = 500; else if (ms > kMicTestMaxMs) ms = kMicTestMaxMs;
    s_mic_test_ms = ms;
    const MicCmd c = MicCmd::Test;
    return xQueueSend(s_mic_q, &c, 0) == pdTRUE;
}

nv_mic_state_t nv_audio_mic_state(void) { return s_mic_state; }

// ---- voice recorder (contract in the header) ----------------------------------------------------

bool nv_audio_rec_start(const char *path) {
    if (!path || !mic_worker_up()) return false;
    if (s_rec_run || s_mic_state == NV_MIC_REC || s_mic_state == NV_MIC_PLAY) return false;
    strncpy(s_rec_path, path, sizeof s_rec_path - 1);
    s_rec_path[sizeof s_rec_path - 1] = '\0';
    s_rec_start = xTaskGetTickCount();
    const MicCmd c = MicCmd::RecStart;
    return xQueueSend(s_mic_q, &c, 0) == pdTRUE;
}

void nv_audio_rec_stop(void) {
    if (!s_mic_q) return;
    const MicCmd c = MicCmd::RecStop;
    xQueueSend(s_mic_q, &c, 0);
}

bool nv_audio_rec_active(void) { return s_rec_run; }

uint32_t nv_audio_rec_secs(void) {
    if (!s_rec_run) return 0;
    return (uint32_t)(((xTaskGetTickCount() - s_rec_start) * portTICK_PERIOD_MS) / 1000);
}
