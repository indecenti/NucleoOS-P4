// ANIMA orchestrator: nucleo_anima_query() and the L0 tier, plus session memory, the action ring,
// tools (settings, reminders, notes, translation) and the cascade into the higher tiers
// (nucleo_anima_l1.c, nucleo_anima_hdc.c, nucleo_anima_facet.c, nucleo_anima_online.c).
//
// L0 pipeline: normalize (lowercase, strip Italian accents, drop punctuation) -> tokenize -> score
// each intent by keyword overlap (prefix-tolerant, so inflections match) -> confidence gate.
// Allocation-free and bounded. On an L0 miss the higher tiers are tried before an honest
// "non lo so".
#include "nucleo_anima.h"
#include "anima_internal.h"
#include "nucleo_anima_conv.h"   // nucleo_anima_teacher_complete: context compaction
#include "anima_l1.h"
#include "anima_phrases.h"      // L0 paraphrase table (generated)
#include "anima_intent.h"       // offline intent suggester (generated weights)
#include "nucleo_anima_online.h"
#include "nucleo_anima_learn.h"
#include "nucleo_anima_profile.h"
#include "nucleo_anima_translate.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"    // vTaskDelay: the bounded waits for the spine gate
#include "nucleo_board.h"
#include "cJSON.h"          // ACT rule add: the automation JSON
#ifndef ANIMA_HOST
#include "esp_attr.h"      // RTC_NOINIT_ATTR — DIAG breadcrumb that survives a warm reboot (device)
#else
#define RTC_NOINIT_ATTR    // host harness: plain global, no RTC section
#define EXT_RAM_BSS_ATTR   // host harness: no PSRAM section
#endif
#include <string.h>
#include <strings.h>   // strncasecmp
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <math.h>
#include <time.h>
#include "esp_log.h"
#include "esp_heap_caps.h"   // comp_mem: summary buffers in PSRAM, allocated on first use
#include <stdatomic.h>

// ANIMA spine gate: serialize nucleo_anima_query() across its TWO callers — the web handler (httpd
// task, nucleo_httpd.c) and the native app worker (app_anima.cpp). Exactly one cascade owns the shared
// L1/session globals (s_centroids, s_idx, the static qv/row scratch, s_session) at a time, which fixes
// the concurrent use-after-free panic (one caller frees the L1 index via nucleo_anima_l1_unload while
// the other is mid-read). Lock order is always gate -> (online TLS arbiter token), never the reverse,
// so there is no deadlock and no double-hold with the online tier's own arbiter use.
static atomic_flag s_anima_gate = ATOMIC_FLAG_INIT;
bool nucleo_anima_try_lock(void) { return !atomic_flag_test_and_set_explicit(&s_anima_gate, memory_order_acquire); }
void nucleo_anima_unlock(void)   { atomic_flag_clear_explicit(&s_anima_gate, memory_order_release); }

// Heap reclaim for OUTSIDE callers (proxy/llm fetchers, exclusive mode, UI transitions): free the L1
// index only if no cascade is mid-query. An unguarded unload from another task frees s_cdir/s_ec_row
// and closes the index FILE* under a running l1_query -> use-after-free. Fail-closed: if ANIMA is
// busy we skip the reclaim (the caller's own heap gate then refuses the work gracefully) rather than
// corrupt a live query. In-cascade callers hold the gate already and keep calling l1_unload directly.
bool nucleo_anima_l1_unload_if_idle(void)
{
    if (!nucleo_anima_try_lock()) return false;
    nucleo_anima_l1_unload();
    nucleo_anima_unlock();
    return true;
}

// Same guarded pattern for the PSRAM file mirrors (12 MB budget, nucleo_anima_l1.c): the memory
// broker calls this when a foreground app's budget doesn't fit. Busy -> 0 freed, never corrupt.
size_t nucleo_anima_l1_cache_flush_if_idle(void)
{
    if (!nucleo_anima_try_lock()) return 0;
    const size_t freed = nucleo_anima_l1_cache_flush();
    nucleo_anima_unlock();
    return freed;
}

static const char *TAG = "anima";

#define SESSION_PATH   NUCLEO_SD_MOUNT "/data/anima/session.txt"
#define TELEMETRY_PATH NUCLEO_SD_MOUNT "/data/anima/telemetry.ndjson"
#define TELEMETRY_CAP  65536          // rotate the routing log past 64 KB (bounded SD use)

#define A_MAX_TOKENS   24
#define A_TOK_LEN      24
#define A_MAX_KW       28

// ---- intent table -----------------------------------------------------------
// Keywords are pre-normalized (lowercase ASCII, no accents). Matching is prefix-
// tolerant in both directions (min 3 chars), so "aprire"~"apri", "foto"~"fotografie".
typedef struct {
    const char    *id;
    anima_action_t action;
    const char    *arg;       // app id / system key, or NULL (generic "open the named app")
    const char    *reply_it;  // static reply IT, or NULL for LAUNCH/SYSTEM
    const char    *reply_en;  // static reply EN
    const char    *kw[A_MAX_KW];
} a_intent_t;

// <gen:app-alias> --- GENERATED from registry/app-aliases.json by tools/anima/gen_aliases.py.
// DO NOT EDIT BY HAND: edit the JSON and run `python tools/anima/gen_aliases.py`.
#define A_MAX_ALIAS 11
typedef struct { const char *id; const char *alias[A_MAX_ALIAS]; } a_alias_t;

// Words the user might type (IT+EN) -> the registry app id ANIMA opens. First match wins.
static const a_alias_t APP_ALIAS[] = {
    { "gallery",       { "foto", "fotografie", "immagini", "galleria", "photos", "photo", "images", "pictures", "gallery", NULL } },
    { "notes",         { "note", "blocco", "appunti", "nota", "scrivi", "notes", "notepad", "write", NULL } },
    { "files",         { "file", "files", "cartelle", "documenti", "esplora", "documents", "folders", NULL } },
    { "music",         { "musica", "brani", "canzoni", "audio", "lettore", "music", "songs", "song", "player", "mp3", NULL } },
    { "video",         { "video", "filmati", "filmato", "film", "videos", "movie", "movies", NULL } },
    { "calc",          { "calcolatrice", "calcoli", "calculator", "math", NULL } },   // No 'calc'/'calcolo' alias on purpose: prefix-collides with future spreadsheet vocab.
    { "terminal",      { "terminale", "terminal", "shell", "console", "prompt", "cli", NULL } },
    { "settings",      { "impostazioni", "settaggi", "configurazione", "opzioni", "settings", "options", "config", NULL } },
    { "tasks",         { "tasks", "task", "attivita", "compiti", "todo", NULL } },
    { "sysmon",        { "monitor", "risorse", "prestazioni", "processi", "resources", "performance", NULL } },   // 'monitor' resolves here; secondscreen uses 'schermo'/'display' instead.
    { "camera",        { "fotocamera", "camera", "scatta", "selfie", "riprendi", NULL } },
    { "recorder",      { "registratore", "registra", "microfono", "recorder", "record", "memo", "dettatura", "registrazione", NULL } },
    { "diag",          { "diagnostica", "log", "registro", "logs", "diagnostics", "crash", NULL } },
    { "apps",          { "app", "apps", "store", "applicazioni", "giochi", "gioco", "games", "game", NULL } },   // 'giochi' opens the store: games are WASM tiles inside it.
    { "secondscreen",  { "schermo", "estendi", "screen", "display", NULL } },
    { "anima",         { "assistente", "chat", "assistant", NULL } },   // No 'anima' alias: the whoami intent owns that word; these open the chat app.
    { "abc123",        { "abc", "alfabeto", "lettere", "numeri", "imparare", "letters", "numbers", NULL } },
    { "pianino",       { "piano", "pianino", "pianoforte", "tastiera", NULL } },
};
// </gen:app-alias>

static const a_intent_t INTENTS[] = {
    // Generic "open <app>": arg NULL -> the named app is resolved from the query.
    { "open_app", ANIMA_ACT_LAUNCH, NULL, NULL, NULL,
      { "apri", "aprire", "avvia", "lancia", "esegui", "vai", "mostra", "mostrami",
        // NB: "vorrei"/"voglio" are DESIRE prefixes that DO carry some launches ("voglio disegnare
        // qualcosa" → Paint) but also precede knowledge ("vorrei sapere perché…"). They stay here, but
        // a_l0_suppress_desire_launch() vetoes an open_app win that rests only on a desire-prefix when the
        // query is a knowledge question (def/question cue + no STRONG open verb) — so "vorrei … test"
        // (test~testo→notepad) no longer hijacks a question into a launch.
        "vedere", "vedi", "guarda", "guardare", "vorrei", "voglio", "portami", "fammi",
        // play/put-on verbs: "metti la musica" / "riproduci la radio" / "play music" -> launch the app
        "metti", "riproduci", "suona", "ascolta", "accedi", "entra",
        "open", "show", "launch", "run", "play", "start" } },

    // Live system queries -> caller fills the localized value into the template.
    { "battery", ANIMA_ACT_SYSTEM, "battery", "Batteria: {value}.", "Battery: {value}.",
      { "batteria", "carica", "autonomia", "energia", "battery", NULL } },
    { "time",    ANIMA_ACT_SYSTEM, "time",    "{value}.", "{value}.",
      { "ora", "ore", "orario", "adesso", "time", "clock", NULL } },
    { "storage", ANIMA_ACT_SYSTEM, "storage", "Spazio SD: {value}.", "SD space: {value}.",
      // NB no bare "memoria": with the C/ESP knowledge packs it means RAM/allocation far more
      // often than SD space ("alloco memoria dinamica" must reach L1, not the storage reading).
      // RAM-free questions are handled by a_is_ram() instead.
      { "spazio", "disco", "scheda", "archiviazione", "space", "storage", "sd", "capacita", "gigabyte", NULL } },

    // Computed-from-state ("4th pillar"): the device answers from its own RTC, so any phrasing of
    // these works with zero cards and is always exact. The caller fills {value} from localtime().
    { "date",   ANIMA_ACT_SYSTEM, "date",   "{value}.", "{value}.",
      // NB no "today"/"calendario": "today" mis-fires ("bitcoin today") and "calendario" is the
      // Calendar app alias. "data"/"oggi"/"giorno"/"date" are specific enough.
      { "data", "oggi", "giorno", "date", NULL } },
    { "year",   ANIMA_ACT_SYSTEM, "year",   "Siamo nel {value}.", "It's {value}.",
      { "anno", "year", "annata", NULL } },
    { "season", ANIMA_ACT_SYSTEM, "season", "Siamo in {value}.", "It's {value}.",
      { "stagione", "season", NULL } },
    // Self-state the device reads from its own runtime (always exact, zero cards). Network/RAM
    // status are detectors (a_is_network/a_is_ram) — they must dodge the knowledge questions
    // ("cos'e il wifi", "alloco memoria") — but version/uptime have unambiguous keywords.
    { "version", ANIMA_ACT_SYSTEM, "version", "Eseguo {value}.", "Running {value}.",
      // NB no bare "firmware": "cos'e il firmware" is a definition (esp.arduino-fw card -> L1).
      { "versione", "version", NULL } },
    { "uptime",  ANIMA_ACT_SYSTEM, "uptime",  "Acceso da {value}.", "Up for {value}.",
      { "uptime", "acceso", "accesa", "avvio", NULL } },

    // Static FAQ answers (L0 covers the obvious questions with zero retrieval).
    { "whoami", ANIMA_ACT_ANSWER, NULL,
      "Sono ANIMA, l'assistente offline di NucleoOS. Funziono senza internet, sul dispositivo.",
      "I'm ANIMA, NucleoOS's offline assistant. I work with no internet, on the device.",
      // NB no bare "sei": it hijacks "sei un robot" / "sei viva" -> those reach L1 (self.* cards).
      // "chi sei" still matches via "chi"; "who are you" via "who".
      { "chi", "anima", "presentati", "who", "you", NULL } },
    // NB no static "help" intent: "cosa sai fare" / "aiuto" / "comandi" are caught by
    // a_is_capabilities() and answered DYNAMICALLY by the executor (live apps + pillars).

    // Bare greetings/thanks: without these L0 misses and the cascade returned an EMPTY reply —
    // the chat showed a blank bubble for "ciao". Both are short-query gated in the scorer
    // (<= 3 tokens) so a greeting prefix never hijacks a real question ("ciao anima, spiegami
    // la fotosintesi" must reach retrieval).
    { "greeting", ANIMA_ACT_ANSWER, NULL,
      "Ciao! Chiedimi di aprire un'app, l'ora, lo spazio libero... o qualsiasi cosa.",
      "Hi! Ask me to open an app, the time, free space... or anything else.",
      { "ciao", "salve", "buongiorno", "buonasera", "buonanotte", "hello", "hey", NULL } },
    { "thanks", ANIMA_ACT_ANSWER, NULL,
      "Prego! Sono qui se ti serve altro.",
      "You're welcome! I'm here if you need anything else.",
      { "grazie", "thanks", "thank", NULL } },

    // "Come si programma un'app?" — the on-device engine can't write code, but it must know its
    // own platform: point at the SDK flow (PC-side clang -> push over Wi-Fi). The web copilot
    // with a cloud key goes further and actually writes the C (see DEV_REF in sd/web/copilot.js).
    // NB keywords are the unambiguous dev words only: "crea"/"app" alone belong to create_file
    // and the app store ("apri le app" must stay a launch, "crea una nota" a file tool).
    { "make_app", ANIMA_ACT_ANSWER, NULL,
      "Le app di NucleoOS sono in C compilato in WASM sul PC: cartella apps/<id> con manifest.json "
      "+ main.c (guida: docs/WASM_APPS.md, esempio: apps/abc123), poi build_push.ps1 la carica via "
      "Wi-Fi. Dal web, ANIMA con una chiave cloud puo' scriverti il codice.",
      "NucleoOS apps are C compiled to WASM on a PC: an apps/<id> folder with manifest.json + "
      "main.c (guide: docs/WASM_APPS.md, example: apps/abc123), then build_push.ps1 uploads it "
      "over Wi-Fi. From the web, ANIMA with a cloud key can write the code for you.",
      { "programmare", "programmo", "sviluppare", "sviluppo", "wasm", "sdk", "develop", "coding", NULL } },
};

// ---- normalization & tokenization ------------------------------------------

// Append one normalized ASCII char to a bounded token; returns new length.
static int a_putc(char *tok, int len, char c)
{
    if (len < A_TOK_LEN - 1) tok[len++] = c;
    return len;
}

// Normalize `in` and split into lowercase ASCII tokens. Italian accented vowels
// (UTF-8 0xC3 0xA0..0xBA) fold to their base letter. Returns token count.
static int a_tokenize(const char *in, char tok[A_MAX_TOKENS][A_TOK_LEN])
{
    int n = 0, len = 0;
    char cur[A_TOK_LEN];
    for (const unsigned char *p = (const unsigned char *)in; ; p++) {
        unsigned char c = *p;
        char out = 0;

        if (c == 0xC3 && p[1]) {            // 2-byte UTF-8 Latin-1 supplement
            unsigned char d = *++p;
            switch (d) {
                case 0xA0: case 0xA1: case 0xA2: out = 'a'; break;  // à á â
                case 0xA8: case 0xA9: case 0xAA: out = 'e'; break;  // è é ê
                case 0xAC: case 0xAD: case 0xAE: out = 'i'; break;  // ì í î
                case 0xB2: case 0xB3: case 0xB4: out = 'o'; break;  // ò ó ô
                case 0xB9: case 0xBA: case 0xBB: out = 'u'; break;  // ù ú û
                default: out = 0; break;
            }
        } else if (isalnum(c)) {
            out = (char)tolower(c);
        }

        if (out) {
            len = a_putc(cur, len, out);
        } else {                            // separator or end -> flush token
            if (len > 0 && n < A_MAX_TOKENS) {
                cur[len] = 0;
                memcpy(tok[n++], cur, len + 1);
                len = 0;
            }
            if (c == 0) break;
        }
    }
    return n;
}

// Prefix-tolerant equality: equal, or one is a >=4-char prefix of the other. The >=4 (not 3)
// matters: 3-letter keywords are prefixes of countless words ("chi"->"chiedo", "ora"->"orario")
// and would hijack knowledge queries at L0 before L1 retrieval runs — so they must match exactly.
static bool a_match(const char *a, const char *b)
{
    if (strcmp(a, b) == 0) return true;
    size_t la = strlen(a), lb = strlen(b), m = la < lb ? la : lb;
    if (m < 4) return false;
    // Prefix tolerance is for inflections (giorno->giornata), which add a SHORT suffix.
    // Cap the length gap at 2 so a keyword can't bleed into a different word that merely
    // shares its start: "data"->"database"/"dataset" was hijacking knowledge Qs into intent=date.
    if ((la > lb ? la - lb : lb - la) > 2) return false;
    return strncmp(a, b, m) == 0;
}

// Count distinct intent keywords present in the token list.
static int a_score(const a_intent_t *it, char tok[A_MAX_TOKENS][A_TOK_LEN], int ntok)
{
    int hits = 0;
    for (int k = 0; k < A_MAX_KW && it->kw[k]; k++)
        for (int t = 0; t < ntok; t++)
            if (a_match(it->kw[k], tok[t])) { hits++; break; }
    return hits;
}

// Resolve the app id named in the query (for the generic open_app intent).
static const char *a_resolve_app(char tok[A_MAX_TOKENS][A_TOK_LEN], int ntok)
{
    for (size_t i = 0; i < sizeof(APP_ALIAS) / sizeof(APP_ALIAS[0]); i++)
        for (int j = 0; j < A_MAX_ALIAS && APP_ALIAS[i].alias[j]; j++)
            for (int t = 0; t < ntok; t++)
                if (a_match(APP_ALIAS[i].alias[j], tok[t]))
                    return APP_ALIAS[i].id;
    return NULL;
}

// The paraphrase key of `in`: "it:"/"en:" + the tokens minus the filler words, exactly as
// tools/gen_anima_phrases.py builds it. False when there is no usable key.
static bool a_phrase_key(const char *in, bool en, char *key, int cap)
{
    char tok[A_MAX_TOKENS][A_TOK_LEN];
    const int ntok = a_tokenize(in, tok);
    if (ntok == 0 || ntok >= A_MAX_TOKENS) return false;  // a full token array may have been cut: no key
    int o = snprintf(key, cap, "%s:", en ? "en" : "it");
    for (int t = 0; t < ntok && o < cap; t++)
        if (!anima_phrase_is_filler(en, tok[t])) o += snprintf(key + o, cap - o, "%s%s", key[o - 1] == ':' ? "" : " ", tok[t]);
    return o < cap && key[o - 1] != ':';
}

// ── USER PHRASES: what this user taught by answering "sì" to a suggestion ("tira su il volume" -> "alza il
// volume"). /data/anima/phrases.user.tsv, one "key<TAB>canonical" per line, loaded on first use; the same
// rewrite as the built-in table, checked after it (the built-in meaning always wins). ──
#define USER_PHRASES_MAX 2048       // PSRAM is plentiful while ANIMA runs (~360 KB, allocated on first use only)
typedef struct { char key[112]; char canon[64]; } user_phrase_t;
static user_phrase_t *s_userp;                   // USER_PHRASES_MAX entries, heap (PSRAM) on first use, never static
static int s_userp_n = -1;                       // -1 = not loaded yet
#define USER_PHRASES_PATH NUCLEO_SD_MOUNT "/data/anima/phrases.user.tsv"

static void userp_load(void)
{
    s_userp_n = 0;
    if (!s_userp) s_userp = (user_phrase_t *)calloc(USER_PHRASES_MAX, sizeof *s_userp);   // > SPIRAM threshold: PSRAM
    if (!s_userp) return;
    FILE *f = fopen(USER_PHRASES_PATH, "r");
    if (!f) return;
    char line[200];
    while (s_userp_n < USER_PHRASES_MAX && fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = 0;
        char *tab = strchr(line, '\t');
        if (!tab || tab == line || !tab[1] || (size_t)(tab - line) >= sizeof s_userp[0].key ||
            strlen(tab + 1) >= sizeof s_userp[0].canon) continue;
        *tab = 0;
        snprintf(s_userp[s_userp_n].key, sizeof s_userp[0].key, "%s", line);
        snprintf(s_userp[s_userp_n].canon, sizeof s_userp[0].canon, "%s", tab + 1);
        s_userp_n++;
    }
    fclose(f);
}

static const char *userp_lookup(const char *key)
{
    if (s_userp_n < 0) userp_load();
    if (!s_userp) return NULL;
    for (int i = 0; i < s_userp_n; i++) if (!strcmp(s_userp[i].key, key)) return s_userp[i].canon;
    return NULL;
}

// Remember `phrase` as `canon`. False when it cannot (full, too long, or the built-in table means otherwise).
static bool userp_learn(const char *phrase, bool en, const char *canon)
{
    char key[160];
    if (!a_phrase_key(phrase, en, key, sizeof key) || strlen(key) >= sizeof s_userp[0].key ||
        strlen(canon) >= sizeof s_userp[0].canon || strchr(canon, '\t') || strchr(canon, '\n')) return false;
    const char *have = anima_phrase_lookup(key);
    if (have) return !strcmp(have, canon);
    if (s_userp_n < 0) userp_load();
    if (!s_userp) return false;
    for (int i = 0; i < s_userp_n; i++)
        if (!strcmp(s_userp[i].key, key)) {
            if (!strcmp(s_userp[i].canon, canon)) return true;
            snprintf(s_userp[i].canon, sizeof s_userp[0].canon, "%s", canon);   // the newest answer wins
            FILE *f = fopen(USER_PHRASES_PATH ".tmp", "w");
            if (!f) return false;
            for (int j = 0; j < s_userp_n; j++) fprintf(f, "%s\t%s\n", s_userp[j].key, s_userp[j].canon);
            fclose(f);
            return rename(USER_PHRASES_PATH ".tmp", USER_PHRASES_PATH) == 0;
        }
    if (s_userp_n >= USER_PHRASES_MAX) return false;
    FILE *f = fopen(USER_PHRASES_PATH, "a");
    if (!f) return false;
    fprintf(f, "%s\t%s\n", key, canon);
    fclose(f);
    snprintf(s_userp[s_userp_n].key, sizeof s_userp[0].key, "%s", key);
    snprintf(s_userp[s_userp_n].canon, sizeof s_userp[0].canon, "%s", canon);
    s_userp_n++;
    return true;
}

// The canonical phrase for a known paraphrase of `in` (tools/anima_phrases.txt, then the user's), or NULL.
static const char *a_paraphrase(const char *in, bool en)
{
    char key[160];
    if (!a_phrase_key(in, en, key, sizeof key)) return NULL;
    const char *c = anima_phrase_lookup(key);
    return c ? c : userp_lookup(key);
}

// `w` is literally an app's name ("terminale"): never a verb, even when a verb prefix-matches it
// ("termina"~"terminale" turned "apri il terminale" into CLOSE terminal).
static bool a_is_app_word(const char *w)
{
    for (size_t i = 0; i < sizeof(APP_ALIAS) / sizeof(APP_ALIAS[0]); i++)
        for (int j = 0; j < A_MAX_ALIAS && APP_ALIAS[i].alias[j]; j++)
            if (!strcmp(APP_ALIAS[i].alias[j], w)) return true;
    return false;
}

// A generic open_app win that rests ONLY on a DESIRE prefix ("vorrei"/"voglio") is really a knowledge
// request when the query also carries a definition/question cue AND has NO strong open verb: "vorrei
// SAPERE PERCHÉ scrivere test automatici" must answer, not open a fuzzily-matched app (test~testo→notepad).
// "voglio DISEGNARE qualcosa" still launches (strong verb, no cue). Returns true to VETO the launch.
static bool a_desire_only_launch(char tok[A_MAX_TOKENS][A_TOK_LEN], int ntok)
{
    static const char *const STRONG[] = { "apri","aprire","avvia","lancia","esegui","vai","mostra","mostrami",
        "open","launch","show","run","start","metti","riproduci","suona","ascolta","play","portami",
        "accedi","entra","disegna","disegnare","draw", NULL };
    static const char *const CUE[] = { "cos","cosa","spiega","spiegami","significa","significato","definizione",
        "differenza","perche","come","quanto","quanta","quanti","quale","quali","chi","quando","dove",
        "sapere","conoscere","capire","why","what","how","explain","mean","means", NULL };
    bool strong = false, cue = false;
    for (int t = 0; t < ntok; t++) {
        for (int i = 0; STRONG[i]; i++) if (!strcmp(STRONG[i], tok[t])) { strong = true; break; }
        for (int i = 0; CUE[i]; i++)    if (!strcmp(CUE[i], tok[t]))    { cue = true; break; }
        // a NEGATED desire ("non voglio creare un file") is never a launch command
        if (!strcmp(tok[t], "non") || !strcmp(tok[t], "not")) return true;
    }
    return cue && !strong;
}

// A QUESTION whose ONLY "open" signal is a WEAK content verb (play/suona/metti/run/start — which also
// mean to play a sport, to put, to execute) is not a launch command: "what team did Einstein's father
// PLAY for", "come si SUONA il piano". Require a STRONG explicit launch verb to fire under a question.
// Veto otherwise so the query reaches the knowledge/online tier. Returns true to VETO the launch.
static bool a_question_not_launch(char tok[A_MAX_TOKENS][A_TOK_LEN], int ntok)
{
    static const char *const strong[] = { "apri","aprire","avvia","avviare","lancia","lanciare","esegui",
        "open","launch","start","avviami","portami","accedi","entra","aprimi", NULL };   // explicit "open the app"
    static const char *const qw[] = { "chi","quale","quali","quando","dove","come","perche","quanti","quanto",
        "quanta","cosa","what","which","who","whom","whose","when","where","why","how","did","does","do", NULL };
    // A temporal/quantity interrogative FRAME ("(in) che anno/ora/giorno …", "qual è l'anno …"): the bare
    // word "che"/"qual" is too broad to be a qword on its own (it heads relative clauses — "le foto CHE ho
    // scattato" — so it isn't in qw above), but IMMEDIATELY before a time/quantity noun it heads a WH-question.
    // Without this, the false-premise "in che anno MORIRÀ javascript" (and its "mostra"-spellfix sibling) had
    // the weak open verb "mostra" launch code-runner instead of abstaining. Mirrors the qword+keyword
    // adjacency in a_ambient_ok; restricted to "che"/"qual" so a genuine "MOSTRA le foto" still launches.
    static const char *const tframe[] = { "che", "qual", NULL };
    static const char *const tnoun[]  = { "anno","anni","annata","ora","ore","orario","giorno","giorni",
        "data","mese","mesi","year","hour","hours","day","days","date","month","time", NULL };
    bool q = false, strong_open = false;
    for (int t = 0; t < ntok; t++) {
        for (int i = 0; qw[i];     i++) if (!strcmp(qw[i],     tok[t])) q = true;
        for (int i = 0; strong[i]; i++) if (!strcmp(strong[i], tok[t])) strong_open = true;
        if (t + 1 < ntok) {
            bool isframe = false, isnoun = false;
            for (int i = 0; tframe[i]; i++) if (!strcmp(tframe[i], tok[t]))   isframe = true;
            for (int i = 0; tnoun[i];  i++) if (!strcmp(tnoun[i],  tok[t+1])) isnoun  = true;
            if (isframe && isnoun) q = true;
        }
    }
    return q && !strong_open;
}

// A real launch is "VERB + OBJECT" — a launch verb AND a DISTINCT app name. Reject the degenerate case
// where every open/app signal collapses onto the SAME ambiguous token: "play"/"player" alone, where the
// verb "play" prefix-matches the media-player alias "player", is a single noun, not "open the player".
// LEGIT structure survives via a token that is a PURE verb ("open the player" — "open" is a verb that is no
// app) OR a PURE app ("play some music" — "music" is an app that is no verb). Returns true to VETO. The
// open-verb list mirrors the open_app intent's launch keywords (desire prefixes vorrei/voglio excluded:
// they are not the structural verb of a command). a_match (prefix-tolerant) is used so "play"~"player".
static bool a_launch_is_degenerate(char tok[A_MAX_TOKENS][A_TOK_LEN], int ntok)
{
    static const char *const verbs[] = { "apri","aprire","avvia","lancia","esegui","vai","mostra","mostrami",
        "vedere","vedi","guarda","guardare","portami","metti","riproduci","suona","ascolta","accedi","entra",
        "open","show","launch","run","play","start", NULL };
    for (int t = 0; t < ntok; t++) {
        bool isverb = false, isapp = false;
        for (int i = 0; verbs[i]; i++) if (a_match(verbs[i], tok[t])) { isverb = true; break; }
        for (size_t i = 0; i < sizeof(APP_ALIAS) / sizeof(APP_ALIAS[0]) && !isapp; i++)
            for (int j = 0; j < A_MAX_ALIAS && APP_ALIAS[i].alias[j]; j++)
                if (a_match(APP_ALIAS[i].alias[j], tok[t])) { isapp = true; break; }
        if (isverb != isapp) return false;   // a PURE verb or a PURE app -> a real verb+object structure
    }
    return true;                             // only verb-AND-app (or filler) tokens -> ambiguous, not a command
}

// ============================================================================
// CORTEX — the cognitive front-end (roadmap "micro-thought"). Normalizes +
// classifies the query ONCE and emits a typed plan the cascade reads, instead
// of every tier re-parsing the input and deciding in isolation. Cheap: a single
// tokenize + a few word-list scans. Pure, allocation-free, O(tokens). The plan
// is the substrate the higher levers build on (gated spellfix, single memory
// reclaim, evidential gating).
// ============================================================================

// Query feature bits (computed once from the input).
enum {
    F_DIGIT    = 1u << 0,   // contains a number
    F_MATHOP   = 1u << 1,   // arithmetic operator (symbol, or word: per/diviso/piu/meno…)
    F_DEFWORD  = 1u << 2,   // a definition cue (cos'e / spiega / significa / differenza)
    F_QWORD    = 1u << 3,   // an interrogative (chi/quale/quando/dove/come/perche/quanti)
    F_OPENVERB = 1u << 4,   // an "open/launch app" verb
    F_CREATEVB = 1u << 5,   // a "create/new/write" verb
    F_TEMPORAL = 1u << 6,   // a time-bound word (domani/oggi/ieri/adesso/stamattina)
    F_WEATHER  = 1u << 7,   // weather (tempo/meteo/pioggia/sole/temperatura/gradi)
    F_NEWS     = 1u << 8,   // news/markets (notizie/prezzo/cambio/bitcoin/borsa)
    F_FILENOUN = 1u << 9,   // file/nota/documento/testo
    F_FOLLOWUP = 1u << 11,  // a bare pronoun-y follow-up (lo/la/quello/it/that)
};

typedef enum {
    ICLASS_UNKNOWN = 0,
    ICLASS_COMMAND,   // open app / read live state
    ICLASS_TOOL,      // create_file / math
    ICLASS_FACT,      // definition / entity question -> L1/online knowledge
    ICLASS_LIVE,      // weather/news/price -> live online tier (never cached)
    ICLASS_FOLLOWUP,  // refers to a previous turn
} anima_iclass_t;

typedef struct {
    uint16_t       feat;               // F_* bitfield
    anima_iclass_t klass;              // dominant intent class
    bool           allow_spellfix;     // the command-vocab typo rescue is safe for this query
    bool           reclaim_before_net; // free L1 ONCE before a heavy net/HDC tier (vs 3 scattered unloads)
} anima_plan_t;

// Does any token match a word in a NULL-terminated list (prefix-tolerant via a_match)?
static bool a_any(char tok[A_MAX_TOKENS][A_TOK_LEN], int ntok, const char *const *list)
{
    for (int t = 0; t < ntok; t++)
        for (int i = 0; list[i]; i++)
            if (a_match(list[i], tok[t])) return true;
    return false;
}

// a_any, but a word of 4 letters or fewer must match EXACTLY: a_match's prefix tolerance read "never"
// as "neve" (snow) and turned "never mind" into a forecast. Longer words keep their inflections.
static bool a_any_word(char tok[A_MAX_TOKENS][A_TOK_LEN], int ntok, const char *const *list)
{
    for (int t = 0; t < ntok; t++)
        for (int i = 0; list[i]; i++)
            if (strlen(list[i]) <= 4 ? !strcmp(list[i], tok[t]) : a_match(list[i], tok[t])) return true;
    return false;
}

// Build the typed plan for `raw`. The intelligence is which signals are present + a cheap
// dominant-class rule — deterministic, no model. Layers downstream read it instead of re-deciding.
static bool a_action_is_statement(char tok[A_MAX_TOKENS][A_TOK_LEN], int ntok);   // fwd: defined below
static void anima_cortex_plan(const char *raw, bool en, anima_plan_t *p)
{
    (void)en;
    memset(p, 0, sizeof(*p));
    char tok[A_MAX_TOKENS][A_TOK_LEN];
    int ntok = a_tokenize(raw, tok);

    // Raw scan for digits + symbol operators (the tokenizer drops punctuation). '-' is left out:
    // it appears in filenames/words far more than as "minus" (the word "meno" carries subtraction).
    for (const unsigned char *q = (const unsigned char *)raw; *q; q++) {
        if (isdigit(*q)) p->feat |= F_DIGIT;
        else if (*q=='+'||*q=='*'||*q=='/'||*q=='^'||*q=='%') p->feat |= F_MATHOP;
    }
    static const char *const w_mathop[] = { "per","diviso","fratto","piu","meno","moltiplicato","times","plus","minus","divided","radice","elevato","potenza","percento","percent", NULL };
    static const char *const w_def[]    = { "cos","cosa","spiega","spiegami","significa","significato","definizione","differenza","funziona","serve", NULL };
    static const char *const w_q[]      = { "chi","quale","quali","quando","dove","come","perche","quanti","quanto","quanta", NULL };
    static const char *const w_open[]   = { "apri","aprire","avvia","lancia","mostra","mostrami","open","launch","show","run","portami","metti","riproduci","suona","ascolta","play","start", NULL };
    static const char *const w_create[] = { "crea","creare","crei","nuovo","nuova","create","make","scrivi","annota","appunta","segna","prepara","genera","draft","jot", NULL };
    static const char *const w_temp[]   = { "domani","oggi","ieri","dopodomani","adesso","stamattina","stasera","stanotte","tonight","today","tomorrow","yesterday", NULL };
    static const char *const w_weather[]= { "tempo","meteo","pioggia","piove","sole","nuvoloso","temperatura","gradi","clima","weather","rain","forecast",
        "vento","neve","nevica","nebbia","umidita","temporale","grandine","sereno","soleggiato","piovoso","afa","ventoso","nuvole","piova","piovera","piovuto","wind","snow","sunny","cloudy", NULL };
    static const char *const w_news[]   = { "notizie","prezzo","cambio","bitcoin","borsa","quotazione","azioni","price","stock", NULL };
    static const char *const w_file[]   = { "file","nota","note","documento","document","testo","appunti","foglio", NULL };
    static const char *const w_fu[]     = { "lo","la","quello","quella","questo","questa","esso","essa","aprilo","aprila","it","that","this", NULL };
    if (a_any(tok,ntok,w_mathop)) p->feat |= F_MATHOP;
    if (a_any(tok,ntok,w_def))    p->feat |= F_DEFWORD;
    if (a_any(tok,ntok,w_q))      p->feat |= F_QWORD;
    if (a_any(tok,ntok,w_open))   p->feat |= F_OPENVERB;
    if (a_any(tok,ntok,w_create)) p->feat |= F_CREATEVB;
    if (a_any(tok,ntok,w_temp))   p->feat |= F_TEMPORAL;
    if (a_any_word(tok,ntok,w_weather))p->feat |= F_WEATHER;
    if (a_any(tok,ntok,w_news))   p->feat |= F_NEWS;
    if (a_any(tok,ntok,w_file))   p->feat |= F_FILENOUN;
    if (a_any(tok,ntok,w_fu))     p->feat |= F_FOLLOWUP;

    uint16_t f = p->feat;
    // Dominant class (cheapest decisive signal first). LIVE beats FACT (a weather phrasing carries a
    // q-word but must not be answered as a definition); math TOOL beats FACT for numeric ops.
    if ((f & F_WEATHER) || (f & F_NEWS) || ((f & F_TEMPORAL) && (f & F_QWORD)))
        p->klass = ICLASS_LIVE;
    else if ((f & F_DIGIT) && (f & F_MATHOP))
        p->klass = ICLASS_TOOL;
    else if ((f & F_CREATEVB) && (f & F_FILENOUN))
        p->klass = ICLASS_TOOL;
    else if ((f & F_DEFWORD) || ((f & F_QWORD) && !(f & F_OPENVERB)))
        p->klass = ICLASS_FACT;
    else if (f & F_OPENVERB)
        p->klass = ICLASS_COMMAND;
    else if (ntok <= 2 && (f & F_FOLLOWUP))
        p->klass = ICLASS_FOLLOWUP;
    else
        p->klass = ICLASS_UNKNOWN;

    // The command-vocab spellfix rescues brittle L0 command/tool words ("ofto"->"foto"). It must
    // NEVER touch a knowledge/live question: those route to L1/online (whose char-ngram encoder is
    // already typo-robust), and "correcting" a valid content word toward the command vocab corrupts
    // them — "che tempo fa domani" had "domani"->"comandi" hijack the query to "capabilities".
    // Also NEVER on an action STATEMENT/negation: the rescue would "correct" the valid past participle
    // "creato"->"crea" and the retry would arm a create the user only DESCRIBED ("ho creato un file").
    p->allow_spellfix = !(f & (F_DEFWORD | F_WEATHER | F_NEWS))
                        && p->klass != ICLASS_FACT && p->klass != ICLASS_LIVE
                        && !a_action_is_statement(tok, ntok);

    // The fact/live/unknown classes are the ones that may reach the heavy net/HDC tiers, which need a
    // large contiguous heap this PSRAM-less chip only has with L1 unloaded. Decide the reclaim ONCE.
    p->reclaim_before_net = (p->klass == ICLASS_FACT || p->klass == ICLASS_LIVE || p->klass == ICLASS_UNKNOWN);
}

// ---- tools (function calling) -----------------------------------------------
// An action verb in the PAST ("ho creato un file", "ho già abbassato la luce") or NEGATED ("non voglio
// creare un file") DESCRIBES or DECLINES an action — it must never EXECUTE one (a fabricated action is the
// action-side of a hallucination). Veto when: a negation is present, OR an auxiliary precedes a past
// participle (IT -ato/-ito/-uto, EN -ed), OR "già"/"already" marks completion. NB the matching imperative
// verbs whose participle prefix-collides are the -ato/-ito family (creato~crea, abbassato~abbassa); the
// auxiliary requirement keeps a genuine request safe ("ho bisogno di creARE un file" — infinitive, no aux+part).
static bool a_action_is_statement(char tok[A_MAX_TOKENS][A_TOK_LEN], int ntok)
{
    static const char *const neg[] = { "non", "mai", "senza", "not", "dont", "don", "never",
        "didnt", "doesnt", "wont", "cant", NULL };   // "don" covers the "don't"->"don"+"t" apostrophe split
    // Past participles of ACTION verbs that PREFIX-COLLIDE with their imperative (IT creato~crea,
    // abbassato~abbassa; EN created~create, lowered~lower): "ho creato un file" / "i already lowered the
    // brightness" DESCRIBE an action, never command one. Matched by stem + a past ending (IT -ato/-ito/-uto,
    // EN -ed) so it works even when the dropped 2-char auxiliary ("ho"/"i") leaves no other signal. The base
    // imperatives never carry these endings (crea/genera/abbassa/create/lower), so none are caught here.
    static const char *const pstem[] = { "creat", "prepar", "generat", "annotat", "segnat", "abbassat",
        "alzat", "aument", "diminu", "attivat", "disattivat", "impostat", "aggiornat", "modificat",
        "lower", "rais", "increas", "decreas", "turn", "add", NULL };   // EN stems (lowered/raised/added/...)
    // "non dimenticare / don't forget X" is a REMINDER idiom (the negation flips a forget-verb into
    // "remember"), NOT a declined command — so the negation veto must skip it.
    bool forget_idiom = false;
    for (int t = 0; t < ntok; t++)
        if (!strncmp(tok[t], "dimentic", 8) || !strncmp(tok[t], "scord", 5) || !strcmp(tok[t], "forget")) forget_idiom = true;
    for (int t = 0; t < ntok; t++) {
        if (!forget_idiom) for (int i = 0; neg[i]; i++) if (!strcmp(neg[i], tok[t])) return true;   // negated -> not a command
        size_t L = strlen(tok[t]); if (L < 5) continue;
        const char *e = tok[t] + L;
        bool past = (L >= 6 && (!strcmp(e-3,"ato") || !strcmp(e-3,"ito") || !strcmp(e-3,"uto"))) || !strcmp(e-2,"ed");
        if (!past) continue;
        for (int i = 0; pstem[i]; i++) if (!strncmp(tok[t], pstem[i], strlen(pstem[i]))) return true;
    }
    return false;
}

// Detect the create_file intent (IT+EN): a "create/new" verb AND a "file/note" noun.
static bool a_is_create_file(char tok[A_MAX_TOKENS][A_TOK_LEN], int ntok)
{
    if (a_action_is_statement(tok, ntok)) return false;   // "ho creato…", "non voglio creare…" -> not a command
    // A READ request ("leggimi il file…", "read me the file I will write tomorrow") is not a create — the
    // "write" is a subordinate/future clause, the actual verb is to read. Don't arm a create on it.
    static const char *const readv[] = { "leggi","leggimi","leggere","read","apri","aprire","mostra","mostrami","show","open", NULL };
    // Only the command part counts: "crea una nota con scritto APRI la porta" is a create, its text is data.
    for (int t = 0; t < ntok; t++) {
        if (!strcmp(tok[t], "scritto") || !strcmp(tok[t], "testo") || !strcmp(tok[t], "dice") ||
            !strcmp(tok[t], "saying") || !strcmp(tok[t], "contenuto")) break;
        for (int i = 0; readv[i]; i++) if (!strcmp(readv[i], tok[t])) return false;
    }
    static const char *verbs[] = { "crea", "creare", "crei", "nuovo", "nuova", "new", "create", "make",
                                   "scrivi", "write", "annota", "appunta", "segna", "prepara", "genera", "draft", "jot", NULL };
    static const char *nouns[] = { "file", "documento", "document", "nota", "note", "testo", "text", "foglio", "appunto", NULL };
    bool v = false, n = false;
    for (int t = 0; t < ntok; t++) {
        for (int i = 0; verbs[i]; i++) if (a_match(verbs[i], tok[t])) v = true;
        for (int i = 0; nouns[i]; i++) if (a_match(nouns[i], tok[t])) n = true;
    }
    return v && n;
}

// Pull a filename from the RAW input (the normalizer would strip the extension dot).
// Prefer a token with a dot; else the token after a trigger word; sanitize + force .txt.
static bool a_extract_filename(const char *raw, char *out, size_t outsz)
{
    char words[A_MAX_TOKENS][40]; int nw = 0, len = 0; char cur[40];
    for (const char *p = raw; ; p++) {
        char c = *p;
        if (c == ' ' || c == '\t' || c == 0) {
            if (len) { cur[len] = 0; if (nw < A_MAX_TOKENS) { memcpy(words[nw++], cur, len + 1); } len = 0; }
            if (!c) break;
        } else if (len < 39) cur[len++] = c;
    }
    static const char *trig[] = { "chiamato", "chiamata", "nome", "named", "called", "titolo", "title", NULL };
    int cand = -1, trig_at = -1;
    for (int i = 0; i < nw && cand < 0; i++) {
        bool hasdot = false, hasalpha = false;
        for (char *q = words[i]; *q; q++) { if (*q == '.') hasdot = true; if (isalpha((unsigned char)*q)) hasalpha = true; }
        if (hasdot && hasalpha) { cand = i; break; }
        char low[40]; int k = 0; for (char *q = words[i]; *q && k < 39; q++) low[k++] = (char)tolower((unsigned char)*q); low[k] = 0;
        for (int j = 0; trig[j]; j++) if (!strcmp(trig[j], low)) trig_at = i;
    }
    if (cand < 0 && trig_at >= 0 && trig_at + 1 < nw) cand = trig_at + 1;
    if (cand < 0) return false;
    int o = 0;
    for (char *q = words[cand]; *q && o < (int)outsz - 5; q++) {
        char c = *q;
        if (isalnum((unsigned char)c) || c == '.' || c == '_' || c == '-') out[o++] = c;
    }
    out[o] = 0;
    if (o == 0 || out[0] == '.') return false;
    if (!strchr(out, '.') && o < (int)outsz - 5) memcpy(out + o, ".txt", 5);   // default extension
    return true;
}


// ---------------------------------------------------------------------------
// The math/skills solver engine lives in anima_solve.c (extracted for scalability).
// Its entry points (anima_solve / a_try_calc / a_fmt_num / units_load) and the two
// symbols it borrows from here (a_norm_phrase / l0_legacy) are declared in
// anima_internal.h. See docs/anima.md �2.
// ---------------------------------------------------------------------------


// ============================================================================
// Agentic controller: a bounded, deterministic policy over a small FSM. No
// generation — the "intelligence" is state + tool dispatch + an uncertainty
// policy, layered on top of the L0/L1 cascade. All state is static (no malloc),
// O(1) RAM, so it fits the MCU. See docs/anima-controller.md.
// ============================================================================

#define ANIMA_RING 8           // working-memory window: last N turns (reference resolution)
#define ANIMA_CHAT 6           // online context window: last N (q,a) turns sent verbatim; older ones are compacted
#define ANIMA_REGS 8           // conversational math registers: user-named results across turns

// Session = decision-relevant conversational state. Persists across reboots on the SD.
static struct {
    char last_app[64];         // last launched app (sized to match result.arg)
    char last_file[64];        // last real file touched (routed paths: /data/<Folder>/<name>)
    char last_kind;            // 'a' app / 'f' file — recency tie-break for "aprilo"
    char last_topic[96];       // last knowledge query (for "tell me more")
    // FSM AWAITING_SLOT: a tool asked for a missing argument; next turn fills it.
    char pending_tool[16];     // "" = no pending slot
    char pending_slot[16];     // which argument we're waiting for ("filename" | "folder")
    char pending_arg[48];      // partial argument stashed between turns (e.g. the filename while
                               // we ask which folder it belongs in)
    // FSM CLARIFY: we offered an ambiguous choice; next turn may resolve by ordinal/name.
    char clarify_opt[2][32];   // L0 app clarify: the two app ids we proposed ("" = none pending)
    bool clarify_l1;           // L1 knowledge clarify pending (the dialogic band)
    long clarify_ans[2];       // the two AKB answer offsets offered, resolved by "1"/"2"
    char skill_clarify[32];    // knowledge<->skill clarify: the bare topic ("fisica") awaiting "spiegami" vs "calcola"
    // Working-memory ring (most recent ANIMA_RING turns). `input` is stored only for turns
    // that produced a real action (so "ripeti"/"again" can replay the last actionable one).
    struct { char input[64]; char intent[16]; char domain[12]; char arg[32]; } ring[ANIMA_RING];
    int  ring_head, ring_len;
    // Conversation transcript (q + reply) for the online teacher's MULTI-TURN context — the offline
    // ring above stores only inputs, which can't reconstruct a dialogue to send to Grok. RAM-only,
    // ~1.1 KB .bss; resolves running references ("divo 3?", "e lui?") several turns back, not just one.
    struct { char q[240]; char a[700]; } chat[ANIMA_CHAT];
    int  chat_head, chat_len;
    uint32_t turn;             // monotonic turn counter (telemetry)
    bool dirty;                // session changed since last persist
    // Last substantive answer, for dialogue acts ("sei sicuro?", "spiegati meglio"). A dialogue-act
    // turn does NOT overwrite this, so a meta-question always refers to the real previous answer.
    struct { char reply[200]; anima_tier_t tier; int conf; char intent[16]; } last;
    // Conversational FOCUS for the deductive tier: the (entity, relation) the KGE reasoner last anchored,
    // so a bare follow-up can re-aim it (swap entity or relation) without re-stating the other. RAM-only,
    // an 8-turn recency window via foc_turn (deterministic, no wall-clock); a structured token, never a
    // text fragment. See foc_template/foc_remember. Presence is foc_subject/foc_relation, never foc_turn.
    char foc_subject[48];
    char foc_relation[24];
    uint32_t foc_turn;
    // Conversational numeric memory for the math reasoning layer (anima_reason): the last computed
    // answer + a few user-named results ("chiamalo A"). RAM-only (NOT in the SD serializer), so a
    // reset wipes it; lets an offline cascade carry values across turns. See anima_reg_* below.
    struct { char name[12]; double val; bool used; } reg[ANIMA_REGS];
    double last_num; bool has_last;
} s_session EXT_RAM_BSS_ATTR;   // ~3 KB of cold session state: PSRAM, not internal RAM

#define s_mem s_session        // the old name still reads/writes the same fields

// Push the just-decided turn into the working-memory ring (newest at head). `input` is "" for
// non-actionable turns (a miss, a clarify question) so it never becomes a replay target.
static void ring_push(const char *input, const char *intent, const char *domain, const char *arg)
{
    int i = s_session.ring_head;
    snprintf(s_session.ring[i].input,  sizeof(s_session.ring[i].input),  "%s", input ? input : "");
    snprintf(s_session.ring[i].intent, sizeof(s_session.ring[i].intent), "%s", intent ? intent : "");
    snprintf(s_session.ring[i].domain, sizeof(s_session.ring[i].domain), "%s", domain ? domain : "");
    snprintf(s_session.ring[i].arg,    sizeof(s_session.ring[i].arg),    "%s", arg ? arg : "");
    s_session.ring_head = (i + 1) % ANIMA_RING;
    if (s_session.ring_len < ANIMA_RING) s_session.ring_len++;
}

// Most recent turn that produced a real action (newest first), or NULL. Drives "ripeti".
static const char *ring_last_input(void)
{
    for (int n = 0; n < s_session.ring_len; n++) {
        int i = (s_session.ring_head - 1 - n + ANIMA_RING * 2) % ANIMA_RING;
        if (s_session.ring[i].input[0]) return s_session.ring[i].input;
    }
    return NULL;
}

// Record a substantive (question, answer) pair into the online-context transcript (newest at head).
// Both must be non-empty — a miss/clarify carries no answer worth replaying to the teacher.
static void fold_add(const char *q, const char *a);   // context compaction, below
static bool s_ctx_dirty;
static void chat_push(const char *q, const char *a)
{
    if (!q || !q[0] || !a || !a[0]) return;
    int i = s_session.chat_head;
    // the ring is full: the oldest turn leaves the window -> it is folded into the summary, not lost
    if (s_session.chat_len == ANIMA_CHAT) fold_add(s_session.chat[i].q, s_session.chat[i].a);
    snprintf(s_session.chat[i].q, sizeof s_session.chat[i].q, "%s", q);
    snprintf(s_session.chat[i].a, sizeof s_session.chat[i].a, "%s", a);
    s_session.chat_head = (i + 1) % ANIMA_CHAT;
    if (s_session.chat_len < ANIMA_CHAT) s_session.chat_len++;
    s_ctx_dirty = true;
}

// Snapshot the recent transcript into `turns` (cap entries), OLDEST first, for the multi-turn online
// chat. Pointers alias the session ring — valid only until the next chat_push (i.e. within one turn).
// Returns the number of turns filled.
static int chat_context(anima_turn_t *turns, int cap)
{
    int n = s_session.chat_len < cap ? s_session.chat_len : cap;
    for (int k = 0; k < n; k++) {
        int i = (s_session.chat_head - n + k + ANIMA_CHAT * 2) % ANIMA_CHAT;
        turns[k].q = s_session.chat[i].q;
        turns[k].a = s_session.chat[i].a;
    }
    return n;
}


// ---- context compaction (Claude Code's /compact, auto-compact near the window's end) -------------
// The request carries [summary of the older conversation] + the last ANIMA_CHAT turns verbatim.
// A turn leaving the ring goes to the fold buffer; a compaction asks the model to merge
// (previous summary + folded turns [+ older ring turns]) into ONE structured summary, so long
// sessions keep their thread (goal, decisions, files, what failed) at a flat token cost.
// Auto: before a turn, when the last request filled >= ANIMA_COMPACT_PCT of the model's window,
// or when the fold buffer grew large. Manual: nucleo_anima_compact(focus).
#define ANIMA_SUM_CAP     2600               // the largest summary (a 64k+ window); small models get less
#define ANIMA_FOLD_CAP    6000
#define CONTEXT_PATH      NUCLEO_SD_MOUNT "/data/anima/context.json"   // summary + fold + window, across reboots
// The context file of the conversation in use: CONTEXT_PATH until the app opens a session
// (nucleo_anima_session_open), then that session's own context.json.
EXT_RAM_BSS_ATTR static char s_ctx_path_buf[112];   // "" = CONTEXT_PATH (a .bss section: no initializer)
static const char *ctx_path(void) { return s_ctx_path_buf[0] ? s_ctx_path_buf : CONTEXT_PATH; }
#define ANIMA_COMPACT_PCT 80
static char *s_csum;    // the rolling summary ("" = none yet)            } PSRAM, comp_mem(): 8.6 KB
static char *s_cfold;   // turns that left the ring, not summarized yet   } allocated on first use
static bool comp_mem(void)
{
    if (!s_csum) s_csum = heap_caps_calloc(1, ANIMA_SUM_CAP, MALLOC_CAP_SPIRAM);
    if (!s_cfold) s_cfold = heap_caps_calloc(1, ANIMA_FOLD_CAP, MALLOC_CAP_SPIRAM);
    return s_csum && s_cfold;
}
static volatile bool s_compacting;
static bool s_autocompact = true;
static anima_compact_info_t s_cinfo;

// Sized to the model's window: a cloud model with room keeps a richer summary and compacts less often;
// a small local model (8k) gets the tight one. Unknown window -> the small, safe values.
static int compact_sum_chars(void)
{
    int used = 0, max = 0;
    nucleo_anima_ctx_stats(&used, &max);
    return max >= 64000 ? 2400 : max >= 16000 ? 1400 : 800;
}
static size_t compact_fold_trigger(void)
{
    int used = 0, max = 0;
    nucleo_anima_ctx_stats(&used, &max);
    return max >= 64000 ? 4800 : max >= 16000 ? 3000 : 1600;
}

// The conversation the model sees (summary, fold, verbatim window) survives a reboot: context.json,
// rewritten through a temp file only when it changed.
static void ctx_save(void)
{
    if (!s_ctx_dirty || !comp_mem()) return;
    s_ctx_dirty = false;
    cJSON *o = cJSON_CreateObject();
    if (!o) return;
    cJSON_AddStringToObject(o, "sum", s_csum);
    cJSON_AddStringToObject(o, "fold", s_cfold);
    cJSON *a = cJSON_AddArrayToObject(o, "chat");
    for (int k = 0; a && k < s_session.chat_len; k++) {
        const int i = (s_session.chat_head - s_session.chat_len + k + ANIMA_CHAT * 2) % ANIMA_CHAT;
        cJSON *t = cJSON_CreateArray();
        cJSON_AddItemToArray(t, cJSON_CreateString(s_session.chat[i].q));
        cJSON_AddItemToArray(t, cJSON_CreateString(s_session.chat[i].a));
        cJSON_AddItemToArray(a, t);
    }
    char *txt = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);
    if (!txt) return;
    char tmp[sizeof s_ctx_path_buf + 4];
    snprintf(tmp, sizeof tmp, "%s.tmp", ctx_path());
    FILE *f = fopen(tmp, "w");
    if (f) { fputs(txt, f); a_commit_tmp(f, tmp, ctx_path()); }
    cJSON_free(txt);
}

static void ctx_load(void)
{
    if (!comp_mem()) return;
    FILE *f = fopen(ctx_path(), "r");
    if (!f) return;
    const size_t cap = ANIMA_SUM_CAP + ANIMA_FOLD_CAP + (size_t)ANIMA_CHAT * 1000 + 1024;
    char *buf = malloc(cap);
    const size_t n = buf ? fread(buf, 1, cap - 1, f) : 0;
    fclose(f);
    if (!buf) return;
    buf[n] = 0;
    cJSON *o = cJSON_Parse(buf);
    free(buf);
    if (!o) return;                                       // unreadable: start clean, never crash
    cJSON *sum = cJSON_GetObjectItem(o, "sum"), *fold = cJSON_GetObjectItem(o, "fold"), *chat = cJSON_GetObjectItem(o, "chat");
    if (cJSON_IsString(sum)) snprintf(s_csum, ANIMA_SUM_CAP, "%s", sum->valuestring);
    if (cJSON_IsString(fold)) snprintf(s_cfold, ANIMA_FOLD_CAP, "%s", fold->valuestring);
    s_session.chat_head = s_session.chat_len = 0;
    cJSON *t;
    if (cJSON_IsArray(chat)) cJSON_ArrayForEach(t, chat) {
        cJSON *q = cJSON_GetArrayItem(t, 0), *a = cJSON_GetArrayItem(t, 1);
        if (!cJSON_IsString(q) || !cJSON_IsString(a) || s_session.chat_len >= ANIMA_CHAT) continue;
        const int i = s_session.chat_len;
        snprintf(s_session.chat[i].q, sizeof s_session.chat[i].q, "%s", q->valuestring);
        snprintf(s_session.chat[i].a, sizeof s_session.chat[i].a, "%s", a->valuestring);
        s_session.chat_len++;
        s_session.chat_head = s_session.chat_len % ANIMA_CHAT;
    }
    cJSON_Delete(o);
}

static void fold_add(const char *q, const char *a)
{
    if (!comp_mem()) return;
    size_t n = strlen(s_cfold);
    const size_t need = strlen(q) + strlen(a) + 24;
    if (n + need >= ANIMA_FOLD_CAP) {            // full before a compaction could run: drop the oldest half
        const char *cut = strchr(s_cfold + ANIMA_FOLD_CAP / 2, '\n');
        if (cut) { memmove(s_cfold, cut + 1, strlen(cut + 1) + 1); n = strlen(s_cfold); }
        else { s_cfold[0] = 0; n = 0; }
    }
    snprintf(s_cfold + n, ANIMA_FOLD_CAP - n, "U: %s\nA: %s\n", q, a);
    s_ctx_dirty = true;
}

const char *nucleo_anima_session_summary(void) { return s_csum ? s_csum : ""; }
void nucleo_anima_set_autocompact(bool on) { s_autocompact = on; }
bool nucleo_anima_autocompact(void) { return s_autocompact; }
bool nucleo_anima_compacting(void) { return s_compacting; }
void nucleo_anima_compact_info(anima_compact_info_t *out) { if (out) *out = s_cinfo; }

// Fold everything but the last `keep` ring turns. 1 = compacted, 0 = nothing to do, -1 = failed.
static int compact_run(const char *focus, int keep, bool en)
{
    if (!comp_mem()) return -1;
    const int fold_turns = s_session.chat_len > keep ? s_session.chat_len - keep : 0;
    if (!s_cfold[0] && !fold_turns) return 0;
    if (!nucleo_anima_online_available() || !nucleo_anima_teacher_configured()) return -1;
    const size_t cap = ANIMA_SUM_CAP + ANIMA_FOLD_CAP + (size_t)ANIMA_CHAT * 960 + 512;
    char *text = malloc(cap);
    if (!text) return -1;
    size_t o = 0;
    if (s_csum[0]) o += snprintf(text + o, cap - o, "%s\n%s\n\n", en ? "PREVIOUS SUMMARY:" : "RIASSUNTO PRECEDENTE:", s_csum);
    o += snprintf(text + o, cap - o, "%s\n%s", en ? "CONVERSATION TO FOLD IN:" : "CONVERSAZIONE DA INCORPORARE:", s_cfold);
    for (int k = 0; k < fold_turns && o + 960 < cap; k++) {
        const int i = (s_session.chat_head - s_session.chat_len + k + ANIMA_CHAT * 2) % ANIMA_CHAT;
        o += snprintf(text + o, cap - o, "U: %s\nA: %s\n", s_session.chat[i].q, s_session.chat[i].a);
    }
    char sys[900];
    snprintf(sys, sizeof sys, en
        ? "You compact an assistant's conversation so it can continue without the full transcript. Write ONE summary, "
          "max %d characters, in English, as short labelled lines: Goal: ... | Done: ... | Decisions/preferences: ... | "
          "Files/paths/commands: ... (exact names) | Errors and fixes: ... | Open: next steps. Merge the previous summary; "
          "drop small talk; never invent. Output ONLY the summary.%s%s"
        : "Compatti la conversazione di un assistente perche' possa continuare senza il testo intero. Scrivi UN riassunto, "
          "max %d caratteri, in italiano, a righe brevi con etichetta: Obiettivo: ... | Fatto: ... | Decisioni/preferenze: ... | "
          "File/percorsi/comandi: ... (nomi esatti) | Errori e soluzioni: ... | Aperto: prossimi passi. Unisci il riassunto "
          "precedente; togli le chiacchiere; non inventare. Restituisci SOLO il riassunto.%s%s",
        compact_sum_chars(), focus && focus[0] ? (en ? " Focus on: " : " Concentrati su: ") : "", focus && focus[0] ? focus : "");
    s_compacting = true;
    char out[ANIMA_SUM_CAP + 8];
    const int rl = nucleo_anima_teacher_complete(sys, text, out, sizeof out);
    s_compacting = false;
    const int folded_chars = (int)o;
    free(text);
    if (rl <= 0) return -1;
    // keep only the last `keep` ring turns
    char (*kq)[240] = malloc(sizeof(char[240]) * ANIMA_CHAT), (*ka)[700] = malloc(sizeof(char[700]) * ANIMA_CHAT);
    int nk = 0;
    if (kq && ka) {
        for (int k = fold_turns; k < s_session.chat_len; k++) {
            const int i = (s_session.chat_head - s_session.chat_len + k + ANIMA_CHAT * 2) % ANIMA_CHAT;
            snprintf(kq[nk], 240, "%s", s_session.chat[i].q);
            snprintf(ka[nk], 700, "%s", s_session.chat[i].a);
            nk++;
        }
        memset(s_session.chat, 0, sizeof s_session.chat);
        s_session.chat_head = s_session.chat_len = 0;
        for (int k = 0; k < nk; k++) {
            snprintf(s_session.chat[k].q, sizeof s_session.chat[k].q, "%s", kq[k]);
            snprintf(s_session.chat[k].a, sizeof s_session.chat[k].a, "%s", ka[k]);
        }
        s_session.chat_head = nk % ANIMA_CHAT;
        s_session.chat_len = nk;
    }
    free(kq); free(ka);
    snprintf(s_csum, ANIMA_SUM_CAP, "%s", out);
    s_cfold[0] = 0;
    s_ctx_dirty = true;
    ctx_save();                                    // a manual /compact has no turn epilogue to save it
    s_cinfo.count++;
    s_cinfo.turns = fold_turns;
    s_cinfo.saved_tokens = (folded_chars - (int)strlen(s_csum)) / 4;
    if (s_cinfo.saved_tokens < 0) s_cinfo.saved_tokens = 0;
    nucleo_anima_ctx_saved(s_cinfo.saved_tokens);
    ESP_LOGI(TAG, "compacted: %d ring turns folded, summary %d chars, ~%d tokens saved",
             fold_turns, (int)strlen(s_csum), s_cinfo.saved_tokens);
    return 1;
}

int nucleo_anima_compact(const char *focus, bool en) { return compact_run(focus, 1, en); }

// Before a turn (under the spine gate): compact when the window is nearly full or the fold is big.
static void compact_auto(bool en)
{
    if (!s_autocompact) return;
    int used = 0, max = 0;
    nucleo_anima_ctx_stats(&used, &max);
    const bool full = max > 0 && (int64_t)used * 100 >= (int64_t)max * ANIMA_COMPACT_PCT && s_session.chat_len >= 2;
    if (full) compact_run(NULL, 1, en);
    else if (s_cfold && strlen(s_cfold) >= compact_fold_trigger()) compact_run(NULL, ANIMA_CHAT, en);
}

// --- conversational numeric registers (the math reasoning layer's working memory) ---------------
// Backed by s_session, so a /reset (memset) clears them and they never leak between conversations.
// Names are expected pre-normalized (lowercase, no spaces) by the caller — anima_reason always folds
// them through a_flat — so a plain strcmp suffices (no locale-dependent strcasecmp on the MCU).
int anima_reg_last(double *out) { if (!s_session.has_last) return 0; if (out) *out = s_session.last_num; return 1; }
void anima_reg_set_last(double val) { s_session.last_num = val; s_session.has_last = true; }
int anima_reg_get(const char *name, double *out)
{
    if (!name || !name[0]) return 0;
    for (int i = 0; i < ANIMA_REGS; i++)
        if (s_session.reg[i].used && !strcmp(s_session.reg[i].name, name)) { if (out) *out = s_session.reg[i].val; return 1; }
    return 0;
}
void anima_reg_set(const char *name, double val)
{
    if (!name || !name[0]) return;
    for (int i = 0; i < ANIMA_REGS; i++)                       // overwrite an existing binding
        if (s_session.reg[i].used && !strcmp(s_session.reg[i].name, name)) { s_session.reg[i].val = val; return; }
    for (int i = 0; i < ANIMA_REGS; i++)                       // else take a free slot
        if (!s_session.reg[i].used) { snprintf(s_session.reg[i].name, sizeof s_session.reg[i].name, "%s", name); s_session.reg[i].val = val; s_session.reg[i].used = true; return; }
    for (int i = 1; i < ANIMA_REGS; i++) s_session.reg[i-1] = s_session.reg[i];   // full: evict the oldest
    snprintf(s_session.reg[ANIMA_REGS-1].name, sizeof s_session.reg[ANIMA_REGS-1].name, "%s", name);
    s_session.reg[ANIMA_REGS-1].val = val; s_session.reg[ANIMA_REGS-1].used = true;
}

// "ripeti" / "di nuovo" / "again" / "repeat": replay the last action. Short inputs only, so it
// can't collide with a real request that merely contains the word ("crea di nuovo il file X").
static bool a_is_repeat(const char *input)
{
    char tok[A_MAX_TOKENS][A_TOK_LEN];
    int n = a_tokenize(input, tok);
    if (n == 0 || n > 3) return false;
    // "procedi"/"continua"/"vai avanti" also replay the last action — the user's "go ahead" after
    // ANIMA did something (the conversational confirm that makes it feel like an agent).
    static const char *solo[] = { "ripeti", "rifai", "again", "repeat",
                                  "procedi", "continua", "prosegui", "avanti", "proceed", "continue", NULL };
    for (int t = 0; t < n; t++)
        for (int i = 0; solo[i]; i++) if (!strcmp(solo[i], tok[t])) return true;
    bool di = false, nu = false;                 // "di nuovo"
    for (int t = 0; t < n; t++) { if (!strcmp(tok[t], "di")) di = true; if (!strcmp(tok[t], "nuovo")) nu = true; }
    return di && nu;
}

// "1"/"primo"/"first" -> 0, "2"/"secondo"/"second" -> 1, else -1 (resolving a clarify pick).
static int a_pick_ordinal(const char *input)
{
    char tok[A_MAX_TOKENS][A_TOK_LEN];
    int n = a_tokenize(input, tok);
    if (n == 0 || n > 3) return -1;
    for (int t = 0; t < n; t++) {
        if (!strcmp(tok[t],"1")||!strcmp(tok[t],"primo")||!strcmp(tok[t],"prima")||!strcmp(tok[t],"first")||!strcmp(tok[t],"uno")||!strcmp(tok[t],"one")) return 0;
        if (!strcmp(tok[t],"2")||!strcmp(tok[t],"secondo")||!strcmp(tok[t],"seconda")||!strcmp(tok[t],"second")||!strcmp(tok[t],"due")||!strcmp(tok[t],"two")) return 1;
    }
    return -1;
}

// Calendar agenda (computed-from-state): "che impegni ho oggi", "cosa devo fare oggi", "chi devo
// vedere oggi". The executor reads /system/config/calendar.json and filters by the RTC date.
static bool a_is_agenda(char tok[A_MAX_TOKENS][A_TOK_LEN], int ntok)
{
    static const char *nouns[] = { "impegni","impegno","appuntamenti","appuntamento","agenda",
                                   "eventi","scadenze","scadenza","appointments","schedule", NULL };
    // a capability question ("sai/puoi mettere eventi?") is NOT a request to SHOW the agenda.
    static const char *const cap[] = { "sai","puoi","riesci","potresti","sapresti","can","could","able","how","come", NULL };
    for (int t = 0; t < ntok; t++) for (int i = 0; cap[i]; i++) if (!strcmp(tok[t], cap[i])) return false;
    // The past and vague horizons ("la settimana SCORSA", "il giorno della mia NASCITA") are not in the
    // calendar's future: abstain instead of showing the wrong days. Future days are a_agenda_range's.
    static const char *const otherday[] = { "scorsa","scorso","scorse","scorsi","nascita","nato","nata",
        "passato","futuro","last","past","future","born","ieri","yesterday", NULL };
    for (int t = 0; t < ntok; t++) for (int i = 0; otherday[i]; i++) if (!strcmp(tok[t], otherday[i])) return false;
    bool devo = false, fv = false;
    for (int t = 0; t < ntok; t++) {
        for (int i = 0; nouns[i]; i++) if (a_match(nouns[i], tok[t])) return true;
        if (!strcmp(tok[t], "devo")) devo = true;
        if (!strcmp(tok[t],"fare")||!strcmp(tok[t],"vedere")||!strcmp(tok[t],"ricordare")) fv = true;
    }
    return devo && fv;   // "cosa devo fare oggi", "chi devo vedere oggi"
}

// Which days an agenda question covers, from today: oggi (0,1), domani (1,1), dopodomani (2,1), a named
// weekday (its next occurrence, 1 day), "questa settimana" (0,7), "la prossima settimana" (next Monday,7).
static void a_agenda_range(char tok[A_MAX_TOKENS][A_TOK_LEN], int ntok, int *from, int *days)
{
    *from = 0; *days = 1;
    time_t now = time(NULL); struct tm lt; localtime_r(&now, &lt);
    static const char *const WD[7][2] = { {"domenica","sunday"}, {"lunedi","monday"}, {"martedi","tuesday"},
        {"mercoledi","wednesday"}, {"giovedi","thursday"}, {"venerdi","friday"}, {"sabato","saturday"} };
    bool next = false, week = false;
    for (int t = 0; t < ntok; t++) {
        if (!strcmp(tok[t],"prossima")||!strcmp(tok[t],"prossimo")||!strcmp(tok[t],"next")) next = true;
        if (!strcmp(tok[t],"settimana")||!strcmp(tok[t],"week")) week = true;
    }
    if (week) {
        if (next) { *from = (8 - lt.tm_wday) % 7; if (*from == 0) *from = 7; }   // next Monday
        *days = 7;
        return;
    }
    for (int t = 0; t < ntok; t++) {
        if (!strcmp(tok[t],"domani")||!strcmp(tok[t],"tomorrow")) { *from = 1; return; }
        if (!strcmp(tok[t],"dopodomani")) { *from = 2; return; }
        for (int d = 0; d < 7; d++)
            if (!strcmp(tok[t], WD[d][0]) || !strcmp(tok[t], WD[d][1])) { *from = (d - lt.tm_wday + 7) % 7; return; }
    }
}

// "What can you do" -> the DYNAMIC capabilities answer (executor builds it from the live app
// registry + the pillars), not a hardcoded list. Distinct from "sai cosa e X" (no "fare" -> L1).
static bool a_is_capabilities(char tok[A_MAX_TOKENS][A_TOK_LEN], int ntok)
{
    // EXACT match (not fuzzy a_match): these are distinctive command words; fuzzy made "elefante"~"elenca"
    // and "adulto"~"abilita" answer "what I can do". Typos are handled upstream by the command spellfix.
    static const char *const kw[] = { "aiuto","help","comandi","funzioni","capacita","elenca","skill","skills","abilita","capabilities", NULL };
    for (int t = 0; t < ntok; t++) for (int i = 0; kw[i]; i++) if (!strcmp(kw[i], tok[t])) return true;
    // "cosa sai FARE" / "che puoi FARE" / "cosa riesci a FARE": the ability verb must DIRECTLY follow the
    // modal (at most one "a"/"da" between). Without adjacency, "SAI dirmi FAI la somma" (a polite lead-in
    // plus an unrelated imperative) false-fired capabilities and leaked the {value} template.
    static const char *const modal[] = { "sai","puoi","sapresti","riesci","potresti", NULL };
    for (int t = 0; t < ntok; t++) {
        bool md = false; for (int i = 0; modal[i]; i++) if (!strcmp(modal[i], tok[t])) md = true;
        if (!md) continue;
        if (t + 1 < ntok && (!strcmp(tok[t+1],"fare")||!strcmp(tok[t+1],"fai"))) return true;
        if (t + 2 < ntok && (!strcmp(tok[t+1],"a")||!strcmp(tok[t+1],"da")) && (!strcmp(tok[t+2],"fare")||!strcmp(tok[t+2],"fai"))) return true;
    }
    return false;
}

// Network status (computed-from-state): "sei connesso?", "a che wifi sei?", "che IP hai?". The
// executor reads the live Wi-Fi mode/SSID/IP. Must NOT steal the knowledge question "cos'e il
// wifi" (-> L1): a topic word (wifi/rete) alone never fires — it needs a status/possessive
// signal AND no definition word; the strong words (connesso/ip…) fire on their own.
static bool a_is_network(char tok[A_MAX_TOKENS][A_TOK_LEN], int ntok)
{
    static const char *strong[] = { "connesso","connessi","connessa","connessione","collegato",
                                    "collegata","collegati","online","connected","ip", NULL };
    static const char *net[]    = { "wifi","rete","internet","network", NULL };
    // NB no bare "che": with "che" any "in CHE anno internet si spegnerà" looked like a status query.
    static const char *state[]  = { "sei","sono","siamo","mia","mio","qual","quale","uso","usi","sto","attuale", NULL };
    static const char *def[]    = { "cos","cosa","spiega","significa","vuol","definizione","differenza","funziona","serve", NULL };
    // NB no "questo/this" nor the to-be verbs "sono/sei/siamo": "persone SONO connesse", "uno due tre SEI"
    // (sei = six) would falsely self-anchor a foreign network question. Only true first-person possessives.
    static const char *const self[] = { "mio","mia","miei","mie","ho","nostro","nostra","my","our", NULL };
    // "il TUO indirizzo ip" is the device's own too, but only next to a strong word: "il tuo sito internet
    // preferito" is no status query.
    static const char *const you[]  = { "tuo","tua","tuoi","your", NULL };
    bool s = false, nt = false, st = false, d = false, poss = false, slf = false, yu = false;
    for (int t = 0; t < ntok; t++) {
        for (int i = 0; you[i];    i++) if (!strcmp(you[i],    tok[t])) yu = true;
        for (int i = 0; def[i];    i++) if (a_match(def[i],    tok[t])) d  = true;
        for (int i = 0; strong[i]; i++) if (a_match(strong[i], tok[t])) s  = true;
        for (int i = 0; net[i];    i++) if (a_match(net[i],    tok[t])) nt = true;
        for (int i = 0; state[i];  i++) if (a_match(state[i],  tok[t])) st = true;
        for (int i = 0; self[i];   i++) if (!strcmp(self[i],   tok[t])) slf = true;
        // a possessive toward a FOREIGN thing ("l'indirizzo ip DEL paradiso") is not a self-status query
        if ((!strcmp(tok[t],"del")||!strcmp(tok[t],"della")||!strcmp(tok[t],"dello")||
             !strcmp(tok[t],"dei")||!strcmp(tok[t],"delle")||!strcmp(tok[t],"of")) && t + 1 < ntok) poss = true;
    }
    if (d) return false;            // it's a definition question -> let L1 answer
    if (poss && !slf) return false; // "ip del paradiso" / "rete della luna" -> not the device's own network
    // A device-status query is SHORT or self-anchored. A long sentence that merely contains "internet"/
    // "connesse" ("quante persone sono connesse a internet in questo istante") is a different question.
    bool concise = slf || ntok <= 5;
    return (s && (concise || yu)) || (nt && st && concise);   // "che ip", "sei connesso" | "qual e la mia rete"
}

// RAM status (computed-from-state): "quanta RAM libera?", "memoria disponibile?". The executor
// reads the live free heap. Must NOT steal "alloco memoria"/"cos'e la RAM" (-> L1): a memory word
// needs an availability signal (libera/disponibile/quanta…) and no definition/allocation word.
static bool a_is_ram(char tok[A_MAX_TOKENS][A_TOK_LEN], int ntok)
{
    static const char *mem[]   = { "ram","memoria", NULL };
    static const char *avail[] = { "libera","libero","liberi","disponibile","disponibili","occupata",
                                   "occupato","usata","resta","rimane","quanta","free", NULL };
    static const char *def[]   = { "cos","cosa","spiega","significa","differenza","alloco","alloca",
                                   "allocare","allocazione","dinamica","heap","stack","serve", NULL };
    bool m = false, a = false, d = false;
    for (int t = 0; t < ntok; t++) {
        for (int i = 0; def[i];   i++) if (a_match(def[i],   tok[t])) d = true;
        for (int i = 0; mem[i];   i++) if (a_match(mem[i],   tok[t])) m = true;
        for (int i = 0; avail[i]; i++) if (a_match(avail[i], tok[t])) a = true;
    }
    if (d) return false;
    return m && a;                  // "quanta ram libera", "memoria disponibile"
}

// Is this a SELF question (about ANIMA), not "chi è <someone-else>"? The whoami intent keys on
// "chi"/"who", which alone hijack "chi è Einstein" / "chi è X" away from the knowledge/teacher
// tier. Require a second-person/self token so only "chi sei (tu)", "come ti chiami", "who are
// you", "presentati", "chi è anima" match; a third-person entity ("chi è X") falls through.
// HOST-ONLY A/B switch: L0_LEGACY=1 reverts THIS round's L0 over-triggering guards (whoami tightening,
// a_ambient_ok, a_solve_date long-incidental) to pre-hardening behaviour, so a before/after diff proves
// the change is a strict improvement. Compiled out of the device build entirely.
bool l0_legacy(void)
{
#ifdef ANIMA_HOST
    return getenv("L0_LEGACY") != NULL;
#else
    return false;
#endif
}

// Genuine self/identity question? The whoami intent keys on bare "chi"/"who"/"you", which appear in
// countless non-identity queries ("do you love me", "what do you think", "who won the cup"). Measured
// against a 311-query OOS set those produced 12 false "Sono ANIMA" answers. So require the identity to
// be the SUBJECT: an interrogative bound to a self-reference (chi sei / who are you / cosa sei / sei
// anima), or "presentati". A bare "you/tu" in a longer sentence is incidental -> falls through to L1.
static bool a_whoami_self(char tok[A_MAX_TOKENS][A_TOK_LEN], int ntok)
{
    if (l0_legacy()) {   // A/B: pre-hardening lax behaviour (any "you/tu/sei/..." token fires whoami)
        static const char *self[] = { "sei","siete","tu","te","ti","tuo","tua","presentati",
                                      "anima","chiami","you","your","yourself", NULL };
        for (int t=0;t<ntok;t++) for (int i=0;self[i];i++) if (!strcmp(self[i],tok[t])) return true;
        return false;
    }
    bool chi=false, who=false, what=false, are=false, you=false, sei=false,
         anima=false, presentati=false, cosa=false, parl=false;
    for (int t = 0; t < ntok; t++) {
        const char *w = tok[t];
        if      (!strncmp(w,"parl",4))                      parl = true;   // "con chi sto parlando/parlo"
        else if (!strcmp(w,"chi"))                          chi = true;
        else if (!strcmp(w,"who"))                          who = true;
        else if (!strcmp(w,"what"))                         what = true;
        else if (!strcmp(w,"cosa") || !strcmp(w,"cos"))     cosa = true;
        else if (!strcmp(w,"sei") || !strcmp(w,"siete"))  { sei = true; are = true; }
        else if (!strcmp(w,"are") || !strcmp(w,"r"))        are = true;
        else if (!strcmp(w,"you")||!strcmp(w,"tu")||!strcmp(w,"te")||!strcmp(w,"ti")||!strcmp(w,"your")) you = true;
        else if (!strcmp(w,"anima"))                        anima = true;
        else if (!strcmp(w,"presentati"))                   presentati = true;
    }
    if (presentati) return true;
    if (anima && (sei||you||chi||are||cosa||what||who)) return true;   // "sei anima","chi e anima","are you anima"
    if (chi && (sei||anima||parl))                      return true;   // "chi sei","chi e anima","con chi parlo" (not "chi...you" = song "shape of you")
    if ((who||what) && you && are)                      return true;   // "who/what are you"
    if (cosa && sei)                                    return true;   // "cosa sei"
    return false;
}

// Is `w` an interrogative/quantity question word that, before a keyword, marks it as the question's
// subject ("che ora", "what year", "quanta batteria")?  ("come"/"how" excluded — too broad.)
static bool a_qword(const char *w)
{
    static const char *q[] = { "che","qual","quale","quanta","quanto","quante","quanti",
                               "what","which","when","quando", NULL };
    for (int i = 0; q[i]; i++) if (!strcmp(q[i], w)) return true;
    return false;
}

// Playback control: 1 = pause ("pausa", "metti in pausa la musica", "pause the song"), 2 = resume
// ("riprendi la musica", "continua la canzone", "resume", a bare "riprendi"), 0 = neither. "continua"
// alone is the conversation ("go on"), so with it resuming needs a media word.
static int a_media_ctl(char tok[A_MAX_TOKENS][A_TOK_LEN], int ntok)
{
    static const char *const media[] = { "musica","canzone","brano","riproduzione","traccia","audio",
                                         "music","song","track","playback", NULL };
    bool pause = false, res = false, resume_en = false, md = false;
    for (int t = 0; t < ntok; t++) {
        if (a_qword(tok[t]) || !strcmp(tok[t], "come") || !strcmp(tok[t], "how")) return 0;
        if (!strcmp(tok[t], "pausa") || !strcmp(tok[t], "pause") || !strcmp(tok[t], "sospendi")) pause = true;
        if (!strcmp(tok[t], "riprendi") || !strcmp(tok[t], "continua") || !strcmp(tok[t], "riparti") ||
            !strcmp(tok[t], "continue")) res = true;
        if (!strcmp(tok[t], "resume") || !strcmp(tok[t], "unpause")) resume_en = true;
        for (int i = 0; media[i]; i++) if (!strcmp(media[i], tok[t])) md = true;
    }
    if (ntok > 5) return 0;
    if (pause && !res && !resume_en) return 1;
    if (resume_en || (res && md) || (ntok == 1 && !strcmp(tok[0], "riprendi"))) return 2;
    return 0;
}

// Back to the launcher: "torna alla home", "portami alla schermata principale", "home", "go home",
// "chiudi tutto" (the shell runs one app at a time, so closing everything IS the home screen). Home
// Assistant, the ~/home folder and a web home page are other things; "spegni tutto" is the lights.
// A question ("come torno alla home?") is left to the how-to path.
static bool a_is_go_home(char tok[A_MAX_TOKENS][A_TOK_LEN], int ntok)
{
    static const char *const nav[]   = { "torna","tornare","torniamo","vai","andare","andiamo","portami","riportami",
                                         "mostra","mostrami","go","back","return","take","show", NULL };
    static const char *const other[] = { "assistant","cartella","folder","file","directory","pagina","page","sito",
                                         "site","web","automation","automazione", NULL };
    static const char *const all[]   = { "tutto","tutte","tutti","everything","all", NULL };
    static const char *const closev[] = { "chiudi","chiudere","close","esci","exit","quit", NULL };
    bool home = false, sch = false, princ = false, nv = false, oth = false, al = false, cl = false;
    for (int t = 0; t < ntok; t++) {
        if (a_qword(tok[t]) || !strcmp(tok[t], "come") || !strcmp(tok[t], "how")) return false;
        if (!strcmp(tok[t], "home") || !strcmp(tok[t], "launcher")) home = true;
        if (!strcmp(tok[t], "schermata") || !strcmp(tok[t], "screen")) sch = true;
        if (!strcmp(tok[t], "principale") || !strcmp(tok[t], "iniziale") || !strcmp(tok[t], "main")) princ = true;
        for (int i = 0; nav[i];    i++) if (!strcmp(nav[i],    tok[t])) nv  = true;
        for (int i = 0; other[i];  i++) if (!strcmp(other[i],  tok[t])) oth = true;
        for (int i = 0; all[i];    i++) if (!strcmp(all[i],    tok[t])) al  = true;
        for (int i = 0; closev[i]; i++) if (!strcmp(closev[i], tok[t])) cl  = true;
    }
    if (oth) return false;
    // Names an app: that is a plan, not "home". "screen" is also the Second Screen alias, but in
    // "the home screen" it belongs to "home".
    const char *app = a_resolve_app(tok, ntok);
    if (app && !(home && !strcmp(app, "secondscreen"))) return false;
    if (cl && al && ntok <= 4) return true;                    // "chiudi tutto", "close all apps"
    const bool target = home || (sch && princ);
    return target && (nv || ntok <= 2) && ntok <= 7;           // "home", "la home", "torna alla home"
}

// A PURE ambient time question ("che ora è", "in che anno siamo", "what year is it") names ONLY the
// time frame plus fillers (to-be, lead-ins, articles, "now/today"). A CONTENT word — a verb other than
// to-be, or a foreign entity — REFRAMES it into a different question the device-clock cannot answer:
//   "in che anno MORIRÀ javascript"   "a che ora TRAMONTA il sole su PLUTONE"   "che ore sono ad ATLANTIDE"
// The past-tense fact-verb guard in a_ambient_ok only caught "morì/nato/…"; future/other verbs and
// trailing entities slipped through the "che anno"/"che ora" adjacency. Any content token => not ambient.
static bool a_temporal_reframed(char tok[A_MAX_TOKENS][A_TOK_LEN], int ntok)
{
    static const char *const ok[] = {
        // to-be / aux / pronouns
        "e","sono","siamo","sei","siete","sta","stai","stiamo","is","are","am","be","been","it","we","you","there","s",
        // interrogatives (IT a_qword covers most; EN ones are listed explicitly so they aren't "content")
        "what","which","who","whom","whose","when","where","why","how",
        // lead-ins ("dimmi che ora è", "sai che giorno è", "mi dici l'anno", "ricordami che ore sono")
        "dimmi","dim","dici","dirmi","dammi","sai","sapere","puoi","potresti","mostra","mostrami","fammi","mi","ti",
        "tell","know","can","could","would","please","show","give","me","let","ricordami","remind",
        // time frame + now/today (the ambient vocabulary ITSELF never counts as a reframe — every keyword
        // of the date/time/year/season intents must appear here, or the keyword would look "foreign")
        "oggi","adesso","ora","ore","orario","orari","time","clock","attuale","corrente","attualmente","momento",
        "now","today","currently","right","moment","current","anno","anni","annata","year","years","stagione",
        "stagioni","season","seasons","data","date","giorno","giorni","day","days","settimana","week","oggigiorno","weekday",
        // articles / prepositions / conjunctions / fillers
        "il","lo","la","i","gli","le","un","uno","una","l","di","del","dello","della","dei","degli","delle",
        "in","a","ad","al","allo","alla","ai","agli","alle","nel","nello","nella","su","sul","per","qui","che",
        "the","of","at","on","to","here","this","della","and", NULL };
    for (int t = 0; t < ntok; t++) {
        const char *w = tok[t];
        if (strlen(w) <= 3) continue;                               // short tokens (fillers, typos like "sno") never reframe
        bool isnum = true; for (const char *c = w; *c; c++) if (!isdigit((unsigned char)*c)) { isnum = false; break; }
        if (isnum) continue;                                        // a bare number is not a reframing entity
        if (a_qword(w)) continue;
        bool filler = false;
        for (int i = 0; ok[i]; i++) if (!strcmp(ok[i], w)) { filler = true; break; }
        if (!filler) return true;                                   // a content word -> reframed, not ambient
    }
    return false;
}

// Ambient SYSTEM intents (date/time/year/season/battery) answer from device state and key on very
// common words (time/year/oggi/giorno). They must fire only when the query is ACTUALLY about that —
// the keyword carries the question, not just appears in it. Measured against a 311-query OOS set these
// over-fired 35 times ("favorite band of all time", "this year", "ciao come stai oggi"). Accept only:
// a short query (keyword dominates), >=2 of the intent's keywords, an interrogative bound to a keyword,
// or a qword present in a <=4-word query. Otherwise the keyword is incidental -> let higher tiers run.
static bool a_ambient_ok(const a_intent_t *it, char tok[A_MAX_TOKENS][A_TOK_LEN], int ntok, int best_score)
{
    // A reframing content word turns a time-readout into a different (often unanswerable) question.
    // Applies to the clock/calendar readouts only — battery keeps its own looser shape ("quanta carica
    // mi resta" legitimately carries the content verb "resta").
    if ((!strcmp(it->id,"date") || !strcmp(it->id,"time") || !strcmp(it->id,"year") || !strcmp(it->id,"season"))
        && a_temporal_reframed(tok, ntok)) return false;
    // A historical FACT verb ("in che anno MORÌ/NATO/ELETTO X") makes this a fact question about an
    // event, NOT the current year/date/season -> never an ambient state answer. Without this, "in che
    // anno morì Obama" answered "Siamo nel 2026" and "in che anno è nato Einstein" missed the HDC fact.
    static const char *const factv[] = { "nato","nata","nascita","morto","morta","mori","deceduto",
        "eletto","eletta","fondato","fondata","costruito","inventato","scritto","scoperto","regnato",
        "born","died","death","elected","founded","built","invented","wrote","discovered","reigned", NULL };
    for (int t = 0; t < ntok; t++) for (int i = 0; factv[i]; i++) if (!strcmp(factv[i], tok[t])) return false;
    // <=2 (not <=3): a 3-word statement with one incidental ambient keyword ("ho fame oggi", "sono triste
    // oggi") must NOT fire the date/time readout. Real 3-word date/time questions carry a qword ("che ora
    // è") or 2+ keywords ("data di oggi"), both handled below.
    if (ntok <= 2 || best_score >= 2) return true;
    bool has_q = false;
    for (int t = 0; t < ntok; t++) {
        if (a_qword(tok[t])) has_q = true;
        for (int k = 0; k < A_MAX_KW && it->kw[k]; k++)
            if (a_match(it->kw[k], tok[t]) && t > 0 && a_qword(tok[t-1])) return true;  // "che ora","what year"
    }
    // Short question with an interrogative ("in che anno siamo"). MUST rest on an EXACT keyword, not a
    // fuzzy prefix collision: "annoia"~"anno" made "quando ANIMA si annoia" answer the current year.
    // Real inflections (giorno->giornata) still pass via the qword-adjacent a_match path above.
    // ...or a short explicit ask ("dimmi l'ora", "mi dici la data", "tell me the time"): the request
    // verb carries the question as a qword would.
    static const char *const askv[] = { "dimmi","dici","dammi","tell","give", NULL };
    for (int t = 0; t < ntok && !has_q; t++) for (int i = 0; askv[i]; i++) if (!strcmp(askv[i], tok[t])) has_q = true;
    if (!has_q || ntok > 4) return false;
    for (int t = 0; t < ntok; t++)
        for (int k = 0; k < A_MAX_KW && it->kw[k]; k++)
            if (!strcmp(it->kw[k], tok[t])) return true;
    return false;
}

// "spazio" is AMBIGUOUS: device disk space vs PHYSICAL space ("quanto spazio occupa il Pacifico/Sahara/la
// Russia" hit the SD-storage reading on dozens of geography cards). The storage reading fires only with a
// device-storage context (memoria/disco/sd present, or libero/ho/disponibile/rimasto), never on a physical-
// occupation question. memoria/disco/sd are unambiguous and always pass.
static bool a_storage_ok(char tok[A_MAX_TOKENS][A_TOK_LEN], int ntok)
{
    bool unamb = false, ctx = false, occupa = false;
    for (int t = 0; t < ntok; t++) {
        const char *w = tok[t];
        // "disco" must be EXACT (+ plural): the fuzzy a_match made "Discord" -> "disco" -> SD-space reading
        // ("nome utente Discord di Cleopatra" answered "Spazio SD: …"). disk/storage/partizion stay fuzzy.
        if (a_match("memoria", w) || !strcmp(w,"disco") || !strcmp(w,"dischi") || a_match("scheda", w)
            || !strcmp(w, "sd") || !strcmp(w,"disk") || !strcmp(w,"disks") || a_match("storage", w)
            || a_match("partizion", w)) unamb = true;   // disco/disk EXACT: fuzzy matched "Discord" -> SD space
        if (!strcmp(w,"libero")||!strcmp(w,"liberi")||!strcmp(w,"disponibile")||!strcmp(w,"rimasto")||
            !strcmp(w,"rimasti")||!strcmp(w,"resta")||!strcmp(w,"occupato")||!strcmp(w,"ho")||!strcmp(w,"rimane")||
            !strcmp(w,"free")||!strcmp(w,"left")||!strcmp(w,"used")||!strcmp(w,"available")) ctx = true;
        if (!strcmp(w,"occupa")||!strcmp(w,"occupano")||!strcmp(w,"copre")||!strcmp(w,"coprono")||
            !strcmp(w,"misura")||!strcmp(w,"estende")||!strcmp(w,"occupies")||!strcmp(w,"covers")) occupa = true;
    }
    if (unamb) return true;                 // memoria/disco/sd/partizione named -> a real storage question
    if (occupa) return false;               // "quanto spazio OCCUPA il Pacifico" -> physical space, not disk
    if (ntok <= 2) {                        // "spazio libero", "quanta memoria" — but only on a REAL storage
        for (int t = 0; t < ntok; t++) {    // word, not a fuzzy false friend: "Discord"~"disco" drops to 1
            const char *w = tok[t];         // token after stopword removal and slipped through as ntok<=2.
            if (!strcmp(w,"spazio")||!strcmp(w,"space")||!strcmp(w,"disco")||!strcmp(w,"dischi")||
                !strcmp(w,"disk")||!strcmp(w,"memoria")||!strcmp(w,"sd")||!strcmp(w,"scheda")||
                !strcmp(w,"storage")||!strcmp(w,"capacita")||!strcmp(w,"gigabyte")||!strcmp(w,"archiviazione"))
                return true;
        }
        return false;
    }
    return ctx;                             // bare "spazio" in a longer query needs a device-storage context
}

// "versione" answers the DEVICE firmware version. It must NOT steal a knowledge question about some
// OTHER software's version ("la versione di python che uscirà nel tremila" -> unknowable/L1), which
// merely contains the word "versione". Fire only when the query is anchored to the device/self (hai/usi/
// sei/è, or firmware/sistema/os/nucleoos/anima), and never when it names another software with "di X".
static bool a_version_ok(char tok[A_MAX_TOKENS][A_TOK_LEN], int ntok)
{
    // NB no generic "this/questo": "in questo istante" must not anchor version (it also fuzzy-collides,
    // "persone"~"versione"). A short version question is allowed by length; a long one needs a real anchor.
    static const char *const self[] = { "hai","usi","sei","gira","giri","esegui","tua","tuo",
        "firmware","sistema","os","nucleoos","nucleo","anima","dispositivo","device","running","your", NULL };
    bool anchored = false, foreign = false;
    for (int t = 0; t < ntok; t++) {
        for (int i = 0; self[i]; i++) if (!strcmp(self[i], tok[t])) anchored = true;
        // "versione di <something>" with a following content noun -> it's about that something, not us.
        if ((!strcmp(tok[t],"di") || !strcmp(tok[t],"of")) && t + 1 < ntok) foreign = true;
    }
    if (foreign) return false;              // "versione di python …" -> about that software, not the device
    if (ntok <= 4) return true;             // short: "che versione", "what version is this"
    return anchored;                        // longer query needs an explicit device anchor
}

// uptime keys on "acceso/avvio/uptime". The command-vocab SPELLFIX rewrites "ultimo" (in "il mio ultimo
// giorno di vita") to "uptime", defeating an exact-match guard — so ALSO require the query to be SHORT:
// a genuine uptime question ("da quanto sei acceso", "uptime") is brief, a reframed sentence is not.
static bool a_uptime_ok(char tok[A_MAX_TOKENS][A_TOK_LEN], int ntok)
{
    if (ntok > 5) return false;
    static const char *const kw[] = { "uptime","acceso","accesa","accesi","accese","avvio","avviato",
                                      "avviata","boot","booted", NULL };
    for (int t = 0; t < ntok; t++) for (int i = 0; kw[i]; i++) if (!strcmp(kw[i], tok[t])) return true;
    return false;
}

// "aprilo" / "open it" / "apri quello": an open verb + a pronoun (or the explicit
// inflected form). Distinct from "apri spotify" (a named, unknown app -> not a follow-up).
static bool a_is_followup_open(char tok[A_MAX_TOKENS][A_TOK_LEN], int ntok)
{
    static const char *fu[]    = { "aprilo", "aprila", "riaprilo", "riaprila", NULL };
    static const char *openv[] = { "apri", "aprire", "open", "mostra", "show", "riapri", NULL };
    static const char *pron[]  = { "lo", "la", "quello", "quella", "esso", "essa",
                                   "questo", "questa", "it", "that", "this", NULL };
    bool fuv = false, opn = false, prn = false;
    for (int t = 0; t < ntok; t++) {
        for (int i = 0; fu[i]; i++)    if (!strcmp(fu[i], tok[t]))    fuv = true;
        for (int i = 0; openv[i]; i++) if (a_match(openv[i], tok[t])) opn = true;
        for (int i = 0; pron[i]; i++)  if (!strcmp(pron[i], tok[t]))  prn = true;
    }
    // "la"/"lo" are also ARTICLES: "apri la calcolatrice" names its app, it is no "aprila" follow-up
    // (it reopened the last app instead).
    return fuv || (opn && prn && !a_resolve_app(tok, ntok));
}

// "chiudilo" / "close it" — the close mirror of a_is_followup_open. The pronoun refers to the last app.
static bool a_is_followup_close(char tok[A_MAX_TOKENS][A_TOK_LEN], int ntok)
{
    static const char *fu[]     = { "chiudilo", "chiudila", "spegnilo", "spegnila", NULL };
    static const char *closev[] = { "chiudi", "chiudere", "close", "esci", "termina", NULL };
    static const char *pron[]   = { "lo", "la", "quello", "quella", "esso", "essa",
                                    "questo", "questa", "it", "that", "this", NULL };
    bool fuv = false, cv = false, prn = false;
    for (int t = 0; t < ntok; t++) {
        for (int i = 0; fu[i]; i++)     if (!strcmp(fu[i], tok[t]))     fuv = true;
        for (int i = 0; closev[i]; i++) if (a_match(closev[i], tok[t])) cv = true;
        for (int i = 0; pron[i]; i++)   if (!strcmp(pron[i], tok[t]))   prn = true;
    }
    return fuv || (cv && prn && !a_resolve_app(tok, ntok));   // "chiudi la musica" names its app
}

// Drill-down follow-up: "dimmi di più" / "tell me more" / "fammi un esempio" — the user wants
// more on the LAST knowledge topic. Single tokens that are unambiguous, or a "piu"/"more"-style
// word paired with a request verb (so "voglio di più ___" works but a bare "more music" doesn't).
// A request to GENERATE code / a programming snippet — only the online model can write code, so the
// cascade routes these straight to Grok (with a code prompt) instead of a Wikipedia bio / L1 card / a
// "tell me more" drill-down. HIGH precision via WHOLE-TOKEN match (so "chi ha creato python" — a
// knowledge question — does NOT trigger): a strong code noun on its own, OR a language name together
// with a generation verb. "cos'è python" stays a knowledge query (lang but no generation verb).
static bool a_is_code_request(const char *input)
{
    char tok[A_MAX_TOKENS][A_TOK_LEN];
    int n = a_tokenize(input, tok);
    if (n == 0) return false;
    static const char *lang[] = { "python","javascript","typescript","golang","kotlin","react",
                                  "java","rust","php","swift","bash","sql","html","css","cpp", NULL };
    static const char *gen[]  = { "scrivi","scrivimi","scrivere","scriva","dammi","fammi","mostrami",
                                  "genera","generami","crea","creami","esempio","esempi","example",
                                  "examples","write","generate","snippet","voglio", NULL };
    static const char *code[] = { "codice","code","script","snippet","programma","algoritmo",
                                  "algorithm","pseudocodice", NULL };
    bool hasLang = false, hasGen = false;
    for (int t = 0; t < n; t++) {
        for (int i = 0; code[i]; i++) if (!strcmp(code[i], tok[t])) return true;   // code noun -> code request
        for (int i = 0; lang[i]; i++) if (!strcmp(lang[i], tok[t])) hasLang = true;
        for (int i = 0; gen[i];  i++) if (!strcmp(gen[i],  tok[t])) hasGen  = true;
    }
    return hasLang && hasGen;
}

static bool a_is_more_request(const char *input)
{
    char tok[A_MAX_TOKENS][A_TOK_LEN];
    int n = a_tokenize(input, tok);
    if (n == 0) return false;
    if (a_is_code_request(input)) return false;   // "(dammi un) esempio di codice python" is a code request, not a drill-down
    static const char *solo[] = { "approfondisci", "approfondire", "dettagli", "dettaglio",
                                  "elaborate", "esempio", "example", "continua", NULL };
    static const char *more[] = { "piu", "more", NULL };                  // "di più", "more"
    static const char *verb[] = { "dimmi", "dammi", "raccontami", "dicci", "sai", "spiegami",
                                  "fammi", "tell", "give", "show", "explain", "voglio", NULL };
    bool m = false, v = false;
    for (int t = 0; t < n; t++) {
        for (int i = 0; solo[i]; i++) if (!strcmp(solo[i], tok[t])) return true;
        for (int i = 0; more[i]; i++) if (!strcmp(more[i], tok[t])) m = true;
        for (int i = 0; verb[i]; i++) if (a_match(verb[i], tok[t])) v = true;
    }
    return m && v;
}

static void mem_update(const anima_result_t *r)
{
    if (r->action == ANIMA_ACT_LAUNCH && r->arg[0]) {
        if (strcmp(s_mem.last_app, r->arg) != 0) s_session.dirty = true;
        snprintf(s_mem.last_app, sizeof(s_mem.last_app), "%s", r->arg); s_mem.last_kind = 'a';
    }
    // create_file is NOT remembered here (it isn't created yet, and may be auth-blocked or
    // refused-if-exists). The executor calls nucleo_anima_note_file() once the file is real.
}

// Record a file as the current context (called by the executor after a real create / on
// an already-existing file), so a follow-up "aprilo" opens the right thing.
void nucleo_anima_note_file(const char *path)
{
    if (path && path[0]) {
        snprintf(s_mem.last_file, sizeof(s_mem.last_file), "%s", path);
        s_mem.last_kind = 'f'; s_session.dirty = true;
    }
}

// Close the agentic loop: the executor tells us how the last action really went.
void nucleo_anima_observe(const char *intent, bool ok)
{
    // A blocked/failed create must not leave a dangling pending slot or stale "current file".
    if (!ok && intent && strcmp(intent, "create_file") == 0) {
        s_session.pending_tool[0] = 0; s_session.pending_slot[0] = 0;
    }
}

// ---- session persistence (survives reboot; written only on change) ----------
static void session_save(void)
{
    if (!s_session.dirty) return;
    FILE *f = fopen(SESSION_PATH ".tmp", "wb");      // tmp + rename: a cut write never loses the session
    if (!f) { s_session.dirty = false; return; }      // SD absent: don't retry every turn
    // The topic is user text (a web POST can carry newlines): one line, or the next boot parses the
    // rest as app= / file= keys.
    char topic[sizeof s_mem.last_topic];
    snprintf(topic, sizeof topic, "%s", s_mem.last_topic);
    for (char *p = topic; *p; p++) if ((unsigned char)*p < 0x20) *p = ' ';
    fprintf(f, "app=%s\nfile=%s\nkind=%c\ntopic=%s\n", s_mem.last_app, s_mem.last_file,
            s_mem.last_kind ? s_mem.last_kind : '-', topic);
    a_commit_tmp(f, SESSION_PATH ".tmp", SESSION_PATH);
    s_session.dirty = false;
}

static void session_load(void)
{
    FILE *f = fopen(SESSION_PATH, "rb");
    if (!f) return;
    char line[160];
    while (fgets(line, sizeof(line), f)) {
        char *nl = strchr(line, '\n'); if (nl) *nl = 0;
        if      (!strncmp(line, "app=", 4))   snprintf(s_mem.last_app, sizeof(s_mem.last_app), "%.*s", (int)sizeof(s_mem.last_app) - 1, line + 4);
        else if (!strncmp(line, "file=", 5))  snprintf(s_mem.last_file, sizeof(s_mem.last_file), "%.*s", (int)sizeof(s_mem.last_file) - 1, line + 5);
        else if (!strncmp(line, "kind=", 5))  s_mem.last_kind = (line[5] && line[5] != '-') ? line[5] : 0;
        else if (!strncmp(line, "topic=", 6)) snprintf(s_mem.last_topic, sizeof(s_mem.last_topic), "%.*s", (int)sizeof(s_mem.last_topic) - 1, line + 6);
    }
    fclose(f);
}

// Set when a reset couldn't get the gate: the next query (which owns the gate) applies it.
static atomic_bool s_reset_pending = false;

static void session_reset_locked(void)
{
    memset(&s_session, 0, sizeof(s_session));
    if (s_csum) s_csum[0] = 0;                    // a new conversation: no summary to carry
    if (s_cfold) s_cfold[0] = 0;
    remove(ctx_path());
    s_ctx_dirty = false;
    s_session.dirty = true;
    session_save();
}

void nucleo_anima_reset_session(void)
{
    // Called from the UI thread; a query may be running on a worker and reading s_session (and
    // writing session.txt). Take the spine gate (bounded wait) so the reset can't tear it.
    bool locked = false;
    for (int i = 0; i < 100 && !(locked = nucleo_anima_try_lock()); i++) vTaskDelay(pdMS_TO_TICKS(10));   // <= 1 s
    if (!locked) { atomic_store(&s_reset_pending, true); return; }   // never touch s_session unlocked
    atomic_store(&s_reset_pending, false);
    session_reset_locked();
    nucleo_anima_unlock();
}

static bool s_inited;   // one-shot: the engine is brought up exactly once per boot (init below)

bool nucleo_anima_session_open(const char *path)
{
    if (!path || !path[0] || strlen(path) >= sizeof s_ctx_path_buf) return false;
    bool locked = false;
    for (int i = 0; i < 100 && !(locked = nucleo_anima_try_lock()); i++) vTaskDelay(pdMS_TO_TICKS(10));   // <= 1 s
    if (!locked) return false;
    if (s_inited && strcmp(path, ctx_path())) {
        ctx_save();                                    // the conversation we leave keeps its file
        memset(&s_session, 0, sizeof(s_session));      // working memory belongs to that conversation
        if (s_csum) s_csum[0] = 0;
        if (s_cfold) s_cfold[0] = 0;
        s_ctx_dirty = false;
        snprintf(s_ctx_path_buf, sizeof s_ctx_path_buf, "%s", path);
        ctx_load();
        s_session.dirty = true;
        session_save();
    } else {
        snprintf(s_ctx_path_buf, sizeof s_ctx_path_buf, "%s", path);   // init (or a reopen) loads this one
    }
    nucleo_anima_unlock();
    return true;
}

// Derive the routing "domain" of a result (mirrors the executor's view; used by telemetry).
static const char *a_domain(const anima_result_t *r)
{
    if (!strcmp(r->intent, "clarify")) return "clarify";   // before FACT: an L1 clarify is tier FACT
    if (r->tier == ANIMA_TIER_FACT) return "knowledge";
    if (!strcmp(r->intent, "calc") || !strcmp(r->intent, "base") ||
        !strcmp(r->intent, "prime") || !strcmp(r->intent, "roman") ||
        !strcmp(r->intent, "geo") || !strcmp(r->intent, "phys")) return "calc";
    switch (r->action) {
        case ANIMA_ACT_TOOL:   return "tool";
        case ANIMA_ACT_SYSTEM: return "system";
        case ANIMA_ACT_LAUNCH: return "app";
        case ANIMA_ACT_ANSWER: return "faq";
        default:               return "none";
    }
}

// ---- routing telemetry: the offline-learning work-list (energy-aware) -------
// Append only the queries worth studying: knowledge hits (L1, already paid the SD read) and
// honest misses. Cheap L0 commands are skipped to keep the "L0 does zero SD I/O" principle.
// The file is bounded (rotated past TELEMETRY_CAP); a write failure is silently ignored.
static void telemetry_log(const char *q, const anima_result_t *r, const char *domain)
{
    if (r->tier == ANIMA_TIER_COMMAND) return;
    FILE *f = fopen(TELEMETRY_PATH, "ab");
    if (!f) return;
    fseek(f, 0, SEEK_END);
    if (ftell(f) > TELEMETRY_CAP) { fclose(f); f = fopen(TELEMETRY_PATH, "wb"); if (!f) return; }
    fprintf(f, "{\"t\":%u,\"q\":\"", (unsigned)s_session.turn);
    // JSON string escaping: control bytes too (a POSTed query with a newline broke the NDJSON line).
    for (const char *p = q; *p && p < q + 80; p++) {
        const unsigned char c = (unsigned char)*p;
        if (c == '"' || c == '\\') { fputc('\\', f); fputc(c, f); }
        else if (c < 0x20) fprintf(f, "\\u%04x", c);
        else fputc(c, f);
    }
    // Every tier by name: REMOTE / STITCH answers were logged as "none" (misses) in the work-list.
    const char *tier = r->tier == ANIMA_TIER_FACT   ? "fact"
                     : r->tier == ANIMA_TIER_STITCH ? "stitch"
                     : r->tier == ANIMA_TIER_REMOTE ? "remote" : "none";
    fprintf(f, "\",\"tier\":\"%s\",\"intent\":\"%s\",\"domain\":\"%s\",\"conf\":%d,\"budget\":%d}\n",
            tier, r->intent, domain, r->confidence, r->budget);
    fclose(f);
}

// ============================================================================
// REASONING TRACE + CONTENT CHANNEL — the agent's visible "thought log" and the
// payload bus that lets a tool carry more than the 64-byte arg field. Both are
// tiny module-static buffers, reset per query: no heap, MCU-frugal (the trace is
// rendered ONCE into the result at the end, not threaded through every tier).
// The trace turns the single-pass cascade into a Claude-Code-style multi-step view
// both UIs render; the content channel is what makes "compose THEN act" possible.
// ============================================================================
EXT_RAM_BSS_ATTR static char s_trace[112];                  // steps taken this turn, " > " joined (ASCII: the device font has no middot)
static void trace_reset(void) { s_trace[0] = 0; }
static void trace_step(const char *step)
{
    if (!step || !*step) return;
    size_t n = strlen(s_trace);
    if (n) { for (const char *s = " > "; *s && n < sizeof(s_trace) - 1; s++) s_trace[n++] = *s; s_trace[n] = 0; }
    snprintf(s_trace + n, sizeof(s_trace) - n, "%s", step);
}

#define AG_CONTENT_MAX 200
EXT_RAM_BSS_ATTR static char s_tool_content[AG_CONTENT_MAX]; // composed payload for the next side-effect tool ("" = none)
static void content_reset(void) { s_tool_content[0] = 0; }
// Set while a compound request's clauses are tried one by one (turn_gate): tools that write or fetch on
// their own (teach, profile, translate) stay out, so a dry run leaves nothing behind.
static bool s_dry_run;
const char *nucleo_anima_tool_content(void) { return s_tool_content; }

// Overflow channel for a reply too long for result.reply[1024] — e.g. a multi-line CODE snippet from
// the online model. online_code stashes the FULL text here on the HEAP (not the fixed struct buffer,
// so a long answer never grows the stack), and the web handler serves it via cJSON (which handles long
// strings). Lives only between the query that sets it and the next (single-threaded httpd) -> 0 RAM idle.
static char *s_long_reply = NULL;
const char *nucleo_anima_long_reply(void) { return s_long_reply; }
void nucleo_anima_set_long_reply(const char *s) {
    free(s_long_reply); s_long_reply = NULL;
    if (s && s[0]) s_long_reply = strdup(s);
}

// ---- tool registry (typed function-calling) ---------------------------------
// Each tool owns a turn when its detector fires, filling the result with a real call, an
// ask-for-slot, or a refusal. New tools are one row — the cascade no longer hard-codes them.
typedef struct {
    const char *name;
    bool        side_effect;   // true -> the executor (httpd) runs it under auth
    int (*try_fn)(const char *raw, char tok[A_MAX_TOKENS][A_TOK_LEN], int ntok, bool en, anima_result_t *r);
} a_tool_t;

// Route a filename to its NucleoOS data folder by extension (mirrors registry/file-
// associations.json: the extension's default app implies the folder). Returns the folder name,
// or NULL if the extension is unknown — the caller then ASKS which folder (never guesses).
static const char *a_folder_for_ext(const char *name)
{
    const char *dot = strrchr(name, '.');
    if (!dot || !dot[1]) return "Documents";     // no extension -> a text note by default
    char ext[12]; int i = 0;
    for (const char *p = dot + 1; *p && i < 11; p++) ext[i++] = (char)tolower((unsigned char)*p);
    ext[i] = 0;
    static const char *docs[]  = { "txt","md","log","json","csv","ini","cfg","yaml","yml","toml",
                                   "xml","html","htm","c","h","cpp","hpp","py","sh","js","css","todo", NULL };
    static const char *pics[]  = { "jpg","jpeg","png","bmp","gif","webp", NULL };
    static const char *music[] = { "mp3","wav","ogg","m4a","flac", NULL };
    static const char *video[] = { "mp4","webm","mov","mkv","avi", NULL };
    for (int j = 0; docs[j];  j++) if (!strcmp(ext, docs[j]))  return "Documents";
    for (int j = 0; pics[j];  j++) if (!strcmp(ext, pics[j]))  return "Pictures";
    for (int j = 0; music[j]; j++) if (!strcmp(ext, music[j])) return "Music";
    for (int j = 0; video[j]; j++) if (!strcmp(ext, video[j])) return "Videos";
    return NULL;
}

// Map a user's folder word (IT/EN) to a real data folder, or NULL.
static const char *a_folder_from_words(char tok[A_MAX_TOKENS][A_TOK_LEN], int ntok)
{
    for (int t = 0; t < ntok; t++) {
        const char *w = tok[t];
        if (!strcmp(w,"documenti")||!strcmp(w,"documento")||!strcmp(w,"documents")||!strcmp(w,"doc")||!strcmp(w,"testo")) return "Documents";
        if (!strcmp(w,"immagini")||!strcmp(w,"immagine")||!strcmp(w,"foto")||!strcmp(w,"pictures")||!strcmp(w,"images")||!strcmp(w,"pics")) return "Pictures";
        if (!strcmp(w,"musica")||!strcmp(w,"music")||!strcmp(w,"audio")||!strcmp(w,"brani")) return "Music";
        if (!strcmp(w,"video")||!strcmp(w,"videos")||!strcmp(w,"filmati")) return "Videos";
    }
    return NULL;
}

// Build the create_file result for a known filename: route it to a folder, or (unknown
// extension) arm the AWAITING_SLOT(folder) state and ask. `folder` overrides routing (used
// when the user just told us the folder). Always produces a /data/<Folder>/<name> path.
static void a_emit_create(const char *name, const char *folder, bool en, anima_result_t *r)
{
    r->tier = ANIMA_TIER_COMMAND;
    snprintf(r->intent, sizeof(r->intent), "create_file");
    if (!folder) folder = a_folder_for_ext(name);
    if (!folder) {                                // unknown extension -> ask which folder
        r->action = ANIMA_ACT_ANSWER; r->awaiting = 1; r->confidence = 70;
        snprintf(r->state, sizeof(r->state), "slot");
        snprintf(r->reply, sizeof(r->reply),
                 en ? "Which folder should %s go in? (Documents, Pictures, Music, Videos)"
                    : "In quale cartella metto %s? (Documenti, Immagini, Musica, Video)", name);
        snprintf(s_session.pending_tool, sizeof(s_session.pending_tool), "create_file");
        snprintf(s_session.pending_slot, sizeof(s_session.pending_slot), "folder");
        snprintf(s_session.pending_arg,  sizeof(s_session.pending_arg),  "%s", name);
        return;
    }
    r->action = ANIMA_ACT_TOOL; r->confidence = 90;
    snprintf(r->state, sizeof(r->state), "tool");
    snprintf(r->arg, sizeof(r->arg), "/data/%s/%s", folder, name);
    snprintf(r->reply, sizeof(r->reply), en ? "Creating %s in %s." : "Creo %s in %s.", name, folder);
}

// Is an interrogative present? Used to keep how-to QUESTIONS ("come si crea un file?") out of the
// imperative create tool — they're knowledge, not an action. Exact match (these are short words).
static bool a_has_qword(char tok[A_MAX_TOKENS][A_TOK_LEN], int ntok)
{
    static const char *q[] = { "come", "cosa", "perche", "quando", "dove", "quale",
                               "how", "what", "why", "when", "where", "which",
                               // capability questions ("posso/puoi creare un file?") are not commands
                               "posso", "puoi", "potresti", "puo", "can", "could", "may", NULL };
    for (int t = 0; t < ntok; t++) for (int i = 0; q[i]; i++) if (!strcmp(q[i], tok[t])) return true;
    return false;
}

// Tool: create_file. Extracts the filename from the RAW input; if absent it arms the
// AWAITING_SLOT(filename) state and asks (never invents a name). Then routes by extension.
static int tool_create_file(const char *raw, char tok[A_MAX_TOKENS][A_TOK_LEN], int ntok, bool en, anima_result_t *r)
{
    if (!a_is_create_file(tok, ntok)) return 0;
    char name[40];
    if (a_extract_filename(raw, name, sizeof(name))) {
        a_emit_create(name, NULL, en, r);
    } else {
        // No explicit filename AND the input is a question ("come si crea un file?") -> not a create
        // command but a how-to; let it fall through to L1 knowledge instead of asking for a name.
        if (a_has_qword(tok, ntok)) return 0;
        r->tier = ANIMA_TIER_COMMAND; r->action = ANIMA_ACT_ANSWER; r->awaiting = 1;
        snprintf(r->intent, sizeof(r->intent), "create_file");
        snprintf(r->state, sizeof(r->state), "slot");
        snprintf(r->reply, sizeof(r->reply), en ? "What should the file be called? E.g. \"create a file note.txt\"."
                                               : "Come vuoi chiamare il file? Es: \"crea un file note.txt\".");
        r->confidence = 70;
        snprintf(s_session.pending_tool, sizeof(s_session.pending_tool), "create_file");
        snprintf(s_session.pending_slot, sizeof(s_session.pending_slot), "filename");
    }
    return 1;
}

// Tool: calc. Owns the turn when the input is a real arithmetic expression.
static int tool_calc(const char *raw, char tok[A_MAX_TOKENS][A_TOK_LEN], int ntok, bool en, anima_result_t *r)
{
    (void)tok; (void)ntok;
    double cv; int ck = a_try_calc(raw, &cv);
    if (!ck) return 0;
    r->tier = ANIMA_TIER_COMMAND; r->action = ANIMA_ACT_ANSWER; r->confidence = 95;
    snprintf(r->intent, sizeof(r->intent), "calc");
    snprintf(r->state, sizeof(r->state), "tool");
    if (ck == 2) snprintf(r->reply, sizeof(r->reply), en ? "I can't divide by zero." : "Non posso dividere per zero.");
    else { char num[40]; a_fmt_round(cv, num, sizeof(num)); snprintf(r->reply, sizeof(r->reply), en ? "It's %s." : "Fa %s.", num); }   // calcolo base -> max 4 dec
    return 1;
}

// Tool: the unified MATH agent — ONE transversal skill the orchestrator calls for ANY
// calculation. Tries the exact solver (percent / Ohm's law / units / powers / roots / abs /
// modulo) then plain arithmetic as the fallback; returns 1 when it answered exactly (the
// number is computed, never guessed), 0 to let the retrieval cascade take over.
static int tool_math(const char *raw, char tok[A_MAX_TOKENS][A_TOK_LEN], int ntok, bool en, anima_result_t *r)
{
    // anima_reason runs FIRST: it owns the conversational/multi-step layer (chains, named registers,
    // self-description) which must orchestrate the single-shot solver, not be pre-empted by it. It reads
    // the previous turn's last-answer (set below), so the order — reason → solve → calc — also keeps the
    // register-read before the register-write within a turn.
    int ok = anima_reason(raw, en, r) || anima_solve(raw, en, r) || tool_calc(raw, tok, ntok, en, r);
    if (ok) { double v; if (a_reply_lastnum(r->reply, &v)) anima_reg_set_last(v); }   // remember for "chiamalo A" / "× 2"
    return ok ? 1 : 0;
}

// ---- agent loop: compose THEN act -------------------------------------------
// The leap from a one-shot Q&A to a deterministic micro-agent: a tool may run a few internal
// reasoning STEPS (compute / take literal text) that fill the content channel, VERIFY the payload,
// then propose ONE side-effect action carrying it. Each step is logged to the trace so both UIs show
// the agent thinking — Claude-Code-style — while staying 100% deterministic (no generation).

// Is `w` a naming word ("con nome X" = filename spec, NOT content)? Lowercased ASCII expected.
static bool a_is_naming(const char *w)
{
    static const char *naming[] = { "nome", "chiamato", "chiamata", "titolo", "title", "named", "called", NULL };
    for (int i = 0; naming[i]; i++) if (!strcmp(w, naming[i])) return true;
    return false;
}

// Content-clause modes — how the agent should turn the clause into a file body:
//   AG_LITERAL   copy the text verbatim   ("con scritto X", "che dice X", ":")
//   AG_AUTO      calc, else if it's a QUESTION answer it from the brain, else literal  (bare "con X")
//   AG_RETRIEVE  always answer the sub from the brain  ("con la risposta a X", "con la definizione di X")
enum { AG_LITERAL = 0, AG_AUTO = 1, AG_RETRIEVE = 2 };

// SAFETY: is this input worth escalating to the ONLINE/knowledge tiers (entity, Wikidata, teacher)?
// A bare DATE (24:04:2027), number, time, cell-ref, code, or wordless/symbol string is NOT a question:
// it must NEVER be sent to the cloud teacher (which would fabricate a "fact") nor learned. Heuristic:
// it must contain a real word — a run of >=3 letters (accented UTF-8 bytes count as letters). Pure
// digits/separators/symbols (dates, numbers, "A1:B10", "x7gq2", "??") have no such run -> not askable.
// The deterministic L0/L1/math tiers still run first; this only fences off the fabrication-prone tiers.
static bool a_is_askable(const char *q)
{
    while (*q == ' ' || *q == '\t') q++;
    int n = (int)strlen(q);
    while (n > 0 && (q[n-1] == ' ' || q[n-1] == '\t' || q[n-1] == '\n' || q[n-1] == '\r')) n--;
    if (n < 3) return false;
    int run = 0, max_run = 0;
    for (int i = 0; i < n; i++) {
        unsigned char c = (unsigned char)q[i];
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c >= 0x80) { if (++run > max_run) max_run = run; }
        else run = 0;
    }
    return max_run >= 3;
}

// Does the sub-clause read as a QUESTION / lookup (so the agent should ANSWER it) rather than literal
// text to copy? Conservative by design (zero-false-write): the cue must be at the START (after at most
// one leading article) — "cos'e X", "la capitale di X", "chi e X" answer; literal text that merely
// CONTAINS a question word later ("la tua risposta a come...", "le cose da fare") stays literal.
static bool a_is_question(const char *sub)
{
    char tok[A_MAX_TOKENS][A_TOK_LEN]; int n = a_tokenize(sub, tok);
    if (n == 0) return false;
    static const char *art[] = { "la", "il", "lo", "i", "gli", "le", "un", "uno", "una", "l", "the", "a", "an", NULL };
    int s = 0;
    for (int i = 0; art[i]; i++) if (!strcmp(art[i], tok[0])) { s = 1; break; }   // skip one leading article
    static const char *cue[] = { "cos", "cosa", "chi", "quale", "quali", "quando", "dove", "come",
        "perche", "quanti", "quanto", "spiega", "significa", "significato", "definizione", "capitale",
        "what", "who", "which", "when", "where", "how", "why", "define", "meaning", "capital", NULL };
    for (int t = s; t < n && t <= s + 1; t++)         // only the first content token (+ one, for "capitale di X")
        for (int i = 0; cue[i]; i++) if (a_match(cue[i], tok[t])) return true;
    return false;
}

// Find a content clause in a create-file request and return the payload (a pointer INTO `raw`, so its
// case is preserved), or NULL. Sets *mode (LITERAL / AUTO / RETRIEVE). Explicit text connectors win
// ("con scritto X", "che dice X", ":"); "con la risposta/definizione di X" asks the brain; bare "con X"
// is content only when not a naming clause ("con nome X").
static const char *a_content_clause(const char *raw, int *mode)
{
    *mode = AG_LITERAL;
    const char *colon = strchr(raw, ':');                 // explicit "...: rest" wins outright (literal)
    if (colon && colon[1]) { const char *c = colon + 1; while (*c == ' ') c++; if (*c) return c; }

    const char *start[A_MAX_TOKENS]; char low[A_MAX_TOKENS][24]; int n = 0;
    for (const char *p = raw; *p && n < A_MAX_TOKENS; ) {
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) break;
        start[n] = p; int k = 0;
        while (*p && *p != ' ' && *p != '\t') {
            unsigned char uc = (unsigned char)*p;
            if (uc == 0xC3 && p[1]) {                       // fold IT accents so "venerdì" matches WD "venerdi"
                unsigned char d = (unsigned char)p[1];
                char f = (d>=0xA0&&d<=0xA2)?'a':(d>=0xA8&&d<=0xAA)?'e':(d>=0xAC&&d<=0xAE)?'i':(d>=0xB2&&d<=0xB4)?'o':(d>=0xB9&&d<=0xBB)?'u':0;
                if (f) { if (k < 23) low[n][k++] = f; p += 2; continue; }
            }
            if (k < 23) low[n][k++] = (char)tolower(uc); p++;
        }
        low[n][k] = 0; n++;
    }
#define TOK_AT(j) (((j) < n) ? start[j] : NULL)
    for (int i = 0; i < n; i++) {
        const char *w = low[i], *n1 = (i + 1 < n) ? low[i + 1] : "", *n2 = (i + 2 < n) ? low[i + 2] : "";
        const char *n3 = (i + 3 < n) ? low[i + 3] : "";
        // RETRIEVE connectors (answer the sub from the offline brain): "con la risposta/definizione a/di X"
        if (!strcmp(w, "con") && !strcmp(n1, "la") && (!strcmp(n2, "risposta") || !strcmp(n2, "definizione")) &&
            (!strcmp(n3, "a") || !strcmp(n3, "di") || !strcmp(n3, "su"))) { *mode = AG_RETRIEVE; return TOK_AT(i + 4); }
        if (!strcmp(w, "con") && (!strcmp(n1, "risposta") || !strcmp(n1, "definizione")) &&
            (!strcmp(n2, "a") || !strcmp(n2, "di") || !strcmp(n2, "su"))) { *mode = AG_RETRIEVE; return TOK_AT(i + 3); }
        if (!strcmp(w, "rispondendo") && !strcmp(n1, "a")) { *mode = AG_RETRIEVE; return TOK_AT(i + 2); }
        if (!strcmp(w, "answering")) { *mode = AG_RETRIEVE; return TOK_AT(i + 1); }
        // 3-token LITERAL connectors (longest match wins at this position)
        if (!strcmp(w, "con")  && !strcmp(n1, "il")  && !strcmp(n2, "testo")) return TOK_AT(i + 3);
        if (!strcmp(w, "with") && !strcmp(n1, "the") && !strcmp(n2, "text"))  return TOK_AT(i + 3);
        // 2-token LITERAL connectors
        if (!strcmp(w, "con")  && (!strcmp(n1, "scritto") || !strcmp(n1, "testo") || !strcmp(n1, "dentro"))) return TOK_AT(i + 2);
        if (!strcmp(w, "che")  && (!strcmp(n1, "dice") || !strcmp(n1, "contiene") || !strcmp(n1, "recita"))) return TOK_AT(i + 2);
        if (!strcmp(w, "with") && (!strcmp(n1, "text") || !strcmp(n1, "content"))) return TOK_AT(i + 2);
        if (!strcmp(w, "that") && (!strcmp(n1, "says") || !strcmp(n1, "reads"))) return TOK_AT(i + 2);
        // 1-token LITERAL connectors
        if (!strcmp(w, "scritto") || !strcmp(w, "contenente") || !strcmp(w, "dicendo") ||
            !strcmp(w, "saying") || !strcmp(w, "containing"))
            return TOK_AT(i + 1);
        // bare connectors -> AUTO (calc / answer-if-question / literal)
        if (!strcmp(w, "con"))  { if (a_is_naming(n1)) continue; *mode = AG_AUTO; return TOK_AT(i + 1); }
        if (!strcmp(w, "with")) { *mode = AG_AUTO; return TOK_AT(i + 1); }
    }
    return NULL;
#undef TOK_AT
}

// Compose a tool payload from a sub-query — the agent's per-step skill dispatch:
//   1) a CALCULATION -> compute it          (math sub-skill, exact)
//   2) AUTO+question or RETRIEVE -> ANSWER it from the offline brain (L1, conf-gated -> zero fabrication)
//   3) otherwise -> literal text
// This is the "research then produce" step: ANIMA reads its own knowledge and writes the answer to a
// file. Logs each step to the visible trace. Pure offline, no generation.
#define AG_RECALL_MINCONF 78    // a retrieved fact must be confident before it's written to a FILE (reliability)
static bool ag_compose(const char *sub, int mode, bool en, char *out, size_t outsz)
{
    while (*sub == ' ') sub++;
    if (!*sub) return false;
    if (mode != AG_LITERAL) {                          // "con scritto X" stays verbatim; else compute/answer
        double v; int ck = a_try_calc(sub, &v);
        if (ck == 1) {
            char num[40]; a_fmt_num(v, num, sizeof(num));
            snprintf(out, outsz, "%s", num);
            char step[56]; snprintf(step, sizeof(step), en ? "compute %.24s=%s" : "calcolo %.24s=%s", sub, num);
            trace_step(step);
            return true;
        }
        // full math agent (percent / units / roots / powers / Ohm), not just basic arithmetic
        anima_result_t mr; memset(&mr, 0, sizeof(mr));
        if (anima_solve(sub, en, &mr) && mr.reply[0]) {
            snprintf(out, outsz, "%s", mr.reply);
            trace_step(en ? "solve" : "risolvo");
            return true;
        }
        if (mode == AG_RETRIEVE || (mode == AG_AUTO && a_is_question(sub))) {
            anima_result_t kr; memset(&kr, 0, sizeof(kr));
            // STRUCTURED deduction first (KGE/HDC, edge-grounded -> precise, zero fabrication): lets the
            // agent WRITE a *deduced* fact ("la capitale della Francia" -> Parigi) that L1 alone, which
            // only stores cards, does not hold. The reasoner's own coherence guards reject a wrong aim.
            bool ok = nucleo_anima_hdc_reason(sub, en ? "en" : "it", &kr) && kr.reply[0];
            // else High-confidence L1 retrieval: a permanent file must never hold a borderline guess.
            if (!ok) { memset(&kr, 0, sizeof(kr));
                ok = nucleo_anima_l1_query(sub, en, false, &kr) && kr.reply[0] && kr.confidence >= AG_RECALL_MINCONF; }
            if (!ok && !a_is_question(sub)) {                 // a bare term -> ask the question form ("cos'e X")
                char qf[120]; snprintf(qf, sizeof(qf), en ? "what is %.100s" : "cos'e %.100s", sub);
                memset(&kr, 0, sizeof(kr));
                ok = nucleo_anima_l1_query(qf, en, false, &kr) && kr.reply[0] && kr.confidence >= AG_RECALL_MINCONF;
            }
            if (ok) {
                snprintf(out, outsz, "%s", kr.reply);
                char step[48]; snprintf(step, sizeof(step), en ? "recall: %.20s" : "cerco: %.20s", sub);
                trace_step(step);
                return true;
            }
            trace_step(en ? "unsure -> literal" : "incerto -> letterale");   // never write a guessed fact
        }
    }
    snprintf(out, outsz, "%s", sub);
    trace_step(en ? "literal text" : "testo letterale");
    return true;
}

// Tool: compose-then-act note. Fires on a create-file request that ALSO carries a content clause —
// runs the internal plan (split filename | content, compose the payload, self-verify) and proposes a
// create_file action with the payload on the content channel. Returns 0 (no content clause) so the
// plain empty-file tool_create_file handles the legacy case. The flagship of the agent loop.
static int tool_note(const char *raw, char tok[A_MAX_TOKENS][A_TOK_LEN], int ntok, bool en, anima_result_t *r)
{
    if (!a_is_create_file(tok, ntok)) return 0;
    int mode; const char *content = a_content_clause(raw, &mode);
    if (!content) return 0;
    trace_step(en ? "plan: compose+write" : "piano: componi+scrivi");

    char head[160]; size_t hl = (size_t)(content - raw);  // text before the connector -> filename scan
    if (hl >= sizeof(head)) hl = sizeof(head) - 1;
    memcpy(head, raw, hl); head[hl] = 0;
    char name[40];
    if (!a_extract_filename(head, name, sizeof(name)))
        snprintf(name, sizeof(name), en ? "note.txt" : "nota.txt");   // sensible agent default

    char body[AG_CONTENT_MAX];
    if (!ag_compose(content, mode, en, body, sizeof(body)) || !body[0]) {
        trace_step(en ? "abort: empty payload" : "annullo: payload vuoto");
        return 0;                                          // nothing to write -> fall through
    }
    snprintf(s_tool_content, sizeof(s_tool_content), "%s", body);
    trace_step(en ? "verify: ok" : "verifica: ok");

    a_emit_create(name, NULL, en, r);                      // routes by extension, sets ACT_TOOL + path
    if (r->action == ANIMA_ACT_TOOL) {
        char prev[48]; snprintf(prev, sizeof(prev), "%.40s", body);
        snprintf(r->reply, sizeof(r->reply), en ? "Creating %s with: %s" : "Creo %s con: %s", name, prev);
    }
    return 1;
}

// Tool: device settings (volume / brightness). Proposes a typed action the executor runs by calling
// the public setter — kept executor-side so nucleo_anima needs no nucleo_app/nucleo_audio dependency
// (no link cycle). Absolute "a 50" -> arg "50"; bare "alza/abbassa" -> arg "+10"/"-10" (relative).
static int tool_setting(const char *raw, char tok[A_MAX_TOKENS][A_TOK_LEN], int ntok, bool en, anima_result_t *r)
{
    if (a_action_is_statement(tok, ntok)) return 0;       // "ho già abbassato la luce" -> a statement, don't mutate
    int target = 0; bool explicit_tgt = false;            // explicit = a real device noun was named
    for (int t = 0; t < ntok; t++) {
        if (a_match("volume", tok[t]) || a_match("audio", tok[t]) || a_match("suono", tok[t])) { target = 1; explicit_tgt = true; }
        if (a_match("luminosita", tok[t]) || a_match("brightness", tok[t]) ||
            a_match("schermo", tok[t]) || a_match("luce", tok[t])) { target = 2; explicit_tgt = true; }
        // brightness IMPLIED (not named): "fai più CHIARO", "metti BUIO" — only acts WITH a command verb,
        // so a bare statement ("ho paura del buio") never fires a settings mutation.
        if (a_match("buio", tok[t]) || a_match("scuro", tok[t]) || a_match("chiaro", tok[t]) ||
            a_match("luminoso", tok[t]) || a_match("luminosa", tok[t]) || a_match("brighter", tok[t]) ||
            a_match("darker", tok[t]) || a_match("dim", tok[t]) || a_match("brighten", tok[t])) { if (!target) target = 2; }
    }
    if (!target) return 0;

    // DEFER to geometry/physics: a "volume"/"suono" that is actually a SHAPE volume or a sound-speed
    // problem ("volume del cubo lato 3", "densità massa 100 volume 50", "il suono viaggia a 340 m/s ...
    // secondi") is NOT a settings command -> let the math tier compute it. (exact match, no fuzzy.)
    {
        static const char *const geo[] = { "cubo","sfera","cilindro","cono","cerchio","quadrato","triangolo",
            "rettangolo","trapezio","piramide","lato","raggio","altezza","diametro","densita","massa","viaggia",
            "velocita","spazio","secondi","cube","sphere","cylinder","cone","circle","triangle","rectangle",
            "side","radius","height","diameter","density","mass","travels","speed","seconds", NULL };
        for (int t = 0; t < ntok; t++) for (int i = 0; geo[i]; i++) if (!strcmp(tok[t], geo[i])) return 0;
    }

    int verb_dir = 0, adj_dir = 0, quant_dir = 0; bool cmd = false, too = false;
    for (int t = 0; t < ntok; t++) {
        // "TROPPO alto / too loud" is a complaint: the wish is the opposite of the adjective
        if (!strcmp(tok[t], "troppo") || !strcmp(tok[t], "troppa") || !strcmp(tok[t], "too")) too = true;
        // imperative DIRECTION verbs (strongest: command + direction)
        if (a_match("alza", tok[t]) || a_match("aumenta", tok[t]) || a_match("raise", tok[t]) || a_match("increase", tok[t]) || a_match("brighten", tok[t])) { verb_dir = +1; cmd = true; }
        if (a_match("abbassa", tok[t]) || a_match("diminuisci", tok[t]) || a_match("riduci", tok[t]) || a_match("lower", tok[t]) || a_match("decrease", tok[t]) || a_match("dim", tok[t])) { verb_dir = -1; cmd = true; }
        // pure SET/CHANGE/MUTE verbs (command, no direction by itself)
        if (a_match("imposta", tok[t]) || a_match("metti", tok[t]) || a_match("set", tok[t]) ||
            a_match("porta", tok[t]) || a_match("regola", tok[t]) || a_match("cambia", tok[t]) ||
            a_match("setta", tok[t]) || a_match("settare", tok[t]) || a_match("settiamo", tok[t]) ||
            a_match("modifica", tok[t]) || a_match("aggiusta", tok[t]) || a_match("change", tok[t]) ||
            a_match("adjust", tok[t]) || a_match("turn", tok[t]) || a_match("fai", tok[t]) ||
            a_match("rendi", tok[t]) || a_match("make", tok[t]) ||
            a_match("muta", tok[t]) || a_match("silenzia", tok[t]) || a_match("azzera", tok[t]) || a_match("mute", tok[t]) || a_match("spegni", tok[t])) cmd = true;
        // comparative ADJECTIVE direction ("più ALTO/BASSO/CHIARO/SCURO") — beats a bare quantifier
        if (a_match("alto", tok[t]) || a_match("alta", tok[t]) || a_match("higher", tok[t]) || a_match("up", tok[t]) ||
            a_match("chiaro", tok[t]) || a_match("luminoso", tok[t]) || a_match("brighter", tok[t]) ||
            !strcmp(tok[t], "su") || !strcmp(tok[t], "forte") || !strcmp(tok[t], "loud") || !strcmp(tok[t], "louder") ||
            !strcmp(tok[t], "bright") || a_match("rumoroso", tok[t]) || a_match("acceso", tok[t])) adj_dir = +1;
        if (a_match("basso", tok[t]) || a_match("bassa", tok[t]) || a_match("down", tok[t]) ||
            a_match("scuro", tok[t]) || a_match("buio", tok[t]) || a_match("darker", tok[t]) ||
            !strcmp(tok[t], "giu") || !strcmp(tok[t], "piano") || !strcmp(tok[t], "quiet") || !strcmp(tok[t], "quieter") ||
            !strcmp(tok[t], "soft") || !strcmp(tok[t], "softer") || !strcmp(tok[t], "dim") || !strcmp(tok[t], "dimmer") ||
            !strcmp(tok[t], "dark")) adj_dir = -1;
        // bare quantifier ("PIÙ volume" / "MENO luce") — weakest, only when no adjective gave a direction
        if (a_match("piu", tok[t]) || a_match("more", tok[t])) { if (!quant_dir) quant_dir = +1; }
        if (a_match("meno", tok[t]) || a_match("less", tok[t])) { if (!quant_dir) quant_dir = -1; }
    }
    if (too && adj_dir) adj_dir = -adj_dir;               // "è troppo forte" -> abbassa; "too dark" -> brighter
    int dir = verb_dir ? verb_dir : (adj_dir ? adj_dir : quant_dir);   // verb > adjective > quantifier

    int val = -1;                                          // first integer in the raw input, if any
    bool by = false;                                       // "alza il volume DI 20": a step, not a level
    for (const char *p = raw; *p; p++) if (isdigit((unsigned char)*p)) {
        val = (int)strtol(p, NULL, 10);
        const char *w = p; while (w > raw && w[-1] == ' ') w--;
        by = (w - raw >= 3 && !strncasecmp(w - 3, " di", 3)) || (w - raw >= 3 && !strncasecmp(w - 3, " by", 3));
        break;
    }
    if (val < 0) for (int t = 0; t < ntok; t++) {          // spelled amounts: "a zero" / "al massimo" / "a metà" / "muto"
        if (a_match("zero", tok[t]) || a_match("spento", tok[t]) || a_match("spegni", tok[t]) || a_match("muto", tok[t]) || a_match("muta", tok[t]) ||
            a_match("silenzia", tok[t]) || a_match("azzera", tok[t]) || a_match("mute", tok[t]) || a_match("off", tok[t])) { val = 0; break; }
        if (a_match("massimo", tok[t]) || a_match("massima", tok[t]) || a_match("max", tok[t]) || a_match("full", tok[t])) { val = 100; break; }
        if (a_match("minimo", tok[t]) || a_match("minima", tok[t]) || a_match("min", tok[t])) { val = 0; break; }
        if (a_match("meta", tok[t]) || a_match("half", tok[t])) { val = 50; break; }
    }

    // FIRE only on a real command: an imperative verb, OR a SHORT named-target phrase carrying a
    // direction/amount ("volume più alto", "volume al 50"). The verb-less path is capped at 4 tokens so a
    // long STATEMENT ("odio quando il volume è troppo alto") never fires a mutation. A bare adjective
    // ("...del buio") never mutates a setting either.
    if (!(cmd || (explicit_tgt && (dir != 0 || val >= 0) && ntok <= 4))) return 0;

    char argbuf[12];
    const bool step = by && verb_dir != 0 && val > 0;      // raise/lower BY an amount: relative
    if (step) { if (val > 100) val = 100; snprintf(argbuf, sizeof(argbuf), "%+d", verb_dir * val); }
    else if (val >= 0) { if (val > 100) val = 100; snprintf(argbuf, sizeof(argbuf), "%d", val); }
    else if (dir) snprintf(argbuf, sizeof(argbuf), "%+d", dir * 10);
    else return 0;                                         // "imposta il volume" with no amount -> miss

    r->tier = ANIMA_TIER_COMMAND; r->action = ANIMA_ACT_TOOL; r->confidence = 88;
    snprintf(r->intent, sizeof(r->intent), target == 1 ? "set_volume" : "set_brightness");
    snprintf(r->state, sizeof(r->state), "tool");
    snprintf(r->arg, sizeof(r->arg), "%s", argbuf);       // machine form: "70" | "+10" | "-10"
    const char *what = target == 1 ? "volume" : (en ? "brightness" : "luminosita");
    const char *art  = target == 1 ? "il" : "la";         // Italian gender: il volume / la luminosita
    if (step) {
        if (en) snprintf(r->reply, sizeof(r->reply), verb_dir > 0 ? "Raising the %s by %d." : "Lowering the %s by %d.", what, val);
        else    snprintf(r->reply, sizeof(r->reply), verb_dir > 0 ? "Alzo %s %s di %d." : "Abbasso %s %s di %d.", art, what, val);
    } else if (en) {
        if (val >= 0) snprintf(r->reply, sizeof(r->reply), "Setting the %s to %d%%.", what, val);
        else          snprintf(r->reply, sizeof(r->reply), dir > 0 ? "Raising the %s." : "Lowering the %s.", what);
    } else {
        if (val >= 0) snprintf(r->reply, sizeof(r->reply), "Imposto %s %s al %d%%.", art, what, val);
        else          snprintf(r->reply, sizeof(r->reply), dir > 0 ? "Alzo %s %s." : "Abbasso %s %s.", art, what);
    }
    trace_step(en ? "tool: device setting" : "tool: impostazione");
    return 1;
}

// Tool: add_event (calendar reminder) — the agent SCHEDULES something. Fires on a reminder phrasing
// ("ricordami di X", "remind me to X") or a create+event-noun ("aggiungi un evento ..."). Parses the
// WHEN (day offset: oggi/domani/dopodomani/"tra N giorni"), an optional TIME ("alle HH[:MM]"), and the
// TEXT; packs them on the content channel as "off=<d>;time=<HH:MM|>;text=<...>" and proposes ONE
// ANIMA_ACT_TOOL the executor resolves against the RTC date and appends to the OS calendar.
static bool a_event_trigger(char tok[A_MAX_TOKENS][A_TOK_LEN], int ntok)
{
    if (a_action_is_statement(tok, ntok)) return false;   // "ho creato un evento", "non aggiungere…" -> not a command
    static const char *remind[] = { "ricordami", "ricorda", "promemoria", "reminder", "remind", "dimenticare", "dimenticarmi", "forget", NULL };
    static const char *verbs[]  = { "crea", "creare", "aggiungi", "segna", "nuovo", "nuova", "add", "new", "set", "metti",
                                    "schedule", "pianifica", "programma", "fissa", "prenota", NULL };
    static const char *nouns[]  = { "evento", "eventi", "appuntamento", "appuntamenti", "impegno", "event", "appointment",
                                    "calendario", "calendar", "agenda", "meeting", "riunione", "incontro", NULL };
    // delete/cancel is NOT a create — "cancella il promemoria" must not fabricate a new reminder.
    static const char *del[]    = { "cancella", "elimina", "rimuovi", "togli", "delete", "remove", "cancel", NULL };
    for (int t = 0; t < ntok; t++) for (int i = 0; del[i]; i++) if (a_match(del[i], tok[t])) return false;
    // "(me/te) LO ricordi?" / "LO ricordi?" = a RECALL question ("do you recall it"), NOT a reminder TASK.
    // An object clitic (lo/la/li/le/ne) right before ricorda/ricordi marks recall — "mi chiamo X, lo ricordi"
    // and "che anno è, me lo ricordi?" must NOT fabricate a calendar reminder. ("ricordami di …" is untouched.)
    for (int t = 1; t < ntok; t++)
        if ((a_match("ricorda", tok[t]) || !strcmp(tok[t], "ricordi")) &&
            (!strcmp(tok[t-1],"lo")||!strcmp(tok[t-1],"la")||!strcmp(tok[t-1],"li")||!strcmp(tok[t-1],"le")||!strcmp(tok[t-1],"ne")))
            return false;
    // interrogatives that make "ricordami <X>" a QUESTION, not a task. NB: NOT "che" — it is also a
    // conjunction ("ricordami che ho la riunione" is a real reminder). IT "quanto/quanti/significato"
    // were missing, so "ricordami quanto fa 2+2" fabricated a junk calendar entry (EN "what" worked).
    static const char *qwords[] = { "come", "cosa", "cos", "chi", "quando", "dove", "perche", "quale", "quali",
                                    "quanto", "quanti", "quanta", "quante", "significato", "significa", "spiega", "spiegami",
                                    "how", "what", "who", "when", "where", "why", "which",
                                    "posso", "puoi", "potresti", "puo", "sai", "riesci", "sapresti",
                                    "can", "could", "may", "able", NULL };
    bool rem = false, v = false, n = false, q = false; bool has_ore = false, has_sono = false;
    for (int t = 0; t < ntok; t++) {
        for (int i = 0; remind[i]; i++) if (a_match(remind[i], tok[t])) rem = true;
        for (int i = 0; verbs[i];  i++) if (a_match(verbs[i],  tok[t])) v = true;
        for (int i = 0; nouns[i];  i++) if (a_match(nouns[i],  tok[t])) n = true;
        for (int i = 0; qwords[i]; i++) if (!strcmp(qwords[i], tok[t])) q = true;   // "ricordami COME..." = a question
        if (!strcmp(tok[t], "ore")) has_ore = true;
        if (!strcmp(tok[t], "sono")) has_sono = true;
    }
    if (q) return false;                          // a how/what/when question is never a reminder to schedule
    if (has_ore && has_sono) return false;        // "ricordami che ore sono" — a time question, not a task
    return rem || (v && n);
}

static int tool_event(const char *raw, char tok[A_MAX_TOKENS][A_TOK_LEN], int ntok, bool en, anima_result_t *r)
{
    if (!a_event_trigger(tok, ntok)) return 0;
    // A create-FILE command whose CONTENT merely contains a reminder word ("crea un file con il testo
    // ...un promemoria", "scrivi una nota che dice ricordati la riunione") is a FILE, not an event ->
    // fall through to tool_note (next in the registry). A genuine "ricordami di scrivere la nota domani"
    // has no content connector, so it still schedules.
    { int m; if (a_is_create_file(tok, ntok) && a_content_clause(raw, &m)) return 0; }
    // DELETE/CANCEL is not a create — scan the RAW string (the normalizer can corrupt "cancella"->"cartella",
    // dodging the tokenized guard). "cancella/elimina il promemoria" must abstain, not fabricate an event.
    { char rl[160]; size_t z = 0; for (; raw[z] && z + 1 < sizeof rl; z++) rl[z] = (char)tolower((unsigned char)raw[z]); rl[z] = 0;
      static const char *const drw[] = { "cancell", "elimin", "rimuov", "delete", "remove", "annulla", NULL };
      for (int i = 0; drw[i]; i++) if (strstr(rl, drw[i])) return 0; }

    const char *start[A_MAX_TOKENS]; char low[A_MAX_TOKENS][24]; int n = 0;
    for (const char *p = raw; *p && n < A_MAX_TOKENS; ) {
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) break;
        start[n] = p; int k = 0;
        while (*p && *p != ' ' && *p != '\t') {
            unsigned char uc = (unsigned char)*p;
            if (uc == 0xC3 && p[1]) {                       // fold IT accents so "venerdì" matches WD "venerdi"
                unsigned char d = (unsigned char)p[1];
                char f = (d>=0xA0&&d<=0xA2)?'a':(d>=0xA8&&d<=0xAA)?'e':(d>=0xAC&&d<=0xAE)?'i':(d>=0xB2&&d<=0xB4)?'o':(d>=0xB9&&d<=0xBB)?'u':0;
                if (f) { if (k < 23) low[n][k++] = f; p += 2; continue; }
            }
            if (k < 23) low[n][k++] = (char)tolower(uc); p++;
        }
        low[n][k] = 0; n++;
    }

    bool drop[A_MAX_TOKENS]; for (int i = 0; i < n; i++) drop[i] = false;

    // WHEN: day offset (default today). Drop the when-words from the text. localtime gives today's
    // weekday so a named day ("venerdì") resolves to its NEXT occurrence.
    struct tm lt; { time_t now = time(NULL); localtime_r(&now, &lt); }   // reentrant: two workers + UI clock
    static const char *const WD[7][3] = {   // index = tm_wday: 0=Sunday .. 6=Saturday
        {"domenica","sunday",NULL}, {"lunedi","monday",NULL}, {"martedi","tuesday",NULL},
        {"mercoledi","wednesday",NULL}, {"giovedi","thursday",NULL}, {"venerdi","friday",NULL}, {"sabato","saturday",NULL} };
    int off = 0;
    int rel_min = 0;                                  // "tra 2 ore" / "in 30 minutes": minutes from now
    for (int i = 0; i < n; i++) {
        if (!strcmp(low[i], "oggi") || !strcmp(low[i], "today")) { off = 0; drop[i] = true; }
        else if (!strcmp(low[i], "dopodomani")) { off = 2; drop[i] = true; }
        // EN "(the day) after tomorrow" -> +2 (must beat the bare 'tomorrow' below)
        else if (!strcmp(low[i], "tomorrow") && i >= 1 && !strcmp(low[i - 1], "after")) {
            off = 2; drop[i] = true; drop[i - 1] = true; if (i >= 2 && !strcmp(low[i - 2], "day")) drop[i - 2] = true;
        }
        else if (!strcmp(low[i], "domani") || !strcmp(low[i], "tomorrow")) { off = 1; drop[i] = true; }
        else if ((!strcmp(low[i], "tra") || !strcmp(low[i], "fra") || !strcmp(low[i], "in")) && i + 1 < n) {
            // "tra N giorni" / "tra una|due settimane" / "in a week" — number word or digit, days or weeks.
            int q = 0, ni = i + 1;
            if (isdigit((unsigned char)low[ni][0])) q = atoi(low[ni]);
            else if (!strcmp(low[ni],"una")||!strcmp(low[ni],"un")||!strcmp(low[ni],"a")||!strcmp(low[ni],"one")) q = 1;
            else if (!strcmp(low[ni],"due")||!strcmp(low[ni],"two")) q = 2;
            else if (!strcmp(low[ni],"tre")||!strcmp(low[ni],"three")) q = 3;
            if (q > 0) {
                bool wk = (ni + 1 < n) && (!strcmp(low[ni+1],"settimana")||!strcmp(low[ni+1],"settimane")||
                                           !strcmp(low[ni+1],"week")||!strcmp(low[ni+1],"weeks"));
                bool dy = (ni + 1 < n) && (!strcmp(low[ni+1],"giorni")||!strcmp(low[ni+1],"giorno")||
                                           !strcmp(low[ni+1],"days")||!strcmp(low[ni+1],"day"));
                int days = wk ? q * 7 : q;
                // Only with a day/week unit: "tra 2 ore" is no day offset (it was scheduled 2 DAYS out).
                bool hr = (ni + 1 < n) && (!strcmp(low[ni+1],"ore")||!strcmp(low[ni+1],"ora")||
                                           !strcmp(low[ni+1],"hours")||!strcmp(low[ni+1],"hour"));
                bool mn = (ni + 1 < n) && (!strcmp(low[ni+1],"minuti")||!strcmp(low[ni+1],"minuto")||
                                           !strcmp(low[ni+1],"minutes")||!strcmp(low[ni+1],"minute")||!strcmp(low[ni+1],"min"));
                if ((dy || wk) && days > 0 && days <= 60) { off = days; drop[i] = true; drop[ni] = true; drop[ni + 1] = true; }
                else if ((hr && q <= 72) || (mn && q <= 24 * 60)) {
                    rel_min = hr ? q * 60 : q; drop[i] = true; drop[ni] = true; drop[ni + 1] = true;
                }
            }
        }
        else {   // a named weekday -> its NEXT occurrence (today's name means next week, +7)
            int w = -1;
            for (int d = 0; d < 7 && w < 0; d++) for (int s = 0; WD[d][s]; s++) if (!strcmp(low[i], WD[d][s])) { w = d; break; }
            if (w >= 0) { off = (w - lt.tm_wday + 7) % 7; if (off == 0) off = 7; drop[i] = true;
                if (i >= 1 && (!strcmp(low[i-1],"prossimo")||!strcmp(low[i-1],"prossima")||!strcmp(low[i-1],"next")||!strcmp(low[i-1],"questo"))) drop[i-1] = true; }
        }
    }

    // TIME: "alle/ore/at HH[:MM]" preferred, else a bare HH:MM or 12-hour "8pm"/"8am". A separate
    // "pomeriggio"/"sera"/"pm" token also shifts to PM.
    int hh = -1, mm = 0;
    for (int i = 0; i < n; i++)
        if ((!strcmp(low[i], "alle") || !strcmp(low[i], "ore") || !strcmp(low[i], "at")) && i + 1 < n &&
            isdigit((unsigned char)low[i + 1][0])) {
            hh = atoi(low[i + 1]); const char *c = strchr(low[i + 1], ':'); if (c) mm = atoi(c + 1);
            if (strstr(low[i + 1], "pm")) { if (hh >= 1 && hh < 12) hh += 12; } else if (strstr(low[i + 1], "am")) { if (hh == 12) hh = 0; }
            drop[i] = true; drop[i + 1] = true; break;
        }
    if (hh < 0)
        for (int i = 0; i < n; i++) {
            if (!isdigit((unsigned char)low[i][0])) continue;
            const char *c = strchr(low[i], ':');
            bool pm = strstr(low[i], "pm") != NULL, am = strstr(low[i], "am") != NULL;
            if (c) { hh = atoi(low[i]); mm = atoi(c + 1); drop[i] = true; if (pm && hh < 12) hh += 12; else if (am && hh == 12) hh = 0; break; }
            if (pm || am) { hh = atoi(low[i]); if (pm) { if (hh >= 1 && hh < 12) hh += 12; } else if (hh == 12) hh = 0; drop[i] = true; break; }
        }
    for (int i = 0; i < n; i++)
        if (!strcmp(low[i], "pomeriggio") || !strcmp(low[i], "sera") || !strcmp(low[i], "pm")) {
            if (hh >= 1 && hh < 12) hh += 12;
            drop[i] = true;
        }
    if (hh > 23 || mm > 59) { hh = -1; mm = 0; }
    if (rel_min > 0 && hh < 0) {                      // "tra 2 ore": a clock time, possibly tomorrow
        struct tm t = lt; t.tm_min += rel_min; t.tm_sec = 0;
        time_t when_t = mktime(&t); struct tm w; localtime_r(&when_t, &w);
        hh = w.tm_hour; mm = w.tm_min;
        struct tm d0 = lt, d1 = w; d0.tm_hour = d1.tm_hour = 12; d0.tm_min = d1.tm_min = d0.tm_sec = d1.tm_sec = 0;
        off = (int)((mktime(&d1) - mktime(&d0) + 43200) / 86400);
    }

    // Drop the LEADING structural run (trigger / verb / event-noun / article / "di"/"che"/"to"/"that").
    static const char *lead[] = { "ricordami","ricorda","promemoria","reminder","remind","me","mi",
        "crea","creare","crei","aggiungi","segna","nuovo","nuova","add","new","set","metti",
        "schedule","pianifica","programma","fissa","prenota",                                      // scheduling verbs
        "evento","eventi","appuntamento","appuntamenti","impegno","event","appointment",
        "calendario","calendar","agenda","al","allo","alla","ai","nel","nella","negli","for",      // "aggiungi AL CALENDARIO X" -> X
        "non","farmi","fammi","dimenticare","dimenticarmi","forget","dont","let","scordare",        // "non farmi dimenticare X" -> X
        "chiamato","chiamata","intitolato","intitolata","titolo","dal","called","titled","named",   // "evento chiamato X" -> X
        "di","che","to","that","un","uno","una","il","lo","la","the","a","per","of", NULL };
    for (int i = 0; i < n; i++) {
        bool isLead = drop[i];
        for (int j = 0; !isLead && lead[j]; j++) if (!strcmp(low[i], lead[j])) isLead = true;
        if (isLead) drop[i] = true; else break;       // stop at the first real content word
    }

    // TEXT = the surviving words, in order (from the original raw, to keep their case).
    char text[AG_CONTENT_MAX]; int tl = 0;
    for (int i = 0; i < n; i++) {
        if (drop[i]) continue;
        const char *s = start[i]; int len = 0; while (s[len] && s[len] != ' ' && s[len] != '\t') len++;
        if (tl && tl < (int)sizeof(text) - 1) text[tl++] = ' ';
        for (int k = 0; k < len && tl < (int)sizeof(text) - 1; k++) text[tl++] = s[k];
    }
    text[tl] = 0;
    if (!text[0]) {                                   // a timed reminder with no body ("promemoria per venerdì
        if (off > 0 || hh >= 0) snprintf(text, sizeof text, en ? "reminder" : "promemoria");  // alle 18") -> generic text
        else return 0;                                // truly nothing to schedule
    }

    // Pack the structured event on the content channel; the executor resolves off->date via the RTC.
    if (hh >= 0) snprintf(s_tool_content, sizeof(s_tool_content), "off=%d;time=%02d:%02d;text=%s", off, hh, mm, text);
    else         snprintf(s_tool_content, sizeof(s_tool_content), "off=%d;time=;text=%s", off, text);

    const char *when = off == 0 ? (en ? "today" : "oggi") : off == 1 ? (en ? "tomorrow" : "domani")
                     : off == 2 ? (en ? "in 2 days" : "dopodomani") : NULL;
    char whenbuf[24]; if (!when) { snprintf(whenbuf, sizeof(whenbuf), en ? "in %d days" : "tra %d giorni", off); when = whenbuf; }
    char timebuf[16] = ""; if (hh >= 0) snprintf(timebuf, sizeof(timebuf), en ? " at %02d:%02d" : " alle %02d:%02d", hh, mm);

    r->tier = ANIMA_TIER_COMMAND; r->action = ANIMA_ACT_TOOL; r->confidence = 86;
    snprintf(r->intent, sizeof(r->intent), "add_event");
    snprintf(r->state, sizeof(r->state), "tool");
    snprintf(r->arg, sizeof(r->arg), "add_event");
    snprintf(r->reply, sizeof(r->reply), en ? "Reminder \"%s\" %s%s." : "Promemoria \"%s\" %s%s.", text, when, timebuf);
    trace_step(en ? "plan: schedule" : "piano: pianifica");
    char st[40]; snprintf(st, sizeof(st), en ? "when=%s%s" : "quando=%s%s", when, timebuf); trace_step(st);
    trace_step(en ? "verify: ok" : "verifica: ok");
    return 1;
}

// LLM TOOL-CALLING: a language model (cloud, LAN Ollama, ...) asks for a device action by replying
// with ONE line "ACT <tool> <args>" (grammar in nucleo_anima_act_grammar). A plain-text protocol, not a
// provider's native tool API, so the smallest local model can use it too. Every field is validated
// here against the same whitelist the L0 tools use; anything else stays an ordinary answer.
// ---- ANIMA's shell tool: the OS registers an executor (the Terminal's shell, run headless) ------------
static int (*s_shell)(const char *line, char *out, int cap);
void nucleo_anima_set_shell(int (*exec)(const char *line, char *out, int cap)) { s_shell = exec; }
bool nucleo_anima_has_shell(void) { return s_shell != NULL; }

// The workspace: the folder ANIMA works in (a project: ~/lua/gioco, an app: /sdcard/apps/x). The
// shell starts its next command there and the model is told (nucleo_anima_sh_grammar), so "ls",
// relative paths and "app check main.lua" mean the project, like the cwd of a coding agent.
EXT_RAM_BSS_ATTR static char s_ws[160];
static bool s_ws_cd;

bool nucleo_anima_set_workspace(const char *path)
{
    char p[160];
    if (!path || !path[0]) { s_ws[0] = 0; s_ws_cd = false; return true; }   // no workspace: the shell stays put
    if (!strcmp(path, "~")) snprintf(p, sizeof p, NUCLEO_SD_MOUNT "/home");
    else if (path[0] == '~' && path[1] == '/') snprintf(p, sizeof p, NUCLEO_SD_MOUNT "/home/%s", path + 2);
    else snprintf(p, sizeof p, "%s", path);
    size_t n = strlen(p);
    while (n > 1 && p[n - 1] == '/') p[--n] = 0;
    // only inside the card, and nothing that could break out of the quoted cd
    if (strncmp(p, NUCLEO_SD_MOUNT "/", strlen(NUCLEO_SD_MOUNT) + 1) || strstr(p, "..") || strpbrk(p, "'\"`$\\;&|\n"))
        return false;
    snprintf(s_ws, sizeof s_ws, "%s", p);
    s_ws_cd = true;
    return true;
}

const char *nucleo_anima_workspace(void) { return s_ws[0] ? s_ws : NUCLEO_SD_MOUNT "/home"; }

int anima_shell_run(const char *line, char *out, int cap)
{
    if (out && cap) out[0] = 0;
    if (!s_shell) return -100;
    if (s_ws_cd && s_ws[0]) {                   // a new workspace: the shell moves there once
        char cd[200], junk[160];
        snprintf(cd, sizeof cd, "cd '%s'", s_ws);
        s_shell(cd, junk, sizeof junk);
        s_ws_cd = false;
    }
    return s_shell(line, out, cap);
}

// 1 = read-only (runs without asking, like Claude Code's safe commands), 0 = it changes something
// (permissions.json "sh"), -1 = needs the Terminal screen (full-screen / interactive): never for ANIMA.
int nucleo_anima_sh_class(const char *line)
{
    static const char *const SAFE[] = { "ls", "dir", "ll", "cat", "head", "tail", "wc", "grep", "egrep", "sort", "uniq",
        "find", "tree", "du", "df", "stat", "pwd", "echo", "date", "uptime", "free", "mem", "ps", "uname", "ver",
        "version", "whoami", "id", "nproc", "sensors", "temp", "ip", "ifconfig", "wifi", "env", "printenv", "which",
        "type", "command", "help", "man", "basename", "dirname", "realpath", "readlink", "seq", "expr", "test", "[",
        "true", "false", "printf", "cut", "tr", "rev", "tac", "nl", "md5sum", "sha1sum", "sha256sum", "xxd",
        "hexdump", "awk", "gawk", "base64", "host", "nslookup", "ping", "hostname", "dmesg", "log", "apps",
        "programs", "history", "cd", "services", "screenshot", "ui", "diff", "jq", "sysinfo", "status", "neofetch", "rg", "la", "l", NULL };
    static const char *const SCREEN[] = { "edit", "nano", "pico", "less", "more", "top", "htop", "watch", "exit",
        "logout", "clear", "cls", "reset", "stty", NULL };
    if (!line) return -1;
    if (strchr(line, '>') || strchr(line, '`') || strstr(line, "$(")) return 0;   // writes a file / runs a substitution
    int cls = 1;
    const char *p = line;
    while (*p) {
        while (*p == ' ' || *p == '\t' || *p == ';' || *p == '|' || *p == '&') p++;
        if (!*p) break;
        char w[24]; int n = 0;
        while (*p && *p != ' ' && *p != '\t' && *p != ';' && *p != '|' && *p != '&' && n < (int)sizeof w - 1) w[n++] = *p++;
        w[n] = 0;
        bool safe = false;
        for (int i = 0; SCREEN[i]; i++) if (!strcmp(w, SCREEN[i])) return -1;
        for (int i = 0; SAFE[i]; i++) if (!strcmp(w, SAFE[i])) safe = true;
        // the rest of this command (to the next separator): a few safe commands have writing options
        const char *e = p;
        while (*e && *e != ';' && *e != '|' && *e != '&') e++;
        char rest[160]; snprintf(rest, sizeof rest, "%.*s", (int)(e - p), p);
        // Subcommand-aware commands: classify on the FIRST argument only (a keyword elsewhere in the
        // line must not make a writer safe) and split words exactly like the shell lexer (space/tab).
        char sub[16] = ""; int words = 0;
        for (const char *r = rest; *r;) {
            while (*r == ' ' || *r == '\t' || *r == '\r') r++;
            if (!*r) break;
            const char *ws = r;
            while (*r && *r != ' ' && *r != '\t' && *r != '\r') r++;
            if (!words++) snprintf(sub, sizeof sub, "%.*s", (int)(r - ws), ws);
        }
#define SUB_IS(x) (!strcmp(sub, x))
        if (!strcmp(w, "store")) safe = SUB_IS("search") || SUB_IS("find") || SUB_IS("list") || SUB_IS("ls") || SUB_IS("info");
        if (!strcmp(w, "sed")) {                          // any option cluster carrying i (-i -ni -Ei) edits in place
            safe = true;
            for (const char *r = rest; *r; r++)
                if (*r == '-' && (r == rest || r[-1] == ' ' || r[-1] == '\t')) {
                    if (r[1] == '-') { if (!strncmp(r + 2, "in-place", 8)) safe = false; continue; }
                    for (const char *q = r + 1; *q && *q != ' ' && *q != '\t'; q++) if (*q == 'i') safe = false;
                }
        }
        if (!strcmp(w, "app")) safe = !words || SUB_IS("check") || SUB_IS("ls") || SUB_IS("help");   // running asks
        if (!strcmp(w, "cfg")) safe = !words || SUB_IS("export") || (words == 1 && !strchr(sub, '='));  // changing asks
        if (!strcmp(w, "wifi")) safe = !words || SUB_IS("status") || SUB_IS("scan");
        if (!strcmp(w, "ha")) safe = !words || SUB_IS("ls") || SUB_IS("find") || SUB_IS("get") || SUB_IS("status") || SUB_IS("help");
        if (!strcmp(w, "dev")) safe = !words || SUB_IS("ls") || SUB_IS("get") || SUB_IS("status") || SUB_IS("help");  // scan writes devices.json
#undef SUB_IS
        if (!strcmp(w, "screenshot")) {   // safe into ~/shots only: a FILE argument could overwrite anything
            const char *r = rest;
            while (*r == ' ') r++;
            if (!strncmp(r, "-d", 2)) { r += 2; while (*r == ' ') r++; while (*r >= '0' && *r <= '9') r++; while (*r == ' ') r++; }
            safe = !*r;
        }
        if (!safe) cls = 0;
        p = e;
    }
    return cls;
}

// ---- file tools (OpenCode's write / edit): whole files and exact replacements, multi-line blocks ------
//   ACT write <path>        ACT edit <path>
//   <<<                     <<<
//   ...content...           ...old text (exact, once)...
//   >>>                     ===
//                           ...new text...
//                           >>>
// Paths: under /sdcard/home (~), /sdcard/data or /sdcard/apps, never "..". Result text for the model.
#define FT_MAX (32 * 1024)

static bool ft_path(const char *in, char *out, int cap)
{
    while (*in == ' ') in++;
    char p[200]; int n = 0;
    while (*in && *in != '\n' && *in != '\r' && *in != '|' && n < (int)sizeof p - 1) p[n++] = *in++;
    while (n && p[n-1] == ' ') n--;
    p[n] = 0;
    if (!n || strstr(p, "..")) return false;
    if (p[0] == '~') snprintf(out, cap, NUCLEO_SD_MOUNT "/home%s", p + 1);
    else if (!strncmp(p, "/sdcard/", 8)) snprintf(out, cap, NUCLEO_SD_MOUNT "/%s", p + 8);
    else if (p[0] != '/') snprintf(out, cap, NUCLEO_SD_MOUNT "/home/%s", p);
    else return false;
    const char *rel = out + strlen(NUCLEO_SD_MOUNT);
    if (strncmp(rel, "/home/", 6) && strncmp(rel, "/data/", 6) && strncmp(rel, "/apps/", 6)) return false;
    // What steers or unlocks the model itself is the user's to change, never the model's: its
    // permissions, its persona and the credential vaults (FAT is case-insensitive, so compare so).
    // The user memory is written only through ACT remember (its own permission, dedupe and cap):
    // memory.jsonl, and MEMORY.md, which is imported into it.
    static const char *const kProtected[] = {
        "/data/anima/permissions.json", "/data/anima/teacher.json", "/data/anima/telegram.json",
        "/data/anima/SOUL.md", "/data/anima/USER.md", "/data/anima/memory.jsonl", "/data/anima/MEMORY.md",
    };
    for (size_t i = 0; i < sizeof kProtected / sizeof kProtected[0]; i++) {
        const size_t l = strlen(kProtected[i]);
        if (!strncasecmp(rel, kProtected[i], l) && (rel[l] == 0 || !strcasecmp(rel + l, ".tmp"))) return false;
    }
    return true;
}

// The body between "<<<" and ">>>" (end of text if the model forgot the close). Pointers into `c`.
static bool ft_block(const char *c, const char **b, size_t *n)
{
    const char *o = strstr(c, "<<<");
    if (!o) return false;
    o += 3;
    if (*o == '\r') o++;
    if (*o == '\n') o++;
    const char *e = strstr(o, "\n>>>");
    if (!e) e = strstr(o, ">>>");
    if (!e) e = o + strlen(o);
    *b = o; *n = (size_t)(e - o);
    return true;
}

// 1 = a file tool line (result in `res`), 0 = not one. Runs it: the caller checked the permission.
// After a write/edit of code or data, its syntax check rides on the result (OpenCode's diagnostics
// after every edit, Aider's auto-lint): the model sees the error and the bad line at once.
static void ft_diag(const char *path, char *res, int cap)
{
    const char *e = strrchr(path, '.');
    if (!e || (strcmp(e, ".lua") && strcmp(e, ".py") && strcmp(e, ".json")) || !s_shell) return;
    char cmd[300], out[700];
    snprintf(cmd, sizeof cmd, "app check '%s'", path);
    if (strchr(path, '\'') || anima_shell_run(cmd, out, sizeof out) < 0 || !out[0]) return;
    const size_t l = strlen(res);
    snprintf(res + l, cap - l, "\nCHECK: %s", out);
}

// Where a file action's arguments start: write, edit, and create_file given a path, which small
// models use for a whole file ("ACT create_file ~/x.lua | ..." + a <<< block) = write. NULL = none.
static const char *ft_args(const char *c)
{
    if (!strncmp(c, "ACT write ", 10)) return c + 10;
    if (!strncmp(c, "ACT edit ", 9)) return c + 9;
    if (!strncmp(c, "ACT create_file ", 16)) {
        const char *a = c + 16;
        while (*a == ' ') a++;
        if (*a == '~' || *a == '/') return a;
    }
    return NULL;
}

bool nucleo_anima_is_file_act(const char *c) { return c && ft_args(c); }

int nucleo_anima_file_tool(const char *content, bool en, char *res, int cap)
{
    if (res && cap) res[0] = 0;
    while (*content == ' ' || *content == '\n' || *content == '`') content++;
    const char *args = ft_args(content);
    if (!args) return 0;
    const bool ed = content[4] == 'e', w = !ed;
    char path[220];
    if (!ft_path(args, path, sizeof path)) {
        snprintf(res, cap, "%s", en ? "error: path not allowed (use ~/..., /sdcard/data/... or /sdcard/apps/...)"
                                    : "errore: percorso non consentito (usa ~/..., /sdcard/data/... o /sdcard/apps/...)");
        return 1;
    }
    const char *b; size_t n;
    if (!ft_block(content, &b, &n)) {
        const char *bar = content[4] == 'c' ? strchr(args, '|') : NULL;   // create_file path | short content
        const char *eol = bar ? strchr(bar, '\n') : NULL;
        if (!bar) { snprintf(res, cap, "error: missing <<< ... >>> block"); return 1; }
        for (b = bar + 1; *b == ' ' || *b == '`'; b++) {}
        n = eol ? (size_t)(eol - b) : strlen(b);
        while (n && (b[n-1] == ' ' || b[n-1] == '`' || b[n-1] == '\r')) n--;
    }
    if (w && n >= 3 && !strncmp(b, "```", 3)) {             // a fenced block inside <<< >>>: drop the fences
        const char *nl = memchr(b, '\n', n);
        n = nl ? n - (size_t)(nl + 1 - b) : 0;
        b = nl ? nl + 1 : b;
    }
    if (w) {                                                // ... and the closing fence, if there is one
        size_t m = n;
        while (m && (b[m-1] == '\n' || b[m-1] == '\r' || b[m-1] == ' ')) m--;
        if (m >= 3 && !strncmp(b + m - 3, "```", 3)) n = m - 3;
    }
    if (n > FT_MAX) { snprintf(res, cap, "error: content over %d bytes", FT_MAX); return 1; }
    const char *shown = path + strlen(NUCLEO_SD_MOUNT);
    if (w) {
        a_mkdirs(path);
        if (!a_write_atomic(path, b, n)) { snprintf(res, cap, "error: cannot write %s", shown); return 1; }
        snprintf(res, cap, "wrote %u bytes to /sdcard%s", (unsigned)n, shown);
        ft_diag(path, res, cap);
        return 1;
    }
    // edit: old === new
    const char *sep = NULL;
    for (const char *q = b; q < b + n; q++) if (!strncmp(q, "\n===\n", 5) || (q == b && !strncmp(q, "===\n", 4))) { sep = q; break; }
    if (!sep) { snprintf(res, cap, "error: edit needs old text, a line ===, new text"); return 1; }
    const char *oldp = b; size_t oldn = (size_t)(sep - b);
    const char *newp = sep + (sep == b ? 4 : 5); size_t newn = (size_t)(b + n - newp);
    FILE *f = fopen(path, "rb");
    if (!f) { snprintf(res, cap, "error: %s does not exist (use write)", shown); return 1; }
    char *buf = malloc(FT_MAX + 1);
    // Read one byte past the cap: a file that does not fit would be cut short by the rewrite below.
    size_t got = buf ? fread(buf, 1, FT_MAX + 1, f) : 0;
    fclose(f);
    if (!buf) { snprintf(res, cap, "error: out of memory"); return 1; }
    if (got > FT_MAX) { free(buf); snprintf(res, cap, "error: %s is over %d bytes, too large to edit", shown, FT_MAX); return 1; }
    buf[got] = 0;
    if (!oldn) { free(buf); snprintf(res, cap, "error: empty old text"); return 1; }
    int hits = 0; char *at = NULL;   // the old text must be there exactly once
    for (char *q = buf; q + oldn <= buf + got && hits < 2; q++)
        if (!memcmp(q, oldp, oldn)) { if (!hits) at = q; hits++; q += oldn - 1; }
    if (hits != 1) {
        free(buf);
        snprintf(res, cap, hits ? "error: the old text appears more than once in /sdcard%s: give more context"
                                : "error: the old text is not in /sdcard%s (read the file first)", shown);
        return 1;
    }
    const size_t total = got - oldn + newn;
    char *nb = total <= FT_MAX ? malloc(total + 1) : NULL;
    if (!nb) { free(buf); snprintf(res, cap, "error: result too large"); return 1; }
    const size_t pre = (size_t)(at - buf);
    memcpy(nb, buf, pre);
    memcpy(nb + pre, newp, newn);
    memcpy(nb + pre + newn, at + oldn, got - pre - oldn);
    const bool ok = a_write_atomic(path, nb, total);
    free(buf); free(nb);
    snprintf(res, cap, ok ? "edited /sdcard%s (%u bytes)" : "error: cannot write /sdcard%s", shown, (unsigned)total);
    if (ok) ft_diag(path, res, cap);
    return 1;
}

// A multi-line action (file tool) waiting for a yes: the whole text, on the heap.
static char *s_pending_blob;

// An ACT line waiting for the user's yes (permission "ask"), and how long it may wait.
EXT_RAM_BSS_ATTR static char s_pending_act[3 * (AG_CONTENT_MAX + 64)];   // up to a whole plan (ANIMA_PLAN_MAX lines)
static int64_t s_pending_act_ms;
static bool    s_act_confirmed;                     // the re-run after a yes skips the permission
#define PENDING_ACT_TTL_MS (2 * 60 * 1000)
static char s_origin[2][12] = {"screen", ""};       // [0] current asker, [1] the one a pending action belongs to
static char s_origin_prev[12];
static bool s_resume;                               // the yes just ran an agent step: the model picks the task up again

const char *nucleo_anima_set_origin(const char *origin)
{
    snprintf(s_origin_prev, sizeof s_origin_prev, "%s", s_origin[0]);
    snprintf(s_origin[0], sizeof s_origin[0], "%s", origin && origin[0] ? origin : "screen");
    return s_origin_prev;
}
static void pending_mark(void) { snprintf(s_origin[1], sizeof s_origin[1], "%s", s_origin[0]); }

static int64_t act_now_ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (int64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000; }

// 1 yes, -1 no, 0 neither (a new request): short confirmations only.
static int act_yes_no(const char *q)
{
    char nz[64]; int n = 0;
    for (const char *p = q; *p && n < (int)sizeof nz - 1; p++) {
        unsigned char c = (unsigned char)*p;
        if (c == 0xC3 && p[1]) { const unsigned char d = (unsigned char)*++p; c = d == 0xAC ? 'i' : d == 0xB2 ? 'o' : d == 0xA8 || d == 0xA9 ? 'e' : d == 0xA0 ? 'a' : '?'; }
        if (isalpha(c) || c == ' ') nz[n++] = (char)tolower(c);
    }
    nz[n] = 0;
    while (n && nz[n-1] == ' ') nz[--n] = 0;
    // nz is accent-folded above, so "si" also covers "sì".
    static const char *const Y[] = { "si", "ok", "okay", "certo", "va bene", "fallo", "procedi", "conferma", "confermo",
                                     "si grazie", "si fallo", "yes", "sure", "do it", "go ahead", "confirm", "yes please", NULL };
    static const char *const N[] = { "no", "annulla", "lascia stare", "non farlo", "no grazie", "stop", "cancel", "dont", "no thanks", NULL };
    for (int i = 0; Y[i]; i++) if (!strcmp(nz, Y[i])) return 1;
    for (int i = 0; N[i]; i++) if (!strcmp(nz, N[i])) return -1;
    return 0;
}

// ── SUGGESTION: the intent suggester (anima_intent.c) offered "Intendi «canon»?" for a sentence nobody
// understood. Only the asker that got it may answer, within a minute: "sì" runs the canonical AND learns
// the sentence (userp_learn), "no" asks for other words, anything else is a new request. ──
#define SUGGEST_TTL_MS (60 * 1000)
EXT_RAM_BSS_ATTR static char s_sugg_canon[64], s_sugg_phrase[160], s_sugg_origin[12];   // PSRAM: internal RAM is tight
static bool s_sugg_en;
static int64_t s_sugg_ms;

static void suggest_offer(const char *canon, const char *phrase, bool en)
{
    snprintf(s_sugg_canon, sizeof s_sugg_canon, "%s", canon);
    snprintf(s_sugg_phrase, sizeof s_sugg_phrase, "%s", phrase);
    snprintf(s_sugg_origin, sizeof s_sugg_origin, "%s", s_origin[0]);
    s_sugg_en = en;
    s_sugg_ms = act_now_ms();
}

// 1 = confirmed (canon_out holds what to run; the sentence is learned), -1 = refused, 0 = no suggestion
// pending or not an answer to it (it is dropped either way).
static int suggest_resolve(const char *q, bool en, char *canon_out, size_t cap)
{
    if (!s_sugg_canon[0]) return 0;
    const bool mine = !strcmp(s_sugg_origin, s_origin[0]) && s_sugg_en == en && act_now_ms() - s_sugg_ms < SUGGEST_TTL_MS;
    const int yn = mine ? act_yes_no(q) : 0;
    if (yn > 0) {
        snprintf(canon_out, cap, "%s", s_sugg_canon);
        userp_learn(s_sugg_phrase, s_sugg_en, s_sugg_canon);
    }
    s_sugg_canon[0] = 0;
    return yn;
}

// A pending "ask" action: yes runs it, no drops it, anything else drops it and is handled normally.
static int act_pending_resolve(const char *q, bool en, anima_result_t *r)
{
    // Only the asker that raised it may answer; automations never confirm anything.
    if ((s_pending_blob || s_pending_act[0]) && (strcmp(s_origin[0], s_origin[1]) || !strcmp(s_origin[0], "rule"))) return 0;
    if (s_pending_blob) {
        char *blob = s_pending_blob; s_pending_blob = NULL;
        const bool fresh = act_now_ms() - s_pending_act_ms < PENDING_ACT_TTL_MS;
        const int yn = fresh ? act_yes_no(q) : 0;
        memset(r, 0, sizeof *r);
        r->tier = ANIMA_TIER_COMMAND; r->action = ANIMA_ACT_ANSWER; r->confidence = 90;
        if (yn > 0 && !strncmp(blob, "RULE ", 5)) {            // a confirmed automation
            snprintf(r->intent, sizeof r->intent, "rule");
            nucleo_anima_rules_add(blob + 5, en, r->reply, sizeof r->reply);
        } else if (yn > 0) {
            snprintf(r->intent, sizeof r->intent, "write");
            nucleo_anima_file_tool(blob, en, r->reply, sizeof r->reply);
            s_resume = true;
        } else if (yn < 0) {
            snprintf(r->intent, sizeof r->intent, "deny");
            snprintf(r->reply, sizeof r->reply, "%s", en ? "OK, I won't." : "Va bene, lascio stare.");
        }
        free(blob);
        if (yn) return 1;
    }
    if (!s_pending_act[0]) return 0;
    char line[sizeof s_pending_act];
    snprintf(line, sizeof line, "%s", s_pending_act);
    const bool fresh = act_now_ms() - s_pending_act_ms < PENDING_ACT_TTL_MS;
    s_pending_act[0] = 0;
    const int yn = fresh ? act_yes_no(q) : 0;
    if (yn > 0) {
        s_act_confirmed = true;
        const int ok = nucleo_anima_act_from_llm(line, en, r);
        s_act_confirmed = false;
        s_resume = ok && !strncmp(line, "ACT sh ", 7);
        return ok;
    }
    if (yn < 0) {
        memset(r, 0, sizeof *r);
        r->tier = ANIMA_TIER_COMMAND; r->action = ANIMA_ACT_ANSWER; r->confidence = 90;
        snprintf(r->intent, sizeof r->intent, "deny");
        snprintf(r->reply, sizeof r->reply, "%s", en ? "OK, I won't." : "Va bene, lascio stare.");
        return 1;
    }
    return 0;
}

int nucleo_anima_pending_answer(const char *q, const anima_turn_t *turns, int nturns, const char *extra_sys,
                                bool en, anima_result_t *r)
{
    s_resume = false;
    if (!act_pending_resolve(q, en, r)) return 0;
    if (!s_resume || !nucleo_anima_online_available()) return 1;
    s_resume = false;
    // The confirmed step ran outside the agent loop: hand its result back so the task goes on.
    const size_t cap = sizeof r->reply + 400;
    char *cont = malloc(cap);
    anima_result_t *r2 = malloc(sizeof *r2);
    if (cont && r2) {
        snprintf(cont, cap, en ? "%s\nRESULT of the step you asked about (the user confirmed it):\n%s\n"
                                 "Continue the task; when it is complete, answer briefly without ACT."
                               : "%s\nRISULTATO del passo per cui hai chiesto conferma (l'utente ha confermato):\n%s\n"
                                 "Continua il compito; quando e' completo, rispondi in breve senza ACT.", q, r->reply);
        memset(r2, 0, sizeof *r2);
        if (nucleo_anima_online_chat_conv(cont, turns, nturns, extra_sys, en, r2) > 0) {
            char step[200];
            const char *nl = strchr(r->reply, '\n');
            snprintf(step, sizeof step, "%.*s", nl ? (int)(nl - r->reply) : (int)strlen(r->reply), r->reply);
            char *body = strdup(r2->reply);
            *r = *r2;
            if (body) { snprintf(r->reply, sizeof r->reply, "%s\n\n%s", step, body); free(body); }
        }
    }
    free(cont); free(r2);
    return 1;
}

const char *nucleo_anima_act_grammar(bool en)
{
    return en
        ? "DEVICE ACTIONS: when the user asks you to DO something on this device that you can do with these, reply with ONLY the ACT lines, nothing else (one action per line, at most 3, run in order; at most one open_app and one add_event/create_file):\n"
          "ACT open_app <id>   (ids: gallery notes files music video calc terminal settings tasks sysmon camera recorder diag apps secondscreen abc123 pianino)\n"
          "ACT close_app music\nACT set_volume <0-100 | +N | -N>\nACT set_brightness <0-100 | +N | -N>\n"
          "ACT add_event <days from today> <HH:MM or -> <text>\nACT create_file <name.txt> | <short content>\n"
          "ACT remember <a lasting fact about the user or their wishes, one line> [#label]   (when they tell you something worth keeping)\n"
          "ACT forget <words>   (removes the remembered facts containing them, when the user asks)\n"
          "ACT timer <duration> [label] | ACT alarm <HH:MM> [label] | ACT timer list | ACT timer cancel   (they ring offline)\n"
          "ACT rule add {json} | ACT rule list | ACT rule delete <id>   (automations \"every day at 8...\", \"when I write X on Telegram...\": see the automazioni skill)\n"
          "Otherwise answer normally. Never claim you did an action without the ACT line."
        : "AZIONI SUL DISPOSITIVO: se l'utente ti chiede di FARE qualcosa su questo dispositivo che puoi fare con queste, rispondi SOLO con le righe ACT, nient'altro (un'azione per riga, al massimo 3, eseguite in ordine; al massimo un open_app e un add_event/create_file):\n"
          "ACT open_app <id>   (id: gallery notes files music video calc terminal settings tasks sysmon camera recorder diag apps secondscreen abc123 pianino)\n"
          "ACT close_app music\nACT set_volume <0-100 | +N | -N>\nACT set_brightness <0-100 | +N | -N>\n"
          "ACT add_event <giorni da oggi> <HH:MM oppure -> <testo>\nACT create_file <nome.txt> | <contenuto breve>\n"
          "ACT remember <un fatto duraturo sull'utente o i suoi desideri, una riga> [#etichetta]   (quando ti dice qualcosa che vale la pena ricordare)\n"
          "ACT forget <parole>   (toglie i ricordi che le contengono, quando l'utente lo chiede)\n"
          "ACT timer <durata> [etichetta] | ACT alarm <HH:MM> [etichetta] | ACT timer list | ACT timer cancel   (suonano anche offline)\n"
          "ACT rule add {json} | ACT rule list | ACT rule delete <id>   (automazioni \"ogni giorno alle 8...\", \"quando scrivo X su Telegram...\": vedi la skill automazioni)\n"
          "Altrimenti rispondi normalmente. Non dire mai di aver fatto un'azione senza la riga ACT.";
}

// The shell part of the grammar, only when the OS registered a shell. Kept short: it is in every prompt.
#define SHG_EN "SHELL: \"ACT sh <command line>\" runs it on the device, a BusyBox-like POSIX shell: use coreutils as on Linux " \
              "(ls cat head tail grep find sed awk sort uniq wc cut tr xargs du df free ps cp mv rm mkdir touch stat curl wget date), " \
              "pipes ; && || > >> $VAR. Keep output short (| head, grep -c, wc -l). Files live under /sdcard (~ = /sdcard/home). " \
              "Also: diff -u A B, jq -r .a.b FILE (or | jq), rg PATTERN (= grep -rn), ll. " \
              "Code: app check FILE (.lua/.py/.json syntax + bad line), app run NAME (a Lua App script, returns its error). " \
              "NucleoOS extras: sysinfo (the whole board in one call) | vol N | notify TEXT | tg TEXT (Telegram) | " \
              "home: ha say TEXT (Home Assistant Assist), ha ls|find|get|on|off|set, dev ls|on|off|get (Shelly/Tasmota/WLED) | " \
              "store search|info|install|remove ID (app store) | apps (installed programs) | launch ID (open an app) | " \
              "system: cfg (all settings) | cfg KEY [VALUE] (brightness dnd thmode lang scr_timeout ha_url..., applied live), cfg export > ~/cfg.txt / cfg import FILE (backup) | " \
              "wifi status|scan|join SSID PASS | bl (Bluetooth) | usb | update status|check|install (firmware) | ps (services) | " \
              "dmesg (system log, app errors) | sensors | lua/js FILE or -e CODE, python FILE or -c CODE (MicroPython: check which python first) | " \
              ANIMA_SH_TOOLS_EN \
              "GUI of any app: ui (screen as text: [ref] role \"text\" @x,y), input tap @REF|X Y, input text TEXT, " \
              "input keyevent ENTER, input swipe X0 Y0 X1 Y1, home; screenshot (-> ~/shots/*.jpg, then ACT see) for the pixels | " \
              "help CMD (one-line usage). One ACT per reply, on its own line (never inside ``` and never with made-up output); you get the output and may continue (max 12 steps), then answer briefly without ACT. " \
              "When a command fails, read the error and fix the cause (path, option: help CMD); never say a program is missing unless which/apps shows it is.\n" \
              "FILES: write a whole file with\nACT write <path>\n<<<\n<content>\n>>>\nand change one exact passage with\n" \
              "ACT edit <path>\n<<<\n<old text, exactly as in the file>\n===\n<new text>\n>>>\n" \
              "Paths: ~/... (= /sdcard/home), /sdcard/data/..., /sdcard/apps/.... Read a file with ACT sh cat <path> first."
#define SHG_IT "SHELL: \"ACT sh <riga di comando>\" la esegue sul dispositivo, una shell POSIX tipo BusyBox: usa i coreutils come su Linux " \
              "(ls cat head tail grep find sed awk sort uniq wc cut tr xargs du df free ps cp mv rm mkdir touch stat curl wget date), " \
              "pipe ; && || > >> $VAR. Tieni corto l'output (| head, grep -c, wc -l). I file stanno sotto /sdcard (~ = /sdcard/home). " \
              "Anche: diff -u A B, jq -r .a.b FILE (o | jq), rg PATTERN (= grep -rn), ll. " \
              "Codice: app check FILE (sintassi .lua/.py/.json + riga sbagliata), app run NOME (script Lua App, ne restituisce l'errore). " \
              "Extra di NucleoOS: sysinfo (tutta la scheda in un comando) | vol N | notify TESTO | tg TESTO (Telegram) | " \
              "casa: ha say TESTO (Assist di Home Assistant), ha ls|find|get|on|off|set, dev ls|on|off|get (Shelly/Tasmota/WLED) | " \
              "store search|info|install|remove ID (store app) | apps (programmi installati) | launch ID (apre un'app) | " \
              "sistema: cfg (tutte le impostazioni) | cfg CHIAVE [VALORE] (brightness dnd thmode lang scr_timeout ha_url..., subito attive), " \
              "cfg export > ~/cfg.txt / cfg import FILE (backup) | wifi status|scan|join SSID PASS | bl (Bluetooth) | usb | " \
              "update status|check|install (firmware) | ps (servizi) | " \
              "dmesg (log di sistema, errori delle app) | sensors | lua/js FILE o -e CODICE, python FILE o -c CODICE (MicroPython: prima controlla which python) | " \
              ANIMA_SH_TOOLS_IT \
              "GUI di ogni app: ui (schermo come testo: [ref] ruolo \"testo\" @x,y), input tap @REF|X Y, input text TESTO, " \
              "input keyevent ENTER, input swipe X0 Y0 X1 Y1, home; screenshot (-> ~/shots/*.jpg, poi ACT see) per i pixel | " \
              "help CMD (uso in una riga). Un ACT per risposta, su una riga sua (mai dentro ``` e mai con output inventati); ricevi l'output e puoi continuare (max 12 passi), poi rispondi in breve senza ACT. " \
              "Se un comando fallisce, leggi l'errore e correggi la causa (percorso, opzione: help CMD); non dire mai che un programma manca se which/apps non lo conferma.\n" \
              "FILE: scrivi un file intero con\nACT write <percorso>\n<<<\n<contenuto>\n>>>\ne cambia un passaggio esatto con\n" \
              "ACT edit <percorso>\n<<<\n<testo vecchio, identico al file>\n===\n<testo nuovo>\n>>>\n" \
              "Percorsi: ~/... (= /sdcard/home), /sdcard/data/..., /sdcard/apps/.... Prima leggi il file con ACT sh cat <percorso>."
// Build mode keeps a visible todo list in the chat (OpenCode/Claude Code style); plan mode is read-only.
#define SHG_TODO_EN "\nTODO: for a task of 3+ steps, open your first reply with a checklist (- [ ] step) followed by the ACT of the first step, and repeat it, ticked (- [x]), in later replies."
#define SHG_TODO_IT "\nTODO: per un compito di 3+ passi, apri la prima risposta con una checklist (- [ ] passo) seguita dall'ACT del primo passo, e ripetila, spuntata (- [x]), nelle risposte dopo."
#define SHG_PLAN_EN "\nPLAN MODE (read-only): only read and look things up; never write, edit, install or change anything. " \
    "End with a numbered plan as a checklist (- [ ] step); the user starts it with /plan off."
#define SHG_PLAN_IT "\nMODALITA' PIANO (sola lettura): solo leggere e verificare; non scrivere, modificare, installare o cambiare nulla. " \
    "Chiudi con un piano numerato come checklist (- [ ] passo); l'utente lo avvia con /plan off."

const char *nucleo_anima_sh_grammar(bool en)
{
    if (!s_shell) return "";
    const char *base = nucleo_anima_agent_mode() == 2 ? (en ? SHG_EN SHG_PLAN_EN : SHG_IT SHG_PLAN_IT)
                                                      : (en ? SHG_EN SHG_TODO_EN : SHG_IT SHG_TODO_IT);
    if (!s_ws[0]) return base;
    // + the workspace, so "the project" / relative paths mean the folder the user picked
    EXT_RAM_BSS_ATTR static char g[sizeof SHG_IT SHG_PLAN_IT + 320];
    const char *ws = s_ws;
    char shown[170];
    if (!strncmp(ws, NUCLEO_SD_MOUNT "/home", strlen(NUCLEO_SD_MOUNT "/home")))
        snprintf(shown, sizeof shown, "~%s", ws + strlen(NUCLEO_SD_MOUNT "/home"));
    else snprintf(shown, sizeof shown, "%s", ws);
    snprintf(g, sizeof g, en ? "%s\nWORKSPACE: %s - the folder the user is working in: sh starts there, \"the project\" means it; "
                               "for ACT write/edit give the full path (%s/...)."
                             : "%s\nWORKSPACE: %s - la cartella su cui l'utente sta lavorando: la shell parte da li', \"il progetto\" e' questa; "
                               "per ACT write/edit usa il percorso completo (%s/...).", base, shown, shown);
    return g;
}

static bool act_num(const char *s, int lo, int hi, int *v)
{
    char *e; long n = strtol(s, &e, 10);
    while (*e == ' ' || *e == '%') e++;
    if (e == s || *e || n < lo || n > hi) return false;
    *v = (int)n; return true;
}

static int act_plan_from_llm(const char *text, bool en, anima_result_t *r);   // several ACT lines: a plan

int nucleo_anima_act_from_llm(const char *text, bool en, anima_result_t *r)
{
    if (!text || !r) return 0;
    while (*text == ' ' || *text == '\n' || *text == '`') text++;
    { const int pr = act_plan_from_llm(text, en, r); if (pr) return pr; }
    if (ft_args(text)) {
        memset(r, 0, sizeof *r);
        r->tier = ANIMA_TIER_REMOTE; r->action = ANIMA_ACT_ANSWER; r->confidence = 75;
        const int perm = nucleo_anima_permission("write");
        if (perm == 2) {
            snprintf(r->intent, sizeof r->intent, "denied");
            snprintf(r->reply, sizeof r->reply, "%s", en ? "I'm not allowed to write files (write is denied in permissions.json)."
                                                         : "Non ho il permesso di scrivere file (write è negato in permissions.json).");
            return 1;
        }
        if (perm == 1) {
            free(s_pending_blob);
            s_pending_blob = strdup(text);
            pending_mark();
            s_pending_act_ms = act_now_ms();
            r->awaiting = 1;
            snprintf(r->intent, sizeof r->intent, "confirm");
            snprintf(r->state, sizeof r->state, "slot");
            char path[220] = "";
            ft_path(ft_args(text), path, sizeof path);
            snprintf(r->reply, sizeof r->reply, en ? "%s /sdcard%s - shall I go ahead? (yes/no)" : "%s /sdcard%s: procedo? (sì/no)",
                     text[4] != 'e' ? (en ? "Write" : "Scrivo") : (en ? "Edit" : "Modifico"),
                     path[0] ? path + strlen(NUCLEO_SD_MOUNT) : "?");
            return 1;
        }
        snprintf(r->intent, sizeof r->intent, "write");
        nucleo_anima_file_tool(text, en, r->reply, sizeof r->reply);
        return 1;
    }
    if (!strncmp(text, "ACT rule ", 9)) {                   // automations: add {json} | list | delete ID
        const char *a = text + 9;
        while (*a == ' ') a++;
        memset(r, 0, sizeof *r);
        r->tier = ANIMA_TIER_REMOTE; r->action = ANIMA_ACT_ANSWER; r->confidence = 80;
        snprintf(r->intent, sizeof r->intent, "rule");
        if (!strncmp(a, "list", 4)) { nucleo_anima_rules_list(en, r->reply, sizeof r->reply); return 1; }
        if (!strncmp(a, "delete ", 7) || !strncmp(a, "remove ", 7)) {
            char id[64]; int k = 0;
            for (const char *p = a + 7; *p && *p != '\n' && *p != ' ' && k < (int)sizeof id - 1; p++) id[k++] = *p;
            id[k] = 0;
            const int perm = nucleo_anima_permission("rule");   // deleting is a change too: same policy as add
            if (perm == 2) {
                snprintf(r->intent, sizeof r->intent, "denied");
                snprintf(r->reply, sizeof r->reply, "%s", en ? "Automations are denied in permissions.json." : "Le automazioni sono negate in permissions.json.");
                return 1;
            }
            if (perm == 1 && !s_act_confirmed) {
                snprintf(s_pending_act, sizeof s_pending_act, "ACT rule delete %s", id);
                s_pending_act_ms = act_now_ms();
                pending_mark();
                snprintf(r->reply, sizeof r->reply, en ? "Delete the automation %s? (yes/no)" : "Elimino l'automazione %s? (sì/no)", id);
                r->awaiting = 1;
                snprintf(r->intent, sizeof r->intent, "confirm");
                snprintf(r->state, sizeof r->state, "slot");
                return 1;
            }
            const int n = nucleo_anima_rules_delete(id);
            snprintf(r->reply, sizeof r->reply, n ? (en ? "Automation %s deleted." : "Automazione %s eliminata.")
                                                 : (en ? "No automation %s." : "Nessuna automazione %s."), id);
            return 1;
        }
        const char *j = strchr(a, '{'), *e = strrchr(a, '}');
        if (strncmp(a, "add", 3) || !j || !e || e < j) return 0;
        const int perm = nucleo_anima_permission("rule");
        if (perm == 2) {
            snprintf(r->intent, sizeof r->intent, "denied");
            snprintf(r->reply, sizeof r->reply, "%s", en ? "Automations are denied in permissions.json." : "Le automazioni sono negate in permissions.json.");
            return 1;
        }
        const size_t jl = (size_t)(e - j + 1);
        char *blob = malloc(jl + 6);
        if (!blob) return 0;
        memcpy(blob, "RULE ", 5); memcpy(blob + 5, j, jl); blob[jl + 5] = 0;
        if (perm == 1) {
            cJSON *o = cJSON_Parse(blob + 5);
            cJSON *d = o ? cJSON_GetObjectItem(o, "description") : NULL, *id = o ? cJSON_GetObjectItem(o, "id") : NULL;
            snprintf(r->reply, sizeof r->reply, en ? "New automation \"%s\": %s - save it? (yes/no)" : "Nuova automazione \"%s\": %s. La salvo? (sì/no)",
                     cJSON_IsString(id) ? id->valuestring : "?", cJSON_IsString(d) ? d->valuestring : "");
            cJSON_Delete(o);
            free(s_pending_blob);
            s_pending_blob = blob;
            s_pending_act_ms = act_now_ms();
            pending_mark();
            r->awaiting = 1;
            snprintf(r->intent, sizeof r->intent, "confirm");
            snprintf(r->state, sizeof r->state, "slot");
            return 1;
        }
        nucleo_anima_rules_add(blob + 5, en, r->reply, sizeof r->reply);
        free(blob);
        return 1;
    }
    if (strncmp(text, "ACT ", 4)) return 0;
    char line[AG_CONTENT_MAX + 64];
    int n = 0;
    for (const char *p = text + 4; *p && *p != '\n' && *p != '`' && n < (int)sizeof line - 1; p++) line[n++] = *p;
    while (n && line[n-1] == ' ') n--;
    line[n] = 0;
    char tool[24] = "", *args = line;
    int tl = 0;
    while (*args && *args != ' ' && tl < (int)sizeof tool - 1) tool[tl++] = *args++;
    tool[tl] = 0;
    while (*args == ' ') args++;

    anima_result_t a; memset(&a, 0, sizeof a);
    a.tier = ANIMA_TIER_REMOTE; a.confidence = 75;
    snprintf(a.state, sizeof a.state, "tool");
    int v = 0;
    if (!strcmp(tool, "forget")) {                              // "ACT forget dentista Rossi"
        memset(r, 0, sizeof *r);
        r->tier = ANIMA_TIER_REMOTE; r->action = ANIMA_ACT_ANSWER; r->confidence = 80;
        snprintf(r->intent, sizeof r->intent, "forget");
        if (nucleo_anima_permission("remember") == 2) {
            snprintf(r->reply, sizeof r->reply, "%s", en ? "Memory changes are denied in permissions.json." : "Le modifiche alla memoria sono negate in permissions.json.");
            return 1;
        }
        const int n = nucleo_anima_memory_forget(args);
        snprintf(r->reply, sizeof r->reply, n ? (en ? "Forgotten (%d)." : "Dimenticato (%d).") : (en ? "Nothing in memory matches." : "Niente in memoria corrisponde."), n);
        return 1;
    }
    if (!strcmp(tool, "timer") || !strcmp(tool, "alarm")) {   // "ACT timer 10 minuti pasta", "ACT alarm 7:30"
        char q[300];
        snprintf(q, sizeof q, "%s %.280s", tool, args);
        return nucleo_anima_timer_tool(q, en, (long long)time(NULL), r);
    }
    if (!strcmp(tool, "open_app")) {
        const char *id = NULL;
        for (size_t i = 0; i < sizeof APP_ALIAS / sizeof APP_ALIAS[0]; i++) if (!strcmp(APP_ALIAS[i].id, args)) id = APP_ALIAS[i].id;
        if (!id) return 0;
        a.action = ANIMA_ACT_LAUNCH;
        snprintf(a.intent, sizeof a.intent, "open_app");
        snprintf(a.arg, sizeof a.arg, "%s", id);
        snprintf(a.reply, sizeof a.reply, en ? "Opening %s." : "Apro %s.", id);
    } else if (!strcmp(tool, "close_app")) {
        if (strcmp(args, "music") && strcmp(args, "radio")) return 0;
        a.action = ANIMA_ACT_TOOL;
        snprintf(a.intent, sizeof a.intent, "close_app");
        snprintf(a.arg, sizeof a.arg, "%s", args);
        snprintf(a.reply, sizeof a.reply, "%s", en ? "Stopping playback." : "Fermo la riproduzione.");
    } else if (!strcmp(tool, "set_volume") || !strcmp(tool, "set_brightness")) {
        const char sign = (args[0] == '+' || args[0] == '-') ? args[0] : 0;   // "+10" / "-10": a step
        if (!act_num(sign ? args + 1 : args, sign ? 1 : 0, 100, &v)) return 0;
        a.action = ANIMA_ACT_TOOL;
        snprintf(a.intent, sizeof a.intent, "%s", tool);
        if (sign) snprintf(a.arg, sizeof a.arg, "%c%d", sign, v);
        else      snprintf(a.arg, sizeof a.arg, "%d", v);
        snprintf(a.reply, sizeof a.reply, "%s %s%%.", tool[4] == 'v' ? "Volume" : (en ? "Brightness" : "Luminosità"), a.arg);
    } else if (!strcmp(tool, "add_event")) {
        char d[8] = "", t[8] = ""; int used = 0;
        if (sscanf(args, "%7s %7s %n", d, t, &used) < 2 || !used || !args[used]) return 0;
        int off, hh = -1, mm = 0;
        if (!act_num(d, 0, 366, &off)) return 0;
        if (strcmp(t, "-") && (sscanf(t, "%d:%d", &hh, &mm) != 2 || hh < 0 || hh > 23 || mm < 0 || mm > 59)) return 0;
        const char *body = args + used;
        if (hh >= 0) snprintf(s_tool_content, sizeof s_tool_content, "off=%d;time=%02d:%02d;text=%s", off, hh, mm, body);
        else         snprintf(s_tool_content, sizeof s_tool_content, "off=%d;time=;text=%s", off, body);
        a.action = ANIMA_ACT_TOOL;
        snprintf(a.intent, sizeof a.intent, "add_event");
        snprintf(a.arg, sizeof a.arg, "add_event");
        snprintf(a.reply, sizeof a.reply, en ? "Reminder \"%s\"." : "Promemoria \"%s\".", body);
    } else if (!strcmp(tool, "create_file")) {
        char name[64]; int k = 0;
        const char *p = args;
        for (; *p && *p != '|' && *p != ' ' && k < (int)sizeof name - 1; p++) {
            const char c = *p;
            if (!(isalnum((unsigned char)c) || c == '-' || c == '_' || c == '.')) return 0;   // a bare name, no paths
            name[k++] = c;
        }
        name[k] = 0;
        if (!k || name[0] == '.' || strstr(name, "..")) return 0;
        while (*p == ' ') p++;
        if (*p == '|') { p++; while (*p == ' ') p++; }
        const char *folder = a_folder_for_ext(name);
        snprintf(s_tool_content, sizeof s_tool_content, "%s", p);
        a.action = ANIMA_ACT_TOOL;
        snprintf(a.intent, sizeof a.intent, "create_file");
        snprintf(a.arg, sizeof a.arg, "/data/%s/%s", folder ? folder : "Documents", name);
        snprintf(a.reply, sizeof a.reply, en ? "Creating %s." : "Creo %s.", name);
    } else if (!strcmp(tool, "sh")) {
        for (char *m; (m = strstr(args, "ACT sh ")) != NULL;) memmove(m, m + 7, strlen(m + 7) + 1);   // chained steps
        if (!s_shell || !args[0]) return 0;
        const int cls = nucleo_anima_sh_class(args);
        if (cls < 0) {
            memset(r, 0, sizeof *r);
            r->tier = ANIMA_TIER_REMOTE; r->action = ANIMA_ACT_ANSWER; r->confidence = 60;
            snprintf(r->intent, sizeof r->intent, "sh");
            snprintf(r->reply, sizeof r->reply, en ? "\"%s\" needs the Terminal screen: open the Terminal app for it."
                                                   : "\"%s\" richiede lo schermo del Terminale: aprilo dall'app Terminale.", args);
            return 1;
        }
        a.action = ANIMA_ACT_ANSWER;                 // runs right here (below, after the permission check)
        snprintf(a.intent, sizeof a.intent, "sh");
        snprintf(a.arg, sizeof a.arg, "%.*s", (int)sizeof a.arg - 1, args);
        snprintf(a.reply, sizeof a.reply, en ? "Run: %s" : "Eseguo: %s", args);
    } else if (!strcmp(tool, "remember")) {
        if (strlen(args) < 3) return 0;
        a.action = ANIMA_ACT_ANSWER;                 // no device action: the engine keeps it itself
        snprintf(a.intent, sizeof a.intent, "remember");
        snprintf(a.arg, sizeof a.arg, "%.*s", (int)sizeof a.arg - 1, args);
        snprintf(a.reply, sizeof a.reply, en ? "I'll remember: %s" : "Me lo ricordo: %s", args);
    } else return 0;
    snprintf(a.trace, sizeof a.trace, "LLM > ACT %s", tool);
    // OpenCode-style permissions (permissions.json): a model's action may run, wait for a yes, or not.
    const int perm = s_act_confirmed ? 0
                   : !strcmp(tool, "sh") && nucleo_anima_sh_class(args) == 1 ? 0   // read-only: never asks
                   : nucleo_anima_permission(tool);
    if (perm == 2) {
        memset(r, 0, sizeof *r);
        r->tier = ANIMA_TIER_REMOTE; r->action = ANIMA_ACT_ANSWER; r->confidence = 70;
        snprintf(r->intent, sizeof r->intent, "denied");
        snprintf(r->reply, sizeof r->reply, en ? "I'm not allowed to do that (%s is denied in permissions.json)."
                                               : "Non ho il permesso di farlo (%s è negato in permissions.json).", tool);
        snprintf(r->trace, sizeof r->trace, "LLM > ACT %s > deny", tool);
        return 1;
    }
    if (perm == 1) {
        snprintf(s_pending_act, sizeof s_pending_act, "ACT %s", line);
        pending_mark();
        s_pending_act_ms = act_now_ms();
        memset(r, 0, sizeof *r);
        r->tier = ANIMA_TIER_REMOTE; r->action = ANIMA_ACT_ANSWER; r->confidence = 75; r->awaiting = 1;
        snprintf(r->intent, sizeof r->intent, "confirm");
        snprintf(r->state, sizeof r->state, "slot");
        char what[sizeof a.reply];
        snprintf(what, sizeof what, "%s", a.reply);
        size_t wl = strlen(what);
        while (wl && (what[wl-1] == '.' || what[wl-1] == ' ')) what[--wl] = 0;
        snprintf(r->reply, sizeof r->reply, en ? "%s - shall I go ahead? (yes/no)" : "%s: procedo? (sì/no)", what);
        snprintf(r->trace, sizeof r->trace, "LLM > ACT %s > ask", tool);
        return 1;
    }
    if (!strcmp(tool, "sh")) {                       // run it now: the reply is the output
        char *o = malloc(1800);
        const int st = o ? anima_shell_run(args, o, 1800) : -1;
        snprintf(a.reply, sizeof a.reply, "$ %.120s\n%.860s%s", args, o && o[0] ? o : (st == 0 ? "(ok)" : ""),
                 st > 0 ? (en ? "\n(exit status non-zero)" : "\n(uscita con errore)") : st == -1 ? (en ? "(the shell is busy)" : "(la shell è occupata)") : "");
        free(o);
    }
    if (!strcmp(tool, "remember") && !nucleo_anima_memory_add(args))
        snprintf(a.reply, sizeof a.reply, "%s", en ? "I couldn't save that to memory." : "Non sono riuscita a salvarlo in memoria.");
    *r = a;
    return 1;
}

// TEACH (offline durable learning): "ricorda che X è Y" / "remember that X is Y" stores a user fact that
// becomes recallable by paraphrase, fully offline (nucleo_anima_learn.c). The frame is TIGHT — an explicit
// teach lead PLUS a binding copula ("è"/"is"/"significa"/"means") — so a reminder ("ricordami DI comprare
// latte", no copula) falls through to tool_event, and a bare statement never triggers a write. The copula
// is matched on the RAW "è" (0xC3 0xA8), distinct from "e" (and), so "Marco e Luigi" isn't split as a fact.
static int tool_teach(const char *raw, char tok[A_MAX_TOKENS][A_TOK_LEN], int ntok, bool en, anima_result_t *r)
{
    (void)tok; (void)ntok;
    char lo[256]; size_t z = 0;                       // lowercase ASCII, leave UTF-8 bytes (è) intact:
    for (; raw[z] && z + 1 < sizeof lo; z++) lo[z] = (char)tolower((unsigned char)raw[z]);
    lo[z] = 0;                                         // lo[] is byte-aligned with raw[] (tolower keeps length)

    static const char *const LEAD[] = {
        "ricorda che ", "ricordati che ", "impara che ", "imparare che ", "memorizza che ", "ricordare che ",
        "tieni a mente che ", "annota che ", "segnati che ", "sappi che ",
        "remember that ", "teach anima that ", "teach you that ", "teach that ", "learn that ",
        "note that ", "keep in mind that ", NULL };
    const char *rem = NULL;
    for (int i = 0; LEAD[i] && !rem; i++) { const char *p = strstr(lo, LEAD[i]); if (p) rem = p + strlen(LEAD[i]); }
    if (!rem) return 0;                                // no explicit teach frame -> not ours

    // Split subject | fact on the FIRST binding copula in the remainder. "è" via its UTF-8 bytes.
    static const char *const COP[] = { " \xC3\xA8 ", " sono ", " significa ", " vuol dire ", " is ", " are ", " means ", NULL };
    const char *cop = NULL; size_t clen = 0;
    for (int i = 0; COP[i]; i++) {
        const char *p = strstr(rem, COP[i]);
        if (p && (!cop || p < cop)) { cop = p; clen = strlen(COP[i]); }
    }
    if (!cop) return 0;                                // a teach lead with no copula -> abstain, don't guess

    // Extract subject/fact from the ORIGINAL raw at the byte-aligned offsets (preserves case + accents).
    size_t roff = (size_t)(rem - lo), coff = (size_t)(cop - lo);
    char subject[160], fact[360];
    size_t sl = coff - roff;       if (sl >= sizeof subject) sl = sizeof subject - 1;
    memcpy(subject, raw + roff, sl); subject[sl] = 0;
    snprintf(fact, sizeof fact, "%s", raw + coff + clen);
    { size_t a = 0; while (subject[a] == ' ') a++; if (a) memmove(subject, subject + a, strlen(subject + a) + 1);
      for (size_t b = strlen(subject); b && subject[b-1] == ' '; ) subject[--b] = 0; }

    int rc = nucleo_anima_learn_put(subject, fact, en);
    r->tier = ANIMA_TIER_COMMAND; r->action = ANIMA_ACT_ANSWER; r->confidence = 90;
    snprintf(r->intent, sizeof r->intent, "teach");
    snprintf(r->state, sizeof r->state, "tool");
    if (rc == 1)
        snprintf(r->reply, sizeof r->reply, en ? "Got it — I'll remember: %s" : "Imparato — ricorderò: %s", fact);
    else if (rc == -1)
        snprintf(r->reply, sizeof r->reply, en ? "I won't store that — it's temporary information."
                                              : "Non lo memorizzo: è un'informazione temporanea.");
    else
        snprintf(r->reply, sizeof r->reply, en ? "I can't learn that right now." : "Non riesco a impararlo ora.");
    return 1;
}

// Thin adapter: the typed personal-profile tier ("mi chiamo X" / "come mi chiamo") reads only the RAW input.
static int tool_profile(const char *raw, char tok[A_MAX_TOKENS][A_TOK_LEN], int ntok, bool en, anima_result_t *r)
{
    (void)tok; (void)ntok;
    return nucleo_anima_profile(raw, en, r);
}

// Translation tier with the RIGHT precedence: ONLINE-FIRST. In hybrid/online mode (online enabled +
// Wi-Fi) a translation request goes to the teacher LLM (Grok) — full sentences, real quality. The offline
// dictionary is the FLOOR, used only when offline, or if the teacher call fails (no key / heap / network).
// "The base translator is offline-only." A non-translation query never enters here (is_request is 0-FP).
static int tool_translate(const char *raw, char tok[A_MAX_TOKENS][A_TOK_LEN], int ntok, bool en, anima_result_t *r)
{
    (void)tok; (void)ntok;
    if (nucleo_anima_translate_is_request(raw) && nucleo_anima_online_available()) {
        if (nucleo_anima_online_chat(raw, NULL, NULL, en, r)) return 1;   // Grok translated it
        // teacher unavailable/failed -> fall through to the grounded offline dictionary
    }
    return nucleo_anima_translate(raw, en, r);   // offline dictionary
}

// Image-generation request? Requires a GENERATION VERB *and* an IMAGE NOUN together, so it never steals the
// bare "disegna"/"draw" Paint-launch alias (verb only) and defers a note/file/event that merely mentions a
// photo. A question form ("come genero un'immagine?") falls through to knowledge. Detection only: image
// synthesis runs in Paint's Atelier on the browser GPU; ANIMA declines here and points there, so the
// on-device assistant never pretends it produced an image (zero hallucination by construction).
static bool a_is_image_gen(char tok[A_MAX_TOKENS][A_TOK_LEN], int ntok)
{
    static const char *const verb[] = { "genera","generare","generami","generarmi","crea","creare","creami","crearmi",
        "disegna","disegnami","disegnarmi","disegnare","dipingi","dipingimi","dipingermi","dipingere","produci",
        "raffigura","raffigurami","raffigurarmi",
        "generate","create","draw","paint","render","make","produce", NULL };
    static const char *const noun[] = { "immagine","immagini","foto","fotografia","fotografie","disegno","disegni",
        "illustrazione","illustrazioni","ritratto","ritratti","paesaggio","quadro","dipinto","figura","icona","logo",
        "image","images","picture","pictures","photo","photos","photograph","drawing","drawings","illustration",
        "portrait","artwork","painting","landscape", NULL };
    // Defer to the dedicated tools when the object is really a note / file / event, not an image.
    static const char *const other[] = { "nota","note","file","documento","document","promemoria","reminder",
        "evento","event","appuntamento","cartella","folder", NULL };
    bool v = false, n = false, o = false;
    for (int t = 0; t < ntok; t++) {
        for (int i = 0; verb[i];  i++) if (a_match(verb[i],  tok[t])) v = true;
        for (int i = 0; noun[i];  i++) if (a_match(noun[i],  tok[t])) n = true;
        for (int i = 0; other[i]; i++) if (a_match(other[i], tok[t])) o = true;
    }
    if (o) return false;                       // "crea una nota con una foto" -> the note tool owns it
    // Reject a how-to / definition / WHO / HOW-MANY question ("come genero un'immagine?", "cosa disegno?",
    // "chi ha disegnato il logo Nike", "posso ...?") but ALLOW a polite second-person REQUEST ("puoi
    // generarmi ...", "potresti disegnarmi ...", "can you make me an image") — those are genuine generation
    // asks to redirect, not how-tos. Exact match (like a_has_qword) so an inflected non-question word can't
    // accidentally veto a real request. ("che" is NOT here: it is the relative "voglio CHE tu generi ...".)
    static const char *const howto[] = { "come","how","cosa","what","perche","why","quando","when","dove",
        "where","quale","which","chi","who","quanti","quanto","quante","significa","significato","spiega",
        "spiegami","definizione","define","meaning","posso", NULL };
    for (int t = 0; t < ntok; t++) for (int i = 0; howto[i]; i++) if (!strcmp(howto[i], tok[t])) return false;
    // Direct REQUEST forms — "disegnami/dipingimi/raffigurami X" and "draw/paint/sketch me X" — are image-gen
    // even without the literal word "immagine": the -mi / "verb me" form is always a request TO the assistant,
    // never a knowledge statement or an app-open ("apri disegnami" / "chi disegnami" don't occur). This is the
    // safe way to catch "disegnami un gatto" / "draw me a dragon" without the false positives a bare
    // verb+object rule would create on "chi ha disegnato ..." or "crea una password".
    // "-mi" request forms + the inherently-pictorial verbs dipingi/dipingere/raffigura (you never "dipingi una
    // password" or "raffigura una conclusione") → image-gen with just an object, no need for the word "immagine".
    // (Bare "disegna"/"crea" are NOT here: "disegna la tua conclusione" / "crea un account" must stay safe.)
    static const char *const reqv[] = { "disegnami","disegnarmi","dipingimi","dipingermi","dipingi","dipingere",
        "raffigurami","raffigurarmi","raffigura", NULL };
    bool req = false, me = false, dps = false;
    for (int t = 0; t < ntok; t++) {
        for (int i = 0; reqv[i]; i++) if (!strcmp(reqv[i], tok[t])) req = true;
        if (!strcmp(tok[t], "me")) me = true;
        if (a_match("draw", tok[t]) || a_match("paint", tok[t]) || a_match("sketch", tok[t])) dps = true;
    }
    if (req && ntok >= 2) return true;
    if (dps && me && ntok >= 3) return true;
    return v && n;
}

// Tool: image-generation gate. Grounded decline + redirect to Paint's Atelier (text only, never a side
// effect: the web app turns this into an "Open Paint" button, the native app shows the text). Runs FIRST in
// TOOLS[] so "genera/disegna un'immagine di X" can never be mis-scored as open_app(paint) by the intent table.
static int tool_image_gen(const char *raw, char tok[A_MAX_TOKENS][A_TOK_LEN], int ntok, bool en, anima_result_t *r)
{
    (void)raw;
    if (!a_is_image_gen(tok, ntok)) return 0;
    r->tier = ANIMA_TIER_COMMAND; r->action = ANIMA_ACT_ANSWER; r->confidence = 80;
    snprintf(r->intent, sizeof(r->intent), "image_gen");
    snprintf(r->state,  sizeof(r->state),  "idle");
    snprintf(r->reply,  sizeof(r->reply), en
        ? "Images are generated in the Atelier studio inside the Paint app - it runs on your browser's GPU, not here in chat. Open Paint, type what you want (or sketch it), press Generate, and the picture is saved on the device."
        : "Le immagini si generano nello studio Atelier dell'app Paint: gira sulla GPU del browser, non qui in chat. Apri Paint, scrivi cosa vuoi (o fai uno schizzo), premi Genera e l'immagine viene salvata sul dispositivo.");
    return 1;
}


// Order matters: IMAGE-GEN first (a grounded decline + redirect, so "genera/disegna un'immagine di X" can't be
// mis-scored as open_app(paint) by the intent table below), then PROFILE (first-person self-facts
// "mi chiamo X"/"come mi chiamo", deterministic), then
// TEACH (its tight frame ignores everything that isn't an explicit "ricorda che X è Y"), then schedule
// (reminder/event) and compose-then-act note (they read the RAW input), then the plain empty-file create,
// the device-settings tool, the offline IT<->EN translator, and finally the unified math agent.
// "il volume è troppo alto", "lo schermo è troppo luminoso", "the sound is too loud": a complaint about a
// level IS the request to correct it, by one step. Only with "troppo/too" + a direction word, and never as a
// question ("perché il volume è troppo alto?").
static int tool_complaint(const char *raw, char tok[A_MAX_TOKENS][A_TOK_LEN], int ntok, bool en, anima_result_t *r)
{
    if (strchr(raw, '?')) return 0;
    static const char *const VOL[] = { "volume","audio","suono","musica","sound","music", NULL };
    static const char *const BRI[] = { "luminosita","schermo","luce","display","brightness","screen", NULL };
    static const char *const UP_IS_BAD[] = { "alto","alta","forte","rumoroso","luminoso","luminosa","chiaro","chiara",
                                             "loud","high","bright", NULL };
    static const char *const DOWN_IS_BAD[] = { "basso","bassa","debole","piano","scuro","scura","buio","fioco","fioca",
                                               "quiet","low","dark","dim", NULL };
    int kind = 0, dir = 0;
    bool too = false;
    for (int t = 0; t < ntok; t++) {
        if (!strcmp(tok[t], "perche") || !strcmp(tok[t], "why") || !strcmp(tok[t], "come")) return 0;
        if (!strcmp(tok[t], "troppo") || !strcmp(tok[t], "too")) too = true;
        for (int i = 0; VOL[i]; i++) if (a_match(VOL[i], tok[t])) kind = kind ? kind : 1;
        for (int i = 0; BRI[i]; i++) if (a_match(BRI[i], tok[t])) kind = kind ? kind : 2;
        if (too) {
            for (int i = 0; UP_IS_BAD[i]; i++) if (!strcmp(UP_IS_BAD[i], tok[t])) dir = -1;
            for (int i = 0; DOWN_IS_BAD[i]; i++) if (!strcmp(DOWN_IS_BAD[i], tok[t])) dir = +1;
        }
    }
    if (!kind || !dir || !too) return 0;
    r->tier = ANIMA_TIER_COMMAND; r->action = ANIMA_ACT_TOOL; r->confidence = 85;
    snprintf(r->intent, sizeof r->intent, "%s", kind == 1 ? "set_volume" : "set_brightness");
    snprintf(r->arg, sizeof r->arg, "%s", dir < 0 ? "-10" : "+10");
    snprintf(r->state, sizeof r->state, "tool");
    if (kind == 1) snprintf(r->reply, sizeof r->reply, "%s", dir < 0 ? (en ? "Turning the volume down." : "Abbasso il volume.")
                                                                     : (en ? "Turning the volume up." : "Alzo il volume."));
    else           snprintf(r->reply, sizeof r->reply, "%s", dir < 0 ? (en ? "Dimming the screen." : "Abbasso la luminosità.")
                                                                     : (en ? "Brightening the screen." : "Alzo la luminosità."));
    return 1;
}

static int tool_timer(const char *raw, char tok[A_MAX_TOKENS][A_TOK_LEN], int ntok, bool en, anima_result_t *r)
{
    (void)tok; (void)ntok;
    return nucleo_anima_timer_tool(raw, en, (long long)time(NULL), r);
}

static const a_tool_t TOOLS[] = {
    { "image_gen",      false, tool_image_gen },
    { "timer",          false, tool_timer },
    { "profile",        true,  tool_profile },
    { "teach",          true,  tool_teach },
    { "add_event",      true,  tool_event },
    { "create_file",    true,  tool_note },
    { "create_file",    true,  tool_create_file },
    { "set_volume",     true,  tool_complaint },   // "il volume è troppo alto" -> one step down
    { "set_brightness", true,  tool_setting },
    { "translate",      false, tool_translate },
    { "math",           false, tool_math },
};

static int tools_dispatch(const char *raw, char tok[A_MAX_TOKENS][A_TOK_LEN], int ntok, bool en, anima_result_t *r)
{
    for (size_t i = 0; i < sizeof(TOOLS) / sizeof(TOOLS[0]); i++) {
        if (s_dry_run && (!strcmp(TOOLS[i].name, "profile") || !strcmp(TOOLS[i].name, "teach") ||
                          !strcmp(TOOLS[i].name, "translate"))) continue;
        if (TOOLS[i].try_fn(raw, tok, ntok, en, r)) return 1;
    }
    return 0;
}

// FSM AWAITING_SLOT: a previous turn asked for a missing argument. Try to satisfy it from
// this input. Returns 1 if consumed. Bails out (clearing the slot) if the input is clearly
// a different command, so the user is never trapped in a half-finished tool call.
static int try_pending_slot(const char *raw, char tok[A_MAX_TOKENS][A_TOK_LEN], int ntok, bool en, anima_result_t *r)
{
    if (!s_session.pending_tool[0]) return 0;
    if (strcmp(s_session.pending_tool, "create_file") == 0) {
        if (a_is_followup_open(tok, ntok)) { s_session.pending_tool[0] = 0; s_session.pending_slot[0] = 0; return 0; }

        // AWAITING_SLOT(folder): the filename is stashed; the user is telling us where to put it.
        if (strcmp(s_session.pending_slot, "folder") == 0) {
            const char *folder = a_folder_from_words(tok, ntok);
            if (folder) {
                char name[48]; snprintf(name, sizeof(name), "%s", s_session.pending_arg);
                s_session.pending_tool[0] = 0; s_session.pending_slot[0] = 0; s_session.pending_arg[0] = 0;
                a_emit_create(name, folder, en, r);
                return 1;
            }
            s_session.pending_tool[0] = 0; s_session.pending_slot[0] = 0; s_session.pending_arg[0] = 0;  // bail
            return 0;
        }

        // AWAITING_SLOT(filename): read a name, then route it (which may in turn ask for a folder).
        char name[40];
        bool got = a_extract_filename(raw, name, sizeof(name));
        if (!got && ntok == 1) {                       // a single bare word -> use it as the name
            int o = 0;
            for (const char *q = tok[0]; *q && o < (int)sizeof(name) - 5; q++)
                if (isalnum((unsigned char)*q) || *q == '_' || *q == '-') name[o++] = *q;
            name[o] = 0;
            if (o > 0) { if (!strchr(name, '.')) memcpy(name + o, ".txt", 5); got = true; }
        }
        if (got) {
            s_session.pending_tool[0] = 0; s_session.pending_slot[0] = 0;
            a_emit_create(name, NULL, en, r);
            return 1;
        }
        s_session.pending_tool[0] = 0; s_session.pending_slot[0] = 0;   // nothing usable -> drop
    }
    return 0;
}

// Collect up to `cap` DISTINCT app ids named in the query (for the open_app clarify path).
static int a_resolve_apps(char tok[A_MAX_TOKENS][A_TOK_LEN], int ntok, const char *out[], int cap)
{
    int n = 0;
    for (size_t i = 0; i < sizeof(APP_ALIAS) / sizeof(APP_ALIAS[0]) && n < cap; i++) {
        bool hit = false;
        for (int j = 0; j < A_MAX_ALIAS && APP_ALIAS[i].alias[j] && !hit; j++)
            for (int t = 0; t < ntok; t++)
                if (a_match(APP_ALIAS[i].alias[j], tok[t])) { hit = true; break; }
        if (hit) {
            bool dup = false; for (int k = 0; k < n; k++) if (out[k] == APP_ALIAS[i].id) dup = true;
            if (!dup) out[n++] = APP_ALIAS[i].id;
        }
    }
    return n;
}

// ---- public API -------------------------------------------------------------

static bool s_ready;

esp_err_t nucleo_anima_init(const char *lang)
{
    // ONE-SHOT. Both device surfaces (the ANIMA app worker and the web worker) call this before
    // every query "to be safe"; re-running it re-parsed the encoder header from SD, reloaded the
    // session (clobbering the live context) and UNLOADED the L1 index — while the other worker
    // could be mid-search on that very index (fread on a closed FILE*, freed directory).
    // Serialized on the spine gate so a query in flight on the other task finishes first.
    if (s_inited) return ESP_OK;
    bool locked = false;
    for (int i = 0; i < 500 && !(locked = nucleo_anima_try_lock()); i++) vTaskDelay(pdMS_TO_TICKS(10));   // <= 5 s
    if (s_inited) { if (locked) nucleo_anima_unlock(); return ESP_OK; }
    if (!locked) return ESP_ERR_TIMEOUT;   // a query has held the gate for 5 s: the caller retries later
    s_inited = true;
    (void)lang;                            // the L0 tables carry both languages; the reply language is per query
    s_ready = true;
    ESP_LOGI(TAG, "L0 ready (%d intents)", (int)(sizeof(INTENTS) / sizeof(INTENTS[0])));
    nucleo_anima_l1_init();    // best-effort: semantic tier if the SD packs are present
    session_load();            // restore conversational context from a previous boot (best-effort)
    ctx_load();                // ...and the conversation window + its compacted summary
    units_load();              // restore user-defined units learned in a previous session
    nucleo_anima_unlock();
    return ESP_OK;
}

// L0: cheap keyword/intent tier (returns tier NONE if not confident). `en` picks the reply
// language; ANIMA understands both regardless (the keyword tables hold IT+EN triggers).
static anima_result_t l0_query(const char *input, bool en)
{
    anima_result_t r = { 0 };
    r.tier = ANIMA_TIER_NONE;
    r.action = ANIMA_ACT_NONE;
    snprintf(r.state, sizeof(r.state), "idle");
    if (!s_ready || !input) return r;

    char tok[A_MAX_TOKENS][A_TOK_LEN];
    int ntok = a_tokenize(input, tok);
    if (ntok == 0) return r;

    // PARAPHRASES (tools/anima_phrases.txt): "più forte", "non sento niente", "louder" -> the canonical
    // phrase this function already understands ("alza il volume"). One rewrite, never a chain; only on
    // an exact table hit, so an unlisted sentence keeps its own path. `corrected` shows what was understood.
    {
        static bool s_in_paraphrase;
        const char *canon = s_in_paraphrase ? NULL : a_paraphrase(input, en);
        if (canon) {
            s_in_paraphrase = true;
            anima_result_t c = l0_query(canon, en);
            s_in_paraphrase = false;
            if (c.tier != ANIMA_TIER_NONE) {
                if (!c.corrected[0]) snprintf(c.corrected, sizeof c.corrected, "%s", canon);
                return c;
            }
        }
    }

    // HOME: before CLOSE, so "chiudi tutto" goes to the launcher instead of looking for an app called "tutto".
    if (a_is_go_home(tok, ntok)) {
        r.tier = ANIMA_TIER_COMMAND; r.action = ANIMA_ACT_TOOL; r.confidence = 88;
        snprintf(r.intent, sizeof r.intent, "go_home");
        snprintf(r.arg, sizeof r.arg, "home");
        snprintf(r.state, sizeof r.state, "tool");
        snprintf(r.reply, sizeof r.reply, en ? "Going back to the Home screen." : "Torno alla Home.");
        return r;
    }
    const int mc = a_media_ctl(tok, ntok);
    if (mc) {
        r.tier = ANIMA_TIER_COMMAND; r.action = ANIMA_ACT_TOOL; r.confidence = 88;
        snprintf(r.intent, sizeof r.intent, "%s", mc == 1 ? "media_pause" : "media_resume");
        snprintf(r.arg, sizeof r.arg, "music");
        snprintf(r.state, sizeof r.state, "tool");
        snprintf(r.reply, sizeof r.reply, "%s", mc == 1 ? (en ? "Pausing." : "Metto in pausa.")
                                                        : (en ? "Resuming." : "Riprendo la riproduzione."));
        return r;
    }

    // CLOSE/EXIT a named app — must NEVER be confused with OPEN. Without this, "chiudi il calendario"
    // matched the launch card and OPENED the very app (inverse action). Emit a close action; if the
    // executor lacks close it is an inert no-op — never the opposite open. Skipped on a how-to question.
    {
        static const char *const closev[] = { "chiudi","chiudere","esci","uscire","termina","terminare",
                                               "spegni","ferma","stoppa","close","exit","quit","stop", NULL };
        static const char *const setw[]   = { "volume","audio","suono","luminosita","schermo","luce","brightness", NULL };
        // ...nor inside the TEXT of a note / reminder: "crea una nota con scritto ... chiudi la finestra" is
        // content, never a command to close an app.
        static const char *const contentw[] = { "crea","scrivi","nota","annota","ricordami","promemoria","segna",
                                                 "scritto","testo","create","write","note","remind", NULL };
        bool wantclose = false, isq = false, is_set = false, content_first = false;
        for (int t = 0; t < ntok; t++) {
            if (a_qword(tok[t])) isq = true;
            bool cv = false;
            if (!a_is_app_word(tok[t]))
                for (int i = 0; closev[i]; i++) if (a_match(closev[i], tok[t])) { wantclose = true; cv = true; }
            if (!wantclose && !cv) for (int i = 0; contentw[i]; i++) if (!strcmp(contentw[i], tok[t])) content_first = true;
            for (int i = 0; setw[i];   i++) if (a_match(setw[i],   tok[t])) is_set = true;   // "spegni l'audio" -> mute, not close
        }
        if (content_first) wantclose = false;
        if (wantclose && !isq && !is_set) {
            const char *app = a_resolve_app(tok, ntok);
            if (app) {
                r.tier = ANIMA_TIER_COMMAND; r.action = ANIMA_ACT_TOOL; r.confidence = 88;
                snprintf(r.intent, sizeof r.intent, "close_app");
                snprintf(r.arg, sizeof r.arg, "%s", app);
                snprintf(r.state, sizeof r.state, "tool");
                snprintf(r.reply, sizeof r.reply, en ? "Closing %s." : "Chiudo %s.", app);
                return r;
            }
        }
    }

    // FSM AWAITING_SLOT: finish a tool call whose argument we asked for last turn.
    if (try_pending_slot(input, tok, ntok, en, &r)) return r;

    // Tool registry (function calling): create_file, calc, ... Checked before intents because
    // tools read the RAW input (filenames, expressions) the normalizer would otherwise mangle.
    if (tools_dispatch(input, tok, ntok, en, &r)) return r;

    // Computed-from-state, executor-filled: today's calendar agenda, and the dynamic capabilities
    // answer. Checked before the intent table so "cosa devo fare oggi" beats the date intent ("oggi").
    if (a_is_agenda(tok, ntok)) {
        r.tier = ANIMA_TIER_COMMAND; r.action = ANIMA_ACT_SYSTEM; r.confidence = 85;
        snprintf(r.intent, sizeof(r.intent), "agenda");
        int from, days; a_agenda_range(tok, ntok, &from, &days);
        snprintf(r.arg, sizeof(r.arg), "agenda:%d:%d", from, days);   // days from today, how many
        snprintf(r.reply, sizeof(r.reply), "{value}");
        snprintf(r.state, sizeof(r.state), "tool");
        return r;
    }
    if (a_is_capabilities(tok, ntok)) {
        r.tier = ANIMA_TIER_COMMAND; r.action = ANIMA_ACT_SYSTEM; r.confidence = 80;
        snprintf(r.intent, sizeof(r.intent), "capabilities");
        snprintf(r.arg, sizeof(r.arg), "capabilities");
        snprintf(r.reply, sizeof(r.reply), "{value}");
        snprintf(r.state, sizeof(r.state), "tool");
        return r;
    }
    if (a_is_network(tok, ntok)) {
        r.tier = ANIMA_TIER_COMMAND; r.action = ANIMA_ACT_SYSTEM; r.confidence = 82;
        snprintf(r.intent, sizeof(r.intent), "network");
        snprintf(r.arg, sizeof(r.arg), "network");
        snprintf(r.reply, sizeof(r.reply), "{value}");
        snprintf(r.state, sizeof(r.state), "tool");
        return r;
    }
    if (a_is_ram(tok, ntok)) {
        r.tier = ANIMA_TIER_COMMAND; r.action = ANIMA_ACT_SYSTEM; r.confidence = 82;
        snprintf(r.intent, sizeof(r.intent), "ram");
        snprintf(r.arg, sizeof(r.arg), "ram");
        snprintf(r.reply, sizeof(r.reply), "{value}");
        snprintf(r.state, sizeof(r.state), "tool");
        return r;
    }

    // Follow-up: "aprilo" / "open it" -> reopen the most recent file/app from utility memory.
    // "chiudilo" / "close it" -> close the last-opened app (the close mirror of the "aprilo" follow-up).
    if (a_is_followup_close(tok, ntok) && s_mem.last_app[0]) {
        r.tier = ANIMA_TIER_COMMAND; r.action = ANIMA_ACT_TOOL; r.confidence = 82; r.from_memory = 1;
        snprintf(r.intent, sizeof(r.intent), "close_app");
        snprintf(r.state, sizeof(r.state), "followup");
        snprintf(r.arg, sizeof(r.arg), "%s", s_mem.last_app);
        snprintf(r.reply, sizeof(r.reply), en ? "Closing %s." : "Chiudo %s.", s_mem.last_app);
        return r;
    }

    if (a_is_followup_open(tok, ntok)) {
        bool have_app = s_mem.last_app[0], have_file = s_mem.last_file[0];
        bool use_file = have_file && (s_mem.last_kind == 'f' || !have_app);   // recency, else fall back
        if (use_file) {
            r.tier = ANIMA_TIER_COMMAND; r.action = ANIMA_ACT_TOOL; r.confidence = 80; r.from_memory = 1;
            snprintf(r.intent, sizeof(r.intent), "open_file");
            snprintf(r.state, sizeof(r.state), "followup");
            snprintf(r.arg, sizeof(r.arg), "%s", s_mem.last_file);
            const char *bn = strrchr(s_mem.last_file, '/'); bn = bn ? bn + 1 : s_mem.last_file;
            snprintf(r.reply, sizeof(r.reply), en ? "Opening %s." : "Apro %s.", bn);
            return r;
        }
        if (have_app) {
            r.tier = ANIMA_TIER_COMMAND; r.action = ANIMA_ACT_LAUNCH; r.confidence = 80; r.from_memory = 1;
            snprintf(r.intent, sizeof(r.intent), "open_app");
            snprintf(r.state, sizeof(r.state), "followup");
            snprintf(r.arg, sizeof(r.arg), "%s", s_mem.last_app);
            snprintf(r.reply, sizeof(r.reply), en ? "Opening %s." : "Apro %s.", s_mem.last_app);
            return r;
        }
        // nothing remembered yet -> fall through to normal handling
    }

    // FSM CLARIFY resolution: if last turn we offered two apps, an ordinal ("il primo") or the
    // app's own name now picks one — without re-asking. The signal we'd normally throw away.
    if (s_session.clarify_opt[0][0]) {
        int pick = -1;
        for (int t = 0; t < ntok; t++) {
            if (!strcmp(tok[t], "primo") || !strcmp(tok[t], "prima") || !strcmp(tok[t], "first") ||
                !strcmp(tok[t], "uno")   || !strcmp(tok[t], "one"))   pick = 0;
            if (!strcmp(tok[t], "secondo") || !strcmp(tok[t], "seconda") || !strcmp(tok[t], "second") ||
                !strcmp(tok[t], "due")     || !strcmp(tok[t], "two"))     pick = 1;
        }
        if (pick < 0) {
            const char *a = a_resolve_app(tok, ntok);
            for (int i = 0; i < 2; i++) if (a && !strcmp(a, s_session.clarify_opt[i])) pick = i;
        }
        if (pick >= 0) {
            r.tier = ANIMA_TIER_COMMAND; r.action = ANIMA_ACT_LAUNCH; r.confidence = 85; r.from_memory = 1;
            snprintf(r.state, sizeof(r.state), "clarify");
            snprintf(r.intent, sizeof(r.intent), "open_app");
            snprintf(r.arg, sizeof(r.arg), "%s", s_session.clarify_opt[pick]);
            snprintf(r.reply, sizeof(r.reply), en ? "Opening %s." : "Apro %s.", r.arg);
            s_session.clarify_opt[0][0] = 0; s_session.clarify_opt[1][0] = 0;
            return r;
        }
        s_session.clarify_opt[0][0] = 0; s_session.clarify_opt[1][0] = 0;   // not a resolution -> drop
    }

    // Pick the best and runner-up intents.
    const a_intent_t *best = NULL;
    int best_score = 0, second_score = 0;
    for (size_t i = 0; i < sizeof(INTENTS) / sizeof(INTENTS[0]); i++) {
        int s = a_score(&INTENTS[i], tok, ntok);
        // whoami keys on bare "chi"/"who" -> guard it to genuine self-questions so "chi è <entity>"
        // falls through to the knowledge/online tier instead of replying "Sono ANIMA".
        if (s > 0 && !strcmp(INTENTS[i].id, "whoami") && !a_whoami_self(tok, ntok)) s = 0;
        // Ambient state intents must be framed, else an incidental "oggi"/"time"/"year"/"giorno"
        // answers the date/clock/year from device state ("favorite band of all time" -> the time).
        if (s > 0 && !l0_legacy() &&
            (!strcmp(INTENTS[i].id,"date") || !strcmp(INTENTS[i].id,"time") ||
             !strcmp(INTENTS[i].id,"year") || !strcmp(INTENTS[i].id,"season") ||
             !strcmp(INTENTS[i].id,"battery")) &&
            !a_ambient_ok(&INTENTS[i], tok, ntok, s)) s = 0;
        // storage has its own guard: "spazio" is ambiguous (disk vs physical space) -> see a_storage_ok.
        if (s > 0 && !l0_legacy() && !strcmp(INTENTS[i].id,"storage") && !a_storage_ok(tok, ntok)) s = 0;
        // version must be about the DEVICE firmware, not "la versione di python" (knowledge/unknowable).
        if (s > 0 && !l0_legacy() && !strcmp(INTENTS[i].id,"version") && !a_version_ok(tok, ntok)) s = 0;
        // uptime keys must hit EXACTLY (fuzzy "ultimo"~"uptime" answered "Acceso da …").
        if (s > 0 && !strcmp(INTENTS[i].id,"uptime") && !a_uptime_ok(tok, ntok)) s = 0;
        // greetings/thanks answer only when the query IS the greeting (a long sentence that
        // merely starts with "ciao"/"grazie" is a real question for the higher tiers).
        if (s > 0 && (!strcmp(INTENTS[i].id,"greeting") || !strcmp(INTENTS[i].id,"thanks"))
            && ntok > 3) s = 0;
        if (s > best_score) { second_score = best_score; best_score = s; best = &INTENTS[i]; }
        else if (s > second_score) { second_score = s; }
    }
    if (!best || best_score == 0) return r;

    // Confidence: grows with matched keywords, penalized when a runner-up ties (ambiguous).
    int conf = 45 + 20 * best_score;
    if (best_score == second_score) conf -= 25;
    if (conf > 100) conf = 100;

    // open_app needs a resolvable app, else it isn't really a command. When the query names
    // TWO different apps it's genuinely ambiguous -> ask instead of guessing (uncertainty
    // policy). The two candidates are remembered so an ordinal/name resolves it next turn.
    const char *app = NULL;
    if (best->action == ANIMA_ACT_LAUNCH && best->arg == NULL) {
        const char *apps[2]; int na = a_resolve_apps(tok, ntok, apps, 2);
        if (na == 0) return r;              // "apri" with no known app -> let higher tiers try
        if (na >= 2) {
            r.tier = ANIMA_TIER_COMMAND; r.action = ANIMA_ACT_ANSWER; r.awaiting = 1; r.confidence = 60;
            snprintf(r.state, sizeof(r.state), "clarify");
            snprintf(r.intent, sizeof(r.intent), "clarify");
            snprintf(r.reply, sizeof(r.reply), en ? "Which one — %s or %s?" : "Quale dei due apro, %s o %s?", apps[0], apps[1]);
            snprintf(s_session.clarify_opt[0], sizeof(s_session.clarify_opt[0]), "%s", apps[0]);
            snprintf(s_session.clarify_opt[1], sizeof(s_session.clarify_opt[1]), "%s", apps[1]);
            return r;
        }
        if (a_desire_only_launch(tok, ntok)) return r;   // "vorrei sapere perché…" → knowledge, not a launch
        if (a_question_not_launch(tok, ntok)) return r;  // "what team did X play for" → not a launch
        if (a_launch_is_degenerate(tok, ntok)) return r; // bare "play"/"player" → ambiguous noun, not "open X"
        app = apps[0];
        conf += 10;
        if (conf > 100) conf = 100;
    }

    if (conf < 55) return r;                 // gate: not confident enough for L0

    // Build the result.
    r.tier   = ANIMA_TIER_COMMAND;
    r.action = best->action;
    r.confidence = conf;
    snprintf(r.intent, sizeof(r.intent), "%s", best->id);

    const char *rep = en ? (best->reply_en ? best->reply_en : best->reply_it) : best->reply_it;
    switch (best->action) {
        case ANIMA_ACT_LAUNCH:
            snprintf(r.arg, sizeof(r.arg), "%s", app ? app : (best->arg ? best->arg : ""));
            snprintf(r.reply, sizeof(r.reply), en ? "Opening %s." : "Apro %s.", r.arg);
            break;
        case ANIMA_ACT_SYSTEM:
            snprintf(r.arg, sizeof(r.arg), "%s", best->arg ? best->arg : "");
            snprintf(r.reply, sizeof(r.reply), "%s", rep ? rep : "");
            break;
        case ANIMA_ACT_ANSWER:
            snprintf(r.reply, sizeof(r.reply), "%s", rep ? rep : "");
            break;
        default: break;
    }
    return r;
}

// Bilingual typo tolerance (spellfix) + foreign-script output cleanup live in anima_text.c
// (pure text utilities, extracted for scalability). Declared in anima_internal.h.


// Strip a conversational knowledge lead-in to the bare TOPIC ("cosa sai di X"->"X", "do you know
// X"->"X") so L1 matches a card's title / "cos'è X" asks regardless of how the user opened the
// question. ONE rule for every card, current and future — scalable, no per-card phrasing bloat.
// Defined after a_norm_phrase (below); forward-declared here. 0=none, 1=formal (L0-first), 2=conversational (L1-first).
void a_norm_phrase(const char *raw, char *out, size_t cap);
static int a_topic_strip(const char *q, char *out, size_t outsz);

// Conversational KNOWLEDGE->SKILL bridge: after telling the user what a topic IS, proactively offer the
// matching skill ANIMA can DO with it + invite a follow-up. Turns retrieval into a dialogue ("so cos'è la
// fisica E so calcolartela"). Curated map (skills are few & known) -> scalable, no per-card data. Appends
// to the knowledge reply only when it fits and isn't already present.
// Shared KNOWLEDGE<->SKILL map: a topic whose concept ANIMA both KNOWS (a card) and can DO (a solver).
// `bare` = the keyword the user types as a standalone topic ("fisica") -> used for ambiguity detection.
// `offer` = appended after a knowledge answer. `ask` = the clarify question when the topic is bare/ambiguous.
typedef struct { const char *bare[5]; const char *kw[8]; const char *offer_it, *offer_en, *ask_it, *ask_en; } a_skill_t;
static const a_skill_t SKILLS[] = {
    {{"fisica","fisico","physics",NULL},{"fisica","fisico","physics","cinematica","dinamica",NULL},
     " Inoltre ho una skill di fisica: calcolo forza, energia, velocità e accelerazione — dammi i dati.",
     " I also have a physics skill: I compute force, energy, speed and acceleration — give me the values.",
     "Posso dirti cos'è la fisica, oppure risolverti un problema (forza, energia, velocità). Quale ti serve?",
     "I can tell you what physics is, or solve a physics problem (force, energy, speed). Which do you need?"},
    {{"matematica","mathematics","aritmetica",NULL},{"matematica","mathematics","aritmetica","equazione","equazioni",NULL},
     " Inoltre posso fare calcoli e risolvere equazioni: scrivimi l'operazione.",
     " I can also do calculations and solve equations: just type the expression.",
     "Vuoi sapere cos'è la matematica, o devo fare un calcolo? Scrivimi pure l'operazione.",
     "Do you want to know what mathematics is, or should I compute something? Just type the expression."},
    {{"geometria","geometry",NULL},{"geometria","geometry","poligono","triangolo",NULL},
     " Inoltre ho una skill di geometria: aree, perimetri e volumi — dammi le misure.",
     " I also have a geometry skill: areas, perimeters and volumes — give me the measures.",
     "Posso spiegarti cos'è la geometria, oppure calcolare aree/perimetri/volumi. Cosa preferisci?",
     "I can explain what geometry is, or compute areas/perimeters/volumes. Which would you like?"},
    {{"vettore","vettori","vector","vectors"},{"vettore","vettori","vector","vectors",NULL},
     " Inoltre calcolo somma, modulo e prodotto di vettori — dammi le componenti.",
     " I can also compute vector sum, magnitude and product — give me the components.",
     "Vuoi la definizione di vettore, o devo calcolare (somma, modulo, prodotto)? Dimmi pure.",
     "Do you want the definition of vector, or should I compute (sum, magnitude, product)? Tell me."},
    {{"elettronica","electronics",NULL},{"elettronica","electronics","resistenza","tensione","corrente","ohm",NULL},
     " Inoltre applico la legge di Ohm (V=R·I): dammi due valori e ricavo il terzo.",
     " I also apply Ohm's law (V=R·I): give me two values and I'll find the third.",
     "Posso dirti cos'è l'elettronica, oppure applicare la legge di Ohm (V=R·I). Quale ti serve?",
     "I can tell you what electronics is, or apply Ohm's law (V=R·I). Which do you need?"},
    {{"statistica","statistics",NULL},{"statistica","statistics","statistico",NULL},
     " Inoltre calcolo media, mediana e deviazione standard — dammi i numeri.",
     " I also compute mean, median and standard deviation — give me the numbers.",
     "Vuoi sapere cos'è la statistica, o devo calcolare (media, mediana, deviazione)? Dammi i numeri.",
     "Do you want to know what statistics is, or should I compute (mean, median, deviation)? Give me the numbers."},
    {{"meteorologia",NULL},{"meteorologia","meteo","weather","clima",NULL},
     " Inoltre ti do il meteo attuale di una città — dimmi quale.",
     " I also give you the current weather for a city — tell me which.", NULL, NULL},   // no clarify: bare "meteo" -> wants weather
};

// After a knowledge answer, offer the matching skill + invite a follow-up. Turns retrieval into dialogue
// ("so cos'è la fisica E so calcolartela"). Appends only when it fits (clips a long fact to make room).
static void a_offer_skill(anima_result_t *r, const char *topic, bool en)
{
    if (!r || !r->reply[0]) return;
    char nq[160]; a_norm_phrase(topic, nq, sizeof nq);
    for (size_t i = 0; i < sizeof(SKILLS)/sizeof(SKILLS[0]); i++) {
        bool hit = false;
        for (int k = 0; SKILLS[i].kw[k]; k++) { char pat[24]; snprintf(pat, sizeof pat, " %s ", SKILLS[i].kw[k]); if (strstr(nq, pat)) { hit = true; break; } }
        if (!hit) continue;
        const char *add = en ? SKILLS[i].offer_en : SKILLS[i].offer_it;
        if (strstr(r->reply, en ? "I also" : "Inoltre")) return;   // already offered
        size_t need = strlen(add), cap = sizeof(r->reply) - 1;
        if (need + 8 >= cap) return;
        size_t room = cap - need;                                  // keep room for the offer; clip the long fact
        if (strlen(r->reply) > room) { size_t cut = room; while (cut > 16 && r->reply[cut] != ' ' && r->reply[cut] != '.') cut--; r->reply[cut] = 0; }
        memcpy(r->reply + strlen(r->reply), add, need + 1);
        return;
    }
}

// Detect a PALPABLE knowledge<->skill ambiguity: the query is essentially just a bare topic that ANIMA
// both knows AND can compute, with no disambiguating cue (no opener -> handled by caller via kind==0; no
// digits/compute -> those go straight to the solver). Returns the SKILLS index (and the clarify question)
// or -1. Narrow on purpose: only fires on the bare word, so "storia della fisica" stays knowledge.
static int a_skill_clarify(const char *q, bool en, char *topic_out, size_t tcap, const char **question)
{
    char nq[120]; a_norm_phrase(q, nq, sizeof nq);
    for (const char *p = q; *p; p++) if (isdigit((unsigned char)*p)) return -1;   // has data -> it's a compute, not ambiguous
    const char *body = nq; while (*body == ' ') body++;                            // strip ONE leading article
    static const char *const art[] = { "il ","lo ","la ","l ","un ","uno ","una ","the ","a ","an ", NULL };
    for (int i = 0; art[i]; i++) { size_t L = strlen(art[i]); if (strncmp(body, art[i], L) == 0) { body += L; break; } }
    char bare[64]; snprintf(bare, sizeof bare, "%s", body);
    size_t bl = strlen(bare); while (bl && bare[bl-1] == ' ') bare[--bl] = 0;      // trim
    for (size_t i = 0; i < sizeof(SKILLS)/sizeof(SKILLS[0]); i++) {
        if (!(en ? SKILLS[i].ask_en : SKILLS[i].ask_it)) continue;                 // skill opts out of clarify
        for (int k = 0; SKILLS[i].bare[k]; k++)
            if (!strcmp(bare, SKILLS[i].bare[k])) {
                snprintf(topic_out, tcap, "%s", SKILLS[i].bare[0]);
                *question = en ? SKILLS[i].ask_en : SKILLS[i].ask_it;
                return (int)i;
            }
    }
    return -1;
}


static int a_norm_ntok(const char *norm);                                   // fwd (defined below)
static bool a_has_phrase(const char *norm, const char *const *phrases);      // fwd (defined below)

// Is `q` a context-needing follow-up FRAGMENT — a short question with no subject of its own ("e cosa
// ha fatto?", "e quando è morto?", "perché?", "e lui?")? Used (MISS-gated) to retry with the last topic
// injected = lightweight coreference without NER.
static bool a_is_followup_q(const char *q)
{
    char nq[120]; a_norm_phrase(q, nq, sizeof nq);
    int nt = a_norm_ntok(nq);
    if (nt < 1 || nt > 7) return false;                    // a fragment, not a whole self-contained question
    // A follow-up has a question cue AND no subject of its own: EVERY token must be a function/cue word
    // (no content noun). So "e cosa ha fatto" -> yes; "cos'è quasar" (has the subject "quasar") -> no.
    static const char *const cue[] = { "cosa","cos","cose","quando","dove","perche","come","chi","quale",
        "quali","quanti","quanto","quanta","lui","lei","esso","essa","suo","sua","loro","ne",
        "what","when","where","why","who","which","how","he","she","it","his","her","they", NULL };
    if (!a_has_phrase(nq, cue)) return false;
    static const char *const fn[] = {  // function words allowed in a subject-less fragment
        "e","ed","ma","poi","allora","cosa","cos","cose","che","ha","hanno","ho","hai","fatto","fece","fa",
        "fanno","era","erano","stato","stata","quando","dove","perche","come","chi","quale","quali","quanti",
        "quanto","quanta","lui","lei","esso","essa","loro","suo","sua","il","lo","la","i","gli","le","di","del",
        "a","da","in","su","per","con","morto","morta","nato","nata","vissuto","serve","servono","significa",
        "significano","vuol","dire","mi","ne","si","cosi","oggi","ora","adesso",
        "what","whats","did","does","has","have","had","he","she","it","they","them","his","her","their","when",
        "where","why","who","which","how","is","are","was","were","do","the","a","an","of","to","about","mean",
        "means","born","die","died","made","make","for","then","and","so","now", NULL };
    char tok[A_MAX_TOKENS][A_TOK_LEN]; int n = a_tokenize(q, tok);
    for (int t = 0; t < n; t++) {
        bool isfn = false;
        for (int i = 0; fn[i] && !isfn; i++) if (!strcmp(fn[i], tok[t])) isfn = true;
        if (!isfn) return false;                           // a content word -> the query has its own subject
    }
    return true;
}

// Run the L0 -> L1 answer cascade on `q` (NO clarify band here — the band runs once, after the
// typo rescue, so a typo'd launch is corrected before a fuzzy L1 clarify can pre-empt it).
// Returns 1 if it produced an answer (fills *r and updates memory); 0 on a miss (*r = L0 NONE).
// ---- Conversational FOCUS shift (structured coreference for the deductive tier) ----------------
// The KGE reasoner DECLARES the (subject, relation) it anchored in r.subject / r.relation. We stash that
// as the conversational focus, so a bare follow-up can re-aim the SAME reasoner deterministically: swap
// the entity ("quando e nato einstein" -> "e newton?") keeping the relation, or swap the relation
// ("e dove si trova?") keeping the subject. NO text subtraction: we rebuild a FULL fact question from a
// relation template and re-run nucleo_anima_hdc_reason, so its lexical/role/coherence guards still apply
// and a wrong re-aim simply REFUSES (zero fabrication). Miss-gated: only after the normal cascade missed.
static const char *foc_template(const char *rel) {
    if (!rel || !rel[0]) return NULL;
    if (!strcmp(rel, "born"))       return "quando e nato %s";
    if (!strcmp(rel, "died"))       return "quando e morto %s";
    if (!strcmp(rel, "capital"))    return "capitale di %s";
    if (!strcmp(rel, "located_in")) return "dove si trova %s";
    if (!strcmp(rel, "author"))     return "chi ha scritto %s";
    return NULL;
}
static void foc_remember(const anima_result_t *r) {
    if (!r->subject[0] || !r->relation[0]) return;
    snprintf(s_session.foc_subject,  sizeof s_session.foc_subject,  "%s", r->subject);
    snprintf(s_session.foc_relation, sizeof s_session.foc_relation, "%s", r->relation);
    s_session.foc_turn = s_session.turn;
}

static int try_cascade(const char *q, bool en, anima_result_t *r)
{
    char topic[160];
    int kind = a_topic_strip(q, topic, sizeof topic);  // 0 none, 1 formal, 2 conversational

    // Helper outcome on a hit: offer the related skill, update memory, return 1.
    #define A_CASCADE_HIT(used) do { a_offer_skill(r, (used), en); \
        snprintf(r->state, sizeof(r->state), "idle"); mem_update(r); \
        snprintf(s_mem.last_topic, sizeof(s_mem.last_topic), "%s", (used)); \
        s_session.dirty = true; return 1; } while (0)

    // CONVERSATIONAL lead-in ("cosa sai di X", "do you know X") is BY DEFINITION a knowledge question —
    // run L1 BEFORE L0 so a chatty opener can't be hijacked by a skill (e.g. "do you know…"->whoami).
    // Try the full phrase first (a card may carry it), then the bare topic. Lead-ins never name a command.
    if (kind == 2) {
        if (nucleo_anima_l1_query(q, en, false, r))     A_CASCADE_HIT(topic);
        if (nucleo_anima_l1_query(topic, en, false, r)) A_CASCADE_HIT(topic);
    }
    *r = l0_query(q, en);
    if (r->tier != ANIMA_TIER_NONE) { mem_update(r); return 1; }
    // PALPABLE knowledge<->skill ambiguity: a bare topic ANIMA both knows and can compute, with no cue
    // (no opener -> kind==0; no digits/compute verb -> handled inside a_skill_clarify). Ask instead of
    // guessing — exactly once. A clear request (opener, or numbers/"calcola") never reaches here.
    if (kind == 0) {
        char ct[40]; const char *cq = NULL;
        if (a_skill_clarify(q, en, ct, sizeof ct, &cq) >= 0) {
            memset(r, 0, sizeof *r);
            r->tier = ANIMA_TIER_FACT; r->action = ANIMA_ACT_ANSWER; r->confidence = 80; r->awaiting = 1;
            snprintf(r->intent, sizeof r->intent, "clarify");
            snprintf(r->state, sizeof r->state, "clarify");
            snprintf(r->reply, sizeof r->reply, "%s", cq);
            snprintf(s_session.skill_clarify, sizeof s_session.skill_clarify, "%s", ct);
            s_session.dirty = true;
            return 1;
        }
    }
    // L1: FULL query first (preserves exact "cos'è X" / "what is X" ask matches), then the stripped
    // topic as a fallback (handles "cos'è LA fisica" / openers the full phrase wouldn't hit).
    if (nucleo_anima_l1_query(q, en, false, r)) {
        if (kind && r->action != ANIMA_ACT_ANSWER) memset(r, 0, sizeof *r);
        else A_CASCADE_HIT(kind ? topic : q);
    }
    if (kind && nucleo_anima_l1_query(topic, en, false, r)) {
        if (r->action != ANIMA_ACT_ANSWER) memset(r, 0, sizeof *r);
        else A_CASCADE_HIT(topic);
    }
    // CANONICAL retry: cards are indexed on question-phrasings ("cos'è X" / "what is X"), so a bare topic
    // stripped from a chatty opener ("raccontami di suono" -> "suono") may not match while the canonical
    // form does. Reconstruct and retry (still gated -> no fabrication). Skip if topic already opens with the
    // cue, OR if the query NAMES a Proper Noun: the lowercased reconstruction would lose its case and bypass
    // the named-entity coverage guard (re-admitting "il linguaggio Floonk" -> the generic card).
    bool names_proper = false;
    { bool first = true;
      for (const char *p = q; *p; ) {
        while (*p && !isalnum((unsigned char)*p) && (unsigned char)*p < 0x80) p++;
        if (!*p) break;
        char f = *p; int len = 0;
        while (*p && (isalnum((unsigned char)*p) || (unsigned char)*p >= 0x80)) { p++; len++; }
        if (!first && len >= 4 && f >= 'A' && f <= 'Z') { names_proper = true; break; }
        first = false;
      } }
    if (kind && !names_proper && topic[0] && strncmp(topic, en ? "what" : "cos", en ? 4 : 3) != 0) {
        char canon[180]; snprintf(canon, sizeof canon, en ? "what is %s" : "cos e %s", topic);
        if (nucleo_anima_l1_query(canon, en, false, r)) {
            if (r->action != ANIMA_ACT_ANSWER) memset(r, 0, sizeof *r);
            else A_CASCADE_HIT(topic);
        }
    }
    #undef A_CASCADE_HIT
    return 0;
}

// Online-only mode (ANIMA app "Online: Solo" setting): when set, the query skips the whole offline
// cascade and answers ONLY via the cloud teacher (Grok). Off by default; implies the network master
// switch is on (the app sets both).
static bool s_online_only;
static bool nucleo_anima_online_only_enabled(void) { return s_online_only; }

// Network policy, set by the apps (see ANIMA_NET_* in nucleo_anima.h).
static int s_net_mode = ANIMA_NET_HYBRID;
void nucleo_anima_set_net_mode(int mode)
{
    if (mode < ANIMA_NET_OFF || mode > ANIMA_NET_LLM) mode = ANIMA_NET_HYBRID;
    s_net_mode = mode;
    nucleo_anima_set_online(mode != ANIMA_NET_OFF);
    nucleo_anima_online_set_local_only(mode == ANIMA_NET_LOCAL);
    s_online_only = mode == ANIMA_NET_LLM;
}
int nucleo_anima_get_net_mode(void) { return s_net_mode; }

// This turn's model is off-limits: set by nucleo_anima_query_no_model (the caller's own model call
// already failed) and, inside a turn, once the LLM-mode call failed — so nothing dials it twice.
static bool s_no_model_turn;
// The turn runs a lower rung than its mode wanted (LLM mode without a usable / answering model).
static bool s_turn_degraded;

void nucleo_anima_route(anima_route_t *o)
{
    memset(o, 0, sizeof *o);
    o->mode    = s_net_mode;
    o->network = s_net_mode != ANIMA_NET_OFF && nucleo_anima_online_available();
    o->web     = o->network && s_net_mode != ANIMA_NET_LOCAL;
    o->model   = o->network && nucleo_anima_model_usable();
    if (!o->network)                         o->run = ANIMA_RUN_DEVICE;
    else if (s_net_mode == ANIMA_NET_LLM)    o->run = o->model ? ANIMA_RUN_AGENT : ANIMA_RUN_WEB;
    else if (s_net_mode == ANIMA_NET_LOCAL)  o->run = o->model ? ANIMA_RUN_LOCAL_LLM : ANIMA_RUN_DEVICE;
    else                                     o->run = o->model ? ANIMA_RUN_HYBRID : ANIMA_RUN_WEB;
    o->degraded = s_net_mode == ANIMA_NET_LLM && !o->model;
}

const char *nucleo_anima_route_label(const anima_route_t *r, bool en)
{
    switch (r->run) {
        case ANIMA_RUN_AGENT:     return en ? "Agent (language model)" : "Agente (modello linguistico)";
        case ANIMA_RUN_HYBRID:    return en ? "Device + web + model as last resort" : "Dispositivo + web + modello come ultima risorsa";
        case ANIMA_RUN_LOCAL_LLM: return en ? "Device + LAN model" : "Dispositivo + modello in LAN";
        case ANIMA_RUN_WEB:       return en ? "Device + web (no model)" : "Dispositivo + web (senza modello)";
        default:                  return en ? "Device only" : "Solo dispositivo";
    }
}

anima_result_t nucleo_anima_query_no_model(const char *input, const char *lang)
{
    s_no_model_turn = true;
    anima_result_t r = nucleo_anima_query(input, lang);
    s_no_model_turn = false;
    nucleo_anima_online_model_off(false);
    return r;
}

// ============================================================================
// DIALOGUE ACTS — the conversational glue that makes ANIMA feel like a coherent
// agent across turns, not a one-shot Q&A box. These are meta-commands that act on
// the CONVERSATION STATE (working memory), NOT on the knowledge base — so they need
// no encoder, no card, no network: pure deterministic patterns over s_session, free
// of the retrieval gate's ceiling. The MCU-honest analog of an agent's turn-taking.
// ============================================================================

// Normalize `raw` to " token token … " (lowercase ASCII, accents folded, single spaces, sentinel
// spaces at both ends) so a multi-word phrase matches with a simple strstr(" phrase ").
void a_norm_phrase(const char *raw, char *out, size_t cap)
{
    int o = 0; if (cap < 2) { if (cap) out[0] = 0; return; }
    out[o++] = ' '; bool sp = true;
    for (const unsigned char *p = (const unsigned char *)raw; *p && o < (int)cap - 2; p++) {
        unsigned char c = *p; char ch = 0;
        if (c == 0xC3 && p[1]) {
            unsigned char d = *++p;
            if      (d>=0xA0&&d<=0xA2) ch='a'; else if (d>=0xA8&&d<=0xAA) ch='e';
            else if (d>=0xAC&&d<=0xAE) ch='i'; else if (d>=0xB2&&d<=0xB4) ch='o';
            else if (d>=0xB9&&d<=0xBB) ch='u';
        } else if (isalnum(c)) ch = (char)tolower(c);
        if (ch) { out[o++] = ch; sp = false; }
        else if (!sp) { out[o++] = ' '; sp = true; }
    }
    if (!sp && o < (int)cap - 1) out[o++] = ' ';
    out[o] = 0;
}
static bool a_has_phrase(const char *norm, const char *const *phrases)
{
    char pat[48];
    for (int i = 0; phrases[i]; i++) { snprintf(pat, sizeof(pat), " %s ", phrases[i]); if (strstr(norm, pat)) return true; }
    return false;
}
static int a_norm_ntok(const char *norm) { int n = 0; for (const char *p = norm; *p; p++) if (*p == ' ') n++; return n > 0 ? n - 1 : 0; }

// Strip a knowledge lead-in to the bare topic. Returns: 0 = no lead-in (keep original); 1 = FORMAL
// opener ("cos'è X" / "what is X") — strip for L1 but keep L0 FIRST (so "what is the time/weather"
// stays a live command); 2 = CONVERSATIONAL opener ("cosa sai di X" / "do you know X") — never a
// command, so the caller runs L1 on the topic FIRST. Never strips semantic words ("come"/"perche").
// An optional leading article is dropped after the opener.
static int a_topic_strip(const char *q, char *out, size_t outsz)
{
    char nq[160];
    a_norm_phrase(q, nq, sizeof nq);                   // " cosa sai di fisica "
    static const char *const lead_conv[] = {           // conversational -> L1-first
        "cosa sai di","cosa sai su","cosa sai sulla","cosa sai sul","che cosa sai di","cosa sapresti dirmi su",
        // di/su + ARTICLE contractions: "cosa sai DELLA fotosintesi" must route like "cosa sai DI X", not
        // carry "della" into the embedding as noise (which abstained on shard topics). Mirrors parlami del/della.
        "cosa sai della","cosa sai del","cosa sai dello","cosa sai dei","cosa sai degli","cosa sai delle","cosa sai dell",
        "che cosa sai della","che cosa sai del","cosa sai sullo","cosa sai sui","cosa sai sugli","cosa sai sulle",
        // "cosa sai DIRMI su/sulla X": l'infisso 'dirmi' rompe gli open"cosa sai X" sopra -> li rispecchio.
        "cosa sai dirmi di","cosa sai dirmi su","cosa sai dirmi sul","cosa sai dirmi sulla","cosa sai dirmi sullo",
        "cosa sai dirmi della","cosa sai dirmi del","cosa sai dirmi dello","cosa sai dirmi dei","cosa sai dirmi delle",
        "che cosa sai dirmi su","che cosa sai dirmi sulla","che cosa sai dirmi della","cosa mi sai dire di","cosa mi sai dire su",
        "dimmi di","dimmi qualcosa di","dimmi qualcosa su","dimmi cosa e","dimmi cos e","dimmi che cos e",
        "dimmi tutto su","dimmi tutto di","mi dici cos e","mi puoi dire cos e","parlami di","parlami del","parlami della",
        "parlami un po di","raccontami di","raccontami qualcosa di","mi parli di","mi spieghi","mi spieghi meglio",
        "mi sai spiegare","mi sapresti spiegare","mi spiegheresti","puoi spiegarmi","potresti spiegarmi","sapresti spiegarmi",
        "spiegami meglio","fammi capire","aiutami a capire","vorrei capire","voglio capire","ho sempre voluto capire",
        "non ho mai capito cosa e","non ho mai capito cosa fosse","non ho mai capito","mi sai dire","sai dirmi","sai cosa e",
        "conosci","hai presente","hai mai sentito parlare di","hai mai sentito di","ne sai qualcosa su",
        "ho sentito parlare di","ho sentito di","ho letto di","ho letto che","ho visto","mi hanno parlato di","sai qualcosa di",
        "what do you know about","what can you tell me about","what can you tell me","do you know about",
        "do you know","tell me about","tell me more about","tell me everything about","tell me what",
        "can you explain","could you explain","can you tell me","help me understand","walk me through",
        "give me a rundown of","give me an overview of","give me the gist of","i never understood","i have never understood",
        "i ve never understood","i always wanted to understand","i want to understand",
        "have you heard of","have you heard about","are you familiar with","ever heard of",
        "i heard about","i heard of","i read about",
        // describe-recall: more conversational openers (workflow-found misses, gate-verified 0-fabrication)
        "cosa sarebbe","cosa sarebbero","raccontami cos e","raccontami che cos e","puoi descrivermi","potresti descrivermi",
        "sai descrivermi","descrivimi un po","spiegatemi","spiegatemelo",
        "break down","break it down","can you break down","give me a breakdown of","what s the deal with","fill me in on", NULL };
    static const char *const lead_formal[] = {         // formal definition -> strip but L0-first
        "cos e","cose","che cos e","che cosa e","cosa e","cosa sono","cosa sia","quali sono","cosa significa",
        "che significa","che vuol dire","significato di","definizione di","definisci","definiscimi","descrivimi","descrivi",
        "spiegami","spiega","explain","describe","define","meaning of","how does","how do","what does",
        "what is the","what is a","what is an","what is","what are","what s","whats","who is","who was","who are",
        "descrivermi",
        // IT mechanism questions -> EN parity (kind=1 = L0-first, so live commands still win, then L1 gets the topic)
        "come funziona","come funzionano","come funziona il","come funziona la","come funziona lo","a cosa serve",
        "a cosa servono","a che serve","come si usa","come si fa", NULL };
    int kind = 0; const char *rest = NULL;
    for (int i = 0; lead_conv[i]; i++) {
        char pat[40]; snprintf(pat, sizeof pat, " %s ", lead_conv[i]);
        if (strncmp(nq, pat, strlen(pat)) == 0) { rest = nq + strlen(pat); kind = 2; break; }
    }
    if (!rest) for (int i = 0; lead_formal[i]; i++) {
        char pat[40]; snprintf(pat, sizeof pat, " %s ", lead_formal[i]);
        if (strncmp(nq, pat, strlen(pat)) == 0) { rest = nq + strlen(pat); kind = 1; break; }
    }
    if (!rest) return 0;
    static const char *const art[] = { "il ","lo ","la ","i ","gli ","le ","un ","uno ","una ","l ",
        "del ","dello ","della ","dei ","degli ","delle ","di ","the ","a ","an ", NULL };
    for (int i = 0; art[i]; i++) { size_t L = strlen(art[i]); if (strncmp(rest, art[i], L) == 0) { rest += L; break; } }
    while (*rest == ' ') rest++;
    if (!*rest) return 0;
    snprintf(out, outsz, "%s", rest);
    // A SECONDARY cue after the topic ("...kubernetes cos e?" from "kubernetes, cos'è?") -> keep only the
    // topic before it. (a_norm_phrase already folded the comma/apostrophe to spaces.) Cut at the earliest.
    static const char *const tailcut[] = { " cos ", " cos e", " cosa ", " che cos", " significa", " spiegami", " spiega ", NULL };
    size_t best = (size_t)-1;
    for (int i = 0; tailcut[i]; i++) { char *c = strstr(out, tailcut[i]); if (c) { size_t pos = (size_t)(c - out); if (pos > 0 && pos < best) best = pos; } }
    if (best != (size_t)-1) out[best] = 0;
    size_t n = strlen(out);                // trim trailing sentinel space / punctuation
    while (n && (out[n-1] == ' ' || out[n-1] == '?' || out[n-1] == '!' || out[n-1] == '.')) out[--n] = 0;
    // Strip TRAILING conversational noise so only the bare topic is left for retrieval
    // ("explain X to me" -> "X"; "how does X work" -> "X"; "X di preciso" -> "X"). Looped: peel several.
    static const char *const tailnoise[] = { " to me", " for me", " to a beginner", " please", " per favore",
        " di preciso", " esattamente", " in simple terms", " in parole semplici", " for dummies",
        " like i m five", " simply", " meglio", " un po", " work", " do", " mean",
        " actually", " really", " precisely", " exactly", NULL };   // adverb-interrupted "what does X actually mean" -> X
    for (bool cut = true; cut; ) {
        cut = false; n = strlen(out);
        for (int i = 0; tailnoise[i]; i++) {
            size_t tl = strlen(tailnoise[i]);
            if (n >= tl && strcmp(out + n - tl, tailnoise[i]) == 0) { out[n - tl] = 0; cut = true; break; }
        }
        n = strlen(out);
        while (n && (out[n-1] == ' ' || out[n-1] == '?' || out[n-1] == '!' || out[n-1] == '.')) out[--n] = 0;
    }
    return out[0] ? kind : 0;
}

// Epistemic self-report: ANIMA explains HOW certain it is about its last answer, read off the tier
// and confidence it ALREADY computed. The honest metacognition layer — the assistant knows how it
// knows (exact compute vs curated card vs deduction vs online vs a low-confidence rescue).
static void a_self_report(bool en, anima_result_t *r)
{
    const char *m; const char *it = s_session.last.intent; anima_tier_t t = s_session.last.tier; int c = s_session.last.conf;
    bool exact = !strcmp(it,"calc")||!strcmp(it,"percent")||!strcmp(it,"convert")||!strcmp(it,"ohm")||!strcmp(it,"base");
    bool live  = !strcmp(it,"battery")||!strcmp(it,"time")||!strcmp(it,"storage")||!strcmp(it,"ram")||!strcmp(it,"network")||
                 !strcmp(it,"date")||!strcmp(it,"uptime")||!strcmp(it,"version")||!strcmp(it,"year")||!strcmp(it,"season");
    if (!s_session.last.reply[0] || t == ANIMA_TIER_NONE)
        m = en ? "Honestly, I didn't know that one — I wouldn't rely on it." : "A dire il vero non lo sapevo: non mi fiderei.";
    else if (exact)                  m = en ? "Yes — it's an exact calculation, I can't get it wrong." : "Si: e un calcolo esatto, non posso sbagliarlo.";
    else if (!strcmp(it,"combinator")) m = en ? "Yes — I worked it out by combining facts I know." : "Si: l'ho calcolata componendo fatti che conosco.";
    else if (!strcmp(it,"hdc"))       m = en ? "Logically yes — I deduced it from facts I hold." : "Logicamente si: l'ho dedotta dai fatti che conosco.";
    else if (t == ANIMA_TIER_REMOTE) m = en ? "Yes — I checked it against an online source." : "Si: l'ho verificata da una fonte online.";
    else if (live)                   m = en ? "Yes — I read that straight from the device." : "Si: e un dato che leggo direttamente dal dispositivo.";
    else if (t == ANIMA_TIER_FACT && c >= 85) m = en ? "Yes — it's something I know with good confidence." : "Si: e una cosa che so con buona certezza.";
    else if (t == ANIMA_TIER_FACT)   m = en ? "Fairly — it's the most likely answer, but not 100%." : "Abbastanza: e la risposta piu probabile, ma non al 100%.";
    else                             m = en ? "Yes, I'm confident." : "Si, ne sono sicuro.";
    snprintf(r->reply, sizeof(r->reply), "%s", m);
}

// The dialogue-act tier. Returns 1 (and fills *r) when the input is a conversational meta-command
// resolved from working memory; 0 to let the normal cascade handle it. Runs early, before retrieval.
static int a_dialogue_act(const char *q, bool en, anima_result_t *r)
{
    char nz[160]; a_norm_phrase(q, nz, sizeof(nz));
    int nt = a_norm_ntok(nz);
    if (nt == 0) return 0;
    memset(r, 0, sizeof(*r));
    r->tier = ANIMA_TIER_COMMAND; r->action = ANIMA_ACT_ANSWER; r->confidence = 90;
    snprintf(r->state, sizeof(r->state), "followup");

    // 1) CONFIDENCE CHALLENGE — "sei sicuro?", "davvero?" -> epistemic self-report.
    static const char *const sure[] = { "sei sicuro","sei sicura","sei certo","sei certa","ne sei sicuro",
        "sicuro","sicura","davvero","are you sure","you sure","really","for real", NULL };
    if (a_has_phrase(nz, sure) && (nt <= 4 || strstr(nz," sei sicuro ") || strstr(nz," sei certo "))) {
        snprintf(r->intent, sizeof(r->intent), "sure"); a_self_report(en, r); return 1;
    }

    // 2) CLARIFICATION — "spiegati meglio", "non ho capito" -> re-answer the last topic with its DETAIL
    //    (more verbose). Short queries only, so "non ho capito X" still gets routed to ask about X.
    static const char *const clar[] = { "spiegati meglio","spiega meglio","spiegamelo meglio","non ho capito",
        "non ho capito bene","non capisco","in che senso","puoi spiegare","puoi essere piu chiaro","cosa intendi",
        "explain better","i dont understand","dont understand","what do you mean","be clearer","explain again", NULL };
    if (nt <= 5 && a_has_phrase(nz, clar)) {
        if (s_mem.last_topic[0] && nucleo_anima_l1_query(s_mem.last_topic, en, true, r)) { snprintf(r->state, sizeof(r->state), "followup"); return 1; }
        snprintf(r->intent, sizeof(r->intent), "explain");
        snprintf(r->reply, sizeof(r->reply), en ? "Tell me which part and I'll try again." : "Dimmi quale parte e riprovo a spiegarla.");
        return 1;
    }

    // 3) RECAP — "ricapitola", "di cosa parlavamo" -> extractive summary of the working-memory ring.
    static const char *const recap[] = { "ricapitola","facciamo il punto","di cosa parlavamo","cosa stavamo dicendo",
        "riassumi","riepilogo","recap","what were we talking about","summarize","sum up", NULL };
    // recap summarizes the CONVERSATION, not an external document. "riassumi il documento che ho in mente"
    // / "summarize the document i have in mind" references a doc ANIMA cannot read -> don't recap the chat.
    static const char *const extdoc[] = { " documento "," document "," file "," libro "," book "," articolo ",
        " article "," testo che "," pagina "," pdf "," lettera ", " note "," nota "," notes "," mail "," email ",
        " messaggi "," messaggio "," foto ", NULL };   // "riassumi le mie note": the user's files, not this chat
    bool ext = false; for (int i = 0; extdoc[i]; i++) if (strstr(nz, extdoc[i])) ext = true;
    if (!ext && a_has_phrase(nz, recap)) {
        snprintf(r->intent, sizeof(r->intent), "recap");
        char buf[256]; int o = 0, shown = 0;
        o += snprintf(buf+o, sizeof(buf)-o, en ? "So far: " : "Finora: ");
        for (int k = 0; k < s_session.ring_len && shown < 3; k++) {
            int i = (s_session.ring_head - 1 - k + ANIMA_RING*2) % ANIMA_RING;
            if (!s_session.ring[i].input[0]) continue;
            o += snprintf(buf+o, sizeof(buf)-o, "%s\"%s\"", shown?", ":"", s_session.ring[i].input); shown++;
        }
        if (!shown && s_mem.last_topic[0]) snprintf(buf, sizeof(buf), en ? "We were on: \"%s\"." : "Eravamo su: \"%s\".", s_mem.last_topic);
        else if (!shown) snprintf(buf, sizeof(buf), en ? "We just started." : "Abbiamo appena iniziato.");
        else snprintf(buf+o, sizeof(buf)-o, ".");
        snprintf(r->reply, sizeof(r->reply), "%s", buf);
        return 1;
    }

    // 4) THANKS — social glue.
    static const char *const thanks[] = { "grazie","grazie mille","ti ringrazio","thanks","thank you","cheers", NULL };
    if (nt <= 3 && a_has_phrase(nz, thanks)) {
        snprintf(r->intent, sizeof(r->intent), "thanks");
        snprintf(r->reply, sizeof(r->reply), en ? "You're welcome!" : "Prego, quando vuoi!");
        return 1;
    }

    // 5) DENIAL — "no", "sbagliato", "non è giusto" -> acknowledge + invite a rephrase (no fact asserted).
    static const char *const deny[] = { "sbagliato","errato","non e giusto","non e corretto","non e quello",
        "non e vero","thats wrong","that is wrong","not right","incorrect","nope", NULL };
    if (a_has_phrase(nz, deny) || (nt <= 2 && strstr(nz, " no "))) {
        snprintf(r->intent, sizeof(r->intent), "deny");
        snprintf(r->reply, sizeof(r->reply), en ? "Sorry — try rephrasing and I'll have another go." : "Scusa: prova a riformulare e ci riprovo.");
        s_mem.last_topic[0] = 0;        // don't let a rejected topic linger for the next "tell me more"
        return 1;
    }
    return 0;
}

// An OPEN-ENDED "describe / explain" request benefits from a fuller, multi-span answer (MOSAICO/L2);
// a crisp factoid reads better terse. Built on the SAME curated, accent-folded machinery the cascade
// already uses — a_topic_strip()'s lead-in families ("parlami di / cos'è / tell me about / what is /
// explain …") plus word-boundary a_has_phrase() over a normalized query — NOT a raw substring list, so
// it can't misfire mid-word and isn't fragile to accents/encoding. MOSAICO still fires only on an L1
// knowledge answer (the intent guard), so this is a "should I enrich?" quality signal, not a safety gate.
#define L1_STITCH_GATE 80      // only enrich a genuinely confident L1 anchor (conf = bestcos*100)
static bool a_is_describe(const char *q)
{
    char topic[160];
    if (a_topic_strip(q, topic, sizeof topic) != 0) return true;   // cos'è / parlami di / what is / tell me about / explain …
    // Explanation/identity cues a_topic_strip doesn't strip, matched word-boundary (a_norm_phrase pads
    // with spaces and folds accents, so " perche " never matches inside "perchéx" and "chi e" never
    // matches inside "chiede"). Curated + locked by eval_describe.jsonl.
    char nq[160]; a_norm_phrase(q, nq, sizeof nq);
    static const char *const cues[] = {
        // definition / identity — also catches EMBEDDED and apostrophe-CONTRACTED forms that
        // a_topic_strip's prefix anchor misses ("what's"->"what s", "cos'è"->"cos e", mid-sentence).
        "cos e", "che cos", "cosa e", "cosa sono", "cosa sia", "quali sono", "quale e",
        "chi e", "chi era", "chi sono", "descrivi", "descrivimi", "spiegami", "spiega",
        "what is", "what s", "what are", "who is", "who was", "who are", "who s", "describe", "explain",
        // explanation / purpose / mechanism
        "come funziona", "come funzionano", "a cosa serve", "a cosa servono", "a che serve", "perche",
        "how does", "how do", "what does", "why is", "why does", "what for", NULL };
    return a_has_phrase(nq, cues);
}

// ============================================================================
// TURN SHAPE + PLANS (docs/ANIMA_MODES.md, "Forma del turno"). Before any tier answers, the shape of a
// request that asks the device to DO something decides who may own it:
//   compound   "chiudi la musica e apri le note"      -> every clause understood by L0: ONE plan, run in
//                                                         order; otherwise nothing runs half-way
//   deferred   "apri la musica tra 10 minuti"          -> never executed now
//   condition  "se domani piove ricordami ..."         -> never executed now
//   recurring  "ogni mattina alle 8 dimmi il meteo"    -> never executed now
//   how-to     "come si alza il volume?"               -> explained, offered (a "sì" does it)
//   negated    "non aprire la musica"                  -> acknowledged, nothing runs
// What L0 cannot do faithfully goes to the language model when one is usable (it answers with ACT
// lines, validated into the same plan), else an honest reply says what to say instead. Certainty
// first: a wrong or partial action is worse than none.

// Imperative action verbs (IT+EN). A request with none of these is not a command, whatever its words.
static bool a_is_action_verb(const char *w)
{
    static const char *const V[] = {
        "apri","chiudi","alza","abbassa","aumenta","diminuisci","riduci","imposta","metti","porta","spegni",
        "accendi","ferma","avvia","lancia","crea","scrivi","ricordami","segna","aggiungi","annota","mostra",
        "mostrami","dimmi","fammi","avvisami","silenzia","togli","riproduci","suona","fai",
        "open","close","turn","set","raise","lower","increase","decrease","mute","create","write","remind",
        "add","show","tell","start","stop","launch","put","play","riapri",
        "torna","vai","portami","riportami","go","return", NULL };   // "... e torna alla home"
    for (int i = 0; V[i]; i++) if (!strcmp(V[i], w)) return true;
    return false;
}
// The same verb with an object pronoun glued on ("abbassalo", "riaprila", "chiudili"): a command whose
// object is "the thing before" — fine on its own, unknowable inside a plan.
static bool a_is_clitic_verb(const char *w)
{
    static const char *const CL[] = { "melo","mela","glielo","gliela","lo","la","li","le","mi","ci","ne", NULL };
    const size_t wl = strlen(w);
    for (int i = 0; CL[i]; i++) {
        const size_t cl = strlen(CL[i]);
        if (wl <= cl + 2 || strcmp(w + wl - cl, CL[i])) continue;
        char stem[24];
        snprintf(stem, sizeof stem, "%.*s", (int)(wl - cl), w);
        if (a_is_action_verb(stem)) return true;
    }
    return false;
}
// Verbs whose object is free text (a note, a reminder): after one of these, " e " joins content words,
// so only a NEW action verb starts a new clause ("crea una nota ... e ricordami ...").
static bool a_is_content_verb(const char *w)
{
    static const char *const V[] = { "crea","scrivi","ricordami","segna","aggiungi","annota","fammi",
                                     "create","write","remind","add", NULL };
    for (int i = 0; V[i]; i++) if (!strcmp(V[i], w)) return true;
    return false;
}
static bool a_is_article(const char *w)
{
    static const char *const A[] = { "il","lo","la","i","gli","le","l","un","una","uno","del","della","dello",
                                     "al","alla","the","a","an","my","mio","mia", NULL };
    for (int i = 0; A[i]; i++) if (!strcmp(A[i], w)) return true;
    return false;
}
static bool a_is_setting_noun(const char *w)
{
    static const char *const N[] = { "volume","audio","suono","luminosita","schermo","luce","brightness","sound", NULL };
    for (int i = 0; N[i]; i++) if (a_match(N[i], w)) return true;
    return false;
}
static bool a_has_action_verb(char tok[A_MAX_TOKENS][A_TOK_LEN], int ntok)
{
    for (int t = 0; t < ntok; t++) if (a_is_action_verb(tok[t])) return true;
    return false;
}

// Split a request into its action clauses. An elliptical clause inherits the verb of the one before it
// ("alza la luminosità e il volume" -> "alza la luminosità" + "alza il volume"). Text after "con scritto",
// "con il testo", "che dice" or ':' is content and is never split. Returns the clause count (1 = whole).
#define TURN_CLAUSES 4
#define TURN_CLAUSE_LEN 160
static int turn_split(const char *q, char out[TURN_CLAUSES][TURN_CLAUSE_LEN])
{
    static const char *const SEP[] = { " e poi ", " e dopo ", ", e ", ", poi ", " poi ", " quindi ", "; ", ", ",
                                       " and then ", " then ", " and ", " e ", NULL };
    char low[256];
    int L = 0;
    for (; q[L] && L < (int)sizeof low - 1; L++) low[L] = (char)tolower((unsigned char)q[L]);
    low[L] = 0;
    int n = 0, start = 0;
    char verb[24] = "";            // the current clause's action verb (an elliptical next clause reuses it)
    char prefix[24] = "";          // verb prepended to the current clause when it is elliptical
    bool content = false;          // the current clause carries free text: only a NEW verb splits it
    bool frozen = false;           // literal content started ("con scritto ...", ':'): nothing splits after
    for (int i = 0; i < L && n < TURN_CLAUSES - 1; i++) {
        if (!frozen && (i == start || low[i - 1] == ' ' || low[i - 1] == '\'')) {
            char w[24]; int k = 0;
            for (int j = i; low[j] && isalpha((unsigned char)low[j]) && k < (int)sizeof w - 1; j++) w[k++] = low[j];
            w[k] = 0;
            if (k && !verb[0] && a_is_action_verb(w)) { snprintf(verb, sizeof verb, "%s", w); content = a_is_content_verb(w); }
            if (!strcmp(w, "scritto") || !strcmp(w, "testo") || !strcmp(w, "dice") || !strcmp(w, "saying")) frozen = true;
        }
        if (low[i] == ':') frozen = true;
        if (frozen || !verb[0]) continue;               // the left part is not (yet) a command: no split
        for (int s = 0; SEP[s]; s++) {
            const size_t sl = strlen(SEP[s]);
            if (strncmp(low + i, SEP[s], sl)) continue;
            const int r0 = i + (int)sl;
            char w1[24] = "", w2[24] = "";              // the first two words on the right
            int k = 0, j = r0;
            while (low[j] && !isalpha((unsigned char)low[j])) j++;
            for (; low[j] && isalpha((unsigned char)low[j]) && k < 23; j++) w1[k++] = low[j];
            w1[k] = 0; k = 0;
            while (low[j] && !isalpha((unsigned char)low[j])) j++;
            for (; low[j] && isalpha((unsigned char)low[j]) && k < 23; j++) w2[k++] = low[j];
            w2[k] = 0;
            bool split = false, ellipsis = false;
            if (a_is_action_verb(w1) || a_is_clitic_verb(w1)) split = true;
            else if (!content) {                        // "e il volume" / "e poi le note": same verb, new object
                const char *noun = a_is_article(w1) ? w2 : w1;
                char nt[A_MAX_TOKENS][A_TOK_LEN];     // a_resolve_app takes a full token table
                snprintf(nt[0], A_TOK_LEN, "%s", noun);
                if (noun[0] && (a_is_setting_noun(noun) || a_resolve_app(nt, 1))) split = ellipsis = true;
            }
            if (!split) continue;
            int e = i;                                  // left clause = q[start, i)
            while (e > start && (q[e - 1] == ' ' || q[e - 1] == ',')) e--;
            snprintf(out[n++], TURN_CLAUSE_LEN, "%s%.*s", prefix, e - start, q + start);
            int rs = r0;
            while (rs < L && q[rs] == ' ') rs++;
            start = rs;
            if (ellipsis) snprintf(prefix, sizeof prefix, "%s ", verb);
            else { prefix[0] = 0; verb[0] = 0; content = false; }
            i = rs - 1;
            break;
        }
    }
    snprintf(out[n], TURN_CLAUSE_LEN, "%s%s", prefix, q + start);
    return n + 1;
}

// ---- plans: one validated list of device actions, built from L0 clauses or from a model's ACT lines --
typedef struct {
    char launch[64];                       // the app the plan opens last ("" = none)
    int  nt;
    anima_step_t t[ANIMA_PLAN_MAX];        // TOOL steps, in the order asked
    int  content;                          // add_event / create_file steps: they share s_tool_content
    int  n;                                // actions accepted
    int  why;                              // why the last push was refused (PLAN_* below)
    int  conj_at;                          // offset of the " e " the last push inserted (0 = none)
    char reply[sizeof(((anima_result_t *)0)->reply)];
} anima_plan_acc_t;
enum { PLAN_OK = 0, PLAN_TWO_APPS, PLAN_TWO_CONTENT, PLAN_CONTRA, PLAN_TOO_MANY, PLAN_NOT_DEVICE };

static bool plan_is_tool(const char *intent)
{
    static const char *const OK[] = { "set_volume","set_brightness","close_app","go_home","media_pause","media_resume",
                                      "add_event","create_file", NULL };
    for (int i = 0; OK[i]; i++) if (!strcmp(OK[i], intent)) return true;
    return false;
}

static bool plan_has_home(const anima_plan_acc_t *p)
{
    for (int i = 0; i < p->nt; i++) if (!strcmp(p->t[i].intent, "go_home")) return true;
    return false;
}

// Add one action (a LAUNCH or a TOOL result). False = it cannot be part of a plan.
static bool plan_push(anima_plan_acc_t *p, const anima_result_t *a, bool en)
{
    p->why = PLAN_NOT_DEVICE;
    if (a->awaiting) return false;
    if (a->action == ANIMA_ACT_LAUNCH && !strcmp(a->intent, "open_app") && !strcmp(a->arg, p->launch)) {
        p->why = PLAN_OK;                                  // "apri la galleria e mostrami le foto": the same app
        return true;
    }
    p->why = PLAN_TOO_MANY;
    if (p->n >= ANIMA_PLAN_MAX) return false;
    if (a->action == ANIMA_ACT_LAUNCH) {
        p->why = PLAN_NOT_DEVICE;
        if (strcmp(a->intent, "open_app") || !a->arg[0]) return false;
        p->why = PLAN_TWO_APPS;
        if (p->launch[0]) return false;                    // the shell runs one app at a time
        p->why = PLAN_CONTRA;
        for (int i = 0; i < p->nt; i++)
            if (!strcmp(p->t[i].intent, "close_app") && !strcmp(p->t[i].arg, a->arg)) return false;   // open X + close X
        if (plan_has_home(p)) return false;                // open X + go home: one of the two loses
        snprintf(p->launch, sizeof p->launch, "%s", a->arg);
    } else if (a->action == ANIMA_ACT_TOOL && plan_is_tool(a->intent)) {
        const bool content = !strcmp(a->intent, "add_event") || !strcmp(a->intent, "create_file");
        p->why = PLAN_TWO_CONTENT;
        if (content && p->content) return false;
        p->why = PLAN_CONTRA;
        if (!strcmp(a->intent, "close_app") && !strcmp(a->arg, p->launch)) return false;
        if (!strcmp(a->intent, "go_home") && (p->launch[0] || plan_has_home(p))) return false;
        p->content += content;
        snprintf(p->t[p->nt].intent, sizeof p->t[p->nt].intent, "%s", a->intent);
        snprintf(p->t[p->nt].arg, sizeof p->t[p->nt].arg, "%s", a->arg);
        p->nt++;
    } else {
        return false;
    }
    // "Abbasso il volume." + "Apro music." -> "Abbasso il volume e apro music."
    char part[sizeof p->reply];
    snprintf(part, sizeof part, "%s", a->reply);
    size_t pl = strlen(part);
    while (pl && (part[pl - 1] == '.' || part[pl - 1] == ' ')) part[--pl] = 0;
    if (p->n && part[0] >= 'A' && part[0] <= 'Z') part[0] = (char)(part[0] - 'A' + 'a');
    if (p->n) {                     // "A e B" -> "A, B e C": the conjunction WE inserted last becomes ", "
        const char *conj = en ? " and " : " e ";
        if (p->conj_at > 0) {
            char tail[sizeof p->reply];
            snprintf(tail, sizeof tail, "%s", p->reply + p->conj_at + strlen(conj));
            snprintf(p->reply + p->conj_at, sizeof p->reply - (size_t)p->conj_at, ", %s", tail);
        }
        const size_t o2 = strlen(p->reply);
        p->conj_at = (int)o2;
        snprintf(p->reply + o2, sizeof p->reply - o2, "%s%s", conj, part);
    } else {
        snprintf(p->reply, sizeof p->reply, "%s", part);
    }
    p->n++;
    p->why = PLAN_OK;
    return true;
}

// The plan as a result: LAUNCH (its app) or ANSWER, with every TOOL in steps[] for nv_anima_os_run.
static void plan_finish(const anima_plan_acc_t *p, anima_tier_t tier, int conf, anima_result_t *r)
{
    memset(r, 0, sizeof *r);
    r->tier = tier;
    r->confidence = conf;
    r->action = p->launch[0] ? ANIMA_ACT_LAUNCH : ANIMA_ACT_ANSWER;
    snprintf(r->intent, sizeof r->intent, "%s", p->launch[0] ? "open_app" : "plan");
    snprintf(r->arg, sizeof r->arg, "%s", p->launch);
    snprintf(r->reply, sizeof r->reply, "%s.", p->reply);
    snprintf(r->state, sizeof r->state, "tool");
    r->nsteps = p->nt;
    memcpy(r->steps, p->t, sizeof r->steps);
    char tr[sizeof r->trace]; int o = snprintf(tr, sizeof tr, "%s", tier == ANIMA_TIER_REMOTE ? "LLM > piano" : "piano");
    for (int i = 0; i < p->nt && o < (int)sizeof tr; i++) o += snprintf(tr + o, sizeof tr - o, " > %s %s", p->t[i].intent, p->t[i].arg);
    if (p->launch[0] && o < (int)sizeof tr) snprintf(tr + o, sizeof tr - o, " > open_app %s", p->launch);
    snprintf(r->trace, sizeof r->trace, "%s", tr);
}

// ---- shapes ------------------------------------------------------------------------------------------
static bool a_tok_in(const char *w, const char *const *list)
{
    for (int i = 0; list[i]; i++) if (!strcmp(list[i], w)) return true;
    return false;
}
// "tra 10 minuti", "fra un'ora", "dopo 5 min", "alle 18", "stasera", "domani", "in 10 minutes", "tonight".
static bool a_has_when(char tok[A_MAX_TOKENS][A_TOK_LEN], int ntok)
{
    static const char *const LEAD[] = { "tra","fra","dopo","entro","in","after","within", NULL };
    static const char *const UNIT[] = { "minuto","minuti","min","ora","ore","secondo","secondi","sec","mezzora",
                                        "minute","minutes","hour","hours","second","seconds", NULL };
    static const char *const DAY[]  = { "stasera","stanotte","stamattina","domani","dopodomani","tonight","tomorrow", NULL };
    for (int t = 0; t < ntok; t++) {
        if (a_tok_in(tok[t], DAY)) return true;
        if ((!strcmp(tok[t], "alle") || !strcmp(tok[t], "at")) && t + 1 < ntok && isdigit((unsigned char)tok[t + 1][0])) return true;
        if (a_tok_in(tok[t], LEAD))
            for (int u = t + 1; u < ntok && u <= t + 3; u++) if (a_tok_in(tok[u], UNIT)) return true;
    }
    return false;
}

typedef enum { TS_PLAIN = 0, TS_COMPOUND, TS_DEFERRED, TS_CONDITION, TS_RECURRING, TS_HOWTO, TS_NEGATED } turn_shape_t;

static turn_shape_t turn_shape(const char *q, char tok[A_MAX_TOKENS][A_TOK_LEN], int ntok, int nclauses)
{
    if (ntok == 0) return TS_PLAIN;
    // "non aprire la musica": Italian negative imperative = non + infinitive
    static const char *const NEG_INF[] = { "aprire","chiudere","alzare","abbassare","impostare","mettere","spegnere",
        "accendere","creare","scrivere","cancellare","cambiare","toccare","avviare","fermare","ricordarmi", NULL };
    // ...but "don't forget the meeting" is a double negation: a reminder, not a refusal.
    const int en_neg = !strcmp(tok[0], "dont") ? 1 : (ntok > 1 && !strcmp(tok[0], "don") && !strcmp(tok[1], "t")) ? 2 : 0;
    const bool forget = en_neg && en_neg < ntok && (!strcmp(tok[en_neg], "forget") || !strcmp(tok[en_neg], "let"));
    if ((!strcmp(tok[0], "non") && ntok > 1 && a_tok_in(tok[1], NEG_INF)) || (en_neg && ntok > en_neg && !forget))
        return TS_NEGATED;
    // how-to: "come si alza il volume", "come posso aprire le note", "how do I open ..." (L0 confirms
    // there is a device action in it before the gate treats it as one)
    if ((!strcmp(tok[0], "come") && ntok > 1 &&
         (!strcmp(tok[1], "si") || !strcmp(tok[1], "posso") || !strcmp(tok[1], "faccio") ||
          !strcmp(tok[1], "devo") || !strcmp(tok[1], "fare") || !strcmp(tok[1], "puoi"))) ||
        (!strcmp(tok[0], "how") && ntok > 1 && (!strcmp(tok[1], "do") || !strcmp(tok[1], "can") || !strcmp(tok[1], "to"))) ||
        (ntok > 2 && !strcmp(tok[0], "in") && !strcmp(tok[1], "che") && !strcmp(tok[2], "modo")))
        return TS_HOWTO;
    if (!a_has_action_verb(tok, ntok)) return TS_PLAIN;    // not a command: the tiers below decide
    static const char *const POLITE[] = { "puoi","riesci","vuoi","potresti","possibile","can","could","possible", NULL };
    const bool polite_if = ntok > 1 && (!strcmp(tok[0], "se") || !strcmp(tok[0], "if")) && a_tok_in(tok[1], POLITE);
    if (polite_if) return nclauses > 1 ? TS_COMPOUND : TS_PLAIN;   // "se puoi apri la musica": a plain request
    if (!strcmp(tok[0], "se") || !strcmp(tok[0], "if") || !strcmp(tok[0], "quando") || !strcmp(tok[0], "when") ||
        strstr(q, ", se ") || strstr(q, ", if "))
        return TS_CONDITION;
    for (int t = 0; t + 1 < ntok; t++) {
        static const char *const PER[] = { "giorno","giorni","mattina","mattine","sera","sere","notte","settimana",
            "ora","mese","lunedi","martedi","mercoledi","giovedi","venerdi","sabato","domenica",
            "day","morning","evening","night","week","hour","monday","friday", NULL };
        if ((!strcmp(tok[t], "ogni") || !strcmp(tok[t], "every")) && a_tok_in(tok[t + 1], PER)) return TS_RECURRING;
        if (!strcmp(tok[t], "tutti") && t + 2 < ntok && !strcmp(tok[t + 1], "i") && !strcmp(tok[t + 2], "giorni")) return TS_RECURRING;
    }
    if (nclauses > 1) return TS_COMPOUND;
    return TS_PLAIN;   // a single clause: DEFERRED is decided once L0 has said what the action is
}

// L0 on one clause with no side effects: session state is restored, the model is off-limits, and the
// tools that write or fetch on their own (teach, profile, translate) are skipped.
EXT_RAM_BSS_ATTR static unsigned char s_sess_bak[sizeof s_session];
static anima_result_t l0_dry(const char *clause, bool en)
{
    memcpy(s_sess_bak, &s_session, sizeof s_session);
    char trace[sizeof s_trace];
    memcpy(trace, s_trace, sizeof trace);                 // a tool's "piano: ..." steps must not leak out
    s_dry_run = true;
    anima_result_t r = l0_query(clause, en);
    s_dry_run = false;
    memcpy(&s_session, s_sess_bak, sizeof s_session);
    memcpy(s_trace, trace, sizeof trace);
    return r;
}

// A reminder or a note may name a time ("ricordami domani", "nota per domani"): that is content, not
// a request to act later.
static bool a_timed_ok(const anima_result_t *r)
{
    return !strcmp(r->intent, "add_event") || !strcmp(r->intent, "create_file");
}

// "come si alza il volume" -> "alza il volume"; "come posso aprire le note" -> "apri le note".
static void a_howto_phrase(const char *q, char *out, size_t cap)
{
    static const char *const LEAD[] = { "come si fa ad ", "come si fa a ", "come faccio ad ", "come faccio a ", "come posso ",
        "come si ", "come devo ", "come fare ad ", "come fare a ", "come puoi ", "in che modo posso ", "in che modo si ",
        "how do i ", "how can i ", "how to ", NULL };
    char low[160]; int L = 0;
    for (; q[L] && L < (int)sizeof low - 1; L++) low[L] = (char)tolower((unsigned char)q[L]);
    low[L] = 0;
    const char *rest = q;
    for (int i = 0; LEAD[i]; i++) if (!strncmp(low, LEAD[i], strlen(LEAD[i]))) { rest = q + strlen(LEAD[i]); break; }
    static const char *const INF[][2] = { {"aprire","apri"},{"apre","apri"},{"alzare","alza"},{"abbassare","abbassa"},
        {"chiudere","chiudi"},{"chiude","chiudi"},{"impostare","imposta"},{"mettere","metti"},{"mette","metti"},
        {"spegnere","spegni"},{"spegne","spegni"},{"accendere","accendi"},{"accende","accendi"},{"aumentare","aumenta"},
        {"diminuire","diminuisci"},{"creare","crea"},{"crea","crea"} };
    char w[24]; int k = 0;
    while (rest[k] && rest[k] != ' ' && k < 23) { w[k] = (char)tolower((unsigned char)rest[k]); k++; }
    w[k] = 0;
    const char *verb = NULL;
    for (size_t i = 0; i < sizeof INF / sizeof INF[0]; i++) if (!strcmp(w, INF[i][0])) verb = INF[i][1];
    snprintf(out, cap, "%s%s", verb ? verb : "", verb ? rest + k : rest);
    size_t ol = strlen(out);
    while (ol && (out[ol - 1] == '?' || out[ol - 1] == ' ' || out[ol - 1] == '.')) out[--ol] = 0;
}

// The action an L0 result would run now, if any (LAUNCH, or a device TOOL).
static bool a_is_device_action(const anima_result_t *r)
{
    return r->tier == ANIMA_TIER_COMMAND && !r->awaiting &&
           (r->action == ANIMA_ACT_LAUNCH || (r->action == ANIMA_ACT_TOOL && plan_is_tool(r->intent)));
}

static void turn_answer(anima_result_t *r, const char *intent, const char *reply)
{
    memset(r, 0, sizeof *r);
    r->tier = ANIMA_TIER_COMMAND; r->action = ANIMA_ACT_ANSWER; r->confidence = 80;
    snprintf(r->intent, sizeof r->intent, "%s", intent);
    snprintf(r->reply, sizeof r->reply, "%s", reply);
    snprintf(r->state, sizeof r->state, "idle");
}

// The gate. Returns 1 when the turn is decided (r filled), 0 to let the cascade run as before.
// model_ok: a language model may be asked this turn (HYBRID / LOCAL, usable, not already failed).
static int turn_gate(const char *q, bool en, bool model_ok, const anima_turn_t *ctx, int nctx, anima_result_t *r)
{
    // A pending slot / clarify belongs to the FSM: never reshape its answer.
    if (s_session.pending_tool[0] || s_session.clarify_opt[0][0]) return 0;
    char tok[A_MAX_TOKENS][A_TOK_LEN];
    const int ntok = a_tokenize(q, tok);
    char cl[TURN_CLAUSES][TURN_CLAUSE_LEN];
    const int nc = turn_split(q, cl);
    turn_shape_t sh = turn_shape(q, tok, ntok, nc);

    if (sh == TS_NEGATED) {
        turn_answer(r, "deny", en ? "OK, I won't do anything." : "Va bene, non faccio niente.");
        return 1;
    }
    anima_result_t one;
    if (sh == TS_PLAIN) {
        // A single clause: is it an action asked for later? ("apri la musica tra 10 minuti")
        if (!a_has_action_verb(tok, ntok) || !a_has_when(tok, ntok)) return 0;
        one = l0_dry(q, en);
        if (!a_is_device_action(&one) || a_timed_ok(&one)) return 0;   // a reminder / a note may name a time
        sh = TS_DEFERRED;
    }
    if (sh == TS_HOWTO) {
        one = l0_dry(q, en);
        if (!a_is_device_action(&one)) return 0;              // "come si fa la pasta": a real question
        // Explain how to say it, and offer it: a "sì" runs it through the ACT confirmation path.
        char act[96] = "";
        if (one.action == ANIMA_ACT_LAUNCH) snprintf(act, sizeof act, "ACT open_app %s", one.arg);
        else if (!strcmp(one.intent, "set_volume") || !strcmp(one.intent, "set_brightness"))
            snprintf(act, sizeof act, "ACT %s %s", one.intent, one.arg);
        char said[128];
        a_howto_phrase(q, said, sizeof said);
        char rep[300];
        if (act[0]) {
            snprintf(s_pending_act, sizeof s_pending_act, "%s", act);
            s_pending_act_ms = act_now_ms();
            pending_mark();                        // only this asker may answer it
            const bool app = one.action == ANIMA_ACT_LAUNCH;
            snprintf(rep, sizeof rep,
                     app ? (en ? "Just ask me (\"%s\") or tap its icon on the Home screen. Shall I open it now? (yes/no)"
                               : "Basta chiedermelo (\"%s\") o toccare la sua icona nella Home. La apro adesso? (sì/no)")
                         : (en ? "Just ask me (\"%s\") or use the quick settings (swipe down from the top). Shall I do it now? (yes/no)"
                               : "Basta chiedermelo (\"%s\") o usare il pannello rapido (scorri dall'alto). Lo faccio adesso? (sì/no)"),
                     said);
            turn_answer(r, "howto", rep);
            r->awaiting = 1;
            snprintf(r->state, sizeof r->state, "slot");
        } else {
            snprintf(rep, sizeof rep, en ? "Just ask me: \"%s\"." : "Basta chiedermelo: \"%s\".", said);
            turn_answer(r, "howto", rep);
        }
        return 1;
    }
    if (sh == TS_COMPOUND) {
        // Every clause must be a device action L0 is sure of; then ONE plan runs them in order.
        anima_plan_acc_t p; memset(&p, 0, sizeof p);
        int bad = -1, why = 0;            // why: 1 not understood, 2 not combinable (p.why says how), 3 timed, 4 pronoun
        const bool prev_off = nucleo_anima_online_model_is_off();
        nucleo_anima_online_model_off(true);
        for (int i = 0; i < nc && bad < 0; i++) {
            char ct[A_MAX_TOKENS][A_TOK_LEN];
            const int cn = a_tokenize(cl[i], ct);
            bool clitic = false;
            for (int t = 0; t < cn && !clitic; t++) clitic = !a_is_action_verb(ct[t]) && a_is_clitic_verb(ct[t]);
            if (clitic) { bad = i; why = 4; break; }       // "...e poi abbassalo": the object is not said
            anima_result_t c = l0_dry(cl[i], en);
            if (c.tier == ANIMA_TIER_NONE || c.confidence < 60)             { bad = i; why = 1; }
            else if (!a_is_device_action(&c))                               { bad = i; why = 2; p.why = PLAN_NOT_DEVICE; }
            else if (a_has_when(ct, cn) && !a_timed_ok(&c))                 { bad = i; why = 3; }
            else if (!plan_push(&p, &c, en))                                { bad = i; why = 2; }
        }
        nucleo_anima_online_model_off(prev_off);
        if (bad < 0) {
            plan_finish(&p, ANIMA_TIER_COMMAND, 85, r);
            return 1;
        }
        content_reset();                                       // nothing half-composed survives
        if (model_ok && nucleo_anima_model_usable() && nucleo_anima_online_chat_ctx(q, ctx, nctx, en, r)) return 1;
        char rep[400];
        if (why == 1)
            snprintf(rep, sizeof rep,
                     en ? "I didn't understand \"%s\", so I did nothing, to avoid doing it halfway. Ask me one thing at a time."
                        : "Non ho capito \"%s\", quindi non ho fatto niente, per non farlo a metà. Chiedimi una cosa alla volta.", cl[bad]);
        else if (why == 3)
            snprintf(rep, sizeof rep,
                     en ? "\"%s\" is for later, and I can't schedule actions yet: I did nothing. Ask me one thing at a time, or set a reminder."
                        : "\"%s\" è per dopo, e non so ancora programmare azioni: non ho fatto niente. Chiedimi una cosa alla volta, o mettiamo un promemoria.", cl[bad]);
        else if (why == 4)
            snprintf(rep, sizeof rep,
                     en ? "I'm not sure what \"%s\" refers to, so I did nothing. Say it in full, one thing at a time."
                        : "Non so a cosa si riferisca \"%s\", quindi non ho fatto niente. Dimmelo per esteso, una cosa alla volta.", cl[bad]);
        else {
            const char *m =
                p.why == PLAN_TWO_APPS    ? (en ? "I can open only one app at a time, so I did nothing: which one should I open?"
                                                : "Posso aprire una sola app alla volta, quindi non ho fatto niente: quale apro?")
              : p.why == PLAN_TWO_CONTENT ? (en ? "I can make one note or one reminder at a time, so I did nothing. Ask me for them one by one."
                                                : "Posso creare una nota o un promemoria per volta, quindi non ho fatto niente. Chiedimeli uno alla volta.")
              : p.why == PLAN_CONTRA      ? (en ? "Those requests contradict each other (open and close the same app), so I did nothing."
                                                : "Le richieste si contraddicono (aprire e chiudere la stessa app), quindi non ho fatto niente.")
              : p.why == PLAN_TOO_MANY    ? (en ? "That is more than 3 actions at once, so I did nothing. Ask me in steps."
                                                : "Sono più di 3 azioni insieme, quindi non ho fatto niente. Chiedimelo a passi.")
              :                             (en ? "I can combine only device actions (apps, volume, brightness, notes, reminders), so I did nothing. Ask me one thing at a time."
                                                : "Posso unire solo azioni sul dispositivo (app, volume, luminosità, note, promemoria), quindi non ho fatto niente. Chiedimi una cosa alla volta.");
            snprintf(rep, sizeof rep, "%s", m);
        }
        turn_answer(r, "plan_partial", rep);
        return 1;
    }
    // deferred / condition / recurring: never now. A model may handle it; else say what works today.
    if (model_ok && nucleo_anima_model_usable() && nucleo_anima_online_chat_ctx(q, ctx, nctx, en, r)) return 1;
    const char *rep =
        sh == TS_DEFERRED
            ? (en ? "I can't schedule an action for later yet, so I haven't done it. I can set a reminder: \"remind me in 10 minutes to ...\"."
                  : "Non so ancora programmare un'azione per dopo, quindi non l'ho fatta. Posso metterti un promemoria: \"ricordami tra 10 minuti di ...\".")
        : sh == TS_CONDITION
            ? (en ? "I can't act on a condition (\"if ...\") yet, so I haven't done anything. Ask me directly, or set a reminder."
                  : "Non so ancora agire a una condizione (\"se ...\"), quindi non ho fatto niente. Chiedimelo direttamente, o mettiamo un promemoria.")
            : (en ? "I can't repeat an action over time yet, so I haven't set anything. I can set a single reminder."
                  : "Non so ancora ripetere un'azione nel tempo, quindi non ho impostato niente. Posso metterti un promemoria singolo.");
    turn_answer(r, sh == TS_DEFERRED ? "deferred" : sh == TS_CONDITION ? "condition" : "recurring", rep);
    return 1;
}

// A model reply with 2+ device ACT lines ("ACT close_app music\nACT open_app notes"): one plan, with the
// same limits as L0's, and the strictest permission of its steps (one "procedo?" covers them all).
// 0 = not a multi-action reply (the single-line path handles it).
static int act_plan_from_llm(const char *text, bool en, anima_result_t *r)
{
    static const char *const DEV[] = { "open_app","close_app","set_volume","set_brightness","add_event","create_file", NULL };
    const char *ln[ANIMA_PLAN_MAX + 1]; int ll[ANIMA_PLAN_MAX + 1];
    int n = 0;
    for (const char *p = text; *p; ) {
        while (*p == ' ' || *p == '\n' || *p == '\r' || *p == '`') p++;
        if (!*p) break;
        const char *e = strchr(p, '\n');
        const int len = e ? (int)(e - p) : (int)strlen(p);
        if (strncmp(p, "ACT ", 4)) { if (n) break; return 0; }   // prose after the ACT block ends it
        char tool[24] = "";
        sscanf(p + 4, "%23s", tool);
        if (!a_tok_in(tool, DEV)) return 0;                        // shell / files / remember: their own path
        if (n <= ANIMA_PLAN_MAX) { ln[n] = p; ll[n] = len; }
        n++;
        p = e ? e + 1 : p + len;
    }
    if (n < 2) return 0;
    memset(r, 0, sizeof *r);
    r->tier = ANIMA_TIER_REMOTE; r->action = ANIMA_ACT_ANSWER; r->confidence = 70;
    if (n > ANIMA_PLAN_MAX) {
        snprintf(r->intent, sizeof r->intent, "plan_invalid");
        snprintf(r->reply, sizeof r->reply, en ? "That is more than %d actions at once: I did nothing. Ask me in steps."
                                               : "Sono più di %d azioni insieme: non ho fatto niente. Chiedimelo a passi.", ANIMA_PLAN_MAX);
        return 1;
    }
    anima_plan_acc_t p; memset(&p, 0, sizeof p);
    int perm = 0;
    bool ok = true;
    const bool confirmed = s_act_confirmed;
    s_act_confirmed = true;                                        // parse each step without asking...
    char block[sizeof s_pending_act]; int bo = 0;
    for (int i = 0; i < n && ok; i++) {
        char one[AG_CONTENT_MAX + 64];
        snprintf(one, sizeof one, "%.*s", ll[i], ln[i]);
        anima_result_t a;
        ok = nucleo_anima_act_from_llm(one, en, &a) && plan_push(&p, &a, en);
        char tool[24] = ""; sscanf(one + 4, "%23s", tool);
        const int pm = confirmed ? 0 : nucleo_anima_permission(tool);   // ...then ask once for the strictest
        if (pm > perm) perm = pm;
        if (bo < (int)sizeof block) bo += snprintf(block + bo, sizeof block - bo, "%s%s", bo ? "\n" : "", one);
    }
    s_act_confirmed = confirmed;
    if (!ok) {
        content_reset();
        snprintf(r->intent, sizeof r->intent, "plan_invalid");
        snprintf(r->reply, sizeof r->reply, "%s",
                 en ? "The model proposed actions I can't do together (one app, and one note or reminder, at a time): I did nothing."
                    : "Il modello ha proposto azioni che non posso fare insieme (un'app, e una nota o un promemoria, per volta): non ho fatto niente.");
        return 1;
    }
    if (perm == 2) {
        content_reset();
        snprintf(r->intent, sizeof r->intent, "denied");
        snprintf(r->reply, sizeof r->reply, "%s", en ? "I'm not allowed to do part of that (permissions.json): I did nothing."
                                                     : "Non ho il permesso per una parte di queste azioni (permissions.json): non ho fatto niente.");
        return 1;
    }
    if (perm == 1) {
        content_reset();
        if (bo >= (int)sizeof block) {
            snprintf(r->intent, sizeof r->intent, "plan_invalid");
            snprintf(r->reply, sizeof r->reply, "%s", en ? "That request is too long to confirm: ask me in steps." : "La richiesta è troppo lunga da confermare: chiedimela a passi.");
            return 1;
        }
        snprintf(s_pending_act, sizeof s_pending_act, "%s", block);
        s_pending_act_ms = act_now_ms();
        pending_mark();                            // only this asker may answer it
        r->awaiting = 1;
        snprintf(r->intent, sizeof r->intent, "confirm");
        snprintf(r->state, sizeof r->state, "slot");
        snprintf(r->reply, sizeof r->reply, en ? "%s - shall I go ahead? (yes/no)" : "%s: procedo? (sì/no)", p.reply);
        snprintf(r->trace, sizeof r->trace, "LLM > piano > ask");
        return 1;
    }
    plan_finish(&p, ANIMA_TIER_REMOTE, 75, r);
    return 1;
}

// Cascade (docs/anima.md §2): L0 keyword tier first (cheap, exact); on a miss fall through
// to the L1 semantic tier (distilled encoder + RAG index, if the SD packs are present).
// Understands IT+EN regardless of `lang`; replies in `lang` ("en" -> English, else Italian).
// DIAG breadcrumb: the ANIMA tier active when the device last reset. Survives a warm reboot (RTC
// memory, not zeroed at boot); read + cleared at boot (main.c) and surfaced at WARN in /api/logs, so a
// crash is locatable over WiFi without the serial console. 0=clean, 0xB0=query entry, 0xC0=L1,
// 0xC1=L1 index load (SD), 0xD0=online/learned recall.
RTC_NOINIT_ATTR uint32_t g_anima_stage;
RTC_NOINIT_ATTR uint8_t  g_anima_phase;   // DIAG: cascade phase (tier funcs never touch it -> never stale)

// Cumulative query telemetry (see nucleo_anima.h). Bumped once in the done: epilogue below — the single
// point every cascade path converges on — so it costs a few u32 stores per query and nothing at rest.
static anima_diag_t s_diag;
void nucleo_anima_diag(anima_diag_t *out)
{
    if (!out) return;
    *out = s_diag;                                            // read without the gate (cheap telemetry):
    out->last_intent[sizeof out->last_intent - 1] = 0;        // a concurrent write must not leave it unterminated
}
static void diag_count(const anima_result_t *r)
{
    s_diag.queries++;
    switch (r->tier) {
        case ANIMA_TIER_COMMAND: s_diag.t_command++; break;
        case ANIMA_TIER_FACT:    s_diag.t_fact++;    break;
        case ANIMA_TIER_STITCH:  s_diag.t_stitch++;  break;
        case ANIMA_TIER_REMOTE:  s_diag.t_remote++;  break;
        default:                 s_diag.t_none++;    break;   // NONE = honest miss / abstain
    }
    s_diag.last_conf = r->confidence;
    snprintf(s_diag.last_intent, sizeof(s_diag.last_intent), "%s", r->intent);
}

anima_result_t nucleo_anima_query(const char *input, const char *lang)
{
    g_anima_stage = 0xB0;                 // DIAG: entered the ANIMA query
    g_anima_phase = 0x01;                 // DIAG: cascade entry
    bool en = lang && (lang[0] == 'e' || lang[0] == 'E');
    // RAM policy: stand the offline L1/HDC brain down for this turn ONLY in ONLINE-ONLY mode, where the
    // cloud LLM is the sole brain and L1's index would be dead weight competing for the TLS handshake's
    // heap. In HYBRID, L1 SERVES (it's half of "L1 + intelligent wiki search") so we must NOT stand it
    // down just because a key exists. AUTO honors this; a user FORCE_ON/OFF from the apps overrides it.
    nucleo_anima_l1_set_online_brain(nucleo_anima_online_available() && nucleo_anima_teacher_configured()
                                     && nucleo_anima_online_only_enabled());
    if (atomic_exchange(&s_reset_pending, false)) session_reset_locked();   // deferred reset (gate held)
    const bool no_model_entry = s_no_model_turn;   // restored at done: (the LLM branch may set it)
    s_session.turn++;
    s_turn_degraded = no_model_entry;              // entered after the caller's model call failed
    nucleo_anima_online_model_off(no_model_entry);
    if (!s_no_model_turn) nucleo_anima_online_turn_begin();   // no_model: keep the caller's fail note
    trace_reset();        // fresh thought-log for this turn
    content_reset();      // no composed payload until a tool produces one
    nucleo_anima_set_long_reply(NULL);   // drop any previous turn's long (code) overflow reply

    // Action memory: "ripeti" / "di nuovo" replays the last actionable turn from the ring.
    const char *q = input;
    bool replayed = false;
    // A COPY: the epilogue's ring_push(q, ...) may overwrite the very ring slot `prev` points into.
    char replay[sizeof s_session.ring[0].input];
    if (a_is_repeat(input)) {
        const char *prev = ring_last_input();
        if (prev) { snprintf(replay, sizeof replay, "%s", prev); q = replay; replayed = true; }   // else honest miss
    }
    // A known paraphrase ("lascia stare", "più forte") enters every tier as its canonical ("no", "alza il
    // volume"), the yes/no of a pending action included. The ring keeps the canonical too.
    const char *const para = replayed ? NULL : a_paraphrase(input, en);
    if (para) { snprintf(replay, sizeof replay, "%s", para); q = replay; }
    anima_result_t r;
    bool hdc_tried = false;   // HDC deductive tier already attempted on this q (it is deterministic)

    // Snapshot the conversation transcript ONCE for every online-teacher call this turn (the ring is
    // only appended at the epilogue, so this stays valid throughout). Oldest->newest; nctx may be 0.
    compact_auto(en);   // near the window's end: fold the older turns into the summary first
    anima_turn_t ctx[ANIMA_CHAT]; int nctx = chat_context(ctx, ANIMA_CHAT);

    // A model's action waiting for a yes/no (permissions.json "ask").
    // The answer to "Intendi «…»? (sì/no)": a yes runs the canonical (and the sentence is learned).
    bool learned = false;
    {
        char canon[sizeof s_sugg_canon];
        const int yn = suggest_resolve(input, en, canon, sizeof canon);
        if (yn > 0) { snprintf(replay, sizeof replay, "%s", canon); q = replay; learned = true; }
        else if (yn < 0) {
            memset(&r, 0, sizeof r);
            r.tier = ANIMA_TIER_COMMAND; r.action = ANIMA_ACT_ANSWER; r.confidence = 90;
            snprintf(r.intent, sizeof r.intent, "deny");
            snprintf(r.reply, sizeof r.reply, "%s", en ? "OK. Say it in other words and I'll learn it."
                                                       : "Va bene. Dimmelo con altre parole, così imparo.");
            goto done;
        }
    }
    if (nucleo_anima_pending_answer(para ? q : input, ctx, nctx, NULL, en, &r)) goto done;

    // A picture came with this message (Telegram photo, gallery...): only a model can look at it.
    if (nucleo_anima_image_pending()) {
        if (nucleo_anima_online_available() && nucleo_anima_online_chat_ctx(q, ctx, nctx, en, &r)) goto done;
        nucleo_anima_attach_image(NULL);
        memset(&r, 0, sizeof r);
        r.tier = ANIMA_TIER_REMOTE; r.action = ANIMA_ACT_ANSWER;
        snprintf(r.reply, sizeof r.reply, "%s", en ? "I need a model to look at pictures: set one in Settings > AI."
                                                   : "Per guardare le immagini mi serve un modello: impostalo in Impostazioni > IA.");
        goto done;
    }

    // Resolve a pending L1 knowledge clarify ("intendi 1) … o 2) …?"): an ordinal picks one of
    // the two offered cards. Not a pick -> drop the clarify and handle the input normally.
    if (s_session.clarify_l1) {
        int pick = a_pick_ordinal(input);
        if (pick >= 0 && nucleo_anima_l1_read(s_session.clarify_ans[pick], en, &r)) {
            s_session.clarify_l1 = false; r.from_memory = 1;
            snprintf(r.state, sizeof(r.state), "clarify");
            snprintf(s_mem.last_topic, sizeof(s_mem.last_topic), "%s", input);   // enable "tell me more"
            s_session.dirty = true;
            goto done;
        }
        s_session.clarify_l1 = false;
    }

    // Resolve a pending KNOWLEDGE<->SKILL clarify ("vuoi sapere cos'è X o calcolarlo?"). Map the reply
    // to one side. Anything else — the actual problem with numbers, or a new subject — drops the clarify
    // and is handled by the normal cascade (so "massa 2 accelerazione 3" goes straight to the solver).
    if (s_session.skill_clarify[0]) {
        char topic[40]; snprintf(topic, sizeof topic, "%s", s_session.skill_clarify);
        s_session.skill_clarify[0] = 0;
        char nz[120]; a_norm_phrase(input, nz, sizeof nz);
        bool digit = false; for (const char *p = input; *p; p++) if (isdigit((unsigned char)*p)) digit = true;
        static const char *const w_know[] = { "cos","cosa","cose","sapere","sappi","definizione","significato",
            "spiega","spiegami","spiegamela","teoria","uno","primo","prima","what","first","know","explain","definition", NULL };
        static const char *const w_do[]   = { "calcola","calcolare","calcolo","calcolarla","risolvi","risolvere",
            "problema","esercizio","due","secondo","seconda","calculate","solve","compute","problem","second", NULL };
            // NB: not "skill" — it collides with "che skill hai" (a capabilities question, not a clarify pick).
        bool wantKnow = a_has_phrase(nz, w_know), wantDo = a_has_phrase(nz, w_do);
        if (wantKnow && !wantDo) {
            if (nucleo_anima_l1_query(topic, en, false, &r)) {
                a_offer_skill(&r, topic, en);
                snprintf(r.state, sizeof(r.state), "clarify");
                snprintf(s_mem.last_topic, sizeof(s_mem.last_topic), "%s", topic);
                s_session.dirty = true; goto done;
            }
        } else if (wantDo && !digit) {                      // wants the skill but gave no data yet -> prompt for it
            memset(&r, 0, sizeof(r));
            r.tier = ANIMA_TIER_COMMAND; r.action = ANIMA_ACT_ANSWER; r.confidence = 80; r.awaiting = 1;
            snprintf(r.intent, sizeof(r.intent), "clarify");
            snprintf(r.state, sizeof(r.state), "clarify");
            snprintf(r.reply, sizeof(r.reply), en ? "Great — give me the data and I'll compute it."
                                                  : "Perfetto, dammi i dati e lo calcolo.");
            goto done;
        }
        // not a clear pick -> fall through: the normal cascade handles the new input (data, calc, or topic).
    }

    // Drill-down: "dimmi di più" / "tell me more" -> re-query the LAST knowledge topic for its
    // detail text. Makes the assistant feel conversational without any generation (two-level
    // retrieval: the same card carries a short reply and a longer detail).
    if (a_is_more_request(q) && s_mem.last_topic[0]) {
        if (nucleo_anima_l1_query(s_mem.last_topic, en, true, &r)) {
            snprintf(r.state, sizeof(r.state), "followup");
        } else {
            memset(&r, 0, sizeof(r));
            r.tier = ANIMA_TIER_COMMAND; r.action = ANIMA_ACT_ANSWER; r.confidence = 60;
            snprintf(r.state, sizeof(r.state), "followup");
            snprintf(r.intent, sizeof(r.intent), "more");
            snprintf(r.reply, sizeof(r.reply), en ? "That's all I have on that." : "Su questo non ho altri dettagli.");
        }
        goto done;
    }

    // Dialogue acts: conversational meta-commands ("sei sicuro?", "spiegati meglio", "ricapitola",
    // "grazie", "no") resolved from working memory — the agent-like turn-taking glue. Skipped while a
    // tool slot or app clarify is pending, so a "no" there resolves the FSM, not the dialogue layer.
    if (!s_session.pending_tool[0] && !s_session.clarify_opt[0][0] && a_dialogue_act(q, en, &r)) goto done;

    // LLM (agent) mode: the language model owns the turn — a direct chat with the multi-turn transcript
    // (no JSON classification, no Wikipedia truth gate, no learning). When it is not usable or does not
    // answer, the turn falls through to the device + web rungs below (docs/ANIMA_MODES.md).
    if (s_online_only) {
        // A model that is not there (none configured, offline, on cooldown after a failure, or already
        // failed for this turn) is not waited on: the device answers at once.
        bool avail = !s_no_model_turn && nucleo_anima_model_usable();
        // CODE even in online-only: take the dedicated code path (code prompt, bigger token budget,
        // verbatim long_reply) — the prose teacher's clip_reply sentence-truncates on '.' and so
        // mangles a snippet mid-statement (cuts "pygame.display." in half).
        if (avail && a_is_code_request(q) && nucleo_anima_online_code(q, en, &r)) {
            mem_update(&r);
            snprintf(s_mem.last_topic, sizeof(s_mem.last_topic), "%s", q);
            s_session.dirty = true;
            goto done;
        }
        // WITH the multi-turn transcript so a pure-online follow-up ("divo 3?", "e lui?") resolves
        // against the running conversation — the old one-shot passthrough dropped all context, which
        // is why online mode "forgot" the prior turn. Still pure (no truth gate, no learning).
        if (avail && nucleo_anima_online_chat_ctx(q, ctx, nctx, en, &r)) {
            mem_update(&r);
            snprintf(s_mem.last_topic, sizeof(s_mem.last_topic), "%s", q);
            s_session.dirty = true;
            goto done;
        }
        // FALLBACK LADDER (docs/ANIMA_MODES.md): no usable model, or it gave no answer -> this turn runs
        // the HYBRID rungs without the model: L0 commands and tools, the solver, L1 knowledge, and the web
        // sources (Wikipedia / Wikidata / weather / news) when there is internet. A command still runs —
        // the user asked for it, and nothing was executed on the model's side. The L1 index the model
        // turn stood down is re-armed (it lazy-loads from SD; the cloud attempt is over). done: labels
        // the answer so it is never mistaken for the model's.
        nucleo_anima_l1_set_online_brain(false);
        s_turn_degraded = true;
        s_no_model_turn = true;     // the rungs below must not dial it again this turn (reset at done:)
        nucleo_anima_online_model_off(true);
    }

    // SAFETY GATE: only a genuine question reaches the online/knowledge tiers below. A bare date /
    // number / cell-ref / code / wordless string is data, not a question -> it must never be answered
    // with a fabricated "fact" nor learned. (Deterministic L0/L1/math already ran; this fences off the
    // online entity/Wikidata/teacher tiers, which are the only ones that can hallucinate.)
    const bool askable = a_is_askable(q);

    // POLICY: the cloud LLM (chat completions: chat_ctx / code / teacher) is allowed ONLY in ONLINE-ONLY
    // mode. HYBRID answers knowledge with L1 + INTELLIGENT WIKI SEARCH (Wikidata facts + Wikipedia bios,
    // all `!online_llm` paths below) — deterministic, grounded, far lighter on this PSRAM-less heap than a
    // chat completion carrying the whole transcript. (Online-only itself is handled+returned far above; so
    // online_llm is effectively false through this hybrid section, which is exactly the intent.)
    // (LLM mode never gets here with a working model: it answered above, or the turn is degraded.)
    const bool online_llm = !s_no_model_turn && nucleo_anima_online_available() && nucleo_anima_teacher_configured()
                            && nucleo_anima_online_only_enabled();
    // LOCAL and HYBRID: the language model is the LAST resort, after every grounded tier missed. Whether
    // it is usable is checked only there (it reads the teacher config), not on every turn.
    const bool llm_fallback = !online_llm && !s_no_model_turn &&
                              (s_net_mode == ANIMA_NET_LOCAL || s_net_mode == ANIMA_NET_HYBRID) &&
                              nucleo_anima_online_available();

    // Classify once up-front: the F_* feature flags drive the live/weather routing below AND the
    // later spellfix gate. (Pure function of q; q is stable from here on.)
    anima_plan_t plan; anima_cortex_plan(q, en, &plan);

    // TURN SHAPE: a compound / timed / conditional / how-to / negated command is decided here, before
    // any tier can grab one of its words (docs/ANIMA_MODES.md). The model is offered only what L0 cannot
    // do faithfully, and only where the mode allows it as the last resort.
    if (turn_gate(q, en, llm_fallback, ctx, nctx, &r)) { mem_update(&r); s_session.dirty = true; goto done; }

    // LIVE-data priority (weather/news): these MUST beat the L0 FAQ/date intents, which would
    // otherwise grab the "che/fa/domani" words in a weather phrasing ("che tempo fara domani a
    // brescia" was being answered as capabilities). A forecast REQUEST is F_WEATHER/F_NEWS but NOT a
    // definition (F_DEFWORD: "cos'è il clima" still answers from L1). Free the L1 index first — the TLS
    // handshake needs a large contiguous heap block on this PSRAM-less chip.
    // ...but NOT a calculation: "20 gradi in fahrenheit" / "tempo di caduta" carry weather words yet are
    // math. A digit or a math operator means compute, not forecast -> let the math/L1 tiers handle it.
    bool has_digit = false; for (const char *d = q; *d; d++) if (*d >= '0' && *d <= '9') { has_digit = true; break; }
    // ...and NOT a create-file command: a weather word inside the note CONTENT ("crea una nota con
    // scritto domani piove") must not let the weather tier swallow the create_file action.
    bool is_create_cmd = (plan.feat & F_CREATEVB) && (plan.feat & F_FILENOUN);
    // ...and NOT geometry: "quanti GRADI ha un triangolo" carries the weather word "gradi" but is a
    // math/geometry question (degrees, not temperature) -> let it fall through to L1/math.
    bool is_geo = strstr(q, "triangol") || strstr(q, "angol") || strstr(q, "quadrat") || strstr(q, "cerchio") ||
                  strstr(q, "poligon") || strstr(q, "esagon") || strstr(q, "pentagon") || strstr(q, "rettangol");
    // ...and NOT an image-generation command: "draw an image of a SNOWY mountain" / "genera una foto di
    // PIOGGIA" carry a weather word but are a request to PAINT a picture, not a forecast -> let the
    // image_gen decline tool (in l0_query) own it, exactly as is_create_cmd protects create_file.
    bool is_image_gen = false;
    { char gtok[A_MAX_TOKENS][A_TOK_LEN]; int gnt = a_tokenize(q, gtok); is_image_gen = a_is_image_gen(gtok, gnt); }
    // An explicit TRANSLATE request ("traduci sole in inglese", "come si dice pioggia") carries a weather
    // word as its OBJECT, not its subject — the offline dictionary must own it, never the forecast. Veto.
    bool is_translate = nucleo_anima_translate_is_request(q);
    bool wx_req = (plan.feat & (F_WEATHER | F_NEWS)) && !(plan.feat & (F_DEFWORD | F_MATHOP)) && !has_digit && !is_create_cmd && !is_geo && !is_image_gen && !is_translate;
    if (askable && (wx_req || nucleo_anima_online_is_live(q, en))) {
        if (nucleo_anima_online_available()) nucleo_anima_l1_unload();
        if (nucleo_anima_online_live(q, en, &r)) { mem_update(&r); s_session.dirty = true; goto done; }
        // A weather/news REQUEST with no live data (offline / unreachable) -> honest miss. NEVER fall
        // through to L1, which would answer "che tempo fa a Roma" with the Rome-history card or "c'è il
        // sole a Bari?" with the Sun-is-a-star card. The forecast just needs internet.
        memset(&r, 0, sizeof r);
        r.tier = ANIMA_TIER_NONE; r.action = ANIMA_ACT_NONE; r.confidence = 0;
        snprintf(r.intent, sizeof r.intent, (plan.feat & F_NEWS) ? "news" : "weather");
        goto done;
    }

    // CODE generation -> the online model directly. "scrivimi / dammi un esempio di codice python"
    // wants a real, professional snippet — which only the LLM produces, never a Wikipedia bio or an L1
    // card. Routed here (before the entity/L1 tiers) with a code prompt + a larger reply budget so the
    // fenced ```block isn't truncated. Online+key only; offline/no-key falls through (we never fabricate
    // code). Frees L1 first (the TLS handshake needs the contiguous heap on this PSRAM-less chip).
    if (online_llm && a_is_code_request(q)) {   // LLM-only: code is generated, never wiki-searched
        nucleo_anima_l1_unload();
        if (nucleo_anima_online_code(q, en, &r)) { mem_update(&r); s_session.dirty = true; goto done; }
    }

    // SPECIFIC entity questions ("cosa ha fatto X", "per cosa è famoso X", "what did X do") that the
    // frozen bio can't answer: let Grok answer them, grounded by its knowledge. Online + key only —
    // chat_ctx returns 0 without a key, so offline/no-key falls through to the entity bio (best
    // effort). Free L1 first (TLS handshake needs the contiguous heap).
    if (online_llm && askable && nucleo_anima_online_is_about(q, en)) {   // LLM-only: hybrid answers "cosa ha fatto X" from the Wikipedia bio below
        nucleo_anima_l1_unload();
        // WITH conversation context: "e cosa ha fatto?" (is_about, subject-less) resolves against the
        // previous turn; "cosa ha fatto einstein" (named) works too (context is harmless).
        if (nucleo_anima_online_chat_ctx(q, ctx, nctx, en, &r)) {
            if (a_is_followup_q(q)) snprintf(r.state, sizeof(r.state), "followup");
            mem_update(&r); s_session.dirty = true; goto done;
        }
    }

    // Structured-FACT priority (hybrid): "quando è morto X" / "capitale di X" must get the PRECISE
    // Wikidata fact, not the generic entity BIO that L1 returns for any question about X (otherwise
    // every Stalin question yields the same bio). Online-only and gated to actual fact-questions, so a
    // non-fact query never pays a net round-trip; offline -> 0 here and the L1 bio (whose prose holds
    // the fact) answers. Free L1 first — the TLS claim fetch needs the contiguous heap.
    if (askable && nucleo_anima_online_available() && nucleo_anima_online_is_fact(q, en)) {
        nucleo_anima_l1_unload();
        if (nucleo_anima_online_fact(q, en, &r)) {
            mem_update(&r);
            snprintf(s_mem.last_topic, sizeof(s_mem.last_topic), "%s", q);
            s_session.dirty = true;
            goto done;
        }
    }

    // CONVERSATIONAL FOCUS SHIFT (structured coreference): a bare CONTINUATION of the current thread.
    // SUBJECT-shift — a leading connector + a new entity, no question word ("e newton?", "e tokyo?") —
    // keeps the relation and swaps the entity; RELATION-shift — a subject-less question fragment — keeps
    // the entity. We re-aim the focus from a relation TEMPLATE (no text subtraction) and re-run the KGE
    // reasoner. This runs BEFORE the generic cascade on purpose, so the conversational relation wins over
    // an unrelated card ("e tokyo?" after "dove si trova lione" must answer location, not Tokyo's capital).
    // The reasoner's own lexical/role/coherence guards reject a wrong re-aim, so on a refuse we simply fall
    // through to the normal cascade (which reloads L1 on demand). Only fires with a fresh focus -> a cold
    // query (no prior fact in the thread) can never be hijacked, so single-shot routing cannot regress.
    if ((s_session.foc_subject[0] || s_session.foc_relation[0]) &&
        (s_session.turn - s_session.foc_turn) <= 8) {
        char ftok[A_MAX_TOKENS][A_TOK_LEN]; int fnt = a_tokenize(q, ftok);
        static const char *const conn[] = { "e","ed","poi","allora","anche","invece","ma","quindi","pure",
                                             "and","then","also","plus", NULL };
        bool lead_conn = false;
        if (fnt >= 1) for (int i = 0; conn[i]; i++) if (!strcmp(conn[i], ftok[0])) { lead_conn = true; break; }
        bool qword = a_has_qword(ftok, fnt);
        char shifted[176]; shifted[0] = 0;
        if (lead_conn && !qword && s_session.foc_relation[0] && fnt >= 2 && fnt <= 4) {
            // SUBJECT-shift: skip leading connectors, take the rest as the new entity, reuse the relation.
            int s0 = 0;
            while (s0 < fnt) { bool c = false; for (int i = 0; conn[i]; i++) if (!strcmp(conn[i], ftok[s0])) c = true; if (!c) break; s0++; }
            char ent[64]; int o = 0; ent[0] = 0;
            for (int t = s0; t < fnt && o + 1 < (int)sizeof ent; t++) o += snprintf(ent + o, sizeof ent - o, "%s%s", o ? " " : "", ftok[t]);
            const char *tmpl = foc_template(s_session.foc_relation);
            if (tmpl && (fnt - s0) >= 1 && strlen(ent) >= 2) snprintf(shifted, sizeof shifted, tmpl, ent);
        } else if (qword && s_session.foc_subject[0] && a_is_followup_q(q)) {
            // RELATION-shift: the subject-less question fragment carries the new relation; reuse the subject.
            snprintf(shifted, sizeof shifted, "%s %s", q, s_session.foc_subject);
        }
        if (shifted[0]) {
            nucleo_anima_l1_unload();                       // the reasoner builds its own KG of HVs; it needs the heap
            if (nucleo_anima_hdc_reason(shifted, en ? "en" : "it", &r)) {
                foc_remember(&r);                           // chain: the re-aimed turn becomes the new focus
                snprintf(r.state, sizeof r.state, "followup");
                mem_update(&r);
                snprintf(s_mem.last_topic, sizeof s_mem.last_topic, "%s", shifted);
                s_session.dirty = true;
                goto done;
            }
        }
    }
    
    // (Hook L0 dynamic-skill .lua su SD rimosso: interprete Lua ~90 KB flash, scaffold inerte
    //  — nessuno script spedito, anima_say scriveva solo sul log. Riattivabile se si integra davvero.)

    // STRUCTURED deduction precedes fuzzy L1 for fact-questions ("quando e nato Dante", "capitale del
    // Kenya", "in che continente e X"): nucleo_anima_hdc_reason fires ONLY on a recognized fact pattern
    // whose entity resolves in the learned graph (edge-grounded forward/inverse, honest-coherent
    // transitive) — otherwise it returns false and L0/L1 run unchanged. This stops a conversational card
    // (e.g. self.age matching "quando e nato <person>") from shadowing a precise stored fact.
    // ...UNLESS the input is a tool COMMAND: "crea una nota con la capitale della Francia" must CREATE
    // the note (composing the deduced fact INTO it), not be hijacked into a bare "Parigi" answer. So skip
    // the standalone reasoner for a create-file command; try_cascade below runs the compose-then-act tool,
    // whose ag_compose consults this very reasoner for the sub-clause -> the deduced fact lands in the file.
    {
        char ctok[A_MAX_TOKENS][A_TOK_LEN]; int cnt = a_tokenize(q, ctok);
        if (!a_is_create_file(ctok, cnt)) {
            // TYPED FACET first: a precise "che lavoro faceva X / X è uomo o donna" beats a fuzzy L1 bio
            // and the KGE (categorical facets don't live in the holographic graph). Abstains on a miss.
            if (nucleo_anima_facet(q, en, &r)) goto done;
            // NSPCG generative tier: a proof-carrying GENERATION for where/why/bridge questions ("dove si
            // trova X", "in che continente e X", "perche X e in Y", "come e collegato X a Y") — structured +
            // grounded, so it precedes the fuzzy L1, and it produces a NEW sentence the corpus never stored
            // with a self-verified proof (refuses if no grounded chain). It needs the contiguous heap for its
            // KG build, so reclaim L1's index FIRST — but ONLY when the query is actually NSPCG-shaped, so a
            // normal fact-question keeps its index for try_cascade below (a pcg refuse reloads it from SD).
            if (nucleo_anima_pcg_detect(q, en ? "en" : "it")) {
                nucleo_anima_l1_unload();
                if (nucleo_anima_pcg_generate(q, en ? "en" : "it", &r)) goto done;
            }
            hdc_tried = true;   // deterministic on the same q: the miss path below must not re-run it
            if (nucleo_anima_hdc_reason(q, en ? "en" : "it", &r)) goto done;
        }
    }

    g_anima_phase = 0x03;                  // DIAG: try_cascade (L0/L1)
    if (try_cascade(q, en, &r)) {
        // Self-improving knowledge: an L1 fact hit that was LEARNED without Grok gets re-vetted now that
        // a key is configured (confirm -> permanent, veto -> dropped). No-op for baked cards / no key /
        // offline. s_mem.last_topic is the bare topic L1 matched on.
        if (r.tier == ANIMA_TIER_FACT && nucleo_anima_online_available())
            nucleo_anima_online_upgrade(s_mem.last_topic, en);
        // MOSAICO (L2 span-stitch): a confident L1 answer to a DESCRIBE/EXPLAIN question is enriched with
        // more grounded spans from the SAME index (its detail + a coherent runner-up) — verbatim span-copy
        // (cannot hallucinate). Crisp factoids and rescue-band guesses are left untouched. The index is
        // still loaded here (the unload only happens on the miss path below), so s_band is this query's.
        if (r.tier == ANIMA_TIER_FACT && r.action == ANIMA_ACT_ANSWER && !strcmp(r.intent, "l1")
            && r.confidence >= L1_STITCH_GATE && a_is_describe(q)
#ifdef ANIMA_HOST
            && !getenv("ANIMA_NO_STITCH")          // A/B toggle for the fluency-grounded gate
#endif
            && nucleo_anima_l1_stitch(q, en, &r))
            trace_step(en ? "stitch: fuse spans" : "stitch: fondo frammenti");
        goto done;
    }

    // Miss -> typo rescue: correct command words and retry ONCE. Because this runs only after a
    // miss, queries that already work are never altered (routing can't regress by construction).
    // CORTEX plan, built lazily HERE (not before try_cascade) so the ~70-90% of queries L0/L1
    // answer never pay for it — true to the cascade's "don't light up what you don't need". Its job
    // here: suppress the command-vocab rescue on fact/live questions, where "correcting" a valid
    // content word toward the command vocab corrupts them ("domani" -> "comandi"). `plan` was already
    // computed up-front (before the LIVE block).
    g_anima_phase = 0x04;                  // DIAG: spellfix rescue
    if (plan.allow_spellfix) {
        char fixed[160];
        if (a_spellfix(q, fixed, sizeof(fixed)) && try_cascade(fixed, en, &r)) {
            snprintf(r.corrected, sizeof(r.corrected), "%.*s", (int)sizeof(r.corrected) - 1, fixed);   // "ho inteso: …"
            goto done;
        }
    }

    // NEURO-SYMBOLIC COMBINATOR tier (FIRST reasoning tier): a compositional question whose answer is
    // COMPUTED by composing >=2 learned facts and exists as NO single stored triple — "chi e nato prima
    // A o B" (compare), "quanti anni tra la nascita di A e B" (subtract), "X era europeo" (nationality->
    // continent), "A e B erano connazionali" (equality). Runs BEFORE the KGE deductive detector below so a
    // composition is never mis-parsed as a simple "X di Y" fact lookup (mirrors the sim's ordering).
    // Pure integer/string composition over the learned triples -> cannot fabricate. ANTI-HIJACK: if the
    // question IS compositional but a required fact is missing, it returns TRUE with an honest "non ho i
    // dati" miss — STOPPING the cascade so no later entity/recall tier answers a comparison with a random
    // bio. It returns false ONLY for a non-compositional query (then the cascade continues normally).
    g_anima_phase = 0x05;                  // DIAG: combinator tier
    if (nucleo_anima_combinator(q, en ? "en" : "it", &r)) {
        if (r.confidence > 0) { mem_update(&r); snprintf(s_mem.last_topic, sizeof(s_mem.last_topic), "%s", q); }
        s_session.dirty = true;
        goto done;
    }

    // OFFLINE DEDUCTIVE tier (HDC/permutation-KGE): a fact L0/L1 didn't have, but that is logically
    // ENTAILED by the learned triples — "qual e la capitale della Francia" (inverse), "in che continente
    // e Lione" (transitive), "quando e nato X" (forward). Deduced on-device by composing relation-
    // rotations, gated by resonance coherence (refuses rather than fabricating). Runs BEFORE the online
    // tiers so a verifiable offline deduction beats a network round-trip; on a refuse it falls through.
    //
    // Free L1's index (+ row cache) FIRST: the reasoner builds its own KG of entity hypervectors with
    // plain mallocs, which land in internal SRAM. L0/L1 already missed here, so the index isn't needed
    // for the rest of this query. Only when the HDC tier actually runs: when it was already tried this
    // turn, unloading just forced the next query to reload the index from the SD for nothing.
    if (!hdc_tried) {
        g_anima_phase = 0x06;              // DIAG: L1 unload (pre-HDC)
        nucleo_anima_l1_unload();
        g_anima_phase = 0x07;              // DIAG: HDC deductive (kg_load_subgraph + kg_build malloc)
        if (nucleo_anima_hdc_reason(q, en ? "en" : "it", &r)) {
            mem_update(&r);
            snprintf(s_mem.last_topic, sizeof(s_mem.last_topic), "%s", q);
            s_session.dirty = true;
            goto done;
        }
    }

    // HARD SAFETY GUARD: never let a non-question (bare date/number/code/garbage) reach the online
    // fetch + teacher tiers — those are the only ones that can FABRICATE a "fact" and learn it. The
    // deterministic + offline-deductive tiers above already had their chance; a miss here is honest.
    if (!askable) { memset(&r, 0, sizeof(r)); r.tier = ANIMA_TIER_NONE; r.action = ANIMA_ACT_NONE; goto done; }

    // Free the L1 index BEFORE the online fetch tiers. On this PSRAM-less chip the mbedTLS handshake
    // needs a large CONTIGUOUS heap block that the loaded ~18 KB index fragments away — without this
    // the Wikipedia/Wikidata fetch silently TIMES OUT and the user gets "non lo so" (then a garbage L1
    // "did you mean"). L0/L1 already missed, so the index isn't needed for the rest of this query; the
    // next query reloads it from SD on demand. (The teacher tier already does this same reclaim.)
    g_anima_phase = 0x08;                  // DIAG: post-askable, online/recall tiers
    if (nucleo_anima_online_available()) nucleo_anima_l1_unload();

    // Trusted-data-first ("dati certi → Grok"): the structured web tiers (Wikidata facts, then the
    // Wikipedia bio) run BEFORE the cloud teacher so a verifiable, learnable fact always wins over the
    // LLM's prose. This is safe now that the Wikipedia hang is fixed at the root — it was an httpd-task
    // stack overflow during the long cert-chain TLS handshake (config.stack_size now 16 KB), NOT a slow
    // or unreachable host — so the structured fetch completes in ~1s instead of stalling ~60s. Grok
    // stays the last-resort fallback for open-ended misses (the teacher tier at the end of the cascade).

    // POLICY (user): when ONLINE and a cloud LLM is configured, the online knowledge tiers go through the
    // LLM ALONE — no Wikipedia opensearch/summary, no Wikidata, no bare-noun lookup, no teacher truth-gate
    // fetch. Rationale is twofold: (1) coherence — one brain answers, not a patchwork; (2) RAM — each of
    // those structured tiers is a SEPARATE outbound TLS handshake (GET), and on this PSRAM-less chip every
    // handshake fragments the scarce heap, so doing 2-3 of them per "chi è X" was what left no contiguous
    // block for the next query ("online works once then stops"). Routing entity questions straight to the
    // chat LLM means ONE handshake per turn. In HYBRID (online_llm==false, defined above) Wikipedia/Wikidata
    // IS the path — one structured GET, remembered for offline. Offline cache/recall (network-free) run regardless.

    // Wikidata precise facts (born/died/capital/author): deterministic, no key. Before the Wikipedia
    // bio so "capitale di X" / "quando è nato X" gives the fact, not a summary. Skipped when the LLM owns
    // online (policy above); keyless devices still use it.
    if (!online_llm && nucleo_anima_online_fact(q, en, &r)) {
        mem_update(&r);
        snprintf(s_mem.last_topic, sizeof(s_mem.last_topic), "%s", q);
        s_session.dirty = true;
        goto done;
    }

    // Online knowledge tier (docs/anima-online.md): a "chi è / cos'è X" question L0+L1 didn't know.
    // With an LLM configured -> answer it via the chat model (one TLS POST), WITH conversation context.
    // Without a key -> the keyless Wikipedia path (learned-card cache, else a structured summary fetch
    // that is REMEMBERED for offline next time). Runs before the band so a real answer beats a fuzzy guess.
    {
        char entity[64], slug[64];
        if (nucleo_anima_online_entity(q, en, entity, sizeof(entity), slug, sizeof(slug))) {
            if (online_llm) {
                if (nucleo_anima_online_chat_ctx(q, ctx, nctx, en, &r)) {
                    mem_update(&r);
                    snprintf(s_mem.last_topic, sizeof(s_mem.last_topic), "%s", q);
                    s_session.dirty = true;
                    goto done;
                }
            } else if (nucleo_anima_online_answer(entity, slug, en, &r)) {
                mem_update(&r);
                snprintf(s_mem.last_topic, sizeof(s_mem.last_topic), "%s", q);   // enable "tell me more"
                s_session.dirty = true;
                goto done;
            }
        }
    }

    // Live tier (docs/anima-online.md): time/place-bound questions — weather, exchange rate, news.
    // Answered FRESH and NEVER learned: caching volatile data would assert stale facts as timeless
    // (volatility law §6). Runs before the band so a real live answer beats a fuzzy "did you mean".
    if (nucleo_anima_online_live(q, en, &r)) { mem_update(&r); s_session.dirty = true; goto done; }

    // USER-TAUGHT facts (offline, network-free): a paraphrase of something the USER taught with "ricorda
    // che / remember that". Embedded by the SAME shared encoder, gated by the same dual-channel discipline
    // as L1. Checked before the Wikipedia recall so a personal fact wins, and (unlike the online tier) it
    // also runs in the host harness, so the teach->recall loop is verifiable without a device. No network.
    g_anima_phase = 0x09;                  // DIAG: learn_recall (offline taught)
    if (nucleo_anima_learn_recall(q, en, &r)) {
        mem_update(&r);
        snprintf(s_mem.last_topic, sizeof(s_mem.last_topic), "%s", q);
        s_session.dirty = true;
        goto done;
    }

    // Semantic recall over LEARNED cards (docs/anima-online.md §5.2): a paraphrase of something the
    // device already learned, matched offline by the shared encoder. No network; conservative gate.
    if (nucleo_anima_online_recall(q, en, &r)) {
        mem_update(&r);
        snprintf(s_mem.last_topic, sizeof(s_mem.last_topic), "%s", q);   // enable "tell me more"
        s_session.dirty = true;
        goto done;
    }

    // CONTEXTUAL FOLLOW-UP (coreference): a subject-less fragment — "e cosa ha fatto?", "e perché?",
    // "e lui?". Handle it HERE, before the clarify band / truth-gated teacher (which would misread a
    // fragment). Online -> Grok with the REAL last turn as context (resolves pronouns/ellipsis). Offline
    // -> the last topic's card (the bio): it still knows what you're talking about.
    if (a_is_followup_q(q)) {
        if (online_llm && nucleo_anima_online_chat_ctx(q, ctx, nctx, en, &r)) {   // LLM-only; hybrid resolves the follow-up against the last topic's L1 card below
            snprintf(r.state, sizeof(r.state), "followup"); mem_update(&r); s_session.dirty = true; goto done;
        }
        if (s_mem.last_topic[0] && nucleo_anima_l1_query(s_mem.last_topic, en, false, &r)) {
            snprintf(r.state, sizeof(r.state), "followup"); mem_update(&r); s_session.dirty = true; goto done;
        }
    }

    // Still a miss -> dialogic clarify band (KNOWLEDGE only), on the last query's top-2 candidates.
    // Runs AFTER the typo rescue so a corrected launch/command isn't pre-empted by a fuzzy clarify.
    {
        long a1, a2;
        if (nucleo_anima_l1_band(en, &r, &a1, &a2)) {
            s_session.clarify_l1 = true; s_session.clarify_ans[0] = a1; s_session.clarify_ans[1] = a2;
            goto done;
        }
    }

    // Bare-noun entity fallback ("batman?", "einstein"): a short command-less noun L0/L1/clarify all
    // missed. STRICT Wikipedia lookup (free, no key) so junk stays an honest miss. Skipped when the LLM
    // owns online (it'll answer below); keyless devices still use it.
    if (!online_llm && nucleo_anima_online_entity_bare(q, en, &r)) {
        mem_update(&r);
        snprintf(s_mem.last_topic, sizeof(s_mem.last_topic), "%s", q);
        s_session.dirty = true;
        goto done;
    }

    // Last resort: the cloud teacher (LLM-backed: self-classifies via chat completions, verifies against
    // Wikipedia, learns). It IS an LLM call, so it runs ONLY when the LLM is allowed (online-only). In
    // HYBRID the Wikipedia/Wikidata tiers above already learned what was verifiable, with no LLM.
    if (online_llm && nucleo_anima_online_teacher(q, en, &r)) {
        mem_update(&r);
        snprintf(s_mem.last_topic, sizeof(s_mem.last_topic), "%s", q);
        s_session.dirty = true;
        goto done;
    }
    // The language model as the "save-the-day" fallback: LLM-first mode, and LOCAL / HYBRID once every
    // grounded tier missed (a question only - data and commands never reach a generator).
    if (r.tier == ANIMA_TIER_NONE && (online_llm || (llm_fallback && askable && nucleo_anima_model_usable())) &&
        nucleo_anima_online_chat_ctx(q, ctx, nctx, en, &r)) {
        mem_update(&r); s_session.dirty = true; goto done;
    }
    // An installed skill's offline answer beats a bare "non lo so" (no network needed).
    if (r.tier == ANIMA_TIER_NONE && askable) {
        char sk[sizeof r.reply];
        if (nucleo_anima_skills_offline(q, sk, sizeof sk)) {
            r.tier = ANIMA_TIER_FACT; r.action = ANIMA_ACT_ANSWER; r.confidence = 60;
            snprintf(r.intent, sizeof r.intent, "skill");
            snprintf(r.reply, sizeof r.reply, "%s", sk);
            goto done;
        }
    }
    // honest "non lo so": r already holds the NONE result from try_cascade.

done: {
        g_anima_stage = 0; g_anima_phase = 0;  // DIAG: query returned cleanly (no crash this turn)
        a_strip_foreign(r.reply);              // universal: clean foreign-script clutter even from old learned cards
        // Nobody understood: OFFER the closest known request instead of a dead end. Only a bare miss (a
        // named one like "weather" already says what it needs), never a replay or a just-confirmed turn.
        if (r.tier == ANIMA_TIER_NONE && !r.intent[0] && !replayed && !learned) {
            float p = 0;
            const char *sg = anima_intent_suggest(input, en, &p);
            if (sg) {
                suggest_offer(sg, input, en);
                memset(&r, 0, sizeof r);
                r.tier = ANIMA_TIER_COMMAND; r.action = ANIMA_ACT_ANSWER; r.confidence = (int)(p * 100.0f);
                r.awaiting = 1;
                snprintf(r.intent, sizeof r.intent, "suggest");
                snprintf(r.arg, sizeof r.arg, "%s", sg);
                snprintf(r.state, sizeof r.state, "slot");
                snprintf(r.reply, sizeof r.reply, en ? "I'm not sure I understood: do you mean \"%s\"? (yes/no)"
                                                     : "Non sono sicuro di aver capito: intendi «%s»? (sì/no)", sg);
            }
        }
        if (learned && r.tier != ANIMA_TIER_NONE && !r.corrected[0])
            snprintf(r.corrected, sizeof r.corrected, "%s", q);     // "understood: alza il volume" (and learned)
        // A miss after a FAILED cloud call says why (bad key, quota, unreachable) instead of a bare
        // "non lo so": the user can fix a key, but not a mystery.
        if (r.tier == ANIMA_TIER_NONE) {
            const char *why = nucleo_anima_online_fail_note(en);
            if (!why[0] && s_turn_degraded)
                why = en ? "no language model available" : "nessun modello linguistico disponibile";
            if (why[0]) {
                char base[sizeof r.reply];
                snprintf(base, sizeof base, "%s", r.reply[0] ? r.reply : (en ? "I don't know." : "Non lo so."));
                snprintf(r.reply, sizeof r.reply, "%s (%s: %s)", base, en ? "online" : "online", why);
            }
            // Never an empty reply (a bare "" reached Telegram and the REST API as silence): say it is a
            // miss AND what works without a model. action stays NONE, so a UI still sees the miss.
            if (!r.reply[0])
                snprintf(r.reply, sizeof r.reply, "%s",
                         en ? "I don't know that yet. Without a model I can open and close apps, tell the time and date, "
                              "do maths, set volume and brightness, timers and reminders, and answer from what is on the SD."
                            : "Non lo so ancora. Senza modello posso aprire e chiudere app, dirti ora e data, fare calcoli, "
                              "regolare volume e luminosità, mettere timer e promemoria e rispondere con quello che c'è sulla SD.");
        }
        // LLM mode answered by a lower rung: say so on a textual answer (a command's reply stays clean,
        // the flag tells the UI). "(offline)" without a network, "(senza modello)" when the web helped.
        if (s_turn_degraded) {
            r.degraded = 1;
            if (r.tier != ANIMA_TIER_NONE && r.action == ANIMA_ACT_ANSWER && r.reply[0] && strcmp(r.intent, "suggest")) {
                const bool net = nucleo_anima_online_available();
                char body[sizeof r.reply];
                snprintf(body, sizeof body, "%s", r.reply);
                snprintf(r.reply, sizeof r.reply, "%s %s", net ? (en ? "(no model)" : "(senza modello)") : "(offline)", body);
            }
        }
        s_no_model_turn = no_model_entry;      // the in-turn "don't dial it again" ends with the turn
        nucleo_anima_online_model_off(no_model_entry);
        if (replayed && r.action != ANIMA_ACT_NONE) { r.from_memory = 1; snprintf(r.state, sizeof(r.state), "followup"); }
        const char *domain = a_domain(&r);
        // Visible reasoning trace. A multi-step agent turn (compose-then-act) joins its steps with
        // " > " — the marker both UIs use to render a Claude-Code-style ⎿ plan. A single-tier answer
        // gets a one-line summary joined with " | " instead, so it's never mistaken for a plan.
        if (s_trace[0]) snprintf(r.trace, sizeof(r.trace), "%s", s_trace);
        else if (!r.trace[0]) {                // a tier's own trace (translate: the dictionary steps) stays
            const char *tn = r.tier == ANIMA_TIER_COMMAND ? "L0" : r.tier == ANIMA_TIER_FACT ? "L1" :
                             r.tier == ANIMA_TIER_REMOTE  ? "web" : r.tier == ANIMA_TIER_NONE ? "-" : "L2";
            if (r.budget > 0) snprintf(r.trace, sizeof(r.trace), "%s %s | %dcl | %d%%", tn, domain, r.budget, r.confidence);
            else              snprintf(r.trace, sizeof(r.trace), "%s %s | %d%%", tn, domain, r.confidence);
        }
        // Remember the input only for turns that did something, so it can be replayed; a miss or
        // an unanswered clarify is not a replay target.
        bool actionable = r.action != ANIMA_ACT_NONE && !r.awaiting && strcmp(domain, "clarify") != 0;
        ring_push(actionable ? q : "", r.intent, domain, r.arg);
        // Remember this turn for a follow-up "sei sicuro?" — including a MISS (so "sei sicuro?" after
        // an honest "non lo so" reports the miss, not a stale earlier answer). A dialogue-act turn must
        // NOT overwrite it, so a meta-question always refers to the real previous substantive turn.
        if (strcmp(r.intent,"sure") && strcmp(r.intent,"recap") &&
            strcmp(r.intent,"thanks") && strcmp(r.intent,"deny") && strcmp(r.intent,"explain")) {
            snprintf(s_session.last.reply, sizeof(s_session.last.reply), "%s", r.reply);
            s_session.last.tier = r.tier; s_session.last.conf = r.confidence;
            snprintf(s_session.last.intent, sizeof(s_session.last.intent), "%s", r.intent);
            // Append to the online-context transcript too, so the cloud teacher sees a real multi-turn
            // dialogue next time (a no-op for empty answers — a miss carries nothing to replay).
            if (r.tier != ANIMA_TIER_NONE && strcmp(r.intent, "stopped")) chat_push(q, r.reply);   // a stopped turn leaves no trace
        }
        // Capture the conversational FOCUS from the QUERY's structure, whichever tier answered (a capital
        // fact often comes from an L1 card, not the reasoner). A bare follow-up ("e newton?") is not itself
        // a fact question -> detect returns false -> the focus set by the shift's foc_remember stands.
        if (r.tier != ANIMA_TIER_NONE && r.action == ANIMA_ACT_ANSWER && !r.awaiting) {
            char drel[24], dsubj[48];
            if (nucleo_anima_hdc_detect(q, drel, sizeof drel, dsubj, sizeof dsubj)) {
                snprintf(s_session.foc_subject,  sizeof s_session.foc_subject,  "%s", dsubj);
                snprintf(s_session.foc_relation, sizeof s_session.foc_relation, "%s", drel);
                s_session.foc_turn = s_session.turn;
            }
        }
        telemetry_log(q, &r, domain);          // offline-learning work-list (misses + L1 only)
        session_save();                        // persist context if it changed
        ctx_save();                            // ...and the conversation the model sees
        diag_count(&r);                        // cumulative tier/abstain telemetry for /api/diag (cheap)
        return r;
    }
}
