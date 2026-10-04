// files_app — the system file browser (SD card + USB drives).
//   Places  : the volumes — microSD and every USB LUN (/usb0..) with label, file system, free
//             space or state (no card / ejected / unformatted) and an Eject button. Shown as the top
//             level while any USB device is attached; without one Files opens straight on /sdcard.
//   List    : one directory at a time (bounded scan, dirs first, alpha sort). File rows show the
//             kind glyph + size. Tap = Open through nv_open (the default app, or the "Open with"
//             sheet); long-press or the row's menu button = Details. A paste bar appears while a
//             Copy/Move is pending, a progress bar while one runs (files_ops, own task).
//   Details : kind, MIME, size, modified, location; "Open with <app>", "Open with...", the file's
//             system actions (e.g. Set as wallpaper), copy / move, rename, two-step delete.
//             Folders get the same page minus the openers (delete is recursive, in the background).
//   Preview : Files' own read-only viewer for text and images, registered as the "files.preview"
//             opener — the safe way to look at a config or a log that no editor should autosave.
// Intents (nv_open.h): RESUME / REVEAL land on the file's folder with its row highlighted and in
// view; OPEN through "files.preview" shows the Preview page. A theme / language rebuild keeps the
// current folder and page (same content object = same instance).
// The system Back button walks the tree up (nv_ui_set_back_handler) and closes the app only at the root.
// Page switches are DEFERRED via lv_async_call (gallery pattern): builders clean the content
// subtree that fired the event, so the rebuild must wait for the event to unwind.
#include "nv_ui_select.h"   // system selection + context menus
#include "nv_ui_focus.h"   // keyboard: Backspace = up a folder
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
#include "nv_usb_storage.h"
#include "ff.h"              // f_readdir: one pass per folder on the card
#include "nv_bgwork.h"
#include "files_ops.h"
#include "nv_clipboard.h"   // paste a screenshot / copied text as a new file

#include "esp_heap_caps.h"
#include "esp_lvgl_port.h"   // lvgl_port_lock: eject result posted from the bg worker

#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cctype>
#include <ctime>

namespace {

constexpr char   kRoot[]        = "/sdcard";   // default volume (Places lists the others)
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

enum class Page { Places, List, Detail, Preview };
Page s_page = Page::List;      // page on screen (a rebuild re-renders it)
Page s_pending = Page::List;
bool s_nav_pending = false;

lv_obj_t *s_del_btn_label = nullptr;   // two-step delete state (Details page)
bool      s_del_armed = false;
lv_obj_t *s_ren_ta = nullptr;          // rename textarea (Details page)

// Pending Copy/Move ("clipboard") — survives folder changes, cleared by paste/cancel/close.
// n > 0: a multi-item clipboard, `path` is their folder and s_mnames their names.
struct Clip { bool set; FopKind kind; char path[NV_OPEN_PATH_MAX]; int n; };
NV_PSRAM_BSS Clip s_clip;
constexpr int kMultiMax = 64;
NV_PSRAM_BSS char s_mnames[kMultiMax][kNameMax];
// Several items (multi-select paste / delete) = one file-ops job after the other, from the tick.
struct Batch { FopKind kind; int n, i; char src[NV_OPEN_PATH_MAX]; char dst[NV_OPEN_PATH_MAX]; };
NV_PSRAM_BSS Batch s_batch;
NV_PSRAM_BSS char s_bnames[kMultiMax][kNameMax];
NV_PSRAM_BSS lv_obj_t *s_list;          // the folder listing (selection container)
NV_PSRAM_BSS lv_point_t s_ctx_pt;                  // where the last context menu opened

// Live refresh (USB hot-plug, card swap, finished copy) — 1 s timer while Files is open.
lv_timer_t *s_tick = nullptr;
uint32_t    s_usb_gen = 0, s_fop_gen = 0;
lv_obj_t   *s_prog_label = nullptr;    // progress row in the list (nullptr when not shown)
lv_obj_t   *s_prog_bar = nullptr;

// One Files instance = one content object between open and close. build() re-runs on a theme /
// language refresh or when nv_open re-delivers an intent: same object -> keep the folder.
lv_obj_t *s_content = nullptr;
bool      s_open = false;

// The last intent acted on, so a plain rebuild does not replay it (a NEW intent delivered in place
// — e.g. "Preview" chosen from our own Details — differs and is applied).
struct Seen { bool valid; uint8_t verb; char handler[NV_OPEN_ID_MAX]; char path[NV_OPEN_PATH_MAX]; };
NV_PSRAM_BSS Seen s_seen;

void build_places(void);
void build_list(void);
lv_obj_t *button_row(lv_obj_t *parent);
void build_detail(void);
void build_preview(void);

void render(Page p) {
    switch (p) {
        case Page::Places:  build_places();  break;
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

// ---- volumes: "/sdcard" or "/usbN". Length of the root prefix of `p`, 0 when it has none.
size_t root_len(const char *p) {
    if (!p) return 0;
    if (!strncmp(p, kRoot, sizeof kRoot - 1) && (p[sizeof kRoot - 1] == 0 || p[sizeof kRoot - 1] == '/'))
        return sizeof kRoot - 1;
    return nv_usb_storage_slot_of(p) >= 0 ? 5 : 0;   // "/usbN"
}
bool at_places(void) { return s_path[0] == '\0'; }
int  cur_usb(void) { return nv_usb_storage_slot_of(s_path); }
bool usb_attached(void) {
    nv_usb_stor_info_t v[NV_USB_STOR_SLOTS];
    return nv_usb_storage_list(v, NV_USB_STOR_SLOTS) > 0;
}
bool vol_mounted(void) {
    const int u = cur_usb();
    if (u < 0) return nv_sd_is_mounted();
    nv_usb_stor_info_t v;
    return nv_usb_storage_get(u, &v) && v.state == NV_USB_STOR_MOUNTED;
}
bool vol_begin(void) { const int u = cur_usb(); return u < 0 ? nv_sd_session_begin() : nv_usb_storage_session_begin(u); }
void vol_end(void)   { const int u = cur_usb(); if (u < 0) nv_sd_session_end(); else nv_usb_storage_session_end(u); }
bool vol_info(uint64_t *total, uint64_t *freeb) {
    const int u = cur_usb();
    if (u < 0) return nv_sd_info(total, freeb);
    nv_usb_stor_info_t v;
    if (!nv_usb_storage_get(u, &v) || v.state != NV_USB_STOR_MOUNTED || v.free_bytes == UINT64_MAX) return false;
    *total = v.total_bytes;
    *freeb = v.free_bytes;
    return true;
}

// "812 MB", "14.6 GB".
void fmt_bytes(uint64_t b, char *out, size_t n) {
    if (b >= (1ull << 30)) snprintf(out, n, "%u.%u GB", (unsigned)(b >> 30), (unsigned)(((b >> 20) & 1023) * 10 >> 10));
    else snprintf(out, n, "%u MB", (unsigned)(b >> 20));
}

// Human name of a USB volume: its label, else the device's product string.
void usb_name(const nv_usb_stor_info_t &v, char *out, size_t n) {
    if (v.label[0]) snprintf(out, n, "%s", v.label);
    else if (v.product[0]) snprintf(out, n, "%s", v.product);
    else snprintf(out, n, "%s", nv_tr(NV_STR_USB_DRIVE));
}

// The card: one f_readdir pass gives every entry's name, size and kind. readdir + stat() per entry
// made FAT rescan the folder for each stat (O(n^2): /sdcard/apps with 150+ apps froze the UI).
bool scan_dir_fatfs(void) {
    char fp[208];
    if (!nv_sd_fatfs_path(s_path, fp, sizeof fp)) return false;
    FF_DIR d;
    if (f_opendir(&d, fp) != FR_OK) return false;
    FILINFO fi;
    while (f_readdir(&d, &fi) == FR_OK && fi.fname[0]) {
        if (fi.fname[0] == '.') continue;               // as the readdir path: hidden = dot-names
        const size_t nl = strlen(fi.fname);
        if (nl >= (size_t)kNameMax) continue;
        if (s_n >= kMaxEntries) { s_overflow = true; break; }
        Ent *en = &s_ents[s_n];
        memcpy(en->name, fi.fname, nl + 1);
        en->dir = (fi.fattrib & AM_DIR) != 0;
        en->size = en->dir ? 0 : (uint32_t)fi.fsize;
        s_n++;
    }
    f_closedir(&d);
    return true;
}

void scan_dir(void) {
    s_n = 0;
    s_overflow = false;
    if (!s_ents || at_places() || !vol_mounted() || !vol_begin()) return;
    if (scan_dir_fatfs()) {
        vol_end();
        qsort(s_ents, (size_t)s_n, sizeof(Ent), ent_cmp);
        return;
    }
    s_n = 0;
    s_overflow = false;
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
    vol_end();
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

// Point the browser at `dir` (a volume root or below); anything else falls back to /sdcard.
void set_folder(const char *dir) {
    const size_t rl = root_len(dir);
    if (!rl || strlen(dir) >= sizeof s_path) {
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
    if (strlen(s_path) <= root_len(s_path)) {                          // volume root -> Places
        s_path[0] = '\0';
        nav_to(Page::Places);
        return;
    }
    char *slash = strrchr(s_path, '/');
    if (!slash || slash == s_path) return;
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
    // Mouse: click selects (Ctrl / Shift / drag for more), double click opens. Finger, Enter: open.
    if (nv_sel_click(e)) return;
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
    if (nv_sel_mouse()) return;   // a held mouse button is a band drag; right click = menu
    const int i = (int)(intptr_t)lv_event_get_user_data(e);
    if (i < 0 || i >= s_n) return;
    select_ent(i);
    nav_to(Page::Detail);
}

void row_ctx_cb(lv_event_t *e);   // context menus (below)
void list_ctx_cb(lv_event_t *e);
bool files_key(uint32_t key, uint8_t usage, uint8_t mods);
// The list page's shortcuts for the F1 sheet (files_key implements them).
const nv_shortcut_t kListKeys[] = {
    {"Backspace / Alt+Freccia su", "Cartella superiore", "Parent folder"},
    {"Canc", "Elimina", "Delete"},
    {"F2", "Rinomina / dettagli", "Rename / details"},
    {"F5", "Aggiorna", "Refresh"},
    {"Ctrl+A", "Seleziona tutto", "Select all"},
    {"Ctrl+C / X / V", "Copia / taglia / incolla", "Copy / cut / paste"},
};

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
    lv_obj_set_style_pad_right(row, NV_SP_1, 0);
    lv_obj_set_style_pad_ver(row, NV_SP_3, 0);
    lv_obj_set_style_min_height(row, NV_TOUCH_MIN, 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, NV_SP_3, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_color(row, th->surface2, LV_STATE_PRESSED);
    nv_sel_item(row);                                         // hover, selection, band drag
    lv_obj_set_user_data(row, (void *)(intptr_t)(i + 1));
    lv_obj_add_event_cb(row, row_ctx_cb, (lv_event_code_t)nv_ui_event_context(), nullptr);
    // SHORT_CLICKED: a long-press (Details) must not also open the file on release.
    lv_obj_add_event_cb(row, row_click_cb, LV_EVENT_SHORT_CLICKED, (void *)(intptr_t)i);
    lv_obj_add_event_cb(row, row_details_cb, LV_EVENT_LONG_PRESSED, (void *)(intptr_t)i);

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
    {   // explicit Details affordance (long-press is not discoverable; folders too: copy/move)
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

// ---------------------------------------------------------------- copy / move / paste / progress
const char *base_name(const char *p) {
    const char *b = strrchr(p, '/');
    return b ? b + 1 : p;
}

void fop_toast(FopResult r) {
    if (r == FOP_BUSY) nv_toast(NV_NOTE_WARN, nv_tr(NV_STR_FILEOP_BUSY));
    else if (r == FOP_INTO_SELF) nv_toast(NV_NOTE_WARN, nv_tr(NV_STR_FILEOP_INTO_SELF));
    else if (r != FOP_OK) nv_toast(NV_NOTE_ERROR, nv_tr(NV_STR_FILEOP_FAILED));
}

void batch_next(void) {
    while (s_batch.i < s_batch.n) {
        char src[448];
        snprintf(src, sizeof src, "%s/%s", s_batch.src, s_bnames[s_batch.i]);
        const FopResult r = fop_start(s_batch.kind, src, s_batch.kind == FOP_DELETE ? nullptr : s_batch.dst);
        if (r == FOP_BUSY) return;    // the tick tries again
        s_batch.i++;
        if (r == FOP_OK) return;      // queued: the next one when it is done
    }
}
// Names already in s_bnames.
void batch_begin(FopKind k, const char *src, const char *dst, int n) {
    s_batch.kind = k;
    snprintf(s_batch.src, sizeof s_batch.src, "%s", src);
    snprintf(s_batch.dst, sizeof s_batch.dst, "%s", dst ? dst : "");
    s_batch.n = n;
    s_batch.i = 0;
    batch_next();
}
bool batch_busy(void) {
    if (s_batch.i >= s_batch.n) return false;
    nv_toast(NV_NOTE_WARN, nv_tr(NV_STR_FILEOP_BUSY));
    return true;
}

// Ctrl+V with no file copy pending: what the SYSTEM clipboard holds becomes a new file here, as on a
// desktop OS — an image (a screenshot) as "Immagine incollata <date>.jpg", text as "Testo incollato
// <date>.txt". A name already taken gets " (2)", " (3)"...
bool sys_clip_pastable(void) {
    const nv_clip_kind_t k = nv_clip_kind();
    char f[NV_CLIP_PATH_MAX];
    return k == NV_CLIP_TEXT || (k == NV_CLIP_IMAGE && nv_clip_image_file(f, sizeof f));
}

bool copy_file_raw(const char *src, const char *dst) {
    FILE *in = fopen(src, "rb");
    if (!in) return false;
    FILE *out = nv_sd_fopen(dst, "wb");
    if (!out) { fclose(in); return false; }
    char *buf = (char *)heap_caps_malloc(16 * 1024, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    bool ok = buf != nullptr;
    size_t n;
    while (ok && (n = fread(buf, 1, 16 * 1024, in)) > 0) ok = fwrite(buf, 1, n, out) == n;
    heap_caps_free(buf);
    fclose(in);
    if (nv_sd_fclose(out) != 0) ok = false;
    if (!ok) remove(dst);
    return ok;
}

void paste_system_clipboard(void) {
    const nv_clip_kind_t k = nv_clip_kind();
    const bool it = nv_i18n_get_lang() == NV_LANG_IT;
    time_t t = time(nullptr);
    struct tm tmv;
    localtime_r(&t, &tmv);
    char stamp[24];
    strftime(stamp, sizeof stamp, "%Y-%m-%d %H.%M.%S", &tmv);
    const char *stem = k == NV_CLIP_IMAGE ? (it ? "Immagine incollata" : "Pasted image") : (it ? "Testo incollato" : "Pasted text");
    const char *ext = k == NV_CLIP_IMAGE ? ".jpg" : ".txt";
    char dst[NV_OPEN_PATH_MAX];
    snprintf(dst, sizeof dst, "%s/%s %s%s", s_path, stem, stamp, ext);
    struct stat st;
    for (int i = 2; stat(dst, &st) == 0 && i < 100; i++) snprintf(dst, sizeof dst, "%s/%s %s (%d)%s", s_path, stem, stamp, i, ext);
    bool ok = false;
    if (k == NV_CLIP_IMAGE) {
        char src[NV_CLIP_PATH_MAX];
        ok = nv_clip_image_file(src, sizeof src) && copy_file_raw(src, dst);
    } else if (k == NV_CLIP_TEXT) {
        char *txt = nv_clip_get_text();
        FILE *f = txt ? nv_sd_fopen(dst, "wb") : nullptr;
        if (f) {
            ok = fwrite(txt, 1, strlen(txt), f) == strlen(txt);
            if (nv_sd_fclose(f) != 0) ok = false;
        }
        free(txt);
    }
    if (ok) {
        char b[160];
        snprintf(b, sizeof b, it ? "Creato \"%s\"" : "Created \"%s\"", strrchr(dst, '/') + 1);
        nv_toast(NV_NOTE_OK, b);
    } else {
        nv_toast(NV_NOTE_WARN, it ? "Niente da incollare, o scrittura non riuscita" : "Nothing to paste, or the write failed");
    }
    nav_to(Page::List);
}

void paste_cb(lv_event_t *) {
    if (!s_clip.set) {                   // no file copy/move pending: the system clipboard
        if (sys_clip_pastable()) paste_system_clipboard();
        return;
    }
    if (s_clip.n > 0) {
        if (batch_busy()) return;
        if (!(s_clip.kind == FOP_MOVE && !strcmp(s_clip.path, s_path))) {
            memcpy(s_bnames, s_mnames, (size_t)s_clip.n * kNameMax);
            batch_begin(s_clip.kind, s_clip.path, s_path, s_clip.n);
        }
        s_clip.set = false;
        nav_to(Page::List);
        return;
    }
    char parent[NV_OPEN_PATH_MAX];
    snprintf(parent, sizeof parent, "%s", s_clip.path);
    if (char *sl = strrchr(parent, '/')) *sl = '\0';
    if (s_clip.kind == FOP_MOVE && !strcmp(parent, s_path)) {   // moving onto itself: nothing to do
        s_clip.set = false;
        nav_to(Page::List);
        return;
    }
    const FopResult r = fop_start(s_clip.kind, s_clip.path, s_path);
    fop_toast(r);
    if (r == FOP_OK || r == FOP_INTO_SELF) s_clip.set = false;
    nav_to(Page::List);
}
void clip_clear_cb(lv_event_t *) {
    s_clip.set = false;
    nav_to(Page::List);
}
void fop_cancel_cb(lv_event_t *) { fop_cancel(); }

void progress_update(void) {
    if (!s_prog_label) return;
    FopStatus st;
    fop_status(&st);
    char t[128];
    if (st.kind == FOP_DELETE)
        lv_snprintf(t, sizeof t, "%s  %s", nv_tr(NV_STR_DELETE), st.current);
    else
        lv_snprintf(t, sizeof t, nv_tr(st.kind == FOP_MOVE ? NV_STR_MOVING_FMT : NV_STR_COPYING_FMT), st.pct, st.current);
    lv_label_set_text(s_prog_label, t);
    if (s_prog_bar) lv_bar_set_value(s_prog_bar, (int32_t)st.pct, LV_ANIM_OFF);
}

// Progress row (running job) + paste bar (pending clipboard) at the top of a folder listing.
void fileop_bars(lv_obj_t *c) {
    const NvTheme *th = nv_theme_get();
    s_prog_label = nullptr;
    s_prog_bar = nullptr;
    FopStatus st;
    fop_status(&st);
    if (st.busy) {
        lv_obj_t *r = lv_obj_create(c);
        lv_obj_remove_style_all(r);
        lv_obj_set_size(r, lv_pct(100), LV_SIZE_CONTENT);
        lv_obj_set_style_bg_color(r, th->surface, 0);
        lv_obj_set_style_bg_opa(r, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(r, NV_RAD_MD, 0);
        lv_obj_set_style_pad_all(r, NV_SP_3, 0);
        lv_obj_set_style_pad_row(r, NV_SP_2, 0);
        lv_obj_set_flex_flow(r, LV_FLEX_FLOW_COLUMN);
        lv_obj_clear_flag(r, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_t *top = button_row(r);
        lv_obj_set_flex_flow(top, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(top, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        s_prog_label = lv_label_create(top);
        lv_obj_set_flex_grow(s_prog_label, 1);
        lv_label_set_long_mode(s_prog_label, LV_LABEL_LONG_DOT);
        lv_obj_set_style_text_color(s_prog_label, th->text, 0);
        lv_obj_t *x = nv_kit_button(top, nv_tr(NV_STR_CANCEL), false);
        lv_obj_add_event_cb(x, fop_cancel_cb, LV_EVENT_CLICKED, nullptr);
        s_prog_bar = lv_bar_create(r);
        lv_obj_set_size(s_prog_bar, lv_pct(100), 6);
        lv_bar_set_range(s_prog_bar, 0, 100);
        lv_obj_set_style_bg_color(s_prog_bar, th->surface2, 0);
        lv_obj_set_style_bg_color(s_prog_bar, th->accent, LV_PART_INDICATOR);
        progress_update();
    }
    if (s_clip.set) {
        lv_obj_t *br = button_row(c);
        char lb[160];
        char what[24];
        if (s_clip.n > 0) lv_snprintf(what, sizeof what, nv_tr(NV_STR_ITEMS_FMT), s_clip.n);
        lv_snprintf(lb, sizeof lb, nv_tr(NV_STR_PASTE_HERE_FMT), s_clip.n > 0 ? what : base_name(s_clip.path));
        lv_obj_t *b = nv_kit_button(br, lb, true);
        lv_obj_add_event_cb(b, paste_cb, LV_EVENT_CLICKED, nullptr);
        lv_obj_t *x = nv_kit_button(br, nv_tr(NV_STR_CANCEL), false);
        lv_obj_add_event_cb(x, clip_clear_cb, LV_EVENT_CLICKED, nullptr);
    }
}

// ---------------------------------------------------------------- Places page
void place_open_cb(lv_event_t *e) {
    const int slot = (int)(intptr_t)lv_event_get_user_data(e);   // -1 = SD card
    if (slot < 0) {
        strcpy(s_path, kRoot);
    } else {
        nv_usb_stor_info_t v;
        if (!nv_usb_storage_get(slot, &v) || v.state != NV_USB_STOR_MOUNTED) return;
        snprintf(s_path, sizeof s_path, "/usb%d", slot);
    }
    s_focus[0] = '\0';
    scan_dir();
    nav_to(Page::List);
}

// Eject runs on the bg worker (SCSI + drain wait can take seconds); result comes back as a toast.
void eject_job(void *arg) {
    const int slot = (int)(intptr_t)arg;
    nv_usb_stor_info_t v{};
    const bool had = nv_usb_storage_get(slot, &v);
    const bool ok = nv_usb_storage_eject(slot);
    if (lvgl_port_lock(2000)) {
        if (ok && had) {
            char nm[40], m[96];
            usb_name(v, nm, sizeof nm);
            lv_snprintf(m, sizeof m, "%s: %s", nm, nv_tr(NV_STR_USB_EJECTED));
            nv_toast(NV_NOTE_OK, m);
        } else if (!ok) {
            nv_toast(NV_NOTE_WARN, nv_tr(NV_STR_EJECT_BUSY));
        }
        lvgl_port_unlock();
    }
}
void place_eject_cb(lv_event_t *e) {
    const int slot = (int)(intptr_t)lv_event_get_user_data(e);
    if (!nv_bgwork_submit(eject_job, (void *)(intptr_t)slot)) nv_toast(NV_NOTE_WARN, nv_tr(NV_STR_EJECT_BUSY));
}

lv_obj_t *place_row(lv_obj_t *col, const char *sym, const char *name, const char *sub, int slot,
                    bool openable, bool ejectable) {
    const NvTheme *th = nv_theme_get();
    lv_obj_t *row = lv_obj_create(col);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(row, th->surface, 0);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(row, NV_RAD_MD, 0);
    lv_obj_set_style_pad_hor(row, NV_SP_4, 0);
    lv_obj_set_style_pad_ver(row, NV_SP_3, 0);
    lv_obj_set_style_min_height(row, NV_TOUCH_MIN + NV_SP_4, 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, NV_SP_4, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    if (openable) {
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_bg_color(row, th->surface2, LV_STATE_PRESSED);
        lv_obj_add_event_cb(row, place_open_cb, LV_EVENT_SHORT_CLICKED, (void *)(intptr_t)slot);
    }
    lv_obj_t *ic = lv_label_create(row);
    lv_label_set_text(ic, sym);
    lv_obj_set_style_text_font(ic, &nv_font_28, 0);
    lv_obj_set_style_text_color(ic, openable ? th->accent : th->text_dim, 0);
    lv_obj_set_style_min_width(ic, 36, 0);
    lv_obj_t *tc = lv_obj_create(row);
    lv_obj_remove_style_all(tc);
    lv_obj_set_height(tc, LV_SIZE_CONTENT);
    lv_obj_set_flex_grow(tc, 1);
    lv_obj_set_flex_flow(tc, LV_FLEX_FLOW_COLUMN);
    lv_obj_clear_flag(tc, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(tc, LV_OBJ_FLAG_CLICKABLE);   // v9 hit-test: let taps reach the row
    lv_obj_t *nm = lv_label_create(tc);
    lv_label_set_text(nm, name);
    lv_obj_set_width(nm, lv_pct(100));
    lv_label_set_long_mode(nm, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_font(nm, &nv_font_20, 0);
    lv_obj_set_style_text_color(nm, th->text_strong, 0);
    lv_obj_t *sb = lv_label_create(tc);
    lv_label_set_text(sb, sub);
    lv_obj_set_width(sb, lv_pct(100));
    lv_label_set_long_mode(sb, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_font(sb, &nv_font_14, 0);
    lv_obj_set_style_text_color(sb, th->text_dim, 0);
    if (ejectable) {
        lv_obj_t *b = nv_kit_button(row, LV_SYMBOL_EJECT, false);
        lv_obj_add_event_cb(b, place_eject_cb, LV_EVENT_CLICKED, (void *)(intptr_t)slot);
    }
    return row;
}

void build_places(void) {
    lv_obj_t *content = nv_ui_app_content();
    if (!content) return;
    lv_obj_clean(content);
    s_del_btn_label = nullptr;
    s_ren_ta = nullptr;
    s_prog_label = nullptr;
    s_prog_bar = nullptr;
    s_page = Page::Places;
    s_path[0] = '\0';
    nv_ui_set_title(nv_tr(NV_STR_APP_FILES));
    nv_ui_set_back_handler(nullptr);
    nv_ui_set_key_handler(nullptr);   // the list's keys act on a folder: none here
    nv_ui_set_shortcuts(nullptr, 0);

    lv_obj_t *c = nv_kit_scroll_column(content);
    char sub[128], f[16], t[16];

    uint64_t total = 0, freeb = 0;
    const bool sd = nv_sd_is_mounted();
    if (sd && nv_sd_info(&total, &freeb)) {
        fmt_bytes(freeb, f, sizeof f);
        fmt_bytes(total, t, sizeof t);
        lv_snprintf(sub, sizeof sub, nv_tr(NV_STR_FREE_OF_FMT), f, t);
    } else {
        snprintf(sub, sizeof sub, "%s", sd ? kRoot : nv_tr(NV_STR_SD_MISSING));
    }
    place_row(c, LV_SYMBOL_SD_CARD, nv_tr(NV_STR_STORAGE_SD), sub, -1, sd, false);

    nv_usb_stor_info_t v[NV_USB_STOR_SLOTS];
    const int n = nv_usb_storage_list(v, NV_USB_STOR_SLOTS);
    for (int i = 0; i < n; i++) {
        const int slot = nv_usb_storage_slot_of(v[i].path);
        char name[48];
        usb_name(v[i], name, sizeof name);
        const bool mounted = v[i].state == NV_USB_STOR_MOUNTED;
        switch (v[i].state) {
            case NV_USB_STOR_MOUNTED:
                fmt_bytes(v[i].total_bytes, t, sizeof t);
                if (v[i].free_bytes == UINT64_MAX) {
                    lv_snprintf(sub, sizeof sub, "%s \xC2\xB7 %s \xC2\xB7 %s", v[i].fs, t, nv_tr(NV_STR_MEASURING));
                } else {
                    char fr[64];
                    fmt_bytes(v[i].free_bytes, f, sizeof f);
                    lv_snprintf(fr, sizeof fr, nv_tr(NV_STR_FREE_OF_FMT), f, t);
                    lv_snprintf(sub, sizeof sub, "%s \xC2\xB7 %s", v[i].fs, fr);
                }
                if (v[i].read_only) {
                    const size_t l = strlen(sub);
                    lv_snprintf(sub + l, sizeof sub - l, " \xC2\xB7 %s", nv_tr(NV_STR_READ_ONLY));
                }
                break;
            case NV_USB_STOR_EJECTED:     snprintf(sub, sizeof sub, "%s", nv_tr(NV_STR_USB_EJECTED)); break;
            case NV_USB_STOR_UNFORMATTED: snprintf(sub, sizeof sub, "%s", nv_tr(NV_STR_USB_UNFORMATTED)); break;
            case NV_USB_STOR_ERROR:       snprintf(sub, sizeof sub, "%s", nv_tr(NV_STR_USB_UNREADABLE)); break;
            default:                      snprintf(sub, sizeof sub, "%s", nv_tr(NV_STR_USB_NO_MEDIA)); break;
        }
        place_row(c, v[i].removable ? LV_SYMBOL_SD_CARD : LV_SYMBOL_USB, name, sub, slot, mounted, mounted);
    }
}

void build_list(void) {
    lv_obj_t *content = nv_ui_app_content();
    if (!content) return;
    lv_obj_clean(content);
    s_del_btn_label = nullptr;
    s_ren_ta = nullptr;
    s_prog_label = nullptr;
    s_prog_bar = nullptr;
    s_page = Page::List;

    const bool at_root = strlen(s_path) <= root_len(s_path);
    const int usb = cur_usb();
    char title[48];
    if (at_root && usb >= 0) {
        nv_usb_stor_info_t v;
        if (nv_usb_storage_get(usb, &v)) usb_name(v, title, sizeof title);
        else snprintf(title, sizeof title, "%s", nv_tr(NV_STR_USB_DRIVE));
    } else if (at_root) {
        snprintf(title, sizeof title, "%s", nv_tr(usb_attached() ? NV_STR_STORAGE_SD : NV_STR_APP_FILES));
    } else {
        snprintf(title, sizeof title, "%s", strrchr(s_path, '/') + 1);
    }
    nv_ui_set_title(title);
    // The volume root closes the app — unless USB volumes exist: then it goes up to Places.
    nv_ui_set_back_handler(at_root && !usb_attached() ? nullptr : back_from_list);
    // Keyboard: Backspace / Alt+Up up a folder, Del, F2, F5, Ctrl+A / C / X / V (files_key).
    nv_ui_set_key_handler(files_key);
    nv_ui_set_shortcuts(kListKeys, (int)(sizeof kListKeys / sizeof kListKeys[0]));

    lv_obj_t *c = nv_kit_scroll_column(content);
    s_list = c;
    nv_sel_attach(c);                 // mouse: band selection, empty-area click clears
    lv_obj_add_event_cb(c, list_ctx_cb, (lv_event_code_t)nv_ui_event_context(), nullptr);
    const NvTheme *th = nv_theme_get();

    // header: current path + volume free space
    lv_obj_t *head = lv_label_create(c);
    uint64_t total = 0, freeb = 0;
    char hd[256];
    if (vol_info(&total, &freeb)) {
        char f[16], t[16];
        fmt_bytes(freeb, f, sizeof f);
        fmt_bytes(total, t, sizeof t);
        snprintf(hd, sizeof hd, "%s   \xC2\xB7   %s / %s", s_path, f, t);
    } else {
        snprintf(hd, sizeof hd, "%s", s_path);
    }
    lv_label_set_text(head, hd);
    lv_obj_set_style_text_font(head, &nv_font_14, 0);
    lv_obj_set_style_text_color(head, th->text_dim, 0);

    if (!vol_mounted()) {
        lv_obj_t *info = nv_kit_info(c);
        lv_label_set_text(info, nv_tr(usb >= 0 ? NV_STR_USB_NO_MEDIA : NV_STR_SD_MISSING));
        lv_obj_set_style_text_color(info, th->text_dim, 0);
        return;
    }
    fileop_bars(c);

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
    if (s_ents[s_sel].dir) {   // recursive: runs on the file-ops task (progress in the list)
        const FopResult r = fop_start(FOP_DELETE, full, nullptr);
        fop_toast(r);
        if (r == FOP_OK) { s_sel_name[0] = '\0'; nav_to(Page::List); }
        return;
    }
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

// Copy / Move: remember the item, go back to browsing — the paste bar finishes the job.
void clip_set(FopKind k) {
    if (!sel_path(s_clip.path, sizeof s_clip.path)) return;
    s_clip.n = 0;
    s_clip.kind = k;
    s_clip.set = true;
    back_to_list();
}
void copy_cb(lv_event_t *) { clip_set(FOP_COPY); }
void move_cb(lv_event_t *) { clip_set(FOP_MOVE); }

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

// ---------------------------------------------------------------- selection: menus + keys
// The selected rows of the listing (the system selection), else the keyboard-focused row.
int sel_rows(lv_obj_t **out, int max) {
    int k = 0;
    if (!s_list || !lv_obj_is_valid(s_list)) return 0;
    const int n = nv_sel_count(s_list);
    for (int j = 0; j < n && k < max; j++) out[k++] = nv_sel_nth(s_list, j);
    if (!k)
        if (lv_obj_t *f = nv_focus_current())
            if (lv_obj_get_parent(f) == s_list && lv_obj_get_user_data(f)) out[k++] = f;
    return k;
}
int row_index(lv_obj_t *r) {
    const int i = (int)(intptr_t)lv_obj_get_user_data(r) - 1;
    return i >= 0 && i < s_n ? i : -1;
}
int first_sel(void) {
    lv_obj_t *r[1];
    return sel_rows(r, 1) ? row_index(r[0]) : -1;
}

void ctx_open(void *) {
    lv_obj_t *r[1];
    if (sel_rows(r, 1)) lv_obj_send_event(r[0], LV_EVENT_SHORT_CLICKED, nullptr);   // no pointer: opens
}
void ctx_open_with(void *) {
    char full[NV_OPEN_PATH_MAX];
    const int i = first_sel();
    if (i >= 0 && ent_path(i, full, sizeof full)) nv_open_with(full);
}
void ctx_details(void *) {
    const int i = first_sel();
    if (i < 0) return;
    select_ent(i);
    nav_to(Page::Detail);
}
void ctx_clip(FopKind k) {
    lv_obj_t *r[kMultiMax];
    const int n = sel_rows(r, kMultiMax);
    if (!n) return;
    if (n == 1) {
        const int i = row_index(r[0]);
        if (i < 0 || !ent_path(i, s_clip.path, sizeof s_clip.path)) return;
        s_clip.n = 0;
    } else {
        int m = 0;
        for (int j = 0; j < n; j++) {
            const int i = row_index(r[j]);
            if (i >= 0) snprintf(s_mnames[m++], kNameMax, "%s", s_ents[i].name);
        }
        snprintf(s_clip.path, sizeof s_clip.path, "%s", s_path);
        s_clip.n = m;
    }
    s_clip.kind = k;
    s_clip.set = true;
    nav_to(Page::List);               // the paste bar appears
}
void ctx_copy(void *) { ctx_clip(FOP_COPY); }
void ctx_cut(void *) { ctx_clip(FOP_MOVE); }
void ctx_paste(void *) { paste_cb(nullptr); }
void ctx_select_all(void *) { if (s_list) nv_sel_all(s_list); }
void ctx_refresh(void *) { scan_dir(); nav_to(Page::List); }
void ctx_noop(void *) {}
void ctx_delete_do(void *) {
    if (batch_busy()) return;
    lv_obj_t *r[kMultiMax];
    const int n = sel_rows(r, kMultiMax);
    int m = 0;
    for (int j = 0; j < n; j++) {
        const int i = row_index(r[j]);
        if (i >= 0) snprintf(s_bnames[m++], kNameMax, "%s", s_ents[i].name);
    }
    if (!m) return;
    s_sel_name[0] = '\0';
    batch_begin(FOP_DELETE, s_path, nullptr, m);
    nav_to(Page::List);               // progress row; the tick rescans when it is done
}
// Delete always asks first, in a small menu where the first one was.
void ctx_delete(void *) {
    lv_obj_t *r[kMultiMax];
    const int n = sel_rows(r, kMultiMax);
    if (!n) return;
    static NV_PSRAM_BSS char q[kNameMax + 4];
    if (n == 1) {
        const int i = row_index(r[0]);
        if (i < 0) return;
        snprintf(q, sizeof q, "%s?", s_ents[i].name);
    } else {
        lv_snprintf(q, sizeof q, nv_tr(NV_STR_DEL_N_FMT), n);
    }
    const nv_menu_item_t m[] = {
        {LV_SYMBOL_WARNING, q, nullptr, nullptr, nullptr, false, true},
        {LV_SYMBOL_TRASH, nv_tr(NV_STR_DELETE), nullptr, ctx_delete_do, nullptr, true, false},
        {LV_SYMBOL_CLOSE, nv_tr(NV_STR_CANCEL), "Esc", ctx_noop, nullptr, false, false},
    };
    nv_ui_menu_open(s_ctx_pt.x, s_ctx_pt.y, m, 3);
}

void row_ctx_cb(lv_event_t *e) {
    if (const lv_point_t *p = (const lv_point_t *)lv_event_get_param(e)) s_ctx_pt = *p;
    const int total = s_list ? LV_MAX(1, nv_sel_count(s_list)) : 1;
    const int i = first_sel();
    if (i < 0) return;
    static NV_PSRAM_BSS char head[32];
    nv_menu_item_t m[10];
    int k = 0;
    if (total == 1) {
        m[k++] = {s_ents[i].dir ? LV_SYMBOL_DIRECTORY : LV_SYMBOL_PLAY, nv_tr(NV_STR_OPEN), "Enter", ctx_open, nullptr, false, false};
        if (!s_ents[i].dir)
            m[k++] = {LV_SYMBOL_SHUFFLE, nv_tr(NV_STR_OPEN_WITH), nullptr, ctx_open_with, nullptr, false, false};
    } else {
        lv_snprintf(head, sizeof head, nv_tr(NV_STR_N_SELECTED_FMT), total);
        m[k++] = {LV_SYMBOL_OK, head, nullptr, nullptr, nullptr, false, true};
    }
    m[k++] = {LV_SYMBOL_COPY, nv_tr(NV_STR_COPY), "Ctrl+C", ctx_copy, nullptr, true, false};
    m[k++] = {LV_SYMBOL_CUT, nv_tr(NV_STR_CUT), "Ctrl+X", ctx_cut, nullptr, false, false};
    if (s_clip.set || sys_clip_pastable()) m[k++] = {LV_SYMBOL_PASTE, nv_tr(NV_STR_PASTE), "Ctrl+V", ctx_paste, nullptr, false, false};
    if (total == 1) m[k++] = {LV_SYMBOL_EDIT, nv_tr(NV_STR_RENAME), "F2", ctx_details, nullptr, true, false};
    m[k++] = {LV_SYMBOL_TRASH, nv_tr(NV_STR_DELETE), "Del", ctx_delete, nullptr, total > 1, false};
    if (total == 1) m[k++] = {LV_SYMBOL_LIST, nv_tr(NV_STR_DETAILS), nullptr, ctx_details, nullptr, true, false};
    nv_ui_menu_open(s_ctx_pt.x, s_ctx_pt.y, m, k);
}

void list_ctx_cb(lv_event_t *e) {     // right click on the empty part of the listing
    if (const lv_point_t *p = (const lv_point_t *)lv_event_get_param(e)) s_ctx_pt = *p;
    const nv_menu_item_t m[] = {
        {LV_SYMBOL_PASTE, nv_tr(NV_STR_PASTE), "Ctrl+V", (s_clip.set || sys_clip_pastable()) ? ctx_paste : nullptr, nullptr, false, false},
        {LV_SYMBOL_LIST, nv_tr(NV_STR_SELECT_ALL), "Ctrl+A", ctx_select_all, nullptr, true, false},
        {LV_SYMBOL_REFRESH, nv_tr(NV_STR_REFRESH), "F5", ctx_refresh, nullptr, false, false},
    };
    nv_ui_menu_open(s_ctx_pt.x, s_ctx_pt.y, m, 3);
}

// List page keys (nv_ui_set_key_handler): desktop file-manager shortcuts.
bool files_key(uint32_t key, uint8_t usage, uint8_t mods) {
    const bool ctrl = mods & 0x11;
    const bool alt  = mods & 0x04;     // left Alt only: right Alt is AltGr on EU layouts
    if (key == LV_KEY_BACKSPACE || (alt && key == LV_KEY_UP)) {    // up a folder (Esc / Back already do)
        nv_ui_set_key_handler(nullptr);
        nv_ui_set_shortcuts(nullptr, 0);
        lv_async_call([](void *) { back_from_list(); }, nullptr);
        return true;
    }
    s_ctx_pt = {LV_HOR_RES / 2 - 120, LV_VER_RES / 3};
    if (key == LV_KEY_DEL) { ctx_delete(nullptr); return true; }
    if (usage == 0x3B) { ctx_details(nullptr); return true; }          // F2: rename (Details)
    if (usage == 0x3E) { ctx_refresh(nullptr); return true; }          // F5
    if (!ctrl) return false;
    switch (usage) {
        case 0x04: ctx_select_all(nullptr); return true;               // A
        case 0x06: ctx_copy(nullptr); return true;                     // C
        case 0x1B: ctx_cut(nullptr); return true;                      // X
        case 0x19: ctx_paste(nullptr); return true;                    // V
        default: return false;
    }
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
    nv_ui_set_back_handler(back_to_list);
    nv_ui_set_key_handler(nullptr);   // Backspace-up belongs to the list page only
    nv_ui_set_shortcuts(nullptr, 0);

    lv_obj_t *c = nv_kit_scroll_column(content);
    const NvTheme *th = nv_theme_get();

    char full[448];
    snprintf(full, sizeof full, "%s/%s", s_path, en->name);
    const bool openable_path = !en->dir && strlen(full) < NV_OPEN_PATH_MAX;
    const char *mime = en->dir ? "inode/directory" : nv_open_mime(en->name);
    const nv_file_kind_t kind = en->dir ? NV_FILE_DIR : nv_open_kind_of_mime(mime);

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
    const char *dot = en->dir ? nullptr : strrchr(en->name, '.');
    char kl[48];
    if (en->dir) {
        lv_snprintf(kl, sizeof kl, "%s", nv_tr(NV_STR_FOLDER));
    } else if (dot && dot != en->name && dot[1]) {
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
    } else if (!en->dir) {
        lv_obj_t *none = nv_kit_info(c);
        lv_label_set_text(none, nv_tr(NV_STR_NO_APP_FOR_FILE));
        lv_obj_set_style_text_color(none, th->text_dim, 0);
    }
    if (strlen(full) < NV_OPEN_PATH_MAX) {   // copy / move to another folder or drive
        lv_obj_t *br = button_row(c);
        lv_obj_t *cb = nv_kit_button(br, nv_tr(NV_STR_COPY), false);
        lv_obj_add_event_cb(cb, copy_cb, LV_EVENT_CLICKED, nullptr);
        lv_obj_t *mb = nv_kit_button(br, nv_tr(NV_STR_MOVE), false);
        lv_obj_add_event_cb(mb, move_cb, LV_EVENT_CLICKED, nullptr);
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
    info_line(card, NV_STR_TYPE, en->dir ? nv_tr(NV_STR_FOLDER) : mime);
    if (!en->dir) {
        char sz[24];
        fmt_size(en->size, sz, sizeof sz);
        info_line(card, NV_STR_SIZE, sz);
    }
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
    lv_obj_add_event_cb(s_ren_ta, rename_cb, LV_EVENT_READY, nullptr);   // Enter in the field renames
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
    nv_ui_set_back_handler(back_from_preview);
    nv_ui_set_key_handler(nullptr);   // Backspace-up belongs to the list page only
    nv_ui_set_shortcuts(nullptr, 0);

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
// 1 s: follow USB hot-plug / card swaps / finished copies without the user touching anything.
void files_tick(lv_timer_t *) {
    if (s_nav_pending) return;
    if (s_batch.i < s_batch.n) {          // multi-item job: start the next item when idle
        FopStatus st;
        fop_status(&st);
        if (!st.busy) batch_next();
    }
    const uint32_t ug = nv_usb_storage_generation(), fg = fop_generation();
    const bool usb_changed = ug != s_usb_gen, fop_done = fg != s_fop_gen;
    s_usb_gen = ug;
    s_fop_gen = fg;
    if (s_prog_label) progress_update();
    if (!usb_changed && !fop_done) return;
    switch (s_page) {
        case Page::Places:
            nav_to(Page::Places);
            break;
        case Page::List:
            if (cur_usb() >= 0 && !vol_mounted()) {   // the drive under us went away / was ejected
                s_path[0] = '\0';
                nav_to(Page::Places);
            } else if (fop_done || (usb_changed && strlen(s_path) <= root_len(s_path))) {
                scan_dir();   // copy finished here, or root header (free space) changed
                nav_to(Page::List);
            }
            break;
        case Page::Detail:
        case Page::Preview:
            if (cur_usb() >= 0 && !vol_mounted()) { s_path[0] = '\0'; nav_to(Page::Places); }
            break;
    }
}

void files_deleted(lv_event_t *) {
    lv_async_call_cancel(nav_apply, nullptr);   // a queued navigation must not run on the next app's tree
    if (s_tick) { lv_timer_delete(s_tick); s_tick = nullptr; }
    s_prog_label = nullptr;
    s_prog_bar = nullptr;
    s_clip.set = false;
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
        // Top level: the volume list while USB drives are attached, else straight into the SD card.
        const bool places = usb_attached();
        if (places) s_path[0] = '\0';
        else strcpy(s_path, kRoot);
        select_ent(-1);
        s_focus[0] = '\0';
        s_page = places ? Page::Places : Page::List;
        s_seen.valid = false;
        s_clip.set = false;
        s_usb_gen = nv_usb_storage_generation();
        s_fop_gen = fop_generation();
        if (!s_tick) s_tick = lv_timer_create(files_tick, 1000, nullptr);
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
