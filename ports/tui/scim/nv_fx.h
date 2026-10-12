/* nv_fx.h — Excel-style formulas over sc-im's expressions (see nv_fx.c). All return 0 on success,
 * -1 when the formula cannot be expressed exactly on the other side (out is then unusable). */
#pragma once
#include <stddef.h>

int nv_fx_lang(const char *code);   /* "en" "it" "es" "fr" "de" -> 0..4 (anything else: 0) */
int nv_fx_user_to_sc(const char *in, char *out, size_t n);
int nv_fx_sc_to_user(const char *sc, int lang, char *out, size_t n);
int nv_fx_excel_to_sc(const char *in, long rowoff, char *out, size_t n);
int nv_fx_sc_to_excel(const char *sc, long rowoff, char *out, size_t n);
/* Is this sc-im expression a text one (sc-im wants it as a label, not a number)? */
int nv_fx_is_string(const char *sc);
