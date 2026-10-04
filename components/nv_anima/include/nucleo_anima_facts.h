// ANIMA facts from Wikidata (CC0), offline: "quando è nato X", "quanti abitanti ha X", "capital of X",
// "wer schrieb X" — exact answers from the facts stored in each AKB6 record (tools/kb/facts.py), in the
// user's language, with the entity found by the same multilingual keys as the encyclopedia. See
// docs/ANIMA_KB.md "Fatti".
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include "nucleo_anima_kb.h"

#ifdef __cplusplus
extern "C" {
#endif

// The relation a question asks for ("born", "population", "capital", ...) and the entity key (normal form).
// False when the question is not a fact question this layer knows.
bool nucleo_anima_facts_parse(const char *q, char *rel, size_t rcap, char *key, size_t kcap);

// The relation of a question that names no entity ("qual è la formula chimica?"): the caller takes the
// entity from the topic in play. False when it is not such a question.
bool nucleo_anima_facts_parse_rel(const char *q, char *rel, size_t rcap);

// The key without leading articles ("les miserables" -> "miserables"), for a second lookup.
const char *nucleo_anima_facts_bare(const char *key);

// The answer sentence for `rel` of entity `ref`, in `lang` ("it", "en", "es", "fr", "de"). 1 = written,
// 0 = the entity has no such fact (the caller says so or lets another tier try).
int nucleo_anima_facts_answer(const char *rel, const anima_kb_ref_t *ref, const char *lang, char *out, size_t cap);

// Does entity `ref` carry fact `rel`? (picks the right one among same-named entities)
bool nucleo_anima_facts_has(const anima_kb_ref_t *ref, const char *rel);

#ifdef __cplusplus
}
#endif
