// notes_app — a searchable list of notes with an autosaving title+body editor, backed by
// individual files under /sdcard/notes/.
//   List   : title + one-line preview + last-modified time per row, search box, + to add one.
//   Detail : title field + body editor; autosaves ~1.2s after the last keystroke so there is no
//            Save button. Two-step delete (tap-again, files_app convention). If the last autosave
//            failed (no SD card) Back reveals an inline Retry/Discard bar instead of just leaving,
//            and so does the system's way out (leave guard) before the app goes.
// Teardown (notes_close): the editor is flushed first. A theme / language / rotation rebuild comes
// back on the same page, note, search, cursor and list scroll; leaving for another app saves them
// as task state. Text whose save failed then is held in PSRAM and comes back still unsaved.
// Page switches are deferred via lv_async_call (files_app pattern): the callback that reacts to
// an event must not tear down its own subtree synchronously.
// Note file format: line 1 = epoch (decimal, last-modified), line 2 = title, rest = body.
//
// Opened on a file (nv_open handler "notes.edit", text/plain + text/markdown): a note of our own
// store opens as that note; any other file opens in EXTERNAL mode — the editor edits the raw file
// (no header lines, no title field, CRLF kept), autosaves through a temp file + rename, and Back
// returns to the app that asked. A file over kMaxNoteBody opens read-only: saving a truncated
// copy would destroy the tail.
#include "apps_internal.h"
#include "nv_mem_attr.h"

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
#include "nv_open.h"
#include "nv_ui_focus.h"   // Ctrl+N / Ctrl+F / Ctrl+S (nv_ui_set_key_handler)

#include "lvgl.h"
#include "esp_heap_caps.h"

#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cstdint>
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
Page s_page = Page::List;      // on screen (a rebuild re-renders it)
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

// Intent state (see the header comment). s_from_intent: the editor was opened on a file by another
// app, so leaving it returns there. s_ext: that file is not one of our notes.
bool s_from_intent = false;
bool s_ext         = false;
bool s_ext_ro      = false;   // external file too large to edit safely
bool s_ext_crlf    = false;   // external file used CRLF line ends: written back the same way
NV_PSRAM_BSS char s_ext_path[NV_OPEN_PATH_MAX];

lv_obj_t   *s_list        = nullptr;   // List page: scrollable row column
lv_obj_t   *s_search      = nullptr;   // List page: search field (Ctrl+F)
lv_obj_t   *s_title_ta    = nullptr;   // Detail page
lv_obj_t   *s_body_ta     = nullptr;
lv_obj_t   *s_status      = nullptr;
lv_obj_t   *s_del_btn_label = nullptr;
bool        s_del_armed   = false;
lv_obj_t   *s_retry_bar   = nullptr;   // hidden unless a save fails and Back needs a decision
lv_timer_t *s_autosave_timer = nullptr;
bool        s_leaving     = false;     // the bar answers the leave guard: resolved, the app closes

// Teardown state. s_keep: close(NV_APP_REBUILD) asked the build that follows at once to go on with
// the page, note and search as they are. Cursor and list scroll come back once (the *_keep flags).
bool     s_keep = false;
uint32_t s_cursor = 0;
bool     s_cursor_keep = false;
int32_t  s_list_scroll = 0;
bool     s_list_scroll_keep = false;
// The intent already applied this open: the system hands it to every rebuild until the app closes,
// and applied again it reloaded the file over the editor. Compared whole.
uint32_t s_applied = 0;   // serial of the intent applied last (NvIntent.serial)
bool s_applied_ok = false;

// Editor text whose save failed at a teardown (no card): held in PSRAM for the editor that comes
// back (rebuild, switch back), which shows it still unsaved. One note at a time, keyed by its file.
char *s_held_body = nullptr;
NV_PSRAM_BSS char s_held_title[kTitleLen];
NV_PSRAM_BSS char s_held_key[NV_OPEN_PATH_MAX];   // s_ext_path or s_cur.fname

// What a suspended Notes task keeps (nv_ui_state_save, on NV_APP_SWITCH only).
struct NotesState {
    char     query[sizeof s_query];
    char     fname[kFnameLen];
    char     ext_path[NV_OPEN_PATH_MAX];
    uint32_t cursor;
    int32_t  scroll;
    uint8_t  page;
    bool     is_new, ext, ext_crlf;
};

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

void set_saved_status(void) {
    char tbuf[16];
    nv_time_format(tbuf, sizeof tbuf, nv_time_is_24h() ? "%H:%M" : "%I:%M %p");
    char msg[48];
    lv_snprintf(msg, sizeof msg, nv_tr(NV_STR_NOTES_SAVED_FMT), tbuf);
    set_status(msg, nv_theme_get()->text_dim);
}

// EXTERNAL mode save: the raw body to a sibling temp file, then unlink + rename (FATFS rename never
// overwrites). A failed write leaves the original untouched; a failed rename keeps the temp file,
// which is then the only good copy (ENGINEERING_RULES §5).
bool save_ext(void) {
    if (!s_body_ta || s_ext_ro) { s_cur.dirty = false; return true; }
    const char *body = lv_textarea_get_text(s_body_ta);
    char tmp[NV_OPEN_PATH_MAX + 8];
    snprintf(tmp, sizeof tmp, "%s.tmp", s_ext_path);
    bool ok = false;
    if (FILE *f = nv_sd_fopen(tmp, "wb")) {
        ok = true;
        const char *p = body ? body : "";
        if (s_ext_crlf) {
            for (const char *q; ok && *p; p = q) {
                q = strchr(p, '\n');
                const size_t len = q ? (size_t)(q - p) : strlen(p);
                if (len && fwrite(p, 1, len, f) != len) ok = false;
                if (q) { if (fwrite("\r\n", 1, 2, f) != 2) ok = false; q++; }
                else q = p + len;
            }
        } else {
            const size_t len = strlen(p);
            if (len && fwrite(p, 1, len, f) != len) ok = false;
        }
        if (nv_sd_fclose(f) != 0) ok = false;
    }
    if (ok) {
        unlink(s_ext_path);
        ok = rename(tmp, s_ext_path) == 0;
    } else {
        unlink(tmp);
    }
    if (!ok) {
        s_cur.save_failed = true;
        set_status(nv_tr(NV_STR_SAVE_FAILED), nv_theme_get()->danger);
        return false;
    }
    s_cur.dirty = false;
    s_cur.save_failed = false;
    set_saved_status();
    return true;
}

// EXTERNAL mode load: up to kMaxNoteBody bytes into the body editor; CRLF -> LF (remembered), a
// UTF-8 BOM is dropped. Returns false when the file cannot be read.
bool load_ext(void) {
    FILE *f = nv_sd_fopen(s_ext_path, "rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END);
    const long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    s_ext_ro = size > kMaxNoteBody;
    const size_t n = size <= 0 ? 0 : (size_t)(s_ext_ro ? kMaxNoteBody : size);
    char *buf = (char *)heap_caps_malloc(n + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) { nv_sd_fclose(f); return false; }
    const size_t rd = n ? fread(buf, 1, n, f) : 0;
    nv_sd_fclose(f);
    size_t i = (rd >= 3 && (uint8_t)buf[0] == 0xEF && (uint8_t)buf[1] == 0xBB && (uint8_t)buf[2] == 0xBF) ? 3 : 0;
    size_t o = 0;
    s_ext_crlf = false;
    for (; i < rd; i++) {
        if (buf[i] == '\r' && i + 1 < rd && buf[i + 1] == '\n') { s_ext_crlf = true; continue; }
        buf[o++] = buf[i] ? buf[i] : ' ';
    }
    buf[o] = '\0';
    if (s_body_ta) lv_textarea_set_text(s_body_ta, buf);
    heap_caps_free(buf);
    return true;
}

// Writes the current title+body to s_cur.fname. A brand-new, still-empty note is not persisted
// at all (nothing to save yet, no orphan file). Returns false only on a real write failure.
bool save_note(void) {
    if (s_ext) return save_ext();
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
    set_saved_status();
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

// ---------------------------------------------------------------- held text (failed save at teardown)

const char *cur_key(void) { return s_ext ? s_ext_path : s_cur.fname; }
void held_drop(void) {
    if (s_held_body) heap_caps_free(s_held_body);
    s_held_body = nullptr;
}
bool held_for_cur(void) { return s_held_body && !strcmp(s_held_key, cur_key()); }
// Copy the editor's text (widgets still alive) for the editor that comes back.
void held_keep(void) {
    held_drop();
    const char *body = s_body_ta ? lv_textarea_get_text(s_body_ta) : nullptr;
    const char *title = s_title_ta ? lv_textarea_get_text(s_title_ta) : nullptr;
    const size_t n = body ? strlen(body) : 0;
    s_held_body = (char *)heap_caps_malloc(n + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_held_body) return;   // no memory: the edit is lost, as before
    memcpy(s_held_body, body ? body : "", n + 1);
    lv_snprintf(s_held_title, sizeof s_held_title, "%s", title ? title : "");
    lv_snprintf(s_held_key, sizeof s_held_key, "%s", cur_key());
}

// Leaving the editor: back to the list, or — opened on a file by another app — back to that app.
void leave_detail(void) {
    if (s_from_intent) nv_open_finish();
    else               nav_to(Page::List);
}

// The system's way out (Home / Recents gesture, Alt+F4...) while the editor has an unsaved edit:
// save it now, and only when that fails stay and show the Retry / Discard bar (detail_done then
// closes the app). Set with the editor; returns false (go ahead) whenever there is nothing to lose.
bool notes_leave_guard(void) {
    if (!s_body_ta || !(s_cur.dirty || s_cur.save_failed)) return false;
    if (s_autosave_timer) { lv_timer_delete(s_autosave_timer); s_autosave_timer = nullptr; }
    if (save_note()) return false;
    s_leaving = true;
    if (s_retry_bar) lv_obj_clear_flag(s_retry_bar, LV_OBJ_FLAG_HIDDEN);
    return true;
}
void leave_app_async(void *) { nv_ui_close_app(); }
// Retry / Discard resolved: back where Back leads, or out of the app when the guard asked.
void detail_done(void) {
    if (!s_leaving) { leave_detail(); return; }
    s_leaving = false;
    nv_ui_set_leave_guard(nullptr);
    lv_async_call(leave_app_async, nullptr);   // not from inside this button's own event
}

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
    leave_detail();
}

void retry_cb(lv_event_t *) { if (save_note()) detail_done(); }

void discard_cb(lv_event_t *) {
    s_cur.dirty = false;
    s_cur.save_failed = false;
    if (s_autosave_timer) { lv_timer_delete(s_autosave_timer); s_autosave_timer = nullptr; }
    detail_done();
}

// Back from the editor: flush any pending edit first. If that succeeds (the common case) just
// leave; if it fails (no SD card) stay put and reveal the Retry/Discard bar instead of silently
// losing the last edit.
void back_from_detail(void) {
    if (!s_cur.dirty) { leave_detail(); return; }
    if (s_autosave_timer) { lv_timer_delete(s_autosave_timer); s_autosave_timer = nullptr; }
    if (save_note()) { leave_detail(); return; }
    if (s_retry_bar) lv_obj_clear_flag(s_retry_bar, LV_OBJ_FLAG_HIDDEN);
}

// ---------------------------------------------------------------- List page actions

// A note's FILE NAME is its creation epoch (n<epoch>.txt), while NoteMeta.epoch is the last-
// modified time rewritten on every autosave — so epoch_in_use() alone no longer protects the
// name of an edited note. With an unset clock (time() restarts near 0 each boot) a new note
// used to pick an existing file's name and the first autosave ("wb") destroyed that note.
bool note_file_exists(long e) {
    char full[64];
    snprintf(full, sizeof full, "%s/n%ld.txt", kNotesDir, e);
    struct stat st;
    return stat(full, &st) == 0;
}

void new_note_cb(lv_event_t *) {
    long e = (long)time(nullptr);
    while (epoch_in_use(e) || note_file_exists(e)) e++;
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
        if (!nv_kit_contains_ci(s_notes[i].title, s_query) && !nv_kit_contains_ci(s_notes[i].preview, s_query)) continue;
        note_row(s_list, &s_notes[i], i);
        shown++;
    }
    if (shown == 0) {
        lv_obj_t *info = nv_kit_info(s_list);
        lv_label_set_text(info, s_n == 0 ? nv_tr(NV_STR_NOTES_EMPTY) : nv_tr(NV_STR_NO_RESULTS));
        lv_obj_set_style_text_color(info, nv_theme_get()->text_dim, 0);
    }
}

// ---------------------------------------------------------------- keyboard
// Called only while no text field has the focus (nv_ui_set_key_handler). HID usages: N, F, S.

const nv_shortcut_t kListKeys[] = {
    {"Ctrl+N", "Nuova nota", "New note"},
    {"Ctrl+F", "Cerca", "Search"},
};
const nv_shortcut_t kDetailKeys[] = {
    {"Ctrl+S", "Salva ora", "Save now"},
};

bool list_key(uint32_t, uint8_t usage, uint8_t mods) {
    if (!(mods & 0x11) || !s_list) return false;   // no SD card: no list, no actions
    if (usage == 0x11) { new_note_cb(nullptr); return true; }
    if (usage == 0x09 && s_search) { nv_focus_set(s_search); return true; }
    return false;
}

// Ctrl+S: flush the pending autosave now (save_note shows the "saved" status).
bool detail_key(uint32_t, uint8_t usage, uint8_t mods) {
    if (!(mods & 0x11) || usage != 0x16) return false;
    if (s_autosave_timer) { lv_timer_delete(s_autosave_timer); s_autosave_timer = nullptr; }
    if (s_cur.dirty) save_note();
    return true;
}

// ---------------------------------------------------------------- page builders

void list_deleted(lv_event_t *) { s_list = nullptr; s_search = nullptr; }

void build_list(void) {
    lv_obj_t *content = nv_ui_app_content();
    if (!content) return;
    lv_obj_clean(content);
    s_list = nullptr;
    s_search = nullptr;
    s_page = Page::List;

    nv_ui_set_title(nv_tr(NV_STR_APP_NOTES));
    nv_ui_set_back_handler(nullptr);
    nv_ui_set_leave_guard(nullptr);   // nothing unsaved on the list
    nv_ui_set_key_handler(list_key);
    nv_ui_set_shortcuts(kListKeys, (int)(sizeof kListKeys / sizeof kListKeys[0]));

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
    s_search = search;
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
    if (s_list_scroll_keep) {   // rebuilt / back from another app: where the list was
        lv_obj_update_layout(s_list);
        lv_obj_scroll_to_y(s_list, s_list_scroll, LV_ANIM_OFF);
    }
    s_list_scroll_keep = false;
}

// Best-effort flush when the editor page goes without Back (notes_close flushed a teardown
// already): catches edits made just before, since the debounce timer alone would otherwise lose
// up to kAutosaveMs of typing. Text held after a failed save is not tried again here.
void detail_deleted(lv_event_t *) {
    if (s_autosave_timer) { lv_timer_delete(s_autosave_timer); s_autosave_timer = nullptr; }
    if (s_cur.dirty && !held_for_cur()) save_note();
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
    s_leaving = false;
    s_page = Page::Detail;
    const NvTheme *th = nv_theme_get();

    if (s_ext) {   // the file's own name is the title; there is no title line to edit
        const char *base = strrchr(s_ext_path, '/');
        nv_ui_set_title(base ? base + 1 : s_ext_path);
    } else {
        nv_ui_set_title(nv_tr(NV_STR_APP_NOTES));
    }
    nv_ui_set_back_handler(back_from_detail);
    nv_ui_set_leave_guard(notes_leave_guard);
    nv_ui_set_key_handler(detail_key);
    nv_ui_set_shortcuts(kDetailKeys, (int)(sizeof kDetailKeys / sizeof kDetailKeys[0]));

    lv_obj_t *root = lv_obj_create(content);
    lv_obj_remove_style_all(root);
    lv_obj_set_size(root, lv_pct(100), lv_pct(100));
    lv_obj_set_style_pad_all(root, 14, 0);
    lv_obj_set_style_pad_row(root, 10, 0);
    lv_obj_set_flex_flow(root, LV_FLEX_FLOW_COLUMN);
    lv_obj_clear_flag(root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(root, detail_deleted, LV_EVENT_DELETE, nullptr);

    s_title_ta = nullptr;
    if (!s_ext) {
        s_title_ta = nv_kit_textarea_ex(root, nv_tr(NV_STR_NOTES_TITLE_PH), true, NV_IME_TEXT, NV_IME_RET_NEXT);
        lv_obj_set_width(s_title_ta, lv_pct(100));
        lv_obj_set_style_text_font(s_title_ta, &nv_font_20, 0);
        lv_textarea_set_max_length(s_title_ta, kTitleLen - 1);
    }

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

    if (!s_ext) {   // deleting someone else's file is the file manager's job, not the editor's
        lv_obj_t *del = nv_kit_button(bar, nv_tr(NV_STR_DELETE), false);
        lv_obj_set_style_text_color(lv_obj_get_child(del, 0), th->danger, 0);
        s_del_btn_label = lv_obj_get_child(del, 0);
        lv_obj_add_event_cb(del, del_cb, LV_EVENT_CLICKED, nullptr);
    }

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
    if (held_for_cur()) {   // a save failed at the last teardown: that text, still unsaved
        if (s_title_ta) lv_textarea_set_text(s_title_ta, s_held_title);
        lv_textarea_set_text(s_body_ta, s_held_body);
        held_drop();
        s_cur.dirty = s_cur.save_failed = true;
        set_status(nv_tr(NV_STR_SAVE_FAILED), th->danger);
    } else if (s_ext) {
        if (!load_ext()) {
            s_ext_ro = true;   // nothing loaded: never write an empty body over the file
            set_status(nv_tr(NV_STR_OPEN_FAILED), th->danger);
        } else if (s_ext_ro) {
            char msg[64];
            lv_snprintf(msg, sizeof msg, nv_tr(NV_STR_PREVIEW_TRUNC_FMT), (unsigned)(kMaxNoteBody / 1024));
            set_status(msg, th->text_dim);
        }
        if (s_ext_ro) {
            lv_obj_remove_flag(s_body_ta, LV_OBJ_FLAG_CLICKABLE);   // read-only: no IME, no edits
            return;
        }
    } else if (!s_cur.is_new) {
        char title[kTitleLen];
        if (load_note(s_cur.fname, title, sizeof title)) lv_textarea_set_text(s_title_ta, title);
    }
    if (s_title_ta) lv_obj_add_event_cb(s_title_ta, field_changed_cb, LV_EVENT_VALUE_CHANGED, nullptr);
    lv_obj_add_event_cb(s_body_ta, field_changed_cb, LV_EVENT_VALUE_CHANGED, nullptr);
    if (s_cursor_keep) lv_textarea_set_cursor_pos(s_body_ta, (int32_t)s_cursor);   // rebuilt / resumed
    s_cursor_keep = false;
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

// Teardown hook (widgets still alive). Queued page work must not land in the next app's content.
// The editor is flushed here, not in its DELETE handler, so a failed save can still keep the text
// (held_keep) for the editor that comes back. REBUILD keeps page, note, search, cursor and list
// scroll; SWITCH saves them as task state; CLOSE forgets them.
void notes_close(nv_app_teardown_t why) {
    lv_async_call_cancel(nav_apply, nullptr);
    lv_async_call_cancel(leave_app_async, nullptr);
    if (s_nav_pending) s_page = s_pending;   // where the user was going: the next build lands there
    s_cursor_keep = false;
    if (s_body_ta && s_page == Page::Detail) {
        if (s_autosave_timer) { lv_timer_delete(s_autosave_timer); s_autosave_timer = nullptr; }
        s_cursor = lv_textarea_get_cursor_pos(s_body_ta);
        s_cursor_keep = true;
        if (s_cur.dirty && !save_note() && why != NV_APP_CLOSE) held_keep();
    }
    s_list_scroll_keep = s_list && s_page == Page::List && !s_nav_pending;
    s_list_scroll = s_list_scroll_keep ? lv_obj_get_scroll_y(s_list) : 0;
    s_nav_pending = false;
    s_leaving = false;
    nv_ui_set_leave_guard(nullptr);   // the rebuilt editor sets it again
    s_keep = why == NV_APP_REBUILD;
    if (why == NV_APP_SWITCH) {
        NotesState st{};
        memcpy(st.query, s_query, sizeof st.query);
        memcpy(st.fname, s_cur.fname, sizeof st.fname);
        memcpy(st.ext_path, s_ext_path, sizeof st.ext_path);
        st.cursor = s_cursor;
        st.scroll = s_list_scroll;
        st.page = (uint8_t)s_page;
        st.is_new = s_cur.is_new;
        st.ext = s_ext;
        st.ext_crlf = s_ext_crlf;
        nv_ui_state_save(&st, sizeof st);
    } else if (why == NV_APP_CLOSE) {
        held_drop();
        s_query[0] = '\0';
    }
    if (why != NV_APP_REBUILD) s_applied_ok = false;
}

void notes_build(lv_obj_t *) {
    if (!s_notes) {
        s_notes = (NoteMeta *)heap_caps_calloc(kMaxNotes, sizeof(NoteMeta), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!s_notes) s_notes = (NoteMeta *)calloc(kMaxNotes, sizeof(NoteMeta));
        if (!s_notes) return;
    }
    const bool keep = s_keep && nv_ui_app_rebuilding();
    s_keep = false;
    s_nav_pending = false;
    const NvIntent *in = nv_open_intent();
    if (in && (in->verb != NV_INTENT_OPEN || !in->path[0])) in = nullptr;
    if (in && s_applied_ok && in->serial == s_applied) in = nullptr;   // applied already
    if (keep && !in) {   // theme / language / rotation: the same page, note and search
        if (s_page == Page::Detail) build_detail();
        else                        build_list();
        s_cursor_keep = s_list_scroll_keep = false;
        return;
    }
    migrate_legacy();

    // Opened on a file: straight into the editor. A file of our own store is that note; anything
    // else is EXTERNAL mode. The intent outlives rebuilds: once applied it is not replayed (only a
    // new one is, delivered in place; notes_close has flushed the pending edit first).
    s_from_intent = s_ext = s_ext_ro = false;
    if (in) {
        s_applied = in->serial;
        s_applied_ok = true;
        s_query[0] = '\0';
        s_cursor_keep = s_list_scroll_keep = false;
        held_drop();
        s_from_intent = true;
        s_cur = {};
        const size_t dl = strlen(kNotesDir);
        const char *base = strrchr(in->path, '/');
        if (!strncmp(in->path, kNotesDir, dl) && in->path + dl == base &&
            strlen(base + 1) < (size_t)kFnameLen) {
            snprintf(s_cur.fname, sizeof s_cur.fname, "%s", base + 1);
            NoteMeta m{};
            if (read_meta(s_cur.fname, &m)) s_cur.epoch = m.epoch;
        } else {
            s_ext = true;
            snprintf(s_ext_path, sizeof s_ext_path, "%s", in->path);
        }
        build_detail();
        return;
    }
    s_applied_ok = false;

    // Back from another app: the same search, and the note that was open (no caller to return to
    // any more) with its cursor, or the list scroll. A fresh open starts on an empty search.
    NotesState st;
    if (!keep && nv_ui_state_load(&st, sizeof st) == sizeof st) {
        st.query[sizeof st.query - 1] = st.fname[sizeof st.fname - 1] = st.ext_path[sizeof st.ext_path - 1] = '\0';
        memcpy(s_query, st.query, sizeof s_query);
        if (st.page == (uint8_t)Page::Detail && (st.ext ? st.ext_path[0] : st.fname[0])) {
            s_cur = {};
            s_ext = st.ext;
            s_ext_crlf = st.ext_crlf;
            if (s_ext) {
                memcpy(s_ext_path, st.ext_path, sizeof s_ext_path);
            } else {
                memcpy(s_cur.fname, st.fname, sizeof s_cur.fname);
                s_cur.is_new = st.is_new;
                NoteMeta m{};
                if (!s_cur.is_new && read_meta(s_cur.fname, &m)) s_cur.epoch = m.epoch;
            }
            if (!held_for_cur()) held_drop();
            s_cursor = st.cursor;
            s_cursor_keep = true;
            s_list_scroll_keep = false;
            build_detail();
            s_cursor_keep = false;
            return;
        }
        s_list_scroll = st.scroll;
        s_list_scroll_keep = true;
    } else {
        s_query[0] = '\0';
        s_list_scroll_keep = false;
    }
    held_drop();
    s_cursor_keep = false;
    build_list();
}

const NvApp kNotesApp = {"notes", "Notes", &nv_icon_notes, 768u << 10, notes_build,
                         NV_STR_APP_NOTES, nullptr, 0u, notes_close};

// Notes edits plain text and Markdown (priority 50: the default editor). Structured text (JSON,
// configs, logs, CSV) is left to Files' read-only Preview — an autosaving editor must not rewrite
// a config the OS reads while the user is still typing.
const NvOpenHandler kNotesEdit = {
    "notes.edit", "notes", "text/plain text/markdown", -1, nullptr, LV_SYMBOL_EDIT,
    NV_OPEN_OPENER, 50, nullptr, nullptr,
};

}  // namespace

void notes_app_register(void) {
    nv_app_register(&kNotesApp);
    nv_open_register(&kNotesEdit);
}
