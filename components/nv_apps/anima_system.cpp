// Live ANIMA_ACT_SYSTEM values (see nv_anima_system.h). Ported from the Cardputer's anima_get
// resolver (G:\Nucleo nucleo_httpd.c) onto the NucleoV2 OS APIs. Every answer is computed from
// device state — never from knowledge cards — so it is always exact.
#include "nv_anima_system.h"

#include "nv_time.h"
#include "nv_sd.h"
#include "nv_wifi.h"
#include "nv_app.h"
#include "nv_audio.h"
#include "nv_hal.h"
#include "nv_config.h"
#include "nv_i18n.h"

#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>

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
        return true;
    }
    if (!strcmp(intent, "set_brightness")) {
        int v = tool_level(arg, nv_config_get_int("brightness", 90), 5);  // 5%: never black the panel
        nv_hal_backlight_set(v);
        nv_config_set_int("brightness", v);
        return true;
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
