// nv_ota — firmware updater, layout v2 (recovery + system). See nv_ota.h and docs/OTA.md.
#include "nv_ota.h"
#include "nv_fwup.h"
#include "nv_log.h"
#include "nv_seclog.h"
#include "nv_mem_attr.h"   // NV_PSRAM_BSS
#include "nv_config.h"
#include "nv_i18n.h"
#include "nv_sd.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include "esp_attr.h"      // RTC_NOINIT_ATTR: the crash streak survives a reset
#include "esp_heap_caps.h"
#include "esp_ota_ops.h"
#include "esp_timer.h"
#include "esp_app_desc.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "esp_vfs_fat.h"   // free space on the card
#include "lwip/netdb.h"
#include "mbedtls/sha256.h"
#include "cJSON.h"

#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <cerrno>
#include <strings.h>   // strcasecmp

static const char *TAG = "ota";

namespace pol = nv_fwup_policy;

// The recovery app built with this firmware (top-level CMakeLists.txt): installed into the
// `recovery` slot once this image has been confirmed, so recovery fixes reach devices in the field.
extern const uint8_t rec_bin_start[] asm("_binary_nucleo_recovery_bin_start");
extern const uint8_t rec_bin_end[]   asm("_binary_nucleo_recovery_bin_end");

namespace {

SemaphoreHandle_t s_lock = nullptr;
volatile nv_ota_state_t s_state = NV_OTA_IDLE;
volatile int  s_progress = 0;
volatile uint32_t s_gen  = 0;
char s_msg[160]      = "";
char s_avail_ver[32] = "";
NV_PSRAM_BSS char s_bin_url[256];
bool s_busy = false;   // a worker task (or the recovery self-update) is running

// What the last verified manifest promised (guarded by s_lock). Installed only if the staged image
// hashes to it; recovery checks it all again before touching the system slot.
NV_PSRAM_BSS nv_fwup_manifest_t s_expect;   // cold: read once per install
bool s_expect_set = false;

NV_PSRAM_BSS char s_boot_notice[200];   // what recovery did on the way to this boot, told once (UI)
bool s_boot_notice_taken = false;

// ---- health ----
volatile bool s_pending = false;     // this image is on probation (PENDING_VERIFY)
volatile bool s_confirmed = false;   // confirmed during this boot, or already VALID at boot
volatile bool s_net_proven = false;  // the update server answered during this boot
volatile bool s_ui_expected = false;
bool (*volatile s_ui_probe)(void) = nullptr;
NV_PSRAM_BSS char s_health[96];

// ---- fault drill (nv_ota_drill / test builds): this boot fails on purpose ----
// "" | "boot" (dies 8 s into probation) | "ui" (UI stops answering) | "net" (update server never
// reached) | "late" (dies 120 s after boot, every boot: a crash loop after confirmation).
char s_fault[8] = "";

// ---- the safety net at a glance, for status screens ----
// Cached: nv_ota_safety_text() runs on the terminal's and the UI's tasks, whose stacks may sit in
// PSRAM, and a flash read from a PSRAM stack asserts (the cache is off during the read). Refreshed by
// refresh_safety() on internal-stack tasks only (boot, the guard, the update workers).
struct Safety { bool lkg; char lkg_ver[32]; uint32_t lkg_size; int rec_ver; bool rec_ok; };
Safety s_safety = {};

// ---- crash streak (survives panics and watchdog resets, not a power cut) ----
struct Streak { uint32_t magic, count, ver_hash, check; };
RTC_NOINIT_ATTR Streak s_rtc;
constexpr uint32_t kStreakMagic = 0x4B525453;   // "STRK"
int s_streak = 0;
pol::Mode s_mode = pol::Mode::Normal;

void lock(void)   { if (s_lock) xSemaphoreTake(s_lock, portMAX_DELAY); }
void unlock(void) { if (s_lock) xSemaphoreGive(s_lock); }

void set_state(nv_ota_state_t st, const char *msg) {
    lock();
    s_state = st;
    if (msg) snprintf(s_msg, sizeof(s_msg), "%s", msg);
    s_gen = s_gen + 1;   // not ++: volatile increment is deprecated in C++20
    unlock();
}
void set_progress(int p) {
    if (p < 0) p = 0; else if (p > 100) p = 100;
    if (p == s_progress) return;
    lock(); s_progress = p; s_gen = s_gen + 1; unlock();
}
void progress_cb(int pct, void *) { set_progress(pct); }

const char *running_version(void) {
    const esp_app_desc_t *d = esp_app_get_description();
    return d ? d->version : "?";
}

uint32_t slot_size(void) {
    const esp_partition_t *p = nv_fwup_system_part();
    return p ? (uint32_t)p->size : 0;
}

const char *mount(void) { return nv_sd_mount_point(); }

uint32_t uptime_s(void) { return (uint32_t)(esp_timer_get_time() / 1000000); }

// ------------------------------------------------------------- streak storage
uint32_t streak_check(const Streak &s) { return s.magic ^ s.count ^ s.ver_hash ^ 0xA5A5A5A5u; }
void streak_store(int count) {
    s_rtc.magic = kStreakMagic;
    s_rtc.count = (uint32_t)count;
    s_rtc.ver_hash = pol::crc32(0, running_version(), strlen(running_version()));
    s_rtc.check = streak_check(s_rtc);
    s_streak = count;
}
int streak_load(void) {
    const uint32_t vh = pol::crc32(0, running_version(), strlen(running_version()));
    if (s_rtc.magic != kStreakMagic || s_rtc.check != streak_check(s_rtc) || s_rtc.ver_hash != vh) return 0;
    return (int)s_rtc.count;
}

// ------------------------------------------------------------- recovery facts
// Version of the recovery app in flash as A*10000+B*100+C (0 if unreadable). 2.x reads the journal
// and keeps the LKG safety copy.
int recovery_version(void) {
    const esp_partition_t *r = nv_fwup_recovery_part();
    esp_app_desc_t d;
    if (!r || esp_ota_get_partition_description(r, &d) != ESP_OK) return 0;
    int a = 0, b = 0, c = 0;
    sscanf(d.version, "%d.%d.%d", &a, &b, &c);
    return a * 10000 + b * 100 + c;
}
bool recovery_ok(void) { return nv_fwup_image_info(nv_fwup_recovery_part(), nullptr, nullptr, 0); }
bool recovery_has_journal(void) { return recovery_version() >= 20000; }

// Flash reads: call from an internal-RAM stack only (see Safety).
void refresh_safety(void) {
    Safety n = {};
    nv_fwup_lkg_info_t li;
    if (nv_fwup_lkg_info(&li)) {
        n.lkg = true;
        snprintf(n.lkg_ver, sizeof n.lkg_ver, "%s", li.version);
        n.lkg_size = li.stored_size;
    }
    n.rec_ver = recovery_version();
    n.rec_ok = recovery_ok();
    lock(); s_safety = n; unlock();
}

// A copy of a DIFFERENT version than the running one that recovery could restore.
bool rescue_source(bool use_card, const nv_fwup_journal_t &j) {
    if (!strcmp(j.no_rescue_ver, running_version())) return false;   // already tried, nothing there
    nv_fwup_lkg_info_t li;
    if (recovery_has_journal() && nv_fwup_lkg_info(&li) && strcmp(li.version, running_version())) return true;
    if (use_card && nv_sd_is_mounted()) {
        char man[64], bin[64];
        nv_fwup_manifest_t m;
        nv_fwup_path(man, sizeof man, mount(), NV_FWUP_PREV_MAN);
        nv_fwup_path(bin, sizeof bin, mount(), NV_FWUP_PREV_BIN);
        FILE *f = fopen(bin, "rb");
        if (f) fclose(f);
        if (f && nv_fwup_manifest_load(man, slot_size(), &m) == NV_FWUP_OK && strcmp(m.version, running_version()))
            return true;
    }
    return false;
}

// Hand over to recovery: restore the safety copy instead of this version. Does not return on success.
bool hand_over(const char *why) {
    NV_LOGE(TAG, "handing over to recovery: %s", why);
    if (recovery_has_journal()) {
        nv_fwup_journal_t j;
        nv_fwup_journal_load(&j);
        j.rescue = 1;
        pol::set_field(j.rescue_ver, sizeof j.rescue_ver, running_version());
        pol::set_field(j.rescue_why, sizeof j.rescue_why, why);
        if (nv_fwup_journal_store(&j) && esp_ota_set_boot_partition(nv_fwup_recovery_part()) == ESP_OK) {
            vTaskDelay(pdMS_TO_TICKS(200));
            esp_restart();
        }
    }
    // Older recovery: mark this image invalid; recovery 1.x then restores nvupd/prev.bin.
    return esp_ota_mark_app_invalid_rollback_and_reboot() == ESP_OK;
}

// ------------------------------------------------------------- manifest signature
bool manifest_verify(const char *json, const char *version, nv_fwup_manifest_t *out) {
    const nv_fwup_err_t e = nv_fwup_manifest_parse(json, slot_size(), out);
    if (e == NV_FWUP_OK && strcmp(out->version, version) == 0) return true;
    NV_LOGE(TAG, "manifest v%s refused: %s", version, nv_fwup_err_str(e));
    char d[NV_SECLOG_DETAIL_MAX];
    snprintf(d, sizeof d, "v%.20s %s", version, e == NV_FWUP_E_SIGNATURE ? "bad signature" : "unsigned manifest");
    nv_seclog_add(NV_SEC_FW_REFUSED, d);
    return false;
}

// ------------------------------------------------------------- network
struct RespBuf { char *buf; int len; int cap; bool overflow; };
esp_err_t http_evt(esp_http_client_event_t *e) {
    if (e->event_id == HTTP_EVENT_ON_DATA && e->user_data) {
        RespBuf *r = (RespBuf *)e->user_data;
        int n = e->data_len;
        // Truncate + flag instead of dropping a chunk that doesn't fit (long `notes`).
        const int room = r->cap - 1 - r->len;
        if (n > room) { n = room; r->overflow = true; }
        if (n > 0) { memcpy(r->buf + r->len, e->data, n); r->len += n; }
    }
    return ESP_OK;
}

// Fetch a small JSON document into `out` (NUL-terminated). True on HTTP 200 + non-empty body.
bool fetch_json(const char *url, char *out, int out_cap) {
    RespBuf rb = { out, 0, out_cap, false };
    esp_http_client_config_t cfg = {};
    cfg.url = url;
    cfg.event_handler = http_evt;
    cfg.user_data = &rb;
    cfg.crt_bundle_attach = esp_crt_bundle_attach;   // https:// manifests
    cfg.timeout_ms = 10000;
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) return false;
    esp_err_t err = esp_http_client_perform(c);
    int status = esp_http_client_get_status_code(c);
    esp_http_client_cleanup(c);
    if (!strcmp(s_fault, "net")) err = ESP_FAIL;   // drill: an image whose update path is broken
    if (err == ESP_OK && status > 0) s_net_proven = true;   // the update path works end to end
    if (err != ESP_OK || status != 200 || rb.len == 0) return false;
    if (rb.overflow) { NV_LOGE(TAG, "%s: larger than %d bytes, refused", url, out_cap); return false; }
    out[rb.len] = '\0';
    return true;
}

// A network interface (Wi-Fi station or Ethernet) is up with an address. Checked before any DNS or
// socket call: the guard starts before esp_netif_init(), and lwIP must not be touched before that.
bool net_up(void) {
    // No netif yet = esp_netif_init() not done: the lookups below are lwIP IPC calls (no lwIP yet).
    if (esp_netif_get_nr_of_ifs() == 0) return false;
    static const char *const kIfs[] = { "WIFI_STA_DEF", "ETH_DEF" };
    for (const char *key : kIfs) {
        esp_netif_t *n = esp_netif_get_handle_from_ifkey(key);
        esp_netif_ip_info_t ip = {};
        if (n && esp_netif_is_netif_up(n) && esp_netif_get_ip_info(n, &ip) == ESP_OK && ip.ip.addr) return true;
    }
    return false;
}

// The host of `url` resolves: we are on a network that reaches the internet (or the LAN server).
bool host_resolves(const char *url) {
    if (!net_up()) return false;
    const char *h = strstr(url, "://");
    h = h ? h + 3 : url;
    char host[96];
    size_t n = strcspn(h, ":/?#");
    if (!n || n >= sizeof host) return false;
    memcpy(host, h, n);
    host[n] = '\0';
    struct addrinfo hints = {};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    struct addrinfo *res = nullptr;
    const int rc = getaddrinfo(host, nullptr, &hints, &res);
    if (res) freeaddrinfo(res);
    return rc == 0;
}

// Any HTTP answer from the update server (TLS handshake included) proves the update path.
bool server_answers(const char *url) {
    NV_PSRAM_BSS static char body[512];
    fetch_json(url, body, sizeof body);
    return s_net_proven;
}

// A version recovery had to roll back is not offered again by the hands-free paths (boot auto-update,
// background watch) - only an explicit "Install" from Settings retries it.
bool is_bad_version(const char *v) {
    char bad[32];
    nv_config_get_str("ota_bad", "", bad, sizeof bad);
    return bad[0] && strcmp(bad, v) == 0;
}

int my_bucket(const char *version) {
    uint8_t mac[6] = {};
    esp_efuse_mac_get_default(mac);
    return pol::rollout_bucket(mac, version);
}

// Parse a manifest body: version/url/notes/rollout + signature. `newer` = strictly newer than running.
// Returns false (state FAILED set) when a newer version is offered but not properly signed.
struct Offer {
    char version[32]; char url[256]; char notes[128];
    bool newer; int rollout; bool in_rollout;
    nv_fwup_manifest_t m;
};
bool parse_offer(const char *body, Offer *o, const char **err) {
    memset(o, 0, sizeof *o);
    cJSON *root = cJSON_Parse(body);
    if (!root) { *err = "Bad manifest"; return false; }
    const cJSON *jver = cJSON_GetObjectItem(root, "version");
    const cJSON *jurl = cJSON_GetObjectItem(root, "url");
    const cJSON *jnot = cJSON_GetObjectItem(root, "notes");
    const cJSON *jrol = cJSON_GetObjectItem(root, "rollout");
    bool ok = cJSON_IsString(jver) && cJSON_IsString(jurl);
    if (!ok) *err = "Manifest missing version/url";
    if (ok) {
        snprintf(o->version, sizeof o->version, "%s", jver->valuestring);
        snprintf(o->url, sizeof o->url, "%s", jurl->valuestring);
        if (cJSON_IsString(jnot)) snprintf(o->notes, sizeof o->notes, "%s", jnot->valuestring);
        // "rollout" is not signed: it only decides WHEN a signed release installs itself.
        o->rollout = cJSON_IsNumber(jrol) ? (int)jrol->valuedouble : 100;
        o->in_rollout = pol::in_rollout(my_bucket(o->version), o->rollout);
        o->newer = nv_fwup_version_newer(o->version, running_version());
        if (o->newer && !manifest_verify(body, o->version, &o->m)) {
            *err = "Update refused: not signed by the release key";
            ok = false;
        }
    }
    cJSON_Delete(root);
    return ok;
}

void remember_offer(const Offer &o) {
    lock();
    snprintf(s_avail_ver, sizeof s_avail_ver, "%s", o.version);
    snprintf(s_bin_url, sizeof s_bin_url, "%s", o.url);
    s_expect = o.m;
    s_expect_set = o.newer;
    unlock();
}

// ------------------------------------------------------------- download
// Open a GET and read the headers, following up to 5 redirects (http->https, GitHub asset -> CDN).
bool open_following_redirects(esp_http_client_handle_t c, int *total) {
    for (int hop = 0;; ++hop) {
        if (esp_http_client_open(c, 0) != ESP_OK) return false;
        *total = (int)esp_http_client_fetch_headers(c);
        const int st = esp_http_client_get_status_code(c);
        const bool redirect = st == 301 || st == 302 || st == 303 || st == 307 || st == 308;
        if (!redirect || hop >= 5 || esp_http_client_set_redirection(c) != ESP_OK) return true;
        NV_LOGI(TAG, "dl: HTTP %d, following the redirect", st);
        esp_http_client_close(c);
    }
}

// Stream `url` to `path` on the card; never more than `cap` bytes.
bool download_to_sd(const char *url, const char *path, uint32_t cap) {
    esp_http_client_config_t cfg = {};
    cfg.url = url;
    cfg.crt_bundle_attach = esp_crt_bundle_attach;
    cfg.timeout_ms = 20000;
    cfg.buffer_size = 4096;
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) return false;
    int total = 0;
    if (!open_following_redirects(c, &total)) { esp_http_client_cleanup(c); return false; }
    FILE *f = fopen(path, "wb");
    if (!f) {
        NV_LOGE(TAG, "dl: fopen('%s') errno=%d", path, errno);
        esp_http_client_close(c); esp_http_client_cleanup(c); return false;
    }
    setvbuf(f, nullptr, _IOFBF, 16 * 1024);
    NV_LOGI(TAG, "dl: %d bytes -> %s", total, path);
    char *buf = (char *)malloc(8192);
    int r = 0; uint32_t done = 0; bool ok = buf != nullptr;
    while (ok && (r = esp_http_client_read(c, buf, 8192)) > 0) {
        if (done + (uint32_t)r > cap) { NV_LOGE(TAG, "dl: bigger than the signed size, aborted"); ok = false; break; }
        if ((int)fwrite(buf, 1, (size_t)r, f) != r) { NV_LOGE(TAG, "dl: write failed (card full?)"); ok = false; break; }
        done += (uint32_t)r;
        if (total > 0) set_progress((int)((int64_t)done * 100 / total));
    }
    if (r < 0) { NV_LOGE(TAG, "dl: read error at %lu", (unsigned long)done); ok = false; }
    free(buf);
    const int status = esp_http_client_get_status_code(c);
    if (fclose(f) != 0) ok = false;
    esp_http_client_close(c);
    esp_http_client_cleanup(c);
    return ok && status == 200 && done > 0;
}

// ------------------------------------------------------------- staging
uint64_t sd_free_bytes(void) {
    uint64_t total = 0, free_b = 0;
    if (esp_vfs_fat_info(mount(), &total, &free_b) != ESP_OK) return 0;
    return free_b;
}

bool cur_manifest_ok(nv_fwup_manifest_t *m) {
    char cur[64];
    nv_fwup_path(cur, sizeof cur, mount(), NV_FWUP_CUR_MAN);
    return nv_fwup_manifest_load(cur, slot_size(), m) == NV_FWUP_OK && !strcmp(m->version, running_version());
}

// Save the running system (and its signed manifest) as nvupd/prev.* so recovery can roll back even
// when the LKG in flash is missing. Needs cur.jsn - the signed manifest of what is installed.
void backup_running(void) {
    char prev_man[64], prev_bin[64];
    nv_fwup_path(prev_man, sizeof prev_man, mount(), NV_FWUP_PREV_MAN);
    nv_fwup_path(prev_bin, sizeof prev_bin, mount(), NV_FWUP_PREV_BIN);
    nv_fwup_manifest_t m;
    if (!cur_manifest_ok(&m)) {
        NV_LOGW(TAG, "no signed manifest for the running v%s: no card copy this time", running_version());
        return;
    }
    nv_fwup_manifest_t have;
    if (nv_fwup_manifest_load(prev_man, slot_size(), &have) == NV_FWUP_OK && !strcmp(have.version, m.version) &&
        nv_fwup_hash_file(prev_bin, &have, nullptr, nullptr) == NV_FWUP_OK)
        return;   // already saved (an earlier attempt)
    set_progress(0);
    set_state(NV_OTA_DOWNLOADING, "Saving the current system for rollback...");
    remove(prev_man);   // a prev.bin without its manifest is never used
    const nv_fwup_err_t e = nv_fwup_backup(nv_fwup_system_part(), &m, prev_bin, progress_cb, nullptr);
    if (e == NV_FWUP_OK && nv_fwup_manifest_save(prev_man, &m))
        NV_LOGI(TAG, "rollback copy of v%s saved on the card", m.version);
    else
        NV_LOGW(TAG, "rollback copy failed: %s", nv_fwup_err_str(e));
}

// After this update, will there be a way back? Recovery 2.x saves the running system into the LKG
// (if it fits the `assets` partition compressed); otherwise the card copy (prev.bin) is the only one.
bool safety_copy_possible(void) {
    const esp_partition_t *a = nv_fwup_assets_part();
    uint32_t len = 0;
    if (recovery_has_journal() && a && nv_fwup_image_info(nv_fwup_system_part(), &len, nullptr, 0) &&
        (uint64_t)len * 7 / 10 + pol::kLkgDataOffset < a->size)   // images compress to ~58 %
        return true;
    nv_fwup_manifest_t m;
    return nv_sd_is_mounted() && cur_manifest_ok(&m);
}

// Image at `bin` (already on the card, matching `m`) -> nvupd/next.*, then point the boot at recovery.
esp_err_t stage_and_arm(const char *bin, const nv_fwup_manifest_t &m, bool hands_free) {
    char next_bin[64], next_man[64];
    nv_fwup_path(next_bin, sizeof next_bin, mount(), NV_FWUP_NEXT_BIN);
    nv_fwup_path(next_man, sizeof next_man, mount(), NV_FWUP_NEXT_MAN);
    if (strcmp(bin, next_bin) != 0) {
        remove(next_bin);
        if (rename(bin, next_bin) != 0) { NV_LOGE(TAG, "stage: rename errno=%d", errno); return ESP_FAIL; }
    }
    backup_running();
    // What recovery must insist on: a hands-free update installs only with a way back.
    if (recovery_has_journal()) {
        nv_fwup_journal_t j;
        nv_fwup_journal_load(&j);
        j.install_flags = hands_free ? pol::kInstallNeedSafetyCopy : 0;
        if (!nv_fwup_journal_store(&j)) NV_LOGW(TAG, "journal write failed (install flags)");
    }
    // next.jsn last: its presence is what tells recovery to install.
    if (!nv_fwup_manifest_save(next_man, &m)) return ESP_FAIL;
    nv_fwup_tries_set(mount(), 0);
    const esp_err_t err = esp_ota_set_boot_partition(nv_fwup_recovery_part());
    if (err != ESP_OK) { NV_LOGE(TAG, "boot -> recovery failed: 0x%x", (int)err); remove(next_man); return err; }
    NV_LOGI(TAG, "v%s staged; recovery installs it on the next restart", m.version);
    refresh_safety();
    return ESP_OK;
}

// Preconditions shared by every install path. Returns a user-facing reason, or nullptr when OK.
const char *install_blocker(uint32_t image_size) {
    if (!nv_fwup_layout_v2()) return "This device needs a one-time reinstall from the web flasher to receive updates";
    if (s_pending && !s_confirmed) return "The last update is still being checked: try again in a few minutes";
    if (!recovery_ok()) return "The recovery app is being repaired: try again in a few minutes";
    if (!nv_sd_is_mounted()) return "Insert a microSD card to update (the update is prepared on the card)";
    if (!nv_fwup_ensure_dir(mount())) return "Cannot write to the SD card";
    // new image + rollback copy of the running one + slack
    const uint64_t need = (uint64_t)image_size + slot_size() / 2 + 4 * 1024 * 1024;
    if (sd_free_bytes() < need) return "Not enough free space on the SD card (about 20 MB needed)";
    return nullptr;
}

// Verify a staged file against `m` (hash + it keeps trusting our key).
const char *verify_staged(const char *path, const nv_fwup_manifest_t &m) {
    set_progress(0);
    set_state(NV_OTA_DOWNLOADING, "Verifying...");
    nv_fwup_err_t e = nv_fwup_hash_file(path, &m, progress_cb, nullptr);
    if (e == NV_FWUP_OK) e = nv_fwup_file_trusts_us(path, m.size, nullptr, nullptr);
    if (e == NV_FWUP_OK) return nullptr;
    char d[NV_SECLOG_DETAIL_MAX];
    snprintf(d, sizeof d, "v%.12s %s", m.version, e == NV_FWUP_E_KEYS ? "drops our key" : "image != manifest");
    nv_seclog_add(NV_SEC_FW_REFUSED, d);
    return e == NV_FWUP_E_KEYS ? "Update refused: that image would not accept further updates"
                               : "Update refused: the image does not match its signature";
}

// Download `url`, verify it against `m`, stage it.
esp_err_t perform_update(const char *url, const nv_fwup_manifest_t &m, bool hands_free, const char **why) {
    if ((*why = install_blocker(m.size))) return ESP_ERR_INVALID_STATE;
    if (hands_free && !safety_copy_possible()) {
        *why = "No safety copy possible: install it from Settings > Update";
        return ESP_ERR_INVALID_STATE;
    }
    char tmp[64];
    nv_fwup_path(tmp, sizeof tmp, mount(), "dl.bin");
    set_progress(0);
    set_state(NV_OTA_DOWNLOADING, "Downloading to the SD card...");
    if (!download_to_sd(url, tmp, m.size)) { remove(tmp); *why = "Download failed"; return ESP_FAIL; }
    if ((*why = verify_staged(tmp, m))) { remove(tmp); return ESP_ERR_INVALID_CRC; }
    const esp_err_t err = stage_and_arm(tmp, m, hands_free);
    if (err != ESP_OK) { remove(tmp); *why = "Cannot prepare the update on the SD card"; }
    return err;
}

// ------------------------------------------------------------- workers
void check_task(void *arg) {
    char *url = (char *)arg;
    set_state(NV_OTA_CHECKING, "Checking for updates...");
    NV_PSRAM_BSS static char body[4096];
    const bool ok = fetch_json(url, body, sizeof(body));
    free(url);
    Offer o;
    const char *err = nullptr;
    if (!ok) {
        set_state(NV_OTA_FAILED, "Cannot reach the update server");
    } else if (!parse_offer(body, &o, &err)) {
        set_state(NV_OTA_FAILED, err);
    } else {
        remember_offer(o);   // a manual check offers a release whatever its rollout
        char m[160];
        if (o.newer && o.notes[0]) snprintf(m, sizeof m, "Version %s: %.100s", o.version, o.notes);
        else snprintf(m, sizeof m, o.newer ? "Version %s available" : "Up to date (%s)", o.version);
        set_state(o.newer ? NV_OTA_AVAILABLE : NV_OTA_UPTODATE, m);
    }
    lock(); s_busy = false; unlock();
    vTaskDelete(nullptr);
}

void update_task(void *) {
    char url[256];
    nv_fwup_manifest_t m;
    bool have;
    lock(); snprintf(url, sizeof(url), "%s", s_bin_url); m = s_expect; have = s_expect_set; unlock();
    const char *why = "No verified update to install";
    const esp_err_t err = have ? perform_update(url, m, false, &why) : ESP_ERR_INVALID_STATE;
    if (err == ESP_OK) {
        nv_config_set_str("ota_bad", "");   // the user chose this version explicitly
        set_progress(100);
        set_state(NV_OTA_SUCCESS, "Update ready - restart to install it");
    } else {
        set_state(NV_OTA_FAILED, why);
        NV_LOGE(TAG, "update failed: %s (0x%x)", why, (int)err);
    }
    lock(); s_busy = false; unlock();
    vTaskDelete(nullptr);
}

// "<name>.bin" on the card + its signed "<name>.json" beside it (release asset nucleos-anima.json).
void install_sd_task(void *arg) {
    char *path = (char *)arg;
    set_progress(0);
    set_state(NV_OTA_DOWNLOADING, "Checking the firmware on the SD card...");
    const char *why = nullptr;
    char man[100];
    const size_t n = strlen(path);
    nv_fwup_manifest_t m;
    FILE *probe = fopen(path, "rb");
    if (!probe) why = "No firmware file on the SD card";
    else fclose(probe);
    if (!why && (n < 4 || n + 2 > sizeof man || strcasecmp(path + n - 4, ".bin"))) why = "Invalid image";
    if (!why) {
        memcpy(man, path, n - 4);
        memcpy(man + n - 4, ".json", 6);
        const nv_fwup_err_t e = nv_fwup_manifest_load(man, slot_size(), &m);
        if (e == NV_FWUP_E_IO) {
            why = "Refused: signed manifest (.json) missing beside the image";
            nv_seclog_add(NV_SEC_FW_REFUSED, "SD image without manifest");
        } else if (e != NV_FWUP_OK) {
            why = "Refused: image not signed by the release key";
            nv_seclog_add(NV_SEC_FW_REFUSED, "SD manifest not signed");
        } else if (!nv_fwup_version_newer(m.version, running_version())) {
            why = "Refused: not newer than the installed firmware";
        }
    }
    if (!why) why = install_blocker(m.size);
    if (!why) why = verify_staged(path, m);
    if (!why) {
        if (stage_and_arm(path, m, false) != ESP_OK) why = "Cannot prepare the update on the SD card";
        else { remove(man); nv_config_set_str("ota_bad", ""); }   // the user chose this version explicitly
    }
    if (!why) {
        set_progress(100);
        set_state(NV_OTA_SUCCESS, "Update ready - restart to install it");
    } else {
        set_state(NV_OTA_FAILED, why);
    }
    free(path);
    lock(); s_busy = false; unlock();
    vTaskDelete(nullptr);
}

// The signed manifest of the version we run, from the release channel ("<base>/<version>.json"), kept
// as nvupd/cur.jsn once the system slot is proven to hash to it. It is what makes the card rollback
// copy possible on a board that was flashed over USB (recovery writes cur.jsn itself after an update).
void ensure_cur_manifest(const char *manifest_url) {
    if (!nv_fwup_layout_v2() || !nv_sd_is_mounted() || !nv_fwup_ensure_dir(mount())) return;
    char cur[64];
    nv_fwup_path(cur, sizeof cur, mount(), NV_FWUP_CUR_MAN);
    nv_fwup_manifest_t m;
    if (nv_fwup_manifest_load(cur, slot_size(), &m) == NV_FWUP_OK && !strcmp(m.version, running_version())) return;
    char url[288];
    const char *slash = strrchr(manifest_url, '/');
    if (!slash) return;
    snprintf(url, sizeof url, "%.*s/%s.json", (int)(slash - manifest_url), manifest_url, running_version());
    NV_PSRAM_BSS static char body[2048];
    if (!fetch_json(url, body, sizeof body)) { NV_LOGW(TAG, "own manifest not found: %s", url); return; }
    if (nv_fwup_manifest_parse(body, slot_size(), &m) != NV_FWUP_OK || strcmp(m.version, running_version())) return;
    if (nv_fwup_hash_partition(nv_fwup_system_part(), &m, nullptr, nullptr) != NV_FWUP_OK) {
        NV_LOGW(TAG, "running image is not the released v%s (local build?)", m.version);
        return;
    }
    if (nv_fwup_manifest_save(cur, &m)) NV_LOGI(TAG, "signed manifest of v%s saved (rollback ready)", m.version);
}

// Boot-time hands-free updater: wait for the network, fetch the manifest, install a newer signed
// version (unless recovery already had to roll that one back, or this device's rollout bucket is not
// reached yet) and restart into recovery. Never while this image is still on probation.
void boot_auto_task(void *arg) {
    char *url = (char *)arg;
    NV_LOGI(TAG, "auto-OTA: start, manifest=%s running=v%s", url, running_version());
    for (int attempt = 1; attempt <= 12; attempt++) {   // ~60 s while Wi-Fi/DHCP settle
        vTaskDelay(pdMS_TO_TICKS(5000));
        NV_PSRAM_BSS static char body[4096];
        if (!fetch_json(url, body, sizeof(body))) {
            NV_LOGW(TAG, "auto-OTA: manifest unreachable (attempt %d/12)", attempt);
            continue;
        }
        Offer o;
        const char *err = nullptr;
        if (!parse_offer(body, &o, &err)) {
            NV_LOGE(TAG, "auto-OTA: %s", err);
            set_state(NV_OTA_FAILED, err);
        } else if (!o.newer) {
            NV_LOGI(TAG, "auto-OTA: v%s is up to date", running_version());
            ensure_cur_manifest(url);
        } else if (is_bad_version(o.version)) {
            remember_offer(o);
            NV_LOGW(TAG, "auto-OTA: v%s was rolled back before, not reinstalled automatically", o.version);
            set_state(NV_OTA_AVAILABLE, "A previous install of this version failed; install it manually to retry");
        } else if (!o.in_rollout) {
            NV_LOGI(TAG, "auto-OTA: v%s is rolling out to %d%% of devices, not this one yet", o.version, o.rollout);
        } else {
            remember_offer(o);
            // A fresh image confirms itself first (probation): an update armed now would replace it
            // before it is proven, and the safety copy would be of an unproven image.
            for (int i = 0; i < 15 * 60 && s_pending && !s_confirmed; i++) vTaskDelay(pdMS_TO_TICKS(1000));
            if (s_pending && !s_confirmed) { NV_LOGW(TAG, "auto-OTA: still on probation, skipped"); break; }
            ensure_cur_manifest(url);   // so this update also leaves a card copy
            const char *why = nullptr;
            NV_LOGI(TAG, "auto-OTA: installing v%s", o.version);
            if (perform_update(o.url, o.m, true, &why) == ESP_OK) {
                set_progress(100);
                set_state(NV_OTA_SUCCESS, "Update prepared - restarting to install");
                vTaskDelay(pdMS_TO_TICKS(1500));
                esp_restart();
            }
            set_state(NV_OTA_FAILED, why);
            NV_LOGE(TAG, "auto-OTA: %s", why);
        }
        break;
    }
    free(url);
    lock(); s_busy = false; unlock();
    vTaskDelete(nullptr);
}

// Background watch tick (see nv_ota_watch_start): one manifest read on a short-lived task.
void watch_task(void *) {
    char url[256];
    nv_ota_get_url(url, sizeof(url));
    NV_PSRAM_BSS static char body[4096];
    Offer o;
    const char *err = nullptr;
    if (!fetch_json(url, body, sizeof(body))) {
        NV_LOGW(TAG, "watch: manifest unreachable (%s)", url);
    } else if (parse_offer(body, &o, &err)) {
        if (o.newer && !is_bad_version(o.version) && o.in_rollout) {
            remember_offer(o);
            char m[64];
            snprintf(m, sizeof(m), "Version %s available", o.version);
            set_state(NV_OTA_AVAILABLE, m);
        } else if (!o.newer) {
            ensure_cur_manifest(url);
        }
    }
    lock(); s_busy = false; unlock();
    vTaskDelete(nullptr);
}

esp_timer_handle_t s_watch_timer = nullptr;
bool s_watch_periodic = false;

void watch_timer_cb(void *) {
    if (!s_watch_periodic) {   // first tick 15 min after boot, then every 6 h
        s_watch_periodic = true;
        esp_timer_start_periodic(s_watch_timer, 6ULL * 3600 * 1000 * 1000);
    }
    lock();
    const bool skip = s_busy || s_state == NV_OTA_SUCCESS;
    if (!skip) s_busy = true;
    unlock();
    if (skip) return;
    // 8 KB internal stack: TLS handshake + cert-bundle verify; may hash the slot (flash reads).
    if (xTaskCreate(watch_task, "ota_watch", 8192, nullptr, 3, nullptr) != pdPASS) {
        lock(); s_busy = false; unlock();
    }
}

// ------------------------------------------------------------- what recovery did
void notice(const char *fmt, const char *a, const char *b) {
    snprintf(s_boot_notice, sizeof s_boot_notice, fmt, a, b);
}

void apply_result(const char *op, bool ok, const char *from, const char *to, const char *why) {
    if (!strcmp(op, "install") && ok) {
        notice(nv_tr(NV_STR_UPDATED_TO), to, "");
        nv_config_set_str("ota_bad", "");
    } else if (!strcmp(op, "install")) {
        notice(nv_tr(NV_STR_UPDATE_FAILED_FMT), to, from);
    } else if (!strcmp(op, "rollback") && ok) {
        notice(nv_tr(NV_STR_UPDATE_ROLLBACK_FMT), from, to);
        nv_config_set_str("ota_bad", from);
    } else if (!strcmp(op, "rescue") && ok) {
        notice(nv_tr(NV_STR_UPDATE_RESCUE_FMT), from, to);
        nv_config_set_str("ota_bad", from);
    } else if (!strcmp(op, "restore") && ok) {
        notice(nv_tr(NV_STR_UPDATED_TO), to, "");
    }
    NV_LOGI(TAG, "recovery: %s %s %s -> %s%s%s", op, ok ? "ok" : "FAILED", from, to, why && why[0] ? ": " : "",
            why ? why : "");
}

void read_boot_result(void) {
    // The journal (recovery 2.x, flash): authoritative, works without a card.
    nv_fwup_journal_t j;
    bool from_journal = false;
    if (nv_fwup_journal_load(&j) && j.res_set) {
        apply_result(j.res_op, j.res_ok, j.res_from, j.res_to, j.res_why);
        j.res_set = 0;
        nv_fwup_journal_store(&j);
        from_journal = true;
    }
    // result.jsn on the card (recovery 1.x; 2.x writes both).
    if (!nv_sd_is_mounted()) return;
    char p[64];
    nv_fwup_path(p, sizeof p, mount(), NV_FWUP_RESULT);
    FILE *f = fopen(p, "rb");
    if (!f) return;
    char buf[512];
    const size_t n = fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    remove(p);   // told once
    if (from_journal) return;
    buf[n] = '\0';
    cJSON *o = cJSON_Parse(buf);
    if (!o) return;
    auto str = [&](const char *k) { const cJSON *v = cJSON_GetObjectItem(o, k); return cJSON_IsString(v) ? v->valuestring : ""; };
    apply_result(str("op"), cJSON_IsTrue(cJSON_GetObjectItem(o, "ok")), str("from"), str("to"), str("why"));
    cJSON_Delete(o);
}

// ------------------------------------------------------------- recovery self-update
// Bring the `recovery` slot in line with the recovery built with this firmware. Only from a confirmed
// image (the bootloader never needs recovery then) and never while an update is armed.
void sync_recovery(void) {
    const esp_partition_t *r = nv_fwup_recovery_part();
    const size_t len = (size_t)(rec_bin_end - rec_bin_start);
    if (!r || len < 1024 || len > r->size) return;
    uint8_t want[32], have[32];
    mbedtls_sha256(rec_bin_start, len, want, 0);
    uint8_t *buf = (uint8_t *)heap_caps_malloc(4096, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!buf) return;
    mbedtls_sha256_context sc;
    mbedtls_sha256_init(&sc);
    mbedtls_sha256_starts(&sc, 0);
    bool rd = true;
    for (size_t off = 0; off < len && rd; off += 4096) {
        const size_t n = len - off < 4096 ? len - off : 4096;
        rd = esp_partition_read(r, off, buf, n) == ESP_OK;
        if (rd) mbedtls_sha256_update(&sc, buf, n);
    }
    mbedtls_sha256_finish(&sc, have);
    mbedtls_sha256_free(&sc);
    if (rd && !memcmp(want, have, 32) && recovery_ok()) {
        free(buf);
        NV_LOGI(TAG, "recovery up to date");
        return;
    }
    lock();
    const bool skip = s_busy || s_state == NV_OTA_SUCCESS;
    if (!skip) s_busy = true;
    unlock();
    if (skip) { free(buf); return; }
    NV_LOGW(TAG, "recovery slot differs from this build's recovery: updating it (%u bytes)", (unsigned)len);
    bool ok = true;
    for (size_t off = 0; off < r->size && off < len && ok; off += 65536) {
        ok = esp_partition_erase_range(r, off, 65536) == ESP_OK;
        vTaskDelay(pdMS_TO_TICKS(5));   // keep the UI breathing between block erases
    }
    for (size_t off = 0; off < len && ok; off += 4096) {
        const size_t n = len - off < 4096 ? len - off : 4096;
        memcpy(buf, rec_bin_start + off, n);   // source in flash: copy out before the write
        ok = esp_partition_write(r, off, buf, n) == ESP_OK;
        if ((off & 0xFFFF) == 0) vTaskDelay(1);
    }
    free(buf);
    ok = ok && recovery_ok();
    NV_LOGI(TAG, "recovery update %s (now %d)", ok ? "done" : "FAILED - retried next boot", recovery_version());
    lock(); s_busy = false; unlock();
}

// ------------------------------------------------------------- the guard
// Watches a fresh image until it proves itself (or rolls it back), clears the crash streak once a
// normal boot is stable, and keeps the recovery app current. Ends when all that is done.
void ui_tick(pol::Probe &p, uint32_t &bad_since) {
    bool ok = true;
    if (s_ui_expected) ok = s_ui_probe ? s_ui_probe() : false;
    if (ok) bad_since = 0;
    else if (!bad_since) bad_since = p.uptime_s ? p.uptime_s : 1;
    p.ui_ok = ok;
    p.ui_bad_s = ok ? 0 : p.uptime_s - bad_since;
}

void guard_task(void *) {
    if (s_pending) {
        pol::Probe p = {};
        uint32_t ui_bad_since = 0, last_net = 0;
        char url[256];
        nv_ota_get_url(url, sizeof url);
        for (;;) {
            p.uptime_s = uptime_s();
            ui_tick(p, ui_bad_since);
            if (!s_net_proven && p.uptime_s >= last_net + 15 && net_up()) {   // ~every 15 s until proven
                last_net = p.uptime_s;
                p.online = host_resolves(url) || host_resolves(NV_OTA_DEFAULT_URL);
                if (p.online && !p.online_first_s) p.online_first_s = p.uptime_s;
                if (p.online && !server_answers(url) && strcmp(url, NV_OTA_DEFAULT_URL)) server_answers(NV_OTA_DEFAULT_URL);
            }
            p.net_proven = s_net_proven;
            const pol::Verdict v = pol::probation(p);
            snprintf(s_health, sizeof s_health, "probation %lu s (%s%s%s)", (unsigned long)p.uptime_s,
                     p.ui_ok ? "UI ok" : "UI not answering", p.net_proven ? ", server ok" : p.online ? ", waiting for the server" : ", offline",
                     "");
            if (v == pol::Verdict::Confirm) {
                if (esp_ota_mark_app_valid_cancel_rollback() == ESP_OK) {
                    s_confirmed = true;
                    snprintf(s_health, sizeof s_health, "confirmed after %lu s", (unsigned long)p.uptime_s);
                    NV_LOGI(TAG, "image v%s confirmed after %lu s (UI %s, server %s) - rollback cancelled",
                            running_version(), (unsigned long)p.uptime_s, s_ui_expected ? "ok" : "n/a",
                            p.net_proven ? "reached" : "offline");
                    break;
                }
            } else if (v == pol::Verdict::Rollback) {
                const char *why = p.ui_bad_s >= pol::kUiHangS ? "UI not responding" : "update server unreachable";
                NV_LOGE(TAG, "image v%s FAILED its probation (%s): rolling back", running_version(), why);
                // Tell recovery why (shown with the rollback notice), then mark this image invalid: the
                // bootloader falls back to recovery, which restores the previous system.
                if (recovery_has_journal()) {
                    nv_fwup_journal_t j;
                    nv_fwup_journal_load(&j);
                    pol::set_field(j.rescue_ver, sizeof j.rescue_ver, running_version());
                    pol::set_field(j.rescue_why, sizeof j.rescue_why, why);
                    nv_fwup_journal_store(&j);
                }
                if (esp_ota_mark_app_invalid_rollback_and_reboot() != ESP_OK) hand_over(why);
                esp_restart();   // last resort: still PENDING, so the bootloader gives up on it
            }
            vTaskDelay(pdMS_TO_TICKS(5000));
        }
    } else {
        snprintf(s_health, sizeof s_health, "confirmed");
    }

    // A normal boot that stays up kStableS clears the crash streak. In safe mode it stays: only a
    // power cycle or "Restart normally" leaves it.
    while (uptime_s() < (uint32_t)pol::kStableS) vTaskDelay(pdMS_TO_TICKS(5000));
    if (s_mode == pol::Mode::Normal && s_streak) {
        NV_LOGI(TAG, "stable for %d s: crash streak %d cleared", pol::kStableS, s_streak);
        streak_store(0);
    }
    sync_recovery();
    refresh_safety();
    vTaskDelete(nullptr);
}

// Fault drill: which failure (if any) this boot acts out. Armed by nv_ota_drill() through NVS
// "ota_drill" = "<version>|<kind>|<boots left>", only for the version that armed it, and consumed as
// it fires: the version recovery restores (and a later reinstall) boots normally. A test build
// (-DNV_OTA_FAULT) fails on every boot instead.
void load_drill(void) {
#ifdef NV_OTA_FAULT
    snprintf(s_fault, sizeof s_fault, "%s", NV_OTA_FAULT);
    return;
#endif
    char d[64];
    nv_config_get_str("ota_drill", "", d, sizeof d);
    if (!d[0]) return;
    char ver[32] = "", kind[8] = "";
    int left = 0;
    if (sscanf(d, "%31[^|]|%7[^|]|%d", ver, kind, &left) != 3 || strcmp(ver, running_version()) || left <= 0) {
        nv_config_set_str("ota_drill", "");   // another version, or spent
        return;
    }
    // boot / ui / net act on a fresh image (the drill re-armed probation); late on any boot.
    if (strcmp(kind, "late") && !s_pending) { nv_config_set_str("ota_drill", ""); return; }
    if (--left > 0) {
        snprintf(d, sizeof d, "%s|%s|%d", ver, kind, left);
        nv_config_set_str("ota_drill", d);
    } else {
        nv_config_set_str("ota_drill", "");
    }
    if (strcmp(kind, "rearm")) snprintf(s_fault, sizeof s_fault, "%s", kind);   // rearm: probation, no fault
}

void inject_fault(void) {
    if (!s_fault[0]) return;
    const char *kFault = s_fault;
    NV_LOGW(TAG, "FAULT DRILL: this boot acts out '%s'", kFault);
    auto later = [](uint32_t s, void (*fn)(void *)) {
        esp_timer_create_args_t a = {};
        a.callback = fn;
        a.name = "ota_fault";
        esp_timer_handle_t t;
        if (esp_timer_create(&a, &t) == ESP_OK) esp_timer_start_once(t, (uint64_t)s * 1000000);
    };
    if (!strcmp(kFault, "boot")) later(8, [](void *) { abort(); });                // dies on probation
    if (!strcmp(kFault, "late")) later(120, [](void *) { abort(); });              // dies after confirmation
                                                                                    // (before kStableS: a crash loop)
    if (!strcmp(kFault, "ui")) {                                                    // UI freezes, no crash
        xTaskCreate([](void *) { vTaskDelay(pdMS_TO_TICKS(15000)); s_ui_probe = []() { return false; };
                                 vTaskDelete(nullptr); }, "ota_fault", 2048, nullptr, 1, nullptr);
    }
}

}  // namespace

// ============================================================= public API
void nv_ota_early_boot(void) {
    const pol::Reset kind = nv_fwup_reset_kind((int)esp_reset_reason());
    streak_store(pol::next_streak(streak_load(), kind));
    esp_ota_img_states_t st;
    const esp_partition_t *run = esp_ota_get_running_partition();
    s_pending = run && esp_ota_get_state_partition(run, &st) == ESP_OK && st == ESP_OTA_IMG_PENDING_VERIFY;
    s_confirmed = !s_pending;
    if (s_streak < pol::kSafeModeAt) return;
    nv_fwup_journal_t j;
    nv_fwup_journal_load(&j);
    s_mode = pol::boot_mode(s_streak, rescue_source(false, j));
    NV_LOGE(TAG, "crash streak %d (last reset %d): %s", s_streak, (int)esp_reset_reason(),
            s_mode == pol::Mode::Rescue ? "asking recovery for the safety copy" : "SAFE MODE");
    if (s_mode == pol::Mode::Rescue && !hand_over("repeated crashes")) s_mode = pol::Mode::Safe;
}

bool nv_ota_safe_mode(void) { return s_mode != pol::Mode::Normal; }

void nv_ota_restart_normal(void) {
    NV_LOGI(TAG, "restart in normal mode requested");
    streak_store(0);
    esp_restart();
}

void nv_ota_init(void) {
    if (!s_lock) s_lock = xSemaphoreCreateMutex();
    read_boot_result();
    if (s_mode != pol::Mode::Normal) {
        snprintf(s_boot_notice, sizeof s_boot_notice, "%s", nv_tr(NV_STR_UPDATE_SAFE_MODE));
        // With the card mounted now, a copy on it may make a rescue possible after all.
        nv_fwup_journal_t j;
        nv_fwup_journal_load(&j);
        if (s_streak >= pol::kRescueAt && rescue_source(true, j)) hand_over("repeated crashes");
    }
    refresh_safety();
    load_drill();
    inject_fault();
    // Rollback: a fresh image is confirmed only once it has proven itself (nv_fwup_policy::probation;
    // the 1.1.57 lesson: marking valid at boot turned a boot-looping image into a brick). One that
    // dies first stays PENDING_VERIFY and the bootloader starts recovery, which restores the previous
    // system. 8 KB internal stack: TLS for the server probe; flash writes (otadata, recovery slot).
    if (xTaskCreate(guard_task, "ota_guard", 8192, nullptr, 4, nullptr) != pdPASS) {
        NV_LOGE(TAG, "guard task: out of memory - confirming the image as before");
        if (s_pending) { esp_ota_mark_app_valid_cancel_rollback(); s_confirmed = true; }
    }
    if (s_pending) NV_LOGI(TAG, "image v%s on probation: confirmed once it proves itself", running_version());
    NV_LOGI(TAG, "OTA service ready, running v%s (%s), recovery %d, %d release keys", running_version(),
            nv_fwup_layout_v2() ? "layout v2" : "LEGACY layout - reinstall from the web flasher",
            recovery_version(), nv_fwup_key_count());
}

void nv_ota_expect_ui(void) { s_ui_expected = true; }
void nv_ota_set_ui_probe(bool (*probe)(void)) {
    if (!strcmp(s_fault, "ui") && s_ui_probe) return;   // keep the drilled fault
    s_ui_probe = probe;
}

const char *nv_ota_drill(const char *kind) {
    static const char *const kKinds[] = { "rearm", "boot", "ui", "net", "late" };
    bool known = false;
    for (const char *k : kKinds) known = known || (kind && !strcmp(kind, k));
    if (!known) return "unknown drill (rearm | boot | ui | net | late)";
    if (!s_confirmed) return "this version is still on probation";
    if (s_mode != pol::Mode::Normal) return "not in safe mode";
    lock();
    const Safety sf = s_safety;
    unlock();
    // Only with a way back: a safety copy of another version that recovery 2.x restores from flash.
    if (!sf.lkg || sf.rec_ver < 20000 || !sf.rec_ok || !strcmp(sf.lkg_ver, running_version()))
        return "no safety copy of another version: install an update first";
    char d[64];
    snprintf(d, sizeof d, "%s|%s|%d", running_version(), kind, !strcmp(kind, "late") ? pol::kRescueAt : 1);
    nv_config_set_str("ota_drill", d);
    static char s_kind[8];
    snprintf(s_kind, sizeof s_kind, "%s", kind);
    // otadata is in flash: re-arm probation and restart on an internal-stack task.
    auto task = [](void *) {
        NV_LOGW(TAG, "fault drill '%s' armed on v%s: restarting", s_kind, running_version());
        if (strcmp(s_kind, "late") &&
            esp_ota_set_boot_partition(esp_ota_get_running_partition()) != ESP_OK) {   // boots PENDING
            nv_config_set_str("ota_drill", "");
            NV_LOGE(TAG, "drill: cannot re-arm probation");
            vTaskDelete(nullptr);
        }
        vTaskDelay(pdMS_TO_TICKS(500));
        esp_restart();
    };
    if (xTaskCreate(task, "ota_drill", 4096, nullptr, 5, nullptr) != pdPASS) {
        nv_config_set_str("ota_drill", "");
        return "out of memory";
    }
    return nullptr;
}

void nv_ota_health_text(char *out, size_t n) {
    if (!out || !n) return;
    snprintf(out, n, "%s%s", s_health[0] ? s_health : (s_pending ? "probation" : "confirmed"),
             s_mode == pol::Mode::Normal ? "" : ", SAFE MODE");
}

void nv_ota_safety_text(char *out, size_t n) {
    if (!out || !n) return;
    lock();
    const Safety sf = s_safety;   // cached: no flash access on the caller's (maybe PSRAM) stack
    unlock();
    char lkg[64] = "none";
    if (sf.lkg) snprintf(lkg, sizeof lkg, "%s (%.1f MB)", sf.lkg_ver, sf.lkg_size / 1048576.0);
    char chan[16];
    nv_ota_get_channel(chan, sizeof chan);
    snprintf(out, n, "safety copy %s, recovery %d.%d.%d%s, streak %d, channel %s, rollout bucket %d",
             lkg, sf.rec_ver / 10000, sf.rec_ver / 100 % 100, sf.rec_ver % 100, sf.rec_ok ? "" : " (BROKEN)",
             s_streak, chan, my_bucket(s_avail_ver[0] ? s_avail_ver : running_version()));
}

void nv_ota_set_channel(const char *channel) {
    nv_config_set_str("ota_chan", channel && !strcmp(channel, "beta") ? "beta" : "");
}
void nv_ota_get_channel(char *out, size_t n) {
    char c[16];
    nv_config_get_str("ota_chan", "", c, sizeof c);
    snprintf(out, n, "%s", strcmp(c, "beta") ? "stable" : "beta");
}

bool nv_ota_request_rescue(const char *why) {
    // The journal and otadata live in flash: do it on an internal-stack task (callers such as the
    // terminal may run on a PSRAM stack). The cached state answers "is there anything to restore".
    lock();
    const Safety sf = s_safety;
    unlock();
    const bool lkg_other = sf.lkg && sf.rec_ver >= 20000 && strcmp(sf.lkg_ver, running_version());
    char bin[64];
    nv_fwup_path(bin, sizeof bin, mount(), NV_FWUP_PREV_BIN);
    FILE *f = nv_sd_is_mounted() ? fopen(bin, "rb") : nullptr;
    if (f) fclose(f);
    if (!lkg_other && !f) return false;
    static char s_why[48];
    snprintf(s_why, sizeof s_why, "%s", why && why[0] ? why : "requested");
    auto task = [](void *) {
        nv_fwup_journal_t j;
        nv_fwup_journal_load(&j);
        if (rescue_source(true, j)) hand_over(s_why);   // restarts on success
        NV_LOGE(TAG, "rescue: nothing usable to restore");
        vTaskDelete(nullptr);
    };
    return xTaskCreate(task, "ota_rescue", 6144, nullptr, 5, nullptr) == pdPASS;
}

nv_ota_state_t nv_ota_state(void) { return s_state; }
int nv_ota_progress(void)         { return s_progress; }
uint32_t nv_ota_generation(void)  { return s_gen; }
const char *nv_ota_running_version(void)   { return running_version(); }
const char *nv_ota_available_version(void) { return s_avail_ver; }
const char *nv_ota_message(void)           { return s_msg; }
bool nv_ota_layout_ok(void)                { return nv_fwup_layout_v2(); }
bool nv_ota_busy(void)                     { lock(); const bool b = s_busy; unlock(); return b; }

bool nv_ota_take_boot_notice(char *out, size_t n) {
    if (s_boot_notice_taken || !s_boot_notice[0] || !out || !n) return false;
    s_boot_notice_taken = true;
    snprintf(out, n, "%s", s_boot_notice);
    return true;
}

void nv_ota_watch_start(void) {
    if (!s_lock) s_lock = xSemaphoreCreateMutex();
    if (s_watch_timer) return;
    esp_timer_create_args_t a = {};
    a.callback = watch_timer_cb;
    a.dispatch_method = ESP_TIMER_TASK;
    a.name = "ota_watch";
    if (esp_timer_create(&a, &s_watch_timer) != ESP_OK) { s_watch_timer = nullptr; return; }
    if (esp_timer_start_once(s_watch_timer, 15ULL * 60 * 1000 * 1000) != ESP_OK) {
        esp_timer_delete(s_watch_timer);
        s_watch_timer = nullptr;
        return;
    }
    NV_LOGI(TAG, "watch: first manifest check in 15 min, then every 6 h");
}

void nv_ota_get_url(char *out, size_t n) {
    nv_config_get_str("ota_url", "", out, n);
    // Empty, or the layout-v1 channel saved by an older firmware: the channel's URL. A v1 image must
    // never be installed on this layout, and v1 boards must never see v2 images.
    // The stable/beta URLs saved by Settings (the field shows the channel's URL) follow the channel.
    if (n && (!out[0] || !strcmp(out, NV_OTA_LEGACY_URL) || !strcmp(out, NV_OTA_DEFAULT_URL) ||
              !strcmp(out, NV_OTA_BETA_URL))) {
        char c[16];
        nv_ota_get_channel(c, sizeof c);
        snprintf(out, n, "%s", strcmp(c, "beta") ? NV_OTA_DEFAULT_URL : NV_OTA_BETA_URL);
    }
}

void nv_ota_check(const char *manifest_url) {
    if (!s_lock) s_lock = xSemaphoreCreateMutex();
    lock();
    if (s_busy) { unlock(); return; }
    s_busy = true;
    unlock();
    if (!manifest_url || !manifest_url[0]) {
        set_state(NV_OTA_FAILED, "No update URL set");
        lock(); s_busy = false; unlock();
        return;
    }
    char *arg = strdup(manifest_url);
    // 12 KB: an https:// manifest means a TLS handshake + cert-bundle verify (~8-10 KB of stack).
    if (!arg || xTaskCreate(check_task, "ota_chk", 12288, arg, 5, nullptr) != pdPASS) {
        free(arg);
        set_state(NV_OTA_FAILED, "Out of memory");
        lock(); s_busy = false; unlock();
    }
}

void nv_ota_update(void) {
    if (!s_lock) s_lock = xSemaphoreCreateMutex();
    lock();
    if (s_busy || s_bin_url[0] == '\0') { unlock(); return; }
    s_busy = true;
    unlock();
    // 8 KB internal stack: TLS + FATFS + flash reads (the rollback copy).
    if (xTaskCreate(update_task, "ota_dl", 8192, nullptr, 5, nullptr) != pdPASS) {
        set_state(NV_OTA_FAILED, "Out of memory");
        lock(); s_busy = false; unlock();
    }
}

void nv_ota_install_sd(const char *path) {
    if (!s_lock) s_lock = xSemaphoreCreateMutex();
    lock();
    if (s_busy) { unlock(); return; }
    s_busy = true;
    unlock();
    char full[96];
    if (path && path[0]) snprintf(full, sizeof(full), "%s", path);
    else snprintf(full, sizeof(full), "%s/nucleos-anima.bin", nv_sd_mount_point());
    char *arg = strdup(full);
    if (!arg || xTaskCreate(install_sd_task, "ota_sd", 6144, arg, 5, nullptr) != pdPASS) {
        free(arg);
        set_state(NV_OTA_FAILED, "Out of memory");
        lock(); s_busy = false; unlock();
    }
}

void nv_ota_reboot(void) {
    if (s_state == NV_OTA_SUCCESS) {
        NV_LOGI(TAG, "restarting into recovery to install the update");
        esp_restart();
    }
}

void nv_ota_boot_autoupdate(const char *manifest_url) {
    if (!manifest_url || !manifest_url[0]) return;
    if (!s_lock) s_lock = xSemaphoreCreateMutex();
    lock();
    if (s_busy) { unlock(); return; }
    s_busy = true;
    unlock();
    char *arg = strdup(manifest_url);
    if (!arg || xTaskCreate(boot_auto_task, "ota_auto", 8192, arg, 4, nullptr) != pdPASS) {
        free(arg);
        lock(); s_busy = false; unlock();
    }
}
