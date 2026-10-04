// ANIMA offline LEXICON — definitions, synonyms, antonyms and inflected forms, IT and EN, from sorted TSV
// files on the SD (built by tools/dicts/gen_dicts.py from Wiktionary and Open English WordNet). Like the
// translator, it is grounded by construction: a word is in the file or it is not — no generation.
//
//   /data/anima/lex-it.tsv    key \t senses \t synonyms \t antonyms     (Italian Wiktionary)
//   /data/anima/lex-en.tsv    key \t senses \t synonyms \t antonyms     (Open English WordNet)
//   /data/anima/forms-it.tsv  form \t lemma[, lemma]                    ("andavo" -> "andare")
//   /data/anima/forms-en.tsv  form \t lemma[, lemma]                    ("went" -> "go")
//   senses = "pos: gloss | pos: gloss | ..."; synonyms/antonyms = "a, b, c" (either may be empty)
//
// Keys use ONE normalization for every dictionary (anima_dict_tokenize): lowercase ASCII, Italian
// accented vowels folded, everything else a separator — the generator writes keys the same way.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include "nucleo_anima.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ANIMA_DICT_TOKENS 24
#define ANIMA_DICT_TOKLEN 24

// Normalize + split like the firmware's a_tokenize(). Returns the token count.
int anima_dict_tokenize(const char *in, char tok[ANIMA_DICT_TOKENS][ANIMA_DICT_TOKLEN]);

// Exact lookup of `key` in a TSV sorted by key (byte order): binary search on the SD, one heap line
// buffer, nothing resident. 1 = found, `out` holds everything after the first tab; 0 = absent/no file.
int anima_dict_get(const char *path, const char *key, char *out, size_t cap);

// A dictionary request ("cosa significa effimero", "sinonimi di veloce", "il contrario di alto",
// "what does ephemeral mean", "synonyms of fast"). 1 = handled (*r filled: the entry, or an honest miss
// for synonyms/antonyms); 0 = not a lexicon request, or a definition the files don't have (so L1 / the
// model may still answer it).
int nucleo_anima_lex(const char *raw, bool en, anima_result_t *r);

// Detect-only: is `raw` a lexicon request? Lets earlier tiers (weather, volume) step aside for
// "sinonimi di pioggia" or "il contrario di alto".
bool nucleo_anima_lex_is_request(const char *raw);

// The definition of one word or short phrase (already isolated, e.g. the topic of a "cos'è X" that no
// other tier answered). Same reply as above; 0 when the lexicon doesn't have it.
int nucleo_anima_lex_define(const char *word, bool en, anima_result_t *r);

// The lemma of an inflected form ("andavo" -> "andare", "went" -> "go"), for the translator too.
// `it` picks the language. 1 = `out` holds the first lemma.
int anima_lex_lemma(const char *key, bool it, char *out, size_t cap);

#ifdef __cplusplus
}
#endif
