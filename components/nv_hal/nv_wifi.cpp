// nv_wifi — Wi-Fi service (simulated + real esp_wifi_remote backends). See nv_wifi.h.
#include "nv_wifi.h"
#include "nv_log.h"
#include "nv_time.h"   // kick SNTP once we have an IP
#include "nv_event_bus.h"   // publish settings-changed so saved creds get SD-backed-up
#include "nv_config.h"      // persist radio on/off so it survives a reboot (boot auto-join)

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "nvs.h"

#include <cstring>
#include <cstdio>

// Pick the real backend only when esp_wifi_remote is present (component added + built).
#if defined(__has_include)
#  if __has_include(<esp_wifi_remote.h>)
#    define NV_WIFI_REAL 1
#  endif
#endif

static const char *TAG = "wifi";

namespace {

constexpr int kMaxAps   = 24;
constexpr int kMaxSaved = 8;

struct SavedNet { char ssid[33]; char psk[65]; };

// ---- shared state (guarded by s_lock) --------------------------------------
SemaphoreHandle_t s_lock = nullptr;
bool            s_inited   = false;
bool            s_enabled  = false;
nv_wifi_state_t s_state    = NV_WIFI_DISABLED;
nv_wifi_ap_t    s_aps[kMaxAps];
int             s_ap_count = 0;
uint32_t        s_scan_gen = 0;

char    s_conn_ssid[33] = "";
char    s_conn_ip[16]   = "";
char    s_conn_gw[16]   = "";
char    s_conn_mask[16] = "";
char    s_conn_dns[16]  = "";
char    s_conn_mac[18]  = "";
int8_t  s_conn_rssi     = 0;
uint8_t s_conn_auth     = NV_WIFI_AUTH_OPEN;
uint8_t s_conn_gen      = NV_WIFI_GEN_LEGACY;
uint8_t s_conn_band     = NV_WIFI_BAND_24;
uint8_t s_conn_chan     = 0;

SavedNet s_saved[kMaxSaved];
int      s_saved_count = 0;

void lock(void)   { if (s_lock) xSemaphoreTake(s_lock, portMAX_DELAY); }
void unlock(void) { if (s_lock) xSemaphoreGive(s_lock); }

// ---- saved-credential store (plain NVS for now; encrypted NVS is a later hardening) ----
void saved_load(void) {
    s_saved_count = 0;
    nvs_handle_t h;
    if (nvs_open("nvwifi", NVS_READONLY, &h) != ESP_OK) return;
    size_t sz = sizeof(s_saved);
    if (nvs_get_blob(h, "saved", s_saved, &sz) == ESP_OK)
        s_saved_count = (int)(sz / sizeof(SavedNet));
    if (s_saved_count > kMaxSaved) s_saved_count = kMaxSaved;
    nvs_close(h);
}
void saved_store(void) {
    nvs_handle_t h;
    if (nvs_open("nvwifi", NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_blob(h, "saved", s_saved, sizeof(SavedNet) * s_saved_count);
    nvs_commit(h);
    nvs_close(h);
    nv_event_publish(NV_EV_SETTINGS_CHANGED, "wifi");   // triggers the debounced SD backup
}
int saved_find(const char *ssid) {  // caller holds lock
    for (int i = 0; i < s_saved_count; i++)
        if (strcmp(s_saved[i].ssid, ssid) == 0) return i;
    return -1;
}
void saved_put(const char *ssid, const char *psk) {  // caller holds lock
    int i = saved_find(ssid);
    if (i < 0) {
        if (s_saved_count >= kMaxSaved) i = 0;          // evict oldest
        else i = s_saved_count++;
    }
    snprintf(s_saved[i].ssid, sizeof(s_saved[i].ssid), "%s", ssid);
    snprintf(s_saved[i].psk,  sizeof(s_saved[i].psk),  "%s", psk ? psk : "");
    saved_store();
}
void saved_remove(const char *ssid) {  // caller holds lock
    int i = saved_find(ssid);
    if (i < 0) return;
    for (int k = i; k < s_saved_count - 1; k++) s_saved[k] = s_saved[k + 1];
    s_saved_count--;
    saved_store();
}

[[maybe_unused]] uint32_t ssid_hash(const char *s) {  // used only by the simulated backend
    uint32_t h = 5381;
    for (; *s; ++s) h = h * 33u + (uint8_t)*s;
    return h;
}

}  // namespace

// ============================================================================
//  Backend: SIMULATED (default when there is no esp_wifi_remote)
// ============================================================================
#ifndef NV_WIFI_REAL

namespace {

// A curated fake neighbourhood so the UI has realistic content to render — a plausible mix of
// security (Open/WPA2/WPA3) and generations (Wi-Fi 4/6) as a real 2.4 GHz scan would show.
struct DemoAp { const char *ssid; int8_t rssi; uint8_t auth; uint8_t gen; };
const DemoAp kDemo[] = {
    {"Home-6",           -41, NV_WIFI_AUTH_WPA3, NV_WIFI_GEN_WIFI6 },
    {"Home-2.4G",        -54, NV_WIFI_AUTH_WPA2, NV_WIFI_GEN_WIFI4 },
    {"FASTWEB-A1B2C3",   -61, NV_WIFI_AUTH_WPA2, NV_WIFI_GEN_WIFI6 },
    {"TIM-98765432",     -66, NV_WIFI_AUTH_WPA2, NV_WIFI_GEN_WIFI4 },
    {"iPhone di Luca",   -69, NV_WIFI_AUTH_WPA3, NV_WIFI_GEN_WIFI6 },
    {"Vodafone-Guest",   -73, NV_WIFI_AUTH_OPEN, NV_WIFI_GEN_WIFI4 },
    {"Bar Centrale WiFi", -78, NV_WIFI_AUTH_OPEN, NV_WIFI_GEN_LEGACY},
    {"NETGEAR-2G",       -83, NV_WIFI_AUTH_WPA2, NV_WIFI_GEN_LEGACY},
};

esp_timer_handle_t s_timer = nullptr;
enum SimAct { ACT_SCAN_DONE, ACT_CONNECT_OK, ACT_CONNECT_FAIL };
SimAct s_act = ACT_SCAN_DONE;
char   s_pending_ssid[33] = "";

void fill_scan(void) {  // caller holds lock
    s_ap_count = 0;
    for (const auto &d : kDemo) {
        if (s_ap_count >= kMaxAps) break;
        nv_wifi_ap_t &a = s_aps[s_ap_count++];
        snprintf(a.ssid, sizeof(a.ssid), "%s", d.ssid);
        a.rssi    = d.rssi;
        a.auth    = d.auth;
        a.gen     = d.gen;
        a.secured = d.auth != NV_WIFI_AUTH_OPEN;
        a.saved   = saved_find(d.ssid) >= 0;
    }
    s_scan_gen++;
}

void sim_timer_cb(void *) {
    lock();
    switch (s_act) {
        case ACT_SCAN_DONE:
            if (s_enabled) { fill_scan(); s_state = NV_WIFI_IDLE; }
            break;
        case ACT_CONNECT_OK: {
            snprintf(s_conn_ssid, sizeof(s_conn_ssid), "%s", s_pending_ssid);
            snprintf(s_conn_ip, sizeof(s_conn_ip), "192.168.1.%u",
                     (unsigned)(2 + ssid_hash(s_pending_ssid) % 250));
            s_conn_rssi = -50;
            s_conn_auth = NV_WIFI_AUTH_WPA2;
            s_conn_gen  = NV_WIFI_GEN_WIFI4;
            for (int i = 0; i < s_ap_count; i++)
                if (strcmp(s_aps[i].ssid, s_pending_ssid) == 0) {
                    s_conn_rssi = s_aps[i].rssi;
                    s_conn_auth = s_aps[i].auth;
                    s_conn_gen  = s_aps[i].gen;
                    break;
                }
            s_conn_band = NV_WIFI_BAND_24;                       // C6 is 2.4 GHz
            s_conn_chan = (uint8_t)(1 + ssid_hash(s_pending_ssid) % 13);
            s_state = NV_WIFI_CONNECTED;
            fill_scan();  // refresh 'saved' flags
            break;
        }
        case ACT_CONNECT_FAIL:
            s_state = NV_WIFI_FAILED;
            break;
    }
    unlock();
}

void sim_arm(SimAct act, uint64_t us) {
    s_act = act;
    if (!s_timer) {
        const esp_timer_create_args_t a = {sim_timer_cb, nullptr, ESP_TIMER_TASK, "wifisim", true};
        esp_timer_create(&a, &s_timer);
    }
    esp_timer_stop(s_timer);
    esp_timer_start_once(s_timer, us);
}

bool ap_secured(const char *ssid) {  // caller holds lock; unknown SSID assumed secured
    for (int i = 0; i < s_ap_count; i++)
        if (strcmp(s_aps[i].ssid, ssid) == 0) return s_aps[i].secured;
    return true;
}

}  // namespace

static bool backend_has_radio(void) { return false; }

static void backend_enable(bool on) {
    lock();
    s_enabled = on;
    if (on) {
        s_state = NV_WIFI_SCANNING;
        sim_arm(ACT_SCAN_DONE, 1200 * 1000);
    } else {
        if (s_timer) esp_timer_stop(s_timer);
        s_state = NV_WIFI_DISABLED;
        s_ap_count = 0; s_conn_ssid[0] = 0; s_conn_ip[0] = 0;
        s_scan_gen++;
    }
    unlock();
}

static void backend_scan(void) {
    lock();
    if (s_enabled) { s_state = NV_WIFI_SCANNING; sim_arm(ACT_SCAN_DONE, 1200 * 1000); }
    unlock();
}

static void backend_connect(const char *ssid, const char *pass) {
    lock();
    const bool secured = ap_secured(ssid);
    const bool have    = (pass && pass[0]) || saved_find(ssid) >= 0 || !secured;
    if (secured && pass && pass[0]) saved_put(ssid, pass);
    snprintf(s_pending_ssid, sizeof(s_pending_ssid), "%s", ssid);
    s_conn_ssid[0] = 0;
    s_state = NV_WIFI_CONNECTING;
    sim_arm(have ? ACT_CONNECT_OK : ACT_CONNECT_FAIL, 1500 * 1000);
    unlock();
}

static void backend_disconnect(void) {
    lock();
    s_conn_ssid[0] = 0; s_conn_ip[0] = 0;
    if (s_enabled) s_state = NV_WIFI_IDLE;
    unlock();
}

#endif  // !NV_WIFI_REAL

// ============================================================================
//  Backend: REAL esp_wifi_remote (dormant until the component + C6 slave exist)
// ============================================================================
#ifdef NV_WIFI_REAL
#include "esp_wifi.h"          // provided by esp_wifi_remote on the P4
#include "esp_wifi_remote.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "freertos/queue.h"
#include "freertos/task.h"

namespace {
esp_netif_t *s_netif = nullptr;
bool         s_radio_ok = false;
char         s_try_ssid[33] = "";

// Auto-reconnect: on an unsolicited drop we retry with backoff a bounded number of times; a
// user-initiated disconnect/disable sets s_user_disc to suppress it.
bool         s_user_disc = false;
int          s_retries   = 0;
constexpr int kMaxRetries = 4;

// Auto-join: after a scan with no active link we connect to the strongest SAVED access point
// in range. Armed on enable, re-armed when a link dies for good, consumed per attempt; an SSID
// whose stored password failed is skipped so it can't spin a connect->fail->scan loop.
bool         s_did_autoconn = false;
bool         s_assoc        = false;   // STA associated (set pre-DHCP, cleared on disconnect)
char         s_bad_ssid[33] = "";      // AP to skip on auto-join (auth fail or flaky link)
int64_t      s_bad_until_us = 0;       // soft blacklist: s_bad_ssid is skipped only until this time
int          s_recover      = 0;       // recovery scans since the link last worked

// True while s_bad_ssid should be skipped on auto-join. A credential failure blacklists ~forever
// (until reboot / a clean connect); a transient connection failure only briefly, so the preferred
// AP is retried once it recovers — meanwhile auto-join falls back to another saved network.
bool ssid_blacklisted(const char *ssid) {   // caller holds lock
    return s_bad_ssid[0] && strcmp(ssid, s_bad_ssid) == 0 && esp_timer_get_time() < s_bad_until_us;
}

// All blocking esp-hosted calls (bring-up, scan, connect) run on this worker so the LVGL
// thread that toggles Wi-Fi never stalls on the SDIO link to the C6.
enum CmdType { C_ENABLE, C_DISABLE, C_SCAN, C_CONNECT, C_RECONNECT, C_DISCONNECT };
struct Cmd { CmdType t; char ssid[33]; char psk[65]; };
QueueHandle_t s_q = nullptr;
void post(CmdType t, const char *ssid = nullptr, const char *psk = nullptr);  // fwd: used by wifi_evt

// Deferred retry: reconnect backoff (1/2/4/8 s) and self-healing rescans (5 s x3, then every
// 30 s) both funnel through one esp_timer whose callback only re-posts to the worker — no
// blocking esp-hosted call ever runs on the esp_timer task.
esp_timer_handle_t s_retry_timer = nullptr;
enum RetryAct { RA_RECONNECT, RA_SCAN };
RetryAct s_retry_act = RA_SCAN;
void retry_cb(void *) {
    lock(); const RetryAct a = s_retry_act; unlock();
    post(a == RA_RECONNECT ? C_RECONNECT : C_SCAN);
}
void arm_retry(RetryAct act, uint32_t ms) {   // caller holds s_lock
    if (!s_retry_timer) {
        const esp_timer_create_args_t a = {retry_cb, nullptr, ESP_TIMER_TASK, "wifiretry", true};
        esp_timer_create(&a, &s_retry_timer);
    }
    s_retry_act = act;
    esp_timer_stop(s_retry_timer);
    esp_timer_start_once(s_retry_timer, (uint64_t)ms * 1000);
}

// ---- driver enum -> nv_wifi model --------------------------------------------------------
uint8_t map_auth(wifi_auth_mode_t m) {
    switch (m) {
        case WIFI_AUTH_OPEN:            return NV_WIFI_AUTH_OPEN;
        case WIFI_AUTH_WEP:             return NV_WIFI_AUTH_WEP;
        case WIFI_AUTH_WPA_PSK:         return NV_WIFI_AUTH_WPA;
        case WIFI_AUTH_WPA2_PSK:
        case WIFI_AUTH_WPA_WPA2_PSK:    return NV_WIFI_AUTH_WPA2;
        case WIFI_AUTH_WPA3_PSK:
        case WIFI_AUTH_WPA2_WPA3_PSK:   return NV_WIFI_AUTH_WPA3;
        case WIFI_AUTH_WPA2_ENTERPRISE: return NV_WIFI_AUTH_WPA2_ENT;
        default:                        return NV_WIFI_AUTH_WPA2;
    }
}
uint8_t map_gen(const wifi_ap_record_t &r) {
    if (r.phy_11ax) return NV_WIFI_GEN_WIFI6;   // 802.11ax  (Wi-Fi 6)
    if (r.phy_11n)  return NV_WIFI_GEN_WIFI4;   // 802.11n   (Wi-Fi 4)
    return NV_WIFI_GEN_LEGACY;                  // 802.11b/g
}
uint8_t ssid_auth(const char *ssid) {  // caller holds lock; look up the cached scan auth
    for (int i = 0; i < s_ap_count; i++)
        if (strcmp(s_aps[i].ssid, ssid) == 0) return s_aps[i].auth;
    return NV_WIFI_AUTH_WPA2;
}

void store_scan_results(void) {  // caller holds lock
    static wifi_ap_record_t recs[kMaxAps];   // static: keep 24*~80B off the event-task stack
    uint16_t got = kMaxAps;
    if (esp_wifi_scan_get_ap_records(&got, recs) != ESP_OK) { s_scan_gen++; return; }
    s_ap_count = 0;
    for (int i = 0; i < (int)got && s_ap_count < kMaxAps; i++) {
        if (recs[i].ssid[0] == 0) continue;                       // skip hidden SSIDs
        // Dedup: the same SSID often appears on several BSSIDs/channels — keep the strongest.
        int found = -1;
        for (int k = 0; k < s_ap_count; k++)
            if (strcmp((const char *)recs[i].ssid, s_aps[k].ssid) == 0) { found = k; break; }
        if (found >= 0) {
            if (recs[i].rssi > s_aps[found].rssi) {
                s_aps[found].rssi = recs[i].rssi;
                s_aps[found].auth = map_auth(recs[i].authmode);
                s_aps[found].gen  = map_gen(recs[i]);
            }
            continue;
        }
        nv_wifi_ap_t &a = s_aps[s_ap_count++];
        snprintf(a.ssid, sizeof(a.ssid), "%s", (const char *)recs[i].ssid);
        a.rssi    = recs[i].rssi;
        a.auth    = map_auth(recs[i].authmode);
        a.gen     = map_gen(recs[i]);
        a.secured = a.auth != NV_WIFI_AUTH_OPEN;
        a.saved   = saved_find(a.ssid) >= 0;
    }
    // Strongest first (insertion sort — n is tiny).
    for (int i = 1; i < s_ap_count; i++) {
        nv_wifi_ap_t key = s_aps[i]; int j = i - 1;
        while (j >= 0 && s_aps[j].rssi < key.rssi) { s_aps[j + 1] = s_aps[j]; j--; }
        s_aps[j + 1] = key;
    }
    s_scan_gen++;
}

void wifi_evt(void *, esp_event_base_t base, int32_t id, void *data) {
    if (base == WIFI_EVENT && id == WIFI_EVENT_SCAN_DONE) {
        char ac_ssid[33] = "", ac_psk[65] = "";
        lock();
        store_scan_results();
        // Only SCANNING collapses to IDLE: a scan runs fine while associated, so completing one
        // must not clobber CONNECTED/CONNECTING (it made the UI + keydeck think the link died).
        if (s_enabled && s_state == NV_WIFI_SCANNING) s_state = NV_WIFI_IDLE;
        if (s_enabled && !s_user_disc && !s_did_autoconn) {
            // Strongest saved AP in range — s_aps is RSSI-sorted; skip the SSID whose stored
            // password just failed (retrying it forever would loop scan->fail->scan).
            int best = -1, bsi = -1;
            for (int i = 0; i < s_ap_count && best < 0; i++)
                if (s_aps[i].saved && !ssid_blacklisted(s_aps[i].ssid)) {
                    bsi = saved_find(s_aps[i].ssid);
                    if (bsi >= 0) best = i;
                }
            // Network we're already on (or associating to — the C6 slave re-joins its last AP
            // on its own, before our scan finishes and without going through do_connect()).
            const char *cur = s_conn_ssid[0] ? s_conn_ssid : (s_assoc ? s_try_ssid : "");
            if (best >= 0) {
                // Auto-join ONLY when we're not already on a saved link. Never leave a working
                // association to chase a "stronger" saved AP: an extender can advertise strong RSSI
                // yet refuse or stall the association with no DISCONNECTED event, stranding the board
                // (observed: roaming to an "_EXT" extender SSID hung with no recovery). Staying put on
                // whatever the C6 slave/first scan settled on is far more reliable than roaming.
                const bool join = (cur[0] == 0);
                s_did_autoconn = true;   // evaluation consumed either way
                if (join) {
                    // %.NNs bounds the read (fixed arrays gcc can't prove NUL-terminated).
                    snprintf(ac_ssid, sizeof(ac_ssid), "%.32s", s_aps[best].ssid);
                    snprintf(ac_psk,  sizeof(ac_psk),  "%.64s", s_saved[bsi].psk);
                    s_state = NV_WIFI_CONNECTING;
                }
            } else if (cur[0] == 0 && s_saved_count > 0) {
                // No known network in range yet: keep looking — fast right after boot, then a
                // slow steady poll so we re-join when a saved AP comes back into range.
                s_recover++;
                arm_retry(RA_SCAN, s_recover <= 3 ? 5000 : 30000);
            }
        }
        unlock();
        if (ac_ssid[0]) { NV_LOGI(TAG, "auto-join '%s'", ac_ssid); post(C_CONNECT, ac_ssid, ac_psk); }
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_CONNECTED) {
        // Associated (pre-DHCP). Capture the real SSID now — covers the case where the C6 slave
        // auto-connected on its own, so s_try_ssid was never set by our do_connect().
        auto *c = (wifi_event_sta_connected_t *)data;
        lock();
        s_assoc = true;
        if (c && c->ssid[0]) {
            int n = c->ssid_len; if (n <= 0 || n > 32) n = (int)strnlen((char *)c->ssid, 32);
            snprintf(s_try_ssid, sizeof(s_try_ssid), "%.*s", n, (const char *)c->ssid);
        }
        unlock();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        auto *d = (wifi_event_sta_disconnected_t *)data;
        const int reason = d ? d->reason : 0;
        NV_LOGW(TAG, "disconnected from '%s' reason=%d", s_try_ssid, reason);
        // Credential/auth failures are terminal: retrying a wrong password 4x just hangs the UI
        // on "Connecting". Transient RF drops (beacon loss, AP reboot) still get the retry budget.
        const bool auth_fail =
            reason == WIFI_REASON_AUTH_FAIL ||
            reason == WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT ||
            reason == WIFI_REASON_HANDSHAKE_TIMEOUT ||
            reason == WIFI_REASON_MIC_FAILURE ||
            reason == WIFI_REASON_AUTH_EXPIRE;
        lock();
        s_assoc = false;
        s_conn_ssid[0] = 0; s_conn_ip[0] = 0;
        if (!s_enabled) {
            s_state = NV_WIFI_DISABLED;
        } else if (s_user_disc) {
            s_state = NV_WIFI_IDLE;
        } else if (!auth_fail && s_try_ssid[0] && s_retries < kMaxRetries) {
            // Transient drop -> retry with backoff (1/2/4/8 s). Instant retries burned the
            // whole budget in ~2 s, far less than an AP reboot takes.
            s_retries++;
            s_state = NV_WIFI_CONNECTING;
            arm_retry(RA_RECONNECT, 500u << s_retries);
        } else {
            s_state = NV_WIFI_FAILED;              // wrong password / retry budget exhausted
            if (s_try_ssid[0]) {
                // Skip this AP on the next auto-join so we FALL BACK to another saved network:
                // ~forever for a credential failure (wrong password), but only 60 s for a transient
                // connection failure (flaky AP/extender) so the preferred AP is retried once it heals.
                snprintf(s_bad_ssid, sizeof(s_bad_ssid), "%s", s_try_ssid);
                s_bad_until_us = auth_fail ? INT64_MAX : esp_timer_get_time() + 60 * 1000000LL;
            }
            if (s_saved_count > 0) {
                // Self-heal: fall back to scan + auto-join (which skips s_bad_ssid) so the
                // board reconnects by itself once an AP is reachable again.
                s_did_autoconn = false;
                s_retries = 0;
                s_recover++;
                arm_retry(RA_SCAN, s_recover <= 3 ? 5000 : 30000);
            }
        }
        unlock();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        auto *e = (ip_event_got_ip_t *)data;
        lock();
        snprintf(s_conn_ssid, sizeof(s_conn_ssid), "%s", s_try_ssid);
        snprintf(s_conn_ip,   sizeof(s_conn_ip),   IPSTR, IP2STR(&e->ip_info.ip));
        snprintf(s_conn_gw,   sizeof(s_conn_gw),   IPSTR, IP2STR(&e->ip_info.gw));
        snprintf(s_conn_mask, sizeof(s_conn_mask), IPSTR, IP2STR(&e->ip_info.netmask));
        esp_netif_dns_info_t dns;
        if (s_netif && esp_netif_get_dns_info(s_netif, ESP_NETIF_DNS_MAIN, &dns) == ESP_OK)
            snprintf(s_conn_dns, sizeof(s_conn_dns), IPSTR, IP2STR(&dns.ip.u_addr.ip4));
        else
            s_conn_dns[0] = 0;
        uint8_t mac[6];
        if (esp_wifi_get_mac(WIFI_IF_STA, mac) == ESP_OK)
            snprintf(s_conn_mac, sizeof(s_conn_mac), "%02X:%02X:%02X:%02X:%02X:%02X",
                     mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
        wifi_ap_record_t ap;
        if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
            // SSID straight from the driver = authoritative. Needed because the C6 esp-hosted slave
            // can auto-connect from its own stored creds without going through our do_connect(), so
            // s_try_ssid may be empty; ap.ssid is the network we're actually on.
            if (ap.ssid[0]) snprintf(s_conn_ssid, sizeof(s_conn_ssid), "%.32s", (const char *)ap.ssid);
            s_conn_rssi = ap.rssi;
            s_conn_auth = map_auth(ap.authmode);
            s_conn_gen  = map_gen(ap);              // negotiated PHY (Wi-Fi 6 / 4 / legacy)
            s_conn_chan = ap.primary;
        } else {
            s_conn_rssi = -50;
        }
        s_conn_band = NV_WIFI_BAND_24;              // C6 associates on 2.4 GHz
        s_retries   = 0;                            // clean link -> reset the retry budget
        s_recover   = 0;                            // ...and the recovery-scan cadence
        s_bad_ssid[0] = 0; s_bad_until_us = 0;      // clear the fallback blacklist on a clean link
        s_state = NV_WIFI_CONNECTED;
        unlock();
        NV_LOGI(TAG, "connected '%s' ip=%s", s_conn_ssid, s_conn_ip);
        nv_time_notify_online();                    // online -> start SNTP (idempotent)
    }
}

bool radio_bringup(void) {
    if (s_radio_ok) return true;
    // Each one-time step is guarded so a failed bring-up can be retried (C6 still booting at
    // power-on): esp_netif_init / netif creation / handler registration must not run twice.
    static bool s_base_ok = false;
    if (!s_base_ok) {
        if (nvs_flash_init() == ESP_ERR_NVS_NO_FREE_PAGES) { nvs_flash_erase(); nvs_flash_init(); }
        if (esp_netif_init() != ESP_OK) return false;
        esp_event_loop_create_default();
        s_netif = esp_netif_create_default_wifi_sta();
        esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_evt, nullptr, nullptr);
        esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_evt, nullptr, nullptr);
        s_base_ok = true;
    }
    static bool s_wifi_inited = false;
    if (!s_wifi_inited) {
        wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
        if (esp_wifi_init(&cfg) != ESP_OK) return false;   // fails if the C6/hosted link is down
        s_wifi_inited = true;
    }
    esp_wifi_set_storage(WIFI_STORAGE_RAM);            // nv_wifi owns credential persistence (NVS)
    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_wifi_set_country_code("01", true);            // world-safe regulatory domain
    if (esp_wifi_start() != ESP_OK) return false;
    // Advertise Wi-Fi 6 (802.11ax) yet keep the full legacy set so we still associate with old
    // b/g/n access points — the AP picks the highest common PHY.
    uint8_t proto = WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G | WIFI_PROTOCOL_11N;
#ifdef WIFI_PROTOCOL_11AX
    proto |= WIFI_PROTOCOL_11AX;
#endif
    esp_wifi_set_protocol(WIFI_IF_STA, proto);
    esp_wifi_set_ps(WIFI_PS_MIN_MODEM);               // Wi-Fi 6 TWT-friendly modem-sleep
    s_radio_ok = true;
    return true;
}

void start_scan(void) {
    // Longer per-channel dwell than the ~120 ms default: distant / slow-beacon APs otherwise
    // miss the scan window, which broke "join the strongest network" right after boot.
    wifi_scan_config_t sc = {};              // all channels, all SSIDs
    sc.scan_time.active.min = 120;
    sc.scan_time.active.max = 240;
    esp_wifi_scan_start(&sc, false);
}

void do_connect(const char *ssid, const char *psk) {
    esp_wifi_scan_stop();   // a scan started on enable is still running -> it blocks association
    snprintf(s_try_ssid, sizeof(s_try_ssid), "%s", ssid);
    uint8_t auth;
    lock();
    s_user_disc = false; s_retries = 0; s_recover = 0; s_did_autoconn = true;
    if (s_retry_timer) esp_timer_stop(s_retry_timer);   // kill stale backoff/rescan shots
    auth = ssid_auth(ssid);
    unlock();

    wifi_config_t wc = {};   // zero-init: unused SSID/PSK bytes stay 0
    memcpy(wc.sta.ssid, ssid, strnlen(ssid, sizeof(wc.sta.ssid)));
    memcpy(wc.sta.password, psk, strnlen(psk, sizeof(wc.sta.password)));
    // Accept WPA2 *and* WPA3 (transition mode) while refusing to silently downgrade below WPA2
    // on a secured network; open networks associate with no threshold.
    wc.sta.threshold.authmode = (auth == NV_WIFI_AUTH_OPEN) ? WIFI_AUTH_OPEN : WIFI_AUTH_WPA2_PSK;
    wc.sta.pmf_cfg.capable  = true;                   // PMF for WPA3 / PMF-optional APs
    wc.sta.pmf_cfg.required = false;
    wc.sta.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;           // accept either WPA3 SAE PWE method
    esp_wifi_set_config(WIFI_IF_STA, &wc);
    if (esp_wifi_connect() == ESP_ERR_WIFI_CONN) {
        // Already associated to a different AP — drop it. The disconnect handler then treats the
        // drop as transient and reconnects to s_try_ssid (already updated to the new network).
        esp_wifi_disconnect();
    }
}

void worker(void *) {
    Cmd c;
    for (;;) {
        if (xQueueReceive(s_q, &c, portMAX_DELAY) != pdTRUE) continue;
        switch (c.t) {
            case C_ENABLE: {
                bool ok = radio_bringup();
                for (int i = 0; !ok && i < 2; i++) {   // C6 may still be booting -> brief retry
                    vTaskDelay(pdMS_TO_TICKS(2000));
                    ok = radio_bringup();
                }
                if (!ok) {   // C6 slave missing / link down -> report, no crash
                    lock(); s_state = NV_WIFI_FAILED; s_enabled = false; unlock();
                    NV_LOGW(TAG, "radio bring-up failed (C6/esp-hosted slave present?)");
                    break;
                }
                lock(); s_state = NV_WIFI_SCANNING; unlock();
                start_scan();
                break;
            }
            case C_DISABLE:
                lock();
                s_user_disc = true;                       // suppress auto-reconnect
                if (s_retry_timer) esp_timer_stop(s_retry_timer);
                s_retries = 0; s_recover = 0;
                unlock();
                if (s_radio_ok) esp_wifi_disconnect();
                lock(); s_state = NV_WIFI_DISABLED; s_ap_count = 0; s_conn_ssid[0] = 0;
                s_scan_gen++; unlock();
                break;
            case C_SCAN:
                if (s_radio_ok) start_scan();
                break;
            case C_CONNECT:
                if (s_radio_ok) do_connect(c.ssid, c.psk);
                break;
            case C_RECONNECT: {   // delayed retry armed by the disconnect handler
                bool go;
                lock(); go = s_enabled && !s_user_disc && s_state == NV_WIFI_CONNECTING; unlock();
                if (go && s_radio_ok) esp_wifi_connect();
                break;
            }
            case C_DISCONNECT:
                lock();
                s_user_disc = true;                        // user asked -> no auto-reconnect
                if (s_retry_timer) esp_timer_stop(s_retry_timer);
                s_retries = 0; s_recover = 0;
                unlock();
                if (s_radio_ok) esp_wifi_disconnect();
                lock();
                s_conn_ssid[0] = 0; s_conn_ip[0] = 0;
                if (s_enabled) s_state = NV_WIFI_IDLE;
                unlock();
                break;
        }
    }
}

void post(CmdType t, const char *ssid, const char *psk) {   // defaults on the fwd decl above
    if (!s_q) {
        s_q = xQueueCreate(4, sizeof(Cmd));
        // Stack stays INTERNAL: this worker calls nvs_commit() (flash write), which runs with the
        // flash cache disabled — a PSRAM stack would be unreachable then and crash.
        xTaskCreate(worker, "nvwifi", 4096, nullptr, 5, nullptr);
    }
    Cmd c = {};
    c.t = t;
    if (ssid) snprintf(c.ssid, sizeof(c.ssid), "%s", ssid);
    if (psk)  snprintf(c.psk, sizeof(c.psk), "%s", psk);
    xQueueSend(s_q, &c, 0);
}
}  // namespace

static bool backend_has_radio(void) { return s_radio_ok; }

static void backend_enable(bool on) {
    lock();
    s_enabled = on;
    s_state = on ? NV_WIFI_SCANNING : NV_WIFI_DISABLED;
    if (on) {
        // Fresh enable-cycle: re-arm auto-join and clear the leftovers of the previous cycle.
        // s_user_disc in particular stays true after off->on and silently killed auto-join.
        s_did_autoconn = false;
        s_user_disc    = false;
        s_recover      = 0;
        s_bad_ssid[0]  = 0;
    }
    unlock();
    post(on ? C_ENABLE : C_DISABLE);
}

static void backend_scan(void) {
    lock(); if (s_enabled) s_state = NV_WIFI_SCANNING; unlock();
    post(C_SCAN);
}

static void backend_connect(const char *ssid, const char *pass) {
    char psk[65] = "";
    lock();
    int si = saved_find(ssid);
    if (pass && pass[0]) { snprintf(psk, sizeof(psk), "%s", pass); saved_put(ssid, pass); }
    else if (si >= 0)    snprintf(psk, sizeof(psk), "%s", s_saved[si].psk);
    s_state = NV_WIFI_CONNECTING;
    s_bad_ssid[0] = 0;   // explicit user gesture -> that SSID gets a fresh chance
    unlock();
    post(C_CONNECT, ssid, psk);
}

static void backend_disconnect(void) { post(C_DISCONNECT); }
#endif  // NV_WIFI_REAL

// ============================================================================
//  Public API (backend-agnostic)
// ============================================================================
void nv_wifi_init(void) {
    if (s_inited) return;
    s_inited = true;
    s_lock = xSemaphoreCreateMutex();
    lock(); saved_load(); unlock();
    NV_LOGI(TAG, "wifi service ready (%s backend)",
            backend_has_radio() ? "real" : "simulated");
    // Restore the last power state: if Wi-Fi was on before reboot, bring the radio up now so the
    // initial scan + auto-join to the strongest saved network happen without user interaction.
    if (nv_config_get_bool("wifi_on", false)) {
        NV_LOGI(TAG, "restoring Wi-Fi ON at boot");
        backend_enable(true);
    }
}

bool nv_wifi_has_radio(void) { return backend_has_radio(); }

void nv_wifi_set_enabled(bool on) {
    if (!s_inited) return;
    nv_config_set_bool("wifi_on", on);   // survives reboot -> boot auto-join
    backend_enable(on);
}
bool nv_wifi_is_enabled(void) { bool v; lock(); v = s_enabled; unlock(); return v; }

nv_wifi_state_t nv_wifi_get_state(void) { nv_wifi_state_t s; lock(); s = s_state; unlock(); return s; }

void nv_wifi_start_scan(void) { if (s_inited) backend_scan(); }

uint32_t nv_wifi_scan_generation(void) { uint32_t g; lock(); g = s_scan_gen; unlock(); return g; }

int nv_wifi_copy_aps(nv_wifi_ap_t *buf, int max) {
    lock();
    int n = s_ap_count < max ? s_ap_count : max;
    for (int i = 0; i < n; i++) buf[i] = s_aps[i];
    unlock();
    return n;
}

void nv_wifi_connect(const char *ssid, const char *pass) {
    if (s_inited && ssid && ssid[0]) backend_connect(ssid, pass);
}
void nv_wifi_disconnect(void) { if (s_inited) backend_disconnect(); }

bool nv_wifi_get_connected(char *ssid_out, size_t ssid_n,
                           char *ip_out, size_t ip_n, int8_t *rssi_out) {
    bool ok;
    lock();
    ok = (s_state == NV_WIFI_CONNECTED) && s_conn_ssid[0] != 0;
    if (ok) {
        if (ssid_out && ssid_n) snprintf(ssid_out, ssid_n, "%s", s_conn_ssid);
        if (ip_out && ip_n)     snprintf(ip_out, ip_n, "%s", s_conn_ip);
        if (rssi_out)           *rssi_out = s_conn_rssi;
    }
    unlock();
    return ok;
}

bool nv_wifi_get_link(nv_wifi_link_t *out) {
    if (!out) return false;
    bool ok;
    lock();
    ok = (s_state == NV_WIFI_CONNECTED) && s_conn_ssid[0] != 0;
    if (ok) {
        snprintf(out->ssid,    sizeof(out->ssid),    "%s", s_conn_ssid);
        snprintf(out->ip,      sizeof(out->ip),      "%s", s_conn_ip);
        snprintf(out->gateway, sizeof(out->gateway), "%s", s_conn_gw);
        snprintf(out->netmask, sizeof(out->netmask), "%s", s_conn_mask);
        snprintf(out->dns,     sizeof(out->dns),     "%s", s_conn_dns);
        snprintf(out->mac,     sizeof(out->mac),     "%s", s_conn_mac);
        out->rssi    = s_conn_rssi;
        out->auth    = s_conn_auth;
        out->gen     = s_conn_gen;
        out->band    = s_conn_band;
        out->channel = s_conn_chan;
    }
    unlock();
    return ok;
}

const char *nv_wifi_auth_label(uint8_t auth) {
    switch (auth) {
        case NV_WIFI_AUTH_OPEN:     return "Open";
        case NV_WIFI_AUTH_WEP:      return "WEP";
        case NV_WIFI_AUTH_WPA:      return "WPA";
        case NV_WIFI_AUTH_WPA3:     return "WPA3";
        case NV_WIFI_AUTH_WPA2_ENT: return "WPA2-E";
        default:                    return "WPA2";
    }
}
const char *nv_wifi_gen_label(uint8_t gen) {
    switch (gen) {
        case NV_WIFI_GEN_WIFI4: return "Wi-Fi 4";
        case NV_WIFI_GEN_WIFI5: return "Wi-Fi 5";
        case NV_WIFI_GEN_WIFI6: return "Wi-Fi 6";
        default:                return "";
    }
}

bool nv_wifi_is_saved(const char *ssid) {
    bool v; lock(); v = saved_find(ssid) >= 0; unlock(); return v;
}
void nv_wifi_forget(const char *ssid) {
    lock();
    saved_remove(ssid);
    if (strcmp(s_conn_ssid, ssid) == 0) { s_conn_ssid[0] = 0; s_conn_ip[0] = 0;
        if (s_enabled) s_state = NV_WIFI_IDLE; }
    for (int i = 0; i < s_ap_count; i++)
        if (strcmp(s_aps[i].ssid, ssid) == 0) s_aps[i].saved = false;
    s_scan_gen++;
    unlock();
}
