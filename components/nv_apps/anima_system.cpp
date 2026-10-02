// Live ANIMA_ACT_SYSTEM values (see nv_anima_system.h). Ported from the Cardputer's anima_get
// resolver (G:\Nucleo nucleo_httpd.c) onto the NucleoV2 OS APIs. Every answer is computed from
// device state — never from knowledge cards — so it is always exact.
#include "nv_anima_system.h"

#include "nv_time.h"
#include "nv_sd.h"
#include "nv_wifi.h"
#include "nv_app.h"
#include "nv_open.h"
#include "nv_audio.h"
#include "nv_hal.h"
#include "nv_config.h"
#include "nv_i18n.h"
#include "nv_media.h"     // close_app: stop background playback
#include "nv_notify.h"
#include "nv_ui.h"         // automations: the foreground app (app_open)    // reminder service: toast + notification center
#include "lvgl.h"
#include "esp_lvgl_port.h"
#include "nucleo_anima.h" // tool payload / outcome, a_write_atomic
#include "cJSON.h"        // the Calendar app's calendar.json

#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_http_client.h"   // automations: Home Assistant state polling
#include "esp_crt_bundle.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"   // automations: the event queue   // the heartbeat runs on a one-shot PSRAM task

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <sys/stat.h>

static void nv_anima_agenda(int from, int days, bool en, char *out, size_t cap);

bool nv_anima_system_value(const char *key, bool en, char *out, size_t cap)
{
    snprintf(out, cap, "%s", en ? "unavailable" : "non disponibile");
    if (!key || !key[0]) return false;

    if (!strcmp(key, "time")) {
        time_t now = time(nullptr);
        if (now > 1672531200) {                     // clock actually set (RTC/SNTP)
            char t[48];
            nv_time_format(t, sizeof t, en ? "%H:%M" : "%H:%M");
            snprintf(out, cap, en ? "It's %s" : "Sono le %s", t);
        } else {
            snprintf(out, cap, en ? "I don't know the time: the clock isn't set"
                                  : "Non conosco l'ora: l'orologio non e' impostato");
        }
        return true;
    }
    if (!strcmp(key, "storage")) {
        uint64_t total = 0, freeb = 0;
        if (!nv_sd_info(&total, &freeb)) return true;   // "unavailable" (SD missing)
        snprintf(out, cap, en ? "%.1f GB free of %.1f GB" : "%.1f GB liberi su %.1f GB",
                 freeb / 1e9, total / 1e9);
        return true;
    }
    if (!strcmp(key, "date") || !strcmp(key, "year") || !strcmp(key, "season")) {
        static const char *WD_IT[] = {"domenica","lunedi","martedi","mercoledi","giovedi","venerdi","sabato"};
        static const char *WD_EN[] = {"Sunday","Monday","Tuesday","Wednesday","Thursday","Friday","Saturday"};
        static const char *MO_IT[] = {"gennaio","febbraio","marzo","aprile","maggio","giugno","luglio","agosto","settembre","ottobre","novembre","dicembre"};
        static const char *MO_EN[] = {"January","February","March","April","May","June","July","August","September","October","November","December"};
        static const char *SE_IT[] = {"inverno","primavera","estate","autunno"};
        static const char *SE_EN[] = {"winter","spring","summer","autumn"};
        time_t now = time(nullptr);
        struct tm *tm = localtime(&now);
        if (!tm || now <= 1672531200) return true;
        int y = tm->tm_year + 1900, mo = tm->tm_mon, wd = tm->tm_wday, d = tm->tm_mday;
        if (!strcmp(key, "year")) {
            snprintf(out, cap, "%d", y);
        } else if (!strcmp(key, "season")) {
            // Astronomical seasons (Northern hemisphere): they turn at the equinoxes/solstices
            // ~the 20th-22nd, NOT the 1st — so 3 June is still SPRING, not summer.
            int s = ((mo == 2 && d >= 20) || mo == 3 || mo == 4 || (mo == 5 && d <= 20)) ? 1
                  : ((mo == 5 && d >= 21) || mo == 6 || mo == 7 || (mo == 8 && d <= 22)) ? 2
                  : ((mo == 8 && d >= 23) || mo == 9 || mo == 10 || (mo == 11 && d <= 20)) ? 3
                  : 0;
            snprintf(out, cap, "%s", en ? SE_EN[s] : SE_IT[s]);
        } else {
            if (en) snprintf(out, cap, "Today is %s, %s %d %d", WD_EN[wd], MO_EN[mo], d, y);
            else    snprintf(out, cap, "Oggi e %s %d %s %d", WD_IT[wd], d, MO_IT[mo], y);
        }
        return true;
    }
    if (!strncmp(key, "agenda", 6)) {   // "agenda:<from>:<days>" — events from the Calendar app
        int from = 0, days = 1;
        if (key[6] == ':') sscanf(key + 7, "%d:%d", &from, &days);
        nv_anima_agenda(from, days, en, out, cap);
        return true;
    }
    if (!strcmp(key, "capabilities")) {
        // DYNAMIC "what can I do": the live app registry + the OS pillars.
        int n = nv_app_count();
        char applist[96] = "";
        int shown = 0;
        for (int i = 0; i < n && shown < 5; i++) {
            const NvApp *a = nv_app_at(i);
            if (!a) continue;
            const char *nm = (a->name && a->name[0]) ? a->name : a->id;
            if (applist[0] && strlen(applist) + strlen(nm) + 3 < sizeof applist)
                strncat(applist, ", ", sizeof applist - strlen(applist) - 1);
            if (strlen(applist) + strlen(nm) + 1 < sizeof applist) {
                strncat(applist, nm, sizeof applist - strlen(applist) - 1);
                shown++;
            }
        }
        if (en) snprintf(out, cap,
            "I can open your %d apps (%s...), tell time/date/season, report SD space, Wi-Fi and "
            "free RAM, play music and videos, show your photos, take notes and answer questions "
            "about NucleoOS, C, electronics and general topics — all offline, on the device.",
            n, applist);
        else snprintf(out, cap,
            "Posso aprire le tue %d app (%s...), dirti ora/data/stagione, lo spazio SD, lo stato "
            "Wi-Fi e la RAM libera, riprodurre musica e video, mostrarti le foto, prendere note e "
            "rispondere a domande su NucleoOS, C, elettronica e cultura generale — tutto offline, "
            "sul dispositivo.",
            n, applist);
        return true;
    }
    if (!strcmp(key, "network")) {
        char ssid[64] = "", ip[20] = "";
        int8_t rssi = 0;
        if (nv_wifi_get_connected(ssid, sizeof ssid, ip, sizeof ip, &rssi))
            snprintf(out, cap, en ? "connected to \"%s\", IP %s" : "connesso a \"%s\", IP %s", ssid, ip);
        else
            snprintf(out, cap, en ? "not connected" : "non connesso");
        return true;
    }
    if (!strcmp(key, "ram")) {
        unsigned kb  = (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024);
        unsigned pmb = (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / (1024 * 1024));
        snprintf(out, cap, en ? "%u KB of fast RAM and %u MB of PSRAM free"
                              : "%u KB di RAM veloce e %u MB di PSRAM liberi", kb, pmb);
        return true;
    }
    if (!strcmp(key, "version")) {
        const esp_app_desc_t *d = esp_app_get_description();
        snprintf(out, cap, "NucleoOS %s", d ? d->version : "?");
        return true;
    }
    if (!strcmp(key, "uptime")) {
        long s = (long)(esp_timer_get_time() / 1000000);
        int dd = (int)(s / 86400), hh = (int)((s % 86400) / 3600), mm = (int)((s % 3600) / 60);
        if (dd)      snprintf(out, cap, en ? "%dd %dh" : "%dg %dh", dd, hh);
        else if (hh) snprintf(out, cap, "%dh %dm", hh, mm);
        else         snprintf(out, cap, "%dm", mm);
        return true;
    }
    if (!strcmp(key, "battery")) {
        // No fuel gauge on this board — powered by USB/DC. Honest answer beats "unavailable".
        snprintf(out, cap, en ? "I'm on wall power, no battery here"
                              : "Sono alimentato dalla presa, niente batteria");
        return true;
    }
    return false;
}

void nv_anima_system_reply(const char *key, const char *tmpl, bool en, char *out, size_t cap)
{
    char value[640];
    nv_anima_system_value(key, en, value, sizeof value);
    const char *ph = tmpl ? strstr(tmpl, "{value}") : nullptr;
    if (ph) snprintf(out, cap, "%.*s%s%s", (int)(ph - tmpl), tmpl, value, ph + 7);
    else    snprintf(out, cap, "%s", tmpl ? tmpl : value);
}

// Apply "70" (absolute) or "+10"/"-10" (relative to `cur`), clamped to [lo,100].
static int tool_level(const char *arg, int cur, int lo)
{
    int v = (arg[0] == '+' || arg[0] == '-') ? cur + atoi(arg) : atoi(arg);
    if (v < lo) v = lo;
    if (v > 100) v = 100;
    return v;
}

bool nv_anima_os_exec(const char *intent, const char *arg)
{
    if (!intent || !arg || !arg[0]) return false;
    if (!strcmp(intent, "set_volume")) {
        int v = tool_level(arg, nv_config_get_int("volume", 60), 0);
        nv_audio_set_volume(v);
        nv_audio_set_mute(v == 0);            // "volume a zero"/"muto" really silences the DAC
        nv_config_set_int("volume", v);       // persist + keep the shade/music sliders honest
        nv_config_set_bool("mute", v == 0);   // ...and the mute icon / next-boot state with the codec
        // Note: this runs on a PSRAM-stack worker; nv_config proxies the NVS writes to an
        // internal-stack helper, so no flash access happens on this stack.
        return true;
    }
    if (!strcmp(intent, "set_brightness")) {
        int v = tool_level(arg, nv_config_get_int("brightness", 90), 5);  // 5%: never black the panel
        nv_hal_backlight_set(v);
        nv_config_set_int("brightness", v);
        return true;
    }
    if (!strcmp(intent, "open_file")) {
        // The engine remembers files by their web-OS logical path ("/data/<Folder>/<name>", rooted
        // at the SD card like nv_web's map_fs); a real /sdcard path passes through. nv_open then
        // does what a tap in Files would: default app or the "Open with" sheet, posted to the UI
        // thread (this runs on an ANIMA worker / the httpd task). Back returns to ANIMA.
        if (strstr(arg, "..")) return false;
        char path[NV_OPEN_PATH_MAX];
        const int w = !strncmp(arg, "/sdcard/", 8) ? snprintf(path, sizeof path, "%s", arg)
                                                   : snprintf(path, sizeof path, "/sdcard%s%s",
                                                              arg[0] == '/' ? "" : "/", arg);
        if (w <= 0 || (size_t)w >= sizeof path) return false;
        return nv_open_file_async(path, nullptr);
    }
    return false;
}

void nv_anima_pretty_launch(char *reply, size_t cap, const char *id)
{
    if (!reply || !id || !id[0]) return;
    const NvApp *a = nv_ui_find_app(id);
    if (!a) return;
    const char *nm = a->name_id >= 0 ? nv_tr((nv_str_id_t)a->name_id) : a->name;
    if (!nm || !nm[0] || !strcmp(nm, id)) return;
    char *hit = strstr(reply, id);
    if (!hit) return;
    char tail[256];
    snprintf(tail, sizeof tail, "%s", hit + strlen(id));
    size_t used = (size_t)(hit - reply);
    snprintf(reply + used, cap - used, "%s%s", nm, tail);
}

void nv_anima_pretty_reply(char *reply, size_t cap, const anima_result_t *r)
{
    if (!reply || !r) return;
    if ((r->action == ANIMA_ACT_LAUNCH || !strcmp(r->intent, "close_app")) && r->arg[0])
        nv_anima_pretty_launch(reply, cap, r->arg);
    for (int i = 0; i < r->nsteps && i < ANIMA_PLAN_MAX; i++)
        if (!strcmp(r->steps[i].intent, "close_app")) nv_anima_pretty_launch(reply, cap, r->steps[i].arg);
}

// ---------------------------------------------------------------- tool executor

namespace {

constexpr const char *kCalendar = "/sdcard/system/config/calendar.json";   // the Calendar app's store

// Plain file -> heap string (NUL-terminated), nullptr when absent / too big.
char *slurp(const char *path, size_t max)
{
    FILE *f = fopen(path, "rb");
    if (!f) return nullptr;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *b = (n >= 0 && (size_t)n <= max) ? (char *)heap_caps_malloc((size_t)n + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) : nullptr;
    if (b && fread(b, 1, (size_t)n, f) != (size_t)n) { heap_caps_free(b); b = nullptr; }
    fclose(f);
    if (b) b[n] = 0;
    return b;
}

// The payload of an add_event proposal: "off=<days>;time=<HH:MM|>;text=<...>".
bool parse_event(const char *c, int *off, char *hhmm, size_t hcap, char *text, size_t tcap)
{
    if (!c || strncmp(c, "off=", 4)) return false;
    *off = atoi(c + 4);
    const char *t = strstr(c, ";time="), *x = strstr(c, ";text=");
    if (!t || !x || x < t) return false;
    snprintf(hhmm, hcap, "%.*s", (int)(x - t - 6), t + 6);
    snprintf(text, tcap, "%s", x + 6);
    return text[0] != 0;
}

bool run_add_event(bool en, char *note, size_t cap)
{
    int off = 0; char hhmm[8] = "", text[256] = "";
    if (!parse_event(nucleo_anima_tool_content(), &off, hhmm, sizeof hhmm, text, sizeof text)) {
        snprintf(note, cap, "%s", en ? "nothing to schedule" : "niente da programmare");
        return false;
    }
    time_t now = time(nullptr);
    struct tm tm;
    localtime_r(&now, &tm);
    if (tm.tm_year + 1900 < 2024) {   // RTC not set yet: "domani" would land in 1970
        snprintf(note, cap, "%s", en ? "the clock is not set yet" : "l'orologio non è ancora impostato");
        return false;
    }
    tm.tm_hour = 12; tm.tm_min = 0; tm.tm_sec = 0;
    tm.tm_mday += off;
    mktime(&tm);
    char key[40];   // room for any int the compiler can imagine (-Werror=format-truncation)
    snprintf(key, sizeof key, "%04d-%02d-%02d", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday);

    char *raw = slurp(kCalendar, 256 * 1024);
    cJSON *root = raw ? cJSON_Parse(raw) : cJSON_CreateObject();
    const bool unreadable = raw && !root;   // a store we can't parse is never overwritten
    heap_caps_free(raw);
    if (unreadable) {
        snprintf(note, cap, "%s", en ? "calendar.json is unreadable, not changed" : "calendar.json illeggibile, non modificato");
        return false;
    }
    if (!root) return false;
    if (!cJSON_GetObjectItem(root, "schema")) cJSON_AddNumberToObject(root, "schema", 1);
    cJSON *evs = cJSON_GetObjectItem(root, "events");
    if (!cJSON_IsObject(evs)) { cJSON_DeleteItemFromObject(root, "events"); evs = cJSON_AddObjectToObject(root, "events"); }
    cJSON *day = cJSON_GetObjectItem(evs, key);
    if (!cJSON_IsArray(day)) { cJSON_DeleteItemFromObject(evs, key); day = cJSON_AddArrayToObject(evs, key); }
    cJSON *ev = cJSON_CreateObject();
    char id[72];
    snprintf(id, sizeof id, "a%lld-%s", (long long)now, key);
    cJSON_AddStringToObject(ev, "id", id);
    cJSON_AddStringToObject(ev, "time", hhmm);
    cJSON_AddStringToObject(ev, "text", text);
    cJSON_AddItemToArray(day, ev);
    char *out = cJSON_Print(root);
    cJSON_Delete(root);
    a_mkdirs(kCalendar);
    const bool ok = out && a_write_atomic(kCalendar, out, strlen(out));
    cJSON_free(out);
    if (ok) snprintf(note, cap, "%s %s%s%s", en ? "calendar:" : "calendario:", key, hhmm[0] ? " " : "", hhmm);
    else    snprintf(note, cap, "%s", en ? "could not write the calendar" : "scrittura del calendario fallita");
    return ok;
}

bool run_create_file(const char *logical, bool en, char *note, size_t cap)
{
    // The engine names files by their web-OS logical path ("/data/Documents/nota.txt"), rooted at
    // the SD card like nv_web's map_fs. Never overwrite: an existing name gets "-2", "-3", ...
    if (!logical[0] || strstr(logical, "..") || logical[0] != '/') return false;
    char path[420];   // "<stem>-<n><ext>" from a 200-byte base: sized for -Werror=format-truncation
    snprintf(path, sizeof path, "/sdcard%s", logical);
    const char *dot = strrchr(path, '.'), *slash = strrchr(path, '/');
    if (dot && dot < slash) dot = nullptr;
    struct stat st;
    for (int n = 2; stat(path, &st) == 0 && n < 100; n++) {
        const int stem = dot ? (int)(dot - path) : (int)strlen(path);
        char base[200];
        snprintf(base, sizeof base, "/sdcard%s", logical);
        snprintf(path, sizeof path, "%.*s-%d%s", stem, base, n, dot ? base + stem : "");
    }
    if (stat(path, &st) == 0) { snprintf(note, cap, "%s", en ? "too many files with that name" : "troppi file con quel nome"); return false; }
    a_mkdirs(path);
    const char *body = nucleo_anima_tool_content();
    const bool ok = a_write_atomic(path, body ? body : "", body ? strlen(body) : 0);
    if (!ok) { snprintf(note, cap, "%s", en ? "could not write the file" : "scrittura del file fallita"); return false; }
    nucleo_anima_note_file(path + 7);   // "aprilo" now opens it (logical path)
    snprintf(note, cap, "%s %s", en ? "saved" : "salvato", path + 7);
    return true;
}

bool run_close_app(const char *id, bool en, char *note, size_t cap)
{
    // ANIMA is the foreground app while it answers, so "chiudi la musica" means the playback that
    // keeps going in the background; any other app is not running.
    if (!strcmp(id, "music") || !strcmp(id, "radio") || !strcmp(id, "media-player")) {
        const nv_media_state_t ms = nv_media_state();
        if (ms != NV_MEDIA_PLAYING && ms != NV_MEDIA_PAUSED) { snprintf(note, cap, "%s", en ? "nothing is playing" : "non c'è niente in riproduzione"); return false; }
        nv_media_stop();
        snprintf(note, cap, "%s", en ? "playback stopped" : "riproduzione fermata");
        return true;
    }
    snprintf(note, cap, "%s", en ? "that app is not open" : "quell'app non è aperta");
    return false;
}

}  // namespace

static bool os_run_one(const anima_result_t *r, bool en, char *note, size_t cap);

bool nv_anima_os_run(const anima_result_t *r, bool en, char *note, size_t cap)
{
    if (note && cap) note[0] = 0;
    if (!nucleo_anima_has_tool_work(r) || !note || !cap) return false;
    // A plan: the primary TOOL (if it is one), then each extra step in the order asked. Every step runs
    // even when one fails (they are independent device actions), and the note lists each outcome.
    bool all = true;
    size_t o = 0;
    if (r->action == ANIMA_ACT_TOOL) {
        all = os_run_one(r, en, note, cap);
        o = strlen(note);
    }
    for (int i = 0; i < r->nsteps && i < ANIMA_PLAN_MAX; i++) {
        anima_result_t s;
        memset(&s, 0, sizeof s);
        s.action = ANIMA_ACT_TOOL;
        snprintf(s.intent, sizeof s.intent, "%s", r->steps[i].intent);
        snprintf(s.arg, sizeof s.arg, "%s", r->steps[i].arg);
        char one[96];
        const bool ok = os_run_one(&s, en, one, sizeof one);
        all = all && ok;
        if (o + 4 < cap) o += (size_t)snprintf(note + o, cap - o, "%s%s%s", o ? "; " : "", ok ? "" : "✗ ", one);
        if (o >= cap) o = cap - 1;
    }
    return all;
}

static bool os_run_one(const anima_result_t *r, bool en, char *note, size_t cap)
{
    if (note && cap) note[0] = 0;
    bool ok = false;
    if (!strcmp(r->intent, "add_event"))        ok = run_add_event(en, note, cap);
    else if (!strcmp(r->intent, "create_file")) ok = run_create_file(r->arg, en, note, cap);
    else if (!strcmp(r->intent, "close_app"))   ok = run_close_app(r->arg, en, note, cap);
    else if (!strcmp(r->intent, "set_volume") || !strcmp(r->intent, "set_brightness")) {
        ok = nv_anima_os_exec(r->intent, r->arg);
        const bool vol = r->intent[4] == 'v';
        if (ok) snprintf(note, cap, "%s %d%%", vol ? "volume" : (en ? "brightness" : "luminosità"),
                         nv_config_get_int(vol ? "volume" : "brightness", vol ? 60 : 90));
    } else {
        ok = nv_anima_os_exec(r->intent, r->arg);
    }
    if (!ok && !note[0]) snprintf(note, cap, "%s", en ? "not done" : "non eseguito");
    nucleo_anima_observe(r->intent, ok);
    return ok;
}

// "che impegni ho oggi / domani / venerdì / questa settimana": the Calendar app's events for `days`
// days starting `from` days after today, each day by time.
static void nv_anima_agenda(int from, int days, bool en, char *out, size_t cap)
{
    if (from < 0 || from > 366) from = 0;
    if (days < 1 || days > 14) days = 1;
    static const char *const wd_it[] = {"dom","lun","mar","mer","gio","ven","sab"};
    static const char *const wd_en[] = {"Sun","Mon","Tue","Wed","Thu","Fri","Sat"};
    const char *when = from == 0 && days == 1 ? (en ? "today" : "oggi")
                     : from == 1 && days == 1 ? (en ? "tomorrow" : "domani")
                     : days > 1 ? (en ? "in that week" : "in quella settimana") : (en ? "that day" : "quel giorno");
    char *raw = slurp(kCalendar, 256 * 1024);
    cJSON *root = raw ? cJSON_Parse(raw) : nullptr;
    heap_caps_free(raw);
    cJSON *evs = root ? cJSON_GetObjectItem(root, "events") : nullptr;
    size_t o = 0; int total = 0;
    time_t now = time(nullptr);
    if (days == 1) {   // "Domani: 09:00 chiamare Marco."
        o = (size_t)snprintf(out, cap, "%s", when);
        if (o < cap) out[0] = (char)toupper((unsigned char)out[0]);
        if (o + 2 < cap) { out[o++] = ':'; out[o++] = ' '; out[o] = 0; }
    }
    for (int dd = 0; dd < days && o < cap; dd++) {
        struct tm tm; localtime_r(&now, &tm);
        tm.tm_hour = 12; tm.tm_min = tm.tm_sec = 0; tm.tm_mday += from + dd; mktime(&tm);
        char key[40];   // room for any int the compiler can imagine (-Werror=format-truncation)
        snprintf(key, sizeof key, "%04d-%02d-%02d", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday);
        cJSON *day = evs ? cJSON_GetObjectItem(evs, key) : nullptr;
        const int n = cJSON_IsArray(day) ? cJSON_GetArraySize(day) : 0;
        if (!n) continue;
        // By time (an event without one first, like the Calendar app's day view).
        const cJSON *ev[32]; int m = 0;
        for (int i = 0; i < n && m < 32; i++) ev[m++] = cJSON_GetArrayItem(day, i);
        auto tstr = [](const cJSON *e) { const cJSON *t = cJSON_GetObjectItem(e, "time"); return cJSON_IsString(t) ? t->valuestring : ""; };
        for (int i = 1; i < m; i++) for (int j = i; j > 0 && strcmp(tstr(ev[j - 1]), tstr(ev[j])) > 0; j--) { const cJSON *x = ev[j]; ev[j] = ev[j - 1]; ev[j - 1] = x; }
        if (days > 1) o += (size_t)snprintf(out + o, cap - o, "%s%s %d: ", total ? ". " : "", (en ? wd_en : wd_it)[tm.tm_wday], tm.tm_mday);
        for (int i = 0; i < m && o < cap; i++) {
            const cJSON *tx = cJSON_GetObjectItem(ev[i], "text");
            const char *t = tstr(ev[i]);
            o += (size_t)snprintf(out + o, cap - o, "%s%s%s%s", i ? "; " : "", t, t[0] ? " " : "",
                                  cJSON_IsString(tx) ? tx->valuestring : "?");
            total++;
        }
    }
    cJSON_Delete(root);
    if (!total) { snprintf(out, cap, en ? "No events %s." : "Nessun impegno %s.", when); return; }
    if (o < cap) snprintf(out + o, cap - o, ".");
}

// ---------------------------------------------------------------- reminder service

namespace {

struct Due { int minute; char text[90]; };           // minute of the day (0..1439), "09:00 chiamare Marco"
constexpr int kDueMax = 24;
Due   *s_due = nullptr;                              // today's timed events (PSRAM, allocated once)
int    s_due_n = 0;
int    s_due_yday = -1;                              // the day s_due belongs to
time_t s_cal_mtime = 0;
long   s_cal_size = -1;
int    s_last_minute = -1;                           // minute of the last check (events after it ring)

void reminders_load(const struct tm &now)
{
    s_due_n = 0;
    s_due_yday = now.tm_yday;
    char key[40];   // room for any int the compiler can imagine (-Werror=format-truncation)
    snprintf(key, sizeof key, "%04d-%02d-%02d", now.tm_year + 1900, now.tm_mon + 1, now.tm_mday);
    char *raw = slurp(kCalendar, 256 * 1024);
    cJSON *root = raw ? cJSON_Parse(raw) : nullptr;
    heap_caps_free(raw);
    cJSON *day = root ? cJSON_GetObjectItem(cJSON_GetObjectItem(root, "events"), key) : nullptr;
    const int n = cJSON_IsArray(day) ? cJSON_GetArraySize(day) : 0;
    for (int i = 0; i < n && s_due_n < kDueMax; i++) {
        const cJSON *e = cJSON_GetArrayItem(day, i);
        const cJSON *t = cJSON_GetObjectItem(e, "time"), *x = cJSON_GetObjectItem(e, "text");
        int hh, mm;
        if (!cJSON_IsString(t) || sscanf(t->valuestring, "%d:%d", &hh, &mm) != 2 || hh < 0 || hh > 23 || mm < 0 || mm > 59) continue;
        Due &d = s_due[s_due_n++];
        d.minute = hh * 60 + mm;
        snprintf(d.text, sizeof d.text, "%02d:%02d %s", hh, mm, cJSON_IsString(x) ? x->valuestring : "");
    }
    cJSON_Delete(root);
}

void heartbeat_tick(void);   // below, with the heartbeat service
void rules_post(const char *type, const char *key, const char *text);   // below, with the automations

void reminders_tick(lv_timer_t *)
{
    time_t t = time(nullptr);
    struct tm now;
    localtime_r(&t, &now);
    if (now.tm_year + 1900 < 2024) return;           // clock not set yet: nothing is "due"
    const int minute = now.tm_hour * 60 + now.tm_min;
    // Re-read the calendar only when it changed on the SD (the app, ANIMA or the web wrote it) or
    // the day turned: one stat() per tick otherwise.
    struct stat st;
    const bool have = stat(kCalendar, &st) == 0;
    const time_t mt = have ? st.st_mtime : 0;
    const long sz = have ? (long)st.st_size : -1;
    const bool first = s_due_yday < 0;
    if (now.tm_yday != s_due_yday || mt != s_cal_mtime || sz != s_cal_size) {
        if (!first && now.tm_yday != s_due_yday) s_last_minute = -1;   // a new day: every timed event is ahead
        s_cal_mtime = mt; s_cal_size = sz;
        reminders_load(now);
    }
    if (first) s_last_minute = minute;               // boot: events already past stay silent
    for (int i = 0; i < s_due_n; i++) {
        if (s_due[i].minute <= s_last_minute || s_due[i].minute > minute) continue;
        nv_notify_post(NV_NOTE_INFO, nv_i18n_get_lang() == NV_LANG_IT ? "Promemoria" : "Reminder", s_due[i].text);
        if (!nv_config_get_bool("qs_dnd", false)) nv_audio_chime();   // INFO toasts are silent by themselves
    }
    if (minute != s_last_minute) {                            // automations: "every day at 8:00"
        char hm[8];
        snprintf(hm, sizeof hm, "%02d:%02d", now.tm_hour, now.tm_min);
        rules_post("schedule", hm, "");
    }
    heartbeat_tick();
    s_last_minute = minute;
}

// ---- heartbeat (OpenClaw's idea): every "anima.hb" minutes (default 30, 0 = off) a one-shot task
// shows the model HEARTBEAT.md plus the live facts (time, today's and tomorrow's agenda). It speaks
// only when something needs the user; nothing happens without a HEARTBEAT.md or a model.
volatile bool s_hb_running = false;
int64_t       s_hb_last_ms = 0;

void hb_post(void *msg)
{
    nv_notify_post(NV_NOTE_INFO, "ANIMA", (const char *)msg);
    if (!nv_config_get_bool("qs_dnd", false)) nv_audio_chime();
    free(msg);
}

void hb_run(void)
{
    const bool en = nv_i18n_get_lang() != NV_LANG_IT;
    char *ctx = (char *)heap_caps_malloc(1400, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    char *out = (char *)heap_caps_malloc(400, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    nucleo_anima_init(en ? "en" : "it");           // idempotent: the engine may not be up yet at this hour
    if (ctx && out && nucleo_anima_try_lock()) {   // the user is talking to ANIMA: skip this round
        time_t t = time(nullptr);
        struct tm now;
        localtime_r(&t, &now);
        char today[600], tomorrow[400];
        nv_anima_agenda(0, 1, en, today, sizeof today);
        nv_anima_agenda(1, 1, en, tomorrow, sizeof tomorrow);
        snprintf(ctx, 1400, "%s %04d-%02d-%02d %02d:%02d\n%s %s\n%s %s", en ? "Now:" : "Adesso:",
                 now.tm_year + 1900, now.tm_mon + 1, now.tm_mday, now.tm_hour, now.tm_min,
                 en ? "Today:" : "Oggi:", today, en ? "Tomorrow:" : "Domani:", tomorrow);
        const int r = nucleo_anima_heartbeat(ctx, en, out, 400);
        if (r == 1) nucleo_anima_tg_notify(out);   // also to the paired Telegram chat, if any
        nucleo_anima_unlock();
        if (r == 1) {
            char *msg = strdup(out);   // posted on the LVGL thread (lv_async_call needs the port lock)
            bool sent = false;
            if (msg && lvgl_port_lock(2000)) { sent = lv_async_call(hb_post, msg) == LV_RESULT_OK; lvgl_port_unlock(); }
            if (!sent) free(msg);
        }
    }
    heap_caps_free(ctx);
    heap_caps_free(out);
}

// Created once, then sleeps until heartbeat_tick wakes it (no task churn, no self-delete).
TaskHandle_t s_hb_task = nullptr;
void hb_task(void *)
{
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        hb_run();
        s_hb_running = false;
    }
}

void heartbeat_tick(void)
{
    const int every = nv_config_get_int("anima.hb", 30);
    if (every <= 0 || s_hb_running) return;
    const int64_t now = esp_timer_get_time() / 1000;
    if (s_hb_last_ms == 0) { s_hb_last_ms = now; return; }          // first look one period after boot
    if (now - s_hb_last_ms < (int64_t)every * 60 * 1000) return;
    s_hb_last_ms = now;
    struct stat st;
    if (stat("/sdcard/data/anima/HEARTBEAT.md", &st) != 0 || st.st_size <= 0) return;   // nothing to check
    if (nucleo_anima_get_net_mode() == ANIMA_NET_OFF) return;
    // TLS + the model call want the roomy stack the ANIMA workers use; PSRAM keeps it off internal RAM.
    if (!s_hb_task && xTaskCreateWithCaps(hb_task, "anima_hb", 24 * 1024, nullptr, 3, &s_hb_task,
                                          MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        s_hb_task = nullptr;
        return;
    }
    s_hb_running = true;
    xTaskNotifyGive(s_hb_task);
}

}  // namespace

// For the Settings page / web: when the next heartbeat is due, in minutes (-1 = off or not set up).
int nv_anima_heartbeat_next_min(void)
{
    const int every = nv_config_get_int("anima.hb", 30);
    struct stat st;
    if (every <= 0 || stat("/sdcard/data/anima/HEARTBEAT.md", &st) != 0) return -1;
    const int64_t now = esp_timer_get_time() / 1000;
    const int64_t left = s_hb_last_ms ? (s_hb_last_ms + (int64_t)every * 60000 - now) / 60000 : every;
    return left < 0 ? 0 : (int)left;
}


namespace {
// ---- automations (nucleo_anima_rules.c): the OS turns what happens into events ------------------
// schedule (once a minute, only when rules.json exists), startup, app_open. A PSRAM task runs them
// under the engine gate, so a run_agent rule never blocks the UI; notifications go back to the LVGL
// thread. Telegram messages go through the rules in nv_apps/anima_channels.cpp.
QueueHandle_t s_rule_q = nullptr;
constexpr const char *kRulesFile = "/sdcard/data/anima/rules.json";

void rule_note_post(void *p)
{
    char *m = (char *)p;
    char *sep = strchr(m, '\x1f');
    if (sep) { *sep = 0; nv_notify_post(NV_NOTE_INFO, m, sep + 1); }
    if (!nv_config_get_bool("qs_dnd", false)) nv_audio_chime();
    free(m);
}

void rule_notify(const char *title, const char *text)   // from the rules task
{
    const size_t n = strlen(title) + strlen(text) + 2;
    char *m = (char *)malloc(n);
    if (!m) return;
    snprintf(m, n, "%s\x1f%s", title, text);
    bool sent = false;
    if (lvgl_port_lock(2000)) { sent = lv_async_call(rule_note_post, m) == LV_RESULT_OK; lvgl_port_unlock(); }
    if (!sent) free(m);
}

// Home Assistant state changes for "ha_state" rules: every 5 s, one /api/template request that
// returns "entity=state" for the watched entities only (a few lines), then the engine diffs them.
// Plain esp_http_client with the token from Settings > Casa: works in ANIMA's offline mode too
// (the home is on the LAN).
void ha_poll(void)
{
    struct stat st;
    if (stat(kRulesFile, &st) != 0) return;
    char url[200], token[300];
    nv_config_get_str("ha_url", "", url, sizeof url);
    nv_config_get_str("ha_token", "", token, sizeof token);
    for (int n = (int)strlen(url); n > 0 && url[n - 1] == '/'; ) url[--n] = 0;
    if (!url[0] || !token[0]) return;
    char *tpl = (char *)heap_caps_malloc(1800, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    char *resp = (char *)heap_caps_malloc(2048, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    bool locked = false;
    if (tpl && resp && (locked = nucleo_anima_try_lock())) {
        const int n = nucleo_anima_rules_ha_watch(tpl, 1800);
        nucleo_anima_unlock();
        locked = false;
        cJSON *o = n > 0 ? cJSON_CreateObject() : nullptr;
        char *body = nullptr;
        if (o) { cJSON_AddStringToObject(o, "template", tpl); body = cJSON_PrintUnformatted(o); cJSON_Delete(o); }
        if (body) {
            strncat(url, "/api/template", sizeof url - strlen(url) - 1);
            esp_http_client_config_t cfg = {};
            cfg.url = url;
            cfg.timeout_ms = 4000;
            cfg.method = HTTP_METHOD_POST;
            cfg.crt_bundle_attach = esp_crt_bundle_attach;
            esp_http_client_handle_t h = esp_http_client_init(&cfg);
            int got = 0;
            if (h) {
                char auth[320];
                snprintf(auth, sizeof auth, "Bearer %s", token);
                esp_http_client_set_header(h, "Authorization", auth);
                esp_http_client_set_header(h, "Content-Type", "application/json");
                const int bl = (int)strlen(body);
                if (esp_http_client_open(h, bl) == ESP_OK && esp_http_client_write(h, body, bl) == bl) {
                    esp_http_client_fetch_headers(h);
                    if (esp_http_client_get_status_code(h) == 200) {
                        int r;
                        while (got < 2047 && (r = esp_http_client_read(h, resp + got, 2047 - got)) > 0) got += r;
                    }
                }
                esp_http_client_close(h);
                esp_http_client_cleanup(h);
            }
            resp[got] = 0;
            if (got > 0 && (locked = nucleo_anima_try_lock())) {
                nucleo_anima_rules_ha_states(resp, nv_i18n_get_lang() != NV_LANG_IT);
                nucleo_anima_unlock();
                locked = false;
            }
            cJSON_free(body);
        }
    }
    heap_caps_free(tpl);
    heap_caps_free(resp);
}

void rules_task(void *)
{
    anima_event_t *ev = (anima_event_t *)heap_caps_malloc(sizeof *ev, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    for (;;) {
        if (!ev) { vTaskDelay(pdMS_TO_TICKS(1000)); continue; }
        if (xQueueReceive(s_rule_q, ev, pdMS_TO_TICKS(5000)) != pdTRUE) { ha_poll(); continue; }
        bool locked = false;
        for (int i = 0; i < 600 && !(locked = nucleo_anima_try_lock()); i++) vTaskDelay(pdMS_TO_TICKS(100));   // up to 60 s
        if (!locked) continue;
        nucleo_anima_rules_handle(ev, nv_i18n_get_lang() != NV_LANG_IT, nullptr, 0);
        nucleo_anima_unlock();
    }
}

void rules_post(const char *type, const char *key, const char *text)
{
    struct stat st;
    if (!s_rule_q || stat(kRulesFile, &st) != 0) return;   // no automations: nothing to wake
    anima_event_t ev = {};
    snprintf(ev.type, sizeof ev.type, "%s", type);
    snprintf(ev.key, sizeof ev.key, "%s", key ? key : "");
    snprintf(ev.text, sizeof ev.text, "%s", text ? text : "");
    time_t t = time(nullptr);
    struct tm tm;
    localtime_r(&t, &tm);
    ev.wday = tm.tm_wday;
    xQueueSend(s_rule_q, &ev, 0);
}

void rules_start(void)
{
    if (s_rule_q) return;
    s_rule_q = xQueueCreate(6, sizeof(anima_event_t));
    if (!s_rule_q) return;
    nucleo_anima_rules_set_notifier(rule_notify);
    if (xTaskCreateWithCaps(rules_task, "anima_rules", 24 * 1024, nullptr, 3, nullptr,
                            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        vQueueDelete(s_rule_q);
        s_rule_q = nullptr;
        return;
    }
    rules_post("startup", "boot", "");
}

}  // namespace

// ANIMA's timers and alarms (nucleo_anima_time.c) ring here, once a second, with or without a network:
// a notification and an alert tone repeated for a few seconds (alarms longer), even in Do Not Disturb,
// since the user asked for them. The SD is read only when the store changed (timers_next is cached).
int s_ring_left = 0;
char s_last_app[32] = "";
void timers_tick(lv_timer_t *)
{
    if (s_ring_left > 0) { s_ring_left--; nv_audio_alert(); }
    const char *app = nv_ui_current_app_id();                 // automations: "when I open X"
    if (app && strcmp(app, s_last_app)) {
        snprintf(s_last_app, sizeof s_last_app, "%s", app);
        if (app[0]) rules_post("app_open", app, app);
    }
    const long long next = nucleo_anima_timers_next();
    const time_t now = time(nullptr);
    if (!next || now < next) return;
    char label[64] = "";
    bool alarm = false;
    if (nucleo_anima_timers_due((long long)now, label, sizeof label, &alarm) <= 0) return;
    const bool it = nv_i18n_get_lang() == NV_LANG_IT;
    char msg[96];
    snprintf(msg, sizeof msg, "%s%s%s", alarm ? (it ? "Sveglia" : "Alarm") : (it ? "Tempo scaduto" : "Time's up"),
             label[0] ? ": " : "", label);
    nv_notify_post(NV_NOTE_WARN, alarm ? (it ? "Sveglia" : "Alarm") : "Timer", msg);
    nv_audio_alert();
    s_ring_left = alarm ? 14 : 5;
}

void nv_anima_reminders_start(void)
{
    if (s_due) return;
    s_due = (Due *)heap_caps_calloc(kDueMax, sizeof(Due), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_due) return;
    // Events already past at boot stay silent: the first tick only sets the clock mark.
    lv_timer_create(reminders_tick, 20 * 1000, nullptr);
    lv_timer_create(timers_tick, 1000, nullptr);
    rules_start();
}
