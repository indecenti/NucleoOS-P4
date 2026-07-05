// nv_usb_audio — USB Audio Class host output. See nv_usb_audio.h for the contract.
// Structure mirrors esp-iot-solution's usb_audio_player example, minus the GMF player:
// nv_audio already delivers ready PCM, so this is just a hot-pluggable UAC sink.
#include "nv_usb_audio.h"

#include "nv_config.h"
#include "nv_log.h"

#include "usb/usb_host.h"
#include "usb/uac_host.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"   // monotonic clock for the sink self-heal cooldown

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"

#include <cstring>

static const char *TAG = "usb_audio";

namespace {

struct Evt {
    enum { DRIVER, DEVICE } group;
    uint8_t addr;
    uint8_t iface_num;
    int event;
    uac_host_device_handle_t handle;
};

QueueHandle_t     s_evtq = nullptr;
SemaphoreHandle_t s_lock = nullptr;
TaskHandle_t      s_uac_task = nullptr;
bool              s_installed = false;

uac_host_device_handle_t s_dev = nullptr;   // guarded by s_lock
volatile bool s_present = false;
// A device can enumerate as UAC yet refuse every write (a data-only Type-C link, a half-broken
// dock). Trust a freshly connected sink; repeated failed writes mark it unhealthy so the router
// stops sending audio there (glitch-free ES8311). But this MUST self-heal: a transient burst of
// errors during format churn (e.g. exiting a video whose stream hammered the device, then the next
// app opens a new format) previously disabled the sink FOREVER — present() went false, so no write
// was ever attempted again, so it could never recover until a physical replug. That looked like
// "audio crashed" in the next app. Now: after kTxRetryUs the sink is re-probed, and a fresh stream
// open or any successful write re-arms it. A genuinely dead device just fails the probe and goes
// quiet again — self-limiting, no permanent death.
volatile bool    s_tx_healthy     = false;
volatile int     s_tx_fails       = 0;      // consecutive failed writes; a success re-arms (self-heal)
volatile int64_t s_tx_unhealthy_us = 0;     // when it was last marked unhealthy (for the re-probe cooldown)
constexpr int64_t kTxRetryUs      = 2000000; // 2 s: re-probe an unhealthy-but-still-enumerated sink
volatile int  s_dev_evt_spam = 0;           // throttle the (noisy) device-event log
uac_host_dev_alt_param_t s_alt = {};        // the PCM alt we stream on (rate menu lives here)
uint32_t s_rate = 0;                        // active stream format (device side)
uint32_t s_src_rate = 0;                    // caller's sample rate (== s_rate when no resampling)
uint8_t  s_ch = 0, s_bits = 0;
int      s_vol = 60;
bool     s_mute = false;
// Deferred volume/mute: the UAC volume is a blocking USB control transfer and the setters run
// on the LVGL thread (slider drag) — the write path applies this flag between chunks instead.
volatile bool s_vol_dirty = false;

int16_t *s_dup = nullptr;                   // convert scratch (PSRAM, lazy): ch-expand + resample
constexpr size_t kDupSamples = 8192;        // 16 KB scratch -> 4K stereo frames per slice

bool alt_supports_rate(const uac_host_dev_alt_param_t &alt, uint32_t rate) {
    if (alt.sample_freq_type == 0)
        return alt.sample_freq_lower <= rate && rate <= alt.sample_freq_upper;
    uint8_t n = alt.sample_freq_type > UAC_FREQ_NUM_MAX ? UAC_FREQ_NUM_MAX : alt.sample_freq_type;
    for (int i = 0; i < n; i++) if (alt.sample_freq[i] == rate) return true;
    return false;
}

uint32_t pick_rate(const uac_host_dev_alt_param_t &alt) {
    static const uint32_t pref[] = {48000, 44100, 32000, 16000, 8000};
    for (uint32_t r : pref) if (alt_supports_rate(alt, r)) return r;
    return alt.sample_freq_type == 0 ? alt.sample_freq_lower : alt.sample_freq[0];
}

// Start (or restart) the stream at `rate`. Caller holds s_lock and s_dev is valid.
bool stream_start_locked(uint32_t rate) {
    if (s_rate == rate && s_ch) return true;
    if (s_rate) uac_host_device_stop(s_dev);            // restart at the new rate
    uac_host_stream_config_t cfg = {};
    cfg.channels = s_alt.channels;
    cfg.bit_resolution = s_alt.bit_resolution;
    cfg.sample_freq = rate;
    if (uac_host_device_start(s_dev, &cfg) != ESP_OK) {
        NV_LOGW(TAG, "stream start %u Hz failed", (unsigned)rate);
        s_rate = 0;
        return false;
    }
    s_rate = rate;
    s_src_rate = rate;                      // native until nv_usb_audio_open says otherwise
    s_ch = s_alt.channels;
    s_bits = s_alt.bit_resolution;
    uac_host_device_set_volume(s_dev, s_mute ? 0 : (uint8_t)s_vol);
    NV_LOGI(TAG, "stream %u Hz / %u ch / %u bit", (unsigned)rate, s_ch, s_bits);
    return true;
}

void on_tx_connected(uint8_t addr, uint8_t iface_num) {
    uac_host_device_handle_t dev = nullptr;
    uac_host_device_config_t cfg = {};
    cfg.addr = addr;
    cfg.iface_num = iface_num;
    // Small on purpose (internal RAM): the real anti-underrun buffering is nv_audio's 2 MB
    // PSRAM playout ring — this stays just the USB transfer staging area (~42 ms).
    cfg.buffer_size = 8000;
    cfg.buffer_threshold = 2000;
    cfg.callback = [](uac_host_device_handle_t h, const uac_host_device_event_t e, void *) {
        Evt evt = {};
        evt.group = Evt::DEVICE;
        evt.event = (int)e;
        evt.handle = h;
        xQueueSend(s_evtq, &evt, 0);
    };
    if (uac_host_device_open(&cfg, &dev) != ESP_OK) { NV_LOGW(TAG, "device open failed"); return; }

    // First PCM output alt wins (a plain speaker/soundbar has exactly one).
    uac_host_dev_info_t info = {};
    if (uac_host_get_device_info(dev, &info) != ESP_OK) { uac_host_device_close(dev); return; }
    bool found = false;
    for (int i = 1; i <= info.iface_alt_num && !found; i++) {
        uac_host_dev_alt_param_t alt = {};
        if (uac_host_get_device_alt_param(dev, i, &alt) != ESP_OK) break;
        if (alt.format == UAC_TYPE_I_PCM && alt.channels > 0 && alt.bit_resolution == 16) {
            s_alt = alt;
            found = true;
        }
    }
    if (!found) {
        NV_LOGW(TAG, "no 16-bit PCM output alt — ignoring device");
        uac_host_device_close(dev);
        return;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_dev = dev;
    s_rate = 0;
    bool ok = stream_start_locked(pick_rate(s_alt));
    s_present = ok;
    s_tx_healthy = ok;            // trust a fresh sink until a write actually fails
    s_tx_fails = 0;
    s_dev_evt_spam = 0;
    xSemaphoreGive(s_lock);
    if (!ok) { uac_host_device_close(dev); xSemaphoreTake(s_lock, portMAX_DELAY); s_dev = nullptr; xSemaphoreGive(s_lock); }
    else NV_LOGI(TAG, "USB speaker connected — system audio routed to USB");
}

void on_disconnected(uac_host_device_handle_t h) {
    xSemaphoreTake(s_lock, portMAX_DELAY);
    const bool ours = (s_dev == h);
    if (ours) { s_dev = nullptr; s_present = false; s_tx_healthy = false; s_rate = 0; s_ch = 0; }
    xSemaphoreGive(s_lock);
    if (ours) {
        uac_host_device_close(h);
        NV_LOGI(TAG, "USB speaker disconnected — audio back on ES8311");
    }
}

void uac_events_task(void *) {
    uac_host_driver_config_t drv = {};
    drv.create_background_task = true;
    drv.task_priority = 5;
    drv.stack_size = 4096;
    drv.core_id = 0;
    drv.callback = [](uint8_t addr, uint8_t iface_num, const uac_host_driver_event_t e, void *) {
        Evt evt = {};
        evt.group = Evt::DRIVER;
        evt.addr = addr;
        evt.iface_num = iface_num;
        evt.event = (int)e;
        xQueueSend(s_evtq, &evt, 0);
    };
    if (uac_host_install(&drv) != ESP_OK) { NV_LOGE(TAG, "uac_host_install failed"); vTaskDelete(nullptr); }
    NV_LOGI(TAG, "UAC host ready (plug a USB speaker into the HS Type-C)");
    Evt evt;
    for (;;) {
        if (xQueueReceive(s_evtq, &evt, portMAX_DELAY) != pdTRUE) continue;
        if (evt.group == Evt::DRIVER) {
            NV_LOGI(TAG, "driver event %d (addr %u iface %u)", evt.event, evt.addr, evt.iface_num);
            if (evt.event == (int)UAC_HOST_DRIVER_EVENT_TX_CONNECTED) on_tx_connected(evt.addr, evt.iface_num);
            // RX (USB microphones) intentionally ignored: the on-board MIC1 covers input.
        } else {
            if (s_dev_evt_spam++ < 4) NV_LOGI(TAG, "device event %d", evt.event);   // throttle: a bad sink floods these
            else if (s_dev_evt_spam == 5) NV_LOGW(TAG, "device event %d (further events suppressed)", evt.event);
            if (evt.event == (int)UAC_HOST_DRIVER_EVENT_DISCONNECTED) on_disconnected(evt.handle);
        }
    }
}

void usb_lib_task(void *) {
    usb_host_config_t cfg = {};
    cfg.skip_phy_setup = false;
    cfg.intr_flags = ESP_INTR_FLAG_LEVEL1;
    if (usb_host_install(&cfg) != ESP_OK) { NV_LOGE(TAG, "usb_host_install failed"); vTaskDelete(nullptr); }
    xTaskNotifyGive(s_uac_task);
    for (;;) {
        uint32_t flags;
        usb_host_lib_handle_events(portMAX_DELAY, &flags);
        if (flags & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS) usb_host_device_free_all();
    }
}

// ---------------------------------------------------------------------------------------------
// Bus diagnostics client: a second usb_host client that logs EVERY device the root port
// enumerates — VID/PID, speed, and each interface's class/subclass — independent of whether
// the UAC driver claims it. This separates "no data on the wire" (nothing ever logs) from
// "device enumerates but its descriptors don't match the UAC driver" (logs but no speaker).
usb_host_client_handle_t s_diag_cl = nullptr;

void diag_log_device(uint8_t addr) {
    usb_device_handle_t dev = nullptr;
    if (usb_host_device_open(s_diag_cl, addr, &dev) != ESP_OK) {
        NV_LOGW(TAG, "diag: device %u open failed", addr);
        return;
    }
    const usb_device_desc_t *dd = nullptr;
    if (usb_host_get_device_descriptor(dev, &dd) == ESP_OK && dd) {
        usb_device_info_t di = {};
        usb_host_device_info(dev, &di);
        NV_LOGI(TAG, "diag: dev %u VID=%04x PID=%04x usb=%x.%02x class=%02x speed=%s",
                addr, dd->idVendor, dd->idProduct, dd->bcdUSB >> 8, dd->bcdUSB & 0xFF,
                dd->bDeviceClass,
                di.speed == USB_SPEED_LOW ? "low" : di.speed == USB_SPEED_FULL ? "full" : "high");
    }
    const usb_config_desc_t *cd = nullptr;
    if (usb_host_get_active_config_descriptor(dev, &cd) == ESP_OK && cd) {
        const uint8_t *p = (const uint8_t *)cd;
        int off = 0;
        while (off + 2 <= cd->wTotalLength) {
            uint8_t len = p[off], type = p[off + 1];
            if (len < 2) break;
            if (type == USB_B_DESCRIPTOR_TYPE_INTERFACE && off + 9 <= cd->wTotalLength) {
                NV_LOGI(TAG, "diag:   iface %u alt %u class=%02x sub=%02x proto=%02x eps=%u",
                        p[off + 2], p[off + 3], p[off + 5], p[off + 6], p[off + 7], p[off + 4]);
            }
            off += len;
        }
    }
    usb_host_device_close(s_diag_cl, dev);
}

void diag_client_task(void *) {
    usb_host_client_config_t cfg = {};
    cfg.is_synchronous = false;
    cfg.max_num_event_msg = 8;
    cfg.async.client_event_callback = [](const usb_host_client_event_msg_t *msg, void *) {
        if (msg->event == USB_HOST_CLIENT_EVENT_NEW_DEV) diag_log_device(msg->new_dev.address);
        else if (msg->event == USB_HOST_CLIENT_EVENT_DEV_GONE) NV_LOGI(TAG, "diag: device gone");
    };
    cfg.async.callback_arg = nullptr;
    if (usb_host_client_register(&cfg, &s_diag_cl) != ESP_OK) {
        NV_LOGW(TAG, "diag client register failed");
        vTaskDelete(nullptr);
    }
    NV_LOGI(TAG, "diag: bus watcher up (logs every enumeration)");
    // NOTE: no root-port power-cycle here — tried as an unstick kick, but it broke an
    // already-streaming UAC device at boot (control transfer error -> disconnect/reconnect).
    // Boot-attached devices enumerate fine on install; the kick only added churn.
    int last = -1;
    for (;;) {
        usb_host_client_handle_events(s_diag_cl, pdMS_TO_TICKS(10000));   // events or 10 s tick
        int n = nv_usb_audio_bus_devices();
        if (n != last) {
            NV_LOGI(TAG, "diag: bus devices: %d", n);
            last = n;
            // Walk and describe EVERY enumerated device — NEW_DEV events fired before this
            // client registered (boot-attached devices) would otherwise stay invisible.
            uint8_t addrs[8];
            int cnt = 0;
            if (usb_host_device_addr_list_fill(sizeof addrs, addrs, &cnt) == ESP_OK)
                for (int i = 0; i < cnt; i++) diag_log_device(addrs[i]);
        }
    }
}

void uac_events_entry(void *) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);   // wait for usb_host_install
    uac_events_task(nullptr);
}

}  // namespace

bool nv_usb_audio_init(void) {
    if (s_installed) return true;
    s_evtq = xQueueCreate(10, sizeof(Evt));
    s_lock = xSemaphoreCreateMutex();
    if (!s_evtq || !s_lock) return false;
    // Forever daemons, no internal-flash writes -> PSRAM stacks (internal SRAM is scarce).
    if (xTaskCreateWithCaps(uac_events_entry, "uac_evt", 4096, nullptr, 4, &s_uac_task,
                            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) return false;
    if (xTaskCreateWithCaps(usb_lib_task, "usb_host", 4096, nullptr, 5, nullptr,
                            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) return false;
    // Diagnostics client (see above). Waits for usb_host_install via a short retry loop.
    xTaskCreateWithCaps([](void *) {
        vTaskDelay(pdMS_TO_TICKS(3000));   // let usb_host_install land first
        diag_client_task(nullptr);
    }, "usb_diag", 4096, nullptr, 3, nullptr, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_installed = true;
    return true;
}

// "present" for callers = a USB sink we can actually PLAY to (enumerated AND accepting writes).
// A device that enumerates but refuses writes reads as absent so audio + the route badge stay on
// the ES8311/JST speaker. Physical enumeration is still visible via nv_usb_audio_bus_devices().
bool nv_usb_audio_present(void) {
    if (!s_present) return false;
    if (s_tx_healthy) return true;
    // Unhealthy but still enumerated: after the cooldown, report present so the next stream re-probes
    // the device with a real open+write. Recovers a transient glitch; a dead sink re-fails and goes
    // quiet again for another cooldown (self-limiting — no log/route flapping faster than kTxRetryUs).
    return (esp_timer_get_time() - s_tx_unhealthy_us) > kTxRetryUs;
}

int nv_usb_audio_bus_devices(void) {
    if (!s_installed) return 0;
    uint8_t list[8];
    int n = 0;
    if (usb_host_device_addr_list_fill(sizeof list, list, &n) != ESP_OK) return 0;
    return n;
}

bool nv_usb_audio_open(int sample_rate, int channels, int bits) {
    if (!s_present || bits != 16) return false;                     // health is re-probed here, not gated
    if (channels < 1 || channels > s_alt.channels) return false;   // mono dup ok; no downmix
    if (sample_rate < 8000 || sample_rate > 96000) return false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool ok = s_dev != nullptr;
    if (ok) {
        // Native rate when the device offers it; otherwise keep the device at its preferred
        // rate and resample in write() (e.g. the Dell AC511 is 48 kHz-only, music is 44.1 kHz).
        if (alt_supports_rate(s_alt, (uint32_t)sample_rate)) ok = stream_start_locked((uint32_t)sample_rate);
        else                                                 ok = stream_start_locked(pick_rate(s_alt));
        if (ok) { s_src_rate = (uint32_t)sample_rate; s_tx_healthy = true; s_tx_fails = 0; }   // fresh stream -> re-trust; write() re-verifies
    }
    xSemaphoreGive(s_lock);
    return ok;
}

int nv_usb_audio_write(const void *pcm, size_t bytes, int src_ch) {
    if (!s_present || !bytes) return -1;
    int ret = (int)bytes;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (!s_dev || !s_rate) { xSemaphoreGive(s_lock); return -1; }
    if (s_vol_dirty) {                                   // deferred volume/mute (see setters)
        s_vol_dirty = false;
        uac_host_device_set_volume(s_dev, (uint8_t)(s_mute ? 0 : s_vol));
        uac_host_device_set_mute(s_dev, s_mute);
    }

    if (src_ch == s_ch && s_src_rate == s_rate) {       // native: straight through
        const esp_err_t err = uac_host_device_write(s_dev, (uint8_t *)pcm, bytes, pdMS_TO_TICKS(1000));
        if (err != ESP_OK) { ret = -1; if (++s_tx_fails >= 3) { s_tx_healthy = false; s_tx_unhealthy_us = esp_timer_get_time(); NV_LOGW(TAG, "uac write: %s -> USB sink paused (ES8311), will re-probe", esp_err_to_name(err)); } }
        else { s_tx_fails = 0; s_tx_healthy = true; }   // a good write re-arms a sink recovering from a glitch
        xSemaphoreGive(s_lock);
        return ret;
    }

    // Convert path: channel-expand (mono -> device width) and/or linear-resample
    // (s_src_rate -> s_rate, 16.16 fixed point) in bounded slices through the PSRAM scratch.
    if (src_ch < 1 || src_ch > s_ch) { xSemaphoreGive(s_lock); return -1; }   // no downmix
    if (!s_dup) s_dup = (int16_t *)heap_caps_malloc(kDupSamples * sizeof(int16_t),
                                                    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_dup) { xSemaphoreGive(s_lock); return -1; }

    const int16_t *in = (const int16_t *)pcm;
    size_t in_frames = bytes / 2 / (size_t)src_ch;
    const uint32_t step = (uint32_t)(((uint64_t)s_src_rate << 16) / s_rate);   // src frames per out frame
    if (!step) { xSemaphoreGive(s_lock); return -1; }
    // Bound each uac write to <= ~4 KB. A big upsample+channel-expand (24 kHz mono -> 48 kHz stereo
    // = x3.5) otherwise turns one 4.6 KB feeder chunk into a 16 KB write that overflows the 8 KB
    // device buffer -> ESP_FAIL -> the whole sink was being disabled. Derive the INPUT slice from the
    // capped output so no samples are dropped (advancing `take` past the produced frames = lost audio).
    size_t max_out = kDupSamples / s_ch;
    const size_t wr_cap = 4096 / ((size_t)s_ch * 2);
    if (wr_cap && max_out > wr_cap) max_out = wr_cap;
    size_t in_cap = (size_t)(((uint64_t)max_out * step) >> 16);
    if (in_cap < 1) in_cap = 1;

    while (in_frames && ret > 0) {
        size_t take = in_frames < in_cap ? in_frames : in_cap;                 // input slice (write-bounded)
        size_t out_frames = (size_t)(((uint64_t)take << 16) / step);
        if (out_frames > max_out) out_frames = max_out;
        if (!out_frames) break;
        uint32_t frac = 0;
        for (size_t f = 0; f < out_frames; f++, frac += step) {
            size_t idx = frac >> 16;
            if (idx >= take - 1) idx = take > 1 ? take - 2 : 0;                // clamp for lerp
            const uint32_t a = frac & 0xFFFF;
            for (int c = 0; c < s_ch; c++) {
                const int sc = c < src_ch ? c : src_ch - 1;                    // mono -> both
                const int32_t v0 = in[idx * src_ch + sc];
                const int32_t v1 = in[(take > 1 ? idx + 1 : idx) * src_ch + sc];
                s_dup[f * s_ch + c] = (int16_t)(v0 + (((v1 - v0) * (int32_t)a) >> 16));
            }
        }
        const esp_err_t err = uac_host_device_write(s_dev, (uint8_t *)s_dup, out_frames * s_ch * 2,
                                                    pdMS_TO_TICKS(1000));
        if (err != ESP_OK) { ret = -1; if (++s_tx_fails >= 3) { s_tx_healthy = false; s_tx_unhealthy_us = esp_timer_get_time(); NV_LOGW(TAG, "uac write(cvt): %s -> USB sink paused (ES8311), will re-probe", esp_err_to_name(err)); } break; }
        s_tx_fails = 0; s_tx_healthy = true;   // a good write re-arms a sink recovering from a glitch
        in += take * src_ch;
        in_frames -= take;
    }
    xSemaphoreGive(s_lock);
    return ret;
}

// Volume/mute are DEFERRED (s_vol_dirty above): the UAC volume is a blocking USB control
// transfer, and these setters run on the LVGL thread — the write path applies it off-UI.
void nv_usb_audio_set_volume(int percent) {
    if (percent < 0) percent = 0; else if (percent > 100) percent = 100;
    s_vol = percent;
    s_vol_dirty = true;
}

void nv_usb_audio_set_mute(bool on) {
    s_mute = on;
    s_vol_dirty = true;
}
