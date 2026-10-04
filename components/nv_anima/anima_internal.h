// ANIMA private cross-TU interface — firmware-internal, NOT the public API.
// (The public API is include/nucleo_anima.h.) This header is the contract between the
// orchestrator (nucleo_anima.c) and the math/skills solver engine (anima_solve.c), both
// of which are private to this component. See docs/anima.md §2.
#pragma once

// The system terminal programs (kSystemApps in nv_wasm.cpp) as the model must call them. In the text
// shell grammar (SHG_*) and in the shorter one sent with native tool schemas: without it a model
// does date math in its head or writes bash ($(..), date -d) this shell does not run.
#define ANIMA_SH_TOOLS_EN "SYSTEM TOOLS (exact answers: never work out dates, sums or math in your head, and this shell has no $(..), " \
    "$((..)), date -d, loops or if): eigenmath -e 'EXPR' (arithmetic, algebra, d(f,x), integral(f,x), roots, exact numbers) | " \
    "units -t '26.2 mi' km (units of measure: quote the amount with its unit; hours = hr) | datediff D1 D2 [-f '%db' business days] (ISO dates or today), " \
    "dateadd D +45d|+10b|+2mo, dateconv D -f '%A %d %B', dateseq D1 +1w D2 | curl -s URL | html2text (a web page as text) | " \
    "pdftotext F.pdf, pdfinfo F.pdf | gunzip -k, zcat, unxz, zstd -d (.gz .xz .zst) | lowdown -thtml -s F.md -o F.html | " \
    "qrencode -o F.svg TEXT | help CMD (exact syntax of any of them). "
#define ANIMA_SH_TOOLS_IT "STRUMENTI DI SISTEMA (risposte esatte: non calcolare mai a mente date, somme o matematica, e questa shell non ha $(..), " \
    "$((..)), date -d, cicli o if): eigenmath -e 'ESPR' (aritmetica, algebra, d(f,x), integral(f,x), roots, numeri esatti) | " \
    "units -t '26.2 mi' km (unita' di misura: quantita' e unita' tra apici; ore = hr) | datediff D1 D2 [-f '%db' giorni lavorativi] (date ISO o today), " \
    "dateadd D +45d|+10b|+2mo, dateconv D -f '%A %d %B', dateseq D1 +1w D2 | curl -s URL | html2text (pagina web come testo) | " \
    "pdftotext F.pdf, pdfinfo F.pdf | gunzip -k, zcat, unxz, zstd -d (.gz .xz .zst) | lowdown -thtml -s F.md -o F.html | " \
    "qrencode -o F.svg TESTO | help CMD (sintassi esatta di ognuno). "
#include <stdbool.h>
#include <stddef.h>
#include "nucleo_anima.h"   // anima_result_t and the tier/action enums

#ifdef __cplusplus
extern "C" {
#endif

// --- exported by nucleo_anima_online.c: the guarded HTTP paths (network mode, TLS budget, heap) ---
// GET / POST-JSON into a heap buffer (caller frees). Body length, or -1 (refused, failed, non-200).
int anima_net_get(const char *url, char **out);
int anima_net_post_json(const char *url, const char *body, char **out);
// The registered shell executor (nucleo_anima_set_shell): exit status, -1 busy, -100 none.
int anima_shell_run(const char *line, char *out, int cap);

// --- exported by anima_solve.c, called by the orchestrator -------------------

// Unified math/skills solver: date arithmetic, spreadsheet, geometry, physics, vectors,
// dimensional-analysis units, percent / Ohm / powers / roots / bases / roman / binop / ...
// Returns 1 when it answered exactly (fills *r), 0 to let the retrieval cascade take over.
int  anima_solve(const char *raw, bool en, anima_result_t *r);

// MULTI-STEP REASONING over the math engine — the conversational, "thinking" layer that sits ABOVE
// anima_solve: cascades ("…, poi moltiplica per 3, poi dimmi se è primo"), named registers across
// turns ("chiamalo A" … "A + B"), and a math self-description ("cosa sai calcolare?"). It orchestrates
// anima_solve step-by-step and is HONEST: a step it can't compute is reported, never faked. Runs only
// at the top level (the orchestrator's math entry), before anima_solve. 1 = answered, 0 = fall through.
int  anima_reason(const char *raw, bool en, anima_result_t *r);

// CONTEXT across turns: a short fragment continuing the previous result ("più 5?", "e diviso 2",
// "raddoppia", "la radice di quello") rewritten against `prev` into a standalone query ("36 piu 5").
// 1 only when the fragment refers back AND the rewrite computes; 0 leaves the input untouched.
int  anima_followup_math(const char *raw, double prev, bool en, char *out, size_t cap);

// --- exported by anima_context.c ---
// CONTEXT for non-maths fragments: rewrite `frag` against the previous query/intent/arg ("che tempo fa a
// roma" + "e a Milano?" -> "che tempo fa a milano"; "converti 5 km in miglia" + "e 10?"; "alza il volume" +
// "di più"). 1 = `out` holds the whole query; 0 = not a continuation of that intent.
int  anima_ctx_rewrite(const char *prev_q, const char *prev_intent, const char *prev_arg, const char *frag,
                       bool en, char *out, size_t cap);

// The principal numeric value carried by a reply: the LAST number in the text (the web twin's chaining
// convention) — so a follow-up can continue from "Fa 2430." → 2430. Returns 0 when the reply has no
// usable number (e.g. an honest refusal), which the cascade treats as "the chain can't continue".
int  a_reply_lastnum(const char *s, double *out);

// --- conversational numeric memory (defined in nucleo_anima.c, used by the reasoning layer) ---------
// Tiny RAM-only register file backed by the session and CLEARED ON RESET (never persisted to SD): the
// last answer plus a few user-named results. This is how an offline cascade "remembers" across turns.
int  anima_reg_get(const char *name, double *out);   // 1 if a register `name` (case-insensitive) is set
void anima_reg_set(const char *name, double val);    // create / overwrite; oldest evicted when full
int  anima_reg_last(double *out);                     // 1 if there is a remembered last answer
void anima_reg_set_last(double val);                  // remember the last numeric answer this turn

// Pure arithmetic: fold IT/EN word operators to symbols and evaluate + - * / and parentheses.
// Returns 0 = not an expression, 1 = ok (result in *out), 2 = division by zero.
int  a_try_calc(const char *raw, double *out);

// Format a double into `out`: trims trailing zeros, locale-free (always '.' decimal).
void a_fmt_num(double v, char *out, size_t n);     // precisione (%.6g): conversioni/unita'/geometria/fisica
void a_fmt_round(double v, char *out, size_t n);   // max 4 decimali: calcolo "normale" (aritmetica/medie)

// Restore user-defined units learned in a previous session (best-effort; SD may be absent).
void units_load(void);

// --- exported by anima_text.c (pure text utilities), called by the orchestrator ---

// Bilingual typo rescue: bounded Damerau-Levenshtein over ANIMA's command vocabulary.
// Applied only on a cascade miss. Returns 1 if it changed a word (corrected in `out`), 0 otherwise.
int  a_spellfix(const char *raw, char *out, size_t outsz);

// Damerau-Levenshtein distance between two short words, capped at `max` (returns max+1 once the
// cap is exceeded). Lets the solver tolerate a light typo in a math modifier ("cubica").
int  a_damlev(const char *a, const char *b, int max);

// Strip foreign-script clutter (Arabic/Cyrillic/CJK/...) from a reply IN PLACE — the device
// can't render it. Keeps Latin/Greek/punctuation; only ever shrinks the string.
void a_strip_foreign(char *s);

// Commit a rewritten temp file over a live store on the SD (ENGINEERING_RULES §5). The writer's errors
// are checked FIRST (a full card used to replace a good store with a truncated one), the original is
// removed (FATFS rename() refuses to overwrite) and on a rename failure the temp file is KEPT: it is
// the only good copy then. Closes `out`. True when `path` now holds the new data.
#include <stdio.h>
bool a_commit_tmp(FILE *out, const char *tmp, const char *path);
// a_write_atomic / a_mkdirs (built on it) are declared in nucleo_anima.h: the OS layer uses them too.

// --- defined in nucleo_anima.c, called by anima_solve.c ----------------------

// Normalize a phrase for substring matching: lowercase, de-accent, drop punctuation,
// and space-pad both ends so " word " tests are exact.
void a_norm_phrase(const char *raw, char *out, size_t cap);

// Host-only A/B switch for the pre-hardening lax behaviour (always false on device).
bool l0_legacy(void);

#ifdef __cplusplus
}
#endif
