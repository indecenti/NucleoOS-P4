// nv_usb MSC — a small read-only USB drive ("NUCLEOOS") that appears next to the extend-screen
// interfaces, so plugging the cable is enough to find the Windows driver installer and the
// no-install sender page. The FAT16 volume is VIRTUAL: boot sector, FATs, root directory (with
// long file names) and data sectors are synthesized on every read from a list of files published
// by the Second Screen module (nv_usb_drive_publish). Nothing is stored; file bytes come from
// memory or from a read callback (e.g. the driver .exe on the SD card).
#include <string.h>
#include <stdlib.h>
#include "tusb.h"
#include "tusb_config.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nv_usb.h"
#include "nv_log.h"

#if CFG_TUD_MSC

static const char *TAG = "nv_usb_msc";

// ---- geometry (FAT16: >= 4085 clusters)
#define SECT            512u
#define TOTAL_SECTORS   65536u               // 32 MB volume
#define SPC             8u                   // 4 KB clusters
#define RESERVED        1u
#define NUM_FATS        2u
#define FAT_SECTORS     32u                  // 8192 entries * 2 B
#define ROOT_ENTRIES    512u
#define ROOT_SECTORS    (ROOT_ENTRIES * 32u / SECT)
#define FAT_START       RESERVED
#define ROOT_START      (RESERVED + NUM_FATS * FAT_SECTORS)
#define DATA_START      (ROOT_START + ROOT_SECTORS)
#define CLUSTERS        ((TOTAL_SECTORS - DATA_START) / SPC)
#define CLUSTER_BYTES   (SPC * SECT)
#define MAX_FILES       NV_USB_DRIVE_MAX_FILES

typedef struct {
    nv_usb_file_t f;
    uint32_t first;     // first cluster (>= 2), 0 when empty
    uint32_t nclust;
} vfile_t;

static SemaphoreHandle_t s_lk = NULL;
static vfile_t s_files[MAX_FILES];
static int s_nfiles = 0;
static uint8_t *s_root = NULL;      // ROOT_SECTORS * SECT, built on publish
static bool s_ready = false;
static bool s_changed = false;      // report "medium changed" once after a re-publish

// ---- directory helpers
static void put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void put32(uint8_t *p, uint32_t v) { for (int i = 0; i < 4; i++) p[i] = (uint8_t)(v >> (8 * i)); }

static uint8_t lfn_checksum(const uint8_t *short11) {
    uint8_t sum = 0;
    for (int i = 0; i < 11; i++) sum = (uint8_t)(((sum & 1) << 7) + (sum >> 1) + short11[i]);
    return sum;
}

// FAT date/time for every entry: 2026-01-01 12:00.
#define FAT_DATE (uint16_t)(((2026 - 1980) << 9) | (1 << 5) | 1)
#define FAT_TIME (uint16_t)(12 << 11)

static int add_lfn(uint8_t *dir, int slot, const char *name, const uint8_t *short11) {
    const int len = (int)strlen(name);
    const int n = (len + 12) / 13;
    const uint8_t ck = lfn_checksum(short11);
    static const uint8_t pos[13] = {1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30};
    for (int k = n; k >= 1; k--) {   // stored last part first
        uint8_t *e = dir + (size_t)slot++ * 32;
        memset(e, 0, 32);
        e[0] = (uint8_t)(k | (k == n ? 0x40 : 0));
        e[11] = 0x0F;
        e[13] = ck;
        for (int i = 0; i < 13; i++) {
            const int ci = (k - 1) * 13 + i;
            uint16_t ch = ci < len ? (uint8_t)name[ci] : (ci == len ? 0x0000 : 0xFFFF);
            put16(e + pos[i], ch);
        }
    }
    return slot;
}

static void build_root(void) {
    memset(s_root, 0, ROOT_SECTORS * SECT);
    int slot = 0;
    uint8_t *v = s_root + (size_t)slot++ * 32;   // volume label
    memcpy(v, "NUCLEOOS   ", 11);
    v[11] = 0x08;
    put16(v + 22, FAT_TIME); put16(v + 24, FAT_DATE);
    for (int i = 0; i < s_nfiles; i++) {
        const vfile_t *f = &s_files[i];
        uint8_t short11[11];
        memcpy(short11, f->f.short83, 11);
        slot = add_lfn(s_root, slot, f->f.name, short11);
        uint8_t *e = s_root + (size_t)slot++ * 32;
        memcpy(e, short11, 11);
        e[11] = 0x01;   // read-only
        put16(e + 14, FAT_TIME); put16(e + 16, FAT_DATE); put16(e + 18, FAT_DATE);
        put16(e + 22, FAT_TIME); put16(e + 24, FAT_DATE);
        put16(e + 26, (uint16_t)f->first);
        put32(e + 28, f->f.size);
    }
}

// ---- sector synthesis
static void boot_sector(uint8_t *b) {
    memset(b, 0, SECT);
    static const uint8_t jmp[3] = {0xEB, 0x3C, 0x90};
    memcpy(b, jmp, 3);
    memcpy(b + 3, "NUCLEOOS", 8);
    put16(b + 11, SECT);
    b[13] = SPC;
    put16(b + 14, RESERVED);
    b[16] = NUM_FATS;
    put16(b + 17, ROOT_ENTRIES);
    put16(b + 19, 0);               // total sectors in the 32-bit field
    b[21] = 0xF8;
    put16(b + 22, FAT_SECTORS);
    put16(b + 24, 63);
    put16(b + 26, 255);
    put32(b + 28, 0);
    put32(b + 32, TOTAL_SECTORS);
    b[36] = 0x80;
    b[38] = 0x29;
    put32(b + 39, 0x4E56324Du);     // volume serial "NV2M"
    memcpy(b + 43, "NUCLEOOS   ", 11);
    memcpy(b + 54, "FAT16   ", 8);
    b[510] = 0x55;
    b[511] = 0xAA;
}

static uint16_t fat_entry(uint32_t c) {
    if (c == 0) return 0xFFF8;
    if (c == 1) return 0xFFFF;
    for (int i = 0; i < s_nfiles; i++) {
        const vfile_t *f = &s_files[i];
        if (f->nclust && c >= f->first && c < f->first + f->nclust)
            return (c == f->first + f->nclust - 1) ? 0xFFFF : (uint16_t)(c + 1);
    }
    return 0;
}

static void fat_sector(uint32_t idx, uint8_t *b) {
    const uint32_t e0 = idx * (SECT / 2);
    for (uint32_t i = 0; i < SECT / 2; i++) put16(b + i * 2, fat_entry(e0 + i));
}

static void data_sector(uint32_t lba, uint8_t *b) {
    memset(b, 0, SECT);
    const uint32_t rel = lba - DATA_START;
    const uint32_t c = rel / SPC + 2;
    for (int i = 0; i < s_nfiles; i++) {
        const vfile_t *f = &s_files[i];
        if (!f->nclust || c < f->first || c >= f->first + f->nclust) continue;
        const uint32_t off = (c - f->first) * CLUSTER_BYTES + (rel % SPC) * SECT;
        if (off >= f->f.size) return;
        uint32_t n = f->f.size - off;
        if (n > SECT) n = SECT;
        if (f->f.data) memcpy(b, f->f.data + off, n);
        else if (f->f.read) f->f.read(f->f.ctx, off, b, n);
        return;
    }
}

static void read_sector(uint32_t lba, uint8_t *b) {
    if (lba == 0) boot_sector(b);
    else if (lba < FAT_START + NUM_FATS * FAT_SECTORS) fat_sector((lba - FAT_START) % FAT_SECTORS, b);
    else if (lba < DATA_START) memcpy(b, s_root + (size_t)(lba - ROOT_START) * SECT, SECT);
    else data_sector(lba, b);
}

// ---------------------------------------------------------------- public API
bool nv_usb_drive_publish(const nv_usb_file_t *files, int n) {
    if (!s_lk) s_lk = xSemaphoreCreateMutex();
    if (!s_lk) return false;
    if (!s_root) s_root = (uint8_t *)heap_caps_calloc(1, ROOT_SECTORS * SECT, MALLOC_CAP_SPIRAM);
    if (!s_root) return false;
    xSemaphoreTake(s_lk, portMAX_DELAY);
    s_nfiles = 0;
    uint32_t next = 2;
    for (int i = 0; i < n && s_nfiles < MAX_FILES; i++) {
        vfile_t *f = &s_files[s_nfiles];
        f->f = files[i];
        f->nclust = (f->f.size + CLUSTER_BYTES - 1) / CLUSTER_BYTES;
        if (next + f->nclust > CLUSTERS + 2) break;   // doesn't fit: skip the rest
        f->first = f->nclust ? next : 0;
        next += f->nclust;
        s_nfiles++;
    }
    build_root();
    if (s_ready) s_changed = true;
    s_ready = true;
    xSemaphoreGive(s_lk);
    NV_LOGI(TAG, "drive: %d files, %lu KB", s_nfiles, (unsigned long)((next - 2) * CLUSTER_BYTES / 1024));
    return true;
}

// ---------------------------------------------------------------- TinyUSB MSC callbacks
void tud_msc_inquiry_cb(uint8_t lun, uint8_t vendor_id[8], uint8_t product_id[16], uint8_t product_rev[4]) {
    (void)lun;
    memcpy(vendor_id, "NucleoOS", 8);
    memcpy(product_id, "Second Screen   ", 16);
    memcpy(product_rev, "1.0 ", 4);
}

bool tud_msc_test_unit_ready_cb(uint8_t lun) {
    (void)lun;
    if (!s_ready) {
        tud_msc_set_sense(lun, SCSI_SENSE_NOT_READY, 0x3A, 0x00);   // medium not present
        return false;
    }
    if (s_changed) {   // new content: make the host drop its cache
        s_changed = false;
        tud_msc_set_sense(lun, SCSI_SENSE_UNIT_ATTENTION, 0x28, 0x00);
        return false;
    }
    return true;
}

void tud_msc_capacity_cb(uint8_t lun, uint32_t *block_count, uint16_t *block_size) {
    (void)lun;
    *block_count = TOTAL_SECTORS;
    *block_size = SECT;
}

bool tud_msc_start_stop_cb(uint8_t lun, uint8_t power_condition, bool start, bool load_eject) {
    (void)lun; (void)power_condition; (void)start; (void)load_eject;
    return true;
}

bool tud_msc_is_writable_cb(uint8_t lun) { (void)lun; return false; }

int32_t tud_msc_read10_cb(uint8_t lun, uint32_t lba, uint32_t offset, void *buffer, uint32_t bufsize) {
    (void)lun;
    if (!s_ready || !s_lk) return -1;
    uint8_t *out = (uint8_t *)buffer;
    uint32_t done = 0;
    static uint8_t sec[SECT];
    xSemaphoreTake(s_lk, portMAX_DELAY);
    while (done < bufsize) {
        const uint32_t abs_off = offset + done;
        const uint32_t s = lba + abs_off / SECT, in = abs_off % SECT;
        if (s >= TOTAL_SECTORS) break;
        read_sector(s, sec);
        uint32_t n = SECT - in;
        if (n > bufsize - done) n = bufsize - done;
        memcpy(out + done, sec + in, n);
        done += n;
    }
    xSemaphoreGive(s_lk);
    return (int32_t)done;
}

int32_t tud_msc_write10_cb(uint8_t lun, uint32_t lba, uint32_t offset, uint8_t *buffer, uint32_t bufsize) {
    (void)lba; (void)offset; (void)buffer; (void)bufsize;
    tud_msc_set_sense(lun, SCSI_SENSE_DATA_PROTECT, 0x27, 0x00);    // write protected
    return -1;
}

int32_t tud_msc_scsi_cb(uint8_t lun, uint8_t const scsi_cmd[16], void *buffer, uint16_t bufsize) {
    (void)buffer; (void)bufsize;
    switch (scsi_cmd[0]) {
    case 0x1E:   // PREVENT ALLOW MEDIUM REMOVAL
        return 0;
    default:
        tud_msc_set_sense(lun, SCSI_SENSE_ILLEGAL_REQUEST, 0x20, 0x00);
        return -1;
    }
}

#else   // !CFG_TUD_MSC

bool nv_usb_drive_publish(const nv_usb_file_t *files, int n) { (void)files; (void)n; return false; }

#endif
