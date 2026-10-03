// nv_ui_shortcuts — the keyboard shortcuts sheet (F1 / Ctrl+/): every system shortcut, plus the ones
// the app in front declared with nv_ui_set_shortcuts(). One table here is the reference the sheet
// shows; ui_kbd_nav (nv_ui.cpp) implements the same keys.
#include "nv_ui_focus.h"
#include "nv_ui.h"
#include "nv_theme.h"
#include "nv_fonts.h"
#include "nv_i18n.h"
#include "nv_app.h"

#include "lvgl.h"
#include <cstring>
#include <initializer_list>

namespace {

struct Row { const char *keys, *it, *en; };
struct Section { const char *it, *en; const Row *rows; int n; };

const Row kSystem[] = {
    {"Win", "Start (scrivi per cercare)", "Start (type to search)"},
    {"Win+E / Win+I", "File / Impostazioni", "Files / Settings"},
    {"Win+A", "Pannello rapido e notifiche", "Quick settings and notifications"},
    {"Win+V", "Cronologia appunti", "Clipboard history"},
    {"Win+L", "Blocca", "Lock"},
    {"Win+D", "Mostra il desktop", "Show the desktop"},
    {"Win+1..9", "Passa all'app n-esima della barra", "Switch to the n-th taskbar app"},
    {"F1 / Ctrl+/", "Questa guida", "This sheet"},
};
const Row kWindows[] = {
    {"Alt+Tab / Win+Tab", "App recenti", "Recent apps"},
    {"Ctrl+W / Alt+F4", "Chiudi l'app", "Close the app"},
    {"Ctrl+Alt+Canc", "Monitor di sistema", "System monitor"},
    {"Tab / Maiusc+Tab", "Controllo successivo / precedente", "Next / previous control"},
    {"Frecce, Invio, Spazio", "Muoviti e attiva", "Move and activate"},
    {"Menu / Maiusc+F10", "Menu contestuale", "Context menu"},
    {"Esc", "Indietro / chiudi il pannello", "Back / close the panel"},
};
const Row kText[] = {
    {"Ctrl+C / X / V", "Copia / taglia / incolla", "Copy / cut / paste"},
    {"Ctrl+A", "Seleziona tutto", "Select all"},
    {"Ctrl+Z / Ctrl+Y", "Annulla / ripeti", "Undo / redo"},
    {"Ctrl+Backspace", "Cancella la parola", "Delete the word"},
    {"Ctrl+Frecce", "Salta di parola", "Jump by word"},
};
const Row kCapture[] = {
    {"Stamp / Win+Maiusc+S", "Screenshot di un'area", "Screenshot of an area"},
    {"Alt+Stamp", "Schermo intero negli appunti", "Whole screen to the clipboard"},
    {"Tasti multimediali", "Volume, muto, play/pausa", "Volume, mute, play/pause"},
};

const nv_shortcut_t *s_app_rows;
int s_app_n;
lv_obj_t *s_sheet;

bool it() { return nv_i18n_get_lang() == NV_LANG_IT; }

// Key names are written in Italian in the tables; other languages get the international names.
const char *keys_text(const char *k) {
    if (it()) return k;
    static char out[64];
    static const char *const map[][2] = {
        {"Maiusc", "Shift"}, {"Canc", "Del"}, {"Invio", "Enter"}, {"Spazio", "Space"},
        {"Frecce", "Arrows"}, {"Stamp", "PrtSc"}, {"Tasti multimediali", "Media keys"},
    };
    size_t o = 0;
    for (const char *p = k; *p && o < sizeof out - 1;) {
        bool hit = false;
        for (auto &m : map) {
            const size_t n = strlen(m[0]);
            if (!strncmp(p, m[0], n)) {
                for (const char *q = m[1]; *q && o < sizeof out - 1; q++) out[o++] = *q;
                p += n; hit = true; break;
            }
        }
        if (!hit) out[o++] = *p++;
    }
    out[o] = 0;
    return out;
}

void close_sheet() {
    if (s_sheet) { lv_obj_delete(s_sheet); s_sheet = nullptr; }
}

lv_obj_t *keycap_row(lv_obj_t *parent, const char *keys, const char *what) {
    const NvTheme *t = nv_theme_get();
    lv_obj_t *r = lv_obj_create(parent);
    lv_obj_remove_style_all(r);
    lv_obj_set_size(r, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(r, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(r, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_ver(r, 3, 0);
    lv_obj_t *k = lv_label_create(r);                // the keys, as a key cap
    lv_label_set_text(k, keys_text(keys));
    lv_obj_set_style_text_font(k, &nv_font_14, 0);
    lv_obj_set_style_text_color(k, t->text_strong, 0);
    lv_obj_set_style_bg_color(k, t->surface3, 0);
    lv_obj_set_style_bg_opa(k, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(k, 4, 0);
    lv_obj_set_style_pad_hor(k, 6, 0);
    lv_obj_set_style_pad_ver(k, 2, 0);
    lv_obj_set_style_min_width(k, 150, 0);
    lv_obj_t *w = lv_label_create(r);
    lv_label_set_text(w, what);
    lv_obj_set_style_text_font(w, &nv_font_14, 0);
    lv_obj_set_style_text_color(w, t->text, 0);
    lv_obj_set_style_pad_left(w, 10, 0);
    lv_obj_set_flex_grow(w, 1);
    lv_label_set_long_mode(w, LV_LABEL_LONG_MODE_WRAP);
    return r;
}

lv_obj_t *section(lv_obj_t *col, const char *title) {
    const NvTheme *t = nv_theme_get();
    lv_obj_t *box = lv_obj_create(col);
    lv_obj_remove_style_all(box);
    lv_obj_set_size(box, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_bottom(box, 10, 0);
    lv_obj_t *h = lv_label_create(box);
    lv_label_set_text(h, title);
    lv_obj_set_style_text_color(h, t->accent, 0);
    lv_obj_set_style_pad_bottom(h, 4, 0);
    return box;
}

void fill(lv_obj_t *col, const Section &s) {
    lv_obj_t *b = section(col, it() ? s.it : s.en);
    for (int i = 0; i < s.n; i++) keycap_row(b, s.rows[i].keys, it() ? s.rows[i].it : s.rows[i].en);
}

}  // namespace

void nv_ui_set_shortcuts(const nv_shortcut_t *list, int n) {
    s_app_rows = list;
    s_app_n = list ? n : 0;
}

bool nv_ui_shortcuts_sheet_open(void) { return s_sheet != nullptr; }
void nv_ui_shortcuts_sheet_close(void) { close_sheet(); }

void nv_ui_shortcuts_sheet(void) {
    if (s_sheet) { close_sheet(); return; }
    const NvTheme *t = nv_theme_get();
    const int32_t sw = lv_display_get_horizontal_resolution(nullptr), sh = lv_display_get_vertical_resolution(nullptr);
    s_sheet = lv_obj_create(lv_layer_top());          // a scrim: tap outside or Esc closes
    lv_obj_remove_style_all(s_sheet);
    lv_obj_set_size(s_sheet, sw, sh);
    lv_obj_set_style_bg_color(s_sheet, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_sheet, LV_OPA_50, 0);
    lv_obj_add_flag(s_sheet, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_sheet, [](lv_event_t *e) {
        if (lv_event_get_target(e) == lv_event_get_current_target(e)) lv_async_call([](void *) { close_sheet(); }, nullptr);
    }, LV_EVENT_CLICKED, nullptr);

    lv_obj_t *card = lv_obj_create(s_sheet);
    lv_obj_remove_style_all(card);
    lv_obj_set_size(card, LV_MIN(sw - 40, 900), LV_MIN(sh - 40, 540));
    lv_obj_center(card);
    lv_obj_set_style_bg_color(card, t->surface, 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(card, 12, 0);
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_set_style_border_color(card, t->divider, 0);
    lv_obj_set_style_pad_all(card, 18, 0);
    lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);     // taps inside never reach the scrim
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);

    lv_obj_t *title = lv_label_create(card);
    lv_label_set_text(title, it() ? LV_SYMBOL_KEYBOARD "  Scorciatoie da tastiera" : LV_SYMBOL_KEYBOARD "  Keyboard shortcuts");
    lv_obj_set_style_text_font(title, &nv_font_20, 0);
    lv_obj_set_style_text_color(title, t->text_strong, 0);
    lv_obj_set_style_pad_bottom(title, 12, 0);

    // Two columns (one on a narrow / portrait screen), scrolling together.
    lv_obj_t *cols = lv_obj_create(card);
    lv_obj_remove_style_all(cols);
    lv_obj_set_width(cols, lv_pct(100));
    lv_obj_set_flex_grow(cols, 1);
    lv_obj_set_flex_flow(cols, sw > sh ? LV_FLEX_FLOW_ROW : LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_column(cols, 24, 0);
    lv_obj_set_scroll_dir(cols, LV_DIR_VER);
    lv_obj_t *c1 = lv_obj_create(cols), *c2 = sw > sh ? lv_obj_create(cols) : c1;
    for (lv_obj_t *c : {c1, c2}) {
        lv_obj_remove_style_all(c);
        lv_obj_set_height(c, LV_SIZE_CONTENT);
        lv_obj_set_flex_grow(c, 1);
        lv_obj_set_width(c, sw > sh ? 1 : lv_pct(100));
        lv_obj_set_flex_flow(c, LV_FLEX_FLOW_COLUMN);
    }
    if (s_app_n > 0) {                               // the app in front first: what the user is doing
        const NvApp *a = nv_ui_current_app();
        lv_obj_t *b = section(c1, a ? a->name : (it() ? "Questa app" : "This app"));
        for (int i = 0; i < s_app_n; i++)
            keycap_row(b, s_app_rows[i].keys, it() ? s_app_rows[i].it : s_app_rows[i].en);
    }
    fill(c1, {"Sistema", "System", kSystem, (int)(sizeof kSystem / sizeof kSystem[0])});
    fill(c1, {"Screenshot e media", "Screenshots and media", kCapture, (int)(sizeof kCapture / sizeof kCapture[0])});
    fill(c2, {"Finestre e navigazione", "Windows and navigation", kWindows, (int)(sizeof kWindows / sizeof kWindows[0])});
    fill(c2, {"Testo", "Text", kText, (int)(sizeof kText / sizeof kText[0])});
}
