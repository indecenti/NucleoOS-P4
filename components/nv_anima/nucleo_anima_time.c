// nucleo_anima_time.c — timers and alarms, offline (IT/EN).
//
// "metti un timer di 10 minuti per la pasta", "timer 1h30", "avvisami tra un quarto d'ora",
// "svegliami alle 7 e mezza", "sveglia domani alle 6:45", "set an alarm for 7pm", "che timer ho?",
// "annulla il timer". The spoken forms follow the rules of Duckling / chrono (numbers in words,
// "e mezza", "meno un quarto", "mezzogiorno", am/pm, "di sera"), in a few hundred lines of C.
// Timers live in /data/anima/timers.json ({"at":epoch,"kind":"timer|alarm","label":..}); the OS rings
// them (nv_apps/anima_system.cpp, once a second) even with no network and no model.
#include "nucleo_anima.h"
#include "nucleo_board.h"
#include "cJSON.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#define TIMERS_FILE NUCLEO_SD_MOUNT "/data/anima/timers.json"
#define TMAX 24
#define TW 32

#define TO 48     // a word as the user wrote it (accents are two bytes)

// w: the normalized word the parser matches; o: the same word as written, for labels ("call Marie").
typedef struct { char w[TMAX][TW]; char o[TMAX][TO]; int n; } words_t;

// Lower-case, fold Italian accents, split on spaces/apostrophes/punctuation; keep ':' and '.' inside numbers.
static void tokenize(const char *s, words_t *t)
{
    t->n = 0;
    int k = 0, ko = 0;
    for (const unsigned char *p = (const unsigned char *)s; ; p++) {
        unsigned char c = *p;
        if (c == 0xC3 && p[1]) {
            unsigned char d = p[1];
            char f = (d >= 0xA0 && d <= 0xA5) ? 'a' : (d >= 0xA8 && d <= 0xAB) ? 'e' : (d >= 0xAC && d <= 0xAF) ? 'i'
                   : (d >= 0xB2 && d <= 0xB6) ? 'o' : (d >= 0xB9 && d <= 0xBC) ? 'u' : 0;
            if (f) {
                if (k < TW - 1 && t->n < TMAX) t->w[t->n][k++] = f;
                if (ko < TO - 2 && t->n < TMAX) { t->o[t->n][ko++] = (char)c; t->o[t->n][ko++] = (char)d; }
                p++; continue;
            }
        }
        const bool keep = isalnum(c) || ((c == ':' || c == '.') && k > 0 && isdigit(p[1]));
        if (keep && c) {
            if (k < TW - 1 && t->n < TMAX) t->w[t->n][k++] = (char)tolower(c);
            if (ko < TO - 1 && t->n < TMAX) t->o[t->n][ko++] = (char)c;
            continue;
        }
        if (k && t->n < TMAX) { t->w[t->n][k] = 0; t->o[t->n][ko] = 0; t->n++; }
        k = 0; ko = 0;
        if (!c) break;
    }
}

static bool is(const char *w, const char *const *set)
{
    for (int i = 0; set[i]; i++) if (!strcmp(w, set[i])) return true;
    return false;
}

// A number in digits or words (IT/EN), 0..999; -1 if not a number. "un/una/a/an" = 1.
static int num(const char *w)
{
    if (isdigit((unsigned char)w[0])) { char *e; long v = strtol(w, &e, 10); return (*e == 0 && v < 1000) ? (int)v : -1; }
    static const struct { const char *w; int v; } N[] = {
        {"un",1},{"uno",1},{"una",1},{"a",1},{"an",1},{"one",1},{"due",2},{"two",2},{"tre",3},{"three",3},
        {"quattro",4},{"four",4},{"cinque",5},{"five",5},{"sei",6},{"six",6},{"sette",7},{"seven",7},{"otto",8},
        {"eight",8},{"nove",9},{"nine",9},{"dieci",10},{"ten",10},{"undici",11},{"eleven",11},{"dodici",12},
        {"twelve",12},{"tredici",13},{"quattordici",14},{"quindici",15},{"fifteen",15},{"sedici",16},
        {"diciassette",17},{"diciotto",18},{"diciannove",19},{"venti",20},{"twenty",20},{"venticinque",25},
        {"trenta",30},{"thirty",30},{"quaranta",40},{"forty",40},{"quarantacinque",45},{"cinquanta",50},
        {"fifty",50},{"sessanta",60},{"sixty",60},{"novanta",90},{"ninety",90},{"cento",100},{NULL,0} };
    for (int i = 0; N[i].w; i++) if (!strcmp(w, N[i].w)) return N[i].v;
    return -1;
}

static int unit_secs(const char *w)
{
    static const char *const S[] = {"s","sec","secs","secondo","secondi","second","seconds",NULL};
    static const char *const M[] = {"m","min","mins","minuto","minuti","minute","minutes",NULL};
    static const char *const H[] = {"h","ora","ore","hour","hours","hr","hrs",NULL};
    return is(w, S) ? 1 : is(w, M) ? 60 : is(w, H) ? 3600 : 0;
}

// A duration anywhere in the words: sum of "N unit" pairs, "1h30", "90s", "mezz'ora", "un quarto
// d'ora", "e mezza/mezzo" after hours, "half an hour", "an hour and a half". Marks the used words.
static int duration(const words_t *t, bool used[TMAX])
{
    int total = 0, last_unit = 0;
    for (int i = 0; i < t->n; i++) {
        const char *w = t->w[i];
        // compact forms: 1h30, 2h, 45m, 90s, 1h30m
        if (isdigit((unsigned char)w[0])) {
            int h = 0, m = 0, s = 0, v = 0; bool any = false, ok = true;
            for (const char *p = w; *p; p++) {
                if (isdigit((unsigned char)*p)) { v = v * 10 + (*p - '0'); continue; }
                if (*p == 'h') { h = v; v = 0; any = true; } else if (*p == 'm') { m = v; v = 0; any = true; }
                else if (*p == 's') { s = v; v = 0; any = true; } else { ok = false; break; }
            }
            if (ok && any) { if (v && h && !m) m = v; total += h * 3600 + m * 60 + s; used[i] = true; last_unit = h ? 3600 : 60; continue; }
        }
        if (!strcmp(w, "mezzora")) { total += 1800; used[i] = true; continue; }
        if (!strcmp(w, "mezz") && i + 1 < t->n && !strcmp(t->w[i+1], "ora")) { total += 1800; used[i] = used[i+1] = true; i++; continue; }
        if (!strcmp(w, "quarto") && i + 2 < t->n && t->w[i+1][0] == 'd' && !strcmp(t->w[i+2], "ora")) {   // un quarto d'ora
            total += 900; used[i] = used[i+1] = used[i+2] = true; if (i && num(t->w[i-1]) == 1) used[i-1] = true; i += 2; continue;
        }
        if (!strcmp(w, "half") && i + 2 < t->n && !strcmp(t->w[i+1], "an") && !strcmp(t->w[i+2], "hour")) {
            total += 1800; used[i] = used[i+1] = used[i+2] = true; i += 2; continue;
        }
        if ((!strcmp(w, "mezza") || !strcmp(w, "mezzo") || !strcmp(w, "half")) && last_unit && i && (!strcmp(t->w[i-1], "e") || !strcmp(t->w[i-1], "a"))) {
            total += last_unit / 2; used[i] = used[i-1] = true; if (i >= 2 && !strcmp(t->w[i-2], "and")) used[i-2] = true; continue;
        }
        int v = num(w);
        if (v >= 0 && i + 1 < t->n) {
            int u = unit_secs(t->w[i+1]);
            if (u) { total += v * u; used[i] = used[i+1] = true; last_unit = u; i++; continue; }
        }
        if (unit_secs(w) == 3600 && (!i || !used[i-1]) && i && (!strcmp(t->w[i-1], "un") || !strcmp(t->w[i-1], "an"))) {
            total += 3600; used[i] = used[i-1] = true; last_unit = 3600; continue;
        }
        // "1 ora e 30" (minutes implied after hours)
        if (v > 0 && v < 60 && last_unit == 3600 && i && !strcmp(t->w[i-1], "e")) { total += v * 60; used[i] = used[i-1] = true; last_unit = 60; continue; }
    }
    return total;
}

// A clock time: "alle 7", "alle 7:30", "7.30", "alle 7 e mezza/un quarto/20", "alle 8 meno un quarto",
// "mezzogiorno", "mezzanotte", "7pm", "7 di sera", "at 6:45 am". hh/mm or false.
static bool clock_time(const words_t *t, bool used[TMAX], int *hh, int *mm)
{
    static const char *const AT[] = {"alle","alla","all","le","ore","at","per","for",NULL};
    int h = -1, m = 0, at = -1;
    for (int i = 0; i < t->n && h < 0; i++) {
        const char *w = t->w[i];
        if (!strcmp(w, "mezzogiorno") || !strcmp(w, "noon")) { h = 12; at = i; break; }
        if (!strcmp(w, "mezzanotte") || !strcmp(w, "midnight")) { h = 0; at = i; break; }
        const bool prev_at = i && is(t->w[i-1], AT);
        const char *c = strpbrk(w, ":.");
        if (isdigit((unsigned char)w[0]) && c) { h = atoi(w); m = atoi(c + 1); at = i; }
        else if (isdigit((unsigned char)w[0]) && (strstr(w, "pm") || strstr(w, "am"))) { h = atoi(w); at = i; }
        else if (prev_at && num(w) >= 0 && num(w) <= 24 && !(i + 1 < t->n && unit_secs(t->w[i+1]))) { h = num(w); at = i; }
        if (h >= 0 && i && is(t->w[i-1], AT)) used[i-1] = true;
    }
    if (h < 0) return false;
    used[at] = true;
    const char *w = t->w[at];
    if (strstr(w, "pm") && h < 12) h += 12;
    if (strstr(w, "am") && h == 12) h = 0;
    int j = at + 1;
    if (j + 1 < t->n && (!strcmp(t->w[j], "e") || !strcmp(t->w[j], "meno"))) {           // e mezza / meno un quarto
        const bool minus = !strcmp(t->w[j], "meno");
        int add = -1, k = j + 1;
        if (!strcmp(t->w[k], "mezza") || !strcmp(t->w[k], "mezzo")) add = 30;
        else if (!strcmp(t->w[k], "quarto")) add = 15;
        else if (num(t->w[k]) == 1 && k + 1 < t->n && !strcmp(t->w[k+1], "quarto")) { add = 15; used[k] = true; k++; }
        else if (!strcmp(t->w[k], "tre") && k + 1 < t->n && !strcmp(t->w[k+1], "quarti")) { add = 45; used[k] = true; k++; }
        else if (num(t->w[k]) > 0 && num(t->w[k]) < 60) add = num(t->w[k]);
        if (add >= 0) { used[j] = used[k] = true; if (minus) { h = (h + 23) % 24; m = 60 - add; } else m = add; j = k + 1; }
    }
    for (; j < t->n && j <= at + 7; j++) {                                                  // di sera / pm / del mattino
        const char *x = t->w[j];
        if (!strcmp(x, "pm") || !strcmp(x, "sera") || !strcmp(x, "pomeriggio") || !strcmp(x, "evening") || !strcmp(x, "afternoon")) {
            if (h < 12) h += 12; used[j] = true; if (!strcmp(t->w[j-1], "di") || !strcmp(t->w[j-1], "in") || !strcmp(t->w[j-1], "the")) used[j-1] = true;
        } else if (!strcmp(x, "am") || !strcmp(x, "mattina") || !strcmp(x, "mattino") || !strcmp(x, "morning")) {
            if (h == 12) h = 0; used[j] = true; if (!strcmp(t->w[j-1], "di") || !strcmp(t->w[j-1], "del") || !strcmp(t->w[j-1], "in") || !strcmp(t->w[j-1], "the")) used[j-1] = true;
        }
    }
    if (h > 23 || m > 59 || m < 0) return false;
    *hh = h; *mm = m;
    return true;
}

// ---- the store --------------------------------------------------------------------------------
static cJSON *load(void)
{
    FILE *f = fopen(TIMERS_FILE, "rb");
    cJSON *a = NULL;
    if (f) {
        char b[4096]; size_t n = fread(b, 1, sizeof b - 1, f); fclose(f); b[n] = 0;
        a = cJSON_Parse(b);
    }
    if (!cJSON_IsArray(a)) { cJSON_Delete(a); a = cJSON_CreateArray(); }
    return a;
}

static volatile bool s_next_valid;   // the cached earliest "at" (the OS asks once a second)
static long long s_next;

static bool save(cJSON *a)
{
    s_next_valid = false;
    char *s = cJSON_PrintUnformatted(a);
    if (!s) return false;
    mkdir(NUCLEO_SD_MOUNT "/data", 0777);
    mkdir(NUCLEO_SD_MOUNT "/data/anima", 0777);
    FILE *f = fopen(TIMERS_FILE ".tmp", "wb");
    bool ok = f && fputs(s, f) >= 0;
    if (f) ok = (fclose(f) == 0) && ok;
    cJSON_free(s);
    if (!ok) { remove(TIMERS_FILE ".tmp"); return false; }
    // FAT cannot rename onto an existing file: without this remove() every save after the first
    // failed, a rung timer stayed in the store and rang again every second, forever.
    remove(TIMERS_FILE);
    return rename(TIMERS_FILE ".tmp", TIMERS_FILE) == 0;
}

// Timers already rung this boot (up to this time): never ring twice, even if the store could not be
// rewritten (SD full / pulled) — the entry is then dropped from the store at the next good save.
static long long s_rung_until;

long long nucleo_anima_timers_next(void)
{
    if (!s_next_valid) {
        cJSON *a = load();
        long long m = 0;
        for (int i = 0; i < cJSON_GetArraySize(a); i++) {
            cJSON *at = cJSON_GetObjectItem(cJSON_GetArrayItem(a, i), "at");
            if (cJSON_IsNumber(at) && (long long)at->valuedouble > s_rung_until &&
                (!m || (long long)at->valuedouble < m)) m = (long long)at->valuedouble;
        }
        cJSON_Delete(a);
        s_next = m;
        s_next_valid = true;
    }
    return s_next;
}

int nucleo_anima_timers_due(long long now, char *label, int cap, bool *alarm)
{
    cJSON *a = load();
    int fired = 0;
    for (int i = cJSON_GetArraySize(a) - 1; i >= 0; i--) {
        cJSON *e = cJSON_GetArrayItem(a, i), *at = cJSON_GetObjectItem(e, "at");
        if (!cJSON_IsNumber(at) || (long long)at->valuedouble > now) continue;
        if ((long long)at->valuedouble <= s_rung_until) { cJSON_DeleteItemFromArray(a, i); continue; }   // rung already
        if (!fired) {
            cJSON *l = cJSON_GetObjectItem(e, "label"), *k = cJSON_GetObjectItem(e, "kind");
            if (label && cap) snprintf(label, cap, "%s", cJSON_IsString(l) ? l->valuestring : "");
            if (alarm) *alarm = cJSON_IsString(k) && !strcmp(k->valuestring, "alarm");
        }
        cJSON_DeleteItemFromArray(a, i);
        fired++;
    }
    if (fired) { s_rung_until = now; save(a); }
    cJSON_Delete(a);
    return fired;
}

static void hhmm(time_t t, char *o, int cap) { struct tm x; localtime_r(&t, &x); snprintf(o, cap, "%02d:%02d", x.tm_hour, x.tm_min); }

static void say_dur(int s, bool en, char *o, int cap)
{
    int h = s / 3600, m = (s % 3600) / 60, sec = s % 60;
    int n = 0;
    o[0] = 0;
    if (h) n += en ? snprintf(o + n, cap - n, "%d h", h) : snprintf(o + n, cap - n, "%d %s", h, h == 1 ? "ora" : "ore");
    if (m) n += snprintf(o + n, cap - n, "%s%d min", n ? " " : "", m);
    if (sec || !n) snprintf(o + n, cap - n, "%s%d s", n ? " " : "", sec);
}

// The label: what follows "per"/"for"/"di"/"to" after the time words, minus fillers ("la pasta").
static void label_of(const words_t *t, const bool used[TMAX], char *out, int cap)
{
    static const char *const SKIP[] = {"metti","imposta","avvia","fai","partire","un","uno","una","il","lo","la","i",
        "timer","countdown","conto","alla","rovescia","sveglia","svegliami","svegli","allarme","avvisami","avvisa",
        "chiamami","dimmi","mi","di","da","tra","fra","in","per","set","start","a","an","the","alarm","wake","me",
        "up","remind","tell","after","for","to","please","per favore","domani","tomorrow","oggi","today","and","e", NULL};
    out[0] = 0;
    int n = 0, start = -1;
    for (int i = 0; i < t->n; i++) if (!used[i] && (!strcmp(t->w[i], "per") || !strcmp(t->w[i], "for"))) start = i + 1;
    for (int i = start >= 0 ? start : 0; i < t->n; i++) {
        if (used[i] || (start < 0 && is(t->w[i], SKIP))) continue;
        n += snprintf(out + n, cap - n, "%s%s", n ? " " : "", t->o[i]);   // as written: "call Marie"
        if (n >= cap - 1) break;
    }
}

static bool has(const words_t *t, const char *const *set)
{
    for (int i = 0; i < t->n; i++) if (is(t->w[i], set)) return true;
    return false;
}

// 1 = handled (reply written), 0 = not a timer/alarm request.
int nucleo_anima_timer_tool(const char *raw, bool en, long long now_epoch, anima_result_t *r)
{
    words_t t;
    tokenize(raw, &t);
    if (!t.n) return 0;
    static const char *const TIMER[] = {"timer","countdown","cronometro",NULL};
    static const char *const ALARM[] = {"sveglia","svegliami","sveglie","alarm","alarms","wake",NULL};
    static const char *const NOTIFY[] = {"avvisami","avvisa","chiamami","remind","notify",NULL};
    static const char *const CANCEL[] = {"annulla","cancella","elimina","ferma","stop","togli","rimuovi","cancel","delete","remove",NULL};
    static const char *const LIST[] = {"quali","quanti","che","elenca","lista","mostra","list","show","which","what","how",NULL};
    // "timer 10 minutes pasta" from a model's ACT line: the label is whatever is left, as with "per"
    bool timer = has(&t, TIMER), alarm = has(&t, ALARM), notify = has(&t, NOTIFY);
    if (!timer && !alarm && !notify) {   // "conto alla rovescia"
        for (int i = 0; i + 2 < t.n; i++) if (!strcmp(t.w[i], "conto") && !strcmp(t.w[i+2], "rovescia")) timer = true;
    }
    if (!timer && !alarm && !notify) return 0;
    memset(r, 0, sizeof *r);
    r->tier = ANIMA_TIER_COMMAND; r->action = ANIMA_ACT_ANSWER; r->confidence = 90;
    snprintf(r->state, sizeof r->state, "tool");
    const time_t now = (time_t)now_epoch;
    char b[64];

    if (has(&t, CANCEL)) {                                   // annulla il timer / cancella le sveglie
        cJSON *a = load();
        int n = 0;
        for (int i = cJSON_GetArraySize(a) - 1; i >= 0; i--) {
            cJSON *k = cJSON_GetObjectItem(cJSON_GetArrayItem(a, i), "kind");
            const bool is_alarm = cJSON_IsString(k) && !strcmp(k->valuestring, "alarm");
            if ((alarm && is_alarm) || (timer && !is_alarm) || (!alarm && !timer)) { cJSON_DeleteItemFromArray(a, i); n++; }
        }
        save(a); cJSON_Delete(a);
        snprintf(r->intent, sizeof r->intent, "timer_cancel");
        if (!n) snprintf(r->reply, sizeof r->reply, "%s", en ? "Nothing to cancel." : "Non c'e' nulla da annullare.");
        else if (en) snprintf(r->reply, sizeof r->reply, "Cancelled %d %s.", n, alarm ? (n == 1 ? "alarm" : "alarms") : (n == 1 ? "timer" : "timers"));
        else snprintf(r->reply, sizeof r->reply, "%s %d %s.", alarm ? (n == 1 ? "Annullata" : "Annullate") : (n == 1 ? "Annullato" : "Annullati"),
                      n, alarm ? (n == 1 ? "sveglia" : "sveglie") : "timer");
        return 1;
    }

    bool used[TMAX] = {0};
    int secs = duration(&t, used);
    int hh = -1, mm = 0;
    const bool clock = secs <= 0 && clock_time(&t, used, &hh, &mm);
    // "apri l'app timer", "cos'e' un timer?": an app or a question, not a command
    static const char *const OPEN[] = {"apri","open","lancia","avvia","launch",NULL};
    static const char *const ASK[] = {"cos","cosa","significa","spiega","what","explain","funziona","works",NULL};
    if (secs <= 0 && !clock && (has(&t, OPEN) || has(&t, ASK)) && !has(&t, LIST)) return 0;
    if (secs <= 0 && !clock && has(&t, ASK)) return 0;

    if (secs <= 0 && !clock && (has(&t, LIST) || t.n <= 2)) {   // "che timer ho?", "timer", "sveglie"
        cJSON *a = load();
        int n = 0, o = 0;
        char list[300] = "";
        for (int i = 0; i < cJSON_GetArraySize(a) && o < (int)sizeof list - 60; i++) {
            cJSON *e = cJSON_GetArrayItem(a, i), *at = cJSON_GetObjectItem(e, "at"), *l = cJSON_GetObjectItem(e, "label"), *k = cJSON_GetObjectItem(e, "kind");
            if (!cJSON_IsNumber(at)) continue;
            const bool is_alarm = cJSON_IsString(k) && !strcmp(k->valuestring, "alarm");
            char w[8]; hhmm((time_t)at->valuedouble, w, sizeof w);
            const char *lb = cJSON_IsString(l) ? l->valuestring : "";
            if (is_alarm) {
                o += snprintf(list + o, sizeof list - o, "%s%s %s%s%s", n ? "; " : "", en ? "alarm" : "sveglia", w, lb[0] ? " " : "", lb);
            } else {
                char d[24];
                say_dur((int)(at->valuedouble - now), en, d, sizeof d);
                o += snprintf(list + o, sizeof list - o, "%stimer%s%s (%s %s)", n ? "; " : "", lb[0] ? " " : "", lb, en ? "left:" : "mancano", d);
            }
            n++;
        }
        cJSON_Delete(a);
        snprintf(r->intent, sizeof r->intent, "timer_list");
        snprintf(r->reply, sizeof r->reply, n ? "%s" : (en ? "No timers or alarms running." : "Nessun timer o sveglia attivo."), list);
        return 1;
    }
    if (secs <= 0 && !clock) {                               // "metti un timer" without a time: ask
        snprintf(r->intent, sizeof r->intent, "timer");
        snprintf(r->reply, sizeof r->reply, "%s", alarm ? (en ? "For what time? (e.g. \"alarm at 7:30\")" : "Per che ora? (es. \"sveglia alle 7:30\")")
                                                         : (en ? "For how long? (e.g. \"timer 10 minutes\")" : "Per quanto tempo? (es. \"timer di 10 minuti\")"));
        return 1;
    }
    if (secs > 0 && secs > 7 * 86400) secs = 7 * 86400;

    time_t at;
    bool is_alarm = alarm && clock;
    if (clock) {
        struct tm x; localtime_r(&now, &x);
        x.tm_hour = hh; x.tm_min = mm; x.tm_sec = 0;
        if (has(&t, (const char *const[]){"domani","tomorrow",NULL})) x.tm_mday += 1;
        at = mktime(&x);
        if (at <= now) { x.tm_mday += 1; at = mktime(&x); }   // 7:00 already past -> tomorrow
        if (!alarm) is_alarm = true;                           // "avvisami alle 7": a clock time rings like an alarm
    } else {
        at = now + secs;
    }
    char label[48];
    label_of(&t, used, label, sizeof label);

    cJSON *a = load();
    if (cJSON_GetArraySize(a) >= 20) { cJSON_Delete(a); snprintf(r->reply, sizeof r->reply, "%s", en ? "Too many timers (20): cancel some first." : "Troppi timer (20): annullane qualcuno."); return 1; }
    cJSON *e = cJSON_CreateObject();
    cJSON_AddNumberToObject(e, "at", (double)at);
    cJSON_AddStringToObject(e, "kind", is_alarm ? "alarm" : "timer");
    cJSON_AddStringToObject(e, "label", label);
    cJSON_AddItemToArray(a, e);
    const bool ok = save(a);
    cJSON_Delete(a);
    snprintf(r->intent, sizeof r->intent, "%s", is_alarm ? "alarm" : "timer");
    snprintf(r->arg, sizeof r->arg, "%lld", (long long)at);
    hhmm(at, b, sizeof b);
    if (!ok) snprintf(r->reply, sizeof r->reply, "%s", en ? "I could not save it (SD card?)." : "Non riesco a salvarlo (scheda SD?).");
    else if (is_alarm) {
        struct tm x, y; localtime_r(&at, &x); localtime_r(&now, &y);
        const bool tomorrow = x.tm_yday != y.tm_yday;
        snprintf(r->reply, sizeof r->reply, en ? "Alarm set for %s%s%s%s." : "Sveglia impostata %salle %s%s%s.",
                 en ? b : (tomorrow ? "domani " : ""), en ? (tomorrow ? " tomorrow" : "") : b,
                 label[0] ? (en ? ": " : ": ") : "", label);
    } else {
        char d[24]; say_dur(secs, en, d, sizeof d);
        snprintf(r->reply, sizeof r->reply, en ? "Timer %s%s%s started: it rings at %s." : "Timer di %s%s%s avviato: suona alle %s.",
                 d, label[0] ? (en ? " for " : " per ") : "", label, b);
    }
    return 1;
}
