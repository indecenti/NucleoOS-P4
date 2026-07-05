// files_app — SD file manager (browse / image viewer / rename / delete).
//   List   : one directory at a time from /sdcard (bounded scan, dirs first, alpha sort).
//   Detail : file info + rename (IME textarea) + two-step delete; images get an Open button.
//   Viewer : full-screen lv_image via the LVGL 'S' FS driver (same stack the Gallery uses).
// The system Back button walks the tree up (nv_ui_set_back) and closes the app only at the
// root. Page switches are DEFERRED via lv_async_call (gallery pattern): builders clean the
// content subtree that fired the event, so the rebuild must wait for the event to unwind.
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

#include "esp_heap_caps.h"

#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cctype>

namespace {

constexpr int kMaxEntries = 200;
constexpr int kNameMax    = 64;

struct Ent {
    char     name[kNameMax];
    uint32_t size;
    bool     dir;
};

Ent *s_ents = nullptr;   // PSRAM, allocated once
int  s_n = 0;
bool s_overflow = false;
char s_path[192] = "/sdcard";
int  s_sel = -1;         // index into s_ents for the Detail page

enum class Page { List, Detail, Viewer };
Page s_pending = Page::List;
bool s_nav_pending = false;

lv_obj_t *s_del_btn_label = nullptr;   // two-step delete state (Detail page)
bool      s_del_armed = false;
lv_obj_t *s_ren_ta = nullptr;          // rename textarea (Detail page)

void build_list(void);
void build_detail(void);
void build_viewer(void);

void nav_apply(void *) {
    s_nav_pending = false;
    switch (s_pending) {
        case Page::List:   build_list();   break;
        case Page::Detail: build_detail(); break;
        case Page::Viewer: build_viewer(); break;
    }
}
void nav_to(Page p) {
    s_pending = p;
    if (!s_nav_pending && lv_async_call(nav_apply, nullptr) == LV_RESULT_OK)
        s_nav_pending = true;
}

// ---------------------------------------------------------------- model
bool is_image(const char *name) {
    const char *dot = strrchr(name, '.');
    if (!dot) return false;
    char ext[6] = "";
    for (int i = 0; dot[1 + i] && i < 5; i++) ext[i] = (char)tolower((unsigned char)dot[1 + i]);
    return !strcmp(ext, "jpg") || !strcmp(ext, "jpeg") || !strcmp(ext, "png") || !strcmp(ext, "bmp");
}

int ent_cmp(const void *a, const void *b) {
    const Ent *x = (const Ent *)a, *y = (const Ent *)b;
    if (x->dir != y->dir) return x->dir ? -1 : 1;   // dirs first
    return strcasecmp(x->name, y->name);
}

void scan_dir(void) {
    s_n = 0;
    s_overflow = false;
    if (!s_ents || !nv_sd_is_mounted()) return;
    DIR *d = opendir(s_path);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d)) != nullptr) {
        if (e->d_name[0] == '.') continue;
        if (s_n >= kMaxEntries) { s_overflow = true; break; }
        Ent *en = &s_ents[s_n];
        snprintf(en->name, sizeof en->name, "%.*s", kNameMax - 1, e->d_name);  // clip long names
        char full[448];
        snprintf(full, sizeof full, "%s/%s", s_path, e->d_name);
        struct stat st{};
        stat(full, &st);
        en->dir = S_ISDIR(st.st_mode);
        en->size = en->dir ? 0 : (uint32_t)st.st_size;
        s_n++;
    }
    closedir(d);
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

void sel_full_path(char *out, size_t n) {
    snprintf(out, n, "%s/%s", s_path, s_sel >= 0 && s_sel < s_n ? s_ents[s_sel].name : "");
}

// ---------------------------------------------------------------- back handling
void back_from_list(void) {
    char *slash = strrchr(s_path, '/');
    if (!slash || slash == s_path + 7 - 7 + 0 || !strcmp(s_path, "/sdcard")) return;  // unreachable at root
    *slash = '\0';
    scan_dir();
    nav_to(Page::List);
}
void back_to_list(void) { nav_to(Page::List); }
void back_to_detail(void) { nav_to(Page::Detail); }

// ---------------------------------------------------------------- List page
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
        scan_dir();
        nav_to(Page::List);
    } else {
        s_sel = i;
        nav_to(Page::Detail);
    }
}

lv_obj_t *file_row(lv_obj_t *col, const char *sym, const char *name, const char *right,
                   lv_event_cb_t cb, void *ud) {
    const NvTheme *th = nv_theme_get();
    lv_obj_t *row = lv_obj_create(col);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(row, th->surface, 0);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(row, NV_RAD_MD, 0);
    lv_obj_set_style_pad_hor(row, NV_SP_4, 0);
    lv_obj_set_style_pad_ver(row, NV_SP_3, 0);
    lv_obj_set_style_min_height(row, NV_TOUCH_MIN, 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, NV_SP_3, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_color(row, th->surface2, LV_STATE_PRESSED);
    if (cb) lv_obj_add_event_cb(row, cb, LV_EVENT_CLICKED, ud);

    lv_obj_t *ic = lv_label_create(row);
    lv_label_set_text(ic, sym);
    lv_obj_set_style_text_color(ic, th->accent, 0);

    lv_obj_t *nm = lv_label_create(row);
    lv_label_set_text(nm, name);
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
    return row;
}

void build_list(void) {
    lv_obj_t *content = nv_ui_app_content();
    if (!content) return;
    lv_obj_clean(content);
    s_del_btn_label = nullptr;
    s_ren_ta = nullptr;

    const bool at_root = !strcmp(s_path, "/sdcard");
    nv_ui_set_title(at_root ? nv_tr(NV_STR_APP_FILES) : strrchr(s_path, '/') + 1);
    nv_ui_set_back(at_root ? nullptr : back_from_list);

    lv_obj_t *c = nv_kit_scroll_column(content);
    const NvTheme *th = nv_theme_get();

    // header: current path + card free space
    lv_obj_t *head = lv_label_create(c);
    uint64_t total = 0, freeb = 0;
    char hd[256];
    if (nv_sd_info(&total, &freeb))
        snprintf(hd, sizeof hd, "%s   ·   %u / %u MB", s_path,
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

    for (int i = 0; i < s_n; i++) {
        char right[24] = "";
        if (!s_ents[i].dir) fmt_size(s_ents[i].size, right, sizeof right);
        file_row(c, s_ents[i].dir ? LV_SYMBOL_DIRECTORY : LV_SYMBOL_FILE,
                 s_ents[i].name, right, row_click_cb, (void *)(intptr_t)i);
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
}

// ---------------------------------------------------------------- Detail page
void del_cb(lv_event_t *) {
    if (s_sel < 0 || s_sel >= s_n) return;
    if (!s_del_armed) {
        s_del_armed = true;
        if (s_del_btn_label) lv_label_set_text(s_del_btn_label, nv_tr(NV_STR_TAP_AGAIN));
        return;
    }
    char full[448];
    sel_full_path(full, sizeof full);
    if (unlink(full) == 0) {
        nv_toast(NV_NOTE_OK, nv_tr(NV_STR_DELETE));
        scan_dir();
        nav_to(Page::List);
    } else {
        nv_toast(NV_NOTE_ERROR, nv_tr(NV_STR_SAVE_FAILED));
    }
}

void rename_cb(lv_event_t *) {
    if (s_sel < 0 || s_sel >= s_n || !s_ren_ta) return;
    const char *newname = lv_textarea_get_text(s_ren_ta);
    if (!newname || !newname[0] || strchr(newname, '/')) return;
    char oldp[448], newp[448];
    sel_full_path(oldp, sizeof oldp);
    snprintf(newp, sizeof newp, "%s/%s", s_path, newname);
    if (rename(oldp, newp) == 0) {
        nv_toast(NV_NOTE_OK, nv_tr(NV_STR_SAVED));
        scan_dir();
        nav_to(Page::List);
    } else {
        nv_toast(NV_NOTE_ERROR, nv_tr(NV_STR_SAVE_FAILED));
    }
}

void open_img_cb(lv_event_t *) { nav_to(Page::Viewer); }

void build_detail(void) {
    lv_obj_t *content = nv_ui_app_content();
    if (!content || s_sel < 0 || s_sel >= s_n) { build_list(); return; }
    lv_obj_clean(content);
    const Ent *en = &s_ents[s_sel];
    s_del_armed = false;

    nv_ui_set_title(en->name);
    nv_ui_set_back(back_to_list);

    lv_obj_t *c = nv_kit_scroll_column(content);
    const NvTheme *th = nv_theme_get();

    char full[448], sz[24];
    sel_full_path(full, sizeof full);
    fmt_size(en->size, sz, sizeof sz);
    lv_obj_t *info = nv_kit_info(c);
    lv_label_set_text_fmt(info, "%s\n%s", full, sz);
    lv_obj_set_style_text_color(info, th->text_dim, 0);

    if (is_image(en->name)) {
        lv_obj_t *ob = nv_kit_button(c, nv_tr(NV_STR_OPEN), true);
        lv_obj_add_event_cb(ob, open_img_cb, LV_EVENT_CLICKED, nullptr);
    }

    s_ren_ta = nv_kit_textarea(c, en->name, true);
    lv_textarea_set_text(s_ren_ta, en->name);
    lv_obj_t *rb = nv_kit_button(c, nv_tr(NV_STR_RENAME), false);
    lv_obj_add_event_cb(rb, rename_cb, LV_EVENT_CLICKED, nullptr);

    lv_obj_t *db = nv_kit_button(c, nv_tr(NV_STR_DELETE), false);
    lv_obj_set_style_text_color(lv_obj_get_child(db, 0), th->danger, 0);
    s_del_btn_label = lv_obj_get_child(db, 0);
    lv_obj_add_event_cb(db, del_cb, LV_EVENT_CLICKED, nullptr);
}

// ---------------------------------------------------------------- Viewer page (images)
void viewer_click_cb(lv_event_t *) { nav_to(Page::Detail); }

void build_viewer(void) {
    lv_obj_t *content = nv_ui_app_content();
    if (!content || s_sel < 0 || s_sel >= s_n) { build_list(); return; }
    lv_obj_clean(content);
    s_del_btn_label = nullptr;
    s_ren_ta = nullptr;

    nv_ui_set_title(s_ents[s_sel].name);
    nv_ui_set_back(back_to_detail);

    lv_obj_t *scrim = lv_obj_create(content);
    lv_obj_remove_style_all(scrim);
    lv_obj_set_size(scrim, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_color(scrim, nv_theme_get()->scrim, 0);
    lv_obj_set_style_bg_opa(scrim, LV_OPA_COVER, 0);
    lv_obj_add_flag(scrim, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(scrim, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(scrim, viewer_click_cb, LV_EVENT_CLICKED, nullptr);

    char src[456];
    char full[448];
    sel_full_path(full, sizeof full);
    snprintf(src, sizeof src, "S:%s", full);   // LVGL FS letter 'S' -> VFS root
    lv_obj_t *img = lv_image_create(scrim);
    lv_image_set_src(img, src);
    lv_obj_center(img);
    lv_image_set_inner_align(img, LV_IMAGE_ALIGN_CONTAIN);
    lv_obj_set_size(img, lv_pct(100), lv_pct(100));
}

// ---------------------------------------------------------------- app plumbing
void files_deleted(lv_event_t *) {
    s_del_btn_label = nullptr;
    s_ren_ta = nullptr;
    s_nav_pending = false;
}

void files_build(lv_obj_t *content) {
    if (!s_ents) {
        s_ents = (Ent *)heap_caps_calloc(kMaxEntries, sizeof(Ent),
                                         MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!s_ents) s_ents = (Ent *)calloc(kMaxEntries, sizeof(Ent));
        if (!s_ents) return;
    }
    strcpy(s_path, "/sdcard");
    s_sel = -1;
    lv_obj_add_event_cb(content, files_deleted, LV_EVENT_DELETE, nullptr);
    scan_dir();
    build_list();
}

const NvApp kFilesApp = {"files", "Files", &nv_icon_files, 1u << 20, files_build,
                         NV_STR_APP_FILES, nullptr};

}  // namespace

void files_app_register(void) { nv_app_register(&kFilesApp); }
