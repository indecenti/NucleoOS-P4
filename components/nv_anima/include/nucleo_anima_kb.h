// ANIMA knowledge packs (AKB6) on the SD — Wikipedia (and later Wikidata) as ANIMA reads it offline.
// Format and measurements: docs/ANIMA_KB.md; builder: tools/kb/akb6.py. Packs live in /data/anima/kb/*.akb6.
// One answer = a bisect of the pack's KEYS section (~20 short reads) + one raw-deflate block inflated with
// tinfl (ESP32 ROM on the device, miniz on the host). Grounded by construction: text comes from the pack
// or there is no answer.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ANIMA_KB_MAXREF 4

typedef struct {
    int8_t   pack;            // index of the pack (nucleo_anima_kb_pack_*)
    uint32_t ent;             // entity id inside it
    char     title[96];
} anima_kb_ref_t;

typedef enum { ANIMA_KB_NONE = 0, ANIMA_KB_EXACT, ANIMA_KB_AMBIGUOUS, ANIMA_KB_SECTION } anima_kb_kind_t;

// The entity a question is about: "chi è X", "cos'è X", "parlami di X", "who is X", "qué es X"... -> X in
// the tokenizer's normal form. A bare name ("Leonardo da Vinci", at most 4 words) counts only when
// `bare_ok`. False when there is no topic.
bool nucleo_anima_kb_topic(const char *q, bool bare_ok, char *key, size_t cap);

// Look the topic up in the packs (the user's language first: "it", "en", "es", "fr", "de"). Fills up to
// `max` refs; returns the kind of hit. AMBIGUOUS: several entities share the name (refs = the options).
// SECTION: the name is only a part of the entity in refs[0] ("Jimbo Kern" -> Personaggi di South Park).
anima_kb_kind_t nucleo_anima_kb_find(const char *key, const char *lang, anima_kb_ref_t *refs, int max, int *n);

// Text of an entity: part 0 = summary, 1.. = passages. 1 = written, 0 = no such part / read error.
int nucleo_anima_kb_text(const anima_kb_ref_t *ref, int part, char *out, size_t cap);

// The entity's Wikidata facts line ("born=1879-03-14|birthplace=Ulm|gender=m", record field 6, written by
// tools/kb/facts.py + akb6.py). 1 = written, 0 = none.
int nucleo_anima_kb_facts(const anima_kb_ref_t *ref, char *out, size_t cap);

// The pack's language ("it") and attribution ("Wikipedia (it.wikipedia.org), CC BY-SA 4.0, via Kiwix").
const char *nucleo_anima_kb_pack_lang(int pack);
const char *nucleo_anima_kb_pack_attribution(int pack);
int nucleo_anima_kb_pack_count(void);      // scans /data/anima/kb once; nucleo_anima_kb_rescan() to refresh
void nucleo_anima_kb_rescan(void);
// Packs changed on the card (the store installed or removed one): the next question rescans. Safe from
// any task: it only marks the list stale, the rescan runs on ANIMA's own task.
void nucleo_anima_kb_invalidate(void);

#ifdef __cplusplus
}
#endif
