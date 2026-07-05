/* nv_keyboard_layouts — private accessor for the language-aware lv_keyboard maps.
 * Implemented in C (nv_keyboard_layouts.c) so the enum-OR-int ctrl initializers are legal;
 * consumed by nv_ime.cpp. Not a public component header. */
#pragma once
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Language selectors — must equal the nv_i18n nv_lang_t values passed by nv_ime.cpp.
 * Only DE/FR steer away from the QWERTY default. */
#define NV_KB_LANG_IT 1
#define NV_KB_LANG_FR 3
#define NV_KB_LANG_DE 4

/* Pointers into the file-static maps for one language (LOWER + UPPER planes). */
typedef struct {
    const char *const *lc_map;  const lv_buttonmatrix_ctrl_t *lc_ctrl;
    const char *const *uc_map;  const lv_buttonmatrix_ctrl_t *uc_ctrl;
} nv_kb_layout_t;

/* Fill *out with the map/ctrl pointers for `lang` (an nv_lang_t value). */
void nv_kb_layout_get(int lang, nv_kb_layout_t *out);

#ifdef __cplusplus
}
#endif
