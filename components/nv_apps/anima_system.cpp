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
#include "nucleo_anima.h" // tool payload / outcome
#include "cJSON.h"        // the Calendar app's calendar.json

#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <sys/stat.h>

static void nv_anima_agenda_today(bool en, char *out, size_t cap);

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
    if (!strcmp(key, "agenda")) {   // today's events from the Calendar app
        nv_anima_agenda_today(en, out, cap);
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

// Write `len` bytes to `path` through a temp file (ENGINEERING_RULES §5): the old file is replaced
// only once the new one is complete; on a rename failure the temp file (the good copy) is kept.
bool write_atomic(const char *path, const char *data, size_t len)
{
    char tmp[200];
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE *f = fopen(tmp, "wb");
    if (!f) return false;
    const bool wok = fwrite(data, 1, len, f) == len;
    if (fclose(f) != 0 || !wok) { remove(tmp); return false; }
    remove(path);
    return rename(tmp, path) == 0;
}

void mkdirs(const char *path)   // every parent directory of `path`
{
    char p[200];
    snprintf(p, sizeof p, "%s", path);
    for (char *s = p + 1; *s; s++) if (*s == '/') { *s = 0; mkdir(p, 0775); *s = '/'; }
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
    char key[16];
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
    char id[40];
    snprintf(id, sizeof id, "a%lld-%s", (long long)now, key);
    cJSON_AddStringToObject(ev, "id", id);
    cJSON_AddStringToObject(ev, "time", hhmm);
    cJSON_AddStringToObject(ev, "text", text);
    cJSON_AddItemToArray(day, ev);
    char *out = cJSON_Print(root);
    cJSON_Delete(root);
    mkdirs(kCalendar);
    const bool ok = out && write_atomic(kCalendar, out, strlen(out));
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
    char path[200];
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
    mkdirs(path);
    const char *body = nucleo_anima_tool_content();
    const bool ok = write_atomic(path, body ? body : "", body ? strlen(body) : 0);
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

bool nv_anima_os_run(const anima_result_t *r, bool en, char *note, size_t cap)
{
    if (note && cap) note[0] = 0;
    if (!r || r->action != ANIMA_ACT_TOOL || !note || !cap) return false;
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

// "che impegni ho oggi": today's events from the Calendar app, by time.
static void nv_anima_agenda_today(bool en, char *out, size_t cap)
{
    snprintf(out, cap, "%s", en ? "No events today." : "Nessun impegno oggi.");
    time_t now = time(nullptr);
    struct tm tm;
    localtime_r(&now, &tm);
    char key[16];
    snprintf(key, sizeof key, "%04d-%02d-%02d", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday);
    char *raw = slurp(kCalendar, 256 * 1024);
    cJSON *root = raw ? cJSON_Parse(raw) : nullptr;
    heap_caps_free(raw);
    cJSON *day = root ? cJSON_GetObjectItem(cJSON_GetObjectItem(root, "events"), key) : nullptr;
    const int n = cJSON_IsArray(day) ? cJSON_GetArraySize(day) : 0;
    if (n > 0) {
        // Sort by time (an event without one goes first, like the Calendar app's day view).
        const cJSON *ev[32]; int m = 0;
        for (int i = 0; i < n && m < 32; i++) ev[m++] = cJSON_GetArrayItem(day, i);
        auto tstr = [](const cJSON *e) { const cJSON *t = cJSON_GetObjectItem(e, "time"); return cJSON_IsString(t) ? t->valuestring : ""; };
        for (int i = 1; i < m; i++) for (int j = i; j > 0 && strcmp(tstr(ev[j - 1]), tstr(ev[j])) > 0; j--) { const cJSON *x = ev[j]; ev[j] = ev[j - 1]; ev[j - 1] = x; }
        size_t o = (size_t)snprintf(out, cap, "%s", en ? "Today: " : "Oggi: ");
        for (int i = 0; i < m && o < cap; i++) {
            const cJSON *tx = cJSON_GetObjectItem(ev[i], "text");
            const char *t = tstr(ev[i]);
            o += (size_t)snprintf(out + o, cap - o, "%s%s%s%s", i ? "; " : "", t, t[0] ? " " : "",
                                  cJSON_IsString(tx) ? tx->valuestring : "?");
        }
        if (o < cap) snprintf(out + o, cap - o, ".");
    }
    cJSON_Delete(root);
}
