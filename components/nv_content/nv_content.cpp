// nv_content — see nv_content.h. One task (PSRAM stack, it never writes flash), polling cheap state
// every few seconds; the heavy work runs on the store's single worker, one job at a time.
#include "nv_content.h"
#include "nv_content_plan.h"
#include "nv_appstore.h"
#include "nv_config.h"
#include "nv_i18n.h"
#include "nv_log.h"
#include "nv_mem_attr.h"   // NV_PSRAM_BSS: the queue tables live in PSRAM (internal RAM is budgeted)
#include "nv_ota.h"
#include "nv_sd.h"
#include "nv_wifi.h"
#include "nv_eth.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"

#include <cstdio>
#include <cstring>
#include <ctime>
#include <sys/stat.h>
#include <unistd.h>

static const char *TAG = "content";

namespace {

namespace plan = nv_content_plan;

// What THIS firmware needs at least (a newer web companion after an OTA is installed on its own when
// an older one is installed). Bump with the release that needs it; publish the pack FIRST
// (tools/dist.py refuses a firmware whose requirement the store doesn't meet).
struct Required { const char *id; const char *min; };
constexpr Required kRequired[] = {
    {"sys-web", "2026.10.1"},
};

constexpr char kQueue[]      = "/sdcard/nucleos/content/queue";
constexpr int  kQueueMax     = 16;
constexpr int  kVerifyMax    = 4;
constexpr int  kListeners    = 4;
constexpr uint32_t kIndexEvery_s = 12 * 3600;
constexpr uint32_t kSweepAge_s   = 24 * 3600;

SemaphoreHandle_t s_lock = nullptr;
void lock()   { xSemaphoreTake(s_lock, portMAX_DELAY); }
void unlock() { xSemaphoreGive(s_lock); }

struct Q { char id[32]; int fails; int64_t not_before_us; };
NV_PSRAM_BSS Q    s_q[kQueueMax];
int  s_qn = 0;
NV_PSRAM_BSS char s_verify[kVerifyMax][32];
int  s_vn = 0;
NV_PSRAM_BSS char s_damaged[kQueueMax][32];             // the last check failed (until reinstalled)
int  s_dn = 0;
NV_PSRAM_BSS char s_active[32];                    // installing / verifying now
bool s_active_verify = false;
const char *s_waiting = "";
bool s_space_wait = false;                 // the last install failed for room: retrying soon won't help
NV_PSRAM_BSS char s_msg[96];
uint32_t s_gen = 0;
bool s_index_loaded = false;
int  s_index_fails = 0;
int64_t s_index_next_us = 0;
bool s_refresh_now = false;
bool s_swept = false;
nv_content_listener_t s_listeners[kListeners] = {};
TaskHandle_t s_task = nullptr;

void bump() { s_gen++; }

int64_t now_us() { return esp_timer_get_time(); }

const char *ui_lang() {
    switch (nv_i18n_get_lang()) {
        case NV_LANG_IT: return "it";
        case NV_LANG_ES: return "es";
        case NV_LANG_FR: return "fr";
        case NV_LANG_DE: return "de";
        default:         return "en";
    }
}

const char *required_min(const char *id) {
    for (const Required &r : kRequired) if (!strcmp(r.id, id)) return r.min;
    return "";
}

bool net_up() { return nv_wifi_get_state() == NV_WIFI_CONNECTED || nv_eth_get_state() == NV_ETH_UP; }

bool store_busy() {
    const nv_store_state_t st = nv_appstore_state();
    return st == NV_STORE_FETCHING || st == NV_STORE_INSTALLING;
}

int q_find(const char *id) { for (int i = 0; i < s_qn; i++) if (!strcmp(s_q[i].id, id)) return i; return -1; }
bool damaged(const char *id) { for (int i = 0; i < s_dn; i++) if (!strcmp(s_damaged[i], id)) return true; return false; }
void set_damaged(const char *id, bool on) {
    for (int i = 0; i < s_dn; i++)
        if (!strcmp(s_damaged[i], id)) { if (!on) { s_damaged[i][0] = 0; memmove(s_damaged[i], s_damaged[--s_dn], 32); } return; }
    if (on && s_dn < kQueueMax) snprintf(s_damaged[s_dn++], 32, "%s", id);
}

// The queue file: one id per line. Written whole (small), via .tmp + rename.
void q_save() {
    if (!nv_sd_is_mounted()) return;
    char tmp[64];
    snprintf(tmp, sizeof tmp, "%s.tmp", kQueue);
    FILE *f = fopen(tmp, "w");
    if (!f) return;
    for (int i = 0; i < s_qn; i++) fprintf(f, "%s\n", s_q[i].id);
    const bool ok = fclose(f) == 0;
    unlink(kQueue);
    if (ok) rename(tmp, kQueue);
}

void q_load() {
    FILE *f = fopen(kQueue, "r");
    if (!f) return;
    char line[64];
    while (s_qn < kQueueMax && fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = 0;
        const size_t l = strlen(line);
        if (!l || l >= sizeof s_q[0].id || q_find(line) >= 0) continue;
        memcpy(s_q[s_qn].id, line, l + 1);
        s_q[s_qn].fails = 0;
        s_q[s_qn].not_before_us = 0;
        s_qn++;
    }
    fclose(f);
    if (s_qn) NV_LOGI(TAG, "%d pack(s) still queued from before", s_qn);
}

bool q_add(const char *id) {
    if (!id || !id[0] || strlen(id) >= 32) return false;
    if (q_find(id) >= 0) return true;
    if (s_qn >= kQueueMax) return false;
    snprintf(s_q[s_qn].id, 32, "%s", id);
    s_q[s_qn].fails = 0;
    s_q[s_qn].not_before_us = 0;
    s_qn++;
    q_save();
    bump();
    return true;
}

void q_remove(int i) {
    memmove(&s_q[i], &s_q[i + 1], (size_t)(s_qn - i - 1) * sizeof(Q));
    s_qn--;
    q_save();
    bump();
}

// Build a plan::Pack for an index row (strings stay valid while `inst` lives).
plan::Pack pack_of(const nv_content_entry_t &e, const char *inst) {
    return plan::Pack{e.id, e.version, inst, e.langs, required_min(e.id)};
}

void hook(const char *dest, const char *name, bool before) {
    for (nv_content_listener_t fn : s_listeners) if (fn) fn(dest, name, before);
}

// Queue what this firmware requires (installed but too old). Never a pack the owner didn't install.
void queue_required() {
    const int n = nv_appstore_content_count();
    const bool web_local = nv_config_get_bool("web_local", false);
    for (int i = 0; i < n; i++) {
        nv_content_entry_t e;
        char inst[16];
        if (!nv_appstore_content_get(i, &e)) continue;
        nv_appstore_content_installed(e.id, inst, sizeof inst);
        const plan::Pack p = pack_of(e, inst);
        if (plan::auto_update(p, web_local)) {
            lock();
            const bool added = q_find(e.id) < 0 && q_add(e.id);
            unlock();
            if (added) NV_LOGI(TAG, "'%s' %s is older than this firmware needs (%s): updating it", e.id, inst, p.required_min);
        } else if (plan::requirement_unmet(p)) {
            NV_LOGW(TAG, "'%s' >= %s is required but the store offers '%s': retrying later", e.id, p.required_min, e.version);
        }
    }
}

// Wait for the store job just started; true when the store went idle within `ms`.
bool wait_store(int ms) {
    vTaskDelay(pdMS_TO_TICKS(300));
    for (; ms > 0 && store_busy(); ms -= 1000) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        lock(); bump(); unlock();                              // progress moved: the UI repaints
    }
    return !store_busy();
}

void set_msg(const char *m) { lock(); snprintf(s_msg, sizeof s_msg, "%s", m ? m : ""); bump(); unlock(); }

void run_index() {
    if (!nv_appstore_content_refresh()) return;
    wait_store(2 * 60 * 1000);
    const bool ok = nv_appstore_state() != NV_STORE_ERROR;
    lock();
    if (ok) { s_index_loaded = true; s_index_fails = 0; s_index_next_us = now_us() + (int64_t)kIndexEvery_s * 1000000; }
    else    { s_index_fails++; s_index_next_us = now_us() + (int64_t)plan::backoff_s(s_index_fails) * 1000000; }
    bump();
    unlock();
    if (ok) queue_required();
    else NV_LOGW(TAG, "content list: %s (retry in %u s)", nv_appstore_message(), (unsigned)plan::backoff_s(s_index_fails));
}

void run_verify(const char *id) {
    lock(); snprintf(s_active, sizeof s_active, "%s", id); s_active_verify = true; bump(); unlock();
    if (nv_appstore_content_verify(id)) wait_store(30 * 60 * 1000);
    const char *vid = nullptr;
    int bad = 0;
    const int r = nv_appstore_content_verify_result(&vid, &bad);
    lock();
    if (vid && !strcmp(vid, id)) set_damaged(id, r == 0);
    s_active[0] = 0;
    s_active_verify = false;
    bump();
    unlock();
    set_msg(nv_appstore_message());
}

void run_install(int qi) {
    char id[32];
    lock(); snprintf(id, sizeof id, "%s", s_q[qi].id); snprintf(s_active, sizeof s_active, "%s", id); s_active_verify = false; bump(); unlock();
    NV_LOGI(TAG, "installing '%s'", id);
    const bool started = nv_appstore_content_install(id);
    if (started) wait_store(6 * 3600 * 1000);
    char ver[16] = "";
    const bool ok = started && nv_appstore_state() == NV_STORE_READY && nv_appstore_content_installed(id, ver, sizeof ver);
    const char *m = nv_appstore_message();
    lock();
    s_active[0] = 0;
    const int i = q_find(id);
    if (ok) {
        s_space_wait = false;
        set_damaged(id, false);
        if (i >= 0) q_remove(i);
    } else if (i >= 0) {
        s_q[i].fails++;
        // no room is not fixed by retrying soon: wait the longest step
        const bool space = strstr(m, "space") != nullptr;
        s_q[i].not_before_us = now_us() + (int64_t)plan::backoff_s(space ? 4 : s_q[i].fails) * 1000000;
        s_space_wait = space;
    }
    bump();
    unlock();
    set_msg(m);
    NV_LOGI(TAG, "'%s': %s", id, ok ? "installed" : m);
}

// The one thing the task does per pass, if anything. Returns what it waits for ("" = nothing).
const char *step() {
    if (!nv_sd_is_mounted()) return "sd";
    if (nv_ota_safe_mode()) return "safe";
    if (!nv_ota_confirmed() || nv_ota_busy()) return "ota";
    if (store_busy()) return "store";
    if (!net_up()) return "net";

    lock();
    const bool want_index = s_refresh_now || now_us() >= s_index_next_us;
    s_refresh_now = false;
    unlock();
    if (want_index) { run_index(); return ""; }

    lock();
    char vid[32] = "";
    if (s_vn) { snprintf(vid, sizeof vid, "%s", s_verify[0]); memmove(s_verify[0], s_verify[1], (size_t)(--s_vn) * 32); }
    int pick = -1;
    int64_t soonest = INT64_MAX;
    for (int i = 0; i < s_qn && pick < 0; i++) {
        if (s_q[i].not_before_us <= now_us()) pick = i;
        else if (s_q[i].not_before_us < soonest) soonest = s_q[i].not_before_us;
    }
    const int qn = s_qn;
    const bool space = s_space_wait;
    unlock();
    if (vid[0]) { run_verify(vid); return ""; }
    if (pick >= 0) { run_install(pick); return ""; }
    if (qn) return space ? "space" : "retry";

    if (!s_swept) {                                           // nothing queued: tidy old leftovers once
        s_swept = true;
        nv_appstore_content_sweep(kSweepAge_s, nullptr);
    }
    return "";
}

void task(void *) {
    vTaskDelay(pdMS_TO_TICKS(20 * 1000));                     // after boot's Wi-Fi join and auto-OTA check
    if (nv_sd_is_mounted()) { lock(); q_load(); unlock(); }
    for (;;) {
        const char *w = step();
        lock();
        if (strcmp(w, s_waiting)) { s_waiting = w; bump(); }
        unlock();
        vTaskDelay(pdMS_TO_TICKS(w[0] ? 5000 : 1000));
    }
}

bool init() {
    if (!s_lock) s_lock = xSemaphoreCreateMutex();
    return s_lock != nullptr;
}

}  // namespace

extern "C" {

void nv_content_start(void) {
    if (s_task || !init()) return;
    nv_appstore_set_content_hook(hook);
    // 6 KB PSRAM stack: SD files + store API calls, never flash (nv_config reads go through its proxy)
    if (xTaskCreateWithCaps(task, "content", 6144, nullptr, 2, &s_task, MALLOC_CAP_SPIRAM) != pdPASS)
        NV_LOGE(TAG, "could not start the content service");
}

void nv_content_add_listener(nv_content_listener_t fn) {
    for (nv_content_listener_t &l : s_listeners) if (!l || l == fn) { l = fn; return; }
}

static void fill(const nv_content_entry_t &e, nv_content_pack_t *out) {
    memset(out, 0, sizeof *out);
    out->e = e;
    nv_appstore_content_installed(e.id, out->installed, sizeof out->installed);
    const plan::Pack p = pack_of(e, out->installed);
    switch (plan::state(p)) {
        case plan::OK:          out->state = NV_CONTENT_OK; break;
        case plan::UPDATE:      out->state = NV_CONTENT_UPDATE; break;
        case plan::MISSING:     out->state = NV_CONTENT_MISSING; break;
        case plan::UNAVAILABLE: out->state = NV_CONTENT_UNAVAILABLE; break;
    }
    out->recommended = plan::recommended(p, ui_lang());
    out->required = p.required_min[0] != 0;
    if (!init()) return;
    lock();
    if (damaged(e.id)) out->state = NV_CONTENT_DAMAGED;
    if (q_find(e.id) >= 0) out->state = NV_CONTENT_QUEUED;
    if (!strcmp(s_active, e.id)) {
        out->state = NV_CONTENT_INSTALLING;
        out->progress = nv_appstore_progress();
    }
    unlock();
}

int nv_content_count(void) { return nv_appstore_content_count(); }

bool nv_content_get(int i, nv_content_pack_t *out) {
    nv_content_entry_t e;
    if (!out || !nv_appstore_content_get(i, &e)) return false;
    fill(e, out);
    return true;
}

bool nv_content_find(const char *id, nv_content_pack_t *out) {
    const int n = nv_appstore_content_count();
    for (int i = 0; id && i < n; i++) {
        nv_content_entry_t e;
        if (nv_appstore_content_get(i, &e) && !strcmp(e.id, id)) { if (out) fill(e, out); return true; }
    }
    return false;
}

bool nv_content_install(const char *id) {
    if (!init() || !nv_content_find(id, nullptr)) return false;   // only what the store offers
    lock();
    const bool ok = q_add(id);
    const int i = q_find(id);
    if (i >= 0) { s_q[i].not_before_us = 0; s_q[i].fails = 0; }  // the owner asked: try now
    s_space_wait = false;
    unlock();
    if (ok && !strcmp(id, "sys-web")) nv_config_set_bool("web_local", false);   // the owner chose the store's web
    return ok;
}

int nv_content_install_recommended(void) {
    int n = 0;
    const int count = nv_content_count();
    for (int i = 0; i < count; i++) {
        nv_content_pack_t p;
        if (!nv_content_get(i, &p) || !p.recommended) continue;
        if ((p.state == NV_CONTENT_MISSING || p.state == NV_CONTENT_UPDATE || p.state == NV_CONTENT_DAMAGED) &&
            nv_content_install(p.e.id))
            n++;
    }
    return n;
}

bool nv_content_cancel(const char *id) {
    if (!init() || !id) return false;
    lock();
    const int i = q_find(id);
    if (i >= 0) q_remove(i);
    unlock();
    return i >= 0;
}

bool nv_content_verify(const char *id) {
    if (!init() || !id || strlen(id) >= 32 || !nv_appstore_content_installed(id, nullptr, 0)) return false;
    lock();
    bool ok = s_vn < kVerifyMax;
    for (int i = 0; i < s_vn; i++) if (!strcmp(s_verify[i], id)) ok = false;
    if (ok) { snprintf(s_verify[s_vn++], 32, "%s", id); bump(); }
    unlock();
    return ok;
}

uint64_t nv_content_recommended_bytes(void) {
    uint64_t b = 0;
    const int count = nv_content_count();
    for (int i = 0; i < count; i++) {
        nv_content_pack_t p;
        if (nv_content_get(i, &p) && p.recommended && p.state != NV_CONTENT_OK) b += p.e.size;
    }
    return b;
}

int nv_content_recommended_missing(void) {
    int n = 0;
    const int count = nv_content_count();
    for (int i = 0; i < count; i++) {
        nv_content_pack_t p;
        if (nv_content_get(i, &p) && p.recommended && p.state != NV_CONTENT_OK) n++;
    }
    return n;
}

const char *nv_content_waiting(void) { if (!init()) return "sd"; lock(); const char *w = s_waiting; unlock(); return w; }

void nv_content_message(char *out, size_t n) {
    if (!out || !n) return;
    out[0] = 0;
    if (!init()) return;
    lock(); snprintf(out, n, "%s", s_msg); unlock();
}

uint32_t nv_content_generation(void) { if (!init()) return 0; lock(); const uint32_t g = s_gen; unlock(); return g; }

bool nv_content_index_loaded(void) { if (!init()) return false; lock(); const bool b = s_index_loaded; unlock(); return b; }

void nv_content_refresh(void) { if (!init()) return; lock(); s_refresh_now = true; unlock(); }

}  // extern "C"
