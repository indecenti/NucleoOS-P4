// nucleo_anima_vault — the rules that keep the model-provider keys of teacher.json on the device.
//
// teacher.json (sealed to the chip on the SD card) holds the API keys: "key" and "vision_key" at the
// top level, "keys": {<provider>: {"base", "model", "key", ...}} per provider. A browser page must
// never read them: it gets a public view with every key blanked (plus "<name>_set" and a masked
// "<name>_hint"), writes go through a merge that keeps the stored keys a page cannot send back, and
// the /api/llm relay adds the key itself, only towards the host that key was saved for.
//
// Pure functions over JSON text (cJSON only), so the host tests cover them; the sealed file I/O is
// the caller's (nv_web). Secret members: "key", or a name ending in "_key", at any depth.
#pragma once
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// A copy of the vault with every secret blanked: "key": "" plus "key_set": true/false and, when set,
// "key_hint": "gsk_…wxyz" (first 4 and last 4 characters; just "…" for a short key). Caller frees
// (free). NULL when `json` does not parse.
char *nucleo_vault_public(const char *json);

// The document to store when a page writes `incoming` over `stored` (either may be NULL / empty):
// a secret that `incoming` leaves out, sends as "" or as a "<name>_set"/"<name>_hint" echo of the public
// view, keeps the stored value — but only when the object it sits in still talks to the same host
// (same "provider", same scheme://host[:port] in "base", or for "vision_key" in "vision_base"), so a
// key is never carried over to another provider. A secret sent as null is removed. "_set" / "_hint"
// members are never stored. Caller frees. NULL when `incoming` does not parse (err says why).
char *nucleo_vault_merge(const char *stored, const char *incoming, char *err, size_t errn);

// The credential the relay adds to a request for `url`: the stored key whose object's "base" (or
// "vision_base") has the same scheme://host[:port] as `url`. Anthropic hosts get "x-api-key" (and
// `version`, the entry's "version" or 2023-06-01), the others "Authorization: Bearer <key>".
// False when no stored key belongs to that host. The caller wipes `value` after use.
bool nucleo_vault_auth_for(const char *json, const char *url, char *name, size_t namen,
                           char *value, size_t valuen, char *version, size_t versionn);

#ifdef __cplusplus
}
#endif
