// nv_usb_storage — USB mass storage host: Bulk-Only Transport + SCSI + FATFS volumes.
// See nv_usb_storage.h.
//
// Tasks:
//  - "usb_stor_cl": usb_host client event pump. Transfer completions run here, so it must never
//    block on I/O itself — device arrival/removal is forwarded to the worker.
//  - "usb_stor":    worker. Attaches devices, polls LUNs (TEST UNIT READY, 1 s) to follow card
//    insert/remove, mounts/unmounts, computes free space, runs eject/format requests.
//  FATFS reads/writes run on the caller's task through the diskio hooks below.
//
// Lifetime rules (why it is shaped like this):
//  - A slot's VFS + FATFS object are registered (f_mount'ed lazily) once and NEVER freed or
//    f_mount(NULL)'d — FatFs deletes the volume mutex there with no locking, which would race a
//    task still inside read() on a yanked drive. Removal instead drops `allow_io` (diskio fails
//    fast, FatFs can't auto-remount) and invalidates fs_type under the volume mutex; the next
//    mount re-runs FatFs' mount_volume (fresh fs->id, so stale FILs get FR_INVALID_OBJECT).
//    The next device/card reuses the slot (same /usbN, same pdrv).
//  - Each device has one bounce transfer; `io` serializes whole BOT commands (CBW/data/CSW).
//    Transfers are freed only on DEV_GONE while holding `io` (in-flight ones complete with
//    NO_DEVICE first), so a diskio caller can never submit a freed transfer.
#include "nv_usb_storage.h"
#include "nv_event_bus.h"
#include "nv_log.h"

#include "usb/usb_helpers.h"
#include "usb/usb_host.h"

#include "diskio_impl.h"
#include "esp_vfs_fat.h"
#include "ff.h"

#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include <atomic>
#include <cerrno>
#include <cstring>

static const char *TAG = "usb_stor";

namespace {

constexpr int      kMaxDevs  = 4;
constexpr int      kSlots    = NV_USB_STOR_SLOTS;
constexpr size_t   kChunk    = 16 * 1024;   // bounce buffer per device (internal DMA RAM is scarce)
constexpr uint32_t kPollMs   = 1000;
constexpr int64_t  kIdleUs   = 3000000;     // skip the presence poll while the volume is busy
constexpr int      kMaxFiles = 32;          // tracked nv_usb_storage_fopen handles

// ------------------------------------------------------------------ state

struct Dev {
    bool used;
    volatile bool gone;
    bool claimed;
    bool xc_leaked;           // a control transfer never completed: never reuse/free it
    bool x_leaked;            // same for the bulk transfer (cancel did not complete it)
    bool retired;             // a leaked transfer may still give `done`: slot never reused
    bool no_sync;             // SYNCHRONIZE CACHE rejected once -> stop sending it
    uint8_t addr, iface, ep_in, ep_out, max_lun, speed;
    uint16_t mps_in, vid, pid;
    uint32_t tag;
    usb_device_handle_t h;
    SemaphoreHandle_t io;     // one BOT command at a time (created once, never deleted)
    SemaphoreHandle_t done;   // transfer completion (created once, never deleted)
    usb_transfer_t *x;        // bulk: CBW / data / CSW
    usb_transfer_t *xc;       // control
};

struct Slot {
    bool used;                // bound to (dev, lun)
    int dev;
    uint8_t lun;
    nv_usb_stor_state_t state;
    bool registered;          // diskio + VFS + FatFs object registered (kept forever once done)
    volatile bool allow_io;   // diskio may touch the device (mounting, mounted, formatting)
    BYTE pdrv;
    FATFS *fs;
    uint32_t ssize;
    uint64_t nsec;
    bool ro, removable;
    char vendor[9], product[17];
    char label[24], fstype[8];
    uint64_t total, freeb;
    volatile bool dirty;      // written since the last free-space read
    volatile bool lost;       // diskio saw "medium not present / changed"
    volatile int64_t last_io_us;
    int64_t retry_at_us;      // ERROR state: next mount attempt
    int sessions;
    bool ejecting;
};

Dev  s_dev[kMaxDevs];
Slot s_slot[kSlots];
int8_t s_pdrv_slot[FF_VOLUMES];
struct Tracked { FILE *f; int slot; } s_files[kMaxFiles];

SemaphoreHandle_t s_mtx;                 // slot table / sessions / files (short holds only)
QueueHandle_t s_q;
usb_host_client_handle_t s_cl;
std::atomic<uint32_t> s_gen{1};
bool s_started;

enum MsgType : uint8_t { M_NEW, M_GONE, M_EJECT, M_FORMAT };
struct Msg {
    MsgType t;
    uint8_t addr;
    usb_device_handle_t h;
    int slot;
    bool exfat;
    char label[12];
    SemaphoreHandle_t reply;
    bool *result;
};

inline void lock()   { xSemaphoreTake(s_mtx, portMAX_DELAY); }
inline void unlock() { xSemaphoreGive(s_mtx); }

uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
uint64_t be64(const uint8_t *p) { return (uint64_t)be32(p) << 32 | be32(p + 4); }
void put_be32(uint8_t *p, uint32_t v) { p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v; }

void copy_trim(char *dst, size_t cap, const uint8_t *src, size_t n) {
    size_t k = 0;
    for (size_t i = 0; i < n && k + 1 < cap; i++) dst[k++] = (src[i] >= 0x20 && src[i] < 0x7F) ? (char)src[i] : ' ';
    while (k && dst[k - 1] == ' ') k--;
    dst[k] = 0;
}

void publish(int slot, nv_usb_stor_state_t prev, bool detached) {
    s_gen++;
    nv_usb_stor_ev_t ev = {slot, s_slot[slot].state, prev, detached};
    nv_event_publish(NV_EV_USB_STORAGE, &ev);
}

void set_state(int si, nv_usb_stor_state_t st) {
    const nv_usb_stor_state_t prev = s_slot[si].state;
    if (prev == st) return;
    lock();
    s_slot[si].state = st;
    unlock();
    publish(si, prev, false);
}

// ------------------------------------------------------------------ transfers

void xfer_cb(usb_transfer_t *t) { xSemaphoreGive(((Dev *)t->context)->done); }

enum : int { XF_OK = 0, XF_STALL = 1, XF_ERR = -1 };

void cancel_ep(Dev &d, uint8_t ep) {
    usb_host_endpoint_halt(d.h, ep);
    usb_host_endpoint_flush(d.h, ep);
    usb_host_endpoint_clear(d.h, ep);
}

int wait_done(Dev &d, usb_transfer_t *t, uint32_t ms) {
    if (xSemaphoreTake(d.done, pdMS_TO_TICKS(ms)) != pdTRUE) {
        NV_LOGW(TAG, "dev %u: ep %02x timeout (%u ms) -> cancel", d.addr, t->bEndpointAddress, (unsigned)ms);
        cancel_ep(d, t->bEndpointAddress);
        if (xSemaphoreTake(d.done, pdMS_TO_TICKS(2000)) != pdTRUE) {   // flushed -> callback fires now
            NV_LOGE(TAG, "dev %u: transfer cancel never completed -> device disabled", d.addr);
            d.x_leaked = true;
            d.gone = true;
        }
        return XF_ERR;
    }
    switch (t->status) {
        case USB_TRANSFER_STATUS_COMPLETED: return XF_OK;
        case USB_TRANSFER_STATUS_STALL:     return XF_STALL;
        case USB_TRANSFER_STATUS_NO_DEVICE: d.gone = true; return XF_ERR;
        default:                            return XF_ERR;
    }
}

// Bulk transfer through the bounce buffer. IN lengths are rounded up to MPS (host requirement).
int bulk(Dev &d, bool in, void *buf, size_t len, uint32_t ms, size_t *actual) {
    usb_transfer_t *t = d.x;
    if (!t || d.x_leaked || d.gone) return XF_ERR;
    const size_t n = in ? usb_round_up_to_mps(len, d.mps_in) : len;
    if (n > t->data_buffer_size) return XF_ERR;
    if (!in) memcpy(t->data_buffer, buf, len);
    t->device_handle = d.h;
    t->bEndpointAddress = in ? d.ep_in : d.ep_out;
    t->num_bytes = (int)n;
    t->callback = xfer_cb;
    t->context = &d;
    t->timeout_ms = ms;
    if (usb_host_transfer_submit(t) != ESP_OK) return XF_ERR;
    const int r = wait_done(d, t, ms);
    if (r == XF_OK) {
        size_t a = (size_t)t->actual_num_bytes;
        if (in) {
            if (a > len) a = len;
            memcpy(buf, t->data_buffer, a);
        }
        if (actual) *actual = a;
    }
    return r;
}

// Control transfer; `data` receives up to wLength bytes for IN requests.
int ctrl(Dev &d, uint8_t rt, uint8_t req, uint16_t val, uint16_t idx, uint16_t len, void *data) {
    usb_transfer_t *t = d.xc;
    if (!t || d.xc_leaked || d.gone) return XF_ERR;
    auto *s = (usb_setup_packet_t *)t->data_buffer;
    s->bmRequestType = rt;
    s->bRequest = req;
    s->wValue = val;
    s->wIndex = idx;
    s->wLength = len;
    t->num_bytes = (int)(sizeof(usb_setup_packet_t) + len);
    t->device_handle = d.h;
    t->bEndpointAddress = 0;
    t->callback = xfer_cb;
    t->context = &d;
    t->timeout_ms = 5000;
    if (usb_host_transfer_submit_control(s_cl, t) != ESP_OK) return XF_ERR;
    // EP0 can't be halted/flushed: wait long, then give the transfer up for good.
    if (xSemaphoreTake(d.done, pdMS_TO_TICKS(5000)) != pdTRUE &&
        xSemaphoreTake(d.done, pdMS_TO_TICKS(25000)) != pdTRUE) {
        NV_LOGE(TAG, "dev %u: control transfer stuck -> device disabled", d.addr);
        d.xc_leaked = true;
        d.gone = true;
        return XF_ERR;
    }
    if (t->status == USB_TRANSFER_STATUS_STALL) return XF_STALL;
    if (t->status != USB_TRANSFER_STATUS_COMPLETED) return XF_ERR;
    if (data && len) {
        int got = t->actual_num_bytes - (int)sizeof(usb_setup_packet_t);
        if (got > len) got = len;
        if (got > 0) memcpy(data, t->data_buffer + sizeof(usb_setup_packet_t), (size_t)got);
    }
    return XF_OK;
}

void clear_halt(Dev &d, uint8_t ep) {
    usb_host_endpoint_halt(d.h, ep);
    if (usb_host_endpoint_flush(d.h, ep) != ESP_OK) return;   // not halted host-side
    usb_host_endpoint_clear(d.h, ep);
    ctrl(d, 0x02, 0x01 /*CLEAR_FEATURE*/, 0 /*ENDPOINT_HALT*/, ep, 0, nullptr);
}

// BOT 5.3.4 Reset Recovery.
void reset_recovery(Dev &d) {
    if (d.gone) return;
    NV_LOGW(TAG, "dev %u: BOT reset recovery", d.addr);
    ctrl(d, 0x21, 0xFF /*Bulk-Only Mass Storage Reset*/, 0, d.iface, 0, nullptr);
    clear_halt(d, d.ep_in);
    clear_halt(d, d.ep_out);
}

// ------------------------------------------------------------------ BOT / SCSI

struct __attribute__((packed)) Cbw {
    uint32_t sig, tag, len;
    uint8_t flags, lun, cblen, cb[16];
};
struct __attribute__((packed)) Csw {
    uint32_t sig, tag, residue;
    uint8_t status;
};
static_assert(sizeof(Cbw) == 31 && sizeof(Csw) == 13, "BOT wrappers");

enum : int { CMD_OK = 0, CMD_FAILED = 1, CMD_TRANSPORT = -1 };

// One SCSI command (caller holds d.io). len <= kChunk.
int bot(Dev &d, uint8_t lun, const uint8_t *cb, uint8_t cblen, bool in, void *data, uint32_t len, uint32_t ms) {
    if (d.gone) return CMD_TRANSPORT;
    Cbw c = {};
    c.sig = 0x43425355;   // "USBC"
    c.tag = ++d.tag;
    c.len = len;
    c.flags = in ? 0x80 : 0x00;
    c.lun = lun;
    c.cblen = cblen;
    memcpy(c.cb, cb, cblen);
    int r = bulk(d, false, &c, sizeof c, 2000, nullptr);
    if (r != XF_OK) {
        if (r == XF_STALL) clear_halt(d, d.ep_out);
        reset_recovery(d);
        return CMD_TRANSPORT;
    }
    if (len) {
        r = bulk(d, in, data, len, ms, nullptr);
        if (r == XF_STALL) clear_halt(d, in ? d.ep_in : d.ep_out);   // device ends the data phase early
        else if (r != XF_OK) { reset_recovery(d); return CMD_TRANSPORT; }
    }
    Csw s = {};
    size_t got = 0;
    r = bulk(d, true, &s, sizeof s, ms, &got);
    if (r == XF_STALL) {
        clear_halt(d, d.ep_in);
        r = bulk(d, true, &s, sizeof s, ms, &got);
    }
    if (r != XF_OK || got != sizeof s || s.sig != 0x53425355 /*"USBS"*/ || s.tag != c.tag || s.status > 1) {
        reset_recovery(d);
        return CMD_TRANSPORT;
    }
    return s.status == 0 ? CMD_OK : CMD_FAILED;
}

struct Sense { uint8_t key, asc, ascq; };

bool request_sense(Dev &d, uint8_t lun, Sense &s) {
    const uint8_t cb[6] = {0x03, 0, 0, 0, 18, 0};
    uint8_t r[18] = {};
    if (bot(d, lun, cb, 6, true, r, sizeof r, 2000) != CMD_OK) return false;
    s = {(uint8_t)(r[2] & 0x0F), r[12], r[13]};
    return true;
}

enum Ready { R_READY, R_NO_MEDIUM, R_CHANGED, R_BECOMING, R_ERR };

Ready classify(const Sense &s) {
    if (s.key == 0) return R_READY;
    if (s.asc == 0x3A) return R_NO_MEDIUM;                           // MEDIUM NOT PRESENT
    if (s.key == 6 && (s.asc == 0x28 || s.asc == 0x29)) return R_CHANGED;   // medium changed / reset
    if (s.key == 2 && s.asc == 0x04) return R_BECOMING;              // LUN not ready (spinning up)
    return R_ERR;
}

Ready test_ready(Dev &d, uint8_t lun) {
    const uint8_t cb[6] = {0x00, 0, 0, 0, 0, 0};
    const int r = bot(d, lun, cb, 6, false, nullptr, 0, 2000);
    if (r == CMD_OK) return R_READY;
    if (r == CMD_TRANSPORT) return R_ERR;
    Sense s;
    return request_sense(d, lun, s) ? classify(s) : R_ERR;
}

bool inquiry(Dev &d, uint8_t lun, uint8_t &type, bool &rmb, char *vendor, char *product) {
    const uint8_t cb[6] = {0x12, 0, 0, 0, 36, 0};
    uint8_t r[36] = {};
    if (bot(d, lun, cb, 6, true, r, sizeof r, 3000) != CMD_OK) return false;
    type = r[0] & 0x1F;
    rmb = (r[1] & 0x80) != 0;
    copy_trim(vendor, 9, r + 8, 8);
    copy_trim(product, 17, r + 16, 16);
    return true;
}

bool read_capacity(Dev &d, uint8_t lun, uint64_t &nsec, uint32_t &ssize) {
    const uint8_t cb[10] = {0x25};
    uint8_t r[8] = {};
    if (bot(d, lun, cb, 10, true, r, sizeof r, 3000) != CMD_OK) return false;
    uint32_t last = be32(r), bs = be32(r + 4);
    if (last == 0xFFFFFFFF) {                                   // > 2^32 blocks: READ CAPACITY(16)
        uint8_t cb16[16] = {0x9E, 0x10};
        cb16[13] = 32;
        uint8_t r16[32] = {};
        if (bot(d, lun, cb16, 16, true, r16, sizeof r16, 3000) != CMD_OK) return false;
        nsec = be64(r16) + 1;
        bs = be32(r16 + 8);
    } else {
        nsec = (uint64_t)last + 1;
    }
    if (bs < 512 || bs > FF_MAX_SS || (bs & (bs - 1))) {
        NV_LOGW(TAG, "dev %u lun %u: unsupported block size %u", d.addr, lun, (unsigned)bs);
        return false;
    }
    ssize = bs;
    return nsec > 0;
}

bool write_protected(Dev &d, uint8_t lun) {
    const uint8_t cb[6] = {0x1A, 0, 0x3F, 0, 192, 0};   // MODE SENSE(6), all pages
    uint8_t r[192] = {};
    if (bot(d, lun, cb, 6, true, r, sizeof r, 2000) != CMD_OK) {
        Sense s;
        request_sense(d, lun, s);   // clear the check condition; assume writable
        return false;
    }
    return (r[2] & 0x80) != 0;
}

// READ(10)/WRITE(10) in bounce-buffer chunks. Caller holds d.io.
DRESULT rw(Slot &s, Dev &d, bool write, uint8_t *buf, uint32_t sector, unsigned count) {
    const uint32_t per = (uint32_t)(kChunk / s.ssize);
    while (count) {
        const uint32_t n = count < per ? count : per;
        uint8_t cb[10] = {(uint8_t)(write ? 0x2A : 0x28), 0};
        put_be32(cb + 2, sector);
        cb[7] = (uint8_t)(n >> 8);
        cb[8] = (uint8_t)n;
        int r = CMD_TRANSPORT;
        for (int attempt = 0; attempt < 3 && !d.gone; attempt++) {
            r = bot(d, s.lun, cb, 10, !write, buf, n * s.ssize, 10000);
            if (r == CMD_OK) break;
            if (r == CMD_FAILED) {
                Sense se;
                if (request_sense(d, s.lun, se)) {
                    const Ready k = classify(se);
                    if (k == R_NO_MEDIUM || k == R_CHANGED) { s.lost = true; return RES_NOTRDY; }
                    if (se.key == 7) return RES_WRPRT;   // DATA PROTECT
                }
            }
        }
        if (r != CMD_OK) {
            NV_LOGW(TAG, "%s lba %u x%u failed on /usb%d", write ? "write" : "read",
                    (unsigned)sector, (unsigned)n, (int)(&s - s_slot));
            return d.gone ? RES_NOTRDY : RES_ERROR;
        }
        buf += n * s.ssize;
        sector += n;
        count -= n;
    }
    return RES_OK;
}

void sync_cache(Dev &d, uint8_t lun) {
    if (d.no_sync) return;
    const uint8_t cb[10] = {0x35};
    if (bot(d, lun, cb, 10, false, nullptr, 0, 10000) == CMD_FAILED) {
        Sense s;
        if (request_sense(d, lun, s) && s.key == 5) d.no_sync = true;   // ILLEGAL REQUEST: unsupported
    }
}

// ------------------------------------------------------------------ diskio

Slot *slot_for_pdrv(BYTE pdrv) {
    if (pdrv >= FF_VOLUMES || s_pdrv_slot[pdrv] < 0) return nullptr;
    Slot &s = s_slot[s_pdrv_slot[pdrv]];
    return s.used ? &s : nullptr;
}

DSTATUS di_init(BYTE pdrv) {
    Slot *s = slot_for_pdrv(pdrv);
    if (!s || !s->allow_io || s_dev[s->dev].gone) return STA_NOINIT;
    return s->ro ? STA_PROTECT : 0;
}

DSTATUS di_status(BYTE pdrv) { return di_init(pdrv); }

// Common path: resolve + lock the device, re-check it is still ours after taking the lock.
template <typename F>
DRESULT with_dev(BYTE pdrv, F fn) {
    Slot *s = slot_for_pdrv(pdrv);
    if (!s || s->lost || !s->allow_io) return RES_NOTRDY;
    Dev &d = s_dev[s->dev];
    if (xSemaphoreTake(d.io, pdMS_TO_TICKS(30000)) != pdTRUE) return RES_ERROR;
    DRESULT r = RES_NOTRDY;
    if (d.used && !d.gone && s->used && s->allow_io) r = fn(*s, d);
    xSemaphoreGive(d.io);
    s->last_io_us = esp_timer_get_time();
    return r;
}

DRESULT di_read(BYTE pdrv, BYTE *buff, uint32_t sector, UINT count) {
    return with_dev(pdrv, [&](Slot &s, Dev &d) { return rw(s, d, false, buff, sector, count); });
}

DRESULT di_write(BYTE pdrv, const BYTE *buff, uint32_t sector, UINT count) {
    return with_dev(pdrv, [&](Slot &s, Dev &d) {
        if (s.ro) return RES_WRPRT;
        s.dirty = true;
        return rw(s, d, true, const_cast<BYTE *>(buff), sector, count);
    });
}

DRESULT di_ioctl(BYTE pdrv, BYTE cmd, void *buff) {
    Slot *s = slot_for_pdrv(pdrv);
    if (!s) return RES_NOTRDY;
    switch (cmd) {
        case CTRL_SYNC:
            return with_dev(pdrv, [&](Slot &sl, Dev &d) { sync_cache(d, sl.lun); return RES_OK; });
        case GET_SECTOR_COUNT: *(LBA_t *)buff = (LBA_t)s->nsec; return RES_OK;
        case GET_SECTOR_SIZE:  *(WORD *)buff = (WORD)s->ssize; return RES_OK;
        case GET_BLOCK_SIZE:   *(DWORD *)buff = 1; return RES_OK;
        case CTRL_TRIM:        return RES_OK;   // no UNMAP over BOT; harmless to skip
    }
    return RES_PARERR;
}

const ff_diskio_impl_t kDiskio = {di_init, di_status, di_read, di_write, di_ioctl};

// ------------------------------------------------------------------ volumes (worker task only)

void drive_str(const Slot &s, char out[4]) { out[0] = (char)('0' + s.pdrv); out[1] = ':'; out[2] = 0; }

bool ensure_registered(int si) {
    Slot &s = s_slot[si];
    if (s.registered) return true;
    BYTE pdrv = 0xFF;
    if (ff_diskio_get_drive(&pdrv) != ESP_OK || pdrv >= FF_VOLUMES) {
        NV_LOGE(TAG, "no free FATFS drive for /usb%d (CONFIG_FATFS_VOLUME_COUNT)", si);
        return false;
    }
    s.pdrv = pdrv;
    s_pdrv_slot[pdrv] = (int8_t)si;
    ff_diskio_register(pdrv, &kDiskio);
    char path[8], drv[4];
    snprintf(path, sizeof path, "/usb%d", si);
    drive_str(s, drv);
    const esp_vfs_fat_conf_t conf = {.base_path = path, .fat_drive = drv, .max_files = 8};
    if (esp_vfs_fat_register_cfg(&conf, &s.fs) != ESP_OK) {
        ff_diskio_unregister(pdrv);
        s_pdrv_slot[pdrv] = -1;
        NV_LOGE(TAG, "VFS register %s failed", path);
        return false;
    }
    f_mount(s.fs, drv, 0);   // lazy: registers the object + creates the volume mutex, no I/O
    s.registered = true;
    return true;
}

// Forget the mounted state (see lifetime rules). Next FatFs access re-runs mount_volume.
// Only under the volume mutex (waits out a caller mid-operation, whose I/O now fails fast);
// false if it could not be taken — with allow_io off the stale object can't do I/O anyway.
bool fs_invalidate(Slot &s) {
    if (!s.registered) return true;
    if (!ff_mutex_take(s.pdrv)) {
        NV_LOGW(TAG, "/usb%d: volume busy, invalidate deferred", (int)(&s - s_slot));
        return false;
    }
    s.fs->fs_type = 0;
    ff_mutex_give(s.pdrv);
    return true;
}

void fs_detach(Slot &s) {
    s.allow_io = false;
    fs_invalidate(s);
}

void clear_volume_info(Slot &s) {
    lock();
    s.label[0] = 0;
    s.fstype[0] = 0;
    s.total = 0;
    s.freeb = 0;
    unlock();
}

bool with_io(Dev &d, uint32_t ms) { return xSemaphoreTake(d.io, pdMS_TO_TICKS(ms)) == pdTRUE; }

// Medium is ready: read geometry and mount. Leaves MOUNTED / UNFORMATTED / ERROR.
void slot_mount(int si) {
    Slot &s = s_slot[si];
    Dev &d = s_dev[s.dev];
    uint64_t nsec = 0;
    uint32_t ssize = 0;
    bool ro = false, ok = false;
    if (with_io(d, 5000)) {
        ok = read_capacity(d, s.lun, nsec, ssize);
        if (ok) ro = write_protected(d, s.lun);
        xSemaphoreGive(d.io);
    }
    if (!ok || nsec > 0xFFFFFFFFull) {   // diskio sector numbers are 32-bit (2 TB at 512 B)
        if (ok) NV_LOGW(TAG, "/usb%d: medium over 2^32 sectors not supported", si);
        s.retry_at_us = esp_timer_get_time() + 10000000;
        set_state(si, NV_USB_STOR_ERROR);
        return;
    }
    s.nsec = nsec;
    s.ssize = ssize;
    s.ro = ro;
    s.lost = false;
    if (!ensure_registered(si)) { set_state(si, NV_USB_STOR_ERROR); return; }

    char drv[4];
    drive_str(s, drv);
    char lab[24] = {};
    if (!fs_invalidate(s)) {   // never let mount_volume reuse a stale FATFS on a new medium
        s.retry_at_us = esp_timer_get_time() + 2000000;
        set_state(si, NV_USB_STOR_ERROR);
        return;
    }
    s.allow_io = true;
    const FRESULT fr = f_getlabel(drv, lab, nullptr);   // runs mount_volume on the fresh medium
    if (fr != FR_OK) {
        fs_detach(s);   // no lazy auto-mount behind our back
        lock();
        s.total = nsec * ssize;
        s.label[0] = s.fstype[0] = 0;
        unlock();
        NV_LOGW(TAG, "/usb%d: mount failed (FRESULT %d)%s", si, (int)fr,
                fr == FR_NO_FILESYSTEM ? " — no FAT/exFAT volume" : "");
        if (fr == FR_NO_FILESYSTEM) set_state(si, NV_USB_STOR_UNFORMATTED);
        else { s.retry_at_us = esp_timer_get_time() + 10000000; set_state(si, NV_USB_STOR_ERROR); }
        return;
    }
    const char *type = s.fs->fs_type == FS_EXFAT ? "exFAT" : s.fs->fs_type == FS_FAT32 ? "FAT32"
                     : s.fs->fs_type == FS_FAT16 ? "FAT16" : "FAT12";
    lock();
    strlcpy(s.label, lab, sizeof s.label);
    strlcpy(s.fstype, type, sizeof s.fstype);
    s.total = (uint64_t)(s.fs->n_fatent - 2) * s.fs->csize * ssize;
    s.freeb = UINT64_MAX;   // first scan pending
    unlock();
    s.dirty = true;
    NV_LOGI(TAG, "/usb%d mounted: %s \"%s\" %llu MB%s (%s %s)", si, type, lab,
            (unsigned long long)(s.total >> 20), ro ? " read-only" : "", s.vendor, s.product);
    set_state(si, NV_USB_STOR_MOUNTED);
}

void slot_unmount(int si, nv_usb_stor_state_t next) {
    Slot &s = s_slot[si];
    fs_detach(s);
    clear_volume_info(s);
    set_state(si, next);
}

void refresh_free(int si) {
    Slot &s = s_slot[si];
    if (s.state != NV_USB_STOR_MOUNTED || !s.dirty) return;
    if (esp_timer_get_time() - s.last_io_us < 2000000 && s.freeb != UINT64_MAX) return;   // still busy
    s.dirty = false;
    char drv[4];
    drive_str(s, drv);
    DWORD nclst = 0;
    FATFS *fs = nullptr;
    if (f_getfree(drv, &nclst, &fs) == FR_OK && fs) {
        lock();
        s.freeb = (uint64_t)nclst * fs->csize * s.ssize;
        unlock();
        s_gen++;
    }
}

// One presence check for a bound slot.
void poll_slot(int si) {
    Slot &s = s_slot[si];
    Dev &d = s_dev[s.dev];
    if (!s.used || s.ejecting || d.gone) return;
    const int64_t now = esp_timer_get_time();
    if (s.state == NV_USB_STOR_MOUNTED && !s.lost && now - s.last_io_us < kIdleUs) return;
    if (s.state == NV_USB_STOR_ERROR && now < s.retry_at_us) return;
    if (!with_io(d, 0)) return;   // busy with real I/O: that is proof enough of presence
    Ready r = test_ready(d, s.lun);
    // UNIT ATTENTION (medium changed / reset) is reported once: remember it, then ask again for
    // the medium's actual state. Whatever that says, a mounted volume is stale from here on.
    const bool changed = r == R_CHANGED;
    if (changed) {
        r = test_ready(d, s.lun);
        if (r == R_CHANGED) r = R_BECOMING;
    }
    xSemaphoreGive(d.io);
    if (d.gone) return;

    switch (s.state) {
        case NV_USB_STOR_MOUNTED:
            if (s.lost || changed || r == R_NO_MEDIUM) {
                NV_LOGI(TAG, "/usb%d: medium %s", si, r == R_NO_MEDIUM ? "removed" : "changed -> remount");
                slot_unmount(si, NV_USB_STOR_EMPTY);
                if (r == R_READY) slot_mount(si);   // else the next poll mounts it once ready
            }
            break;
        case NV_USB_STOR_EJECTED:
            if (changed) {   // card re-inserted into the reader
                if (r == R_READY) slot_mount(si);
                else set_state(si, NV_USB_STOR_EMPTY);
            }
            break;
        default:   // EMPTY / UNFORMATTED / ERROR
            if (r == R_READY && (s.state != NV_USB_STOR_UNFORMATTED || changed)) {
                slot_mount(si);
            } else if ((r == R_NO_MEDIUM || (changed && r != R_READY)) && s.state != NV_USB_STOR_EMPTY) {
                clear_volume_info(s);
                set_state(si, NV_USB_STOR_EMPTY);
            }
            break;
    }
}

// ------------------------------------------------------------------ attach / detach (worker)

bool parse_msc(const usb_config_desc_t *cd, uint8_t &iface, uint8_t &in, uint8_t &out, uint16_t &mps_in) {
    const uint8_t *p = (const uint8_t *)cd;
    const int total = cd->wTotalLength;
    bool in_msc = false, found = false;
    in = out = 0;
    for (int off = 0; off + 2 <= total;) {
        const uint8_t len = p[off], type = p[off + 1];
        if (len < 2 || off + len > total) break;
        if (type == USB_B_DESCRIPTOR_TYPE_INTERFACE && len >= 9) {
            if (found && in && out) break;   // first complete BOT interface wins
            // class 08 (mass storage), SCSI transparent (06) or legacy subclasses, Bulk-Only (50)
            in_msc = p[off + 5] == 0x08 && p[off + 7] == 0x50 && p[off + 3] == 0;
            if (in_msc) { found = true; iface = p[off + 2]; in = out = 0; }
        } else if (type == USB_B_DESCRIPTOR_TYPE_ENDPOINT && len >= 7 && in_msc) {
            const uint8_t addr = p[off + 2];
            if ((p[off + 3] & 0x03) == 0x02) {   // bulk
                if (addr & 0x80) { in = addr; mps_in = (uint16_t)(p[off + 4] | p[off + 5] << 8); }
                else out = addr;
            }
        }
        off += len;
    }
    return found && in && out;
}

int bind_slot(int di, uint8_t lun, bool rmb, const char *vendor, const char *product) {
    lock();
    int si = -1;
    for (int i = 0; i < kSlots && si < 0; i++) if (!s_slot[i].used) si = i;
    if (si >= 0) {
        Slot &s = s_slot[si];
        s.used = true;
        s.dev = di;
        s.lun = lun;
        s.state = NV_USB_STOR_EMPTY;
        s.removable = rmb;
        s.ro = false;
        s.lost = false;
        s.allow_io = false;
        s.ejecting = false;
        s.sessions = 0;
        s.nsec = 0;
        s.ssize = 512;
        s.last_io_us = 0;
        s.retry_at_us = 0;
        strlcpy(s.vendor, vendor, sizeof s.vendor);
        strlcpy(s.product, product, sizeof s.product);
        s.label[0] = s.fstype[0] = 0;
        s.total = s.freeb = 0;
    }
    unlock();
    return si;
}

void dev_release(Dev &d) {
    if (d.claimed) usb_host_interface_release(s_cl, d.h, d.iface);
    if (d.x && !d.x_leaked) usb_host_transfer_free(d.x);
    if (d.xc && !d.xc_leaked) usb_host_transfer_free(d.xc);
    usb_host_device_close(s_cl, d.h);
    if (d.x_leaked || d.xc_leaked) d.retired = true;   // its late completion must hit no new device
    d.x = d.xc = nullptr;
    d.claimed = false;
    d.used = false;
}

void dev_attach(uint8_t addr) {
    for (auto &d : s_dev) if (d.used && d.addr == addr && !d.gone) return;   // already ours
    usb_device_handle_t h = nullptr;
    if (usb_host_device_open(s_cl, addr, &h) != ESP_OK) return;
    const usb_config_desc_t *cd = nullptr;
    uint8_t iface = 0, ep_in = 0, ep_out = 0;
    uint16_t mps = 512;
    if (usb_host_get_active_config_descriptor(h, &cd) != ESP_OK || !cd ||
        !parse_msc(cd, iface, ep_in, ep_out, mps)) {
        usb_host_device_close(s_cl, h);   // not mass storage (hub, HID, audio...)
        return;
    }
    int di = -1;
    for (int i = 0; i < kMaxDevs && di < 0; i++) if (!s_dev[i].used && !s_dev[i].retired) di = i;
    if (di < 0) {
        NV_LOGW(TAG, "dev %u: too many storage devices (max %d)", addr, kMaxDevs);
        usb_host_device_close(s_cl, h);
        return;
    }
    Dev &d = s_dev[di];
    xSemaphoreTake(d.io, portMAX_DELAY);
    d.used = true;
    d.gone = false;
    d.claimed = false;
    d.xc_leaked = false;
    d.x_leaked = false;
    d.no_sync = false;
    d.addr = addr;
    d.h = h;
    d.iface = iface;
    d.ep_in = ep_in;
    d.ep_out = ep_out;
    d.mps_in = mps ? mps : 512;
    d.tag = 0;
    d.max_lun = 0;
    const usb_device_desc_t *dd = nullptr;
    if (usb_host_get_device_descriptor(h, &dd) == ESP_OK && dd) { d.vid = dd->idVendor; d.pid = dd->idProduct; }
    usb_device_info_t info = {};
    usb_host_device_info(h, &info);
    d.speed = info.speed == USB_SPEED_HIGH ? 2 : info.speed == USB_SPEED_FULL ? 1 : 0;
    while (xSemaphoreTake(d.done, 0) == pdTRUE) {}   // drop a stale give from a previous device

    bool ok = usb_host_interface_claim(s_cl, h, iface, 0) == ESP_OK;
    d.claimed = ok;
    ok = ok && usb_host_transfer_alloc(kChunk, 0, &d.x) == ESP_OK;
    ok = ok && usb_host_transfer_alloc(64, 0, &d.xc) == ESP_OK;
    if (!ok) {
        NV_LOGE(TAG, "dev %u: claim/alloc failed (internal DMA RAM?)", addr);
        dev_release(d);
        xSemaphoreGive(d.io);
        return;
    }
    uint8_t ml = 0;
    if (ctrl(d, 0xA1, 0xFE /*GET MAX LUN*/, 0, iface, 1, &ml) != XF_OK) ml = 0;   // STALL = single LUN
    d.max_lun = ml > 15 ? 0 : ml;
    NV_LOGI(TAG, "dev %u: mass storage %04x:%04x %s-speed, %u LUN(s)", addr, d.vid, d.pid,
            d.speed == 2 ? "high" : d.speed == 1 ? "full" : "low", d.max_lun + 1);

    int bound = 0;
    for (uint8_t lun = 0; lun <= d.max_lun; lun++) {
        uint8_t type = 0;
        bool rmb = false;
        char vendor[9] = {}, product[17] = {};
        bool got = false;
        for (int t = 0; t < 3 && !got && !d.gone; t++) {
            got = inquiry(d, lun, type, rmb, vendor, product);
            if (!got) vTaskDelay(pdMS_TO_TICKS(200));   // some sticks need a moment after reset
        }
        if (!got) continue;
        if (type != 0x00 && type != 0x0E) {           // CD-ROM emulation LUNs etc.
            NV_LOGI(TAG, "dev %u lun %u: peripheral type %02x skipped", addr, lun, type);
            continue;
        }
        const int si = bind_slot(di, lun, rmb, vendor, product);
        if (si < 0) { NV_LOGW(TAG, "no free /usbN slot"); break; }
        NV_LOGI(TAG, "dev %u lun %u -> /usb%d (%s %s%s)", addr, lun, si, vendor, product, rmb ? ", removable" : "");
        publish(si, NV_USB_STOR_EMPTY, false);
        bound++;
    }
    xSemaphoreGive(d.io);
    if (!bound) {
        xSemaphoreTake(d.io, portMAX_DELAY);
        dev_release(d);
        xSemaphoreGive(d.io);
        return;
    }
    // First mount right away instead of waiting a poll period (a stick reports UNIT ATTENTION
    // once, then ready — a few quick polls get it mounted within ~1 s of plugging).
    for (int round = 0; round < 6; round++) {
        bool pending = false;
        for (int i = 0; i < kSlots; i++) {
            if (!s_slot[i].used || s_slot[i].dev != di) continue;
            if (s_slot[i].state == NV_USB_STOR_EMPTY) { poll_slot(i); pending |= s_slot[i].state == NV_USB_STOR_EMPTY; }
        }
        if (!pending || d.gone) break;
        vTaskDelay(pdMS_TO_TICKS(150));
    }
}

void dev_detach(usb_device_handle_t h) {
    for (int di = 0; di < kMaxDevs; di++) {
        Dev &d = s_dev[di];
        if (!d.used || d.h != h) continue;
        d.gone = true;   // diskio callers fail fast from here on
        // Invalidate the volumes BEFORE taking `io`: fs_detach waits for the FatFs volume mutex,
        // whose holder may itself be queued on `io` (it then sees gone/allow_io and bails).
        for (int si = 0; si < kSlots; si++)
            if (s_slot[si].used && s_slot[si].dev == di) fs_detach(s_slot[si]);
        xSemaphoreTake(d.io, portMAX_DELAY);   // in-flight transfers complete with NO_DEVICE
        for (int si = 0; si < kSlots; si++) {
            Slot &s = s_slot[si];
            if (!s.used || s.dev != di) continue;
            const nv_usb_stor_state_t prev = s.state;
            lock();
            s.used = false;
            s.state = NV_USB_STOR_EMPTY;
            s.label[0] = s.fstype[0] = 0;
            s.total = s.freeb = 0;
            unlock();
            NV_LOGI(TAG, "/usb%d: device removed", si);
            publish(si, prev, true);
        }
        dev_release(d);
        xSemaphoreGive(d.io);
        return;
    }
}

bool drain_sessions(int si, int ms) {
    for (int t = 0; t < ms / 50; t++) {
        lock();
        const int n = s_slot[si].sessions;
        unlock();
        if (!n) return true;
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    return false;
}

bool do_eject(int si) {
    Slot &s = s_slot[si];
    if (!s.used) return false;
    if (s.state != NV_USB_STOR_MOUNTED) {
        if (s.state == NV_USB_STOR_EJECTED) return true;
        set_state(si, NV_USB_STOR_EJECTED);   // nothing mounted: just mark it safe
        return true;
    }
    lock();
    s.ejecting = true;
    unlock();
    if (!drain_sessions(si, 3000)) {
        lock();
        s.ejecting = false;
        unlock();
        NV_LOGW(TAG, "/usb%d: eject refused, files still open", si);
        return false;
    }
    fs_detach(s);
    Dev &d = s_dev[s.dev];
    if (with_io(d, 10000)) {
        sync_cache(d, s.lun);
        const uint8_t allow[6] = {0x1E, 0, 0, 0, 0, 0};           // PREVENT ALLOW MEDIUM REMOVAL: allow
        const uint8_t stop[6]  = {0x1B, 0, 0, 0, 0x02, 0};        // START STOP UNIT: LoEj, stop
        bot(d, s.lun, allow, 6, false, nullptr, 0, 2000);
        if (bot(d, s.lun, stop, 6, false, nullptr, 0, 5000) == CMD_FAILED) { Sense se; request_sense(d, s.lun, se); }
        xSemaphoreGive(d.io);
    }
    clear_volume_info(s);
    lock();
    s.ejecting = false;
    unlock();
    NV_LOGI(TAG, "/usb%d ejected", si);
    set_state(si, NV_USB_STOR_EJECTED);
    return true;
}

bool do_format(int si, bool exfat, const char *label) {
    Slot &s = s_slot[si];
    if (!s.used || s.ro || s.state == NV_USB_STOR_EMPTY) return false;
    if (s.state == NV_USB_STOR_MOUNTED) {
        lock();
        s.ejecting = true;
        unlock();
        const bool drained = drain_sessions(si, 3000);
        if (drained) fs_detach(s);
        lock();
        s.ejecting = false;
        unlock();
        if (!drained) return false;
    }
    if (!s.nsec) {   // ERROR/EJECTED without geometry: read it now
        Dev &d = s_dev[s.dev];
        bool ok = false;
        if (with_io(d, 5000)) { ok = read_capacity(d, s.lun, s.nsec, s.ssize); xSemaphoreGive(d.io); }
        if (!ok) return false;
    }
    if (!ensure_registered(si)) return false;
    clear_volume_info(s);
    set_state(si, NV_USB_STOR_EMPTY);   // UI: "working" while mkfs runs

    const uint64_t bytes = s.nsec * s.ssize;
    MKFS_PARM opt = {};
    opt.fmt = exfat ? FM_EXFAT : bytes >= (512ull << 20) ? FM_FAT32 : (FM_FAT | FM_FAT32);
    const size_t work_sz = 64 * 1024;
    void *work = heap_caps_malloc(work_sz, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!work) return false;
    char drv[4];
    drive_str(s, drv);
    if (!fs_invalidate(s)) return false;
    s.allow_io = true;
    NV_LOGI(TAG, "/usb%d: formatting %s (%llu MB)...", si, exfat ? "exFAT" : "FAT", (unsigned long long)(bytes >> 20));
    const FRESULT fr = f_mkfs(drv, &opt, work, work_sz);
    heap_caps_free(work);
    if (fr != FR_OK) {
        fs_detach(s);
        NV_LOGE(TAG, "/usb%d: f_mkfs failed (FRESULT %d)", si, (int)fr);
        set_state(si, NV_USB_STOR_UNFORMATTED);
        return false;
    }
    fs_detach(s);
    slot_mount(si);
    if (s.state == NV_USB_STOR_MOUNTED && label && *label) {
        char lab[16];
        snprintf(lab, sizeof lab, "%s%.11s", drv, label);
        if (f_setlabel(lab) == FR_OK) {
            lock();
            strlcpy(s.label, label, sizeof s.label);
            unlock();
            s_gen++;
        }
    }
    return s.state == NV_USB_STOR_MOUNTED;
}

// ------------------------------------------------------------------ tasks

void client_task(void *) {
    usb_host_client_config_t cfg = {};
    cfg.is_synchronous = false;
    cfg.max_num_event_msg = 8;
    cfg.async.client_event_callback = [](const usb_host_client_event_msg_t *m, void *) {
        Msg msg = {};
        if (m->event == USB_HOST_CLIENT_EVENT_NEW_DEV) { msg.t = M_NEW; msg.addr = m->new_dev.address; }
        else if (m->event == USB_HOST_CLIENT_EVENT_DEV_GONE) { msg.t = M_GONE; msg.h = m->dev_gone.dev_hdl; }
        else return;
        xQueueSend(s_q, &msg, pdMS_TO_TICKS(100));
    };
    // usb_host_install is owned by nv_usb_audio: retry until the library is up.
    while (usb_host_client_register(&cfg, &s_cl) != ESP_OK) vTaskDelay(pdMS_TO_TICKS(500));
    // Devices enumerated before we registered never produce NEW_DEV for us: walk the bus once.
    uint8_t addrs[16];
    int n = 0;
    if (usb_host_device_addr_list_fill(sizeof addrs, addrs, &n) == ESP_OK)
        for (int i = 0; i < n; i++) { Msg msg = {}; msg.t = M_NEW; msg.addr = addrs[i]; xQueueSend(s_q, &msg, 0); }
    NV_LOGI(TAG, "mass storage host ready (/usb0../usb%d, FAT + exFAT)", kSlots - 1);
    for (;;) usb_host_client_handle_events(s_cl, portMAX_DELAY);
}

void worker_task(void *) {
    Msg m;
    int64_t next_poll = 0;
    for (;;) {
        const int64_t now = esp_timer_get_time();
        const TickType_t wait = now >= next_poll ? 0 : pdMS_TO_TICKS((next_poll - now) / 1000 + 1);
        if (xQueueReceive(s_q, &m, wait) == pdTRUE) {
            bool r = false;
            switch (m.t) {
                case M_NEW:    vTaskDelay(pdMS_TO_TICKS(50)); dev_attach(m.addr); break;
                case M_GONE:   dev_detach(m.h); break;
                case M_EJECT:  r = do_eject(m.slot); break;
                case M_FORMAT: r = do_format(m.slot, m.exfat, m.label); break;
            }
            if (m.result) *m.result = r;
            if (m.reply) xSemaphoreGive(m.reply);
            continue;
        }
        next_poll = esp_timer_get_time() + (int64_t)kPollMs * 1000;
        for (int i = 0; i < kSlots; i++) {
            if (!s_slot[i].used) continue;
            poll_slot(i);
            refresh_free(i);
        }
    }
}

bool request(Msg m) {
    if (!s_started || m.slot < 0 || m.slot >= kSlots) return false;
    bool result = false;
    m.reply = xSemaphoreCreateBinary();
    m.result = &result;
    if (!m.reply) return false;
    if (xQueueSend(s_q, &m, pdMS_TO_TICKS(1000)) != pdTRUE) { vSemaphoreDelete(m.reply); return false; }
    xSemaphoreTake(m.reply, portMAX_DELAY);
    vSemaphoreDelete(m.reply);
    return result;
}

void utf16_ascii(const usb_str_desc_t *sd, char *out, size_t cap) {
    out[0] = 0;
    if (!sd || sd->bLength < 2) return;
    const int n = (sd->bLength - 2) / 2;
    size_t k = 0;
    for (int i = 0; i < n && k + 1 < cap; i++) {
        const uint16_t c = sd->wData[i];
        out[k++] = (c >= 0x20 && c < 0x7F) ? (char)c : '?';
    }
    out[k] = 0;
}

}  // namespace

// ------------------------------------------------------------------ public API

bool nv_usb_storage_init(void) {
    if (s_started) return true;
    s_mtx = xSemaphoreCreateMutex();
    s_q = xQueueCreate(12, sizeof(Msg));
    if (!s_mtx || !s_q) return false;
    for (auto &p : s_pdrv_slot) p = -1;
    for (auto &d : s_dev) {
        d.io = xSemaphoreCreateMutex();
        d.done = xSemaphoreCreateBinary();
        if (!d.io || !d.done) return false;
    }
    // Forever daemons that never write internal flash -> PSRAM stacks (internal SRAM is scarce).
    if (xTaskCreateWithCaps(client_task, "usb_stor_cl", 3072, nullptr, 5, nullptr,
                            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) return false;
    if (xTaskCreateWithCaps(worker_task, "usb_stor", 6144, nullptr, 3, nullptr,
                            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) return false;
    s_started = true;
    return true;
}

uint32_t nv_usb_storage_generation(void) { return s_gen.load(); }

static void fill_info(int i, nv_usb_stor_info_t *o) {
    const Slot &s = s_slot[i];
    const Dev &d = s_dev[s.dev];
    memset(o, 0, sizeof *o);
    o->used = s.used;
    o->state = s.state;
    snprintf(o->path, sizeof o->path, "/usb%d", i);
    strlcpy(o->label, s.label, sizeof o->label);
    strlcpy(o->fs, s.fstype, sizeof o->fs);
    strlcpy(o->vendor, s.vendor, sizeof o->vendor);
    strlcpy(o->product, s.product, sizeof o->product);
    o->total_bytes = s.total;
    o->free_bytes = s.freeb;
    o->read_only = s.ro;
    o->removable = s.removable;
    o->vid = d.vid;
    o->pid = d.pid;
    o->addr = d.addr;
    o->lun = s.lun;
    o->speed = d.speed;
}

int nv_usb_storage_list(nv_usb_stor_info_t *out, int max) {
    if (!s_started) return 0;
    int n = 0;
    lock();
    for (int i = 0; i < kSlots && n < max; i++) if (s_slot[i].used) fill_info(i, &out[n++]);
    unlock();
    return n;
}

bool nv_usb_storage_get(int slot, nv_usb_stor_info_t *out) {
    if (!s_started || slot < 0 || slot >= kSlots) return false;
    lock();
    const bool used = s_slot[slot].used;
    if (used) fill_info(slot, out);
    unlock();
    return used;
}

int nv_usb_storage_mounted_count(void) {
    if (!s_started) return 0;
    int n = 0;
    lock();
    for (auto &s : s_slot) n += s.used && s.state == NV_USB_STOR_MOUNTED;
    unlock();
    return n;
}

int nv_usb_storage_slot_of(const char *path) {
    if (!path || strncmp(path, "/usb", 4) != 0) return -1;
    const char c = path[4];
    if (c < '0' || c >= '0' + kSlots) return -1;
    if (path[5] != 0 && path[5] != '/') return -1;
    return c - '0';
}

bool nv_usb_storage_eject(int slot) {
    Msg m = {};
    m.t = M_EJECT;
    m.slot = slot;
    return request(m);
}

bool nv_usb_storage_format(int slot, bool exfat, const char *label) {
    Msg m = {};
    m.t = M_FORMAT;
    m.slot = slot;
    m.exfat = exfat;
    if (label) strlcpy(m.label, label, sizeof m.label);
    return request(m);
}

bool nv_usb_storage_session_begin(int slot) {
    if (!s_started || slot < 0 || slot >= kSlots) return false;
    lock();
    Slot &s = s_slot[slot];
    const bool ok = s.used && s.state == NV_USB_STOR_MOUNTED && !s.ejecting && !s.lost;
    if (ok) s.sessions++;
    unlock();
    return ok;
}

void nv_usb_storage_session_end(int slot) {
    if (!s_started || slot < 0 || slot >= kSlots) return;
    lock();
    if (s_slot[slot].sessions > 0) s_slot[slot].sessions--;
    unlock();
}

FILE *nv_usb_storage_fopen(const char *path, const char *mode) {
    const int slot = nv_usb_storage_slot_of(path);
    if (slot < 0 || !nv_usb_storage_session_begin(slot)) { errno = ENODEV; return nullptr; }
    FILE *f = fopen(path, mode);
    if (!f) { nv_usb_storage_session_end(slot); return nullptr; }
    lock();
    for (auto &t : s_files) if (!t.f) { t.f = f; t.slot = slot; unlock(); return f; }
    unlock();
    fclose(f);   // table full: refuse rather than lose track of the session
    nv_usb_storage_session_end(slot);
    errno = EMFILE;
    return nullptr;
}

bool nv_usb_storage_fclose(FILE *f, int *ret) {
    if (!f || !s_started) return false;
    int slot = -1;
    lock();
    for (auto &t : s_files) if (t.f == f) { slot = t.slot; t.f = nullptr; break; }
    unlock();
    if (slot < 0) return false;
    const int r = fclose(f);
    if (ret) *ret = r;
    nv_usb_storage_session_end(slot);
    return true;
}

int nv_usb_bus_list(nv_usb_bus_dev_t *out, int max) {
    if (!s_started || !s_cl) return 0;
    uint8_t addrs[16];
    usb_device_handle_t hs[16] = {};
    int n = 0, k = 0;
    if (usb_host_device_addr_list_fill(sizeof addrs, addrs, &n) != ESP_OK) return 0;
    // Open everything first: a device's parent handle is then compared against handles we hold
    // (never dereferenced), and every opened device stays valid until the final close loop.
    for (int i = 0; i < n; i++)
        if (usb_host_device_open(s_cl, addrs[i], &hs[i]) != ESP_OK) hs[i] = nullptr;
    for (int i = 0; i < n && k < max; i++) {
        if (!hs[i]) continue;
        nv_usb_bus_dev_t &o = out[k];
        memset(&o, 0, sizeof o);
        memset(o.if_classes, 0xFF, sizeof o.if_classes);
        o.addr = addrs[i];
        usb_device_info_t info = {};
        if (usb_host_device_info(hs[i], &info) == ESP_OK) {
            o.speed = info.speed == USB_SPEED_HIGH ? 2 : info.speed == USB_SPEED_FULL ? 1 : 0;
            o.port = info.parent.port_num;
            if (info.parent.dev_hdl) {
                o.parent_addr = 0xFF;   // behind a hub whose address we could not resolve
                for (int j = 0; j < n; j++)
                    if (hs[j] && hs[j] == info.parent.dev_hdl) { o.parent_addr = addrs[j]; break; }
            }
            utf16_ascii(info.str_desc_manufacturer, o.manufacturer, sizeof o.manufacturer);
            utf16_ascii(info.str_desc_product, o.product, sizeof o.product);
        }
        const usb_device_desc_t *dd = nullptr;
        if (usb_host_get_device_descriptor(hs[i], &dd) == ESP_OK && dd) {
            o.vid = dd->idVendor;
            o.pid = dd->idProduct;
            o.dev_class = dd->bDeviceClass;
        }
        const usb_config_desc_t *cd = nullptr;
        if (usb_host_get_active_config_descriptor(hs[i], &cd) == ESP_OK && cd) {
            const uint8_t *p = (const uint8_t *)cd;
            int nc = 0;
            for (int off = 0; off + 2 <= cd->wTotalLength && nc < (int)sizeof o.if_classes;) {
                const uint8_t len = p[off];
                if (len < 2) break;
                if (p[off + 1] == USB_B_DESCRIPTOR_TYPE_INTERFACE && len >= 9 && p[off + 3] == 0)
                    o.if_classes[nc++] = p[off + 5];
                off += len;
            }
        }
        k++;
    }
    for (int i = 0; i < n; i++) if (hs[i]) usb_host_device_close(s_cl, hs[i]);
    return k;
}
