// nucleo_anima_rules.c — automations: events -> rules -> actions (ESP-Claw's event router, Apache-2.0,
// adapted to NucleoOS). /data/anima/rules.json is an ordered array; the first matching rules run:
//
//   {"id":"meteo","description":"il meteo ogni mattina","enabled":true,"consume_on_match":false,
//    "match":{"event_type":"schedule","at":"07:30","days":"1-5"},
//    "actions":[{"type":"run_agent","input":{"prompt":"Che tempo fa oggi?"}},
//               {"type":"send_message","input":{"channel":"telegram","text":"{{last.output}}"}}]}
//
// Events (posted by the OS): schedule (every minute: "at" HH:MM, "every" N minutes, "days" 0-6 with
// 0 = Sunday, ranges and lists), message (Telegram text; "text" + "text_match_rule":"prefix" exposes
// {{match.remainder}}), startup, app_open (event_key = app id). Actions: run_agent {prompt},
// run_sh {command}, run_script {path} (ESP-Claw's; .lua or .py), send_message {channel: telegram|notify|
// reply, text}, drop. ESP-Claw rule files keep working where the action has a NucleoOS equivalent. Templates: {{last.output}},
// {{match.remainder}}, {{event.text}}, {{event.key}}, {{date}}, {{time}}.
// The caller holds the engine gate (run_agent goes through the whole cascade).
#include "nucleo_anima.h"
#include "anima_internal.h"   // anima_shell_run: the registered shell
#include "nucleo_board.h"
#include "cJSON.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>

#define RULES_FILE NUCLEO_SD_MOUNT "/data/anima/rules.json"
#define RULES_MAX_BYTES (32 * 1024)
#define OUT_CAP 1600

static void (*s_notify)(const char *title, const char *text);
void nucleo_anima_rules_set_notifier(void (*fn)(const char *title, const char *text)) { s_notify = fn; }

static cJSON *rules_load(void)
{
    FILE *f = fopen(RULES_FILE, "rb");
    if (!f) return cJSON_CreateArray();
    char *b = malloc(RULES_MAX_BYTES + 1);
    cJSON *a = NULL;
    if (b) { size_t n = fread(b, 1, RULES_MAX_BYTES, f); b[n] = 0; a = cJSON_Parse(b); free(b); }
    fclose(f);
    if (!cJSON_IsArray(a)) { cJSON_Delete(a); a = cJSON_CreateArray(); }
    return a;
}

static bool rules_save(cJSON *a)
{
    char *s = cJSON_Print(a);
    if (!s) return false;
    mkdir(NUCLEO_SD_MOUNT "/data", 0777);
    mkdir(NUCLEO_SD_MOUNT "/data/anima", 0777);
    FILE *f = fopen(RULES_FILE ".tmp", "wb");
    bool ok = f && fputs(s, f) >= 0;
    if (f) ok = fclose(f) == 0 && ok;
    cJSON_free(s);
    return ok && rename(RULES_FILE ".tmp", RULES_FILE) == 0;
}

static const char *js(cJSON *o, const char *k)
{
    cJSON *v = o ? cJSON_GetObjectItem(o, k) : NULL;
    return cJSON_IsString(v) ? v->valuestring : NULL;
}

// "1-5", "0,6", "1-3,5", "" (every day): is weekday w (0 = Sunday) in it?
static bool days_has(const char *d, int w)
{
    if (!d || !*d) return true;
    for (const char *p = d; *p; ) {
        while (*p == ' ' || *p == ',') p++;
        if (!isdigit((unsigned char)*p)) { p++; continue; }
        int a = (int)strtol(p, (char **)&p, 10), b = a;
        if (*p == '-') { p++; b = (int)strtol(p, (char **)&p, 10); }
        if (w >= a && w <= b) return true;
    }
    return false;
}

// Does the rule's "match" hit this event? remainder gets the text after a prefix command.
static bool matches(cJSON *m, const anima_event_t *ev, char *remainder, int rcap)
{
    remainder[0] = 0;
    const char *t = js(m, "event_type");
    if (!t || strcmp(t, ev->type)) return false;
    const char *k = js(m, "event_key");
    if (k && strcmp(k, ev->key)) return false;
    if (!strcmp(ev->type, "schedule")) {
        int hh = 0, mm = 0;
        if (sscanf(ev->key, "%d:%d", &hh, &mm) != 2) return false;
        if (!days_has(js(m, "days"), ev->wday)) return false;
        const char *at = js(m, "at");
        cJSON *every = cJSON_GetObjectItem(m, "every");
        if (at) { int ah, am; if (sscanf(at, "%d:%d", &ah, &am) != 2 || ah != hh || am != mm) return false; }
        else if (cJSON_IsNumber(every) && every->valueint > 0) { if ((hh * 60 + mm) % every->valueint) return false; }
        else return false;                                  // a schedule needs "at" or "every"
    }
    if (!strcmp(ev->type, "ha_state")) {                     // "to": the new state, "from": the old one
        const char *to = js(m, "to"), *fr = js(m, "from");
        if (to && strcasecmp(to, ev->text)) return false;
        if (fr && strcasecmp(fr, ev->from)) return false;
    }
    const char *txt = js(m, "text");
    if (txt) {
        const char *in = ev->text;
        while (*in == ' ') in++;
        const char *rule = js(m, "text_match_rule");
        const size_t tl = strlen(txt);
        if (rule && !strcmp(rule, "prefix")) {
            if (strncasecmp(in, txt, tl) || (in[tl] && in[tl] != ' ')) return false;
            const char *r = in + tl;
            while (*r == ' ') r++;
            snprintf(remainder, rcap, "%s", r);
        } else if (strcasecmp(in, txt)) {
            return false;
        }
    }
    return true;
}

// {{last.output}} {{match.remainder}} {{event.text}} {{event.key}} {{date}} {{time}}
static void render(const char *tpl, const anima_event_t *ev, const char *last, const char *rem, char *out, int cap)
{
    int n = 0;
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    for (const char *p = tpl; *p && n < cap - 1; ) {
        if (p[0] == '{' && p[1] == '{') {
            const char *e = strstr(p, "}}");
            if (e) {
                char key[32]; int kl = (int)(e - p - 2);
                if (kl > 0 && kl < (int)sizeof key) {
                    memcpy(key, p + 2, (size_t)kl); key[kl] = 0;
                    char tmp[24];
                    const char *v = !strcmp(key, "last.output") ? last : !strcmp(key, "match.remainder") ? rem
                                  : !strcmp(key, "event.text") ? ev->text : !strcmp(key, "event.key") ? ev->key
                                  : !strcmp(key, "event.from") ? ev->from : NULL;
                    if (!strcmp(key, "date")) { snprintf(tmp, sizeof tmp, "%04d-%02d-%02d", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday); v = tmp; }
                    if (!strcmp(key, "time")) { snprintf(tmp, sizeof tmp, "%02d:%02d", tm.tm_hour, tm.tm_min); v = tmp; }
                    if (v) { n += snprintf(out + n, cap - n, "%s", v); if (n >= cap) n = cap - 1; p = e + 2; continue; }
                }
            }
        }
        out[n++] = *p++;
    }
    out[n] = 0;
}

int nucleo_anima_rules_handle(const anima_event_t *ev, bool en, char *reply, int cap)
{
    if (reply && cap) reply[0] = 0;
    if (!ev || !ev->type[0]) return 0;
    cJSON *rules = rules_load();
    char *last = calloc(1, OUT_CAP), *buf = malloc(OUT_CAP);
    int matched = 0, consumed = 0;
    char rem[300];
    for (int i = 0; last && buf && i < cJSON_GetArraySize(rules) && !consumed; i++) {
        cJSON *r = cJSON_GetArrayItem(rules, i);
        cJSON *on = cJSON_GetObjectItem(r, "enabled");
        if (cJSON_IsFalse(on) || !matches(cJSON_GetObjectItem(r, "match"), ev, rem, sizeof rem)) continue;
        matched++;
        cJSON *acts = cJSON_GetObjectItem(r, "actions"), *a;
        cJSON_ArrayForEach(a, acts) {
            const char *ty = js(a, "type") ? js(a, "type") : js(a, "kind");
            cJSON *in = cJSON_GetObjectItem(a, "input");
            if (!ty) continue;
            if (!strcmp(ty, "drop")) { consumed = 1; break; }
            if (!strcmp(ty, "run_agent")) {
                render(js(in, "prompt") ? js(in, "prompt") : "{{event.text}}", ev, last, rem, buf, OUT_CAP);
                anima_result_t *res = malloc(sizeof *res);
                if (res) {
                    char prev[12];
                    snprintf(prev, sizeof prev, "%s", nucleo_anima_set_origin("rule"));
                    *res = nucleo_anima_query(buf, en ? "en" : "it");
                    nucleo_anima_set_origin(prev);
                    const char *lr = nucleo_anima_long_reply();
                    snprintf(last, OUT_CAP, "%s", lr && lr[0] ? lr : res->reply);
                    free(res);
                }
            } else if (!strcmp(ty, "run_script")) {             // ESP-Claw's action: a script file
                const char *path = js(in, "path");
                if (path) {
                    const size_t pl = strlen(path);
                    snprintf(buf, OUT_CAP, "%s %s", pl > 3 && !strcmp(path + pl - 3, ".py") ? "python" : "lua", path);
                    if (anima_shell_run(buf, last, OUT_CAP) < 0) snprintf(last, OUT_CAP, "(shell unavailable)");
                }
            } else if (!strcmp(ty, "run_sh")) {
                render(js(in, "command") ? js(in, "command") : "", ev, last, rem, buf, OUT_CAP);
                if (buf[0] && anima_shell_run(buf, last, OUT_CAP) < 0) snprintf(last, OUT_CAP, "(shell unavailable)");
            } else if (!strcmp(ty, "send_message")) {
                const char *ch = js(in, "channel");
                render(js(in, "text") ? js(in, "text") : "{{last.output}}", ev, last, rem, buf, OUT_CAP);
                if (ch && !strcmp(ch, "telegram")) nucleo_anima_tg_notify(buf);
                else if (ch && !strcmp(ch, "reply")) { if (reply && cap) snprintf(reply, cap, "%s", buf); }
                else if (s_notify) s_notify(js(r, "description") ? js(r, "description") : "ANIMA", buf);
            }
        }
        cJSON *cm = cJSON_GetObjectItem(r, "consume_on_match");
        if (cJSON_IsTrue(cm)) consumed = 1;
        const char *ack = js(r, "ack");
        if (consumed && reply && cap && !reply[0]) {
            if (ack) render(ack, ev, last, rem, reply, cap);
            else snprintf(reply, cap, "%s", last);
        }
    }
    free(last); free(buf);
    cJSON_Delete(rules);
    return consumed ? 2 : matched ? 1 : 0;
}

// ---- managing rules (the model's ACT rule, the shell, the web) ------------------------------
int nucleo_anima_rules_add(const char *json, bool en, char *msg, int cap)
{
    cJSON *r = cJSON_Parse(json);
    const char *id = js(r, "id");
    cJSON *m = r ? cJSON_GetObjectItem(r, "match") : NULL, *a = r ? cJSON_GetObjectItem(r, "actions") : NULL;
    if (!cJSON_IsObject(r) || !id || !id[0] || !cJSON_IsObject(m) || !js(m, "event_type") || !cJSON_IsArray(a) || !cJSON_GetArraySize(a)) {
        snprintf(msg, cap, "%s", en ? "invalid rule: it needs id, match.event_type and actions"
                                    : "regola non valida: servono id, match.event_type e actions");
        cJSON_Delete(r);
        return 0;
    }
    if (!strcmp(js(m, "event_type"), "schedule") && !js(m, "at") && !cJSON_IsNumber(cJSON_GetObjectItem(m, "every"))) {
        snprintf(msg, cap, "%s", en ? "a schedule rule needs match.at (HH:MM) or match.every (minutes)"
                                    : "una regola schedule vuole match.at (HH:MM) o match.every (minuti)");
        cJSON_Delete(r);
        return 0;
    }
    char idc[64];
    snprintf(idc, sizeof idc, "%s", id);                             // r is handed to the array below
    id = idc;
    cJSON *rules = rules_load();
    for (int i = cJSON_GetArraySize(rules) - 1; i >= 0; i--)          // same id: replace
        if (js(cJSON_GetArrayItem(rules, i), "id") && !strcmp(js(cJSON_GetArrayItem(rules, i), "id"), id)) cJSON_DeleteItemFromArray(rules, i);
    if (cJSON_GetArraySize(rules) >= 40) { cJSON_Delete(rules); cJSON_Delete(r); snprintf(msg, cap, "%s", en ? "too many rules (40)" : "troppe regole (40)"); return 0; }
    if (!cJSON_GetObjectItem(r, "enabled")) cJSON_AddBoolToObject(r, "enabled", true);
    cJSON_AddItemToArray(rules, r);
    const bool ok = rules_save(rules);
    cJSON_Delete(rules);
    snprintf(msg, cap, ok ? (en ? "automation \"%s\" saved" : "automazione \"%s\" salvata") : (en ? "cannot save rules.json (%s)" : "non riesco a salvare rules.json (%s)"), id);
    return ok;
}

int nucleo_anima_rules_delete(const char *id)
{
    cJSON *rules = rules_load();
    int n = 0;
    for (int i = cJSON_GetArraySize(rules) - 1; i >= 0; i--) {
        const char *x = js(cJSON_GetArrayItem(rules, i), "id");
        if (x && (!strcmp(x, id) || !strcmp(id, "*"))) { cJSON_DeleteItemFromArray(rules, i); n++; }
    }
    if (n) rules_save(rules);
    cJSON_Delete(rules);
    return n;
}

int nucleo_anima_rules_list(bool en, char *out, int cap)
{
    cJSON *rules = rules_load();
    int n = 0, len = 0;
    out[0] = 0;
    cJSON *r;
    cJSON_ArrayForEach(r, rules) {
        cJSON *m = cJSON_GetObjectItem(r, "match");
        const char *d = js(r, "description");
        char when[64] = "";
        const char *t = js(m, "event_type") ? js(m, "event_type") : "?";
        if (!strcmp(t, "schedule")) {
            cJSON *ev = cJSON_GetObjectItem(m, "every");
            if (js(m, "at")) snprintf(when, sizeof when, "%s %s%s", en ? "at" : "alle", js(m, "at"), js(m, "days") ? (en ? " (days " : " (giorni ") : "");
            else if (cJSON_IsNumber(ev)) snprintf(when, sizeof when, en ? "every %d min" : "ogni %d min", ev->valueint);
            if (js(m, "days") && js(m, "at")) { size_t wl = strlen(when); snprintf(when + wl, sizeof when - wl, "%s)", js(m, "days")); }
        } else if (js(m, "text")) snprintf(when, sizeof when, "%s \"%s\"", t, js(m, "text"));
        else snprintf(when, sizeof when, "%s%s%s", t, js(m, "event_key") ? " " : "", js(m, "event_key") ? js(m, "event_key") : "");
        len += snprintf(out + len, cap - len, "%s%s%s: %s%s%s", n ? "\n" : "", cJSON_IsFalse(cJSON_GetObjectItem(r, "enabled")) ? "(off) " : "",
                        js(r, "id") ? js(r, "id") : "?", when, d ? " - " : "", d ? d : "");
        if (len >= cap) { len = cap - 1; break; }
        n++;
    }
    cJSON_Delete(rules);
    if (!n) snprintf(out, cap, "%s", en ? "No automations." : "Nessuna automazione.");
    return n;
}

// ---- Home Assistant state changes -------------------------------------------------------------
#define HA_WATCH_MAX 24
static struct { char id[48]; char st[48]; } s_ha[HA_WATCH_MAX];
static int s_ha_n = -1;          // -1 = not primed: the first answer only remembers

int nucleo_anima_rules_ha_watch(char *tpl, int cap)
{
    cJSON *rules = rules_load();
    int n = 0, len = 0;
    char ids[HA_WATCH_MAX][48];
    cJSON *r;
    cJSON_ArrayForEach(r, rules) {
        cJSON *m = cJSON_GetObjectItem(r, "match");
        const char *t = js(m, "event_type"), *k = js(m, "event_key");
        if (cJSON_IsFalse(cJSON_GetObjectItem(r, "enabled")) || !t || strcmp(t, "ha_state") || !k || !strchr(k, '.')) continue;
        bool dup = false;
        for (int i = 0; i < n && !dup; i++) dup = !strcmp(ids[i], k);
        if (!dup && n < HA_WATCH_MAX && strlen(k) < 48 && !strpbrk(k, "'\"{}%")) snprintf(ids[n++], 48, "%s", k);
    }
    cJSON_Delete(rules);
    if (!n) { s_ha_n = -1; return 0; }
    len = snprintf(tpl, cap, "{%% for e in [");
    for (int i = 0; i < n && len < cap - 64; i++) len += snprintf(tpl + len, cap - len, "%s'%s'", i ? "," : "", ids[i]);
    len += snprintf(tpl + len, cap - len, "] %%}{{ e }}={{ states(e) }}\n{%% endfor %%}");
    return len < cap ? n : 0;
}

int nucleo_anima_rules_ha_states(const char *resp, bool en)
{
    if (!resp) return 0;
    const bool primed = s_ha_n >= 0;
    if (!primed) s_ha_n = 0;
    int fired = 0;
    for (const char *p = resp; *p; ) {
        const char *e = strchr(p, '\n');
        const size_t l = e ? (size_t)(e - p) : strlen(p);
        const char *eq = memchr(p, '=', l);
        if (eq && eq > p && (size_t)(eq - p) < 48 && l - (size_t)(eq - p) - 1 < 48) {
            char id[48], st[48];
            snprintf(id, sizeof id, "%.*s", (int)(eq - p), p);
            snprintf(st, sizeof st, "%.*s", (int)(l - (size_t)(eq - p) - 1), eq + 1);
            int i = 0;
            while (i < s_ha_n && strcmp(s_ha[i].id, id)) i++;
            if (i == s_ha_n && s_ha_n < HA_WATCH_MAX) { snprintf(s_ha[i].id, sizeof s_ha[i].id, "%s", id); s_ha[i].st[0] = 0; s_ha_n++; if (primed) snprintf(s_ha[i].st, sizeof s_ha[i].st, "%s", st); }
            else if (i < s_ha_n && strcmp(s_ha[i].st, st)) {
                anima_event_t ev;
                memset(&ev, 0, sizeof ev);
                snprintf(ev.type, sizeof ev.type, "ha_state");
                snprintf(ev.key, sizeof ev.key, "%s", id);
                snprintf(ev.text, sizeof ev.text, "%s", st);
                snprintf(ev.from, sizeof ev.from, "%s", s_ha[i].st);
                snprintf(s_ha[i].st, sizeof s_ha[i].st, "%s", st);
                const time_t now = time(NULL); struct tm tm; localtime_r(&now, &tm); ev.wday = tm.tm_wday;
                // unavailable/unknown flapping is not a change worth a rule
                if (primed && strcmp(st, "unavailable") && strcmp(st, "unknown") && strcmp(ev.from, "unavailable") && strcmp(ev.from, "unknown") &&
                    nucleo_anima_rules_handle(&ev, en, NULL, 0) > 0) fired++;
            }
            if (!primed && i < s_ha_n) snprintf(s_ha[i].st, sizeof s_ha[i].st, "%s", st);
        }
        p = e ? e + 1 : p + l;
    }
    return fired;
}
