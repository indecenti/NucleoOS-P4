// template_app — a minimal, complete native NucleoOS app to copy from. NOT part of the build.
//
// It shows the pieces every native app uses (see docs/APP_SDK.md):
//   - an NvApp descriptor written with designated initializers
//   - build(content) that reads the theme tokens and uses nv_kit_* widgets
//   - a list of clickable rows the keyboard walks (Tab / arrows / Enter) with no extra code
//   - an app key handler (Ctrl+N new item, Delete removes the focused item)
//   - the app's rows in the F1 / Ctrl+/ shortcuts sheet (nv_ui_set_shortcuts)
//   - a sub-page with an in-app Back (nv_ui_set_back_handler): Esc / Back returns to the list
//   - suspended-task state (nv_ui_state_save / nv_ui_state_load) and a system toast (nv_toast)
//   - teardown on the root's LV_EVENT_DELETE (pointers cleared, pending lv_async_calls cancelled)
//
// To turn it into a real app (3 edits):
//   1. Copy this file to components/nv_apps/<name>_app.cpp, rename the "tpl" identifiers, the id
//      ("template" is persisted state: pick the final id now) and template_app_register().
//   2. components/nv_apps/apps_internal.h: declare `void <name>_app_register(void);`
//      components/nv_apps/nv_apps.cpp:   call it in nv_apps_register_all() (call order = launcher
//      order; the user's own Home arrangement is persisted by app id).
//   3. components/nv_apps/CMakeLists.txt: add "<name>_app.cpp" to SRCS (add any new component it
//      uses to REQUIRES).
// Then: give it an icon (nv_icons.h), a translated name (an NV_STR_APP_* id in nv_i18n, all five
// locales) and replace the English literals below with nv_tr(NV_STR_*) strings.
#include "apps_internal.h"

#include "nv_app.h"
#include "nv_fonts.h"
#include "nv_i18n.h"
#include "nv_icons.h"
#include "nv_notify.h"     // nv_toast: the system toast
#include "nv_theme.h"
#include "nv_ui.h"         // nv_ui_set_back_handler, nv_ui_state_save/load
#include "nv_ui_focus.h"   // nv_ui_set_key_handler, nv_ui_set_shortcuts, nv_focus_*
#include "nv_ui_host.h"    // nv_ui_set_title
#include "nv_ui_kit.h"     // nv_kit_* widgets + NV_SP_* / NV_RAD_* spacing

#include "lvgl.h"
#include <cstdint>

namespace {

constexpr const char *kName = "Template";
constexpr int kMaxItems = 32;

// Everything a suspended task needs to come back where it was. Plain data, well under the 1 KB
// nv_ui_state_save limit. Small enough for internal .bss; bulky per-open state goes to PSRAM
// (heap_caps_malloc(MALLOC_CAP_SPIRAM) in build, free on LV_EVENT_DELETE).
struct TplState {
    uint16_t n;                 // items in the list
    uint16_t next_id;           // id given to the next new item
    int16_t open;               // item shown on the detail page, -1 = the list
    uint16_t ids[kMaxItems];
};
TplState s_st;

lv_obj_t *s_root = nullptr;     // nv_kit_scroll_column: the page lives in here
lv_obj_t *s_list = nullptr;     // the rows (list page only)
int s_pending = -1;             // row index for the deferred open / delete

const nv_shortcut_t kKeys[] = {  // key names as the system sheet writes them (nv_ui_shortcuts.cpp)
    {"Ctrl+N", "Nuovo elemento", "New item"},
    {"Canc", "Elimina l'elemento selezionato", "Delete the focused item"},
    {"Invio", "Apri l'elemento", "Open the item"},
    {"Esc", "Torna all'elenco", "Back to the list"},
};

void show_list(void);
void show_detail(int i);

void reset_state(void) {
    s_st = {};
    s_st.open = -1;
    for (int i = 0; i < 3; i++) s_st.ids[s_st.n++] = ++s_st.next_id;
}

void add_item(void) {
    if (s_st.n >= kMaxItems) { nv_toast(NV_NOTE_WARN, "The list is full"); return; }
    s_st.ids[s_st.n++] = ++s_st.next_id;
    s_st.open = -1;
    show_list();
    nv_toast(NV_NOTE_OK, "Item added");
}

void delete_item(int i) {
    if (i < 0 || i >= s_st.n) return;
    for (int k = i; k < s_st.n - 1; k++) s_st.ids[k] = s_st.ids[k + 1];
    s_st.n--;
    s_st.open = -1;
    show_list();
}

// Opening / deleting rebuilds the page, which deletes the row whose event (or focus) started it:
// never do that inside the row's own handler, defer it to the next LVGL loop.
void open_async(void *) { show_detail(s_pending); }
void delete_async(void *) { delete_item(s_pending); }

void row_click_cb(lv_event_t *e) {
    s_pending = lv_obj_get_index(lv_event_get_target_obj(e));
    lv_async_call(open_async, nullptr);
}
void new_click_cb(lv_event_t *) { add_item(); }
void del_click_cb(lv_event_t *) {
    s_pending = s_st.open;
    lv_async_call(delete_async, nullptr);
}

// In-app Back (header arrow, Back gesture, Esc): detail page -> list. On the list page the handler
// is NULL, so Back closes the app as usual.
void tpl_back(void) { s_st.open = -1; show_list(); }

// Physical keyboard. Runs while the app is in front and no text field is focused, before
// navigation; return false for everything you do not handle (Tab, arrows, Enter, Esc stay with the
// system). Called on auto-repeat too.
bool tpl_key(uint32_t key, uint8_t usage, uint8_t mods) {
    const bool ctrl = mods & 0x11;                         // left / right Ctrl (HID modifier byte)
    if (ctrl && usage == 0x11) { add_item(); return true; }   // Ctrl+N (HID usage of N)
    if (key == LV_KEY_DEL) {
        if (s_st.open >= 0) { s_pending = s_st.open; lv_async_call(delete_async, nullptr); return true; }
        lv_obj_t *f = nv_focus_current();                  // the row with the focus ring, if any
        if (!f || !s_list || lv_obj_get_parent(f) != s_list) return false;
        s_pending = lv_obj_get_index(f);
        lv_async_call(delete_async, nullptr);
        return true;
    }
    return false;
}

void show_list(void) {
    if (!s_root) return;
    lv_obj_clean(s_root);
    nv_ui_set_back_handler(nullptr);
    nv_ui_set_title(kName);
    const NvTheme *th = nv_theme_get();

    // Toolbar: item count + primary "New" action.
    lv_obj_t *bar = lv_obj_create(s_root);
    lv_obj_remove_style_all(bar);
    lv_obj_set_size(bar, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(bar, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *count = lv_label_create(bar);
    lv_label_set_text_fmt(count, "%d items", s_st.n);
    lv_obj_set_style_text_color(count, th->text_dim, 0);
    lv_obj_t *add = nv_kit_button(bar, LV_SYMBOL_PLUS " New", true);
    lv_obj_add_event_cb(add, new_click_cb, LV_EVENT_CLICKED, nullptr);

    // The rows: clickable buttons with their own CLICKED handler, created in reading order, so the
    // focus engine walks them with Tab / arrows and Enter opens one. Nothing to register.
    s_list = lv_obj_create(s_root);
    lv_obj_remove_style_all(s_list);
    lv_obj_set_size(s_list, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(s_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s_list, NV_SP_2, 0);
    lv_obj_clear_flag(s_list, LV_OBJ_FLAG_SCROLLABLE);
    for (int i = 0; i < s_st.n; i++) {
        char txt[32];
        lv_snprintf(txt, sizeof txt, LV_SYMBOL_FILE "  Item %u", (unsigned)s_st.ids[i]);
        lv_obj_t *row = nv_kit_button(s_list, txt, false);
        lv_obj_set_width(row, lv_pct(100));
        lv_obj_add_event_cb(row, row_click_cb, LV_EVENT_CLICKED, nullptr);
        if (i == 0) nv_focus_prefer(row);                  // first focus when the page opens
    }
    if (s_st.n == 0) {
        lv_obj_t *empty = nv_kit_info(s_root);
        lv_label_set_text(empty, "No items. Press Ctrl+N or tap New.");
        nv_focus_prefer(add);
    }
}

void show_detail(int i) {
    if (!s_root || i < 0 || i >= s_st.n) { show_list(); return; }
    s_st.open = (int16_t)i;
    s_list = nullptr;
    lv_obj_clean(s_root);
    nv_ui_set_back_handler(tpl_back);                      // Back / Esc: to the list, not out
    const NvTheme *th = nv_theme_get();

    char title[32];
    lv_snprintf(title, sizeof title, "Item %u", (unsigned)s_st.ids[i]);
    nv_ui_set_title(title);

    lv_obj_t *h = lv_label_create(s_root);
    lv_label_set_text(h, title);
    lv_obj_set_style_text_font(h, &nv_font_20, 0);
    lv_obj_set_style_text_color(h, th->text_strong, 0);

    lv_obj_t *info = nv_kit_info(s_root);
    lv_label_set_text(info, "A sub-page: Esc or Back returns to the list.");

    lv_obj_t *del = nv_kit_button(s_root, nv_tr(NV_STR_DELETE), false);
    lv_obj_set_style_text_color(lv_obj_get_child(del, 0), th->danger, 0);
    lv_obj_add_event_cb(del, del_click_cb, LV_EVENT_CLICKED, nullptr);
}

// Teardown: the root goes away when the app closes, is suspended (another app opened) or is
// rebuilt in place (theme / language change, a new file intent). Save the task state, drop the
// pointers, cancel deferred work that would touch the deleted widgets.
void root_deleted(lv_event_t *) {
    nv_ui_state_save(&s_st, sizeof s_st);
    lv_async_call_cancel(open_async, nullptr);
    lv_async_call_cancel(delete_async, nullptr);
    s_root = s_list = nullptr;
}

void tpl_build(lv_obj_t *content) {
    // Per-open hooks: the shell clears all of them on close AND before every rebuild.
    nv_ui_set_key_handler(tpl_key);
    nv_ui_set_shortcuts(kKeys, (int)(sizeof kKeys / sizeof kKeys[0]));

    TplState st;
    if (nv_ui_state_load(&st, sizeof st) == sizeof st) s_st = st;   // resumed task / rebuild
    else reset_state();                                             // fresh open

    // Colours come from the live theme at build time; a theme change re-runs build().
    const NvTheme *th = nv_theme_get();
    lv_obj_set_style_bg_color(content, th->bg, 0);
    lv_obj_set_style_bg_opa(content, LV_OPA_COVER, 0);

    s_root = nv_kit_scroll_column(content);
    lv_obj_add_event_cb(s_root, root_deleted, LV_EVENT_DELETE, nullptr);

    if (s_st.open >= 0) show_detail(s_st.open);
    else show_list();
}

const NvApp kTplApp = {
    .id = "template",             // stable: persisted (Home layout, task state, defaults)
    .name = kName,                // English label, used when name_id < 0
    .icon = &nv_icon_apps,        // launcher icon (nv_icons.h)
    .ram_budget = 256u << 10,     // the Memory Broker frees this much before launch
    .build = tpl_build,
    .name_id = -1,                // NV_STR_APP_<NAME> once translated; NOT 0 (= "Settings")
    .user = nullptr,
    .flags = 0,                   // NV_APP_FLAG_GAME for Start > Games
};

}  // namespace

void template_app_register(void) { nv_app_register(&kTplApp); }
