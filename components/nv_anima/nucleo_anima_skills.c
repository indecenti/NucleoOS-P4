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
#include "nucleo_anima.h"
#include "nucleo_board.h"
#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#define SKILLS_DIR     NUCLEO_SD_MOUNT "/data/anima/skills"
#define SKILLS_MAX     32
#define SKILL_BODY_MAX 1200
#define SKILLS_ACTIVE  2

typedef struct {
    char file[48];
    char name[32];
    char desc[96];
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
    while (fgets(line, sizeof line, f)) {
        if (!strncmp(line, "---", 3)) return ftell(f);
        char *c = strchr(line, ':');
        if (!c) continue;
        *c = 0;
        char *v = c + 1;
        trim(line); trim(v);
        lower(line);
        if (!strcmp(line, "name"))             snprintf(k->name, sizeof k->name, "%s", v);
        else if (!strcmp(line, "description")) snprintf(k->desc, sizeof k->desc, "%s", v);
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
        if (n < 4 || n >= sizeof s_sk[0].file || strcmp(e->d_name + n - 3, ".md")) continue;
        char path[128];
        snprintf(path, sizeof path, SKILLS_DIR "/%s", e->d_name);
        FILE *f = fopen(path, "r");
        if (!f) continue;
        skill_t *k = &s_sk[s_nsk];
        memset(k, 0, sizeof *k);
        if (parse_head(f, k) > 0 && k->trig[0]) {
            snprintf(k->file, sizeof k->file, "%s", e->d_name);
            if (!k->name[0]) snprintf(k->name, sizeof k->name, "%.*s", (int)(n - 3), e->d_name);
            s_nsk++;
        }
        fclose(f);
    }
    closedir(d);
}

// How many trigger phrases of `k` occur in the lowercased question (word-bounded at the start).
static int score(const skill_t *k, const char *ql)
{
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
        char path[128];
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
