// json.c — a small JSON reader for the store's catalog of books (docs/BOOKS_PLAN.md §2) and the
// system downloader's answers: walks a document in place, no tree, nothing allocated. Strings are
// decoded (escapes, \uXXXX with surrogate pairs) into the caller's buffer. The input is untrusted
// (it comes from the network): every step is bounded by the end of the text, a malformed document
// ends the walk (fuzzed: test/fuzz_json.c).
#include "json.h"

#include <stdlib.h>
#include <string.h>

int utf8_put(uint32_t cp, char *out);   // font.c

static const char *ws(const char *p, const char *e) {
    while (p < e && (*p == ' ' || *p == '\n' || *p == '\r' || *p == '\t')) p++;
    return p;
}

// The end of the value starting at p (after white space), or NULL when it is malformed.
static const char *value_end(const char *p, const char *e, int depth) {
    p = ws(p, e);
    if (p >= e || depth > 32) return NULL;
    if (*p == '"') {
        for (p++; p < e; p++) {
            if (*p == '\\') { p++; continue; }
            if (*p == '"') return p + 1;
        }
        return NULL;
    }
    if (*p == '{' || *p == '[') {
        const char close = *p == '{' ? '}' : ']';
        p = ws(p + 1, e);
        if (p < e && *p == close) return p + 1;
        for (;;) {
            if (close == '}') {   // "key": value
                p = value_end(p, e, depth + 1);
                if (!p) return NULL;
                p = ws(p, e);
                if (p >= e || *p != ':') return NULL;
                p++;
            }
            p = value_end(p, e, depth + 1);
            if (!p) return NULL;
            p = ws(p, e);
            if (p >= e) return NULL;
            if (*p == close) return p + 1;
            if (*p != ',') return NULL;
            p++;
        }
    }
    while (p < e && !strchr(",}] \n\r\t", *p)) p++;   // number, true, false, null
    return p;
}

void js_init(js_t *j, const char *s, size_t n) {
    j->p = s;
    j->e = s + n;
}

int js_enter(js_t *j, char open) {
    j->p = ws(j->p, j->e);
    if (j->p >= j->e || *j->p != open) return 0;
    j->p++;
    return 1;
}

int js_next(js_t *j, char close) {
    j->p = ws(j->p, j->e);
    if (j->p >= j->e) return 0;
    if (*j->p == ',') j->p = ws(j->p + 1, j->e);
    if (j->p >= j->e || *j->p == close) {
        if (j->p < j->e) j->p++;
        return 0;
    }
    return 1;
}

size_t js_string_at(const char *p, const char *e, char *out, size_t cap) {
    size_t k = 0;
    p = ws(p, e);
    if (!cap) return 0;
    out[0] = 0;
    if (p >= e || *p != '"') return 0;
    for (p++; p < e && *p != '"'; p++) {
        uint32_t c = (unsigned char)*p;
        if (c == '\\' && p + 1 < e) {
            p++;
            switch (*p) {
            case 'n': c = '\n'; break;
            case 't': c = '\t'; break;
            case 'r': c = '\r'; break;
            case 'b': c = 8; break;
            case 'f': c = 12; break;
            case 'u': {
                if (p + 4 >= e) { c = 0xFFFD; break; }
                c = (uint32_t)strtoul((char[5]){p[1], p[2], p[3], p[4], 0}, NULL, 16);
                p += 4;
                if (c >= 0xD800 && c < 0xDC00 && p + 6 < e && p[1] == '\\' && p[2] == 'u') {
                    const uint32_t lo = (uint32_t)strtoul((char[5]){p[3], p[4], p[5], p[6], 0}, NULL, 16);
                    if (lo >= 0xDC00 && lo < 0xE000) { c = 0x10000 + ((c - 0xD800) << 10) + (lo - 0xDC00); p += 6; }
                }
                break;
            }
            default: c = (unsigned char)*p; break;   // \" \\ \/
            }
            if (c < 0x20) c = ' ';
            char b[4];
            const int n = utf8_put(c, b);
            if (k + (size_t)n < cap) { memcpy(out + k, b, (size_t)n); k += (size_t)n; }
            continue;
        }
        if (c < 0x20) c = ' ';
        if (k + 1 < cap) out[k++] = (char)c;
    }
    out[k] = 0;
    return k;
}

int js_key(js_t *j, char *key, size_t cap) {
    j->p = ws(j->p, j->e);
    const char *end = value_end(j->p, j->e, 0);
    if (!end || *j->p != '"') { j->p = j->e; return 0; }
    js_string_at(j->p, end, key, cap);
    j->p = ws(end, j->e);
    if (j->p >= j->e || *j->p != ':') { j->p = j->e; return 0; }
    j->p++;
    return 1;
}

int js_skip(js_t *j) {
    const char *end = value_end(j->p, j->e, 0);
    if (!end) { j->p = j->e; return 0; }
    j->p = end;
    return 1;
}

size_t js_string(js_t *j, char *out, size_t cap) {
    const char *start = j->p;
    if (!js_skip(j)) { if (cap) out[0] = 0; return 0; }
    return js_string_at(start, j->p, out, cap);
}

double js_number(js_t *j) {
    const char *start = ws(j->p, j->e);
    if (!js_skip(j)) return 0;
    char b[32];
    size_t n = (size_t)(j->p - start);
    if (n >= sizeof b) n = sizeof b - 1;
    memcpy(b, start, n);
    b[n] = 0;
    return strtod(b, NULL);
}

const char *js_here(const js_t *j) { return ws(j->p, j->e); }
