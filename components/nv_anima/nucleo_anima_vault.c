// nucleo_anima_vault — see include/nucleo_anima_vault.h. Pure JSON rules, no I/O.
#include "nucleo_anima_vault.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"

// ---- names ------------------------------------------------------------------------------------

static bool is_secret(const char *name) {
    if (!name) return false;
    const size_t n = strlen(name);
    return !strcmp(name, "key") || (n > 4 && !strcmp(name + n - 4, "_key"));
}

// "key_set" / "vision_key_hint": the public view's companions of a secret, never stored.
static bool is_companion(const char *name) {
    if (!name) return false;
    const size_t n = strlen(name);
    char base[64];
    if (n > 4 && !strcmp(name + n - 4, "_set") && n - 4 < sizeof base) {
        memcpy(base, name, n - 4); base[n - 4] = 0;
        return is_secret(base);
    }
    if (n > 5 && !strcmp(name + n - 5, "_hint") && n - 5 < sizeof base) {
        memcpy(base, name, n - 5); base[n - 5] = 0;
        return is_secret(base);
    }
    return false;
}

// The member naming the host a secret is sent to: "key" -> "base", "vision_key" -> "vision_base"
// (or "vision_url" when the object uses that).
static const char *target_of(cJSON *obj, const char *secret, char *buf, size_t n) {
    if (!strcmp(secret, "key")) return "base";
    const size_t sl = strlen(secret) - 4;   // drop "_key"
    snprintf(buf, n, "%.*s_base", (int)sl, secret);
    if (cJSON_GetObjectItem(obj, buf)) return buf;
    snprintf(buf, n, "%.*s_url", (int)sl, secret);
    return buf;
}

// ---- hosts ------------------------------------------------------------------------------------

// "https://User@API.Groq.com:443/openai/v1" -> "https://api.groq.com". False when there is no scheme
// or no host. Default ports are dropped, so ":443" and none compare equal.
static bool host_of(const char *url, char *out, size_t n) {
    if (!url || !n) return false;
    const char *sep = strstr(url, "://");
    if (!sep || sep == url) return false;
    const char *a = sep + 3;
    const char *e = a + strcspn(a, "/?#");
    const char *at = NULL;
    for (const char *p = a; p < e; p++) if (*p == '@') at = p;   // userinfo: the host follows the last '@'
    if (at) a = at + 1;
    if (a >= e) return false;
    const size_t sl = (size_t)(sep - url), hl = (size_t)(e - a);
    if (sl + 3 + hl + 1 > n) return false;
    size_t o = 0;
    for (size_t i = 0; i < sl; i++) out[o++] = (char)tolower((unsigned char)url[i]);
    memcpy(out + o, "://", 3); o += 3;
    for (size_t i = 0; i < hl; i++) out[o++] = (char)tolower((unsigned char)a[i]);
    out[o] = 0;
    const bool https = !strncmp(out, "https://", 8), http = !strncmp(out, "http://", 7);
    const size_t ol = strlen(out);
    if (https && ol > 4 && !strcmp(out + ol - 4, ":443")) out[ol - 4] = 0;
    if (http && ol > 3 && !strcmp(out + ol - 3, ":80")) out[ol - 3] = 0;
    return true;
}

static const char *str_of(cJSON *obj, const char *name) {
    cJSON *it = obj ? cJSON_GetObjectItem(obj, name) : NULL;
    return cJSON_IsString(it) ? it->valuestring : NULL;
}

// Does `secret` in `in` still go where it went in `st`? Same "provider" (when either names one) and
// the same host in the target member (when either has one).
static bool same_target(cJSON *in, cJSON *st, const char *secret) {
    const char *pi = str_of(in, "provider"), *ps = str_of(st, "provider");
    if ((pi || ps) && (!pi || !ps || strcmp(pi, ps) != 0)) return false;
    char tb[64];
    const char *target = target_of(st, secret, tb, sizeof tb);
    const char *ui = str_of(in, target), *us = str_of(st, target);
    if (!ui && !us) return true;
    char hi[160], hs[160];
    return ui && us && host_of(ui, hi, sizeof hi) && host_of(us, hs, sizeof hs) && !strcmp(hi, hs);
}

// ---- public view --------------------------------------------------------------------------------

static void hint_of(const char *k, char *out, size_t n) {
    const size_t l = strlen(k);
    if (l >= 12) snprintf(out, n, "%.4s\xE2\x80\xA6%s", k, k + l - 4);
    else snprintf(out, n, "\xE2\x80\xA6");
}

static void blank_secrets(cJSON *obj) {
    if (cJSON_IsArray(obj)) {
        for (cJSON *c = obj->child; c; c = c->next) blank_secrets(c);
        return;
    }
    if (!cJSON_IsObject(obj)) return;
    // Companions are added after the walk (adding while iterating would revisit them).
    char names[16][48];
    bool set[16];
    char hints[16][32];
    int n = 0;
    for (cJSON *c = obj->child; c; c = c->next) {
        if (is_secret(c->string)) {
            const char *v = cJSON_IsString(c) ? c->valuestring : NULL;
            const bool has = v && v[0];
            if (n < 16 && strlen(c->string) < sizeof names[0]) {
                snprintf(names[n], sizeof names[0], "%s", c->string);
                set[n] = has;
                hints[n][0] = 0;
                if (has) hint_of(v, hints[n], sizeof hints[n]);
                n++;
            }
            if (v) memset(c->valuestring, 0, strlen(v));   // freed below without being wiped
            cJSON *blank = cJSON_CreateString("");
            if (blank) cJSON_ReplaceItemInObject(obj, c->string, blank);
            c = blank ? blank : c;
        } else if (is_companion(c->string)) {
            continue;   // stale companions from an older write: rewritten below
        } else {
            blank_secrets(c);
        }
    }
    for (int i = 0; i < n; i++) {
        char nm[64];
        snprintf(nm, sizeof nm, "%s_set", names[i]);
        cJSON_DeleteItemFromObject(obj, nm);
        cJSON_AddBoolToObject(obj, nm, set[i]);
        snprintf(nm, sizeof nm, "%s_hint", names[i]);
        cJSON_DeleteItemFromObject(obj, nm);
        if (set[i]) cJSON_AddStringToObject(obj, nm, hints[i]);
    }
}

char *nucleo_vault_public(const char *json) {
    cJSON *root = json ? cJSON_Parse(json) : NULL;
    if (!root) return NULL;
    blank_secrets(root);
    char *out = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return out;
}

// ---- merge ----------------------------------------------------------------------------------------

static void drop_companions(cJSON *obj) {
    for (cJSON *c = obj->child; c;) {
        cJSON *nx = c->next;
        if (is_companion(c->string)) cJSON_DeleteItemFromObject(obj, c->string);
        c = nx;
    }
}

static void merge_obj(cJSON *in, cJSON *st) {
    if (!cJSON_IsObject(in)) return;
    drop_companions(in);
    // Children first: a top-level key may be derived from a merged "keys" entry (below).
    for (cJSON *c = in->child; c; c = c->next)
        if (cJSON_IsObject(c)) merge_obj(c, st ? cJSON_GetObjectItem(st, c->string) : NULL);
    // Stored secrets the page did not mention at all (before the nulls below are dropped: a removed
    // key must not come back as "not mentioned").
    for (cJSON *s = st ? st->child : NULL; s; s = s->next) {
        if (!is_secret(s->string) || cJSON_GetObjectItem(in, s->string)) continue;
        if (cJSON_IsString(s) && s->valuestring[0] && same_target(in, st, s->string))
            cJSON_AddStringToObject(in, s->string, s->valuestring);
    }
    // Secrets the page blanked (keep the stored one) or nulled (remove it).
    for (cJSON *c = in->child; c;) {
        cJSON *nx = c->next;
        if (is_secret(c->string) && !(cJSON_IsString(c) && c->valuestring[0])) {
            const char *sv = (!cJSON_IsNull(c) && st) ? str_of(st, c->string) : NULL;
            if (sv && sv[0] && same_target(in, st, c->string)) cJSON_ReplaceItemInObject(in, c->string, cJSON_CreateString(sv));
            else cJSON_DeleteItemFromObject(in, c->string);
        }
        c = nx;
    }
}

// The top-level key is the active provider's: when the page did not send one and the stored one did
// not carry over (another provider is active now), take it from that provider's "keys" entry, if the
// entry talks to the same host as the top level.
static void mirror_active(cJSON *root) {
    const char *k = str_of(root, "key");
    if (k && k[0]) return;
    const char *prov = str_of(root, "provider");
    cJSON *keys = cJSON_GetObjectItem(root, "keys");
    cJSON *e = (prov && cJSON_IsObject(keys)) ? cJSON_GetObjectItem(keys, prov) : NULL;
    const char *ek = str_of(e, "key");
    if (!ek || !ek[0]) return;
    const char *rb = str_of(root, "base"), *eb = str_of(e, "base");
    char hr[160], he[160];
    if (rb && eb && (!host_of(rb, hr, sizeof hr) || !host_of(eb, he, sizeof he) || strcmp(hr, he) != 0)) return;
    if (!rb && eb) cJSON_AddStringToObject(root, "base", eb);
    cJSON_DeleteItemFromObject(root, "key");
    cJSON_AddStringToObject(root, "key", ek);
}

char *nucleo_vault_merge(const char *stored, const char *incoming, char *err, size_t errn) {
    cJSON *in = incoming ? cJSON_Parse(incoming) : NULL;
    if (!cJSON_IsObject(in)) {
        if (err && errn) snprintf(err, errn, "not a JSON object");
        cJSON_Delete(in);
        return NULL;
    }
    cJSON *st = (stored && stored[0]) ? cJSON_Parse(stored) : NULL;
    if (!cJSON_IsObject(st)) { cJSON_Delete(st); st = NULL; }
    // A page that writes only the active provider (onboarding, a model switch) does not delete the
    // other providers: no "keys" member at all keeps the stored ones; "keys": {} empties them.
    cJSON *sk = st ? cJSON_GetObjectItem(st, "keys") : NULL;
    if (cJSON_IsObject(sk) && !cJSON_GetObjectItem(in, "keys")) cJSON_AddItemToObject(in, "keys", cJSON_Duplicate(sk, 1));
    merge_obj(in, st);
    mirror_active(in);
    char *out = cJSON_Print(in);
    cJSON_Delete(in);
    cJSON_Delete(st);
    return out;
}

// ---- relay credential -----------------------------------------------------------------------------

static bool find_key(cJSON *obj, const char *host, const char **key, cJSON **owner) {
    if (cJSON_IsArray(obj)) {
        for (cJSON *c = obj->child; c; c = c->next) if (find_key(c, host, key, owner)) return true;
        return false;
    }
    if (!cJSON_IsObject(obj)) return false;
    for (cJSON *c = obj->child; c; c = c->next) {
        if (!is_secret(c->string) || !cJSON_IsString(c) || !c->valuestring[0]) continue;
        char tb[64], h[160];
        const char *target = str_of(obj, target_of(obj, c->string, tb, sizeof tb));
        if (target && host_of(target, h, sizeof h) && !strcmp(h, host)) { *key = c->valuestring; *owner = obj; return true; }
    }
    for (cJSON *c = obj->child; c; c = c->next)
        if ((cJSON_IsObject(c) || cJSON_IsArray(c)) && find_key(c, host, key, owner)) return true;
    return false;
}

bool nucleo_vault_auth_for(const char *json, const char *url, char *name, size_t namen,
                           char *value, size_t valuen, char *version, size_t versionn) {
    char host[160];
    if (!json || !host_of(url, host, sizeof host)) return false;
    cJSON *root = cJSON_Parse(json);
    if (!root) return false;
    const char *key = NULL;
    cJSON *owner = NULL;
    bool ok = find_key(root, host, &key, &owner);
    if (ok) {
        const bool anthropic = strstr(host, "://api.anthropic.com") != NULL;
        if (anthropic) {
            snprintf(name, namen, "x-api-key");
            snprintf(value, valuen, "%s", key);
            const char *v = str_of(owner, "version");
            if (version && versionn) snprintf(version, versionn, "%s", v && v[0] ? v : "2023-06-01");
        } else {
            snprintf(name, namen, "Authorization");
            snprintf(value, valuen, "Bearer %s", key);
            if (version && versionn) version[0] = 0;
        }
        ok = strlen(key) + 8 < valuen;   // a truncated key is no key
        if (!ok) memset(value, 0, valuen);
    }
    // cJSON frees its strings without wiping them; overwrite the parsed keys first.
    for (cJSON *stack[16], **sp = stack, *o = root; o;) {
        for (cJSON *c = o->child; c; c = c->next)
            if (is_secret(c->string) && cJSON_IsString(c)) memset(c->valuestring, 0, strlen(c->valuestring));
        cJSON *next = NULL;
        for (cJSON *c = o->child; c; c = c->next)
            if ((cJSON_IsObject(c) || cJSON_IsArray(c)) && sp < stack + 16) *sp++ = c;
        next = sp > stack ? *--sp : NULL;
        o = next;
    }
    cJSON_Delete(root);
    return ok;
}
