// nv_telemetry — see nv_telemetry.h. Opt-in, anonymous, one report a day.
#include "nv_telemetry.h"
#include "nv_config.h"
#include "nv_log.h"
#include "nv_time.h"
#include "nv_bgwork.h"
#include "nv_i18n.h"
#include "nv_sd.h"
#include "nv_pad.h"
#include "nv_hid_host.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "freertos/timers.h"
#include "esp_app_desc.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_ota_ops.h"
#include "esp_random.h"
#include "esp_system.h"
#include "esp_timer.h"

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>

namespace {

constexpr char TAG[] = "tele";
constexpr char kUrl[]  = "https://nucleoos.indexhub.it/t/1";
constexpr char kFile[] = "/sdcard/nucleos/tele.bin";
constexpr uint32_t kMagic = 0x31454c54;          // "TLE1"
constexpr int kApps = 24;                          // distinct apps counted between two reports
constexpr int kReasons = 16;
constexpr uint32_t kTickMs = 10 * 60 * 1000;       // due check + counter save
constexpr int kSpreadS = 15 * 60;                  // random delay once due: spreads the server load

// esp_reset_reason_t -> the tokens nv_web's /api/info uses (index = the enum value).
const char *const kReason[kReasons] = {
    "unknown", "poweron", "ext", "sw", "panic", "int_wdt", "task_wdt", "wdt",
    "deepsleep", "brownout", "sdio", "usb", "jtag", "efuse", "pwr_glitch", "cpu_lockup" };
bool is_crash(int r) {
    return r == ESP_RST_PANIC || r == ESP_RST_INT_WDT || r == ESP_RST_TASK_WDT || r == ESP_RST_WDT ||
           r == ESP_RST_BROWNOUT || r == ESP_RST_CPU_LOCKUP;
}

struct AppUse { char id[24]; uint16_t launches; uint16_t minutes; };
struct Counters {
    uint32_t magic;
    uint16_t reset[kReasons];
    uint16_t store[3];                 // nv_tl_store_t
    uint8_t  ota;                      // 0 none, 1 an update landed, 2 an update was rolled back
    uint8_t  pad_;
    AppUse   app[kApps];
};

SemaphoreHandle_t s_mx = nullptr;
Counters s_c = {};
int      s_consent = NV_TELEMETRY_UNASKED;   // cache of nv_config "tl_consent"
bool     s_dirty = false;
bool     s_sending = false;
int      s_cur = -1;                         // app slot in the foreground, -1 = none / not counted
int64_t  s_open_us = 0;
uint32_t s_last_day = 0;                     // YYYYMMDD of the last report sent
int64_t  s_due_us = 0;                       // when today's report goes (0 = not scheduled yet)
TimerHandle_t s_timer = nullptr;

struct Lock {
    Lock()  { if (s_mx) xSemaphoreTake(s_mx, portMAX_DELAY); }
    ~Lock() { if (s_mx) xSemaphoreGive(s_mx); }
};

bool yes() { return s_consent == NV_TELEMETRY_YES; }

// ---- persistence (SD, via the background worker: never on the UI thread) ----------------------
void save_job(void *arg) {
    Counters *c = (Counters *)arg;
    if (nv_sd_is_mounted()) {
        FILE *f = fopen(kFile, "wb");
        if (f) { fwrite(c, 1, sizeof *c, f); fclose(f); }
    }
    free(c);
}
void save_async() {   // caller holds the lock
    Counters *copy = (Counters *)malloc(sizeof(Counters));
    if (!copy) return;
    *copy = s_c;
    if (nv_bgwork_submit(save_job, copy)) s_dirty = false;
    else free(copy);
}
void forget_job(void *) { if (nv_sd_is_mounted()) remove(kFile); }
void load() {
    if (!nv_sd_is_mounted()) return;
    FILE *f = fopen(kFile, "rb");
    if (!f) return;
    Counters c;
    if (fread(&c, 1, sizeof c, f) == sizeof c && c.magic == kMagic) s_c = c;
    fclose(f);
    for (AppUse &a : s_c.app) a.id[sizeof a.id - 1] = 0;
}
void clear_counters() { s_c = {}; s_c.magic = kMagic; s_cur = -1; }

uint32_t today() {
    struct tm t;
    nv_time_now(&t);
    return (uint32_t)(t.tm_year + 1900) * 10000 + (uint32_t)(t.tm_mon + 1) * 100 + (uint32_t)t.tm_mday;
}

// ---- the report -------------------------------------------------------------------------------
const char *lang_code() {
    switch (nv_i18n_get_lang()) {
        case NV_LANG_IT: return "it";
        case NV_LANG_ES: return "es";
        case NV_LANG_FR: return "fr";
        case NV_LANG_DE: return "de";
        default:         return "en";
    }
}

struct Buf {
    char s[1400];
    size_t n = 0;
    void add(const char *fmt, ...) __attribute__((format(printf, 2, 3))) {
        if (n >= sizeof s - 1) return;
        va_list ap;
        va_start(ap, fmt);
        const int w = vsnprintf(s + n, sizeof s - n, fmt, ap);
        va_end(ap);
        if (w > 0) n = n + (size_t)w >= sizeof s ? sizeof s - 1 : n + (size_t)w;
    }
};

// Builds the query from a snapshot. `month` / `first`: the flags that count devices.
void build_url(Buf &b, const Counters &c, bool month, bool first) {
    b.add("%s?v=%s&l=%s", kUrl, esp_app_get_description()->version, lang_code());
    char region[8];
    nv_config_get_str("store_region", "", region, sizeof region);
    bool ok = region[0] != 0;
    for (const char *p = region; *p; p++)
        if (!((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') || *p == '*')) ok = false;
    b.add("&r=%s&m=%d&n=%d", ok ? region : "*", month ? 1 : 0, first ? 1 : 0);
    int crashes = 0;
    bool any = false;
    for (int i = 0; i < kReasons; i++) {
        if (!c.reset[i]) continue;
        b.add("%s%s.%u", any ? "," : "&rb=", kReason[i], (unsigned)c.reset[i]);
        any = true;
        if (is_crash(i)) crashes += c.reset[i];
    }
    b.add("&cr=%d", crashes);
    if (c.ota) b.add("&ota=%s", c.ota == 2 ? "rollback" : "ok");
    // Apps: most minutes first; ids are public (system or store apps), [A-Za-z0-9_-] only.
    int order[kApps], n = 0;
    for (int i = 0; i < kApps; i++) if (c.app[i].id[0]) order[n++] = i;
    for (int a = 1; a < n; a++)
        for (int k = a; k > 0 && c.app[order[k]].minutes > c.app[order[k - 1]].minutes; k--) {
            const int t = order[k]; order[k] = order[k - 1]; order[k - 1] = t;
        }
    for (int i = 0; i < n; i++)
        b.add("%s%s.%u.%u", i ? "," : "&a=", c.app[order[i]].id, (unsigned)c.app[order[i]].launches,
              (unsigned)c.app[order[i]].minutes);
    b.add("&si=%u&su=%u&sx=%u", (unsigned)c.store[0], (unsigned)c.store[1], (unsigned)c.store[2]);
    b.add("&hw=%u.%d.%d.%d", (unsigned)(heap_caps_get_total_size(MALLOC_CAP_SPIRAM) >> 20),
          nv_sd_is_mounted() ? 1 : 0, nv_pad_count() > 0 ? 1 : 0, nv_hid_host_keyboard_present() ? 1 : 0);
    b.add("&hm=%u", (unsigned)(heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL) / 1024));
}

void send_task(void *) {
    Counters snap;
    { Lock l; snap = s_c; }
    const uint32_t day = today();
    char mon[16] = "";
    nv_config_get_str("tl_month", "", mon, sizeof mon);
    char this_mon[16];
    snprintf(this_mon, sizeof this_mon, "%06u", (unsigned)(day / 100));
    const bool month = strcmp(mon, this_mon) != 0;
    const bool first = !nv_config_get_bool("tl_first", false);
    Buf *b = new Buf;
    build_url(*b, snap, month, first);

    esp_http_client_config_t cfg = {};
    cfg.url = b->s;
    cfg.crt_bundle_attach = esp_crt_bundle_attach;
    cfg.timeout_ms = 8000;
    int status = 0;
    esp_http_client_handle_t h = esp_http_client_init(&cfg);
    if (h) {
        if (esp_http_client_perform(h) == ESP_OK) status = esp_http_client_get_status_code(h);
        esp_http_client_cleanup(h);
    }
    delete b;
    NV_LOGI(TAG, "report: HTTP %d", status);
    if (status == 204 || status == 200) {
        nv_config_set_int("tl_day", (int)day);
        nv_config_set_str("tl_month", this_mon);
        nv_config_set_bool("tl_first", true);
        Lock l;
        s_last_day = day;
        // Subtract what was sent (counts that arrived meanwhile stay for the next report).
        for (int i = 0; i < kReasons; i++) s_c.reset[i] -= snap.reset[i] <= s_c.reset[i] ? snap.reset[i] : s_c.reset[i];
        for (int i = 0; i < 3; i++) s_c.store[i] -= snap.store[i] <= s_c.store[i] ? snap.store[i] : s_c.store[i];
        if (s_c.ota == snap.ota) s_c.ota = 0;
        for (int i = 0; i < kApps; i++) {
            if (i == s_cur) {            // still in the foreground: keep the slot, restart its counts
                s_c.app[i].launches = 0;
                s_c.app[i].minutes = 0;
                s_open_us = esp_timer_get_time();
            } else if (!strcmp(s_c.app[i].id, snap.app[i].id)) {
                s_c.app[i] = {};
            }
        }
        save_async();
    }
    { Lock l; s_sending = false; s_due_us = 0; }
    vTaskDelete(nullptr);
}

// The due check runs on the background worker: the FreeRTOS timer task has a 2 KB stack, too
// small for localtime() with a time zone.
void tick_job(void *) {
    Lock l;
    if (!yes() || s_sending) return;
    if (s_dirty) save_async();
    if (!nv_time_is_synced()) return;               // no clock yet = no network yet, too
    if (today() == s_last_day) return;
    const int64_t now = esp_timer_get_time();
    if (!s_due_us) { s_due_us = now + (int64_t)(esp_random() % kSpreadS) * 1000000; return; }
    if (now < s_due_us) return;
    // TLS needs an internal-RAM stack; the task lives only for this one request.
    s_sending = xTaskCreate(send_task, "tele_tx", 10240, nullptr, 2, nullptr) == pdPASS;
}
void tick(TimerHandle_t) { nv_bgwork_submit(tick_job, nullptr); }

}  // namespace

extern "C" {

void nv_telemetry_init(void) {
    if (!s_mx) s_mx = xSemaphoreCreateMutex();
    Lock l;
    s_consent = nv_config_get_int("tl_consent", NV_TELEMETRY_UNASKED);
    clear_counters();
    if (!yes()) return;                              // no consent: nothing is recorded at all
    load();
    s_last_day = (uint32_t)nv_config_get_int("tl_day", 0);
    const int r = (int)esp_reset_reason();
    if (r >= 0 && r < kReasons && s_c.reset[r] < 0xffff) s_c.reset[r]++;
    // An update landed: the version changed since the last boot ("last_ver", which the UI then
    // updates). A rollback: the partition the bootloader gave up on, noted once per version.
    char last[36] = "";
    nv_config_get_str("last_ver", "", last, sizeof last);
    const char *run = esp_app_get_description()->version;
    if (last[0] && strcmp(last, run) != 0) s_c.ota = 1;
    const esp_partition_t *bad = esp_ota_get_last_invalid_partition();
    esp_app_desc_t d;
    if (bad && esp_ota_get_partition_description(bad, &d) == ESP_OK && strcmp(d.version, run) != 0) {
        char seen[36] = "";
        nv_config_get_str("tl_inv", "", seen, sizeof seen);
        if (strcmp(seen, d.version) != 0) {
            nv_config_set_str("tl_inv", d.version);
            s_c.ota = 2;
        }
    }
    save_async();
}

void nv_telemetry_start(void) {
    if (s_timer) return;
    s_timer = xTimerCreate("tele", pdMS_TO_TICKS(kTickMs), pdTRUE, nullptr, tick);
    if (s_timer) xTimerStart(s_timer, 0);
}

nv_telemetry_consent_t nv_telemetry_consent(void) { return (nv_telemetry_consent_t)s_consent; }

void nv_telemetry_set_consent(bool on) {
    nv_config_set_int("tl_consent", on ? NV_TELEMETRY_YES : NV_TELEMETRY_NO);
    Lock l;
    const bool was = yes();
    s_consent = on ? NV_TELEMETRY_YES : NV_TELEMETRY_NO;
    if (!on) {                                     // opting out forgets what was collected
        clear_counters();
        s_due_us = 0;
        nv_bgwork_submit(forget_job, nullptr);
    } else if (!was) {
        clear_counters();
        s_last_day = (uint32_t)nv_config_get_int("tl_day", 0);
    }
    NV_LOGI(TAG, "consent: %s", on ? "yes" : "no");
}

void nv_telemetry_app_open(const char *id, bool countable) {
    Lock l;
    s_cur = -1;
    if (!yes() || !countable || !id || !id[0] || strlen(id) >= sizeof s_c.app[0].id) return;
    for (const char *p = id; *p; p++)
        if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') || *p == '_' || *p == '-'))
            return;
    int slot = -1, free_slot = -1;
    for (int i = 0; i < kApps; i++) {
        if (!strcmp(s_c.app[i].id, id)) { slot = i; break; }
        if (!s_c.app[i].id[0] && free_slot < 0) free_slot = i;
    }
    if (slot < 0) {
        if (free_slot < 0) return;                 // table full until the next report
        slot = free_slot;
        snprintf(s_c.app[slot].id, sizeof s_c.app[slot].id, "%s", id);
    }
    if (s_c.app[slot].launches < 0xffff) s_c.app[slot].launches++;
    s_cur = slot;
    s_open_us = esp_timer_get_time();
    s_dirty = true;
}

void nv_telemetry_app_close(void) {
    Lock l;
    if (s_cur < 0) return;
    const int64_t min = (esp_timer_get_time() - s_open_us + 59999999) / 60000000;
    AppUse &a = s_c.app[s_cur];
    a.minutes = (uint16_t)(a.minutes + min > 60000 ? 60000 : a.minutes + min);
    s_cur = -1;
    s_dirty = true;
}

void nv_telemetry_store(nv_tl_store_t what) {
    Lock l;
    if (!yes() || what < 0 || what > NV_TL_STORE_UNINSTALL) return;
    if (s_c.store[what] < 0xffff) s_c.store[what]++;
    s_dirty = true;
}

}  // extern "C"
