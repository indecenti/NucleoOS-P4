// tasks_app — a simple persistent checklist backed by /sdcard/tasks.txt.
// One task per line: first char '1' (done) / '0' (pending), the rest is the text. Add via the
// one-line input (auto-bound to the SystemUI IME), tap a row's circle to toggle done, trash to
// delete. Every change rewrites the whole file — the list is small, so simplicity wins over
// incremental writes. All labels via nv_tr().
#include "apps_internal.h"

#include "nv_app.h"
#include "nv_ui_kit.h"   // nv_kit_* + (transitively) nv_ime_hide
#include "nv_icons.h"
#include "nv_i18n.h"
#include "nv_theme.h"

#include "lvgl.h"
#include <cstdio>   // fopen/fgets/fprintf/fclose
#include <cstring>  // strncpy/strlen
#include <cstdint>  // intptr_t

namespace {

constexpr char kTasksPath[] = "/sdcard/tasks.txt";
constexpr int  kMaxTasks = 64;
constexpr int  kTaskLen  = 96;

struct Task {
    char text[kTaskLen];
    bool done;
};
Task s_tasks[kMaxTasks];
int  s_ntasks = 0;
lv_obj_t *s_list  = nullptr;   // scroll column of task rows
lv_obj_t *s_input = nullptr;   // one-line new-task entry

void tasks_load(void) {
    s_ntasks = 0;
    FILE *f = fopen(kTasksPath, "rb");
    if (!f) return;
    char line[kTaskLen + 8];
    while (s_ntasks < kMaxTasks && fgets(line, sizeof line, f)) {
        size_t n = strlen(line);
        while (n && (line[n - 1] == '\n' || line[n - 1] == '\r')) line[--n] = '\0';
        if (n < 1) continue;
        Task &t = s_tasks[s_ntasks];
        const bool flagged = (line[0] == '0' || line[0] == '1');
        t.done = (line[0] == '1');
        const char *body = flagged ? line + 1 : line;
        strncpy(t.text, body, kTaskLen - 1);
        t.text[kTaskLen - 1] = '\0';
        if (t.text[0]) s_ntasks++;
    }
    fclose(f);
}

void tasks_save(void) {
    FILE *f = fopen(kTasksPath, "wb");
    if (!f) return;
    for (int i = 0; i < s_ntasks; i++)
        fprintf(f, "%c%s\n", s_tasks[i].done ? '1' : '0', s_tasks[i].text);
    fclose(f);
}

void rebuild_list(void);   // fwd

void toggle_cb(lv_event_t *e) {
    const int i = (int)(intptr_t)lv_event_get_user_data(e);
    if (i < 0 || i >= s_ntasks) return;
    s_tasks[i].done = !s_tasks[i].done;
    tasks_save();
    rebuild_list();
}

void delete_cb(lv_event_t *e) {
    const int i = (int)(intptr_t)lv_event_get_user_data(e);
    if (i < 0 || i >= s_ntasks) return;
    for (int j = i; j < s_ntasks - 1; j++) s_tasks[j] = s_tasks[j + 1];
    s_ntasks--;
    tasks_save();
    rebuild_list();
}

void add_cb(lv_event_t *) {
    if (!s_input || s_ntasks >= kMaxTasks) return;
    const char *txt = lv_textarea_get_text(s_input);
    if (!txt || !txt[0]) return;
    Task &t = s_tasks[s_ntasks];
    strncpy(t.text, txt, kTaskLen - 1);
    t.text[kTaskLen - 1] = '\0';
    t.done = false;
    s_ntasks++;
    lv_textarea_set_text(s_input, "");
    nv_ime_hide();
    tasks_save();
    rebuild_list();
}

void rebuild_list(void) {
    if (!s_list) return;
    lv_obj_clean(s_list);   // wipe the current rows; cheap for a short list
    const NvTheme *th = nv_theme_get();

    if (s_ntasks == 0) {
        lv_obj_t *empty = lv_label_create(s_list);
        lv_label_set_text(empty, nv_tr(NV_STR_NO_TASKS));
        lv_obj_set_style_text_color(empty, th->text_dim, 0);
        return;
    }

    for (int i = 0; i < s_ntasks; i++) {
        lv_obj_t *row = lv_obj_create(s_list);
        lv_obj_remove_style_all(row);
        lv_obj_set_size(row, lv_pct(100), LV_SIZE_CONTENT);
        lv_obj_set_style_bg_color(row, th->surface, 0);
        lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(row, 12, 0);
        lv_obj_set_style_pad_all(row, 10, 0);
        lv_obj_set_style_pad_column(row, 10, 0);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

        // Circular check toggle (filled + tick when done).
        lv_obj_t *chk = lv_button_create(row);
        lv_obj_set_size(chk, 40, 40);
        lv_obj_set_style_radius(chk, 20, 0);
        lv_obj_set_style_shadow_width(chk, 0, 0);
        lv_obj_set_style_bg_color(chk, s_tasks[i].done ? th->success_solid : th->surface3, 0);
        lv_obj_add_event_cb(chk, toggle_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        lv_obj_t *cl = lv_label_create(chk);
        lv_label_set_text(cl, s_tasks[i].done ? LV_SYMBOL_OK : "");
        lv_obj_set_style_text_color(cl, th->on_primary, 0);
        lv_obj_center(cl);

        // Task text (strikethrough + dimmed when done).
        lv_obj_t *lbl = lv_label_create(row);
        lv_label_set_text(lbl, s_tasks[i].text);
        lv_obj_set_flex_grow(lbl, 1);
        lv_label_set_long_mode(lbl, LV_LABEL_LONG_DOT);
        lv_obj_set_style_text_color(lbl, s_tasks[i].done ? th->text_dim : th->text_strong, 0);
        if (s_tasks[i].done)
            lv_obj_set_style_text_decor(lbl, LV_TEXT_DECOR_STRIKETHROUGH, 0);

        // Delete (trash).
        lv_obj_t *del = lv_button_create(row);
        lv_obj_set_size(del, 40, 40);
        lv_obj_set_style_radius(del, 20, 0);
        lv_obj_set_style_shadow_width(del, 0, 0);
        lv_obj_set_style_bg_opa(del, LV_OPA_TRANSP, 0);
        lv_obj_set_style_bg_opa(del, LV_OPA_20, LV_STATE_PRESSED);
        lv_obj_add_event_cb(del, delete_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        lv_obj_t *dl = lv_label_create(del);
        lv_label_set_text(dl, LV_SYMBOL_TRASH);
        lv_obj_set_style_text_color(dl, th->danger, 0);
        lv_obj_center(dl);
    }
}

void page_deleted(lv_event_t *) {
    nv_ime_hide();   // the bound input is about to be freed — drop any raised keyboard
    s_list = nullptr;
    s_input = nullptr;
}

void tasks_build(lv_obj_t *content) {
    tasks_load();

    lv_obj_t *root = lv_obj_create(content);
    lv_obj_remove_style_all(root);
    lv_obj_set_size(root, lv_pct(100), lv_pct(100));
    lv_obj_set_style_pad_all(root, 14, 0);
    lv_obj_set_style_pad_row(root, 10, 0);
    lv_obj_set_flex_flow(root, LV_FLEX_FLOW_COLUMN);
    lv_obj_clear_flag(root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(root, page_deleted, LV_EVENT_DELETE, nullptr);

    // Add bar: one-line input (IME-bound) + Add button.
    lv_obj_t *bar = lv_obj_create(root);
    lv_obj_remove_style_all(bar);
    lv_obj_set_size(bar, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(bar, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(bar, 10, 0);
    lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);

    s_input = nv_kit_textarea(bar, nv_tr(NV_STR_TASK_NEW), true);
    lv_obj_set_flex_grow(s_input, 1);

    char ab[24];
    lv_snprintf(ab, sizeof ab, LV_SYMBOL_PLUS "  %s", nv_tr(NV_STR_TASK_ADD));
    lv_obj_t *add = nv_kit_button(bar, ab, true);
    lv_obj_add_event_cb(add, add_cb, LV_EVENT_CLICKED, nullptr);

    // The list fills the rest of the height.
    s_list = nv_kit_scroll_column(root);
    lv_obj_set_flex_grow(s_list, 1);
    rebuild_list();
}

const NvApp kTasksApp = {"tasks", "Tasks", &nv_icon_tasks, 512u << 10, tasks_build,
                         NV_STR_APP_TASKS, nullptr};

}  // namespace

void tasks_app_register(void) { nv_app_register(&kTasksApp); }
