// ANIMA in Spanish, French and German, offline. The engine reasons in Italian and English; a request in
// es/fr/de is read THROUGH English and the reply comes back in the user's language:
//
//   "sube el volumen"  --fold-->  "sube el volumen"  --phrase table-->  "turn the volume up"  --> engine
//   "abre la calculadora" --glossary--> "open the calculator"                                   --> engine
//   engine: "Raising the volume."  --reply table-->  "Subo el volumen."
//
// The phrase table is the generated anima_phrases.c (tools/anima_phrases_xl.txt); the glossary and the
// reply table live in anima_lang.c. A reply the table doesn't know stays in English (what these users got
// before), never a guess. See docs/ANIMA_L0.md, "Spagnolo, francese, tedesco".
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include "nucleo_anima.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum { ANIMA_XL_NONE = 0, ANIMA_XL_ES, ANIMA_XL_FR, ANIMA_XL_DE } anima_xlang_t;

// "es"/"fr"/"de" (any case, "es-ES" too) -> the language; anything else (it, en, NULL) -> NONE.
anima_xlang_t anima_xlang(const char *lang);
const char *anima_xlang_code(anima_xlang_t xl);           // "es" / "fr" / "de" / ""

// The language of the turn in progress (set by nucleo_anima_query around the engine call), so a tier
// such as the translator can read a word in the user's own language. NONE outside es/fr/de turns.
void anima_lang_set_current(anima_xlang_t xl);
anima_xlang_t anima_lang_current(void);
// The user's own words for this turn (before the English reading), "" outside es/fr/de turns. Tiers that look
// up NAMES use it: "Sexe, Mensonges et Vidéo" must not become "... and Vidéo".
void anima_lang_set_original(const char *text);
const char *anima_lang_original(void);

// Lowercase and fold the Latin letters of es/fr/de (á ñ ü ö ä ß ç œ ¿ ¡ ...) to ASCII. Mirrors
// xfold() in tools/gen_anima_phrases.py.
void anima_lang_fold(const char *in, char *out, size_t cap);

// The English the engine should read for `in`: a whole-phrase match first ("¿qué hora es?" -> "what time
// is it"), else the command glossary word by word ("abre la calculadora" -> "open the calculator").
// Words the glossary doesn't know (names, numbers, the text of a note) pass through unchanged.
void anima_lang_to_en(const char *in, anima_xlang_t xl, char *out, size_t cap);

// Rewrite an English reply in the user's language, in place: the reply table, then dates and day/month
// names. Placeholders such as {value} survive. True when the reply was translated.
bool anima_lang_localize(anima_result_t *r, anima_xlang_t xl);

#ifdef __cplusplus
}
#endif
