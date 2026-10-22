// json.h — a small in-place JSON reader (json.c): the catalog of books and the downloader's answers.
//
//   js_t j; js_init(&j, text, n);
//   if (js_enter(&j, '{')) while (js_next(&j, '}')) { char k[32]; js_key(&j, k, sizeof k); ... js_skip(&j); }
#pragma once
#include <stddef.h>
#include <stdint.h>

typedef struct { const char *p, *e; } js_t;

void js_init(js_t *j, const char *s, size_t n);
int js_enter(js_t *j, char open);          // '{' or '[': 1 when the next value is that
int js_next(js_t *j, char close);          // 1 another member / element follows; 0 the end (consumed)
int js_key(js_t *j, char *key, size_t cap);   // an object's key and its ':'
int js_skip(js_t *j);                      // the value, whatever it is
size_t js_string(js_t *j, char *out, size_t cap);   // a string value, decoded (else "" and skipped)
double js_number(js_t *j);
const char *js_here(const js_t *j);        // where the next value starts
size_t js_string_at(const char *p, const char *e, char *out, size_t cap);
