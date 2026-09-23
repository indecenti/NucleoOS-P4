// files_app — the system file browser (SD card).
//   List    : one directory at a time from /sdcard (bounded scan, dirs first, alpha sort). File rows
//             show the kind glyph + size. Tap = Open through nv_open (the default app, or the "Open
//             with" sheet); long-press or the row's menu button = Details.
//   Details : kind, MIME, size, modified, location; "Open with <app>", "Open with...", the file's
//             system actions (e.g. Set as wallpaper), rename, two-step delete.
//   Preview : Files' own read-only viewer for text and images, registered as the "files.preview"
//             opener — the safe way to look at a config or a log that no editor should autosave.
// Intents (nv_open.h): RESUME / REVEAL land on the file's folder with its row highlighted and in
// view; OPEN through "files.preview" shows the Preview page. A theme / language rebuild keeps the
// current folder and page (same content object = same instance).
// The system Back button walks the tree up (nv_ui_set_back) and closes the app only at the root.
// Page switches are DEFERRED via lv_async_call (gallery pattern): builders clean the content
// subtree that fired the event, so the rebuild must wait for the event to unwind.
#include "nv_mem_attr.h"   // NV_PSRAM_BSS: cold app tables out of internal SRAM
#include "apps_internal.h"

#include "nv_app.h"
#include "nv_ui_host.h"
#include "nv_ui_kit.h"
#include "nv_icons.h"
#include "nv_i18n.h"
#include "nv_fonts.h"
#include "nv_theme.h"
#include "nv_notify.h"
#include "nv_open.h"
#include "nv_sd.h"
#include "nv_time.h"

#include "esp_heap_caps.h"

#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cctype>
#include <ctime>

namespace {

constexpr char   kRoot[]        = "/sdcard";
constexpr int    kMaxEntries    = 200;
constexpr int    kNameMax       = 128;         // longer names are skipped (never clipped: a clipped
                                               // name is a path that does not exist)
constexpr size_t kPreviewMax    = 32 * 1024;   // text preview ceiling (label text lives in PSRAM)
constexpr int    kPreviewImgMax = 2048;        // SW decoder guard, same cap as the Gallery

struct Ent {
    char     name[kNameMax];
    uint32_t size;
    bool     dir;
};

Ent *s_ents = nullptr;   // PSRAM, allocated once
int  s_n = 0;
bool s_overflow = false;
NV_PSRAM_BSS char s_path[192];                     // current folder (LVGL thread only)
int  s_sel = -1;                                   // index into s_ents for the Details page
NV_PSRAM_BSS char s_sel_name[kNameMax];            // survives a rescan (rebuild re-resolves s_sel)
NV_PSRAM_BSS char s_focus[kNameMax];               // row to highlight + scroll into view once
NV_PSRAM_BSS char s_preview[NV_OPEN_PATH_MAX];     // file on the Preview page
char *s_text = nullptr;                            // Preview text (PSRAM, label points into it)

enum class Page { List, Detail, Preview };
Page s_page = Page::List;      // page on screen (a rebuild re-renders it)
Page s_pending = Page::List;
bool s_nav_pending = false;

lv_obj_t *s_del_btn_label = nullptr;   // two-step delete state (Details page)
bool      s_del_armed = false;
lv_obj_t *s_ren_ta = nullptr;          // rename textarea (Details page)

// One Files instance = one content object between open and close. build() re-runs on a theme /
// language refresh or when nv_open re-delivers an intent: same object -> keep the folder.
lv_obj_t *s_content = nullptr;
bool      s_open = false;

// The last intent acted on, so a plain rebuild does not replay it (a NEW intent delivered in place
// — e.g. "Preview" chosen from our own Details — differs and is applied).
struct Seen { bool valid; uint8_t verb; char handler[NV_OPEN_ID_MAX]; char path[NV_OPEN_PATH_MAX]; };
NV_PSRAM_BSS Seen s_seen;

void build_list(void);
void build_detail(void);
void build_preview(void);

void render(Page p) {
    switch (p) {
        case Page::List:    build_list();    break;
        case Page::Detail:  build_detail();  break;
        case Page::Preview: build_preview(); break;
    }
}
void nav_apply(void *) {
    s_nav_pending = false;
    render(s_pending);
}
void nav_to(Page p) {
    s_pending = p;
    if (!s_nav_pending && lv_async_call(nav_apply, nullptr) == LV_RESULT_OK)
        s_nav_pending = true;
}

// ---------------------------------------------------------------- model
int ent_cmp(const void *a, const void *b) {
    const Ent *x = (const Ent *)a, *y = (const Ent *)b;
    if (x->dir != y->dir) return x->dir ? -1 : 1;   // dirs first
    return strcasecmp(x->name, y->name);
}

void scan_dir(void) {
    s_n = 0;
    s_overflow = false;
    if (!s_ents || !nv_sd_is_mounted() || !nv_sd_session_begin()) return;
    if (DIR *d = opendir(s_path)) {
        struct dirent *e;
        while ((e = readdir(d)) != nullptr) {
            if (e->d_name[0] == '.') continue;
            const size_t nl = strlen(e->d_name);
            if (nl >= (size_t)kNameMax) continue;
            if (s_n >= kMaxEntries) { s_overflow = true; break; }
            Ent *en = &s_ents[s_n];
            memcpy(en->name, e->d_name, nl + 1);   // length checked above
            char full[448];
            snprintf(full, sizeof full, "%s/%s", s_path, e->d_name);
            struct stat st{};
            stat(full, &st);
            en->dir = S_ISDIR(st.st_mode);
            en->size = en->dir ? 0 : (uint32_t)st.st_size;
            s_n++;
        }
        closedir(d);
    }
    nv_sd_session_end();
    qsort(s_ents, (size_t)s_n, sizeof(Ent), ent_cmp);
}

void fmt_size(uint32_t b, char *out, size_t n) {
    if (b >= 1024u * 1024u)
        snprintf(out, n, "%u.%u MB", (unsigned)(b >> 20), (unsigned)(((b >> 10) & 1023) * 10 >> 10));
    else if (b >= 1024u)
        snprintf(out, n, "%u KB", (unsigned)(b >> 10));
    else
        snprintf(out, n, "%u B", (unsigned)b);
}

// Full path of entry i; false when it does not fit an nv_open path.
bool ent_path(int i, char *out, size_t n) {
    if (i < 0 || i >= s_n) return false;
    const int w = snprintf(out, n, "%s/%s", s_path, s_ents[i].name);
    return w > 0 && (size_t)w < n;
}

void select_ent(int i) {
    s_sel = i;
    snprintf(s_sel_name, sizeof s_sel_name, "%s", (i >= 0 && i < s_n) ? s_ents[i].name : "");
}

int find_ent(const char *name) {
    for (int i = 0; i < s_n; i++)
        if (!strcmp(s_ents[i].name, name)) return i;
    return -1;
}

// Point the browser at `dir` (must be /sdcard or below); anything else falls back to the root.
void set_folder(const char *dir) {
    const size_t rl = strlen(kRoot);
    if (!dir || strncmp(dir, kRoot, rl) != 0 || (dir[rl] != '\0' && dir[rl] != '/') ||
        strlen(dir) >= sizeof s_path) {
        strcpy(s_path, kRoot);
        return;
    }
    snprintf(s_path, sizeof s_path, "%s", dir);
    size_t n = strlen(s_path);
    while (n > rl && s_path[n - 1] == '/') s_path[--n] = '\0';
}

// Folder of `path` + its basename as the row to focus.
void set_folder_of(const char *path) {
    char dir[NV_OPEN_PATH_MAX];
    snprintf(dir, sizeof dir, "%s", path);
    char *slash = strrchr(dir, '/');
    s_focus[0] = '\0';
    if (slash && slash != dir) {
        snprintf(s_focus, sizeof s_focus, "%s", slash + 1);
        *slash = '\0';
    }
    set_folder(dir);
}

// ---------------------------------------------------------------- intents
bool intent_seen(const NvIntent *in) {
    return s_seen.valid && s_seen.verb == in->verb && !strcmp(s_seen.handler, in->handler) &&
           !strcmp(s_seen.path, in->path);
}

void apply_intent(const NvIntent *in) {
    s_seen.valid = true;
    s_seen.verb = in->verb;
    snprintf(s_seen.handler, sizeof s_seen.handler, "%s", in->handler);
    snprintf(s_seen.path, sizeof s_seen.path, "%s", in->path);
    struct stat st;
    const bool is_dir = stat(in->path, &st) == 0 && S_ISDIR(st.st_mode);
    if (in->verb == NV_INTENT_OPEN) {           // "files.preview"
        snprintf(s_preview, sizeof s_preview, "%s", in->path);
        set_folder_of(in->path);
        s_page = Page::Preview;
    } else {                                    // RESUME / REVEAL
        if (is_dir) { set_folder(in->path); s_focus[0] = '\0'; }
        else        set_folder_of(in->path);
        s_page = Page::List;
    }
}

// ---------------------------------------------------------------- back handling
void back_from_list(void) {
    char *slash = strrchr(s_path, '/');
    if (!slash || slash == s_path || !strcmp(s_path, kRoot)) return;  // unreachable at root
    snprintf(s_focus, sizeof s_focus, "%s", slash + 1);                // land on the folder we left
    *slash = '\0';
    scan_dir();
    nav_to(Page::List);
}
void back_to_list(void) {
    snprintf(s_focus, sizeof s_focus, "%s", s_sel_name);
    nav_to(Page::List);
}
// Preview is always reached through an intent. With a caller (another app opened the file) Back
// returns there; opened from our own list, it drops the intent and shows the file in its folder.
void back_from_preview(void) {
    const NvIntent *in = nv_open_intent();
    if (in && in->caller[0]) { nv_open_finish(); return; }
    nv_open_intent_drop();
    s_seen.valid = false;
    scan_dir();
    nav_to(Page::List);
}

// ---------------------------------------------------------------- List page
// Tap a file: open it (nv_open picks the app or asks). Nothing can open it: show its Details.
void open_ent(int i) {
    char full[NV_OPEN_PATH_MAX];
    const NvOpenHandler *h = nullptr;
    if (!ent_path(i, full, sizeof full) || nv_open_handlers(full, NV_OPEN_OPENER, &h, 1) == 0) {
        select_ent(i);
        nav_to(Page::Detail);
        return;
    }
    nv_open_file(full);   // deferred launch: safe from this row's own event
}

void row_click_cb(lv_event_t *e) {
    const int i = (int)(intptr_t)lv_event_get_user_data(e);
    if (i < 0 || i >= s_n) return;
    if (s_ents[i].dir) {
        const size_t len = strlen(s_path);
        if (len + 1 + strlen(s_ents[i].name) >= sizeof s_path) {   // would truncate: refuse
            nv_toast(NV_NOTE_WARN, s_ents[i].name);
            return;
        }
        snprintf(s_path + len, sizeof s_path - len, "/%s", s_ents[i].name);
        s_focus[0] = '\0';
        scan_dir();
        nav_to(Page::List);
    } else {
        open_ent(i);
    }
}

void row_details_cb(lv_event_t *e) {
    const int i = (int)(intptr_t)lv_event_get_user_data(e);
    if (i < 0 || i >= s_n || s_ents[i].dir) return;
    select_ent(i);
    nav_to(Page::Detail);
}

lv_obj_t *file_row(lv_obj_t *col, int i, const char *right) {
    const NvTheme *th = nv_theme_get();
    const Ent *en = &s_ents[i];
    lv_obj_t *row = lv_obj_create(col);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(row, th->surface, 0);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(row, NV_RAD_MD, 0);
    lv_obj_set_style_pad_left(row, NV_SP_4, 0);
    lv_obj_set_style_pad_right(row, en->dir ? NV_SP_4 : NV_SP_1, 0);
    lv_obj_set_style_pad_ver(row, NV_SP_3, 0);
    lv_obj_set_style_min_height(row, NV_TOUCH_MIN, 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, NV_SP_3, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_color(row, th->surface2, LV_STATE_PRESSED);
    // SHORT_CLICKED: a long-press (Details) must not also open the file on release.
    lv_obj_add_event_cb(row, row_click_cb, LV_EVENT_SHORT_CLICKED, (void *)(intptr_t)i);
    if (!en->dir) lv_obj_add_event_cb(row, row_details_cb, LV_EVENT_LONG_PRESSED, (void *)(intptr_t)i);

    lv_obj_t *ic = lv_label_create(row);
    lv_label_set_text(ic, nv_open_kind_symbol(en->dir ? NV_FILE_DIR : nv_open_kind(en->name)));
    lv_obj_set_style_text_color(ic, th->accent, 0);
    lv_obj_set_style_min_width(ic, 24, 0);

    lv_obj_t *nm = lv_label_create(row);
    lv_label_set_text(nm, en->name);
    lv_obj_set_style_text_font(nm, &nv_font_20, 0);
    lv_obj_set_style_text_color(nm, th->text, 0);
    lv_label_set_long_mode(nm, LV_LABEL_LONG_DOT);
    lv_obj_set_flex_grow(nm, 1);

    if (right && right[0]) {
        lv_obj_t *rt = lv_label_create(row);
        lv_label_set_text(rt, right);
        lv_obj_set_style_text_font(rt, &nv_font_14, 0);
        lv_obj_set_style_text_color(rt, th->text_dim, 0);
    }
    if (!en->dir) {   // explicit Details affordance (long-press is not discoverable)
        lv_obj_t *mb = lv_obj_create(row);
        lv_obj_remove_style_all(mb);
        lv_obj_set_size(mb, NV_TOUCH_MIN, NV_TOUCH_MIN);
        lv_obj_set_style_radius(mb, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(mb, th->surface3, LV_STATE_PRESSED);
        lv_obj_set_style_bg_opa(mb, LV_OPA_COVER, LV_STATE_PRESSED);
        lv_obj_clear_flag(mb, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(mb, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(mb, row_details_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        lv_obj_t *ml = lv_label_create(mb);
        lv_label_set_text(ml, LV_SYMBOL_BARS);
        lv_obj_set_style_text_color(ml, th->text_dim, 0);
        lv_obj_center(ml);
    }
    return row;
}

void build_list(void) {
    lv_obj_t *content = nv_ui_app_content();
    if (!content) return;
    lv_obj_clean(content);
    s_del_btn_label = nullptr;
    s_ren_ta = nullptr;
    s_page = Page::List;

    const bool at_root = !strcmp(s_path, kRoot);
    nv_ui_set_title(at_root ? nv_tr(NV_STR_APP_FILES) : strrchr(s_path, '/') + 1);
    nv_ui_set_back(at_root ? nullptr : back_from_list);

    lv_obj_t *c = nv_kit_scroll_column(content);
    const NvTheme *th = nv_theme_get();

    // header: current path + card free space
    lv_obj_t *head = lv_label_create(c);
    uint64_t total = 0, freeb = 0;
    char hd[256];
    if (nv_sd_info(&total, &freeb))
        snprintf(hd, sizeof hd, "%s   \xC2\xB7   %u / %u MB", s_path,
                 (unsigned)(freeb >> 20), (unsigned)(total >> 20));
    else
        snprintf(hd, sizeof hd, "%s", s_path);
    lv_label_set_text(head, hd);
    lv_obj_set_style_text_font(head, &nv_font_14, 0);
    lv_obj_set_style_text_color(head, th->text_dim, 0);

    if (!nv_sd_is_mounted()) {
        lv_obj_t *info = nv_kit_info(c);
        lv_label_set_text(info, nv_tr(NV_STR_SD_MISSING));
        lv_obj_set_style_text_color(info, th->text_dim, 0);
        return;
    }

    lv_obj_t *focus = nullptr;
    for (int i = 0; i < s_n; i++) {
        char right[24] = "";
        if (!s_ents[i].dir) fmt_size(s_ents[i].size, right, sizeof right);
        lv_obj_t *row = file_row(c, i, right);
        if (s_focus[0] && !strcmp(s_ents[i].name, s_focus)) focus = row;
    }
    if (s_n == 0) {
        lv_obj_t *info = nv_kit_info(c);
        lv_label_set_text(info, nv_tr(NV_STR_NONE));
        lv_obj_set_style_text_color(info, th->text_dim, 0);
    }
    if (s_overflow) {
        lv_obj_t *info = nv_kit_info(c);
        lv_label_set_text_fmt(info, "+%d ...", kMaxEntries);
        lv_obj_set_style_text_color(info, th->text_dim, 0);
    }
    // Coming back from an opened file (or a revealed one): mark its row and bring it into view.
    if (focus) {
        lv_obj_set_style_bg_color(focus, th->surface2, 0);
        lv_obj_set_style_border_side(focus, LV_BORDER_SIDE_LEFT, 0);
        lv_obj_set_style_border_width(focus, 3, 0);
        lv_obj_set_style_border_color(focus, th->accent, 0);
        lv_obj_update_layout(c);
        lv_obj_scroll_to_view(focus, LV_ANIM_OFF);
    }
    s_focus[0] = '\0';
}

// ---------------------------------------------------------------- Details page
void del_cb(lv_event_t *) {
    if (s_sel < 0 || s_sel >= s_n) return;
    if (!s_del_armed) {
        s_del_armed = true;
        if (s_del_btn_label) lv_label_set_text(s_del_btn_label, nv_tr(NV_STR_TAP_AGAIN));
        return;
    }
    char full[448];
    snprintf(full, sizeof full, "%s/%s", s_path, s_ents[s_sel].name);
    if (unlink(full) == 0) {
        nv_toast(NV_NOTE_OK, nv_tr(NV_STR_DELETE));
        s_sel_name[0] = '\0';
        scan_dir();
        nav_to(Page::List);
    } else {
        nv_toast(NV_NOTE_ERROR, nv_tr(NV_STR_SAVE_FAILED));
    }
}

void rename_cb(lv_event_t *) {
    if (s_sel < 0 || s_sel >= s_n || !s_ren_ta) return;
    const char *newname = lv_textarea_get_text(s_ren_ta);
    if (!newname || !newname[0] || strchr(newname, '/') || strlen(newname) >= (size_t)kNameMax) return;
    if (!strcmp(newname, s_ents[s_sel].name)) return;
    char oldp[448], newp[448];
    snprintf(oldp, sizeof oldp, "%s/%s", s_path, s_ents[s_sel].name);
    snprintf(newp, sizeof newp, "%s/%s", s_path, newname);
    if (rename(oldp, newp) == 0) {
        nv_toast(NV_NOTE_OK, nv_tr(NV_STR_SAVED));
        snprintf(s_focus, sizeof s_focus, "%s", newname);
        scan_dir();
        nav_to(Page::List);
    } else {
        nv_toast(NV_NOTE_ERROR, nv_tr(NV_STR_SAVE_FAILED));
    }
}

bool sel_path(char *out, size_t n) { return ent_path(s_sel, out, n); }

void open_default_cb(lv_event_t *) {
    char full[NV_OPEN_PATH_MAX];
    if (sel_path(full, sizeof full)) nv_open_file(full);
}
void open_with_cb(lv_event_t *) {
    char full[NV_OPEN_PATH_MAX];
    if (sel_path(full, sizeof full)) nv_open_with(full);
}
void action_cb(lv_event_t *e) {
    const NvOpenHandler *h = (const NvOpenHandler *)lv_event_get_user_data(e);
    char full[NV_OPEN_PATH_MAX];
    if (h && sel_path(full, sizeof full)) nv_open_run(h, full);
}

// "Label ........ value" line of the info card.
void info_line(lv_obj_t *card, nv_str_id_t label, const char *value) {
    const NvTheme *th = nv_theme_get();
    lv_obj_t *r = lv_obj_create(card);
    lv_obj_remove_style_all(r);
    lv_obj_set_size(r, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(r, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(r, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_column(r, NV_SP_4, 0);
    lv_obj_clear_flag(r, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *k = lv_label_create(r);
    lv_label_set_text(k, nv_tr(label));
    lv_obj_set_style_text_color(k, th->text_dim, 0);
    lv_obj_set_width(k, 160);
    lv_obj_t *v = lv_label_create(r);
    lv_label_set_text(v, value);
    lv_obj_set_style_text_color(v, th->text, 0);
    lv_label_set_long_mode(v, LV_LABEL_LONG_WRAP);
    lv_obj_set_flex_grow(v, 1);
}

lv_obj_t *button_row(lv_obj_t *parent) {
    lv_obj_t *r = lv_obj_create(parent);
    lv_obj_remove_style_all(r);
    lv_obj_set_size(r, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(r, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_column(r, NV_SP_2, 0);
    lv_obj_set_style_pad_row(r, NV_SP_2, 0);
    lv_obj_clear_flag(r, LV_OBJ_FLAG_SCROLLABLE);
    return r;
}

void build_detail(void) {
    lv_obj_t *content = nv_ui_app_content();
    if (!content) return;
    if (s_sel < 0 || s_sel >= s_n || strcmp(s_ents[s_sel].name, s_sel_name) != 0) {
        s_sel = find_ent(s_sel_name);   // the folder was rescanned since the selection
        if (s_sel < 0) { build_list(); return; }
    }
    lv_obj_clean(content);
    const Ent *en = &s_ents[s_sel];
    s_del_armed = false;
    s_page = Page::Detail;

    nv_ui_set_title(en->name);
    nv_ui_set_back(back_to_list);

    lv_obj_t *c = nv_kit_scroll_column(content);
    const NvTheme *th = nv_theme_get();

    char full[448];
    snprintf(full, sizeof full, "%s/%s", s_path, en->name);
    const bool openable_path = strlen(full) < NV_OPEN_PATH_MAX;
    const char *mime = nv_open_mime(en->name);
    const nv_file_kind_t kind = nv_open_kind_of_mime(mime);

    // hero: kind badge + name + "Kind · EXT"
    lv_obj_t *hero = lv_obj_create(c);
    lv_obj_remove_style_all(hero);
    lv_obj_set_size(hero, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(hero, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(hero, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(hero, NV_SP_4, 0);
    lv_obj_clear_flag(hero, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *badge = lv_obj_create(hero);
    lv_obj_remove_style_all(badge);
    lv_obj_set_size(badge, 64, 64);
    lv_obj_set_style_radius(badge, NV_RAD_MD, 0);
    lv_obj_set_style_bg_color(badge, th->accent, 0);
    lv_obj_set_style_bg_opa(badge, LV_OPA_20, 0);
    lv_obj_clear_flag(badge, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *bic = lv_label_create(badge);
    lv_label_set_text(bic, nv_open_kind_symbol(kind));
    lv_obj_set_style_text_font(bic, &nv_font_28, 0);   // pinned: hero glyph
    lv_obj_set_style_text_color(bic, th->accent, 0);
    lv_obj_center(bic);
    lv_obj_t *tcol = lv_obj_create(hero);
    lv_obj_remove_style_all(tcol);
    lv_obj_set_height(tcol, LV_SIZE_CONTENT);
    lv_obj_set_flex_grow(tcol, 1);
    lv_obj_set_flex_flow(tcol, LV_FLEX_FLOW_COLUMN);
    lv_obj_clear_flag(tcol, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *nm = lv_label_create(tcol);
    lv_label_set_text(nm, en->name);
    lv_obj_set_width(nm, lv_pct(100));
    lv_label_set_long_mode(nm, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(nm, &nv_font_20, 0);
    lv_obj_set_style_text_color(nm, th->text_strong, 0);
    const char *dot = strrchr(en->name, '.');
    char kl[48];
    if (dot && dot != en->name && dot[1]) {
        char ext[12] = "";
        for (int k = 0; dot[1 + k] && k < 11; k++) ext[k] = (char)toupper((unsigned char)dot[1 + k]);
        lv_snprintf(kl, sizeof kl, "%s \xC2\xB7 %s", nv_open_kind_label(kind), ext);
    } else {
        lv_snprintf(kl, sizeof kl, "%s", nv_open_kind_label(kind));
    }
    lv_obj_t *kd = lv_label_create(tcol);
    lv_label_set_text(kd, kl);
    lv_obj_set_style_text_font(kd, &nv_font_14, 0);
    lv_obj_set_style_text_color(kd, th->text_dim, 0);

    // open / actions
    const NvOpenHandler *ops[8], *acts[8];
    const int nops  = openable_path ? nv_open_handlers(full, NV_OPEN_OPENER, ops, 8) : 0;
    const int nacts = openable_path ? nv_open_handlers(full, NV_OPEN_ACTION, acts, 8) : 0;
    const NvOpenHandler *pref = nops ? nv_open_preferred(full) : nullptr;
    if (nops || nacts) {
        lv_obj_t *br = button_row(c);
        char lb[96];
        if (pref) {
            lv_snprintf(lb, sizeof lb, nv_tr(NV_STR_OPEN_WITH_FMT), nv_open_handler_label(pref));
            lv_obj_t *b = nv_kit_button(br, lb, true);
            lv_obj_add_event_cb(b, open_default_cb, LV_EVENT_CLICKED, nullptr);
        }
        if (nops >= 2 || (nops == 1 && !pref)) {
            lv_snprintf(lb, sizeof lb, "%s...", nv_tr(NV_STR_OPEN_WITH));
            lv_obj_t *b = nv_kit_button(br, lb, !pref);
            lv_obj_add_event_cb(b, open_with_cb, LV_EVENT_CLICKED, nullptr);
        }
        for (int k = 0; k < nacts; k++) {
            lv_snprintf(lb, sizeof lb, "%s  %s", nv_open_handler_symbol(acts[k], mime),
                        nv_open_handler_label(acts[k]));
            lv_obj_t *b = nv_kit_button(br, lb, false);
            lv_obj_add_event_cb(b, action_cb, LV_EVENT_CLICKED, (void *)acts[k]);
        }
    } else {
        lv_obj_t *none = nv_kit_info(c);
        lv_label_set_text(none, nv_tr(NV_STR_NO_APP_FOR_FILE));
        lv_obj_set_style_text_color(none, th->text_dim, 0);
    }

    // info card
    lv_obj_t *card = lv_obj_create(c);
    lv_obj_remove_style_all(card);
    lv_obj_set_size(card, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(card, th->surface, 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(card, NV_RAD_MD, 0);
    lv_obj_set_style_pad_all(card, NV_SP_4, 0);
    lv_obj_set_style_pad_row(card, NV_SP_2, 0);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    info_line(card, NV_STR_TYPE, mime);
    char sz[24];
    fmt_size(en->size, sz, sizeof sz);
    info_line(card, NV_STR_SIZE, sz);
    struct stat st{};
    char when[40] = "-";
    if (stat(full, &st) == 0 && st.st_mtime > 1672531200) {   // FAT without a set clock says 1980
        struct tm tmv{};
        const time_t t = st.st_mtime;
        localtime_r(&t, &tmv);
        strftime(when, sizeof when, nv_time_is_24h() ? "%d/%m/%Y  %H:%M" : "%d/%m/%Y  %I:%M %p", &tmv);
    }
    info_line(card, NV_STR_MODIFIED, when);
    info_line(card, NV_STR_LOCATION, s_path);

    // rename + delete
    s_ren_ta = nv_kit_textarea(c, en->name, true);
    lv_textarea_set_text(s_ren_ta, en->name);
    lv_obj_t *mr = button_row(c);
    lv_obj_t *rb = nv_kit_button(mr, nv_tr(NV_STR_RENAME), false);
    lv_obj_add_event_cb(rb, rename_cb, LV_EVENT_CLICKED, nullptr);
    lv_obj_t *db = nv_kit_button(mr, nv_tr(NV_STR_DELETE), false);
    lv_obj_set_style_text_color(lv_obj_get_child(db, 0), th->danger, 0);
    s_del_btn_label = lv_obj_get_child(db, 0);
    lv_obj_add_event_cb(db, del_cb, LV_EVENT_CLICKED, nullptr);
}

// ---------------------------------------------------------------- Preview page ("files.preview")
void preview_deleted(lv_event_t *) {
    if (s_text) { heap_caps_free(s_text); s_text = nullptr; }
}

// Read up to kPreviewMax bytes as displayable text: CR/LF -> LF, tabs -> spaces, other control
// bytes -> space. Returns false on a read error; *truncated when the file was longer.
bool load_text(const char *path, bool *truncated) {
    *truncated = false;
    FILE *f = nv_sd_fopen(path, "rb");
    if (!f) return false;
    s_text = (char *)heap_caps_malloc(kPreviewMax + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_text) { nv_sd_fclose(f); return false; }
    const size_t n = fread(s_text, 1, kPreviewMax, f);
    *truncated = (n == kPreviewMax) && fgetc(f) != EOF;
    nv_sd_fclose(f);
    size_t o = 0;
    for (size_t i = 0; i < n; i++) {
        char ch = s_text[i];
        if (ch == '\r') { if (i + 1 < n && s_text[i + 1] == '\n') continue; ch = '\n'; }
        else if (ch == '\t') ch = ' ';
        else if ((unsigned char)ch < 0x20 && ch != '\n') ch = ' ';
        else if (ch == 0x7F) ch = ' ';
        s_text[o++] = ch;
    }
    s_text[o] = '\0';
    return true;
}

void build_preview(void) {
    lv_obj_t *content = nv_ui_app_content();
    if (!content) return;
    lv_obj_clean(content);
    s_del_btn_label = nullptr;
    s_ren_ta = nullptr;
    s_page = Page::Preview;
    const NvTheme *th = nv_theme_get();

    const char *base = strrchr(s_preview, '/');
    nv_ui_set_title(base ? base + 1 : s_preview);
    nv_ui_set_back(back_from_preview);

    lv_obj_t *root = lv_obj_create(content);
    lv_obj_remove_style_all(root);
    lv_obj_set_size(root, lv_pct(100), lv_pct(100));
    lv_obj_clear_flag(root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(root, preview_deleted, LV_EVENT_DELETE, nullptr);

    if (nv_open_kind(s_preview) == NV_FILE_IMAGE) {
        lv_obj_set_style_bg_color(root, th->scrim, 0);
        lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);
        // LVGL's decoders match the extension case-sensitively (camera files are .JPG): lowercase
        // it in the source string only — FAT itself is case-insensitive.
        char src[NV_OPEN_PATH_MAX + 4];
        snprintf(src, sizeof src, "S:%s", s_preview);
        if (char *d = strrchr(src, '.'))
            for (char *p = d + 1; *p; ++p) *p = (char)tolower((unsigned char)*p);
        lv_image_header_t hdr;
        lv_memzero(&hdr, sizeof hdr);
        const bool ok = lv_image_decoder_get_info(src, &hdr) == LV_RESULT_OK &&
                        hdr.w > 0 && hdr.h > 0 && hdr.w <= kPreviewImgMax && hdr.h <= kPreviewImgMax;
        if (ok) {
            lv_obj_t *img = lv_image_create(root);
            lv_image_set_src(img, src);
            lv_obj_set_size(img, lv_pct(100), lv_pct(100));
            lv_image_set_inner_align(img, LV_IMAGE_ALIGN_CONTAIN);
            lv_obj_center(img);
        } else {
            lv_obj_t *l = lv_label_create(root);
            lv_label_set_text(l, nv_tr(NV_STR_IMAGE_UNAVAILABLE));
            lv_obj_set_style_text_color(l, th->on_primary, 0);
            lv_obj_center(l);
        }
        return;
    }

    lv_obj_t *c = nv_kit_scroll_column(root);
    bool truncated = false;
    if (!load_text(s_preview, &truncated)) {
        lv_obj_t *info = nv_kit_info(c);
        lv_label_set_text(info, nv_tr(NV_STR_OPEN_FAILED));
        lv_obj_set_style_text_color(info, th->text_dim, 0);
        return;
    }
    if (truncated) {
        lv_obj_t *info = nv_kit_info(c);
        lv_label_set_text_fmt(info, nv_tr(NV_STR_PREVIEW_TRUNC_FMT), (unsigned)(kPreviewMax / 1024));
        lv_obj_set_style_text_color(info, th->text_dim, 0);
    }
    lv_obj_t *txt = lv_label_create(c);
    lv_obj_set_width(txt, lv_pct(100));
    lv_label_set_long_mode(txt, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_color(txt, th->text, 0);
    // Static text: the 32 KB buffer stays in PSRAM (freed with the page) instead of being copied
    // into LVGL's own heap pool.
    lv_label_set_text_static(txt, s_text);
}

// ---------------------------------------------------------------- app plumbing
void files_deleted(lv_event_t *) {
    lv_async_call_cancel(nav_apply, nullptr);   // a queued navigation must not run on the next app's tree
    s_del_btn_label = nullptr;
    s_ren_ta = nullptr;
    s_nav_pending = false;
    s_open = false;
    s_content = nullptr;
}

void files_build(lv_obj_t *content) {
    if (!s_ents) {
        s_ents = (Ent *)heap_caps_calloc(kMaxEntries, sizeof(Ent),
                                         MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!s_ents) s_ents = (Ent *)calloc(kMaxEntries, sizeof(Ent));
        if (!s_ents) return;
    }
    const bool rebuild = s_open && content == s_content;
    if (!rebuild) {   // a fresh open starts at the root
        s_open = true;
        s_content = content;
        lv_obj_add_event_cb(content, files_deleted, LV_EVENT_DELETE, nullptr);
        strcpy(s_path, kRoot);
        select_ent(-1);
        s_focus[0] = '\0';
        s_page = Page::List;
        s_seen.valid = false;
    }
    lv_async_call_cancel(nav_apply, nullptr);   // a nav queued before the rebuild targets dead widgets
    s_nav_pending = false;
    if (const NvIntent *in = nv_open_intent())
        if (!intent_seen(in)) apply_intent(in);
    scan_dir();
    render(s_page);   // build() runs outside any content event: safe to build now
}

const NvApp kFilesApp = {"files", "Files", &nv_icon_files, 1u << 20, files_build,
                         NV_STR_APP_FILES, nullptr};

// Files' read-only viewer as a system opener: text of any kind and the image formats LVGL decodes.
// Priority 10 — the real editors / viewers (Notes 50, Gallery 50) stay the default.
const NvOpenHandler kPreviewHandler = {
    "files.preview", "files", "text/* application/json application/xml image/jpeg image/png image/bmp",
    NV_STR_PREVIEW, nullptr, LV_SYMBOL_EYE_OPEN, NV_OPEN_OPENER, 10, nullptr, nullptr,
};

}  // namespace

void files_app_register(void) {
    nv_app_register(&kFilesApp);
    nv_open_register(&kPreviewHandler);
}
