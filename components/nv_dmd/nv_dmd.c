// nv_dmd — see include/nv_dmd.h. Two receivers feed one "newest frame" for the renderer:
//   * the socket side: accept, HTTP upgrade, WebSocket frames, dmdext messages;
//   * the USB side: the ZeDMD protocol on the USB-Serial-JTAG port.
// All parsing is in dmd_core.c.
#include "nv_dmd.h"

#include <errno.h>
#include <string.h>

#include "dmd_core.h"
#include "driver/usb_serial_jtag.h"
#include "driver/usb_serial_jtag_vfs.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include "miniz.h"   // ESP ROM tinfl
#include "nv_log.h"
#include "nv_mem_attr.h"
#include "soc/reg_base.h"
#include "soc/soc.h"

#define TAG "dmd"

#define MSG_CAP   (DMD_MAX_PX * 3 + 1024)   // the largest message: an rgb24 frame at DMD_MAX_PX dots
#define RX_CAP    (MSG_CAP + 64)            // one whole frame (header + payload) must fit
#define HS_CAP    4096
#define Z_W       128                       // the ZeDMD we answer as (dmdext's [zedmd]: 128x32)
#define Z_H       32
#define Z_CHUNK   1920                      // package size: libzedmd's maximum, the fewest acks per frame
#define USB_RX    2048
#define STOP_MS   2000                      // the longest nv_dmd_stop() waits for a task (LVGL thread)
#define BQ_BYTES  (6 * 1024 * 1024)         // bridge messages waiting for the app, at most (PSRAM)

// Task-context state only (no ISR, no DMA): PSRAM .bss, internal SRAM is the scarce tier.
static NV_PSRAM_BSS struct {
    TaskHandle_t      task, utask;
    volatile bool     run;           // the receivers are on (the USB task runs while it is)
    volatile bool     wrun;          // the Wi-Fi task runs while it is (a port change restarts only that one)
    SemaphoreHandle_t done, udone;   // given by each task as it exits
    SemaphoreHandle_t lock;       // frame buffers + status
    SemaphoreHandle_t fresh;      // binary: a new frame was published
    int               port;
    volatile int32_t  color;      // colour override for gray frames (-1: the PC's; set by the app)
    uint8_t          *rx, *msg;
    uint32_t         *work, *ready;  // network decode buffer / the newest frame
    uint32_t         *draw;          // the renderer's copy (nv_dmd_acquire)
    dmd_zedmd_t      *z;             // USB: ZeDMD parser, its picture, the ROM inflater
    uint32_t         *zimg;
    uint32_t         *ztint;     // a monochrome ZeDMD frame in the panel's colour (the picture itself keeps building)
    tinfl_decompressor *tinfl;
    uint8_t          *urx;
    dmd_link_t        link;          // USB: the menu bridge's messages (same port, no table running)
    uint8_t          *lbuf;
    QueueHandle_t     bq;            // bridge messages for the app (nv_dmd_bridge_take)
    uint32_t          bq_bytes;      // their payloads, under `lock`
    bool              usb_owned;     // we installed the USB-Serial-JTAG driver
    volatile bool     zedmd_on;      // a ZeDMD client (dmdext, a table running) has the port: no bridge there
    bool              muted;
    vprintf_like_t    prev_vprintf;
    int               rw, rh;     // dimensions of `ready`
    uint32_t          gen;
    nv_dmd_status_t   st;
} s;

// ---------------------------------------------------------------- frames

// A decoded frame from either link becomes the newest one (copied: each link keeps its own buffer).
static void publish_ex(const uint32_t *px, int w, int h, const char *fmt, const char *game, bool usb, bool local) {
    const size_t n = (size_t)w * (size_t)h;
    const int64_t now = esp_timer_get_time();
    xSemaphoreTake(s.lock, portMAX_DELAY);
    if (!s.ready) { xSemaphoreGive(s.lock); return; }   // stopped meanwhile
    if (!local) {
        s.st.frames++;
        s.st.last_pc_us = now;
        s.st.last_usb = usb;
    }
    s.st.last_frame_us = now;
    s.st.fmt = fmt;
    if (game) memcpy(s.st.game, game, sizeof s.st.game);
    // Only a different picture is a new generation: the display skips identical frames for free.
    const bool same = s.gen && s.rw == w && s.rh == h && !memcmp(s.ready, px, n * 4);
    if (!same) {
        bool blank = true;
        for (size_t i = 0; i < n && blank; i++) blank = !(px[i] & 0xFFFFFFu);
        s.st.blank = blank;
        memcpy(s.ready, px, n * 4);
        s.rw = w;
        s.rh = h;
        s.st.w = w;
        s.st.h = h;
        if (++s.gen == 0) s.gen = 1;
    }
    xSemaphoreGive(s.lock);
    if (!same) xSemaphoreGive(s.fresh);
}

static void publish(const uint32_t *px, int w, int h, const char *fmt, const char *game, bool usb) {
    publish_ex(px, w, h, fmt, game, usb, false);
}

bool nv_dmd_show(const uint32_t *px, int w, int h, const char *what) {
    if (!s.run || !px || w < 1 || h < 1 || (size_t)w * (size_t)h > DMD_MAX_PX) return false;
    publish_ex(px, w, h, what, NULL, false, true);
    return true;
}

bool nv_dmd_wait(uint32_t ms) { return s.fresh && xSemaphoreTake(s.fresh, pdMS_TO_TICKS(ms)) == pdTRUE; }

uint32_t nv_dmd_latest(void) { return s.gen; }

// The renderer gets a copy of the newest frame: the lock is held for a copy (16 KB for 128x32, under a
// millisecond), never for a whole draw, so the receivers - the USB one must ack within 16 ms - never wait
// on the display.
const uint32_t *nv_dmd_acquire(int *w, int *h, uint32_t *gen) {
    if (!s.lock) return NULL;
    xSemaphoreTake(s.lock, portMAX_DELAY);
    if (!s.ready || !s.draw || !s.gen) { xSemaphoreGive(s.lock); return NULL; }
    *w = s.rw;
    *h = s.rh;
    if (gen) *gen = s.gen;
    memcpy(s.draw, s.ready, (size_t)s.rw * (size_t)s.rh * 4);
    xSemaphoreGive(s.lock);
    return s.draw;
}

bool nv_dmd_copy(uint32_t *dst, size_t cap, int *w, int *h) {
    if (!s.lock || !dst) return false;
    xSemaphoreTake(s.lock, portMAX_DELAY);
    const size_t n = (size_t)s.rw * (size_t)s.rh;
    const bool ok = s.ready && s.gen && n && n <= cap;
    if (ok) {
        memcpy(dst, s.ready, n * 4);
        *w = s.rw;
        *h = s.rh;
    }
    xSemaphoreGive(s.lock);
    return ok;
}

// Only the display uses s.draw, and the buffers are freed only after the display stopped (the DMD app
// stops the receivers once its layer is detached, or from the LVGL thread that also runs the draw).
void nv_dmd_release(void) {}

void nv_dmd_set_color(int32_t rgb) { s.color = rgb; }

void nv_dmd_get_status(nv_dmd_status_t *out) {
    if (!s.lock) { memset(out, 0, sizeof *out); out->fmt = "-"; return; }
    xSemaphoreTake(s.lock, portMAX_DELAY);
    *out = s.st;
    xSemaphoreGive(s.lock);
}

static void set_client(bool on, const char *peer) {
    xSemaphoreTake(s.lock, portMAX_DELAY);
    s.st.client = on;
    snprintf(s.st.peer, sizeof s.st.peer, "%s", on && peer ? peer : "");
    xSemaphoreGive(s.lock);
}

static void count_bad(void) {
    xSemaphoreTake(s.lock, portMAX_DELAY);
    s.st.bad++;
    xSemaphoreGive(s.lock);
}

// ---------------------------------------------------------------- connection

static bool send_all(int fd, const void *p, size_t n) {
    const uint8_t *b = p;
    while (n) {
        const int r = send(fd, b, n, 0);
        if (r <= 0) return false;
        b += r;
        n -= (size_t)r;
    }
    return true;
}

static bool send_frame(int fd, uint8_t op, const uint8_t *pl, size_t n) {
    uint8_t h[10];
    const size_t hl = dmd_ws_header(op, n, h);
    return send_all(fd, h, hl) && (!n || send_all(fd, pl, n));
}

typedef struct {
    int         fd;
    bool        upgraded;
    size_t      len;        // bytes in s.rx
    dmd_ws_t    ws;
    dmd_state_t d;
} conn_t;

static void conn_close(conn_t *c) {
    if (c->fd >= 0) close(c->fd);
    c->fd = -1;
    set_client(false, NULL);
}

static void conn_open(conn_t *c, int fd, const struct sockaddr_in *a) {
    char ip[16];
    inet_ntoa_r(a->sin_addr, ip, sizeof ip);
    const int one = 1, idle = 10, intvl = 5, cnt = 3;
    // A PC that vanished (power, cable) is noticed in ~25 s, not after lwIP's default 2 h of silence.
    setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &one, sizeof one);
    setsockopt(fd, IPPROTO_TCP, TCP_KEEPIDLE, &idle, sizeof idle);
    setsockopt(fd, IPPROTO_TCP, TCP_KEEPINTVL, &intvl, sizeof intvl);
    setsockopt(fd, IPPROTO_TCP, TCP_KEEPCNT, &cnt, sizeof cnt);
    // Our replies (101 / 400, pong, close) give up after 200 ms: a PC that stops reading is dropped, never
    // waited for (nv_dmd_stop() waits for this task on the LVGL thread). recv() needs no timeout: it runs
    // only after select() said there is data.
    const struct timeval sto = {.tv_sec = 0, .tv_usec = 200000};
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &sto, sizeof sto);
    c->fd = fd;
    c->upgraded = false;
    c->len = 0;
    dmd_ws_init(&c->ws, s.msg, MSG_CAP);
    dmd_state_init(&c->d);
    set_client(true, ip);
    nv_log_write(NV_LOG_INFO, TAG, "client %s", ip);
}

// Read what the socket has and act on it. False: drop the connection.
static bool conn_read(conn_t *c) {
    const size_t cap = c->upgraded ? RX_CAP : HS_CAP;
    if (c->len >= cap) return false;
    const int r = recv(c->fd, s.rx + c->len, cap - c->len, 0);
    if (r <= 0) return false;
    c->len += (size_t)r;

    if (!c->upgraded) {
        char resp[256];
        size_t used = 0, rl = 0;
        const dmd_hs_t hs = dmd_ws_handshake((const char *)s.rx, c->len, &used, resp, sizeof resp, &rl);
        if (hs == DMD_HS_INCOMPLETE) return true;
        if (hs != DMD_HS_OK) {
            if (rl) send_all(c->fd, resp, rl);
            count_bad();
            return false;
        }
        if (rl && !send_all(c->fd, resp, rl)) return false;
        c->upgraded = true;
        memmove(s.rx, s.rx + used, c->len - used);
        c->len -= used;
    }

    size_t off = 0;
    while (off < c->len) {
        size_t used = 0;
        const dmd_ws_ev_t ev = dmd_ws_next(&c->ws, s.rx + off, c->len - off, &used);
        if (ev == DMD_WS_MORE) break;
        if (ev == DMD_WS_ERROR) { count_bad(); return false; }
        off += used;
        if (ev == DMD_WS_PING) {
            if (!send_frame(c->fd, 0xA, c->ws.ctl, c->ws.ctl_len)) return false;
        } else if (ev == DMD_WS_CLOSE) {
            send_frame(c->fd, 0x8, NULL, 0);
            return false;
        } else if (ev == DMD_WS_MESSAGE) {
            c->d.color_override = s.color;
            const dmd_msg_t m = dmd_proto_feed(&c->d, c->ws.msg, c->ws.msg_len, s.work, DMD_MAX_PX);
            if (m == DMD_MSG_FRAME) publish(s.work, c->d.w, c->d.h, dmd_fmt_name(c->d.last_fmt), c->d.game, false);
            else if (m == DMD_MSG_BAD) count_bad();
            else if (m == DMD_MSG_META) {   // the name and size show on the status page before a frame
                xSemaphoreTake(s.lock, portMAX_DELAY);
                memcpy(s.st.game, c->d.game, sizeof s.st.game);
                xSemaphoreGive(s.lock);
            }
        }
    }
    if (off) {
        memmove(s.rx, s.rx + off, c->len - off);
        c->len -= off;
    }
    return true;
}

// ---------------------------------------------------------------- task

static int listen_on(int port) {
    const int fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (fd < 0) return -1;
    const int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in a = {0};
    a.sin_family = AF_INET;
    a.sin_port = htons((uint16_t)port);
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(fd, (struct sockaddr *)&a, sizeof a) < 0 || listen(fd, 2) < 0) {
        nv_log_write(NV_LOG_ERROR, TAG, "port %d: %d", port, errno);
        close(fd);
        return -1;
    }
    return fd;
}

static void dmd_task(void *arg) {
    (void)arg;
    int lfd = -1;
    conn_t c = {.fd = -1};
    int64_t fps_t0 = esp_timer_get_time();
    uint32_t fps_n0 = 0;
    while (s.wrun) {
        if (lfd < 0) {
            lfd = listen_on(s.port);
            xSemaphoreTake(s.lock, portMAX_DELAY);
            s.st.listening = lfd >= 0;
            xSemaphoreGive(s.lock);
            if (lfd < 0) { vTaskDelay(pdMS_TO_TICKS(1000)); continue; }   // Wi-Fi not up yet: retry
            nv_log_write(NV_LOG_INFO, TAG, "listening on %d", s.port);
        }
        fd_set rd;
        FD_ZERO(&rd);
        FD_SET(lfd, &rd);
        int maxfd = lfd;
        if (c.fd >= 0) { FD_SET(c.fd, &rd); if (c.fd > maxfd) maxfd = c.fd; }
        struct timeval tv = {.tv_sec = 0, .tv_usec = 100000};
        const int n = select(maxfd + 1, &rd, NULL, NULL, &tv);
        if (n < 0) {   // the network went away under us: reopen the port
            conn_close(&c);
            close(lfd);
            lfd = -1;
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }
        if (n > 0 && FD_ISSET(lfd, &rd)) {
            struct sockaddr_in a;
            socklen_t al = sizeof a;
            const int fd = accept(lfd, (struct sockaddr *)&a, &al);
            if (fd >= 0) {
                conn_close(&c);          // the newest client wins (the PC restarted or reconnected)
                conn_open(&c, fd, &a);
            }
        }
        if (n > 0 && c.fd >= 0 && FD_ISSET(c.fd, &rd) && !conn_read(&c)) {
            nv_log_write(NV_LOG_INFO, TAG, "client gone");
            conn_close(&c);
        }
        const int64_t now = esp_timer_get_time();
        if (now - fps_t0 >= 1000000) {
            xSemaphoreTake(s.lock, portMAX_DELAY);
            s.st.fps10 = (uint32_t)((uint64_t)(s.st.frames - fps_n0) * 10000000ull / (uint64_t)(now - fps_t0));
            fps_n0 = s.st.frames;
            xSemaphoreGive(s.lock);
            fps_t0 = now;
        }
    }
    conn_close(&c);
    if (lfd >= 0) close(lfd);
    xSemaphoreTake(s.lock, portMAX_DELAY);
    s.st.listening = false;
    xSemaphoreGive(s.lock);
    xSemaphoreGive(s.done);
    vTaskDeleteWithCaps(NULL);
}

// ---------------------------------------------------------------- USB (ZeDMD)

static int rom_inflate(void *ctx, const uint8_t *in, size_t n, uint8_t *out, size_t cap) {
    tinfl_decompressor *d = ctx;
    tinfl_init(d);
    size_t in_sz = n, out_sz = cap;
    const tinfl_status st = tinfl_decompress(d, in, &in_sz, out, out, &out_sz,
                                             TINFL_FLAG_PARSE_ZLIB_HEADER | TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF);
    return st == TINFL_STATUS_DONE ? (int)out_sz : -1;
}

static int mute_vprintf(const char *fmt, va_list ap) { (void)fmt; (void)ap; return 0; }

// Serial logs off while a PC streams over USB: a line would land between the protocol's bytes (the PC
// then drops that frame and asks for a full one). The log ring (/api/logs) keeps recording.
static void mute(bool on) {
    if (on == s.muted) return;
    s.muted = on;
    nv_log_serial_mute(on);
    if (on) s.prev_vprintf = esp_log_set_vprintf(mute_vprintf);
    else esp_log_set_vprintf(s.prev_vprintf ? s.prev_vprintf : vprintf);
}

static void set_usb(bool on) {
    xSemaphoreTake(s.lock, portMAX_DELAY);
    s.st.usb = on;
    xSemaphoreGive(s.lock);
}

typedef struct {
    uint8_t  type;
    uint32_t len;
    uint8_t *data;   // PSRAM, len + 1 bytes (NUL-terminated for JSON)
} bmsg_t;

// A complete bridge message: the ping only refreshes "bridge present", the rest goes to the app.
static void on_bridge(void *ctx, uint8_t type, const uint8_t *p, uint32_t len) {
    (void)ctx;
    const int64_t now = esp_timer_get_time();
    xSemaphoreTake(s.lock, portMAX_DELAY);
    s.st.bridge = true;
    s.st.last_bridge_us = now;
    xSemaphoreGive(s.lock);
    s.zedmd_on = false;   // the bridge has the port again (before the ack below, which needs it)
    if (type == 'P' || !s.bq) return;
    // Bounded in bytes too: 16 full-panel images would be 19 MB of PSRAM. Dropped, an image or a list gets no
    // ack, and the bridge sends it again.
    xSemaphoreTake(s.lock, portMAX_DELAY);
    const uint32_t queued = s.bq_bytes;
    const bool room = !queued || queued + len <= BQ_BYTES;
    if (room) s.bq_bytes += len;
    xSemaphoreGive(s.lock);
    if (!room) {
        nv_log_write(NV_LOG_WARN, TAG, "bridge: %u KB wait for the app, a message (type %c, %u KB) dropped",
                     (unsigned)(queued >> 10), type, (unsigned)(len >> 10));
        return;
    }
    bmsg_t m = {.type = type, .len = len, .data = heap_caps_malloc(len + 1, MALLOC_CAP_SPIRAM)};
    if (m.data) {
        memcpy(m.data, p, len);
        m.data[len] = 0;
    }
    // The app takes them on its 10 Hz tick: wait one tick rather than drop an image it asked for, not longer
    // (the receiver reads nothing meanwhile, and the driver's buffer holds ~160 ms of the bridge).
    if (!m.data || xQueueSend(s.bq, &m, pdMS_TO_TICKS(100)) != pdTRUE) {
        xSemaphoreTake(s.lock, portMAX_DELAY);
        s.bq_bytes -= len;
        xSemaphoreGive(s.lock);
        if (m.data) heap_caps_free(m.data);
        nv_log_write(NV_LOG_WARN, TAG, m.data ? "bridge: the app's queue is full, a message (type %c, %u KB) dropped"
                                              : "bridge: no memory for a message (type %c, %u KB), dropped",
                     type, (unsigned)(len >> 10));
        return;
    }
    // An image or a list came whole: said to the bridge (its length and checksum), which sends a game's pictures
    // and the lists again until they are confirmed (a USB-Serial-JTAG receive loses bytes now and then: seen at
    // every few starts; a lost list left the menu on the card's copy, days old)
    if (type == 'I' || type == 'L') {
        char ack[64];
        snprintf(ack, sizeof ack, "{\"q\":\"ack\",\"n\":%u,\"s\":%u}", (unsigned)len, (unsigned)s.link.sum);
        nv_dmd_bridge_send('Q', ack);
    }
    const int64_t took = esp_timer_get_time() - now;
    if (took > 30000)
        nv_log_write(NV_LOG_WARN, TAG, "bridge: a message (type %c, %u KB) held the receiver %d ms", type,
                     (unsigned)(len >> 10), (int)(took / 1000));
}

bool nv_dmd_bridge_take(uint8_t *type, uint8_t **data, uint32_t *len) {
    bmsg_t m;
    if (!s.bq || xQueueReceive(s.bq, &m, 0) != pdTRUE) return false;
    xSemaphoreTake(s.lock, portMAX_DELAY);
    s.bq_bytes -= m.len <= s.bq_bytes ? m.len : s.bq_bytes;
    xSemaphoreGive(s.lock);
    *type = m.type;
    *data = m.data;
    *len = m.len;
    return true;
}

void nv_dmd_bridge_free(uint8_t *data) { heap_caps_free(data); }

bool nv_dmd_bridge_send(uint8_t type, const char *json) {
    // While dmdext has the port (a table running) a request would reach dmdext, not the bridge: lost (and
    // noise in its ZeDMD stream). Refused instead, so the caller asks again once the bridge is back.
    if (!s.utask || !json || s.zedmd_on) return false;
    const uint32_t n = (uint32_t)strlen(json);
    if (n > 900) return false;
    uint8_t f[9 + 900 + 2];
    dmd_link_header(type, n, f);
    memcpy(f + 9, json, n);
    dmd_link_trailer(type, n, (const uint8_t *)json, f + 9 + n);
    // one write: a log line can never land inside the message
    return usb_serial_jtag_write_bytes(f, 9 + n + 2, pdMS_TO_TICKS(50)) == (int)(9 + n + 2);
}

// The PC that drove the board as a ZeDMD is gone: it said so, it went silent, or the bridge has the port again
// (a table killed with dmdext's port open never sends its reset).
static void zedmd_gone(bool *pc, const char *why, uint32_t resets) {
    if (!*pc) return;
    *pc = false;
    s.zedmd_on = false;
    set_usb(false);
    mute(false);
    nv_log_write(NV_LOG_INFO, TAG, "usb: the ZeDMD PC is gone (%s), %u transfers cut short", why, (unsigned)resets);
}

static void usb_task(void *arg) {
    (void)arg;
    uint8_t out[512];                 // up to USB_RX / 32 chunk acks + one handshake answer
    int64_t last = 0;                 // the last read that had bytes
    int64_t z_last = 0;               // the last ZeDMD command parsed (keep-alives included)
    int64_t l_last = 0;               // the last bytes fed while the link was inside a message
    uint32_t z_cmds = 0, z_resets = 0;   // the parser's command count seen; transfers reset in this session
    bool pc = false;
    // Measured inside each bridge message, said with a damaged one (or a long wait): the driver drops what its
    // buffer (128 KB, ~160 ms of the bridge) cannot hold, without a word.
    int64_t m_gap = 0, m_link = 0, m_rest = 0;   // longest wait between two reads; longest feed / ZeDMD+reply
    uint32_t m_reads = 0, m_full = 0;            // reads in the message; those that filled USB_RX (a backlog)
    while (s.run) {
        const int r = usb_serial_jtag_read_bytes(s.urx, USB_RX, pdMS_TO_TICKS(10));
        const int64_t now = esp_timer_get_time();
        // A pause inside a transfer: the PC gave up on it (an ack it did not get within 16 ms) and starts the
        // next one with "FRAME". Judged by what the port shows - silence (an empty read) or new data opening with
        // "FRAME" - never by the time between two reads alone: that may be this task's own stall (a big bridge
        // message, a busy core), and the rest of the transfer then waits in the driver's buffer.
        if (dmd_zedmd_busy(s.z) && now - last > 12000 && (r <= 0 || (r >= 5 && !memcmp(s.urx, "FRAME", 5)))) {
            dmd_zedmd_reset(s.z);
            z_resets++;
            xSemaphoreTake(s.lock, portMAX_DELAY);
            s.st.zedmd_resets++;
            xSemaphoreGive(s.lock);
        }
        if (s.st.bridge && now - s.st.last_bridge_us > 6000000) {   // the bridge pings every 2 s
            xSemaphoreTake(s.lock, portMAX_DELAY);
            s.st.bridge = false;
            xSemaphoreGive(s.lock);
        }
        if (pc && now - z_last > 10000000) zedmd_gone(&pc, "no command for 10 s", z_resets);   // keep-alive every 3 s
        // A bridge message that stopped half-way (the bridge closed the port, or bytes were lost in its length):
        // the parser would otherwise take whatever follows, up to 1.2 MB, as its payload.
        if (dmd_link_busy(&s.link) && now - l_last > 1000000) {
            const uint32_t got = s.link.st == 2 ? s.link.got : 0;
            dmd_link_reset(&s.link);
            count_bad();
            const uint8_t ty = s.link.bad_type;
            nv_log_write(NV_LOG_WARN, TAG, "bridge: a message (type %c, %u of %u bytes) stopped arriving, dropped",
                         ty >= 32 && ty < 127 ? ty : '?', (unsigned)got, (unsigned)s.link.bad_len);
            m_gap = m_link = m_rest = 0;
            m_reads = m_full = 0;
        }
        if (r <= 0) continue;
        if (s.link.st == 2) {   // inside a bridge message
            if (last && now - last > m_gap) m_gap = now - last;
            m_reads++;
            if (r == USB_RX) m_full++;
        }
        last = now;
        size_t ol = 0;
        const uint32_t bad0 = s.link.bad, ok0 = s.link.ok, rs0 = s.link.resync, len0 = s.link.len;
        dmd_link_feed(&s.link, s.urx, (size_t)r, on_bridge, NULL);   // the menu bridge, if it is the one talking
        const int64_t t_link = esp_timer_get_time() - now;
        if (t_link > m_link) m_link = t_link;
        if (dmd_link_busy(&s.link)) l_last = now;
        if (s.link.ok != ok0) zedmd_gone(&pc, "the bridge has the port", z_resets);   // one program owns a port
        if (s.link.bad != bad0) {   // a bridge message arrived damaged and was dropped: never silently
            const uint8_t ty = s.link.bad_type;
            char saved[64] = "";
            if (s.link.resync != rs0)
                snprintf(saved, sizeof saved, "; %u bytes were missing, the next message saved", (unsigned)s.link.lost);
            nv_log_write(NV_LOG_WARN, "dmd",
                         "bridge: a message (type %c, %u KB) arrived damaged, dropped (longest wait between reads %d ms, "
                         "longest feed %d ms, ZeDMD+reply %d ms, %u of %u reads full%s)",
                         ty >= 32 && ty < 127 ? ty : '?', (unsigned)(s.link.bad_len >> 10), (int)(m_gap / 1000),
                         (int)(m_link / 1000), (int)(m_rest / 1000), (unsigned)m_full, (unsigned)m_reads, saved);
        } else if (s.link.ok != ok0 && len0 >= 65536 && m_gap > 100000) {   // whole, but close to the limit
            nv_log_write(NV_LOG_WARN, TAG, "usb: %d ms between two reads inside a %u KB message (it arrived whole)",
                         (int)(m_gap / 1000), (unsigned)(len0 >> 10));
        }
        if (s.link.bad != bad0 || s.link.ok != ok0) {   // a message ended: the next one is measured afresh
            m_gap = m_link = m_rest = 0;
            m_reads = m_full = 0;
        }
        if (s.link.st) {   // inside a bridge message (a big image takes a few seconds): it is there
            xSemaphoreTake(s.lock, portMAX_DELAY);
            s.st.last_bridge_us = now;
            xSemaphoreGive(s.lock);
        }
        const int64_t t1 = esp_timer_get_time();
        const int ev = dmd_zedmd_feed(s.z, s.urx, (size_t)r, out, sizeof out, &ol);
        if (ol) usb_serial_jtag_write_bytes(out, ol, pdMS_TO_TICKS(20));
        if (s.link.st == 2 && esp_timer_get_time() - t1 > m_rest) m_rest = esp_timer_get_time() - t1;
        if (s.z->cmds != z_cmds) {   // the PC is still there (a keep-alive counts, the bridge's bytes do not)
            z_cmds = s.z->cmds;
            z_last = now;
        }
        if (ev & (DMD_Z_EV_HELLO | DMD_Z_EV_RENDER)) {
            s.zedmd_on = true;
            mute(true);
            if (!pc) {
                pc = true;
                z_last = now;
                z_resets = 0;
                set_usb(true);
            }
        }
        if (ev & DMD_Z_EV_RENDER) {   // over USB dmdext colours a plain DMD itself: the panel's colour wins
            const int32_t col = s.color;
            if (col >= 0 && s.ztint && dmd_mono_recolor(s.zimg, Z_W * Z_H, (uint32_t)col, s.ztint))
                publish(s.ztint, Z_W, Z_H, "ZeDMD", NULL, true);
            else
                publish(s.zimg, Z_W, Z_H, "ZeDMD", NULL, true);
        }
        if (ev & DMD_Z_EV_ERROR) count_bad();
        if (ev & DMD_Z_EV_BYE) zedmd_gone(&pc, "it closed", z_resets);
    }
    mute(false);
    xSemaphoreGive(s.udone);
    vTaskDeleteWithCaps(NULL);
}

// The USB-Serial-JTAG resets the chip on the DTR/RTS pattern esptool uses. Windows hosts (dmdext's
// libserialport, usbser.sys) produce it just by opening the port, and the board rebooted every time
// a table started (reset reason "usb", verified on the cabinet). While the DMD owns the port that
// reset is off; leaving the app turns it back on, so esptool can flash again.
// USB_SERIAL_JTAG_CHIP_RST_REG (named USB_DEVICE_CHIP_RST_REG on rev 3 silicon) bit 2:
// USB_UART_CHIP_RST_DIS.
#define USJ_CHIP_RST_REG (DR_REG_USB_SERIAL_JTAG_BASE + 0x4c)
#define USJ_CHIP_RST_DIS BIT(2)
static void usb_reset_lock(bool on) {
    if (on) REG_SET_BIT(USJ_CHIP_RST_REG, USJ_CHIP_RST_DIS);
    else REG_CLR_BIT(USJ_CHIP_RST_REG, USJ_CHIP_RST_DIS);
}

// Take the USB-Serial-JTAG port: its driver (the console then writes through it too, dropping output
// when no one reads) and a receiver task. False if the port is not available.
static bool usb_start(void) {
    if (!usb_serial_jtag_is_driver_installed()) {
        // A big receive buffer: the driver's interrupt drops what does not fit (ESP-IDF 5.5.2 usb_serial_jtag.c
        // ignores xRingbufferSendFromISR's result) while the PC already had the packet acknowledged. The bridge
        // sends ~800 KB/s: 4 KB was 5 ms, less than the receiver spends copying a 500 KB image out, and the next
        // image was lost (a FullDMD then the backglass with grill, at a table's start). 128 KB: ~160 ms (PSRAM).
        // Not the cause of the rarer losses still seen with the menu loaded (up to 64 bytes, the receiver never
        // behind): the same with the buffer in internal RAM or the interrupt on CPU 0 (measured on the cabinet).
        usb_serial_jtag_driver_config_t cfg = {.tx_buffer_size = 1024, .rx_buffer_size = 128 * 1024};
        if (usb_serial_jtag_driver_install(&cfg) != ESP_OK) return false;
        usb_serial_jtag_vfs_use_driver();
        s.usb_owned = true;
    }
    s.z = heap_caps_malloc(sizeof *s.z, MALLOC_CAP_SPIRAM);
    s.zimg = heap_caps_malloc(Z_W * Z_H * 4, MALLOC_CAP_SPIRAM);
    s.ztint = heap_caps_malloc(Z_W * Z_H * 4, MALLOC_CAP_SPIRAM);
    s.tinfl = heap_caps_malloc(sizeof *s.tinfl, MALLOC_CAP_SPIRAM);
    s.urx = heap_caps_malloc(USB_RX, MALLOC_CAP_SPIRAM);
    s.lbuf = heap_caps_malloc(DMD_LINK_MAX, MALLOC_CAP_SPIRAM);
    if (!s.bq) s.bq = xQueueCreate(16, sizeof(bmsg_t));
    if (!s.z || !s.zimg || !s.tinfl || !s.urx || !s.lbuf || !s.bq) return false;
    dmd_link_init(&s.link, s.lbuf, DMD_LINK_MAX);
    dmd_zedmd_init(s.z, Z_W, Z_H, Z_CHUNK, s.zimg, rom_inflate, s.tinfl);
    xSemaphoreTake(s.udone, 0);
    if (xTaskCreatePinnedToCoreWithCaps(usb_task, "nvdmdusb", 6144, NULL, 6, &s.utask, 0, MALLOC_CAP_SPIRAM) != pdPASS) {
        s.utask = NULL;
        return false;
    }
    usb_reset_lock(true);
    xSemaphoreTake(s.lock, portMAX_DELAY);
    s.st.usb_ready = true;
    xSemaphoreGive(s.lock);
    return true;
}

// After s.run went false. False when the task did not exit within STOP_MS: then nothing is freed (it may still
// use its buffers and the driver), and a later nv_dmd_stop() waits again.
static bool usb_stop(void) {
    usb_reset_lock(false);
    if (s.utask) {
        if (xSemaphoreTake(s.udone, pdMS_TO_TICKS(STOP_MS)) != pdTRUE) return false;   // reads time out every 10 ms
        s.utask = NULL;
    }
    s.zedmd_on = false;
    if (s.usb_owned) {   // the console goes back to writing the port directly
        usb_serial_jtag_vfs_use_nonblocking();
        usb_serial_jtag_driver_uninstall();
        s.usb_owned = false;
    }
    heap_caps_free(s.z);
    heap_caps_free(s.zimg);
    heap_caps_free(s.ztint);
    heap_caps_free(s.tinfl);
    heap_caps_free(s.urx);
    heap_caps_free(s.lbuf);
    s.lbuf = NULL;
    if (s.bq) {   // messages nobody will take
        bmsg_t m;
        while (xQueueReceive(s.bq, &m, 0) == pdTRUE) heap_caps_free(m.data);
        xSemaphoreTake(s.lock, portMAX_DELAY);
        s.bq_bytes = 0;
        xSemaphoreGive(s.lock);
    }
    s.z = NULL;
    s.zimg = NULL;
    s.ztint = NULL;
    s.tinfl = NULL;
    s.urx = NULL;
    return true;
}

// ---------------------------------------------------------------- start / stop

static void free_bufs(void) {
    heap_caps_free(s.rx);
    heap_caps_free(s.msg);
    heap_caps_free(s.work);
    heap_caps_free(s.ready);
    heap_caps_free(s.draw);
    s.rx = s.msg = NULL;
    s.work = s.ready = s.draw = NULL;
}

// The Wi-Fi task on `port` (the buffers are there).
static bool wifi_start(int port) {
    xSemaphoreTake(s.done, 0);
    s.port = port;
    xSemaphoreTake(s.lock, portMAX_DELAY);
    s.st.port = port;
    s.st.listening = false;
    xSemaphoreGive(s.lock);
    s.wrun = true;
    // PSRAM stack: sockets only, never flash (ENGINEERING_RULES §2). Core 0, away from LVGL.
    if (xTaskCreatePinnedToCoreWithCaps(dmd_task, "nvdmd", 6144, NULL, 5, &s.task, 0, MALLOC_CAP_SPIRAM) != pdPASS) {
        s.task = NULL;
        s.wrun = false;
        return false;
    }
    return true;
}

// False when the task did not exit within STOP_MS (s.task and its buffers stay: a later stop waits again).
static bool wifi_stop(void) {
    if (!s.task) return true;
    s.wrun = false;
    if (xSemaphoreTake(s.done, pdMS_TO_TICKS(STOP_MS)) != pdTRUE) return false;   // select() wakes every 100 ms
    s.task = NULL;
    return true;
}

bool nv_dmd_set_port(int port) {
    if (port < 1 || port > 65535) port = NV_DMD_PORT_DEFAULT;
    if (!s.run) return false;
    if (s.task && s.wrun && s.port == port) return true;   // (a task given up for stopping is not a server)
    if (!wifi_stop()) {
        nv_log_write(NV_LOG_ERROR, TAG, "the Wi-Fi receiver did not stop within %d ms: still on port %d", STOP_MS, s.port);
        return false;
    }
    if (!wifi_start(port)) {
        nv_log_write(NV_LOG_ERROR, TAG, "no task for the Wi-Fi receiver (port %d)", port);
        return false;
    }
    return true;
}

bool nv_dmd_start(int port) {
    if (port < 1 || port > 65535) port = NV_DMD_PORT_DEFAULT;
    if (s.run) return nv_dmd_set_port(port);   // running: only the Wi-Fi server moves, USB and the frame stay
    if (s.task || s.utask) {                  // an earlier stop timed out: wait again, never two tasks
        nv_dmd_stop();
        if (s.task || s.utask) return false;
    }
    if (!s.lock) {
        s.lock = xSemaphoreCreateMutex();
        s.fresh = xSemaphoreCreateBinary();
        s.done = xSemaphoreCreateBinary();
        s.udone = xSemaphoreCreateBinary();
        if (!s.lock || !s.fresh || !s.done || !s.udone) return false;
    }
    s.rx = heap_caps_malloc(RX_CAP, MALLOC_CAP_SPIRAM);
    s.msg = heap_caps_malloc(MSG_CAP, MALLOC_CAP_SPIRAM);
    s.work = heap_caps_calloc(DMD_MAX_PX, 4, MALLOC_CAP_SPIRAM);
    s.ready = heap_caps_calloc(DMD_MAX_PX, 4, MALLOC_CAP_SPIRAM);
    s.draw = heap_caps_calloc(DMD_MAX_PX, 4, MALLOC_CAP_SPIRAM);
    if (!s.rx || !s.msg || !s.work || !s.ready || !s.draw) { free_bufs(); return false; }
    xSemaphoreTake(s.lock, portMAX_DELAY);
    memset(&s.st, 0, sizeof s.st);
    s.st.fmt = "-";
    s.gen = 0;
    s.rw = s.rh = 0;
    xSemaphoreGive(s.lock);
    xSemaphoreTake(s.fresh, 0);
    s.run = true;
    if (!wifi_start(port)) {
        s.run = false;
        free_bufs();
        return false;
    }
    if (!usb_start()) nv_log_write(NV_LOG_WARN, TAG, "USB link unavailable");   // Wi-Fi still works
    return true;
}

// On the LVGL thread: every wait is bounded. A task that does not exit in time keeps every buffer it may still
// use (logged), and the next nv_dmd_stop() / nv_dmd_start() waits for it again.
void nv_dmd_stop(void) {
    if (!s.run && !s.task && !s.utask) return;
    s.run = false;
    const bool wifi = wifi_stop();
    const bool usb = usb_stop();
    if (!wifi || !usb) {
        nv_log_write(NV_LOG_ERROR, TAG, "the %s receiver did not stop within %d ms: its buffers stay allocated",
                     !wifi ? "Wi-Fi" : "USB", STOP_MS);
        return;
    }
    xSemaphoreTake(s.lock, portMAX_DELAY);
    free_bufs();
    s.gen = 0;
    memset(&s.st, 0, sizeof s.st);
    s.st.fmt = "-";
    xSemaphoreGive(s.lock);
}
