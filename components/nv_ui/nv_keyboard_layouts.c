/* nv_keyboard_layouts — language-aware custom lv_keyboard maps (C, on purpose).
 *
 * The button-matrix ctrl arrays are `lv_buttonmatrix_ctrl_t[]` initialized with OR'd flag|width
 * expressions (e.g. LV_BUTTONMATRIX_CTRL_CHECKED | 7). Those expressions have type `int`, and
 * C++ forbids the implicit int->unscoped-enum conversion in an aggregate initializer — so these
 * tables live in a C translation unit, and nv_ime.cpp consumes them through nv_kb_layout_get().
 *
 * lv_keyboard_set_map() stores the map/ctrl POINTERS into LVGL globals, so every array here has
 * static (file-scope) lifetime. Row shapes match LVGL's default letter rows (10/9/7 letters =>
 * 12/11/12 buttons) so the ctrl widths reuse the proven default pattern verbatim, plus one
 * uniform 12-key accent row and the shared 6-key control row (53 ctrl entries per map).
 */
#include "nv_keyboard_layouts.h"
#include "lvgl.h"

/* LVGL keeps these private to lv_keyboard.c; re-declare local equivalents. LV_KB_BTN adds the
 * POPOVER width flag exactly as LVGL does (the keyboard strips POPOVER when popovers are off).
 * The mode literals must equal LVGL's defaults for the built-in event cb to recognize them. */
#define NV_KB_BTN(w)          (LV_BUTTONMATRIX_CTRL_POPOVER | (w))
#define NV_KB_MODE_SPECIAL    "1#"
#define NV_KB_MODE_UPPER      "ABC"
#define NV_KB_MODE_LOWER      "abc"

#define KB_W1  NV_KB_BTN(1)
#define KB_W3  NV_KB_BTN(3)
#define KB_W4  NV_KB_BTN(4)

/* KEYBOARD(hide) | 1#(special) | LEFT | SPACE | RIGHT | OK */
#define NV_KB_CTRL_ROW \
    LV_SYMBOL_KEYBOARD, NV_KB_MODE_SPECIAL, \
    LV_SYMBOL_LEFT, " ", LV_SYMBOL_RIGHT, LV_SYMBOL_OK, ""

#define NV_KB_CTRL_ROW_CTRL \
    LV_KEYBOARD_CTRL_BUTTON_FLAGS | 2, \
    LV_KEYBOARD_CTRL_BUTTON_FLAGS | 2, \
    LV_BUTTONMATRIX_CTRL_CHECKED | 2, 6, LV_BUTTONMATRIX_CTRL_CHECKED | 2, \
    LV_KEYBOARD_CTRL_BUTTON_FLAGS | 2

#define NV_KB_ACCENT_ROW_CTRL \
    KB_W1, KB_W1, KB_W1, KB_W1, KB_W1, KB_W1, KB_W1, KB_W1, KB_W1, KB_W1, KB_W1, KB_W1

/* Three standard letter rows — ctrl copied from LVGL's default_kb_ctrl_lc_map (12+11+12=35). */
#define NV_KB_LETTER_ROWS_CTRL \
    LV_KEYBOARD_CTRL_BUTTON_FLAGS | 5, KB_W4, KB_W4, KB_W4, KB_W4, KB_W4, \
        KB_W4, KB_W4, KB_W4, KB_W4, KB_W4, LV_BUTTONMATRIX_CTRL_CHECKED | 7, \
    LV_KEYBOARD_CTRL_BUTTON_FLAGS | 6, KB_W3, KB_W3, KB_W3, KB_W3, KB_W3, \
        KB_W3, KB_W3, KB_W3, KB_W3, LV_BUTTONMATRIX_CTRL_CHECKED | 7, \
    LV_BUTTONMATRIX_CTRL_CHECKED | KB_W1, LV_BUTTONMATRIX_CTRL_CHECKED | KB_W1, \
        KB_W1, KB_W1, KB_W1, KB_W1, KB_W1, KB_W1, KB_W1, \
        LV_BUTTONMATRIX_CTRL_CHECKED | KB_W1, LV_BUTTONMATRIX_CTRL_CHECKED | KB_W1, \
        LV_BUTTONMATRIX_CTRL_CHECKED | KB_W1

/* -------- QWERTY (EN / IT / ES) -------- */
static const char *const qwerty_lc[] = {
    "1#", "q", "w", "e", "r", "t", "y", "u", "i", "o", "p", LV_SYMBOL_BACKSPACE, "\n",
    "ABC", "a", "s", "d", "f", "g", "h", "j", "k", "l", LV_SYMBOL_NEW_LINE, "\n",
    "_", "-", "z", "x", "c", "v", "b", "n", "m", ".", ",", ":", "\n",
    "à", "á", "è", "é", "ì", "í", "ò", "ó", "ù", "ú", "ñ", "ç", "\n",
    NV_KB_CTRL_ROW
};
static const lv_buttonmatrix_ctrl_t qwerty_lc_c[] = {
    NV_KB_LETTER_ROWS_CTRL, NV_KB_ACCENT_ROW_CTRL, NV_KB_CTRL_ROW_CTRL };

static const char *const qwerty_uc[] = {
    "1#", "Q", "W", "E", "R", "T", "Y", "U", "I", "O", "P", LV_SYMBOL_BACKSPACE, "\n",
    "abc", "A", "S", "D", "F", "G", "H", "J", "K", "L", LV_SYMBOL_NEW_LINE, "\n",
    "_", "-", "Z", "X", "C", "V", "B", "N", "M", ".", ",", ":", "\n",
    "À", "Á", "È", "É", "Ì", "Í", "Ò", "Ó", "Ù", "Ú", "Ñ", "Ç", "\n",
    NV_KB_CTRL_ROW
};
static const lv_buttonmatrix_ctrl_t qwerty_uc_c[] = {
    NV_KB_LETTER_ROWS_CTRL, NV_KB_ACCENT_ROW_CTRL, NV_KB_CTRL_ROW_CTRL };

/* -------- ITALIAN — QWERTY, ergonomics tuned for IT:
 *   • apostrophe ' on the main plane (l', un', dell' — essential and was missing)
 *   • only the accents Italian actually uses: à è é ì ò ù (no ñ / acute doubles)
 *   • the reclaimed keys become useful punctuation: @ ! ? ; ( )
 * Same row shapes as QWERTY, so it reuses qwerty_lc_c / qwerty_uc_c verbatim. */
static const char *const it_lc[] = {
    "1#", "q", "w", "e", "r", "t", "y", "u", "i", "o", "p", LV_SYMBOL_BACKSPACE, "\n",
    "ABC", "a", "s", "d", "f", "g", "h", "j", "k", "l", LV_SYMBOL_NEW_LINE, "\n",
    "'", "-", "z", "x", "c", "v", "b", "n", "m", ".", ",", ":", "\n",
    "à", "è", "é", "ì", "ò", "ù", "@", "!", "?", ";", "(", ")", "\n",
    NV_KB_CTRL_ROW
};
static const char *const it_uc[] = {
    "1#", "Q", "W", "E", "R", "T", "Y", "U", "I", "O", "P", LV_SYMBOL_BACKSPACE, "\n",
    "abc", "A", "S", "D", "F", "G", "H", "J", "K", "L", LV_SYMBOL_NEW_LINE, "\n",
    "'", "-", "Z", "X", "C", "V", "B", "N", "M", ".", ",", ":", "\n",
    "À", "È", "É", "Ì", "Ò", "Ù", "@", "!", "?", ";", "(", ")", "\n",
    NV_KB_CTRL_ROW
};

/* -------- QWERTZ (DE) — y<->z swapped, ä ö ü ß on the accent row -------- */
static const char *const qwertz_lc[] = {
    "1#", "q", "w", "e", "r", "t", "z", "u", "i", "o", "p", LV_SYMBOL_BACKSPACE, "\n",
    "ABC", "a", "s", "d", "f", "g", "h", "j", "k", "l", LV_SYMBOL_NEW_LINE, "\n",
    "_", "-", "y", "x", "c", "v", "b", "n", "m", ".", ",", ":", "\n",
    "ä", "ö", "ü", "ß", "à", "é", "è", "ñ", "ç", "-", ",", ".", "\n",
    NV_KB_CTRL_ROW
};
static const lv_buttonmatrix_ctrl_t qwertz_lc_c[] = {
    NV_KB_LETTER_ROWS_CTRL, NV_KB_ACCENT_ROW_CTRL, NV_KB_CTRL_ROW_CTRL };

static const char *const qwertz_uc[] = {
    "1#", "Q", "W", "E", "R", "T", "Z", "U", "I", "O", "P", LV_SYMBOL_BACKSPACE, "\n",
    "abc", "A", "S", "D", "F", "G", "H", "J", "K", "L", LV_SYMBOL_NEW_LINE, "\n",
    "_", "-", "Y", "X", "C", "V", "B", "N", "M", ".", ",", ":", "\n",
    "Ä", "Ö", "Ü", "ß", "À", "É", "È", "Ñ", "Ç", "-", ",", ".", "\n",
    NV_KB_CTRL_ROW
};
static const lv_buttonmatrix_ctrl_t qwertz_uc_c[] = {
    NV_KB_LETTER_ROWS_CTRL, NV_KB_ACCENT_ROW_CTRL, NV_KB_CTRL_ROW_CTRL };

/* -------- AZERTY (FR) — a<->q, z<->w swapped; m at end of 3rd letter run -------- */
static const char *const azerty_lc[] = {
    "1#", "a", "z", "e", "r", "t", "y", "u", "i", "o", "p", LV_SYMBOL_BACKSPACE, "\n",
    "ABC", "q", "s", "d", "f", "g", "h", "j", "k", "l", LV_SYMBOL_NEW_LINE, "\n",
    "_", "-", "w", "x", "c", "v", "b", "n", "m", ".", ",", ":", "\n",
    "é", "è", "à", "ç", "ù", "ê", "â", "î", "ô", "û", "ë", "ï", "\n",
    NV_KB_CTRL_ROW
};
static const lv_buttonmatrix_ctrl_t azerty_lc_c[] = {
    NV_KB_LETTER_ROWS_CTRL, NV_KB_ACCENT_ROW_CTRL, NV_KB_CTRL_ROW_CTRL };

static const char *const azerty_uc[] = {
    "1#", "A", "Z", "E", "R", "T", "Y", "U", "I", "O", "P", LV_SYMBOL_BACKSPACE, "\n",
    "abc", "Q", "S", "D", "F", "G", "H", "J", "K", "L", LV_SYMBOL_NEW_LINE, "\n",
    "_", "-", "W", "X", "C", "V", "B", "N", "M", ".", ",", ":", "\n",
    "É", "È", "À", "Ç", "Ù", "Ê", "Â", "Î", "Ô", "Û", "Ë", "Ï", "\n",
    NV_KB_CTRL_ROW
};
static const lv_buttonmatrix_ctrl_t azerty_uc_c[] = {
    NV_KB_LETTER_ROWS_CTRL, NV_KB_ACCENT_ROW_CTRL, NV_KB_CTRL_ROW_CTRL };

/* Compile-time guard: ctrl length == map buttons (map tokens - 4 "\n" - 1 terminating ""). */
#define NV_KB_MAP_BTNS(m) (sizeof(m) / sizeof((m)[0]) - 5)
#define NV_KB_CTRL_LEN(c) (sizeof(c) / sizeof((c)[0]))
_Static_assert(NV_KB_MAP_BTNS(qwerty_lc) == NV_KB_CTRL_LEN(qwerty_lc_c), "qwerty lc");
_Static_assert(NV_KB_MAP_BTNS(qwerty_uc) == NV_KB_CTRL_LEN(qwerty_uc_c), "qwerty uc");
_Static_assert(NV_KB_MAP_BTNS(qwertz_lc) == NV_KB_CTRL_LEN(qwertz_lc_c), "qwertz lc");
_Static_assert(NV_KB_MAP_BTNS(qwertz_uc) == NV_KB_CTRL_LEN(qwertz_uc_c), "qwertz uc");
_Static_assert(NV_KB_MAP_BTNS(azerty_lc) == NV_KB_CTRL_LEN(azerty_lc_c), "azerty lc");
_Static_assert(NV_KB_MAP_BTNS(azerty_uc) == NV_KB_CTRL_LEN(azerty_uc_c), "azerty uc");
_Static_assert(NV_KB_MAP_BTNS(it_lc) == NV_KB_CTRL_LEN(qwerty_lc_c), "it lc");
_Static_assert(NV_KB_MAP_BTNS(it_uc) == NV_KB_CTRL_LEN(qwerty_uc_c), "it uc");

void nv_kb_layout_get(int lang, nv_kb_layout_t *out) {
    switch (lang) {
        case NV_KB_LANG_DE:  /* QWERTZ */
            out->lc_map = qwertz_lc; out->lc_ctrl = qwertz_lc_c;
            out->uc_map = qwertz_uc; out->uc_ctrl = qwertz_uc_c;
            break;
        case NV_KB_LANG_FR:  /* AZERTY */
            out->lc_map = azerty_lc; out->lc_ctrl = azerty_lc_c;
            out->uc_map = azerty_uc; out->uc_ctrl = azerty_uc_c;
            break;
        case NV_KB_LANG_IT:  /* QWERTY tuned for Italian */
            out->lc_map = it_lc; out->lc_ctrl = qwerty_lc_c;
            out->uc_map = it_uc; out->uc_ctrl = qwerty_uc_c;
            break;
        default:             /* QWERTY: EN / ES (ES keeps ñ + acute accents) */
            out->lc_map = qwerty_lc; out->lc_ctrl = qwerty_lc_c;
            out->uc_map = qwerty_uc; out->uc_ctrl = qwerty_uc_c;
            break;
    }
}
