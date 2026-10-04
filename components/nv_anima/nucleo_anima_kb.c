// ANIMA knowledge packs (AKB6) — see nucleo_anima_kb.h and docs/ANIMA_KB.md.
#include "nucleo_anima_kb.h"
#include "nucleo_anima_lex.h"     // anima_dict_tokenize: the one normalizer every table uses
#include "anima_lang.h"           // anima_lang_fold: á ñ ü ß ... -> ASCII, as the pack builder folds keys
#include "nucleo_board.h"         // NUCLEO_SD_MOUNT
#include "miniz.h"                // tinfl: the ESP32 ROM on the device, vendored miniz on the host
#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define KB_DIR      NUCLEO_SD_MOUNT "/data/anima/kb"
#define KB_MAXPACKS 8
#define KB_LINE     1024
#define KB_BLOCKMAX (256u * 1024u)   // inflated block cap (the builder writes ~32 KB): a corrupt pack is refused

typedef struct {
    char     path[128];
    char     lang[4];
    char     attribution[112];
    uint64_t keys_off, keys_end, ents_off, bidx_off, blks_end, qids_off, qids_end;
    uint32_t n_ent, n_blk;
} kb_pack_t;

static kb_pack_t s_pack[KB_MAXPACKS];
static int s_npack = -1;                     // -1 = not scanned yet

static uint32_t rd32(const uint8_t *b) { return b[0] | (b[1] << 8) | (b[2] << 16) | ((uint32_t)b[3] << 24); }
static uint64_t rd64(const uint8_t *b) { return rd32(b) | ((uint64_t)rd32(b + 4) << 32); }

// Header + section table; META only for the attribution. False = not a usable AKB6 file.
static bool kb_open_pack(const char *path, kb_pack_t *p)
{
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    uint8_t h[64];
    bool ok = fread(h, 1, 64, f) == 64 && !memcmp(h, "AKB6", 4) && (h[4] | (h[5] << 8)) == 1;
    if (ok) {
        memset(p, 0, sizeof *p);
        snprintf(p->path, sizeof p->path, "%s", path);
        memcpy(p->lang, h + 8, 3); p->lang[3] = 0;
        p->n_ent = rd32(h + 12); p->n_blk = rd32(h + 20);
        const uint32_t nsec = rd32(h + 24);
        uint64_t meta_off = 0, meta_sz = 0, blks_off = 0, blks_sz = 0;
        for (uint32_t i = 0; ok && i < nsec && i < 16; i++) {
            uint8_t s[20];
            if (fread(s, 1, 20, f) != 20) { ok = false; break; }
            const uint64_t off = rd64(s + 4), sz = rd64(s + 12);
            if (!memcmp(s, "META", 4)) { meta_off = off; meta_sz = sz; }
            else if (!memcmp(s, "KEYS", 4)) { p->keys_off = off; p->keys_end = off + sz; }
            else if (!memcmp(s, "ENTS", 4)) p->ents_off = off;
            else if (!memcmp(s, "QIDS", 4)) { p->qids_off = off; p->qids_end = off + sz; }
            else if (!memcmp(s, "BIDX", 4)) p->bidx_off = off;
            else if (!memcmp(s, "BLKS", 4)) { blks_off = off; blks_sz = sz; }
        }
        p->blks_end = blks_off + blks_sz;
        ok = ok && p->keys_off && p->ents_off && p->bidx_off && blks_off && p->n_ent;
        if (ok && meta_sz && meta_sz < 4096) {                    // "attribution": "..."
            char *m = (char *)malloc(meta_sz + 1);
            if (m && fseek(f, (long)meta_off, SEEK_SET) == 0 && fread(m, 1, meta_sz, f) == meta_sz) {
                m[meta_sz] = 0;
                const char *a = strstr(m, "\"attribution\": \"");
                if (!a) a = strstr(m, "\"attribution\":\"");
                if (a) {
                    a = strchr(a + 14, '"') + 1;
                    const char *e = strchr(a, '"');
                    if (e) snprintf(p->attribution, sizeof p->attribution, "%.*s", (int)(e - a), a);
                }
            }
            free(m);
        }
        if (!p->attribution[0]) snprintf(p->attribution, sizeof p->attribution, "Wikipedia, CC BY-SA 4.0");
    }
    fclose(f);
    return ok;
}

void nucleo_anima_kb_rescan(void)
{
    s_npack = 0;
    DIR *d = opendir(KB_DIR);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d)) && s_npack < KB_MAXPACKS) {
        const size_t n = strlen(e->d_name);
        if (n < 6 || strcmp(e->d_name + n - 5, ".akb6")) continue;
        char path[128];
        snprintf(path, sizeof path, KB_DIR "/%s", e->d_name);
        if (kb_open_pack(path, &s_pack[s_npack])) s_npack++;
    }
    closedir(d);
}

int nucleo_anima_kb_pack_count(void) { if (s_npack < 0) nucleo_anima_kb_rescan(); return s_npack; }
void nucleo_anima_kb_invalidate(void) { s_npack = -1; }
const char *nucleo_anima_kb_pack_lang(int i) { return (i >= 0 && i < s_npack) ? s_pack[i].lang : ""; }
const char *nucleo_anima_kb_pack_attribution(int i) { return (i >= 0 && i < s_npack) ? s_pack[i].attribution : ""; }

// ---- topic --------------------------------------------------------------------------------------

static bool tok_in(const char *w, const char *const *l) { for (int i = 0; l[i]; i++) if (!strcmp(w, l[i])) return true; return false; }

bool nucleo_anima_kb_topic(const char *q, bool bare_ok, char *key, size_t cap)
{
    char folded[256];
    anima_lang_fold(q, folded, sizeof folded);                  // "Napoleón", "Müller": as the builder folds keys
    char tok[ANIMA_DICT_TOKENS][ANIMA_DICT_TOKLEN];
    const int n = anima_dict_tokenize(folded, tok);
    if (n < 1 || n >= ANIMA_DICT_TOKENS) return false;
    // lead-ins, longest first (folded tokens: "cos'è" -> "cos e", "qué" -> "que")
    static const char *const LEADS[] = {
        "che cosa e", "che cos e", "cos e", "cosa e", "cosa sono", "cos erano", "chi e stato", "chi e stata",
        "chi erano", "chi era", "chi e", "chi sono", "cosa sai di", "cosa sai dirmi di", "cosa sai dirmi su",
        "dimmi qualcosa su", "dimmi qualcosa di", "parlami di", "parlami del", "parlami della", "parlami dello",
        "parlami dei", "parlami degli", "parlami delle", "parlami dell", "raccontami di", "dimmi di",
        "who is", "who was", "who were", "what is", "what are", "what was", "tell me about",
        "quien es", "quien era", "quien fue", "que es", "que son", "qui est", "qui etait", "qu est ce que",
        "c est quoi", "wer ist", "wer war", "was ist", "was sind", NULL };
    static const char *const ARTS[] = { "il", "lo", "la", "i", "gli", "le", "l", "un", "uno", "una", "del", "della",
        "dello", "dei", "degli", "delle", "dell", "di", "su", "sul", "sulla", "the", "a", "an", "about", "el", "los",
        "las", "une", "les", "des", "du", "der", "die", "das", "den", "dem", "ein", "eine", "e", NULL };
    int s = 0;
    for (int i = 0; LEADS[i] && !s; i++) {
        char lt[8][ANIMA_DICT_TOKLEN]; int ln = 0;
        char buf[64]; snprintf(buf, sizeof buf, "%s", LEADS[i]);
        for (char *t = strtok(buf, " "); t && ln < 8; t = strtok(NULL, " ")) snprintf(lt[ln++], ANIMA_DICT_TOKLEN, "%s", t);
        if (ln >= n) continue;
        bool m = true;
        for (int k = 0; k < ln && m; k++) m = !strcmp(tok[k], lt[k]);
        if (m) s = ln;
    }
    if (!s && !bare_ok) return false;
    if (!s && n > 4) return false;
    while (s < n - 1 && tok_in(tok[s], ARTS)) s++;           // "la fotosintesi" -> "fotosintesi"
    int o = 0; key[0] = 0;
    for (int i = s; i < n && o < (int)cap - 1; i++) o += snprintf(key + o, cap - o, "%s%s", o ? " " : "", tok[i]);
    return o > 1 && o < (int)cap;
}

// ---- lookup -------------------------------------------------------------------------------------------

// Binary search of the KEYS section [lo, hi): whole lines, byte order (the anima_dict_get algorithm,
// bounded to one section of the pack). The value after the tab goes to `out`.
static bool kb_key_get(FILE *f, uint64_t lo64, uint64_t hi64, const char *key, char *out, size_t cap)
{
    char *line = (char *)malloc(KB_LINE);
    if (!line) return false;
    long lo = (long)lo64, hi = (long)hi64;
    const long end = hi;
    int c; bool found = false;
    while (hi - lo > 4096) {
        const long mid = lo + (hi - lo) / 2;
        fseek(f, mid, SEEK_SET);
        while ((c = fgetc(f)) != EOF && c != '\n') {}
        const long ls = ftell(f);
        if (ls >= hi || !fgets(line, KB_LINE, f)) { hi = mid; continue; }
        char *tab = strchr(line, '\t');
        if (!tab) { lo = ftell(f); continue; }
        *tab = 0;
        if (strcmp(line, key) < 0) lo = ftell(f);
        else hi = mid;
    }
    fseek(f, lo, SEEK_SET);
    long scanned = 0;
    while (scanned < 3 * KB_LINE + 8192 && ftell(f) < end && fgets(line, KB_LINE, f)) {
        scanned += (long)strlen(line);
        char *tab = strchr(line, '\t');
        if (!tab) continue;
        *tab = 0;
        const int cmp = strcmp(line, key);
        if (cmp == 0) {
            char *v = tab + 1; v[strcspn(v, "\r\n")] = 0;
            snprintf(out, cap, "%s", v); found = true; break;
        }
        if (cmp > 0) break;
    }
    free(line);
    return found;
}

// The record of an entity, inflated: fields joined by 0x1E. Caller frees *buf.
static char *kb_record(const kb_pack_t *p, uint32_t ent, size_t *len)
{
    if (ent >= p->n_ent) return NULL;
    FILE *f = fopen(p->path, "rb");
    if (!f) return NULL;
    uint8_t e[12], b[16];
    char *rec = NULL;
    uint8_t *z = NULL, *raw = NULL;
    tinfl_decompressor *t = NULL;
    if (fseek(f, (long)(p->ents_off + 12ull * ent), SEEK_SET) || fread(e, 1, 12, f) != 12) goto out;
    const uint32_t blk = rd32(e), off = rd32(e + 4), n = rd32(e + 8);
    if (blk >= p->n_blk || fseek(f, (long)(p->bidx_off + 16ull * blk), SEEK_SET) || fread(b, 1, 16, f) != 16) goto out;
    const uint64_t boff = rd64(b);
    const uint32_t csz = rd32(b + 8), rsz = rd32(b + 12);
    if (!csz || !rsz || rsz > KB_BLOCKMAX || (uint64_t)off + n > rsz || boff + csz > p->blks_end) goto out;
    z = (uint8_t *)malloc(csz); raw = (uint8_t *)malloc(rsz);
    t = (tinfl_decompressor *)malloc(sizeof(tinfl_decompressor));   // ~11 KB: heap (PSRAM), never the stack
    if (!z || !raw || !t || fseek(f, (long)boff, SEEK_SET) || fread(z, 1, csz, f) != csz) goto out;
    tinfl_init(t);
    size_t in_len = csz, out_len = rsz;
    const tinfl_status st = tinfl_decompress(t, z, &in_len, raw, raw, &out_len, TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF);
    if (st != TINFL_STATUS_DONE || out_len != rsz) goto out;
    rec = (char *)malloc(n + 1);
    if (rec) { memcpy(rec, raw + off, n); rec[n] = 0; *len = n; }
out:
    free(t); free(raw); free(z);
    fclose(f);
    return rec;
}

static const char *kb_field(const char *rec, int idx, size_t *n)
{
    const char *p = rec;
    for (int i = 0; i < idx; i++) { p = strchr(p, 0x1E); if (!p) { *n = 0; return ""; } p++; }
    *n = strcspn(p, "\x1e");
    return p;
}

// A hit in another language's pack: when the user's own language pack has the same Wikidata entity, answer
// from that one ("who is Einstein" found via the Italian pack -> the English article, if installed).
static void kb_to_user_lang(anima_kb_ref_t *ref, const char *lang)
{
    if (!lang) return;
    size_t len = 0;
    char *rec = kb_record(&s_pack[ref->pack], ref->ent, &len);
    if (!rec) return;
    size_t ql; const char *q = kb_field(rec, 1, &ql);
    char qid[24];
    snprintf(qid, sizeof qid, "%.*s", (int)(ql < sizeof qid ? ql : sizeof qid - 1), q);
    free(rec);
    if (qid[0] != 'Q') return;
    for (int i = 0; i < s_npack; i++) {
        if (strcmp(s_pack[i].lang, lang) || !s_pack[i].qids_off) continue;
        FILE *f = fopen(s_pack[i].path, "rb");
        if (!f) continue;
        char v[24];
        const bool hit = kb_key_get(f, s_pack[i].qids_off, s_pack[i].qids_end, qid, v, sizeof v);
        fclose(f);
        if (!hit) continue;
        ref->pack = (int8_t)i; ref->ent = (uint32_t)strtoul(v, NULL, 10);
        char *r2 = kb_record(&s_pack[i], ref->ent, &len);           // its title in the user's language
        if (r2) { size_t tl; const char *t = kb_field(r2, 0, &tl);
                  snprintf(ref->title, sizeof ref->title, "%.*s", (int)tl, t); free(r2); }
        return;
    }
}

anima_kb_kind_t nucleo_anima_kb_find(const char *key, const char *lang, anima_kb_ref_t *refs, int max, int *n)
{
    *n = 0;
    if (!key || !key[0] || nucleo_anima_kb_pack_count() <= 0) return ANIMA_KB_NONE;
    char val[256];
    // the user's language first, then English (the most widely read fallback), then the others
    for (int pass = 0; pass < 3; pass++) {
        for (int i = 0; i < s_npack; i++) {
            const bool mine = lang && !strcmp(s_pack[i].lang, lang);
            const bool eng = !strcmp(s_pack[i].lang, "en");
            if (pass == 0 ? !mine : pass == 1 ? (mine || !eng) : (mine || eng)) continue;
            FILE *f = fopen(s_pack[i].path, "rb");
            if (!f) continue;
            const bool hit = kb_key_get(f, s_pack[i].keys_off, s_pack[i].keys_end, key, val, sizeof val);
            fclose(f);
            if (!hit) continue;
            // ids in pack order: exact first, then '~' ambiguous, then '^' section (the builder sorts them)
            uint32_t ex[ANIMA_KB_MAXREF], am[ANIMA_KB_MAXREF], se = 0, xl = 0;
            int nex = 0, nam = 0, nse = 0, nxl = 0;
            for (char *t = strtok(val, ","); t; t = strtok(NULL, ",")) {
                const char mark = (*t == '~' || *t == '^' || *t == '@') ? *t++ : 0;
                const uint32_t id = (uint32_t)strtoul(t, NULL, 10);
                if (mark == '@') { if (!nxl++) xl = id; continue; }        // the title in another language
                if (!mark && nex < ANIMA_KB_MAXREF) ex[nex++] = id;
                else if (mark == '~' && nam < ANIMA_KB_MAXREF) am[nam++] = id;
                else if (mark == '^' && !nse++) se = id;
            }
            anima_kb_kind_t kind = ANIMA_KB_NONE;
            const uint32_t *src = NULL; int cnt = 0;
            if (!nex && nxl)   { ex[nex++] = xl; }                      // "napoleon" -> Napoleone Bonaparte
            if (nex)           { kind = ANIMA_KB_EXACT;     src = ex; cnt = 1; }
            else if (nam > 1)  { kind = ANIMA_KB_AMBIGUOUS; src = am; cnt = nam; }
            else if (nam == 1) { kind = ANIMA_KB_EXACT;     src = am; cnt = 1; }
            else if (nse)      { kind = ANIMA_KB_SECTION;   src = &se; cnt = 1; }
            for (int j = 0; j < cnt && *n < max; j++) {
                refs[*n].pack = (int8_t)i; refs[*n].ent = src[j]; refs[*n].title[0] = 0; (*n)++;
            }
            for (int j = 0; j < *n; j++) {                       // titles, for the reply / the clarify options
                size_t len = 0; char *rec = kb_record(&s_pack[refs[j].pack], refs[j].ent, &len);
                if (rec) { size_t fl; const char *t0 = kb_field(rec, 0, &fl);
                           snprintf(refs[j].title, sizeof refs[j].title, "%.*s", (int)fl, t0); free(rec); }
            }
            if (*n && kind != ANIMA_KB_AMBIGUOUS && pass > 0) kb_to_user_lang(&refs[0], lang);
            if (*n) return kind;
        }
    }
    return ANIMA_KB_NONE;
}

int nucleo_anima_kb_facts(const anima_kb_ref_t *ref, char *out, size_t cap)
{
    if (!ref || ref->pack < 0 || ref->pack >= s_npack) return 0;
    size_t len = 0;
    char *rec = kb_record(&s_pack[ref->pack], ref->ent, &len);
    if (!rec) return 0;
    size_t fl;
    const char *f = kb_field(rec, 6, &fl);
    if (fl) snprintf(out, cap, "%.*s", (int)fl, f);
    free(rec);
    return fl != 0;
}

int nucleo_anima_kb_text(const anima_kb_ref_t *ref, int part, char *out, size_t cap)
{
    if (!ref || ref->pack < 0 || ref->pack >= s_npack) return 0;
    size_t len = 0;
    char *rec = kb_record(&s_pack[ref->pack], ref->ent, &len);
    if (!rec) return 0;
    int ok = 0; size_t fl;
    if (part == 0) {
        const char *s = kb_field(rec, 4, &fl);
        if (fl) { snprintf(out, cap, "%.*s", (int)fl, s); ok = 1; }
    } else {
        const char *p = kb_field(rec, 5, &fl);
        for (int k = 1; fl && k < part; k++) {                   // passages are joined by 0x1F
            const char *q = memchr(p, 0x1F, fl);
            if (!q) { fl = 0; break; }
            fl -= (size_t)(q + 1 - p); p = q + 1;
        }
        if (fl) { size_t m = strcspn(p, "\x1f\x1e"); if (m > fl) m = fl;
                  if (m) { snprintf(out, cap, "%.*s", (int)m, p); ok = 1; } }
    }
    free(rec);
    return ok;
}
