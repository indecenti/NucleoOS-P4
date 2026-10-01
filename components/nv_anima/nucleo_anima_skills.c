// SKILLS: plain-text know-how on the SD card, added by copying a file — no rebuild.
//
//   /sdcard/data/anima/skills/<name>.md
//   ---
//   name: ricette
//   description: aiuta a cucinare con quello che c'è in casa
//   triggers: ricetta, cucinare, cosa cucino, recipe
//   offline: Dimmi gli ingredienti che hai e ti propongo un piatto semplice.
//   ---
//   <instructions for the language model, any length (only the first SKILL_BODY_MAX bytes are used)>
//
// A skill whose triggers appear in the question is ACTIVE for that turn:
//   online  — its body joins the model's system prompt (nucleo_anima_skills_prompt);
//   offline — its `offline:` line answers when every grounded tier missed (nucleo_anima_skills_offline).
// The index (front matter only) is cached; rebuilt when the folder changes (or every 30 s).
//
// Also the open Agent Skills format (agentskills.io: Claude Code, Codex, Gemini CLI, OpenClaw...):
//   /sdcard/data/anima/skills/<name>/SKILL.md  (+ optional scripts/, references/, assets/)
// with YAML front matter (name, description; block scalars > and | too) or ESP-Claw's JSON front
// matter. Without `triggers:` the description's keywords activate it. Progressive disclosure: the
// catalog (nucleo_anima_skills_catalog: names, descriptions, paths) rides in the agent's prompt and the
// model reads SKILL.md and its files through the shell when a task needs them.
#include "nucleo_anima.h"
#include "nucleo_board.h"
#include "cJSON.h"
#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#define SKILLS_DIR     NUCLEO_SD_MOUNT "/data/anima/skills"
#define SKILLS_MAX     32
#define SKILL_BODY_MAX 4000
#define SKILLS_ACTIVE  2

typedef struct {
    char file[80];       // "x.md" or "<dir>/SKILL.md"
    char name[64];
    char desc[400];
    char trig[200];      // lowercase, comma-separated phrases
    char offline[240];
} skill_t;

static skill_t *s_sk;    // heap (PSRAM on the device), SKILLS_MAX entries
static int s_nsk = -1;   // -1 = never scanned
static time_t s_dir_mtime, s_scan_at;

static void lower(char *s) { for (; *s; s++) *s = (char)tolower((unsigned char)*s); }

static void trim(char *s)
{
    int n = (int)strlen(s);
    while (n && (s[n-1] == ' ' || s[n-1] == '\r' || s[n-1] == '\n' || s[n-1] == '\t')) s[--n] = 0;
    int k = 0; while (s[k] == ' ' || s[k] == '\t') k++;
    if (k) memmove(s, s + k, strlen(s + k) + 1);
}

// Parse the front matter of one file. Returns the file offset where the body starts (0 = not a skill).
static long parse_head(FILE *f, skill_t *k)
{
    char line[320];
    if (!fgets(line, sizeof line, f) || strncmp(line, "---", 3)) return 0;
    char json[1200]; int jl = 0;          // ESP-Claw style: a JSON object between the --- lines
    char block = 0;                       // YAML "description: >" / "|": the indented lines that follow
    while (fgets(line, sizeof line, f)) {
        if (!strncmp(line, "---", 3)) {
            if (jl) {
                json[jl] = 0;
                cJSON *o = cJSON_Parse(json);
                cJSON *n = o ? cJSON_GetObjectItem(o, "name") : NULL, *d = o ? cJSON_GetObjectItem(o, "description") : NULL;
                if (cJSON_IsString(n)) snprintf(k->name, sizeof k->name, "%s", n->valuestring);
                if (cJSON_IsString(d)) snprintf(k->desc, sizeof k->desc, "%s", d->valuestring);
                cJSON_Delete(o);
            }
            return ftell(f);
        }
        if (jl || (line[0] == '{' && !k->name[0])) {
            const int n = (int)strlen(line);
            if (jl + n < (int)sizeof json - 1) { memcpy(json + jl, line, (size_t)n); jl += n; }
            continue;
        }
        if (block && (line[0] == ' ' || line[0] == '\t')) {   // continuation of a block scalar
            char *v = line; trim(v);
            const size_t dl = strlen(k->desc);
            snprintf(k->desc + dl, sizeof k->desc - dl, "%s%s", dl ? " " : "", v);
            continue;
        }
        block = 0;
        if (line[0] == ' ' || line[0] == '\t' || line[0] == '#') continue;   // nested YAML (metadata:) / comments
        char *c = strchr(line, ':');
        if (!c) continue;
        *c = 0;
        char *v = c + 1;
        trim(line); trim(v);
        lower(line);
        const size_t vl = strlen(v);
        if (vl >= 2 && (v[0] == '"' || v[0] == '\'') && v[vl - 1] == v[0]) { v[vl - 1] = 0; v++; }   // quoted scalar
        if (!strcmp(line, "name"))             snprintf(k->name, sizeof k->name, "%s", v);
        else if (!strcmp(line, "description")) {
            if (!strcmp(v, ">") || !strcmp(v, "|") || !strcmp(v, ">-") || !strcmp(v, "|-") || !*v) { block = 1; k->desc[0] = 0; }
            else snprintf(k->desc, sizeof k->desc, "%s", v);
        }
        else if (!strcmp(line, "triggers"))    { snprintf(k->trig, sizeof k->trig, "%s", v); lower(k->trig); }
        else if (!strcmp(line, "offline"))     snprintf(k->offline, sizeof k->offline, "%s", v);
    }
    return 0;
}

static void scan(void)
{
    struct stat st;
    if (stat(SKILLS_DIR, &st) != 0) { s_nsk = 0; return; }
    // FAT does not always touch a folder's mtime when a file is copied in: also rescan every 30 s.
    const time_t now = time(NULL);
    if (s_nsk >= 0 && st.st_mtime == s_dir_mtime && now - s_scan_at < 30) return;
    s_dir_mtime = st.st_mtime; s_scan_at = now;
    s_nsk = 0;
    if (!s_sk && !(s_sk = calloc(SKILLS_MAX, sizeof *s_sk))) return;
    DIR *d = opendir(SKILLS_DIR);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d)) && s_nsk < SKILLS_MAX) {
        const size_t n = strlen(e->d_name);
        if (e->d_name[0] == '.') continue;
        const bool md = n >= 4 && !strcmp(e->d_name + n - 3, ".md");
        char rel[sizeof s_sk[0].file];
        if (md) {
            if (n >= sizeof rel) continue;
            snprintf(rel, sizeof rel, "%s", e->d_name);
        } else {                                          // an Agent Skills folder: <dir>/SKILL.md
            if (n + 10 >= sizeof rel) continue;
            snprintf(rel, sizeof rel, "%s/SKILL.md", e->d_name);
        }
        char path[160];
        snprintf(path, sizeof path, SKILLS_DIR "/%s", rel);
        FILE *f = fopen(path, "r");
        if (!f) continue;
        skill_t *k = &s_sk[s_nsk];
        memset(k, 0, sizeof *k);
        if (parse_head(f, k) > 0 && (k->trig[0] || k->desc[0])) {
            snprintf(k->file, sizeof k->file, "%s", rel);
            if (!k->name[0]) snprintf(k->name, sizeof k->name, "%.*s", (int)(md ? n - 3 : n), e->d_name);
            s_nsk++;
        }
        fclose(f);
    }
    closedir(d);
}

// Content words of a skill's description that appear in the question (whole words, 5+ letters, not
// stop words). Agent Skills have no trigger list: the description is what tells when to use them.
static int desc_hits(const skill_t *k, const char *ql)
{
    static const char *const STOP[] = { "about","after","their","there","these","those","which","while","with",
        "when","where","using","should","would","could","other","every","needs","users","tasks","files","based",
        "della","delle","dello","degli","quando","quello","questa","questo","come","sono","anche","oppure",
        "skill","skills","agent","agents","assistant","claude","model","user","help","helps", NULL };
    char d[sizeof k->desc];
    snprintf(d, sizeof d, "%s", k->desc);
    lower(d);
    int hits = 0;
    char *sv = NULL;
    for (char *w = strtok_r(d, " ,.;:()[]/\"'!?-", &sv); w; w = strtok_r(NULL, " ,.;:()[]/\"'!?-", &sv)) {
        if (strlen(w) < 5) continue;
        bool stop = false;
        for (int i = 0; STOP[i] && !stop; i++) stop = !strcmp(w, STOP[i]);
        if (stop) continue;
        if (strlen(w) >= 6) w[strlen(w) - 1] = 0;      // a crude stem: "ricetta" also finds "ricette"
        for (const char *h = strstr(ql, w); h; h = strstr(h + 1, w))
            if (h == ql || !isalnum((unsigned char)h[-1])) { hits++; break; }
    }
    return hits;
}

// How many trigger phrases of `k` occur in the lowercased question (word-bounded at the start).
// A skill without triggers scores on its description: two content words, or one for a short question.
static int score(const skill_t *k, const char *ql)
{
    if (!k->trig[0]) {
        const int h = desc_hits(k, ql);
        return h >= 2 || (h == 1 && strlen(ql) < 40) ? h : 0;
    }
    int sc = 0;
    char t[sizeof k->trig];
    snprintf(t, sizeof t, "%s", k->trig);
    char *sv = NULL;
    for (char *p = strtok_r(t, ",", &sv); p; p = strtok_r(NULL, ",", &sv)) {
        trim(p);
        if (!*p) continue;
        for (const char *h = strstr(ql, p); h; h = strstr(h + 1, p))
            if (h == ql || !isalnum((unsigned char)h[-1])) { sc++; break; }
    }
    return sc;
}

// Best matching skills, most triggers first. Returns how many (<= max).
static int match(const char *q, int *idx, int max)
{
    scan();
    if (s_nsk <= 0 || !q) return 0;
    char ql[256];
    snprintf(ql, sizeof ql, "%s", q);
    lower(ql);
    int best[SKILLS_MAX], n = 0;
    for (int i = 0; i < s_nsk; i++) { best[i] = score(&s_sk[i], ql); }
    while (n < max) {
        int bi = -1;
        for (int i = 0; i < s_nsk; i++) if (best[i] > 0 && (bi < 0 || best[i] > best[bi])) bi = i;
        if (bi < 0) break;
        idx[n++] = bi; best[bi] = 0;
    }
    return n;
}

int nucleo_anima_skills_prompt(const char *q, bool en, char *out, int cap)
{
    if (!out || cap < 64) return 0;
    out[0] = 0;
    int idx[SKILLS_ACTIVE];
    const int n = match(q, idx, SKILLS_ACTIVE);
    int len = 0;
    for (int i = 0; i < n && len < cap - 64; i++) {
        const skill_t *k = &s_sk[idx[i]];
        char path[160];
        snprintf(path, sizeof path, SKILLS_DIR "/%s", k->file);
        FILE *f = fopen(path, "r");
        if (!f) continue;
        skill_t tmp;
        memset(&tmp, 0, sizeof tmp);
        const long body = parse_head(f, &tmp);
        if (body > 0) {
            len += snprintf(out + len, cap - len, "%sSKILL \"%s\"%s%s:\n", len ? "\n\n" : (en ? "ACTIVE SKILLS (follow them for this request):\n" : "SKILL ATTIVE (seguile per questa richiesta):\n"),
                            k->name, k->desc[0] ? " - " : "", k->desc);
            if (len >= cap) len = cap - 1;
            int room = cap - len - 1;
            if (room > SKILL_BODY_MAX) room = SKILL_BODY_MAX;
            const size_t got = fread(out + len, 1, room > 0 ? (size_t)room : 0, f);
            len += (int)got;
            out[len] = 0;
            trim(out);
            len = (int)strlen(out);
        }
        fclose(f);
    }
    return len;
}

int nucleo_anima_skills_offline(const char *q, char *out, int cap)
{
    int idx[1];
    if (!out || cap < 2 || match(q, idx, 1) < 1 || !s_sk[idx[0]].offline[0]) return 0;
    snprintf(out, cap, "%s", s_sk[idx[0]].offline);
    return 1;
}

int nucleo_anima_skills_list(char *out, int cap)
{
    scan();
    int len = 0;
    if (!out || cap < 2) return 0;
    out[0] = 0;
    for (int i = 0; i < s_nsk && len < cap - 1; i++)
        len += snprintf(out + len, cap - len, "%s%s", i ? ", " : "", s_sk[i].name);
    return s_nsk > 0 ? s_nsk : 0;
}

// ---- the workspace (OpenClaw-style plain files the user edits, next to the skills) -----------------
//   /data/anima/SOUL.md          who ANIMA is: tone, values, limits      -> the model's system prompt
//   /data/anima/USER.md          who the user is: name, habits, prefs    -> the model's system prompt
//   /data/anima/MEMORY.md        what ANIMA learned (the model appends with ACT remember) -> prompt (tail)
//   /data/anima/HEARTBEAT.md     the proactive checklist (nucleo_anima_heartbeat)
//   /data/anima/permissions.json what a model may do on its own: {"create_file":"ask", ...}
#define WS_DIR NUCLEO_SD_MOUNT "/data/anima"
#define WS_FILE_MAX 1200

// Read up to cap-1 bytes of a workspace file, trimmed. Returns the length (0 = missing/empty).
static int ws_read(const char *name, char *out, int cap)
{
    char path[96];
    snprintf(path, sizeof path, WS_DIR "/%s", name);
    out[0] = 0;
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    const size_t n = fread(out, 1, (size_t)cap - 1, f);
    fclose(f);
    out[n] = 0;
    trim(out);
    return (int)strlen(out);
}

int nucleo_anima_workspace_prompt(bool en, char *out, int cap)
{
    if (!out || cap < 64) return 0;
    out[0] = 0;
    int len = 0;
    static const struct { const char *file, *it, *en; } W[] = {
        { "SOUL.md", "LA TUA PERSONALITÀ (SOUL.md, scritta dall'utente):", "YOUR PERSONALITY (SOUL.md, written by the user):" },
        { "USER.md", "CHI È L'UTENTE (USER.md, scritto dall'utente):",     "ABOUT THE USER (USER.md, written by the user):" },
    };
    char *buf = malloc(WS_FILE_MAX + 1);
    if (!buf) return 0;
    for (size_t i = 0; i < sizeof W / sizeof W[0]; i++) {
        if (ws_read(W[i].file, buf, WS_FILE_MAX + 1) <= 0) continue;
        const int w = snprintf(out + len, cap - len, "%s%s\n%s", len ? "\n\n" : "", en ? W[i].en : W[i].it, buf);
        if (w < 0 || w >= cap - len) { out[len] = 0; break; }
        len += w;
    }
    // MEMORY.md grows (ANIMA appends to it): the most recent part is what the model gets.
    char path[96];
    snprintf(path, sizeof path, WS_DIR "/MEMORY.md");
    FILE *f = fopen(path, "r");
    if (f) {
        fseek(f, 0, SEEK_END);
        const long sz = ftell(f);
        const long from = sz > WS_FILE_MAX ? sz - WS_FILE_MAX : 0;
        fseek(f, from, SEEK_SET);
        size_t n = fread(buf, 1, WS_FILE_MAX, f);
        fclose(f);
        buf[n] = 0;
        char *start = buf;
        if (from > 0) { char *nl = strchr(buf, '\n'); if (nl) start = nl + 1; }   // begin on a whole line
        trim(start);
        if (start[0]) {
            const int w = snprintf(out + len, cap - len, "%s%s\n%s", len ? "\n\n" : "",
                                   en ? "WHAT YOU REMEMBER (MEMORY.md, most recent):" : "COSA RICORDI (MEMORY.md, le più recenti):", start);
            if (w > 0 && w < cap - len) len += w; else out[len] = 0;
        }
    }
    free(buf);
    return len;
}

int nucleo_anima_memory_add(const char *fact)
{
    if (!fact) return 0;
    char line[240]; int n = 0;
    for (const char *p = fact; *p && n < (int)sizeof line - 1; p++) line[n++] = (*p == '\n' || *p == '\r') ? ' ' : *p;
    line[n] = 0;
    trim(line);
    if (strlen(line) < 3) return 0;
    char path[96];
    snprintf(path, sizeof path, WS_DIR "/MEMORY.md");
    mkdir(NUCLEO_SD_MOUNT "/data", 0777);
    mkdir(WS_DIR, 0777);
    struct stat st;
    const bool fresh = stat(path, &st) != 0;
    FILE *f = fopen(path, "a");
    if (!f) return 0;
    time_t t = time(NULL);
    struct tm tm;
    localtime_r(&t, &tm);
    if (fresh) fputs("# MEMORY.md - what ANIMA remembers (edit freely)\n\n", f);
    if (tm.tm_year + 1900 >= 2024) fprintf(f, "- %s (%04d-%02d-%02d)\n", line, tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday);
    else fprintf(f, "- %s\n", line);
    return fclose(f) == 0;
}

int nucleo_anima_heartbeat_list(char *out, int cap)
{
    if (!out || cap < 2) return 0;
    return ws_read("HEARTBEAT.md", out, cap);
}

// allow 0 / ask 1 / deny 2. Defaults: what is undone in a tap runs; what leaves something behind
// (an event, a file) asks first. permissions.json may also say "*": "ask" for everything.
int nucleo_anima_permission(const char *tool)
{
    int def = (!strcmp(tool, "add_event") || !strcmp(tool, "create_file") || !strcmp(tool, "sh") || !strcmp(tool, "write") ||
               !strcmp(tool, "rule")) ? 1 : 0;
    char buf[600];
    if (ws_read("permissions.json", buf, sizeof buf) <= 0) return def;
    cJSON *o = cJSON_Parse(buf);
    if (!o) return def;
    cJSON *v = cJSON_GetObjectItem(o, tool);
    if (!cJSON_IsString(v)) v = cJSON_GetObjectItem(o, "*");
    int r = def;
    if (cJSON_IsString(v)) r = !strcmp(v->valuestring, "allow") ? 0 : !strcmp(v->valuestring, "deny") ? 2 :
                               !strcmp(v->valuestring, "ask") ? 1 : def;
    // "mode": "auto" - the autonomous mode (Claude Code's skip-permissions): what would ask runs at
    // once; an explicit "deny" still holds.
    // "mode": "plan" - read-only (OpenCode's plan agent): anything that changes something is denied.
    cJSON *m = cJSON_GetObjectItem(o, "mode");
    if (r == 1 && cJSON_IsString(m) && !strcmp(m->valuestring, "auto")) r = 0;
    if (cJSON_IsString(m) && !strcmp(m->valuestring, "plan") && def) r = 2;
    cJSON_Delete(o);
    return r;
}

// The agent mode ("mode" in permissions.json): 0 normal, 1 auto (no asking), 2 plan (read-only),
// keeping every other entry.
int nucleo_anima_agent_mode(void)
{
    char buf[600];
    if (ws_read("permissions.json", buf, sizeof buf) <= 0) return 0;
    cJSON *o = cJSON_Parse(buf);
    cJSON *m = o ? cJSON_GetObjectItem(o, "mode") : NULL;
    const int r = !cJSON_IsString(m) ? 0 : !strcmp(m->valuestring, "auto") ? 1 : !strcmp(m->valuestring, "plan") ? 2 : 0;
    cJSON_Delete(o);
    return r;
}
bool nucleo_anima_auto_mode(void) { return nucleo_anima_agent_mode() == 1; }
bool nucleo_anima_set_auto_mode(bool on) { return nucleo_anima_set_agent_mode(on ? 1 : 0); }

bool nucleo_anima_set_agent_mode(int mode)
{
    char buf[600];
    cJSON *o = ws_read("permissions.json", buf, sizeof buf) > 0 ? cJSON_Parse(buf) : NULL;
    if (!o) o = cJSON_CreateObject();
    if (!o) return false;
    cJSON_DeleteItemFromObject(o, "mode");
    if (mode == 1 || mode == 2) cJSON_AddStringToObject(o, "mode", mode == 1 ? "auto" : "plan");
    char *txt = cJSON_Print(o);
    cJSON_Delete(o);
    if (!txt) return false;
    mkdir(NUCLEO_SD_MOUNT "/data", 0777);
    mkdir(WS_DIR, 0777);
    FILE *f = fopen(WS_DIR "/permissions.json", "w");
    const bool ok = f && fputs(txt, f) >= 0;
    if (f) fclose(f);
    cJSON_free(txt);
    return ok;
}

// The skill catalog for the agent (progressive disclosure): one line per skill, name, description
// and where its SKILL.md lives, so the model can read it (and its scripts/ references/) with the shell.
int nucleo_anima_skills_catalog(bool en, char *out, int cap)
{
    if (!out || cap < 80) return 0;
    out[0] = 0;
    scan();
    if (s_nsk <= 0) return 0;
    int len = snprintf(out, cap, "%s", en ? "SKILLS on the device (read one with ACT sh cat <path> when the task fits it):\n"
                                          : "SKILL sul dispositivo (leggine una con ACT sh cat <percorso> quando il compito le corrisponde):\n");
    for (int i = 0; i < s_nsk && len < cap - 40; i++) {
        const skill_t *k = &s_sk[i];
        len += snprintf(out + len, cap - len, "- %s: %.160s (" SKILLS_DIR "/%s)\n", k->name, k->desc[0] ? k->desc : k->trig, k->file);
    }
    if (len >= cap) len = cap - 1;
    out[len] = 0;
    return len;
}
