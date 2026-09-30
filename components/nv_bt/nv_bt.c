// nv_bt — Bluetooth LE game controllers: NimBLE host on the P4, controller on the C6 (HCI over the
// esp_hosted SDIO link, VHCI), HID-over-GATT client feeding nv_pad.
//
// The HOGP discovery flow is adapted from ESP-IDF components/esp_hid/src/nimble_hidh.c and
// examples/bluetooth/nimble/common/nimble_central_utils/peer.c (Copyright Espressif Systems
// (Shanghai) CO LTD, Apache-2.0); hci_rx_handler below is adapted from esp_hosted
// host/drivers/bt/vhci_drv.c (same copyright, Apache-2.0).
//
// Threads:
//   - API callers (LVGL, httpd) only touch the s_lock-protected mirrors and post commands.
//   - "nv_bt" control task (internal stack, lives only during on/off transitions): esp_hosted RPCs
//     and nimble_port_init/stop/deinit block, so they never run on a UI thread.
//   - NimBLE host task (internal stack: it writes bonds to NVS): every GAP/GATT step is an async
//     state machine in its callbacks — nothing in there waits.
//   - nv_bgwork: pad mapping lookup (may read /sdcard/data/pads.txt), off the host task.
#include "nv_bt.h"

#include <stdio.h>
#include <string.h>

#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"

#include "nv_bgwork.h"
#include "nv_config.h"
#include "nv_hid_gamepad.h"
#include "nv_log.h"
#include "nv_pad.h"

static const char *TAG = "bt";

// ---- shared helpers (both backends) ------------------------------------------------------------

void nv_bt_addr_str(const uint8_t a[6], char out[18]) {
    snprintf(out, 18, "%02x:%02x:%02x:%02x:%02x:%02x", a[5], a[4], a[3], a[2], a[1], a[0]);
}

bool nv_bt_addr_parse(const char *s, uint8_t a[6]) {
    unsigned v[6];
    char tail;
    if (!s || sscanf(s, "%2x:%2x:%2x:%2x:%2x:%2x%c", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &tail) != 6)
        return false;
    for (int i = 0; i < 6; i++) a[5 - i] = (uint8_t)v[i];
    return true;
}

#if defined(CONFIG_BT_NIMBLE_ENABLED) && defined(CONFIG_ESP_HOSTED_ENABLE_BT_NIMBLE)

#include "esp_hosted.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "nimble/hci_common.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "nimble/transport.h"
#include "nimble/transport/hci_h4.h"
#include "os/os_mbuf.h"

void ble_store_config_init(void);   // no public header (same as the IDF examples)

#define SCAN_MAX   24
#define BONDS_MAX  (CONFIG_BT_NIMBLE_MAX_BONDS < 8 ? CONFIG_BT_NIMBLE_MAX_BONDS : 8)
#define PEER_MAX   (CONFIG_BT_NIMBLE_MAX_CONNECTIONS < NV_PAD_MAX ? CONFIG_BT_NIMBLE_MAX_CONNECTIONS : NV_PAD_MAX)
#define RPT_MAX    16
#define CHR_MAX    24
#define MAP_CAP    1536        // HID report maps are a few hundred bytes (Xbox ~300)
#define DONE_MAX   8
#define FORGET_MAX 4
#define SETUP_TIMEOUT_US (30LL * 1000 * 1000)
#define NAMES_CAP  (BONDS_MAX * 48 + 1)

// ---- UUIDs -------------------------------------------------------------------------------------
enum {
    UUID_GAP_NAME = 0x2A00, UUID_HID_SVC = 0x1812, UUID_BAS_SVC = 0x180F, UUID_BATT_LEVEL = 0x2A19,
    UUID_PNP_ID = 0x2A50, UUID_REPORT_MAP = 0x2A4B, UUID_REPORT = 0x2A4D, UUID_PROTO_MODE = 0x2A4E,
    UUID_CCCD = 0x2902, UUID_REPORT_REF = 0x2908,
};

// ---- per-connection state (host task only) ----------------------------------------------------
typedef struct {
    uint16_t val, cccd, ref;
    uint8_t  props, id, type;          // type: 1 input, 2 output, 3 feature (Report Reference)
} rpt_t;

enum {   // setup steps, in order
    ST_FREE = 0, ST_SEC, ST_MTU, ST_SVCS, ST_HID_CHRS, ST_HID_DSCS, ST_RPT_REFS, ST_MAP, ST_PROTO,
    ST_PNP, ST_NAME, ST_BAS_CHRS, ST_BAS_DSCS, ST_BATT, ST_SUBS, ST_MAPPING, ST_READY, ST_DEAD,
};

typedef struct {
    uint8_t  st, idx;
    bool     sec_retry, map_pending, is_pad;
    uint16_t conn;
    uint32_t gen;
    ble_addr_t addr;                   // identity address (after pairing / resolution)
    int64_t  deadline;
    uint16_t hid_s, hid_e, bas_s, bas_e;
    uint16_t map_h, proto_h, batt_h, batt_cccd, rumble_h;
    uint8_t  batt_props, battery;
    uint8_t  n_rpt, n_chr, pad_rpt;
    rpt_t    rpt[RPT_MAX];
    uint16_t chr_val[CHR_MAX];         // every HID characteristic value handle (descriptor owner lookup)
    uint8_t *map;
    uint16_t map_len;
    uint16_t vid, pid;
    char     name[32];
    nv_hid_pad_layout_t layout;
    nv_pad_map_t pmap;
    int      slot;
    uint16_t rum_lo, rum_hi;           // written by the rumble hook under s_lock
    bool     rum_dirty;
} peer_t;

typedef struct {
    uint8_t addr[6];
    uint8_t type;
    char    name[32];
} bond_t;

typedef struct {                       // nv_bgwork mapping job
    uint16_t conn;
    uint32_t gen;
    uint16_t vid, pid;
    bool     db;
    nv_hid_pad_layout_t layout;
    nv_pad_map_t map;
} map_job_t;

typedef enum { OP_NONE = 0, OP_SCAN, OP_CONN, OP_WL, OP_CANCEL } gap_op_t;
typedef enum { PH_OFF = 0, PH_STARTING, PH_RUNNING, PH_ERROR } phase_t;

enum { CMD_SCAN = 1, CMD_SCAN_STOP = 2, CMD_CONNECT = 4, CMD_FORGET = 8, CMD_RUMBLE = 16, CMD_DONE = 32 };

// ---- shared state (s_lock) ---------------------------------------------------------------------
static SemaphoreHandle_t s_lock;
static bool     s_inited, s_enabled, s_ctl_busy, s_start_failed;
static bool     s_host_started;        // control task only
static bool     s_running;             // host up: commands may be posted
static volatile bool s_synced;
static phase_t  s_phase;
static char     s_error[64], s_busy[32];
static bool     s_m_scanning, s_m_connecting;
static uint8_t  s_m_npads;
static ble_addr_t s_m_conn[PEER_MAX];  // connected identity addresses (nv_bt_paired)
static uint8_t  s_m_nconn;
static nv_bt_device_t *s_scan;         // PSRAM
static int      s_nscan;
static bond_t   s_bonds[BONDS_MAX];    // bonded devices + their names ("bt_names")
static int      s_nbonds;
static uint32_t s_cmd_bits;
static int      s_cmd_scan_secs;
static ble_addr_t s_cmd_conn_addr;
static ble_addr_t s_cmd_forget[FORGET_MAX];
static int      s_ncmd_forget;
static map_job_t *s_done[DONE_MAX];
static int      s_ndone;

// ---- host task state ---------------------------------------------------------------------------
static peer_t  *s_peers;               // PSRAM, PEER_MAX
static uint8_t  s_own_addr_type;
static gap_op_t s_op;
static bool     s_scan_want, s_conn_want, s_wl_dirty;
static int      s_scan_secs;
static ble_addr_t s_conn_addr;
static uint32_t s_gen;
static struct ble_npl_event s_cmd_ev;
static struct ble_npl_callout s_tick;
static volatile bool s_hci_live;       // NimBLE transport pools exist (hci_rx_handler may feed them)
static volatile int  s_hci_inflight;

static bool lock_ms(uint32_t ms) { return s_lock && xSemaphoreTake(s_lock, pdMS_TO_TICKS(ms)) == pdTRUE; }
static void unlock(void) { xSemaphoreGive(s_lock); }

static void set_error(const char *msg) {
    if (!lock_ms(200)) return;
    snprintf(s_error, sizeof s_error, "%s", msg ? msg : "");
    unlock();
}

static void set_busy(const char *name) {
    if (!lock_ms(200)) return;
    snprintf(s_busy, sizeof s_busy, "%s", name ? name : "");
    unlock();
}

static bool addr_eq(const uint8_t a[6], uint8_t at, const ble_addr_t *b) {
    return at == b->type && memcmp(a, b->val, 6) == 0;
}

// ---- bond names (nv_config "bt_names": "TTAABBCCDDEEFFName;" per bond) -------------------------

static int hexval(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static void bonds_load(void) {
    char *buf = heap_caps_malloc(NAMES_CAP, MALLOC_CAP_SPIRAM);
    if (!buf) return;
    nv_config_get_str("bt_names", "", buf, NAMES_CAP);
    s_nbonds = 0;
    for (char *e = buf; *e && s_nbonds < BONDS_MAX;) {
        char *end = strchr(e, ';');
        if (!end) break;
        *end = 0;
        uint8_t raw[7];
        bool ok = strlen(e) >= 14;
        for (int i = 0; ok && i < 7; i++) {
            const int h = hexval(e[2 * i]), l = hexval(e[2 * i + 1]);
            if (h < 0 || l < 0) ok = false; else raw[i] = (uint8_t)(h << 4 | l);
        }
        if (ok) {
            bond_t *b = &s_bonds[s_nbonds++];
            b->type = raw[0];
            memcpy(b->addr, raw + 1, 6);
            snprintf(b->name, sizeof b->name, "%s", e + 14);
        }
        e = end + 1;
    }
    heap_caps_free(buf);
}

// Serialize under s_lock into a fresh PSRAM buffer; the caller saves it outside the lock.
static char *bonds_serialize_locked(void) {
    char *buf = heap_caps_malloc(NAMES_CAP, MALLOC_CAP_SPIRAM);
    if (!buf) return NULL;
    size_t n = 0;
    buf[0] = 0;
    for (int i = 0; i < s_nbonds; i++) {
        const bond_t *b = &s_bonds[i];
        n += snprintf(buf + n, NAMES_CAP - n, "%02x%02x%02x%02x%02x%02x%02x%s;", b->type, b->addr[0],
                      b->addr[1], b->addr[2], b->addr[3], b->addr[4], b->addr[5], b->name);
        if (n >= NAMES_CAP) { buf[NAMES_CAP - 1] = 0; break; }
    }
    return buf;
}

static void bonds_save(char *buf) {
    if (!buf) return;
    nv_config_set_str("bt_names", buf);
    heap_caps_free(buf);
}

static void clean_name(char *dst, size_t cap, const char *src, size_t len) {
    size_t n = 0;
    for (size_t i = 0; i < len && n + 1 < cap; i++) {
        const char c = src[i];
        if (!c) break;
        dst[n++] = (c == ';' || (unsigned char)c < 0x20) ? ' ' : c;
    }
    dst[n] = 0;
}

// Add or rename a bond (host task, after pairing / after reading the GAP name).
static void bonds_add(const ble_addr_t *a, const char *name) {
    char *out = NULL;
    if (!lock_ms(200)) return;
    int i = 0;
    while (i < s_nbonds && !addr_eq(s_bonds[i].addr, s_bonds[i].type, a)) i++;
    const bool known = i < s_nbonds;
    if (!known) {
        if (s_nbonds == BONDS_MAX) {           // NimBLE's store dropped its oldest bond too (status_rr)
            memmove(&s_bonds[0], &s_bonds[1], sizeof(bond_t) * (BONDS_MAX - 1));
            s_nbonds--;
        }
        i = s_nbonds++;
        s_bonds[i].type = a->type;
        memcpy(s_bonds[i].addr, a->val, 6);
        s_bonds[i].name[0] = 0;
    }
    if (!known || strcmp(s_bonds[i].name, name)) {
        snprintf(s_bonds[i].name, sizeof s_bonds[i].name, "%s", name);
        out = bonds_serialize_locked();
    }
    unlock();
    if (out) { bonds_save(out); s_wl_dirty = true; }
}

static bool bonds_remove_locked(const ble_addr_t *a) {
    for (int i = 0; i < s_nbonds; i++) {
        if (!addr_eq(s_bonds[i].addr, s_bonds[i].type, a)) continue;
        memmove(&s_bonds[i], &s_bonds[i + 1], sizeof(bond_t) * (s_nbonds - i - 1));
        s_nbonds--;
        return true;
    }
    return false;
}

static void bonds_name_of(const ble_addr_t *a, char *out, size_t cap) {
    out[0] = 0;
    if (!lock_ms(200)) return;
    for (int i = 0; i < s_nbonds; i++)
        if (addr_eq(s_bonds[i].addr, s_bonds[i].type, a)) { snprintf(out, cap, "%s", s_bonds[i].name); break; }
    if (!out[0])
        for (int i = 0; i < s_nscan; i++)
            if (addr_eq(s_scan[i].addr, s_scan[i].addr_type, a)) { snprintf(out, cap, "%s", s_scan[i].name); break; }
    unlock();
}

// The names list is authoritative: store bonds without a name entry were forgotten while Bluetooth
// was off (delete them); names whose bond NimBLE dropped go away.
static void bonds_reconcile(void) {
    ble_addr_t ids[BONDS_MAX + 2];
    int n = 0;
    if (ble_store_util_bonded_peers(ids, &n, BONDS_MAX + 2) != 0) return;
    char *out = NULL;
    if (!lock_ms(200)) return;
    bool changed = false;
    for (int i = 0; i < s_nbonds;) {
        bool found = false;
        for (int k = 0; k < n && !found; k++) found = addr_eq(s_bonds[i].addr, s_bonds[i].type, &ids[k]);
        if (found) { i++; continue; }
        memmove(&s_bonds[i], &s_bonds[i + 1], sizeof(bond_t) * (s_nbonds - i - 1));
        s_nbonds--;
        changed = true;
    }
    bool stale[BONDS_MAX + 2] = {0};
    for (int k = 0; k < n; k++) {
        stale[k] = true;
        for (int i = 0; i < s_nbonds; i++)
            if (addr_eq(s_bonds[i].addr, s_bonds[i].type, &ids[k])) stale[k] = false;
    }
    if (changed) out = bonds_serialize_locked();
    unlock();
    for (int k = 0; k < n; k++)
        if (stale[k]) ble_store_util_delete_peer(&ids[k]);
    if (out) bonds_save(out);
    s_wl_dirty = true;
}

// ---- mirrors for the UI ------------------------------------------------------------------------

static void mirror_update(void) {
    uint8_t npads = 0, nconn = 0;
    ble_addr_t conn[PEER_MAX];
    const char *busy = NULL;
    for (int i = 0; i < PEER_MAX; i++) {
        const peer_t *p = &s_peers[i];
        if (p->st == ST_FREE || p->st == ST_DEAD) continue;
        conn[nconn++] = p->addr;
        if (p->st == ST_READY) { if (p->slot >= 0) npads++; }
        else if (!busy) busy = p->name;
    }
    if (!lock_ms(200)) return;
    s_m_npads = npads;
    s_m_nconn = nconn;
    memcpy(s_m_conn, conn, sizeof(ble_addr_t) * nconn);
    // A command still queued keeps what the API call already showed.
    s_m_scanning = s_op == OP_SCAN || (s_cmd_bits & CMD_SCAN);
    s_m_connecting = s_op == OP_CONN || busy || (s_cmd_bits & CMD_CONNECT);
    if (busy) snprintf(s_busy, sizeof s_busy, "%s", busy);
    else if (s_op != OP_CONN && !(s_cmd_bits & CMD_CONNECT)) s_busy[0] = 0;
    unlock();
}

// ---- GAP master procedure arbitration (host task) ----------------------------------------------

static int gap_event(struct ble_gap_event *ev, void *arg);

static const struct ble_gap_conn_params kConnFast = {   // user-initiated connect: 50% scan duty
    .scan_itvl = 0x0060, .scan_window = 0x0030, .itvl_min = 6, .itvl_max = 12,
    .latency = 0, .supervision_timeout = 300, .min_ce_len = 0, .max_ce_len = 0,
};
static const struct ble_gap_conn_params kConnBackground = {   // auto-reconnect: 100 ms every 1 s
    .scan_itvl = 1600, .scan_window = 160, .itvl_min = 6, .itvl_max = 12,
    .latency = 0, .supervision_timeout = 300, .min_ce_len = 0, .max_ce_len = 0,
};

static int peers_used(void) {
    int n = 0;
    for (int i = 0; i < PEER_MAX; i++) n += s_peers[i].st != ST_FREE;
    return n;
}

static bool peer_connected(const ble_addr_t *a) {
    for (int i = 0; i < PEER_MAX; i++)
        if (s_peers[i].st != ST_FREE && ble_addr_cmp(&s_peers[i].addr, a) == 0) return true;
    return false;
}

// Bonded devices not connected now (the accept list for background reconnect).
static int reconnect_list(ble_addr_t *out, int max) {
    bond_t b[BONDS_MAX];
    int nb = 0, n = 0;
    if (!lock_ms(200)) return 0;
    nb = s_nbonds;
    memcpy(b, s_bonds, sizeof(bond_t) * nb);
    unlock();
    for (int i = 0; i < nb && n < max; i++) {
        ble_addr_t a = { .type = b[i].type };
        memcpy(a.val, b[i].addr, 6);
        if (!peer_connected(&a)) out[n++] = a;
    }
    return n;
}

// Decide which single GAP master procedure should run (explicit connect > user scan > background
// reconnect) and move towards it. Cancels complete asynchronously (CONNECT with BLE_HS_EAPP), so
// this is simply re-run on every relevant event.
static void gap_kick(void) {
    if (!s_synced) return;
    ble_addr_t wl[BONDS_MAX];
    int nwl = 0;
    gap_op_t want = OP_NONE;
    if (s_conn_want) want = OP_CONN;
    else if (s_scan_want) want = OP_SCAN;
    else if (peers_used() < PEER_MAX && (nwl = reconnect_list(wl, BONDS_MAX)) > 0) want = OP_WL;

    switch (s_op) {
    case OP_CANCEL:
    case OP_CONN:
        return;                                    // wait for its CONNECT event
    case OP_SCAN:
        if (want == OP_SCAN) return;               // a user scan runs to its end (or scan_stop)
        ble_gap_disc_cancel();                     // synchronous, no DISC_COMPLETE
        s_op = OP_NONE;
        break;
    case OP_WL:
        if (want == OP_WL && !s_wl_dirty) return;
        if (ble_gap_conn_cancel() == 0) { s_op = OP_CANCEL; return; }
        s_op = OP_NONE;
        break;
    case OP_NONE:
        break;
    }

    int rc = 0;
    if (want == OP_CONN) {
        s_conn_want = false;
        rc = ble_gap_connect(s_own_addr_type, &s_conn_addr, 10000, &kConnFast, gap_event, NULL);
        if (rc == 0) s_op = OP_CONN;
        else if (rc != BLE_HS_EDONE) { NV_LOGW(TAG, "connect: rc=%d", rc); set_error("Couldn't start the connection"); set_busy(NULL); }
        else set_busy(NULL);                       // already connected
    } else if (want == OP_SCAN) {
        const struct ble_gap_disc_params dp = {
            .itvl = 0x0060, .window = 0x0030, .filter_policy = 0, .limited = 0, .passive = 0,
            .filter_duplicates = 0,                // scan responses carry most pads' names
        };
        rc = ble_gap_disc(s_own_addr_type, s_scan_secs * 1000, &dp, gap_event, NULL);
        if (rc == 0) s_op = OP_SCAN;
        else { s_scan_want = false; NV_LOGW(TAG, "scan: rc=%d", rc); set_error("Couldn't start the scan"); }
    } else if (want == OP_WL) {
        rc = ble_gap_wl_set(wl, (uint8_t)nwl);
        if (rc == 0) rc = ble_gap_connect(s_own_addr_type, NULL, BLE_HS_FOREVER, &kConnBackground, gap_event, NULL);
        if (rc == 0) { s_op = OP_WL; s_wl_dirty = false; }
        else NV_LOGD(TAG, "background reconnect: rc=%d (retry on tick)", rc);
    }
    mirror_update();
}

// ---- scan results ------------------------------------------------------------------------------

static void on_adv(const struct ble_gap_disc_desc *d) {
    struct ble_hs_adv_fields f;
    if (ble_hs_adv_parse_fields(&f, d->data, d->length_data) != 0) return;
    bool hid = false;
    for (int i = 0; i < f.num_uuids16; i++) hid |= ble_uuid_u16(&f.uuids16[i].u) == UUID_HID_SVC;
    const uint16_t app = f.appearance_is_present ? f.appearance : 0;
    const bool hid_app = app >= 0x03C0 && app <= 0x03C4;
    if (!lock_ms(20)) return;
    int i = 0;
    while (i < s_nscan && !addr_eq(s_scan[i].addr, s_scan[i].addr_type, &d->addr)) i++;
    if (i == s_nscan) {
        // Only HID devices; a scan response alone can't tell, so it only updates known entries.
        if (!(hid || hid_app) || d->event_type == BLE_HCI_ADV_RPT_EVTYPE_SCAN_RSP) { unlock(); return; }
        if (s_nscan == SCAN_MAX) {                 // replace the weakest when this one is stronger
            int w = 0;
            for (int k = 1; k < s_nscan; k++) if (s_scan[k].rssi < s_scan[w].rssi) w = k;
            if (s_scan[w].rssi >= d->rssi) { unlock(); return; }
            i = w;
        } else {
            i = s_nscan++;
        }
        memset(&s_scan[i], 0, sizeof s_scan[i]);
        memcpy(s_scan[i].addr, d->addr.val, 6);
        s_scan[i].addr_type = d->addr.type;
        s_scan[i].rssi = d->rssi;
        for (int b = 0; b < s_nbonds; b++)
            if (addr_eq(s_bonds[b].addr, s_bonds[b].type, &d->addr)) s_scan[i].paired = true;
    }
    nv_bt_device_t *e = &s_scan[i];
    if (d->rssi > e->rssi && d->rssi != 127) e->rssi = d->rssi;
    e->hid |= hid;
    if (app) e->appearance = app;
    if (f.name_len && (f.name_is_complete || !e->name[0]))
        clean_name(e->name, sizeof e->name, (const char *)f.name, f.name_len);
    unlock();
}

// ---- HID setup state machine (host task) -------------------------------------------------------

static peer_t *peer_by_conn(uint16_t conn) {
    for (int i = 0; i < PEER_MAX; i++)
        if (s_peers[i].st != ST_FREE && s_peers[i].st != ST_DEAD && s_peers[i].conn == conn) return &s_peers[i];
    return NULL;
}

static void peer_fail(peer_t *p, const char *msg, int rc) {
    NV_LOGW(TAG, "%s: %s (rc=%d)", p->name, msg, rc);
    char e[64];
    snprintf(e, sizeof e, "%s: %s", p->name, msg);
    set_error(e);
    p->st = ST_DEAD;                               // freed by its DISCONNECT event
    ble_gap_terminate(p->conn, BLE_ERR_REM_USER_CONN_TERM);
}

static void peer_step(peer_t *p);

// Common prologue: the peer is gone (or going) -> ignore late GATT callbacks.
#define GATT_PEER()                                                              \
    peer_t *p = peer_by_conn(conn);                                              \
    if (!p || (err && err->status == BLE_HS_ENOTCONN)) return 0

static int on_mtu(uint16_t conn, const struct ble_gatt_error *err, uint16_t mtu, void *arg) {
    GATT_PEER();
    NV_LOGD(TAG, "%s: MTU %u", p->name, mtu);
    p->st = ST_SVCS;
    peer_step(p);
    return 0;
}

static int on_svc(uint16_t conn, const struct ble_gatt_error *err, const struct ble_gatt_svc *svc, void *arg) {
    GATT_PEER();
    if (err->status == 0 && svc) {
        const uint16_t u = ble_uuid_u16(&svc->uuid.u);
        if (u == UUID_HID_SVC && !p->hid_s) { p->hid_s = svc->start_handle; p->hid_e = svc->end_handle; }
        else if (u == UUID_BAS_SVC && !p->bas_s) { p->bas_s = svc->start_handle; p->bas_e = svc->end_handle; }
        return 0;
    }
    if (err->status != BLE_HS_EDONE) { peer_fail(p, "service discovery failed", err->status); return 0; }
    if (!p->hid_s) { peer_fail(p, "no HID service", 0); return 0; }
    p->st = ST_HID_CHRS;
    peer_step(p);
    return 0;
}

static int on_hid_chr(uint16_t conn, const struct ble_gatt_error *err, const struct ble_gatt_chr *chr, void *arg) {
    GATT_PEER();
    if (err->status == 0 && chr) {
        if (p->n_chr < CHR_MAX) p->chr_val[p->n_chr++] = chr->val_handle;
        switch (ble_uuid_u16(&chr->uuid.u)) {
        case UUID_REPORT_MAP: p->map_h = chr->val_handle; break;
        case UUID_PROTO_MODE: p->proto_h = chr->val_handle; break;
        case UUID_REPORT:
            if (p->n_rpt < RPT_MAX) {
                rpt_t *r = &p->rpt[p->n_rpt++];
                memset(r, 0, sizeof *r);
                r->val = chr->val_handle;
                r->props = chr->properties;
            }
            break;
        default: break;
        }
        return 0;
    }
    if (err->status != BLE_HS_EDONE) { peer_fail(p, "HID discovery failed", err->status); return 0; }
    p->st = ST_HID_DSCS;
    peer_step(p);
    return 0;
}

// One Find Information pass over the whole HID service; a descriptor belongs to the characteristic
// with the highest value handle below it.
static int on_hid_dsc(uint16_t conn, const struct ble_gatt_error *err, uint16_t chr_val, const struct ble_gatt_dsc *dsc, void *arg) {
    GATT_PEER();
    if (err->status == 0 && dsc) {
        const uint16_t u = ble_uuid_u16(&dsc->uuid.u);
        if (u != UUID_CCCD && u != UUID_REPORT_REF) return 0;
        uint16_t owner = 0;
        for (int i = 0; i < p->n_chr; i++)
            if (p->chr_val[i] < dsc->handle && p->chr_val[i] > owner) owner = p->chr_val[i];
        for (int i = 0; i < p->n_rpt; i++) {
            if (p->rpt[i].val != owner) continue;
            if (u == UUID_CCCD) p->rpt[i].cccd = dsc->handle; else p->rpt[i].ref = dsc->handle;
        }
        return 0;
    }
    if (err->status != BLE_HS_EDONE) { peer_fail(p, "HID descriptor discovery failed", err->status); return 0; }
    p->st = ST_RPT_REFS;
    p->idx = 0;
    peer_step(p);
    return 0;
}

static int on_rpt_ref(uint16_t conn, const struct ble_gatt_error *err, struct ble_gatt_attr *attr, void *arg) {
    GATT_PEER();
    if (err->status == 0 && attr && OS_MBUF_PKTLEN(attr->om) >= 2) {
        uint8_t v[2];
        os_mbuf_copydata(attr->om, 0, 2, v);
        p->rpt[p->idx].id = v[0];
        p->rpt[p->idx].type = v[1];
    }
    p->idx++;
    peer_step(p);
    return 0;
}

static int on_map(uint16_t conn, const struct ble_gatt_error *err, struct ble_gatt_attr *attr, void *arg) {
    GATT_PEER();
    if (err->status == 0 && attr) {
        const int len = OS_MBUF_PKTLEN(attr->om);
        if (attr->offset < MAP_CAP) {
            const int n = attr->offset + len > MAP_CAP ? MAP_CAP - attr->offset : len;
            os_mbuf_copydata(attr->om, 0, n, p->map + attr->offset);
            if (attr->offset + n > p->map_len) p->map_len = attr->offset + n;
        }
        return 0;
    }
    if (err->status != BLE_HS_EDONE) { peer_fail(p, "report map read failed", err->status); return 0; }
    p->st = ST_PROTO;
    peer_step(p);
    return 0;
}

static int on_pnp(uint16_t conn, const struct ble_gatt_error *err, struct ble_gatt_attr *attr, void *arg) {
    GATT_PEER();
    if (err->status == 0 && attr) {
        uint8_t v[7];
        if (OS_MBUF_PKTLEN(attr->om) >= 7 && os_mbuf_copydata(attr->om, 0, 7, v) == 0) {
            p->vid = (uint16_t)(v[1] | v[2] << 8);
            p->pid = (uint16_t)(v[3] | v[4] << 8);
        }
        return 0;
    }
    p->st = ST_NAME;                               // EDONE or no PnP ID: both fine
    peer_step(p);
    return 0;
}

static int on_name(uint16_t conn, const struct ble_gatt_error *err, struct ble_gatt_attr *attr, void *arg) {
    GATT_PEER();
    if (err->status == 0 && attr) {
        char raw[31];
        const int n = OS_MBUF_PKTLEN(attr->om) < (int)sizeof raw ? OS_MBUF_PKTLEN(attr->om) : (int)sizeof raw;
        if (n > 0 && os_mbuf_copydata(attr->om, 0, n, raw) == 0) clean_name(p->name, sizeof p->name, raw, n);
        return 0;
    }
    struct ble_gap_conn_desc desc;
    if (ble_gap_conn_find(p->conn, &desc) == 0 && desc.sec_state.bonded) bonds_add(&p->addr, p->name);
    p->st = ST_BAS_CHRS;
    peer_step(p);
    return 0;
}

static int on_bas_chr(uint16_t conn, const struct ble_gatt_error *err, const struct ble_gatt_chr *chr, void *arg) {
    GATT_PEER();
    if (err->status == 0 && chr) {
        if (!p->batt_h) { p->batt_h = chr->val_handle; p->batt_props = chr->properties; }
        return 0;
    }
    p->st = ST_BAS_DSCS;
    peer_step(p);
    return 0;
}

static int on_bas_dsc(uint16_t conn, const struct ble_gatt_error *err, uint16_t chr_val, const struct ble_gatt_dsc *dsc, void *arg) {
    GATT_PEER();
    if (err->status == 0 && dsc) {
        if (!p->batt_cccd && ble_uuid_u16(&dsc->uuid.u) == UUID_CCCD) p->batt_cccd = dsc->handle;
        return 0;
    }
    p->st = ST_BATT;
    peer_step(p);
    return 0;
}

static int on_batt(uint16_t conn, const struct ble_gatt_error *err, struct ble_gatt_attr *attr, void *arg) {
    GATT_PEER();
    uint8_t b;
    if (err->status == 0 && attr && OS_MBUF_PKTLEN(attr->om) >= 1 && os_mbuf_copydata(attr->om, 0, 1, &b) == 0)
        p->battery = b > 100 ? 100 : b;
    p->st = ST_SUBS;
    p->idx = 0;
    peer_step(p);
    return 0;
}

static int on_sub(uint16_t conn, const struct ble_gatt_error *err, struct ble_gatt_attr *attr, void *arg) {
    GATT_PEER();
    if (err->status != 0) NV_LOGW(TAG, "%s: subscribe #%u failed (%d)", p->name, p->idx, err->status);
    p->idx++;
    peer_step(p);
    return 0;
}

static void map_job(void *arg);

static void setup_done(peer_t *p) {
    p->deadline = 0;
    // Low latency: ask for 7.5-15 ms when the pad settled on something slower.
    struct ble_gap_conn_desc desc;
    if (ble_gap_conn_find(p->conn, &desc) == 0 && desc.conn_itvl > 12) {
        const struct ble_gap_upd_params up = {
            .itvl_min = 6, .itvl_max = 12, .latency = desc.conn_latency,
            .supervision_timeout = desc.supervision_timeout, .min_ce_len = 0, .max_ce_len = 0,
        };
        ble_gap_update_params(p->conn, &up);
    }
    mirror_update();
    gap_kick();
}

static bool submit_map_job(peer_t *p) {
    map_job_t *j = heap_caps_calloc(1, sizeof *j, MALLOC_CAP_SPIRAM);
    if (!j) return false;
    j->conn = p->conn;
    j->gen = p->gen;
    j->vid = p->vid;
    j->pid = p->pid;
    j->layout = p->layout;
    if (!nv_bgwork_submit(map_job, j)) { heap_caps_free(j); return false; }
    return true;
}

// All GATT reads done: parse the report map and look the pad up (on nv_bgwork).
static void finish_setup(peer_t *p) {
    p->is_pad = p->map_len && nv_hid_pad_parse(p->map, p->map_len, &p->layout);
    NV_LOGI(TAG, "%s: VID %04x PID %04x, report map %u bytes, %u reports, %s", p->name, p->vid, p->pid,
            p->map_len, p->n_rpt, p->is_pad ? "gamepad" : "not a gamepad");
    heap_caps_free(p->map);
    p->map = NULL;
    if (!p->is_pad) {                              // BLE keyboard / mouse / remote: stays paired, no pad slot
        p->st = ST_READY;
        setup_done(p);
        return;
    }
    int pr = -1;
    for (int i = 0; i < p->n_rpt && pr < 0; i++)
        if (p->rpt[i].type == 1 && p->rpt[i].id == p->layout.report_id) pr = i;
    for (int i = 0; i < p->n_rpt && pr < 0; i++)   // no Report Reference: first notifying report
        if (p->rpt[i].type == 0 && (p->rpt[i].props & BLE_GATT_CHR_PROP_NOTIFY)) pr = i;
    if (pr < 0) { peer_fail(p, "gamepad input report not found", 0); return; }
    p->pad_rpt = (uint8_t)pr;
    if (p->vid == 0x045E)                          // Xbox: output report 3 = rumble
        for (int i = 0; i < p->n_rpt; i++)
            if (p->rpt[i].type == 2 && p->rpt[i].id == 3) p->rumble_h = p->rpt[i].val;
    p->map_pending = !submit_map_job(p);           // queue full: retried by the 1 s tick
}

static void peer_step(peer_t *p) {
    int rc = 0;
    for (;;) {
        switch (p->st) {
        case ST_MTU:
            if (ble_gattc_exchange_mtu(p->conn, on_mtu, NULL) == 0) return;
            p->st = ST_SVCS;
            continue;
        case ST_SVCS:
            rc = ble_gattc_disc_all_svcs(p->conn, on_svc, NULL);
            break;
        case ST_HID_CHRS:
            rc = ble_gattc_disc_all_chrs(p->conn, p->hid_s, p->hid_e, on_hid_chr, NULL);
            break;
        case ST_HID_DSCS:
            rc = ble_gattc_disc_all_dscs(p->conn, p->hid_s, p->hid_e, on_hid_dsc, NULL);
            break;
        case ST_RPT_REFS:
            while (p->idx < p->n_rpt && !p->rpt[p->idx].ref) p->idx++;
            if (p->idx >= p->n_rpt) { p->st = ST_MAP; continue; }
            rc = ble_gattc_read(p->conn, p->rpt[p->idx].ref, on_rpt_ref, NULL);
            break;
        case ST_MAP:
            if (!p->map_h) { peer_fail(p, "no report map", 0); return; }
            if (!p->map) p->map = heap_caps_malloc(MAP_CAP, MALLOC_CAP_SPIRAM);
            if (!p->map) { peer_fail(p, "out of memory", 0); return; }
            p->map_len = 0;
            rc = ble_gattc_read_long(p->conn, p->map_h, 0, on_map, NULL);
            break;
        case ST_PROTO:
            if (p->proto_h) {                      // Report protocol (pads have no boot protocol anyway)
                const uint8_t mode = 1;
                ble_gattc_write_no_rsp_flat(p->conn, p->proto_h, &mode, 1);
            }
            p->st = ST_PNP;
            continue;
        case ST_PNP:
            if (ble_gattc_read_by_uuid(p->conn, 1, 0xFFFF, BLE_UUID16_DECLARE(UUID_PNP_ID), on_pnp, NULL) == 0) return;
            p->st = ST_NAME;
            continue;
        case ST_NAME:
            if (ble_gattc_read_by_uuid(p->conn, 1, 0xFFFF, BLE_UUID16_DECLARE(UUID_GAP_NAME), on_name, NULL) == 0) return;
            p->st = ST_BAS_CHRS;
            continue;
        case ST_BAS_CHRS:
            if (p->bas_s && ble_gattc_disc_chrs_by_uuid(p->conn, p->bas_s, p->bas_e, BLE_UUID16_DECLARE(UUID_BATT_LEVEL),
                                                        on_bas_chr, NULL) == 0) return;
            p->st = ST_SUBS;
            p->idx = 0;
            continue;
        case ST_BAS_DSCS:
            if (p->batt_h && p->batt_h < p->bas_e && (p->batt_props & BLE_GATT_CHR_PROP_NOTIFY) &&
                ble_gattc_disc_all_dscs(p->conn, p->batt_h, p->bas_e, on_bas_dsc, NULL) == 0) return;
            p->st = ST_BATT;
            continue;
        case ST_BATT:
            if (p->batt_h && (p->batt_props & BLE_GATT_CHR_PROP_READ) &&
                ble_gattc_read(p->conn, p->batt_h, on_batt, NULL) == 0) return;
            p->st = ST_SUBS;
            p->idx = 0;
            continue;
        case ST_SUBS: {
            // Every input report (idx < n_rpt), then the battery level (idx == n_rpt).
            static const uint8_t kNotify[2] = { 1, 0 };
            while (p->idx < p->n_rpt && !(p->rpt[p->idx].cccd && p->rpt[p->idx].type != 2 && p->rpt[p->idx].type != 3))
                p->idx++;
            uint16_t h = 0;
            if (p->idx < p->n_rpt) h = p->rpt[p->idx].cccd;
            else if (p->idx == p->n_rpt) h = p->batt_cccd;
            if (!h && p->idx == p->n_rpt) { p->idx++; continue; }
            if (!h) { p->st = ST_MAPPING; continue; }
            rc = ble_gattc_write_flat(p->conn, h, kNotify, sizeof kNotify, on_sub, NULL);
            if (rc) { NV_LOGW(TAG, "%s: subscribe rc=%d", p->name, rc); p->idx++; continue; }
            return;
        }
        case ST_MAPPING:
            finish_setup(p);
            return;
        default:
            return;
        }
        if (rc) peer_fail(p, "GATT request failed", rc);
        return;
    }
}

// ---- pad slot ----------------------------------------------------------------------------------

static bool rumble_hook(void *ctx, uint16_t low, uint16_t high) {
    peer_t *p = &s_peers[(intptr_t)ctx];
    if (!lock_ms(20)) return false;
    bool ok = s_running;
    if (ok) {
        p->rum_lo = low;
        p->rum_hi = high;
        p->rum_dirty = true;
        s_cmd_bits |= CMD_RUMBLE;
        ble_npl_eventq_put(nimble_port_get_dflt_eventq(), &s_cmd_ev);
    }
    unlock();
    return ok;
}

// Xbox BLE rumble (output report 3, SDL/xpadneo layout): enable mask, LT, RT, strong, weak (0..100),
// on-time, off-time (10 ms units), repeat. nv_pad stops it with a (0, 0) call.
static void rumble_send(peer_t *p) {
    uint16_t lo, hi;
    if (!lock_ms(50)) return;
    lo = p->rum_lo;
    hi = p->rum_hi;
    p->rum_dirty = false;
    unlock();
    const uint8_t pkt[8] = { 0x0F, 0, 0, (uint8_t)(lo / 656), (uint8_t)(hi / 656), 0xFF, 0x00, 0xEB };
    ble_gattc_write_no_rsp_flat(p->conn, p->rumble_h, pkt, sizeof pkt);
}

static void pad_attach(peer_t *p, const map_job_t *j) {
    p->pmap = j->map;
    nv_pad_info_t info = {
        .source = NV_PAD_SRC_BLE, .mapped = j->db, .battery = p->batt_h ? p->battery : 255,
        .rumble = p->rumble_h != 0, .vid = p->vid, .pid = p->pid,
    };
    snprintf(info.name, sizeof info.name, "%s", p->name[0] ? p->name : "Bluetooth pad");
    p->slot = nv_pad_attach(&info);
    p->st = ST_READY;
    if (p->slot < 0) {
        NV_LOGW(TAG, "%s: all %d pad slots taken", p->name, NV_PAD_MAX);
    } else {
        if (p->rumble_h) nv_pad_set_rumble(p->slot, rumble_hook, (void *)(intptr_t)(p - s_peers));
        NV_LOGI(TAG, "pad '%s' ready: slot %d, %s mapping, %u axes %u hats %u buttons, report id %u%s", p->name,
                p->slot, j->db ? "DB" : "guessed", p->layout.n_axes, p->layout.n_hats, p->layout.n_buttons,
                p->layout.report_id, p->rumble_h ? ", rumble" : "");
        set_error(NULL);
    }
    setup_done(p);
}

static void map_job(void *arg) {              // nv_bgwork task
    map_job_t *j = arg;
    j->db = nv_hid_pad_map(NV_PAD_BUS_BT, j->vid, j->pid, &j->layout, &j->map);
    bool queued = false;
    if (lock_ms(1000)) {
        if (s_running && s_ndone < DONE_MAX) {
            s_done[s_ndone++] = j;
            s_cmd_bits |= CMD_DONE;
            ble_npl_eventq_put(nimble_port_get_dflt_eventq(), &s_cmd_ev);
            queued = true;
        }
        unlock();
    }
    if (!queued) heap_caps_free(j);
}

// ---- connection lifecycle ----------------------------------------------------------------------

static void peer_on_connect(uint16_t conn) {
    struct ble_gap_conn_desc desc;
    if (ble_gap_conn_find(conn, &desc) != 0) return;
    peer_t *p = NULL;
    for (int i = 0; i < PEER_MAX && !p; i++) if (s_peers[i].st == ST_FREE) p = &s_peers[i];
    if (!p) { ble_gap_terminate(conn, BLE_ERR_CONN_LIMIT); return; }
    memset(p, 0, sizeof *p);
    p->st = ST_SEC;
    p->conn = conn;
    p->gen = ++s_gen;
    p->addr = desc.peer_id_addr;
    p->slot = -1;
    p->battery = 255;
    p->deadline = esp_timer_get_time() + SETUP_TIMEOUT_US;
    bonds_name_of(&p->addr, p->name, sizeof p->name);
    if (!p->name[0]) snprintf(p->name, sizeof p->name, "Bluetooth device");
    char a[18];
    nv_bt_addr_str(p->addr.val, a);
    NV_LOGI(TAG, "connected %s (%s), securing link", p->name, a);
    mirror_update();
    // Pads refuse the HID service until the link is encrypted: pair (new) or encrypt (bonded).
    const int rc = ble_gap_security_initiate(conn);
    if (rc == BLE_HS_EALREADY) { p->st = ST_MTU; peer_step(p); }
    else if (rc != 0) peer_fail(p, "security request failed", rc);
}

static void peer_on_disconnect(uint16_t conn, int reason) {
    for (int i = 0; i < PEER_MAX; i++) {
        peer_t *p = &s_peers[i];
        if (p->st == ST_FREE || p->conn != conn) continue;
        if (p->slot >= 0) nv_pad_detach(p->slot);
        if (p->st != ST_READY && p->st != ST_DEAD) {
            char e[64];
            snprintf(e, sizeof e, "%s disconnected during setup", p->name);
            set_error(e);
        }
        NV_LOGI(TAG, "disconnected %s (reason 0x%x)", p->name, reason);
        heap_caps_free(p->map);
        memset(p, 0, sizeof *p);
        p->slot = -1;
        s_wl_dirty = true;
    }
    mirror_update();
}

static void on_enc_change(uint16_t conn, int status) {
    peer_t *p = peer_by_conn(conn);
    if (!p) return;
    struct ble_gap_conn_desc desc;
    if (ble_gap_conn_find(conn, &desc) != 0) return;
    if (status == 0) {
        p->addr = desc.peer_id_addr;               // identity address after key distribution
        if (desc.sec_state.bonded) bonds_add(&p->addr, p->name);
        NV_LOGI(TAG, "%s: link encrypted%s", p->name, desc.sec_state.bonded ? " (bonded)" : "");
        if (p->st == ST_SEC) { p->st = ST_MTU; peer_step(p); }
        return;
    }
    // Typically the pad was re-paired with another host and lost our keys: forget and pair again once.
    NV_LOGW(TAG, "%s: encryption failed (%d)", p->name, status);
    if (!p->sec_retry) {
        p->sec_retry = true;
        ble_store_util_delete_peer(&desc.peer_id_addr);
        if (lock_ms(200)) {
            char *out = bonds_remove_locked(&desc.peer_id_addr) ? bonds_serialize_locked() : NULL;
            unlock();
            bonds_save(out);
        }
        if (ble_gap_security_initiate(conn) == 0) return;
    }
    peer_fail(p, "pairing failed", status);
}

static int gap_event(struct ble_gap_event *ev, void *arg) {
    switch (ev->type) {
    case BLE_GAP_EVENT_DISC:
        if (s_op == OP_SCAN) on_adv(&ev->disc);
        return 0;
    case BLE_GAP_EVENT_DISC_COMPLETE:
        if (s_op == OP_SCAN) s_op = OP_NONE;
        s_scan_want = false;
        gap_kick();
        mirror_update();
        return 0;
    case BLE_GAP_EVENT_CONNECT: {
        const gap_op_t was = s_op;
        if (was == OP_CONN || was == OP_WL || was == OP_CANCEL) s_op = OP_NONE;
        if (ev->connect.status == 0) {
            if (was == OP_CONN) set_busy(NULL);
            peer_on_connect(ev->connect.conn_handle);
        } else if (was == OP_CONN) {
            NV_LOGW(TAG, "connect failed (%d)", ev->connect.status);
            set_error(ev->connect.status == BLE_HS_ETIMEOUT ? "Device not found: switch it to pairing mode"
                                                            : "Connection failed");
            set_busy(NULL);
        }
        gap_kick();
        mirror_update();
        return 0;
    }
    case BLE_GAP_EVENT_DISCONNECT:
        peer_on_disconnect(ev->disconnect.conn.conn_handle, ev->disconnect.reason);
        gap_kick();
        return 0;
    case BLE_GAP_EVENT_ENC_CHANGE:
        on_enc_change(ev->enc_change.conn_handle, ev->enc_change.status);
        return 0;
    case BLE_GAP_EVENT_REPEAT_PAIRING: {
        // The pad wants to pair again although we hold a bond: drop the old keys and let it.
        struct ble_gap_conn_desc desc;
        if (ble_gap_conn_find(ev->repeat_pairing.conn_handle, &desc) == 0)
            ble_store_util_delete_peer(&desc.peer_id_addr);
        return BLE_GAP_REPEAT_PAIRING_RETRY;
    }
    case BLE_GAP_EVENT_PASSKEY_ACTION:
        if (ev->passkey.params.action == BLE_SM_IOACT_NUMCMP) {
            struct ble_sm_io io = { .action = BLE_SM_IOACT_NUMCMP, .numcmp_accept = 1 };
            ble_sm_inject_io(ev->passkey.conn_handle, &io);
        }
        return 0;
    case BLE_GAP_EVENT_CONN_UPDATE_REQ:
    case BLE_GAP_EVENT_L2CAP_UPDATE_REQ: {
        // Accept what the pad asks for, capped at 15 ms when it allows that (input latency).
        const struct ble_gap_upd_params *pp = ev->conn_update_req.peer_params;
        struct ble_gap_upd_params *sp = ev->conn_update_req.self_params;
        if (pp && sp) {
            *sp = *pp;
            if (pp->itvl_min <= 12 && pp->itvl_max > 12) sp->itvl_max = 12;
        }
        return 0;
    }
    case BLE_GAP_EVENT_NOTIFY_RX: {
        peer_t *p = peer_by_conn(ev->notify_rx.conn_handle);
        if (!p) return 0;
        struct os_mbuf *om = ev->notify_rx.om;
        const int len = OS_MBUF_PKTLEN(om);
        const uint16_t h = ev->notify_rx.attr_handle;
        if (h == p->batt_h && p->batt_h) {
            uint8_t b;
            if (len >= 1 && os_mbuf_copydata(om, 0, 1, &b) == 0) {
                p->battery = b > 100 ? 100 : b;
                if (p->slot >= 0) nv_pad_set_battery(p->slot, p->battery);
            }
            return 0;
        }
        if (p->st != ST_READY || p->slot < 0 || h != p->rpt[p->pad_rpt].val) return 0;
        // BLE reports come without the report ID byte the decoder expects: put it back.
        uint8_t buf[96];
        int off = 0;
        if (p->layout.report_id) buf[off++] = p->layout.report_id;
        const int n = len < (int)sizeof buf - off ? len : (int)sizeof buf - off;
        if (os_mbuf_copydata(om, 0, n, buf + off) != 0) return 0;
        nv_hid_raw_t raw;
        memset(&raw, 0, sizeof raw);
        if (!nv_hid_pad_decode(&p->layout, buf, (size_t)(off + n), &raw)) return 0;
        nv_pad_input_t in;
        nv_pad_map_apply(&p->pmap, &raw, &in);
        nv_pad_update(p->slot, &in);
        return 0;
    }
    case BLE_GAP_EVENT_MTU:
        NV_LOGD(TAG, "conn %u MTU %u", ev->mtu.conn_handle, ev->mtu.value);
        return 0;
    default:
        return 0;
    }
}

// ---- host task events --------------------------------------------------------------------------

static void cmd_event(struct ble_npl_event *ev) {
    map_job_t *done[DONE_MAX];
    ble_addr_t forget[FORGET_MAX];
    int ndone = 0, nforget = 0;
    uint32_t bits = 0;
    if (!lock_ms(200)) { ble_npl_eventq_put(nimble_port_get_dflt_eventq(), &s_cmd_ev); return; }
    bits = s_cmd_bits;
    s_cmd_bits = 0;
    if (bits & CMD_SCAN) s_scan_secs = s_cmd_scan_secs;
    if (bits & CMD_CONNECT) s_conn_addr = s_cmd_conn_addr;
    nforget = s_ncmd_forget;
    memcpy(forget, s_cmd_forget, sizeof(ble_addr_t) * nforget);
    s_ncmd_forget = 0;
    ndone = s_ndone;
    memcpy(done, s_done, sizeof(map_job_t *) * ndone);
    s_ndone = 0;
    unlock();

    for (int i = 0; i < nforget; i++) {
        for (int k = 0; k < PEER_MAX; k++)
            if (s_peers[k].st != ST_FREE && ble_addr_cmp(&s_peers[k].addr, &forget[i]) == 0)
                ble_gap_terminate(s_peers[k].conn, BLE_ERR_REM_USER_CONN_TERM);
        ble_store_util_delete_peer(&forget[i]);
        s_wl_dirty = true;
    }
    for (int i = 0; i < ndone; i++) {
        peer_t *p = peer_by_conn(done[i]->conn);
        if (p && p->gen == done[i]->gen && p->st == ST_MAPPING) pad_attach(p, done[i]);
        heap_caps_free(done[i]);
    }
    if (bits & CMD_RUMBLE)
        for (int k = 0; k < PEER_MAX; k++)
            if (s_peers[k].st == ST_READY && s_peers[k].rumble_h && s_peers[k].rum_dirty) rumble_send(&s_peers[k]);
    if (bits & CMD_SCAN_STOP) s_scan_want = false;
    if (bits & CMD_SCAN) { s_scan_want = true; if (s_op == OP_SCAN) { ble_gap_disc_cancel(); s_op = OP_NONE; } }
    if (bits & CMD_CONNECT) s_conn_want = true;
    gap_kick();
    mirror_update();
}

static void tick_event(struct ble_npl_event *ev) {
    const int64_t now = esp_timer_get_time();
    for (int i = 0; i < PEER_MAX; i++) {
        peer_t *p = &s_peers[i];
        if (p->st == ST_FREE || p->st == ST_DEAD || p->st == ST_READY) continue;
        if (p->st == ST_MAPPING && p->map_pending) p->map_pending = !submit_map_job(p);
        if (p->deadline && now > p->deadline) peer_fail(p, "setup timed out", 0);
    }
    gap_kick();                                    // retries a background reconnect that failed to start
    ble_npl_callout_reset(&s_tick, ble_npl_time_ms_to_ticks32(1000));
}

static void on_sync(void) {
    ble_hs_util_ensure_addr(0);
    ble_hs_id_infer_auto(0, &s_own_addr_type);
    s_op = OP_NONE;
    s_synced = true;
    bonds_reconcile();
    if (lock_ms(200)) {
        if (s_phase == PH_ERROR || s_phase == PH_STARTING) s_phase = PH_RUNNING;
        s_error[0] = 0;
        unlock();
    }
    NV_LOGI(TAG, "BLE host synced (%d paired)", s_nbonds);
    ble_npl_callout_reset(&s_tick, ble_npl_time_ms_to_ticks32(1000));
    gap_kick();
}

static void on_reset(int reason) {
    s_synced = false;
    s_op = OP_NONE;
    NV_LOGW(TAG, "BLE host reset (reason %d)", reason);
}

static void host_task(void *arg) {
    nimble_port_run();                             // returns after nimble_port_stop()
    nimble_port_freertos_deinit();
}

// esp_hosted hands every HCI packet from the C6 to this (weak in vhci_drv.c; same logic). The
// guard drops packets while the NimBLE transport pools don't exist — Bluetooth off, or being torn
// down — instead of allocating from freed pools.
int hci_rx_handler(uint8_t *buf, size_t buf_len) {
    if (!s_hci_live || buf_len < 2) return ESP_OK;
    __atomic_add_fetch(&s_hci_inflight, 1, __ATOMIC_SEQ_CST);
    int ret = ESP_FAIL;
    if (!s_hci_live) goto out;
    if (buf[0] == HCI_H4_EVT) {
        if (buf_len < 3) goto out;
        const int total = 2 + buf[2];
        if (total > MYNEWT_VAL(BLE_TRANSPORT_EVT_SIZE) || (size_t)total > buf_len - 1) goto out;
        if (buf[1] == BLE_HCI_EVCODE_HW_ERROR) goto out;
        const bool adv = buf[1] == BLE_HCI_EVCODE_LE_META && buf_len > 3 &&
                         (buf[3] == BLE_HCI_LE_SUBEV_ADV_RPT || buf[3] == BLE_HCI_LE_SUBEV_EXT_ADV_RPT);
        uint8_t *evbuf = ble_transport_alloc_evt(adv ? 1 : 0);   // adv reports: discardable pool
        if (!evbuf) goto out;
        memcpy(evbuf, &buf[1], total);
        if (ble_transport_to_hs_evt(evbuf) == 0) ret = ESP_OK;
    } else if (buf[0] == HCI_H4_ACL) {
        struct os_mbuf *m = ble_transport_alloc_acl_from_ll();
        if (!m) goto out;
        if (os_mbuf_append(m, &buf[1], buf_len - 1) != 0) { os_mbuf_free_chain(m); goto out; }
        ble_transport_to_hs_acl(m);
        ret = ESP_OK;
    }
out:
    __atomic_sub_fetch(&s_hci_inflight, 1, __ATOMIC_SEQ_CST);
    return ret;
}

// ---- start / stop (control task) ---------------------------------------------------------------

static void set_phase(phase_t ph, const char *err) {
    if (!lock_ms(500)) return;
    s_phase = ph;
    if (err) snprintf(s_error, sizeof s_error, "%s", err);
    unlock();
}

static bool want_on(void) {
    bool on = false;
    if (lock_ms(500)) { on = s_enabled; unlock(); }
    return on;
}

static void do_start(void) {
    set_phase(PH_STARTING, "");
    // The C6 link is normally opened by the Wi-Fi bring-up; open it ourselves if nobody does.
    esp_hosted_coprocessor_fwver_t fw;
    bool up = false;
    for (int i = 0; i < 75 && want_on(); i++) {    // <= 15 s
        if (esp_hosted_get_coprocessor_fwversion(&fw) == ESP_OK) { up = true; break; }
        if (i == 15) esp_hosted_connect_to_slave();
        vTaskDelay(pdMS_TO_TICKS(200));
    }
    if (!up) {
        if (want_on()) { NV_LOGW(TAG, "C6 link down, Bluetooth not started"); set_phase(PH_ERROR, "Radio co-processor not answering"); }
        s_start_failed = true;
        return;
    }
    NV_LOGI(TAG, "starting BLE host (C6 firmware %u.%u.%u)", (unsigned)fw.major1, (unsigned)fw.minor1, (unsigned)fw.patch1);
    // Firmware without these RPCs starts its BLE controller at boot: failure is only a warning.
    if (esp_hosted_bt_controller_init() != ESP_OK) NV_LOGW(TAG, "C6 BT controller init RPC failed (old firmware?)");
    if (esp_hosted_bt_controller_enable() != ESP_OK) NV_LOGW(TAG, "C6 BT controller enable RPC failed (old firmware?)");

    if (nimble_port_init() != ESP_OK) {
        NV_LOGE(TAG, "nimble_port_init failed");
        set_phase(PH_ERROR, "Bluetooth host init failed");
        s_start_failed = true;
        return;
    }
    s_hci_live = true;
    ble_hs_cfg.reset_cb = on_reset;
    ble_hs_cfg.sync_cb = on_sync;
    ble_hs_cfg.store_status_cb = ble_store_util_status_rr;
    ble_hs_cfg.sm_io_cap = BLE_HS_IO_NO_INPUT_OUTPUT;   // pads have no display: Just Works
    ble_hs_cfg.sm_bonding = 1;
    ble_hs_cfg.sm_mitm = 0;
    ble_hs_cfg.sm_sc = 1;
    ble_hs_cfg.sm_our_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_hs_cfg.sm_their_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_store_config_init();                       // bonds from NVS (this task has an internal stack)

    ble_npl_event_init(&s_cmd_ev, cmd_event, NULL);
    ble_npl_callout_init(&s_tick, nimble_port_get_dflt_eventq(), tick_event, NULL);
    s_op = OP_NONE;
    s_scan_want = s_conn_want = false;
    s_host_started = true;
    if (lock_ms(500)) { s_running = true; unlock(); }
    nimble_port_freertos_init(host_task);

    for (int i = 0; i < 50 && !s_synced && want_on(); i++) vTaskDelay(pdMS_TO_TICKS(100));
    if (s_synced) return;                          // on_sync set PH_RUNNING
    if (want_on()) {
        // The host keeps retrying; on_sync clears this if the controller shows up later.
        NV_LOGW(TAG, "BLE controller not answering (C6 firmware without BLE over SDIO? update it)");
        set_phase(PH_ERROR, "Bluetooth controller not answering: update the C6 firmware");
    }
}

static void do_stop(void) {
    if (lock_ms(1000)) { s_running = false; s_cmd_bits = 0; s_ndone = 0; s_ncmd_forget = 0; unlock(); }
    // (map jobs already queued free themselves when s_running is false)
    if (s_host_started) {
        const int rc = nimble_port_stop();         // terminates links (bounded by HS_STOP_TIMEOUT)
        if (rc != 0) {
            NV_LOGW(TAG, "nimble_port_stop rc=%d", rc);
            nimble_port_freertos_deinit();         // host task still in its loop: delete it
        }
        vTaskDelay(pdMS_TO_TICKS(20));             // let the host task finish deleting itself
        s_hci_live = false;
        for (int i = 0; i < 100 && __atomic_load_n(&s_hci_inflight, __ATOMIC_SEQ_CST); i++) vTaskDelay(1);
        ble_npl_callout_deinit(&s_tick);
        ble_npl_event_deinit(&s_cmd_ev);
        nimble_port_deinit();
        s_host_started = false;
    }
    s_synced = false;
    s_op = OP_NONE;
    if (esp_hosted_bt_controller_disable() != ESP_OK) NV_LOGD(TAG, "BT controller disable RPC failed");
    for (int i = 0; i < PEER_MAX; i++) {           // host task is gone: safe to touch the peers
        peer_t *p = &s_peers[i];
        if (p->st != ST_FREE && p->slot >= 0) nv_pad_detach(p->slot);
        heap_caps_free(p->map);
        memset(p, 0, sizeof *p);
        p->slot = -1;
    }
    if (lock_ms(1000)) {
        s_phase = PH_OFF;
        s_error[0] = s_busy[0] = 0;
        s_m_npads = s_m_nconn = 0;
        s_m_scanning = s_m_connecting = false;
        unlock();
    }
    NV_LOGI(TAG, "Bluetooth off");
}

static void ctl_task(void *arg) {
    for (;;) {
        bool want = false;
        if (lock_ms(1000)) {
            want = s_enabled;
            if (want == s_host_started || (want && s_start_failed)) {
                s_ctl_busy = false;
                unlock();
                break;
            }
            unlock();
        } else {
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }
        if (want) do_start(); else do_stop();
    }
    vTaskDelete(NULL);
}

static void ctl_kick_locked(void) {
    if (s_ctl_busy) return;                        // the running control task re-reads s_enabled
    // Internal-RAM stack: nimble_port_init reads the bond store from NVS.
    if (xTaskCreatePinnedToCore(ctl_task, "nv_bt", 4608, NULL, 4, NULL, tskNO_AFFINITY) == pdPASS)
        s_ctl_busy = true;
    else
        snprintf(s_error, sizeof s_error, "out of memory");
}

// ---- public API --------------------------------------------------------------------------------

void nv_bt_init(void) {
    if (s_inited) return;
    s_lock = xSemaphoreCreateMutex();
    s_scan = heap_caps_calloc(SCAN_MAX, sizeof(nv_bt_device_t), MALLOC_CAP_SPIRAM);
    s_peers = heap_caps_calloc(PEER_MAX, sizeof(peer_t), MALLOC_CAP_SPIRAM);
    if (!s_lock || !s_scan || !s_peers) { NV_LOGE(TAG, "init: out of memory"); return; }
    for (int i = 0; i < PEER_MAX; i++) s_peers[i].slot = -1;
    bonds_load();
    s_inited = true;
    const bool on = nv_config_get_bool("bt_on", false);
    NV_LOGI(TAG, "Bluetooth service ready (%d paired, %s)", s_nbonds, on ? "on" : "off");
    if (on && lock_ms(500)) {
        s_enabled = true;
        ctl_kick_locked();
        unlock();
    }
}

void nv_bt_set_enabled(bool on) {
    if (!s_inited) return;
    if (nv_bt_is_enabled() != on) nv_config_set_bool("bt_on", on);
    if (!lock_ms(500)) return;
    if (on && !s_enabled) s_start_failed = false;  // user retry after an error
    s_enabled = on;
    ctl_kick_locked();
    unlock();
}

bool nv_bt_is_enabled(void) {
    if (!s_inited) return false;
    if (!lock_ms(50)) return s_enabled;
    const bool v = s_enabled;
    unlock();
    return v;
}

void nv_bt_status(nv_bt_status_t *out) {
    if (!out) return;
    memset(out, 0, sizeof *out);
    if (!s_inited || !lock_ms(50)) return;
    switch (s_phase) {
    case PH_OFF:      out->state = s_enabled ? NV_BT_STARTING : NV_BT_OFF; break;
    case PH_STARTING: out->state = NV_BT_STARTING; break;
    case PH_ERROR:    out->state = NV_BT_ERROR; break;
    case PH_RUNNING:
        out->state = s_m_scanning ? NV_BT_SCANNING : s_m_connecting ? NV_BT_CONNECTING : NV_BT_READY;
        break;
    }
    if (!s_enabled && s_phase != PH_OFF) out->state = NV_BT_OFF;   // switching off
    out->n_connected = s_m_npads;
    out->n_paired = (uint8_t)s_nbonds;
    snprintf(out->error, sizeof out->error, "%s", s_error);
    snprintf(out->busy_name, sizeof out->busy_name, "%s", s_busy);
    unlock();
}

bool nv_bt_scan_start(int seconds) {
    if (!s_inited) return false;
    if (seconds < 1) seconds = 1;
    if (seconds > 60) seconds = 60;
    if (!lock_ms(50)) return false;
    const bool ok = s_running && s_synced;
    if (ok) {
        s_nscan = 0;                               // a new scan starts with a fresh list
        s_error[0] = 0;
        s_cmd_scan_secs = seconds;
        s_cmd_bits |= CMD_SCAN;
        s_cmd_bits &= ~CMD_SCAN_STOP;
        s_m_scanning = true;                       // shown at once; the host confirms / clears it
        ble_npl_eventq_put(nimble_port_get_dflt_eventq(), &s_cmd_ev);
    }
    unlock();
    return ok;
}

void nv_bt_scan_stop(void) {
    if (!s_inited || !lock_ms(50)) return;
    if (s_running) {
        s_cmd_bits |= CMD_SCAN_STOP;
        s_cmd_bits &= ~CMD_SCAN;
        ble_npl_eventq_put(nimble_port_get_dflt_eventq(), &s_cmd_ev);
    }
    unlock();
}

int nv_bt_scan_results(nv_bt_device_t *out, int max) {
    if (!s_inited || !out || max <= 0 || !lock_ms(50)) return 0;
    int n = s_nscan < max ? s_nscan : max;
    // Strongest first: partial selection sort over the whole list, copying the best n.
    bool taken[SCAN_MAX] = {0};
    for (int k = 0; k < n; k++) {
        int best = -1;
        for (int i = 0; i < s_nscan; i++)
            if (!taken[i] && (best < 0 || s_scan[i].rssi > s_scan[best].rssi)) best = i;
        taken[best] = true;
        out[k] = s_scan[best];
    }
    unlock();
    return n;
}

bool nv_bt_connect(const uint8_t addr[6], uint8_t addr_type) {
    if (!s_inited || !addr || !lock_ms(50)) return false;
    const bool ok = s_running && s_synced;
    if (ok) {
        s_cmd_conn_addr.type = addr_type;
        memcpy(s_cmd_conn_addr.val, addr, 6);
        s_busy[0] = 0;
        for (int i = 0; i < s_nscan; i++)
            if (addr_eq(s_scan[i].addr, s_scan[i].addr_type, &s_cmd_conn_addr))
                snprintf(s_busy, sizeof s_busy, "%s", s_scan[i].name);
        if (!s_busy[0]) nv_bt_addr_str(addr, s_busy);
        s_error[0] = 0;
        s_m_connecting = true;
        s_cmd_bits |= CMD_CONNECT;
        ble_npl_eventq_put(nimble_port_get_dflt_eventq(), &s_cmd_ev);
    }
    unlock();
    return ok;
}

int nv_bt_paired(nv_bt_device_t *out, bool *connected, int max) {
    if (!s_inited || !out || max <= 0 || !lock_ms(50)) return 0;
    const int n = s_nbonds < max ? s_nbonds : max;
    for (int i = 0; i < n; i++) {
        nv_bt_device_t *d = &out[i];
        memset(d, 0, sizeof *d);
        memcpy(d->addr, s_bonds[i].addr, 6);
        d->addr_type = s_bonds[i].type;
        d->hid = true;
        d->paired = true;
        snprintf(d->name, sizeof d->name, "%s", s_bonds[i].name);
        if (connected) {
            connected[i] = false;
            for (int k = 0; k < s_m_nconn; k++)
                if (addr_eq(s_bonds[i].addr, s_bonds[i].type, &s_m_conn[k])) connected[i] = true;
        }
    }
    unlock();
    return n;
}

bool nv_bt_forget(const uint8_t addr[6], uint8_t addr_type) {
    if (!s_inited || !addr || !lock_ms(200)) return false;
    ble_addr_t a = { .type = addr_type };
    memcpy(a.val, addr, 6);
    const bool found = bonds_remove_locked(&a);
    char *out = found ? bonds_serialize_locked() : NULL;
    for (int i = 0; i < s_nscan; i++)
        if (addr_eq(s_scan[i].addr, s_scan[i].addr_type, &a)) s_scan[i].paired = false;
    // Running: the host drops the link and the NimBLE keys now. Off: bonds_reconcile deletes the
    // keys on the next start (the names list is authoritative).
    if (s_running && s_ncmd_forget < FORGET_MAX) {
        s_cmd_forget[s_ncmd_forget++] = a;
        s_cmd_bits |= CMD_FORGET;
        ble_npl_eventq_put(nimble_port_get_dflt_eventq(), &s_cmd_ev);
    }
    unlock();
    bonds_save(out);
    return found;
}

#else  // ---- no NimBLE in this build (CONFIG_BT_ENABLED / ESP_HOSTED_ENABLE_BT_NIMBLE off) ------

static bool s_on;

void nv_bt_init(void) {
    s_on = nv_config_get_bool("bt_on", false);
    NV_LOGI(TAG, "Bluetooth not built into this firmware");
}
void nv_bt_set_enabled(bool on) { s_on = on; nv_config_set_bool("bt_on", on); }
bool nv_bt_is_enabled(void) { return s_on; }
void nv_bt_status(nv_bt_status_t *out) {
    if (!out) return;
    memset(out, 0, sizeof *out);
    out->state = s_on ? NV_BT_ERROR : NV_BT_OFF;
    if (s_on) snprintf(out->error, sizeof out->error, "Bluetooth not built into this firmware");
}
bool nv_bt_scan_start(int seconds) { (void)seconds; return false; }
void nv_bt_scan_stop(void) {}
int  nv_bt_scan_results(nv_bt_device_t *out, int max) { (void)out; (void)max; return 0; }
bool nv_bt_connect(const uint8_t addr[6], uint8_t addr_type) { (void)addr; (void)addr_type; return false; }
int  nv_bt_paired(nv_bt_device_t *out, bool *connected, int max) { (void)out; (void)connected; (void)max; return 0; }
bool nv_bt_forget(const uint8_t addr[6], uint8_t addr_type) { (void)addr; (void)addr_type; return false; }

#endif
