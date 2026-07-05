// notes_app — a searchable list of notes with an autosaving title+body editor, backed by
// individual files under /sdcard/notes/.
//   List   : title + one-line preview + last-modified time per row, search box, + to add one.
//   Detail : title field + body editor; autosaves ~1.2s after the last keystroke so there is no
//            Save button. Two-step delete (tap-again, files_app convention). If the last autosave
//            failed (no SD card) Back reveals an inline Retry/Discard bar instead of just leaving.
// Page switches are deferred via lv_async_call (files_app pattern): the callback that reacts to
// an event must not tear down its own subtree synchronously.
// Note file format: line 1 = epoch (decimal, last-modified), line 2 = title, rest = body.
#include "apps_internal.h"

#include "nv_app.h"
#include "nv_ui_host.h"
#include "nv_ui_kit.h"
#include "nv_icons.h"
#include "nv_i18n.h"
#include "nv_fonts.h"
#include "nv_theme.h"
#include "nv_notify.h"
#include "nv_sd.h"
#include "nv_time.h"

#include "lvgl.h"
#include "esp_heap_caps.h"

#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cstdint>
#include <cctype>
#include <ctime>

namespace {

constexpr char kNotesDir[]     = "/sdcard/notes";
constexpr char kLegacyPath[]   = "/sdcard/notes.txt";   // old single-note app's file, pre-rewrite
constexpr int  kMaxNotes       = 120;
constexpr int  kTitleLen       = 64;
constexpr int  kPreviewLen     = 64;
constexpr int  kFnameLen       = 24;
constexpr long kMaxNoteBody    = 64 * 1024;   // cap: never over-allocate on a huge/garbage file
constexpr uint32_t kAutosaveMs = 1200;        // debounce delay after the last keystroke

struct NoteMeta {
    char title[kTitleLen];
    char preview[kPreviewLen];   // first line of the body, single line, control chars stripped
    long epoch;
    char fname[kFnameLen];
};

NoteMeta *s_notes = nullptr;   // PSRAM index, allocated once
int  s_n = 0;
char s_query[48] = "";

enum class Page { List, Detail };
Page s_pending = Page::List;
bool s_nav_pending = false;

// Working copy for the editor. Not written to disk until the first non-empty autosave, so
// tapping + and immediately leaving never litters the card with an empty file.
struct CurNote {
    char fname[kFnameLen];
    long epoch;
    bool is_new;
    bool dirty;
    bool save_failed;
};
CurNote s_cur{};

lv_obj_t   *s_list        = nullptr;   // List page: scrollable row column
lv_obj_t   *s_title_ta    = nullptr;   // Detail page
lv_obj_t   *s_body_ta     = nullptr;
lv_obj_t   *s_status      = nullptr;
lv_obj_t   *s_del_btn_label = nullptr;
bool        s_del_armed   = false;
lv_obj_t   *s_retry_bar   = nullptr;   // hidden unless a save fails and Back needs a decision
lv_timer_t *s_autosave_timer = nullptr;

void build_list(void);
void build_detail(void);
void render_rows(void);

void nav_apply(void *) {
    s_nav_pending = false;
    switch (s_pending) {
        case Page::List:   build_list();   break;
        case Page::Detail: build_detail(); break;
    }
}
void nav_to(Page p) {
    s_pending = p;
    if (!s_nav_pending && lv_async_call(nav_apply, nullptr) == LV_RESULT_OK)
        s_nav_pending = true;
}

// ---------------------------------------------------------------- model

bool ci_contains(const char *hay, const char *needle) {
    if (!needle || !needle[0]) return true;
    if (!hay) return false;
    const size_t hn = strlen(hay), nn = strlen(needle);
    if (nn > hn) return false;
    for (size_t i = 0; i + nn <= hn; i++) {
        size_t j = 0;
        while (j < nn && tolower((unsigned char)hay[i + j]) == tolower((unsigned char)needle[j])) j++;
        if (j == nn) return true;
    }
    return false;
}

// Read the epoch + title header and a short body snippet -- enough to render a list row without
// loading the whole (possibly 64KB) note.
bool read_meta(const char *fname, NoteMeta *out) {
    char full[256];
    snprintf(full, sizeof full, "%s/%s", kNotesDir, fname);
    FILE *f = fopen(full, "rb");
    if (!f) return false;
    char line[128];
    out->epoch = fgets(line, sizeof line, f) ? atol(line) : 0;
    out->title[0] = '\0';
    if (fgets(line, sizeof line, f)) {
        size_t n = strlen(line);
        while (n && (line[n - 1] == '\n' || line[n - 1] == '\r')) line[--n] = '\0';
        lv_snprintf(out->title, sizeof out->title, "%s", line);   // lv_snprintf: no -Wformat-truncation
    }
    char body[128] = "";
    const size_t rd = fread(body, 1, sizeof body - 1, f);
    body[rd] = '\0';
    for (size_t i = 0; i < rd; i++) if (body[i] == '\n' || body[i] == '\r') body[i] = ' ';
    lv_snprintf(out->preview, sizeof out->preview, "%s", body);
    fclose(f);
    snprintf(out->fname, sizeof out->fname, "%s", fname);
    return true;
}

int note_cmp(const void *a, const void *b) {
    const NoteMeta *x = (const NoteMeta *)a, *y = (const NoteMeta *)b;
    return (y->epoch > x->epoch) - (y->epoch < x->epoch);   // newest first
}

void scan_notes(void) {
    s_n = 0;
    if (!s_notes || !nv_sd_is_mounted()) return;
    mkdir(kNotesDir, 0777);   // ok if it already exists
    DIR *d = opendir(kNotesDir);
    if (!d) return;
    struct dirent *e;
    while (s_n < kMaxNotes && (e = readdir(d)) != nullptr) {
        if (e->d_name[0] == '.') continue;
        if (read_meta(e->d_name, &s_notes[s_n])) s_n++;
    }
    closedir(d);
    qsort(s_notes, (size_t)s_n, sizeof(NoteMeta), note_cmp);
}

bool epoch_in_use(long e) {
    for (int i = 0; i < s_n; i++) if (s_notes[i].epoch == e) return true;
    return false;
}

void fmt_row_time(long epoch, char *out, size_t n) {
    const time_t t = (time_t)epoch;
    struct tm tmv{};
    localtime_r(&t, &tmv);
    struct tm now{};
    nv_time_now(&now);
    const bool today = tmv.tm_year == now.tm_year && tmv.tm_yday == now.tm_yday;
    strftime(out, n, today ? (nv_time_is_24h() ? "%H:%M" : "%I:%M %p") : "%d/%m", &tmv);
}

// ---------------------------------------------------------------- persistence

void set_status(const char *text, lv_color_t color) {
    if (!s_status) return;
    lv_label_set_text(s_status, text);
    lv_obj_set_style_text_color(s_status, color, 0);
}

// Writes the current title+body to s_cur.fname. A brand-new, still-empty note is not persisted
// at all (nothing to save yet, no orphan file). Returns false only on a real write failure.
bool save_note(void) {
    if (!s_title_ta || !s_body_ta) return true;
    const char *title = lv_textarea_get_text(s_title_ta);
    const char *body  = lv_textarea_get_text(s_body_ta);
    const bool empty  = (!title || !title[0]) && (!body || !body[0]);
    if (s_cur.is_new && empty) { s_cur.dirty = false; return true; }

    char full[256];
    snprintf(full, sizeof full, "%s/%s", kNotesDir, s_cur.fname);
    FILE *f = fopen(full, "wb");
    if (!f) {
        s_cur.save_failed = true;
        set_status(nv_tr(NV_STR_SAVE_FAILED), nv_theme_get()->danger);
        return false;
    }
    s_cur.epoch = (long)time(nullptr);
    fprintf(f, "%ld\n%s\n", s_cur.epoch, title ? title : "");
    if (body && body[0]) fwrite(body, 1, strlen(body), f);
    fclose(f);

    s_cur.is_new = false;
    s_cur.dirty = false;
    s_cur.save_failed = false;
    char tbuf[16];
    nv_time_format(tbuf, sizeof tbuf, nv_time_is_24h() ? "%H:%M" : "%I:%M %p");
    char msg[48];
    lv_snprintf(msg, sizeof msg, nv_tr(NV_STR_NOTES_SAVED_FMT), tbuf);
    set_status(msg, nv_theme_get()->text_dim);
    return true;
}

// Loads title into `title_out` and the body straight into s_body_ta.
bool load_note(const char *fname, char *title_out, size_t title_cap) {
    char full[256];
    snprintf(full, sizeof full, "%s/%s", kNotesDir, fname);
    FILE *f = fopen(full, "rb");
    if (!f) return false;
    char line[128];
    fgets(line, sizeof line, f);   // epoch line, unused here (kept as-is until the next save)
    title_out[0] = '\0';
    if (fgets(line, sizeof line, f)) {
        size_t n = strlen(line);
        while (n && (line[n - 1] == '\n' || line[n - 1] == '\r')) line[--n] = '\0';
        lv_snprintf(title_out, title_cap, "%s", line);   // lv_snprintf: no -Wformat-truncation
    }
    const long pos = ftell(f);
    fseek(f, 0, SEEK_END);
    long total = ftell(f) - pos;
    fseek(f, pos, SEEK_SET);
    if (total > kMaxNoteBody) total = kMaxNoteBody;
    if (total > 0 && s_body_ta) {
        char *buf = (char *)malloc((size_t)total + 1);
        if (buf) {
            const size_t rd = fread(buf, 1, (size_t)total, f);
            buf[rd] = '\0';
            lv_textarea_set_text(s_body_ta, buf);
            free(buf);
        }
    }
    fclose(f);
    return true;
}

// ---------------------------------------------------------------- autosave

void autosave_fire(lv_timer_t *) {
    s_autosave_timer = nullptr;
    if (s_cur.dirty) save_note();
}

void schedule_autosave(void) {
    s_cur.dirty = true;
    if (s_autosave_timer) lv_timer_delete(s_autosave_timer);
    s_autosave_timer = lv_timer_create(autosave_fire, kAutosaveMs, nullptr);
    lv_timer_set_repeat_count(s_autosave_timer, 1);
    set_status(nv_tr(NV_STR_NOTES_SAVING), nv_theme_get()->text_dim);
}

void field_changed_cb(lv_event_t *) { schedule_autosave(); }

// ---------------------------------------------------------------- Detail page actions

void del_cb(lv_event_t *) {
    if (!s_del_armed) {
        s_del_armed = true;
        if (s_del_btn_label) lv_label_set_text(s_del_btn_label, nv_tr(NV_STR_TAP_AGAIN));
        return;
    }
    if (s_autosave_timer) { lv_timer_delete(s_autosave_timer); s_autosave_timer = nullptr; }
    s_cur.dirty = false;
    if (!s_cur.is_new) {
        char full[256];
        snprintf(full, sizeof full, "%s/%s", kNotesDir, s_cur.fname);
        unlink(full);
    }
    nv_toast(NV_NOTE_OK, nv_tr(NV_STR_DELETE));
    nav_to(Page::List);
}

void retry_cb(lv_event_t *) { if (save_note()) nav_to(Page::List); }

void discard_cb(lv_event_t *) {
    s_cur.dirty = false;
    if (s_autosave_timer) { lv_timer_delete(s_autosave_timer); s_autosave_timer = nullptr; }
    nav_to(Page::List);
}

// Back from the editor: flush any pending edit first. If that succeeds (the common case) just
// leave; if it fails (no SD card) stay put and reveal the Retry/Discard bar instead of silently
// losing the last edit.
void back_from_detail(void) {
    if (!s_cur.dirty) { nav_to(Page::List); return; }
    if (s_autosave_timer) { lv_timer_delete(s_autosave_timer); s_autosave_timer = nullptr; }
    if (save_note()) { nav_to(Page::List); return; }
    if (s_retry_bar) lv_obj_clear_flag(s_retry_bar, LV_OBJ_FLAG_HIDDEN);
}

// ---------------------------------------------------------------- List page actions

void new_note_cb(lv_event_t *) {
    long e = (long)time(nullptr);
    while (epoch_in_use(e)) e++;
    s_cur.epoch = e;
    snprintf(s_cur.fname, sizeof s_cur.fname, "n%ld.txt", e);
    s_cur.is_new = true;
    s_cur.dirty = false;
    s_cur.save_failed = false;
    nav_to(Page::Detail);
}

void row_click_cb(lv_event_t *e) {
    const int i = (int)(intptr_t)lv_event_get_user_data(e);
    if (i < 0 || i >= s_n) return;
    snprintf(s_cur.fname, sizeof s_cur.fname, "%s", s_notes[i].fname);
    s_cur.epoch = s_notes[i].epoch;
    s_cur.is_new = false;
    s_cur.dirty = false;
    s_cur.save_failed = false;
    nav_to(Page::Detail);
}

void search_changed_cb(lv_event_t *e) {
    lv_obj_t *ta = (lv_obj_t *)lv_event_get_target(e);
    const char *t = lv_textarea_get_text(ta);
    snprintf(s_query, sizeof s_query, "%s", t ? t : "");
    render_rows();
}

lv_obj_t *note_row(lv_obj_t *col, const NoteMeta *m, int index) {
    const NvTheme *th = nv_theme_get();
    lv_obj_t *row = lv_obj_create(col);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(row, th->surface, 0);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(row, NV_RAD_MD, 0);
    lv_obj_set_style_pad_all(row, NV_SP_3, 0);
    lv_obj_set_style_pad_column(row, NV_SP_3, 0);
    lv_obj_set_style_min_height(row, NV_TOUCH_MIN, 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_color(row, th->surface2, LV_STATE_PRESSED);
    lv_obj_add_event_cb(row, row_click_cb, LV_EVENT_CLICKED, (void *)(intptr_t)index);

    lv_obj_t *txt = lv_obj_create(row);
    lv_obj_remove_style_all(txt);
    lv_obj_set_height(txt, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(txt, LV_FLEX_FLOW_COLUMN);
    lv_obj_clear_flag(txt, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_grow(txt, 1);

    const bool untitled = !m->title[0];
    lv_obj_t *title = lv_label_create(txt);
    lv_label_set_text(title, untitled ? nv_tr(NV_STR_NOTES_TITLE_PH) : m->title);
    lv_obj_set_width(title, lv_pct(100));
    lv_obj_set_style_text_font(title, &nv_font_20, 0);
    lv_obj_set_style_text_color(title, untitled ? th->text_dim : th->text, 0);
    lv_label_set_long_mode(title, LV_LABEL_LONG_DOT);

    if (m->preview[0]) {
        lv_obj_t *prev = lv_label_create(txt);
        lv_label_set_text(prev, m->preview);
        lv_obj_set_width(prev, lv_pct(100));
        lv_obj_set_style_text_font(prev, &nv_font_14, 0);
        lv_obj_set_style_text_color(prev, th->text_dim, 0);
        lv_label_set_long_mode(prev, LV_LABEL_LONG_DOT);
    }

    lv_obj_t *tm = lv_label_create(row);
    char tbuf[16];
    fmt_row_time(m->epoch, tbuf, sizeof tbuf);
    lv_label_set_text(tm, tbuf);
    lv_obj_set_style_text_font(tm, &nv_font_14, 0);
    lv_obj_set_style_text_color(tm, th->text_dim, 0);
    return row;
}

void render_rows(void) {
    if (!s_list) return;
    lv_obj_clean(s_list);
    int shown = 0;
    for (int i = 0; i < s_n; i++) {
        if (!ci_contains(s_notes[i].title, s_query) && !ci_contains(s_notes[i].preview, s_query)) continue;
        note_row(s_list, &s_notes[i], i);
        shown++;
    }
    if (shown == 0) {
        lv_obj_t *info = nv_kit_info(s_list);
        lv_label_set_text(info, s_n == 0 ? nv_tr(NV_STR_NOTES_EMPTY) : nv_tr(NV_STR_NO_RESULTS));
        lv_obj_set_style_text_color(info, nv_theme_get()->text_dim, 0);
    }
}

// ---------------------------------------------------------------- page builders

void list_deleted(lv_event_t *) { s_list = nullptr; }

void build_list(void) {
    lv_obj_t *content = nv_ui_app_content();
    if (!content) return;
    lv_obj_clean(content);
    s_list = nullptr;

    nv_ui_set_title(nv_tr(NV_STR_APP_NOTES));
    nv_ui_set_back(nullptr);

    scan_notes();

    lv_obj_t *root = lv_obj_create(content);
    lv_obj_remove_style_all(root);
    lv_obj_set_size(root, lv_pct(100), lv_pct(100));
    lv_obj_set_style_pad_all(root, 14, 0);
    lv_obj_set_style_pad_row(root, 10, 0);
    lv_obj_set_flex_flow(root, LV_FLEX_FLOW_COLUMN);
    lv_obj_clear_flag(root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(root, list_deleted, LV_EVENT_DELETE, nullptr);

    if (!nv_sd_is_mounted()) {
        lv_obj_t *info = nv_kit_info(root);
        lv_label_set_text(info, nv_tr(NV_STR_SD_MISSING));
        lv_obj_set_style_text_color(info, nv_theme_get()->text_dim, 0);
        return;
    }

    lv_obj_t *bar = lv_obj_create(root);
    lv_obj_remove_style_all(bar);
    lv_obj_set_size(bar, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(bar, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(bar, 10, 0);
    lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *search = nv_kit_textarea_ex(bar, nv_tr(NV_STR_SEARCH), true, NV_IME_TEXT, NV_IME_RET_DONE);
    lv_obj_set_flex_grow(search, 1);
    if (s_query[0]) lv_textarea_set_text(search, s_query);
    lv_obj_add_event_cb(search, search_changed_cb, LV_EVENT_VALUE_CHANGED, nullptr);

    char nb[40];
    lv_snprintf(nb, sizeof nb, LV_SYMBOL_PLUS "  %s", nv_tr(NV_STR_NOTES_NEW));
    lv_obj_t *add = nv_kit_button(bar, nb, true);
    lv_obj_add_event_cb(add, new_note_cb, LV_EVENT_CLICKED, nullptr);

    s_list = nv_kit_scroll_column(root);
    lv_obj_set_flex_grow(s_list, 1);
    render_rows();
}

// Best-effort flush for the path where the whole app is torn down (home/switch) instead of the
// user tapping Back: catches edits made just before that, since the debounce timer alone would
// otherwise lose up to kAutosaveMs of typing.
void detail_deleted(lv_event_t *) {
    if (s_autosave_timer) { lv_timer_delete(s_autosave_timer); s_autosave_timer = nullptr; }
    if (s_cur.dirty) save_note();
    nv_ime_hide();
    s_title_ta = s_body_ta = s_status = s_del_btn_label = s_retry_bar = nullptr;
}

void build_detail(void) {
    lv_obj_t *content = nv_ui_app_content();
    if (!content) return;
    lv_obj_clean(content);
    s_del_armed = false;
    s_del_btn_label = nullptr;
    s_retry_bar = nullptr;
    const NvTheme *th = nv_theme_get();

    nv_ui_set_title(nv_tr(NV_STR_APP_NOTES));
    nv_ui_set_back(back_from_detail);

    lv_obj_t *root = lv_obj_create(content);
    lv_obj_remove_style_all(root);
    lv_obj_set_size(root, lv_pct(100), lv_pct(100));
    lv_obj_set_style_pad_all(root, 14, 0);
    lv_obj_set_style_pad_row(root, 10, 0);
    lv_obj_set_flex_flow(root, LV_FLEX_FLOW_COLUMN);
    lv_obj_clear_flag(root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(root, detail_deleted, LV_EVENT_DELETE, nullptr);

    s_title_ta = nv_kit_textarea_ex(root, nv_tr(NV_STR_NOTES_TITLE_PH), true, NV_IME_TEXT, NV_IME_RET_NEXT);
    lv_obj_set_width(s_title_ta, lv_pct(100));
    lv_obj_set_style_text_font(s_title_ta, &nv_font_20, 0);
    lv_textarea_set_max_length(s_title_ta, kTitleLen - 1);

    lv_obj_t *bar = lv_obj_create(root);
    lv_obj_remove_style_all(bar);
    lv_obj_set_size(bar, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(bar, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(bar, 12, 0);
    lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);

    s_status = lv_label_create(bar);
    lv_label_set_text(s_status, "");
    lv_obj_set_style_text_color(s_status, th->text_dim, 0);
    lv_obj_set_flex_grow(s_status, 1);

    lv_obj_t *del = nv_kit_button(bar, nv_tr(NV_STR_DELETE), false);
    lv_obj_set_style_text_color(lv_obj_get_child(del, 0), th->danger, 0);
    s_del_btn_label = lv_obj_get_child(del, 0);
    lv_obj_add_event_cb(del, del_cb, LV_EVENT_CLICKED, nullptr);

    s_body_ta = nv_kit_textarea(root, nv_tr(NV_STR_NOTES_PLACEHOLDER), false);
    lv_obj_set_width(s_body_ta, lv_pct(100));
    lv_obj_set_flex_grow(s_body_ta, 1);

    // Hidden unless a save fails and Back needs the user to choose.
    s_retry_bar = lv_obj_create(root);
    lv_obj_remove_style_all(s_retry_bar);
    lv_obj_set_size(s_retry_bar, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(s_retry_bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(s_retry_bar, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(s_retry_bar, 10, 0);
    lv_obj_clear_flag(s_retry_bar, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_retry_bar, LV_OBJ_FLAG_HIDDEN);
    lv_obj_t *retry = nv_kit_button(s_retry_bar, nv_tr(NV_STR_NOTES_RETRY), true);
    lv_obj_add_event_cb(retry, retry_cb, LV_EVENT_CLICKED, nullptr);
    lv_obj_t *disc = nv_kit_button(s_retry_bar, nv_tr(NV_STR_NOTES_DISCARD), false);
    lv_obj_add_event_cb(disc, discard_cb, LV_EVENT_CLICKED, nullptr);

    // Populate before wiring autosave, so the initial set_text() never arms the debounce timer.
    if (!s_cur.is_new) {
        char title[kTitleLen];
        if (load_note(s_cur.fname, title, sizeof title)) lv_textarea_set_text(s_title_ta, title);
    }
    lv_obj_add_event_cb(s_title_ta, field_changed_cb, LV_EVENT_VALUE_CHANGED, nullptr);
    lv_obj_add_event_cb(s_body_ta, field_changed_cb, LV_EVENT_VALUE_CHANGED, nullptr);
}

// ---------------------------------------------------------------- app plumbing

// One-time import of the old single-note app's file into the new per-note store, so upgrading
// doesn't make a user's existing note vanish. Removes the legacy file once folded in (or if it
// was empty) so this only ever runs once.
void migrate_legacy(void) {
    if (!nv_sd_is_mounted()) return;
    FILE *old = fopen(kLegacyPath, "rb");
    if (!old) return;
    fseek(old, 0, SEEK_END);
    long n = ftell(old);
    fseek(old, 0, SEEK_SET);
    if (n <= 0) { fclose(old); unlink(kLegacyPath); return; }
    if (n > kMaxNoteBody) n = kMaxNoteBody;
    char *buf = (char *)malloc((size_t)n + 1);
    if (!buf) { fclose(old); return; }
    const size_t rd = fread(buf, 1, (size_t)n, old);
    buf[rd] = '\0';
    fclose(old);

    mkdir(kNotesDir, 0777);   // ok if it already exists
    const long e = (long)time(nullptr);
    char full[256];
    snprintf(full, sizeof full, "%s/n%ld.txt", kNotesDir, e);
    FILE *nf = fopen(full, "wb");
    if (nf) {
        fprintf(nf, "%ld\n\n", e);   // epoch line + empty title line, then the old body
        fwrite(buf, 1, rd, nf);
        fclose(nf);
        unlink(kLegacyPath);
    }
    free(buf);
}

void notes_build(lv_obj_t *) {
    if (!s_notes) {
        s_notes = (NoteMeta *)heap_caps_calloc(kMaxNotes, sizeof(NoteMeta), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!s_notes) s_notes = (NoteMeta *)calloc(kMaxNotes, sizeof(NoteMeta));
        if (!s_notes) return;
    }
    s_query[0] = '\0';
    migrate_legacy();
    build_list();
}

const NvApp kNotesApp = {"notes", "Notes", &nv_icon_notes, 768u << 10, notes_build,
                         NV_STR_APP_NOTES, nullptr};

}  // namespace

void notes_app_register(void) { nv_app_register(&kNotesApp); }
