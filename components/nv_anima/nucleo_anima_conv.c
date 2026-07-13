// ANIMA conversations + user memory — the "mini Claude" persistence layer (see nucleo_anima_conv.h).
//
// Storage (all SD, all bounded):
//   conv/<id>.j  — append-only JSONL messages {"r":"u"|"a","ts":<unix>,"t":"<text ≤3000>"}
//   conv/<id>.m  — meta {"v":1,"title","created","updated","n","cut","sum"}; rewritten atomically
//   memory.jsonl — user facts {"ts":<unix>,"t":"<fact ≤240>"} (the device CLAUDE.md)
//
// Claude-style context discipline on a PSRAM-poor chip:
//   * a request never carries the whole history — only [memory + rolling summary] as an extra
//     system block plus the last ≤8 complete turns (each clipped), so RAM and tokens stay flat;
//   * COMPACTION folds the oldest un-summarized turns into meta.sum via one bounded teacher call
//     and advances meta.cut — the transcript file itself is never rewritten (append-only, honest);
//   * "ricordati che …" turns are captured BEFORE the LLM: the fact lands in memory.jsonl and the
//     confirmation is generated locally (works offline, costs zero tokens).
//
// THREADING: this module IS crossed by two tasks — the ANIMA worker (chat/compaction under the
// spine lock, and grok_chat's global memory injection can run on the NATIVE app task too) and the
// serial httpd task (conv/memory CRUD handlers take no spine lock). Every public entry point
// therefore takes the module's own RECURSIVE mutex; the shared s_line scan buffer below relies on it.
#include "nucleo_anima_conv.h"
#include "nucleo_anima_online.h"     // anima_turn_t, nucleo_anima_online_chat_conv, online_available
#include "nucleo_board.h"
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <stdlib.h>
#include <ctype.h>
#include <time.h>
#include <dirent.h>
#include <sys/stat.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "cJSON.h"

static const char *TAG = "anima.conv";

// Module mutex (recursive: conv_chat re-enters append/compact/ctx through their public faces).
// Lazy create is guarded by a spinlock so two first-callers can't both create it.
static SemaphoreHandle_t s_mtx;
static portMUX_TYPE s_mtx_mux = portMUX_INITIALIZER_UNLOCKED;
static void conv_lock(void)
{
    if (!s_mtx) {
        SemaphoreHandle_t m = xSemaphoreCreateRecursiveMutex();
        portENTER_CRITICAL(&s_mtx_mux);
        if (!s_mtx) { s_mtx = m; m = NULL; }
        portEXIT_CRITICAL(&s_mtx_mux);
        if (m) vSemaphoreDelete(m);          // lost the race: ours is redundant
    }
    xSemaphoreTakeRecursive(s_mtx, portMAX_DELAY);
}
static void conv_unlock(void) { xSemaphoreGiveRecursive(s_mtx); }

#define CONV_DIR   NUCLEO_SD_MOUNT "/data/anima/conv"
#define MEM_PATH   NUCLEO_SD_MOUNT "/data/anima/memory.jsonl"
#define SUM_CAP    900               // rolling summary chars kept in meta
#define CTX_PAIRS  8                 // recent complete turns sent to the provider
#define CTX_CLIP   600               // chars per message inside the request context
#define COMPACT_AT 20                // compact when this many un-summarized messages accumulated
#define COMPACT_TAKE 12              // messages folded into the summary per compaction
#define MEM_INJECT_CAP 1200          // chars of memory facts injected into the system block

// One shared line buffer for every JSONL scan (guarded by the module mutex above). SIZED FOR THE
// WORST-CASE RECORD, not the raw text: a 3000-char message where every byte needs \uXXXX escaping
// (cJSON escapes control bytes 6:1, quotes/backslashes 2:1) is ~18 KB on disk — a smaller buffer
// makes fgets split one record into unparseable fragments and silently desyncs meta.n/cut from the
// real line count. PSRAM: fgets doesn't need DMA-capable memory.
EXT_RAM_BSS_ATTR static char s_line[20480];

// ---- small helpers ----------------------------------------------------------

// JSON-escape `s` into dst (cap incl. NUL): quotes, backslash, \n; other control bytes -> \uXXXX
// (same semantics as nv_web's json_escape, so both firmware escapers behave identically).
// UTF-8 passes through raw (valid JSON). Returns chars written.
static int jesc(char *dst, int cap, const char *s)
{
    int o = 0;
    for (const unsigned char *p = (const unsigned char *)s; *p && o < cap - 8; p++) {
        if (*p == '"' || *p == '\\') { dst[o++] = '\\'; dst[o++] = (char)*p; }
        else if (*p == '\n') { dst[o++] = '\\'; dst[o++] = 'n'; }
        else if (*p < 0x20) { o += snprintf(dst + o, cap - o, "\\u%04x", *p); }
        else dst[o++] = (char)*p;
    }
    dst[o] = 0;
    return o;
}

static bool id_ok(const char *id)
{
    if (!id || !id[0]) return false;
    int n = 0;
    for (; id[n]; n++) { if (n >= NV_CONV_ID_CAP - 1) return false;
        char c = id[n]; if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'))) return false; }
    return n >= 2;
}

static void conv_paths(const char *id, char *jp, int jcap, char *mp, int mcap)
{
    snprintf(jp, jcap, CONV_DIR "/%s.j", id);
    snprintf(mp, mcap, CONV_DIR "/%s.m", id);
}

static void ensure_dirs(void)
{
    mkdir(NUCLEO_SD_MOUNT "/data", 0777);
    mkdir(NUCLEO_SD_MOUNT "/data/anima", 0777);
    mkdir(CONV_DIR, 0777);
}

// ---- meta -------------------------------------------------------------------

typedef struct {
    char title[64];
    long created, updated;
    int  n, cut;
    char sum[SUM_CAP + 4];
} conv_meta_t;

static bool meta_load(const char *id, conv_meta_t *m)
{
    memset(m, 0, sizeof *m);
    if (!id_ok(id)) return false;
    char jp[96], mp[96]; conv_paths(id, jp, sizeof jp, mp, sizeof mp);
    FILE *f = fopen(mp, "r");
    if (!f) return false;
    size_t n = fread(s_line, 1, sizeof s_line - 1, f); fclose(f); s_line[n] = 0;
    cJSON *o = cJSON_Parse(s_line);
    if (!o) return false;
    cJSON *t = cJSON_GetObjectItem(o, "title"), *s = cJSON_GetObjectItem(o, "sum");
    cJSON *c = cJSON_GetObjectItem(o, "created"), *u = cJSON_GetObjectItem(o, "updated");
    cJSON *nn = cJSON_GetObjectItem(o, "n"), *cu = cJSON_GetObjectItem(o, "cut");
    if (cJSON_IsString(t)) snprintf(m->title, sizeof m->title, "%s", t->valuestring);
    if (cJSON_IsString(s)) snprintf(m->sum, sizeof m->sum, "%s", s->valuestring);
    if (cJSON_IsNumber(c)) m->created = (long)c->valuedouble;
    if (cJSON_IsNumber(u)) m->updated = (long)u->valuedouble;
    if (cJSON_IsNumber(nn)) m->n = nn->valueint;
    if (cJSON_IsNumber(cu)) m->cut = cu->valueint;
    cJSON_Delete(o);
    if (m->cut < 0) m->cut = 0;
    if (m->cut > m->n) m->cut = m->n;
    return true;
}

static bool meta_save(const char *id, const conv_meta_t *m)
{
    char jp[96], mp[96]; conv_paths(id, jp, sizeof jp, mp, sizeof mp);
    cJSON *o = cJSON_CreateObject();
    cJSON_AddNumberToObject(o, "v", 1);
    cJSON_AddStringToObject(o, "title", m->title);
    cJSON_AddNumberToObject(o, "created", (double)m->created);
    cJSON_AddNumberToObject(o, "updated", (double)m->updated);
    cJSON_AddNumberToObject(o, "n", m->n);
    cJSON_AddNumberToObject(o, "cut", m->cut);
    cJSON_AddStringToObject(o, "sum", m->sum);
    char *s = cJSON_PrintUnformatted(o); cJSON_Delete(o);
    if (!s) return false;
    char tmp[104]; snprintf(tmp, sizeof tmp, "%s.tmp", mp);
    FILE *f = fopen(tmp, "w");
    if (!f) { free(s); return false; }
    fputs(s, f); fclose(f); free(s);
    remove(mp);                       // FatFs rename won't overwrite
    if (rename(tmp, mp) != 0) { remove(tmp); return false; }
    return true;
}

// ---- create / list / delete ---------------------------------------------------

// Directory scan callback shape: gather the lightweight list once for list/prune.
typedef struct { char id[NV_CONV_ID_CAP]; char title[64]; long updated; int n; } conv_lite_t;

static int scan_convs(conv_lite_t *arr, int max)
{
    DIR *d = opendir(CONV_DIR);
    if (!d) return 0;
    int cnt = 0;
    struct dirent *e;
    while ((e = readdir(d)) && cnt < max) {
        const char *dot = strrchr(e->d_name, '.');
        if (!dot || strcmp(dot, ".m") != 0) continue;
        char id[NV_CONV_ID_CAP]; int il = (int)(dot - e->d_name);
        if (il <= 0 || il >= NV_CONV_ID_CAP) continue;
        memcpy(id, e->d_name, il); id[il] = 0;
        conv_meta_t m;
        if (!meta_load(id, &m)) continue;
        snprintf(arr[cnt].id, sizeof arr[cnt].id, "%s", id);
        snprintf(arr[cnt].title, sizeof arr[cnt].title, "%s", m.title);
        arr[cnt].updated = m.updated; arr[cnt].n = m.n;
        cnt++;
    }
    closedir(d);
    // insertion sort, newest-updated first
    for (int i = 1; i < cnt; i++) {
        conv_lite_t t = arr[i]; int j = i - 1;
        while (j >= 0 && arr[j].updated < t.updated) { arr[j + 1] = arr[j]; j--; }
        arr[j + 1] = t;
    }
    return cnt;
}

static int conv_delete_impl(const char *id)
{
    if (!id_ok(id)) return -1;
    char jp[96], mp[96]; conv_paths(id, jp, sizeof jp, mp, sizeof mp);
    remove(jp); remove(mp);
    return 0;
}

static int conv_create_impl(char *id, int idcap, const char *title)
{
    if (!id || idcap < NV_CONV_ID_CAP) return -1;
    ensure_dirs();
    // prune GATE: a bare dirent count first (no file opens) — the full scan_convs pass (open+parse
    // of every meta) only runs when the store is actually at the cap, so "new chat" stays cheap.
    int metas = 0;
    DIR *d = opendir(CONV_DIR);
    if (d) { struct dirent *e; while ((e = readdir(d))) { const char *dot = strrchr(e->d_name, '.'); if (dot && !strcmp(dot, ".m")) metas++; } closedir(d); }
    if (metas >= NV_CONV_MAX) {
        conv_lite_t *lst = heap_caps_calloc(NV_CONV_MAX + 4, sizeof *lst, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (lst) {
            int cnt = scan_convs(lst, NV_CONV_MAX + 4);
            for (int i = cnt - 1; i >= 0 && cnt >= NV_CONV_MAX; i--, cnt--)   // list is newest-first
                conv_delete_impl(lst[i].id);
            free(lst);
        }
    }
    long now = (long)time(NULL);
    for (int i = 0; i < 26; i++) {                                        // same-second collision -> suffix
        if (i == 0) snprintf(id, idcap, "c%08lx", (unsigned long)now);
        else        snprintf(id, idcap, "c%08lx%c", (unsigned long)now, (char)('a' + i - 1));
        char jp[96], mp[96]; conv_paths(id, jp, sizeof jp, mp, sizeof mp);
        struct stat st;
        if (stat(mp, &st) == 0) continue;
        conv_meta_t m; memset(&m, 0, sizeof m);
        if (title && title[0]) snprintf(m.title, sizeof m.title, "%s", title);
        m.created = m.updated = now;
        if (!meta_save(id, &m)) return -1;
        FILE *f = fopen(jp, "w"); if (f) fclose(f);                       // empty transcript
        return 0;
    }
    return -1;
}

static int conv_list_json_impl(char *out, int cap)
{
    if (!out || cap < 16) return -1;
    ensure_dirs();
    conv_lite_t *lst = heap_caps_calloc(NV_CONV_MAX + 4, sizeof *lst, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!lst) return -1;
    int cnt = scan_convs(lst, NV_CONV_MAX + 4);
    int o = snprintf(out, cap, "{\"convs\":[");
    for (int i = 0; i < cnt && o < cap - 8; i++) {
        char et[136]; jesc(et, sizeof et, lst[i].title);
        o += snprintf(out + o, cap - o, "%s{\"id\":\"%s\",\"title\":\"%s\",\"updated\":%ld,\"n\":%d}",
                      i ? "," : "", lst[i].id, et, lst[i].updated, lst[i].n);
    }
    free(lst);
    if (o >= cap - 3) { out[cap - 1] = 0; return -1; }                    // clipped mid-array: refuse
    o += snprintf(out + o, cap - o, "]}");
    return o;
}

static int conv_set_title_impl(const char *id, const char *title)
{
    conv_meta_t m;
    if (!meta_load(id, &m)) return -1;
    snprintf(m.title, sizeof m.title, "%s", title ? title : "");
    m.updated = (long)time(NULL);
    return meta_save(id, &m) ? 0 : -1;
}

// ---- messages ----------------------------------------------------------------

static int conv_append_impl(const char *id, char role, const char *text)
{
    if (!text || (role != 'u' && role != 'a')) return -1;
    conv_meta_t m;
    if (!meta_load(id, &m)) return -1;
    char jp[96], mp[96]; conv_paths(id, jp, sizeof jp, mp, sizeof mp);

    char clip[NV_CONV_MSG_CAP + 4];
    snprintf(clip, sizeof clip, "%.*s", NV_CONV_MSG_CAP, text);
    cJSON *o = cJSON_CreateObject();
    char r[2] = { role, 0 };
    cJSON_AddStringToObject(o, "r", r);
    cJSON_AddNumberToObject(o, "ts", (double)time(NULL));
    cJSON_AddStringToObject(o, "t", clip);
    char *line = cJSON_PrintUnformatted(o); cJSON_Delete(o);
    if (!line) return -1;
    FILE *f = fopen(jp, "a");
    if (!f) { free(line); return -1; }
    fputs(line, f); fputc('\n', f); fclose(f); free(line);

    m.n++;
    m.updated = (long)time(NULL);
    if (!m.title[0] && role == 'u') {                                     // auto-title: first user msg
        int o2 = 0;
        for (const char *p = clip; *p && o2 < 44; p++) m.title[o2++] = (*p == '\n' || *p == '\r') ? ' ' : *p;
        m.title[o2] = 0;
    }
    return meta_save(id, &m) ? 0 : -1;
}

static int conv_msgs_json_impl(const char *id, int tail, char **out_heap)
{
    if (!out_heap) return -1;
    *out_heap = NULL;
    conv_meta_t m;
    if (!meta_load(id, &m)) return -1;
    if (tail <= 0 || tail > 60) tail = 60;
    char jp[96], mp[96]; conv_paths(id, jp, sizeof jp, mp, sizeof mp);
    FILE *f = fopen(jp, "r");
    if (!f) return -1;
    // meta.n is maintained on every append — no counting pass over a potentially multi-100KB
    // transcript; worst case (an append whose meta_save failed) the window is off by one line.
    int total = m.n, skip = total > tail ? total - tail : 0;

    size_t cap = 256 + 160 + SUM_CAP * 2 + (size_t)(tail) * (NV_CONV_MSG_CAP * 2 + 64);
    char *out = heap_caps_malloc(cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!out) { fclose(f); return -1; }
    char et[136], es[SUM_CAP * 2 + 8];
    jesc(et, sizeof et, m.title); jesc(es, sizeof es, m.sum);
    size_t o = snprintf(out, cap, "{\"id\":\"%s\",\"title\":\"%s\",\"n\":%d,\"cut\":%d,\"sum\":\"%s\",\"msgs\":[",
                        id, et, m.n, m.cut, es);
    rewind(f);
    int idx = 0, emitted = 0;
    while (fgets(s_line, sizeof s_line, f)) {
        if (idx++ < skip) continue;
        cJSON *mo = cJSON_Parse(s_line);
        if (!mo) continue;
        cJSON *r = cJSON_GetObjectItem(mo, "r"), *ts = cJSON_GetObjectItem(mo, "ts"), *t = cJSON_GetObjectItem(mo, "t");
        if (cJSON_IsString(r) && cJSON_IsString(t) && o + NV_CONV_MSG_CAP * 2 + 96 < cap) {
            char *etx = heap_caps_malloc(NV_CONV_MSG_CAP * 2 + 8, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            if (etx) {
                jesc(etx, NV_CONV_MSG_CAP * 2 + 8, t->valuestring);
                o += snprintf(out + o, cap - o, "%s{\"r\":\"%s\",\"ts\":%ld,\"t\":\"%s\"}",
                              emitted ? "," : "", r->valuestring,
                              cJSON_IsNumber(ts) ? (long)ts->valuedouble : 0L, etx);
                emitted++;
                free(etx);
            }
        }
        cJSON_Delete(mo);
    }
    fclose(f);
    o += snprintf(out + o, cap - o, "]}");
    *out_heap = out;
    return (int)o;
}

// Recent complete u→a pairs after meta.cut, for the provider request. Fills up to `max` turns
// (oldest→newest), each side clipped to CTX_CLIP chars inside one heap blob (*blob, caller frees).
static int conv_tail_turns(const char *id, const conv_meta_t *m, anima_turn_t *turns, int max, char **blob)
{
    *blob = NULL;
    char jp[96], mp[96]; conv_paths(id, jp, sizeof jp, mp, sizeof mp);
    FILE *f = fopen(jp, "r");
    if (!f) return 0;
    // ring of the last (2*max+1) post-cut messages, each clipped
    const int slots = max * 2 + 1, ssz = CTX_CLIP + 4;
    char *ring = heap_caps_calloc((size_t)slots, ssz + 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!ring) { fclose(f); return 0; }
    char *roles = ring + (size_t)slots * ssz;                              // one role byte per slot (tail of the same alloc)
    int idx = 0, kept = 0;
    int msgno = 0;
    while (fgets(s_line, sizeof s_line, f)) {
        if (msgno++ < m->cut) continue;
        cJSON *mo = cJSON_Parse(s_line);
        if (!mo) continue;
        cJSON *r = cJSON_GetObjectItem(mo, "r"), *t = cJSON_GetObjectItem(mo, "t");
        if (cJSON_IsString(r) && cJSON_IsString(t) && t->valuestring[0]) {
            char *slot = ring + (size_t)(idx % slots) * ssz;
            snprintf(slot, ssz, "%.*s", CTX_CLIP, t->valuestring);
            roles[idx % slots] = r->valuestring[0];
            idx++; if (kept < slots) kept++;
        }
        cJSON_Delete(mo);
    }
    fclose(f);
    // walk the ring in order, pairing u→a
    int nt = 0;
    const char *pend_q = NULL;
    for (int k = 0; k < kept; k++) {
        int s = (idx - kept + k) % slots; if (s < 0) s += slots;
        char *txt = ring + (size_t)s * ssz;
        if (roles[s] == 'u') pend_q = txt;
        else if (roles[s] == 'a' && pend_q) {
            if (nt == max) {                                               // keep the newest `max` pairs
                memmove(&turns[0], &turns[1], (size_t)(max - 1) * sizeof turns[0]);
                nt = max - 1;
            }
            turns[nt].q = pend_q; turns[nt].a = txt; nt++;
            pend_q = NULL;
        }
    }
    if (nt == 0) { free(ring); return 0; }
    *blob = ring;
    return nt;
}

// ---- user memory ---------------------------------------------------------------

static int mem_add_impl(const char *fact)
{
    if (!fact || !fact[0]) return -1;
    ensure_dirs();
    char clip[NV_MEM_FACT_CAP + 4];
    snprintf(clip, sizeof clip, "%.*s", NV_MEM_FACT_CAP, fact);
    cJSON *o = cJSON_CreateObject();
    cJSON_AddNumberToObject(o, "ts", (double)time(NULL));
    cJSON_AddStringToObject(o, "t", clip);
    char *line = cJSON_PrintUnformatted(o); cJSON_Delete(o);
    if (!line) return -1;
    FILE *f = fopen(MEM_PATH, "a");
    if (!f) { free(line); return -1; }
    fputs(line, f); fputc('\n', f); fclose(f); free(line);
    // bound: drop the oldest beyond NV_MEM_MAX (rewrite, atomic)
    f = fopen(MEM_PATH, "r");
    if (!f) return 0;
    int total = 0;
    while (fgets(s_line, sizeof s_line, f)) total++;
    if (total <= NV_MEM_MAX) { fclose(f); return 0; }
    rewind(f);
    char tmp[96]; snprintf(tmp, sizeof tmp, MEM_PATH ".tmp");
    FILE *out = fopen(tmp, "w");
    if (!out) { fclose(f); return 0; }
    int skip = total - NV_MEM_MAX;
    while (fgets(s_line, sizeof s_line, f)) { if (skip > 0) { skip--; continue; } fputs(s_line, out); }
    fclose(f); fclose(out);
    remove(MEM_PATH);
    if (rename(tmp, MEM_PATH) != 0) remove(tmp);
    return 0;
}

static int mem_del_impl(long ts)
{
    FILE *f = fopen(MEM_PATH, "r");
    if (!f) return -1;
    char tmp[96]; snprintf(tmp, sizeof tmp, MEM_PATH ".tmp");
    FILE *out = fopen(tmp, "w");
    if (!out) { fclose(f); return -1; }
    bool found = false;
    while (fgets(s_line, sizeof s_line, f)) {
        cJSON *o = cJSON_Parse(s_line);
        long lts = 0;
        if (o) { cJSON *j = cJSON_GetObjectItem(o, "ts"); if (cJSON_IsNumber(j)) lts = (long)j->valuedouble; cJSON_Delete(o); }
        if (lts == ts && !found) { found = true; continue; }
        fputs(s_line, out);
    }
    fclose(f); fclose(out);
    if (!found) { remove(tmp); return -1; }
    remove(MEM_PATH);
    if (rename(tmp, MEM_PATH) != 0) { remove(tmp); return -1; }
    return 0;
}

static int mem_list_json_impl(char *out, int cap)
{
    if (!out || cap < 16) return -1;
    int o = snprintf(out, cap, "{\"facts\":[");
    FILE *f = fopen(MEM_PATH, "r");
    int emitted = 0;
    if (f) {
        while (fgets(s_line, sizeof s_line, f) && o < cap - (NV_MEM_FACT_CAP * 2 + 48)) {
            cJSON *mo = cJSON_Parse(s_line);
            if (!mo) continue;
            cJSON *ts = cJSON_GetObjectItem(mo, "ts"), *t = cJSON_GetObjectItem(mo, "t");
            if (cJSON_IsString(t)) {
                char et[NV_MEM_FACT_CAP * 2 + 8]; jesc(et, sizeof et, t->valuestring);
                o += snprintf(out + o, cap - o, "%s{\"ts\":%ld,\"t\":\"%s\"}",
                              emitted ? "," : "", cJSON_IsNumber(ts) ? (long)ts->valuedouble : 0L, et);
                emitted++;
            }
            cJSON_Delete(mo);
        }
        fclose(f);
    }
    o += snprintf(out + o, cap - o, "]}");
    return o;
}

static int mem_block_impl(char *out, int cap, bool en)
{
    if (!out || cap < 80) return -1;
    out[0] = 0;
    FILE *f = fopen(MEM_PATH, "r");
    if (!f) return 0;
    // collect fact texts (bounded) so we can budget newest-first but emit chronological
    char (*facts)[NV_MEM_FACT_CAP + 4] = heap_caps_calloc(NV_MEM_MAX, NV_MEM_FACT_CAP + 4,
                                                          MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!facts) { fclose(f); return 0; }
    int nf = 0;
    while (fgets(s_line, sizeof s_line, f) && nf < NV_MEM_MAX) {
        cJSON *mo = cJSON_Parse(s_line);
        if (!mo) continue;
        cJSON *t = cJSON_GetObjectItem(mo, "t");
        if (cJSON_IsString(t) && t->valuestring[0]) snprintf(facts[nf++], NV_MEM_FACT_CAP + 4, "%s", t->valuestring);
        cJSON_Delete(mo);
    }
    fclose(f);
    if (nf == 0) { free(facts); return 0; }
    // newest-first budget: find the first (oldest) fact that still fits
    int start = nf, budget = MEM_INJECT_CAP;
    for (int i = nf - 1; i >= 0; i--) {
        int need = (int)strlen(facts[i]) + 3;
        if (budget - need < 0) break;
        budget -= need; start = i;
    }
    int o = snprintf(out, cap, "%s", en
        ? "USER MEMORY (facts the user asked you to remember — use them, don't recite them unprompted):\n"
        : "MEMORIA UTENTE (fatti che l'utente ti ha chiesto di ricordare — usali, non recitarli senza motivo):\n");
    for (int i = start; i < nf && o < cap - 8; i++)
        o += snprintf(out + o, cap - o, "- %s\n", facts[i]);
    free(facts);
    return o;
}

// lowercase ASCII copy (UTF-8 bytes pass through; byte length preserved so offsets map back)
static void lower_ascii(char *dst, int cap, const char *src)
{
    int i = 0;
    for (; src[i] && i < cap - 1; i++) dst[i] = (char)tolower((unsigned char)src[i]);
    dst[i] = 0;
}

static bool mem_capture_impl(const char *input, bool en, char *reply, int rcap)
{
    if (!input || !reply || rcap < 32) return false;
    char low[520]; lower_ascii(low, sizeof low, input);
    const char *p = low; while (*p == ' ') p++;
    int lead = (int)(p - low);

    // "what do you remember about me" — answer locally from the store
    static const char *ASK[] = { "cosa ricordi", "che cosa ricordi", "cosa sai di me", "che sai di me",
                                 "what do you remember", "what do you know about me", NULL };
    for (int i = 0; ASK[i]; i++) {
        if (!strncmp(p, ASK[i], strlen(ASK[i]))) {
            char blk[1400];
            int n = nucleo_anima_mem_block(blk, sizeof blk, en);
            if (n > 0) {
                const char *nl = strchr(blk, '\n');                         // drop the header line
                snprintf(reply, rcap, "%s", nl ? nl + 1 : blk);
            } else snprintf(reply, rcap, "%s", en ? "I don't have any saved memories yet. Tell me \"remember that …\"."
                                                  : "Non ho ancora memorie salvate. Dimmi \"ricordati che …\".");
            return true;
        }
    }

    // "ricordati che X / remember that X" — store X
    static const char *PRE[] = { "ricordati che ", "ricorda che ", "ricordati di ", "ricordati: ", "ricorda: ",
                                 "memorizza che ", "memorizza: ",
                                 "remember that ", "remember to ", "remember: ", NULL };
    for (int i = 0; PRE[i]; i++) {
        size_t pl = strlen(PRE[i]);
        if (strncmp(p, PRE[i], pl) != 0) continue;
        const char *fact = input + lead + (int)pl;                          // original-case text
        while (*fact == ' ') fact++;
        char clean[NV_MEM_FACT_CAP + 4];
        snprintf(clean, sizeof clean, "%.*s", NV_MEM_FACT_CAP, fact);
        int n = (int)strlen(clean);
        while (n > 0 && (clean[n-1] == ' ' || clean[n-1] == '.' || clean[n-1] == '!' )) clean[--n] = 0;
        if (n < 2) return false;
        if (nucleo_anima_mem_add(clean) != 0) {
            snprintf(reply, rcap, "%s", en ? "I couldn't save that (SD problem)." : "Non sono riuscita a salvarlo (problema SD).");
            return true;
        }
        snprintf(reply, rcap, en ? "Saved to memory: \"%s\"" : "Memorizzato: \"%s\"", clean);
        return true;
    }
    return false;
}

// ---- context block + compaction -------------------------------------------------

static int conv_ctx_block_impl(const char *id, bool en, char *out, int cap)
{
    if (!out || cap < 128) return -1;
    out[0] = 0;
    conv_meta_t m; bool have_meta = meta_load(id, &m);
    char mem[1400];
    int ml = nucleo_anima_mem_block(mem, sizeof mem, en);
    int o = 0;
    if (ml > 0) o += snprintf(out + o, cap - o, "%s", mem);
    if (have_meta && m.sum[0] && o < cap - 64)
        o += snprintf(out + o, cap - o, "%s%s\n%s\n",
                      o ? "\n" : "",
                      en ? "SUMMARY OF THE CONVERSATION SO FAR (older turns, already handled):"
                         : "RIASSUNTO DELLA CONVERSAZIONE FINORA (turni più vecchi, già gestiti):",
                      m.sum);
    return o;
}

// PHASED locking: the SD reads (meta + transcript into `text`) happen under the module mutex; the
// teacher round-trip (seconds) runs UNLOCKED so other surfaces aren't stalled; the meta update
// re-locks and RE-LOADS meta (a concurrent append may have bumped n/updated meanwhile) before
// stamping the new sum/cut.
static int conv_compact_impl(const char *id, bool en)
{
    conv_lock();
    conv_meta_t m;
    if (!meta_load(id, &m)) { conv_unlock(); return -1; }
    if (m.n - m.cut < COMPACT_AT || !nucleo_anima_online_available()) { conv_unlock(); return 0; }

    // gather COMPACT_TAKE messages from cut, clipped hard (token budget, not fidelity)
    char jp[96], mp[96]; conv_paths(id, jp, sizeof jp, mp, sizeof mp);
    FILE *f = fopen(jp, "r");
    if (!f) { conv_unlock(); return -1; }
    const size_t tcap = (size_t)COMPACT_TAKE * (400 + 16) + SUM_CAP + 128;
    char *text = heap_caps_malloc(tcap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!text) { fclose(f); conv_unlock(); return -1; }
    size_t o = 0;
    if (m.sum[0]) o += snprintf(text + o, tcap - o, "%s\n%s\n\n%s\n",
                                en ? "PREVIOUS SUMMARY:" : "RIASSUNTO PRECEDENTE:", m.sum,
                                en ? "NEW TURNS:" : "NUOVI SCAMBI:");
    int msgno = 0, taken = 0;
    while (fgets(s_line, sizeof s_line, f) && taken < COMPACT_TAKE) {
        if (msgno++ < m.cut) continue;
        cJSON *mo = cJSON_Parse(s_line);
        if (!mo) continue;
        cJSON *r = cJSON_GetObjectItem(mo, "r"), *t = cJSON_GetObjectItem(mo, "t");
        if (cJSON_IsString(r) && cJSON_IsString(t) && o + 440 < tcap) {
            o += snprintf(text + o, tcap - o, "%s: %.400s\n",
                          r->valuestring[0] == 'u' ? (en ? "User" : "Utente") : (en ? "Assistant" : "Assistente"),
                          t->valuestring);
            taken++;
        }
        cJSON_Delete(mo);
    }
    fclose(f);
    conv_unlock();
    if (taken == 0) { free(text); return 0; }

    const char *sys = en
        ? "Compress this dialog into ONE conversation summary, max 700 characters, in English: established facts, decisions, user preferences, open questions. Output ONLY the summary."
        : "Comprimi questo dialogo in UN riassunto di conversazione, max 700 caratteri, in italiano: fatti stabiliti, decisioni, preferenze dell'utente, questioni aperte. Restituisci SOLO il riassunto.";
    char newsum[SUM_CAP + 4];
    int rl = nucleo_anima_teacher_complete(sys, text, newsum, sizeof newsum);   // UNLOCKED: network call
    free(text);
    if (rl <= 0) return -1;                                  // teacher failed; context stays bounded anyway

    conv_lock();
    conv_meta_t m2;                                          // fresh: appends may have landed meanwhile
    if (!meta_load(id, &m2)) { conv_unlock(); return -1; }
    snprintf(m2.sum, sizeof m2.sum, "%.*s", SUM_CAP, newsum);
    m2.cut = m.cut + taken;                                  // advance from the cut we summarized
    if (m2.cut > m2.n) m2.cut = m2.n;
    meta_save(id, &m2);
    conv_unlock();
    ESP_LOGI(TAG, "compacted %s: cut %d/%d, sum %d chars", id, m2.cut, m2.n, (int)strlen(m2.sum));
    return 1;
}

// ---- the conversational turn ----------------------------------------------------

// PHASED locking (see conv_compact_impl): SD phases hold the module mutex, the provider round-trip
// runs unlocked so the native app's memory injection and web CRUD never stall behind a slow network.
static int conv_chat_impl(const char *id_in, const char *input, bool en,
                          anima_result_t *out, char *id_out, int idcap)
{
    if (!input || !input[0] || !out) return -1;
    memset(out, 0, sizeof *out);
    char id[NV_CONV_ID_CAP];
    conv_meta_t m;

    conv_lock();                                             // phase 1: resolve conv + local capture
    if (id_in && id_ok(id_in) && meta_load(id_in, &m)) snprintf(id, sizeof id, "%s", id_in);
    else if (conv_create_impl(id, sizeof id, NULL) != 0) { conv_unlock(); return -2; }
    if (id_out && idcap > 0) snprintf(id_out, idcap, "%s", id);

    nucleo_anima_set_long_reply(NULL);                       // we bypass nucleo_anima_query: clear the stale tail

    // "ricordati che …" — handled locally, zero tokens, works offline
    if (mem_capture_impl(input, en, out->reply, sizeof out->reply)) {
        out->tier = ANIMA_TIER_FACT; out->action = ANIMA_ACT_ANSWER;
        snprintf(out->intent, sizeof out->intent, "memory");
        out->confidence = 95;
        conv_append_impl(id, 'u', input);
        conv_append_impl(id, 'a', out->reply);
        conv_unlock();
        return 1;
    }
    conv_unlock();

    nucleo_anima_conv_compact(id, en);                       // rolling summary when due (phased-locked inside)

    anima_turn_t turns[CTX_PAIRS];
    char *blob = NULL;
    char ctx[2600];
    conv_lock();                                             // phase 2: build the request context
    meta_load(id, &m);                                       // fresh cut after a possible compaction
    int nt = conv_tail_turns(id, &m, turns, CTX_PAIRS, &blob);
    int cl = conv_ctx_block_impl(id, en, ctx, sizeof ctx);
    conv_unlock();

    int rc = nucleo_anima_online_chat_conv(input, nt ? turns : NULL, nt, cl > 0 ? ctx : NULL, en, out);
    free(blob);                                              // UNLOCKED: network call above
    if (rc <= 0) return 0;                                   // offline / no key -> honest miss

    conv_lock();                                             // phase 3: persist the turn
    conv_append_impl(id, 'u', input);
    const char *full = nucleo_anima_long_reply();            // prefer the untruncated tail for the transcript
    conv_append_impl(id, 'a', (full && full[0]) ? full : out->reply);
    conv_unlock();
    return 1;
}


// ---- public faces: every entry point serializes on the module mutex ------------------------------
// (recursive, so conv_chat's internal re-entry through these same faces is safe; see header comment)
int nucleo_anima_conv_create(char *id, int idcap, const char *title)
{ conv_lock(); int r = conv_create_impl(id, idcap, title); conv_unlock(); return r; }
int nucleo_anima_conv_list_json(char *out, int cap)
{ conv_lock(); int r = conv_list_json_impl(out, cap); conv_unlock(); return r; }
int nucleo_anima_conv_append(const char *id, char role, const char *text)
{ conv_lock(); int r = conv_append_impl(id, role, text); conv_unlock(); return r; }
int nucleo_anima_conv_msgs_json(const char *id, int tail, char **out_heap)
{ conv_lock(); int r = conv_msgs_json_impl(id, tail, out_heap); conv_unlock(); return r; }
int nucleo_anima_conv_delete(const char *id)
{ conv_lock(); int r = conv_delete_impl(id); conv_unlock(); return r; }
int nucleo_anima_conv_set_title(const char *id, const char *title)
{ conv_lock(); int r = conv_set_title_impl(id, title); conv_unlock(); return r; }
int nucleo_anima_conv_ctx_block(const char *id, bool en, char *out, int cap)
{ conv_lock(); int r = conv_ctx_block_impl(id, en, out, cap); conv_unlock(); return r; }
// compact and chat lock IN PHASES internally: their LLM round-trips (teacher/provider, seconds to
// minutes) must not hold the module mutex, or a concurrent surface (native app's memory injection,
// web CRUD) would stall on portMAX_DELAY for the whole network call.
int nucleo_anima_conv_compact(const char *id, bool en)
{ return conv_compact_impl(id, en); }
int nucleo_anima_conv_chat(const char *id_in, const char *input, bool en,
                           anima_result_t *out, char *id_out, int idcap)
{ return conv_chat_impl(id_in, input, en, out, id_out, idcap); }
int nucleo_anima_mem_add(const char *fact)
{ conv_lock(); int r = mem_add_impl(fact); conv_unlock(); return r; }
int nucleo_anima_mem_del(long ts)
{ conv_lock(); int r = mem_del_impl(ts); conv_unlock(); return r; }
int nucleo_anima_mem_list_json(char *out, int cap)
{ conv_lock(); int r = mem_list_json_impl(out, cap); conv_unlock(); return r; }
int nucleo_anima_mem_block(char *out, int cap, bool en)
{ conv_lock(); int r = mem_block_impl(out, cap, en); conv_unlock(); return r; }
bool nucleo_anima_mem_capture(const char *input, bool en, char *reply, int rcap)
{ conv_lock(); bool r = mem_capture_impl(input, en, reply, rcap); conv_unlock(); return r; }
