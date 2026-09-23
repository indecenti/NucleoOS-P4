// nv_open — see nv_open.h. File-type table, handler registry, default apps (nv_config
// "open_defs"), the intent slots handed to launched apps, and the system "Open with" sheet.
//
// Every launch is deferred to the next LVGL loop (lv_async_call): callers are event handlers of
// the app that is about to be torn down (a Files row, a chooser row), and nv_ui_open_app() deletes
// that app synchronously. The request is copied, so the caller's buffers may go away.
//
// Intent slots: `s_pending` is filled right before nv_ui_open_app() and is what the target sees
// during its first build(); once the app is up it becomes `s_active`, which lives until that app
// closes (nv_open_on_app_closed) or drops it. Keeping the two apart matters because opening the
// target closes the caller first, and that close must not wipe the intent in flight.
#include "nv_open.h"

#include "nv_app.h"
#include "nv_ui.h"
#include "nv_ui_kit.h"
#include "nv_gesture.h"
#include "nv_ime.h"
#include "nv_i18n.h"
#include "nv_theme.h"
#include "nv_fonts.h"
#include "nv_notify.h"
#include "nv_config.h"
#include "nv_log.h"
#include "nv_mem_attr.h"   // NV_PSRAM_BSS: registry / slots / sheet state out of internal SRAM

#include "lvgl.h"
#include "esp_lvgl_port.h"
#include "esp_heap_caps.h"

#include <sys/stat.h>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <strings.h>

namespace {

const char *TAG = "nv_open";
constexpr char kOctet[] = "application/octet-stream";
constexpr char kFilesApp[] = "files";   // the system file browser (reveal / resume target)

// ================================================================== type table
struct TypeRow { const char *ext; const char *mime; nv_file_kind_t kind; };

// One row per extension; several extensions may share a MIME. The openers' "types" lists (in each
// app's register function) declare what their decoders really support — a row here only names
// the type, it does not promise that anything opens it.
const TypeRow kTypes[] = {
    // text and structured text
    {"txt",      "text/plain",                NV_FILE_TEXT},
    {"text",     "text/plain",                NV_FILE_TEXT},
    {"md",       "text/markdown",             NV_FILE_TEXT},
    {"markdown", "text/markdown",             NV_FILE_TEXT},
    {"log",      "text/x-log",                NV_FILE_TEXT},
    {"csv",      "text/csv",                  NV_FILE_TEXT},
    {"tsv",      "text/tab-separated-values", NV_FILE_TEXT},
    {"json",     "application/json",          NV_FILE_TEXT},
    {"xml",      "application/xml",           NV_FILE_TEXT},
    {"html",     "text/html",                 NV_FILE_TEXT},
    {"htm",      "text/html",                 NV_FILE_TEXT},
    {"css",      "text/css",                  NV_FILE_TEXT},
    {"js",       "text/javascript",           NV_FILE_TEXT},
    {"ini",      "text/x-ini",                NV_FILE_TEXT},
    {"cfg",      "text/x-ini",                NV_FILE_TEXT},
    {"conf",     "text/x-ini",                NV_FILE_TEXT},
    {"yaml",     "text/yaml",                 NV_FILE_TEXT},
    {"yml",      "text/yaml",                 NV_FILE_TEXT},
    {"c",        "text/x-c",                  NV_FILE_TEXT},
    {"h",        "text/x-c",                  NV_FILE_TEXT},
    {"cpp",      "text/x-c++",                NV_FILE_TEXT},
    {"hpp",      "text/x-c++",                NV_FILE_TEXT},
    {"py",       "text/x-python",             NV_FILE_TEXT},
    {"sh",       "text/x-shellscript",        NV_FILE_TEXT},
    // images
    {"jpg",      "image/jpeg",                NV_FILE_IMAGE},
    {"jpeg",     "image/jpeg",                NV_FILE_IMAGE},
    {"png",      "image/png",                 NV_FILE_IMAGE},
    {"bmp",      "image/bmp",                 NV_FILE_IMAGE},
    {"gif",      "image/gif",                 NV_FILE_IMAGE},
    {"webp",     "image/webp",                NV_FILE_IMAGE},
    // audio
    {"mp3",      "audio/mpeg",                NV_FILE_AUDIO},
    {"wav",      "audio/wav",                 NV_FILE_AUDIO},
    {"flac",     "audio/flac",                NV_FILE_AUDIO},
    {"aac",      "audio/aac",                 NV_FILE_AUDIO},
    {"m4a",      "audio/mp4",                 NV_FILE_AUDIO},
    {"ogg",      "audio/ogg",                 NV_FILE_AUDIO},
    {"opus",     "audio/opus",                NV_FILE_AUDIO},
    // video
    {"avi",      "video/x-msvideo",           NV_FILE_VIDEO},
    {"mpg",      "video/mpeg",                NV_FILE_VIDEO},
    {"mpeg",     "video/mpeg",                NV_FILE_VIDEO},
    {"m1v",      "video/mpeg",                NV_FILE_VIDEO},
    {"mp4",      "video/mp4",                 NV_FILE_VIDEO},
    {"h264",     "video/h264",                NV_FILE_VIDEO},
    {"mkv",      "video/x-matroska",          NV_FILE_VIDEO},
    {"mov",      "video/quicktime",           NV_FILE_VIDEO},
    // apps, archives, documents
    {"wasm",     "application/wasm",          NV_FILE_APP},
    {"zip",      "application/zip",           NV_FILE_ARCHIVE},
    {"tar",      "application/x-tar",         NV_FILE_ARCHIVE},
    {"gz",       "application/gzip",          NV_FILE_ARCHIVE},
    {"pdf",      "application/pdf",           NV_FILE_OTHER},
    {"bin",      kOctet,                      NV_FILE_OTHER},
};
constexpr int kBuiltinN = sizeof(kTypes) / sizeof(kTypes[0]);
constexpr int kExtMax   = 12;   // extension incl. NUL

struct ExtraType { char ext[kExtMax]; char mime[NV_OPEN_MIME_MAX]; nv_file_kind_t kind; };
constexpr int kMaxExtra = 24;
NV_PSRAM_BSS ExtraType s_extra[kMaxExtra];   // app-declared types (boot-time only)
int s_extra_n = 0;

// Lower-cased extension of the last path component into out[kExtMax]. False for no extension,
// hidden files (".profile") and extensions longer than kExtMax - 1.
bool ext_of(const char *name, char *out) {
    if (!name) return false;
    const char *base = strrchr(name, '/');
    base = base ? base + 1 : name;
    const char *dot = strrchr(base, '.');
    if (!dot || dot == base || !dot[1]) return false;
    size_t i = 0;
    for (const char *p = dot + 1; *p; p++) {
        if (i + 1 >= (size_t)kExtMax) return false;
        out[i++] = (char)tolower((unsigned char)*p);
    }
    out[i] = '\0';
    return true;
}

bool valid_mime(const char *m, bool allow_star) {
    if (!m || !m[0] || strlen(m) >= NV_OPEN_MIME_MAX) return false;
    const char *slash = strchr(m, '/');
    if (!slash || slash == m || !slash[1] || strchr(slash + 1, '/')) return false;
    for (const char *p = m; *p; p++) {
        const char c = *p;
        if (isalnum((unsigned char)c) || c == '/' || c == '.' || c == '+' || c == '-') continue;
        if (c == '*' && allow_star) continue;
        return false;
    }
    return true;
}

// ================================================================== handler registry
constexpr int kMaxHandlers = 64;
NV_PSRAM_BSS const NvOpenHandler *s_h[kMaxHandlers];
int s_hn = 0;

bool app_present(const char *app_id) { return app_id && app_id[0] && nv_ui_find_app(app_id); }

// ================================================================== default apps
// Persisted as one nv_config string: "mime=handler;mime=handler". Loaded lazily, cached here.
constexpr char kDefKey[] = "open_defs";
constexpr int  kMaxDefs  = 24;
struct Def { char mime[NV_OPEN_MIME_MAX]; char id[NV_OPEN_ID_MAX]; };
NV_PSRAM_BSS Def  s_defs[kMaxDefs];
int               s_defs_n = -1;   // -1 = not loaded yet
NV_PSRAM_BSS char s_defbuf[kMaxDefs * (NV_OPEN_MIME_MAX + NV_OPEN_ID_MAX + 2) + 1];

void defs_load(void) {
    if (s_defs_n >= 0) return;
    s_defs_n = 0;
    nv_config_get_str(kDefKey, "", s_defbuf, sizeof s_defbuf);
    char *p = s_defbuf;
    while (*p && s_defs_n < kMaxDefs) {
        char *semi = strchr(p, ';');
        if (semi) *semi = '\0';
        char *eq = strchr(p, '=');
        if (eq) {
            *eq = '\0';
            const char *id = eq + 1;
            const size_t ml = strlen(p), il = strlen(id);
            if (ml && il && ml < NV_OPEN_MIME_MAX && il < NV_OPEN_ID_MAX) {   // lengths checked:
                memcpy(s_defs[s_defs_n].mime, p, ml + 1);                    // plain copies
                memcpy(s_defs[s_defs_n].id, id, il + 1);
                s_defs_n++;
            }
        }
        if (!semi) break;
        p = semi + 1;
    }
}

void defs_save(void) {
    size_t o = 0;
    s_defbuf[0] = '\0';
    for (int i = 0; i < s_defs_n; i++) {
        const int w = snprintf(s_defbuf + o, sizeof s_defbuf - o, "%s%s=%s", o ? ";" : "",
                               s_defs[i].mime, s_defs[i].id);
        if (w < 0 || (size_t)w >= sizeof s_defbuf - o) break;   // cannot happen at kMaxDefs; stay bounded
        o += (size_t)w;
    }
    nv_config_set_str(kDefKey, s_defbuf);
}

int defs_find(const char *mime) {
    defs_load();
    for (int i = 0; i < s_defs_n; i++)
        if (!strcasecmp(s_defs[i].mime, mime)) return i;
    return -1;
}

// ================================================================== resolution
// Openers accepting `mime`, installed apps only, best-first: the user default, then priority,
// then registration order (stable insertion sort — the list is tiny).
int collect(const char *mime, nv_open_role_t role, const NvOpenHandler **out, int max) {
    if (!mime || !out || max <= 0) return 0;
    const NvOpenHandler *v[kMaxHandlers];
    int n = 0;
    for (int i = 0; i < s_hn; i++) {
        const NvOpenHandler *h = s_h[i];
        if (h->role != role || !nv_open_mime_match(h->types, mime)) continue;
        if (role == NV_OPEN_OPENER && !app_present(h->app_id)) continue;
        v[n++] = h;
    }
    const int d = role == NV_OPEN_OPENER ? defs_find(mime) : -1;
    auto rank = [&](const NvOpenHandler *h) {
        return (d >= 0 && !strcmp(h->id, s_defs[d].id)) ? 1000 : (int)h->priority;
    };
    for (int i = 1; i < n; i++) {
        const NvOpenHandler *x = v[i];
        const int rx = rank(x);
        int j = i - 1;
        while (j >= 0 && rank(v[j]) < rx) { v[j + 1] = v[j]; j--; }
        v[j + 1] = x;
    }
    const int k = n < max ? n : max;
    for (int i = 0; i < k; i++) out[i] = v[i];
    return k;
}

// ================================================================== intent slots
struct Slot {
    bool     valid;
    char     target[NV_OPEN_ID_MAX];   // app the intent belongs to
    NvIntent in;
};
NV_PSRAM_BSS Slot s_pending;   // handed to the target during its launch build()
NV_PSRAM_BSS Slot s_active;    // bound to the foreground app until it closes

void slot_fill(Slot &s, const char *target, uint8_t verb, const char *path, const char *handler,
               const char *caller) {
    memset(&s, 0, sizeof s);
    s.valid = true;
    snprintf(s.target, sizeof s.target, "%s", target);
    snprintf(s.in.path, sizeof s.in.path, "%s", path ? path : "");
    snprintf(s.in.mime, sizeof s.in.mime, "%s", nv_open_mime(s.in.path));
    snprintf(s.in.handler, sizeof s.in.handler, "%s", handler ? handler : "");
    // Never "return" to yourself: re-delivery into the same app has no caller.
    if (caller && caller[0] && strcmp(caller, target) != 0)
        snprintf(s.in.caller, sizeof s.in.caller, "%s", caller);
    s.in.verb = verb;
}

const char *cur_app(void) { return nv_ui_current_app_id(); }

// Hand `s_pending` (already filled for `app`) to it. An app that is already in the foreground gets
// the intent in place (build() re-runs) instead of a close/relaunch; WASM apps are the exception —
// their build() starts the module, so they restart cleanly through the normal open path.
void deliver(const NvApp *app) {
    const char *cur = cur_app();
    if (cur[0] && !strcmp(cur, app->id) && app->user == nullptr) {
        s_active = s_pending;
        s_pending.valid = false;
        nv_ui_rebuild_app();
        return;
    }
    nv_ui_open_app(app);   // closes the current app first (its close leaves s_pending alone)
    if (!strcmp(cur_app(), app->id)) s_active = s_pending;   // refused launch (broker) = no intent
    s_pending.valid = false;
}

void launch_opener(const NvOpenHandler *h, const char *path, const char *caller) {
    const NvApp *app = h ? nv_ui_find_app(h->app_id) : nullptr;
    if (!app) { nv_toast(NV_NOTE_ERROR, nv_tr(NV_STR_OPEN_FAILED)); return; }
    NV_LOGI(TAG, "open %s -> %s", path, h->id);
    slot_fill(s_pending, app->id, NV_INTENT_OPEN, path, h->id, caller);
    deliver(app);
}

void launch_reveal(const char *path, const char *caller) {
    const NvApp *app = nv_ui_find_app(kFilesApp);
    if (!app) { nv_toast(NV_NOTE_ERROR, nv_tr(NV_STR_OPEN_FAILED)); return; }
    slot_fill(s_pending, app->id, NV_INTENT_REVEAL, path, nullptr, caller);
    deliver(app);
}

void do_finish(void) {
    const char *cur = cur_app();
    if (!cur[0]) return;
    if (!s_active.valid || strcmp(s_active.target, cur) != 0 || !s_active.in.caller[0]) {
        s_active.valid = false;
        nv_ui_close_app();
        return;
    }
    const NvApp *caller = nv_ui_find_app(s_active.in.caller);
    if (!caller) { s_active.valid = false; nv_ui_close_app(); return; }
    // The caller comes back on the file it handed out (Files re-selects it in its folder).
    slot_fill(s_pending, caller->id, NV_INTENT_RESUME, s_active.in.path, nullptr, nullptr);
    deliver(caller);
}

bool path_ok(const char *path) {
    return path && path[0] == '/' && strlen(path) < NV_OPEN_PATH_MAX && !strstr(path, "..");
}

// ================================================================== "Open with" sheet
enum class ChMode : uint8_t { Open, PickDefault };
constexpr int kChMax = 12;

struct Chooser {
    lv_obj_t *scrim;
    lv_obj_t *remember;   // switch (Open mode with 2+ apps)
    ChMode    mode;
    char      path[NV_OPEN_PATH_MAX];
    char      mime[NV_OPEN_MIME_MAX];
    char      caller[NV_OPEN_ID_MAX];
    const NvOpenHandler *items[kChMax];
    int       n;
    int       pick;           // row index to apply (-1 = "Ask every time")
    bool      pick_pending;
    bool      close_pending;
    void    (*done)(void *);
    void     *done_ctx;
};
NV_PSRAM_BSS Chooser s_ch;

void chooser_close_now(void) {
    if (s_ch.scrim) {
        lv_obj_t *s = s_ch.scrim;
        s_ch.scrim = nullptr;
        s_ch.remember = nullptr;
        lv_obj_delete(s);
    }
}

void close_apply(void *) {
    s_ch.close_pending = false;
    chooser_close_now();
}
void close_deferred(void) {
    if (s_ch.close_pending || !s_ch.scrim) return;
    if (lv_async_call(close_apply, nullptr) == LV_RESULT_OK) s_ch.close_pending = true;
}

void pick_apply(void *) {
    s_ch.pick_pending = false;
    if (!s_ch.scrim) return;
    const int i = s_ch.pick;
    const bool remember = s_ch.remember && lv_obj_has_state(s_ch.remember, LV_STATE_CHECKED);
    const NvOpenHandler *h = (i >= 0 && i < s_ch.n) ? s_ch.items[i] : nullptr;
    const ChMode mode = s_ch.mode;
    void (*done)(void *) = s_ch.done;
    void *done_ctx = s_ch.done_ctx;
    char path[NV_OPEN_PATH_MAX], mime[NV_OPEN_MIME_MAX], caller[NV_OPEN_ID_MAX];
    snprintf(path, sizeof path, "%s", s_ch.path);
    snprintf(mime, sizeof mime, "%s", s_ch.mime);
    snprintf(caller, sizeof caller, "%s", s_ch.caller);
    chooser_close_now();

    if (mode == ChMode::PickDefault) {
        nv_open_set_default(mime, h ? h->id : nullptr);
        if (done) done(done_ctx);
        return;
    }
    if (!h) return;
    if (remember) nv_open_set_default(mime, h->id);
    launch_opener(h, path, caller);
}

void row_cb(lv_event_t *e) {
    s_ch.pick = (int)(intptr_t)lv_event_get_user_data(e);
    if (s_ch.pick_pending) return;   // the row is deleted by the apply: defer, coalesce taps
    if (lv_async_call(pick_apply, nullptr) == LV_RESULT_OK) s_ch.pick_pending = true;
}
void dismiss_cb(lv_event_t *) { close_deferred(); }
void scrim_deleted(lv_event_t *) {
    s_ch.scrim = nullptr;
    s_ch.remember = nullptr;
}

lv_obj_t *sheet_row(lv_obj_t *list, const char *sym, const char *label, bool is_default, int idx) {
    const NvTheme *th = nv_theme_get();
    lv_obj_t *row = lv_obj_create(list);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_style_min_height(row, NV_TOUCH_MIN + NV_SP_2, 0);
    lv_obj_set_style_bg_color(row, th->surface2, 0);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(row, th->surface3, LV_STATE_PRESSED);
    lv_obj_set_style_radius(row, NV_RAD_SM, 0);
    lv_obj_set_style_pad_hor(row, NV_SP_3, 0);
    lv_obj_set_style_pad_ver(row, NV_SP_2, 0);
    lv_obj_set_style_pad_column(row, NV_SP_3, 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(row, row_cb, LV_EVENT_CLICKED, (void *)(intptr_t)idx);

    lv_obj_t *badge = lv_obj_create(row);
    lv_obj_remove_style_all(badge);
    lv_obj_set_size(badge, 40, 40);
    lv_obj_set_style_radius(badge, NV_RAD_SM - 2, 0);
    lv_obj_set_style_bg_color(badge, th->accent, 0);
    lv_obj_set_style_bg_opa(badge, LV_OPA_20, 0);
    lv_obj_clear_flag(badge, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(badge, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_t *ic = lv_label_create(badge);
    lv_label_set_text(ic, sym);
    lv_obj_set_style_text_color(ic, th->accent, 0);
    lv_obj_center(ic);

    lv_obj_t *nm = lv_label_create(row);
    lv_label_set_text(nm, label);
    lv_obj_set_style_text_font(nm, &nv_font_20, 0);
    lv_obj_set_style_text_color(nm, th->text_strong, 0);
    lv_label_set_long_mode(nm, LV_LABEL_LONG_DOT);
    lv_obj_set_flex_grow(nm, 1);

    if (is_default) {
        lv_obj_t *tag = lv_label_create(row);
        lv_label_set_text(tag, nv_tr(NV_STR_DEFAULT_TAG));
        lv_obj_set_style_text_font(tag, &nv_font_14, 0);
        lv_obj_set_style_text_color(tag, th->accent, 0);
    }
    return row;
}

// Centered modal card over a dim scrim, on the active screen (below a lock overlay raised later).
// Layer-free: bg_opa only — no shadow/opa/transform (P4 software renderer).
void chooser_show(ChMode mode, const char *path, const char *mime, const char *caller,
                  void (*done)(void *), void *done_ctx) {
    nv_open_dismiss();
    s_ch.n = collect(mime, NV_OPEN_OPENER, s_ch.items, kChMax);
    if (s_ch.n == 0) { nv_toast(NV_NOTE_WARN, nv_tr(NV_STR_NO_APP_FOR_FILE)); return; }
    nv_ime_hide();

    s_ch.mode = mode;
    s_ch.done = done;
    s_ch.done_ctx = done_ctx;
    s_ch.pick_pending = s_ch.close_pending = false;
    snprintf(s_ch.path, sizeof s_ch.path, "%s", path ? path : "");
    snprintf(s_ch.mime, sizeof s_ch.mime, "%s", mime);
    snprintf(s_ch.caller, sizeof s_ch.caller, "%s", caller ? caller : "");

    char def[NV_OPEN_ID_MAX];
    nv_open_get_default(mime, def, sizeof def);
    const NvTheme *th = nv_theme_get();

    lv_obj_t *scrim = lv_obj_create(lv_screen_active());
    s_ch.scrim = scrim;
    lv_obj_remove_style_all(scrim);
    lv_obj_set_size(scrim, LV_HOR_RES, LV_VER_RES);
    lv_obj_set_style_bg_color(scrim, th->scrim, 0);
    lv_obj_set_style_bg_opa(scrim, LV_OPA_50, 0);
    lv_obj_clear_flag(scrim, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(scrim, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(scrim, dismiss_cb, LV_EVENT_CLICKED, nullptr);
    lv_obj_add_event_cb(scrim, scrim_deleted, LV_EVENT_DELETE, nullptr);

    lv_obj_t *card = lv_obj_create(scrim);
    lv_obj_remove_style_all(card);
    const int cw = LV_HOR_RES - 2 * NV_SP_5 < 560 ? LV_HOR_RES - 2 * NV_SP_5 : 560;
    lv_obj_set_size(card, cw, LV_SIZE_CONTENT);
    lv_obj_center(card);
    lv_obj_set_style_bg_color(card, th->surface, 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(card, NV_RAD_LG, 0);
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_set_style_border_color(card, th->divider, 0);
    lv_obj_set_style_pad_all(card, NV_SP_4, 0);
    lv_obj_set_style_pad_row(card, NV_SP_2, 0);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);   // swallow taps: only the scrim dismisses

    lv_obj_t *title = lv_label_create(card);
    lv_label_set_text(title, nv_tr(NV_STR_OPEN_WITH));
    lv_obj_set_style_text_font(title, &nv_font_20, 0);
    lv_obj_set_style_text_color(title, th->text_strong, 0);

    lv_obj_t *sub = lv_label_create(card);
    if (mode == ChMode::Open) {
        const char *base = strrchr(s_ch.path, '/');
        lv_label_set_text(sub, base ? base + 1 : s_ch.path);
    } else {
        lv_label_set_text_fmt(sub, "%s \xC2\xB7 %s", nv_open_kind_label(nv_open_kind_of_mime(mime)), mime);
    }
    lv_obj_set_width(sub, lv_pct(100));
    lv_label_set_long_mode(sub, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_font(sub, &nv_font_14, 0);
    lv_obj_set_style_text_color(sub, th->text_dim, 0);

    lv_obj_t *list = lv_obj_create(card);
    lv_obj_remove_style_all(list);
    lv_obj_set_size(list, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_style_max_height(list, LV_VER_RES * 55 / 100, 0);
    lv_obj_set_style_pad_row(list, NV_SP_2, 0);
    lv_obj_set_style_pad_top(list, NV_SP_2, 0);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scroll_dir(list, LV_DIR_VER);

    if (mode == ChMode::PickDefault)
        sheet_row(list, LV_SYMBOL_LIST, nv_tr(NV_STR_ASK_EVERY_TIME), !def[0], -1);
    for (int i = 0; i < s_ch.n; i++) {
        const NvOpenHandler *h = s_ch.items[i];
        sheet_row(list, nv_open_handler_symbol(h, mime), nv_open_handler_label(h),
                  def[0] && !strcmp(def, h->id), i);
    }

    s_ch.remember = nullptr;
    if (mode == ChMode::Open && s_ch.n >= 2) {
        lv_obj_t *rr = lv_obj_create(card);
        lv_obj_remove_style_all(rr);
        lv_obj_set_size(rr, lv_pct(100), LV_SIZE_CONTENT);
        lv_obj_set_style_min_height(rr, NV_TOUCH_MIN, 0);
        lv_obj_set_flex_flow(rr, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(rr, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_clear_flag(rr, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_t *rl = lv_label_create(rr);
        lv_label_set_text(rl, nv_tr(NV_STR_REMEMBER_CHOICE));
        lv_obj_set_style_text_color(rl, th->text, 0);
        lv_obj_set_flex_grow(rl, 1);
        s_ch.remember = lv_switch_create(rr);
        lv_obj_set_style_bg_color(s_ch.remember, th->primary,
                                  (lv_style_selector_t)LV_PART_INDICATOR | LV_STATE_CHECKED);
    }

    lv_obj_t *cancel = nv_kit_button(card, nv_tr(NV_STR_CANCEL), false);
    lv_obj_set_width(cancel, lv_pct(100));
    lv_obj_add_event_cb(cancel, dismiss_cb, LV_EVENT_CLICKED, nullptr);

    nv_gesture_raise();   // system edge strips stay above the sheet (Back dismisses it)
}

// ================================================================== deferred requests
enum class Op : uint8_t { Open, With, Run, Reveal, Finish };
struct Req {
    Op   op;
    bool caller_known;                 // false: capture the foreground app when applied
    char path[NV_OPEN_PATH_MAX];
    char handler[NV_OPEN_ID_MAX];
    char caller[NV_OPEN_ID_MAX];
};

Req *req_new(Op op, const char *path, const char *handler) {
    Req *r = (Req *)heap_caps_calloc(1, sizeof(Req), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!r) r = (Req *)calloc(1, sizeof(Req));
    if (!r) return nullptr;
    r->op = op;
    snprintf(r->path, sizeof r->path, "%s", path ? path : "");
    snprintf(r->handler, sizeof r->handler, "%s", handler ? handler : "");
    return r;
}

void open_now(const char *path, const char *caller) {
    struct stat st;
    if (stat(path, &st) != 0) { nv_toast(NV_NOTE_WARN, nv_tr(NV_STR_FILE_NOT_FOUND)); return; }
    if (S_ISDIR(st.st_mode)) { launch_reveal(path, caller); return; }
    const char *mime = nv_open_mime(path);
    if (const NvOpenHandler *h = nv_open_preferred_mime(mime)) { launch_opener(h, path, caller); return; }
    chooser_show(ChMode::Open, path, mime, caller, nullptr, nullptr);   // toasts when none
}

void req_apply(void *p) {
    Req *r = (Req *)p;
    if (!r->caller_known) snprintf(r->caller, sizeof r->caller, "%s", cur_app());
    switch (r->op) {
        case Op::Open:   open_now(r->path, r->caller); break;
        case Op::With:   chooser_show(ChMode::Open, r->path, nv_open_mime(r->path), r->caller, nullptr, nullptr); break;
        case Op::Reveal: launch_reveal(r->path, r->caller); break;
        case Op::Finish: do_finish(); break;
        case Op::Run: {
            const NvOpenHandler *h = nv_open_find(r->handler);
            struct stat st;
            if (!h) nv_toast(NV_NOTE_ERROR, nv_tr(NV_STR_OPEN_FAILED));
            else if (stat(r->path, &st) != 0) nv_toast(NV_NOTE_WARN, nv_tr(NV_STR_FILE_NOT_FOUND));
            else if (h->role == NV_OPEN_ACTION) { if (h->run) h->run(r->path, h->ctx); }
            else launch_opener(h, r->path, r->caller);
            break;
        }
    }
    heap_caps_free(r);
}

// LVGL thread: queue a request with the current app as its caller.
bool post(Req *r) {
    if (!r) return false;
    snprintf(r->caller, sizeof r->caller, "%s", cur_app());
    r->caller_known = true;
    if (lv_async_call(req_apply, r) != LV_RESULT_OK) { heap_caps_free(r); return false; }
    return true;
}

}  // namespace

// ================================================================== types (public)
const char *nv_open_mime(const char *name) {
    char ext[kExtMax];
    if (!ext_of(name, ext)) return kOctet;
    for (int i = 0; i < kBuiltinN; i++)
        if (!strcmp(ext, kTypes[i].ext)) return kTypes[i].mime;
    for (int i = 0; i < s_extra_n; i++)
        if (!strcmp(ext, s_extra[i].ext)) return s_extra[i].mime;
    return kOctet;
}

nv_file_kind_t nv_open_kind_of_mime(const char *mime) {
    if (!mime || !mime[0]) return NV_FILE_OTHER;
    for (int i = 0; i < kBuiltinN; i++)
        if (!strcasecmp(mime, kTypes[i].mime)) return kTypes[i].kind;
    for (int i = 0; i < s_extra_n; i++)
        if (!strcasecmp(mime, s_extra[i].mime)) return s_extra[i].kind;
    if (!strncasecmp(mime, "text/", 5))  return NV_FILE_TEXT;
    if (!strncasecmp(mime, "image/", 6)) return NV_FILE_IMAGE;
    if (!strncasecmp(mime, "audio/", 6)) return NV_FILE_AUDIO;
    if (!strncasecmp(mime, "video/", 6)) return NV_FILE_VIDEO;
    return NV_FILE_OTHER;
}

nv_file_kind_t nv_open_kind(const char *name) { return nv_open_kind_of_mime(nv_open_mime(name)); }

const char *nv_open_kind_symbol(nv_file_kind_t kind) {
    switch (kind) {
        case NV_FILE_DIR:     return LV_SYMBOL_DIRECTORY;
        case NV_FILE_TEXT:    return LV_SYMBOL_LIST;
        case NV_FILE_IMAGE:   return LV_SYMBOL_IMAGE;
        case NV_FILE_AUDIO:   return LV_SYMBOL_AUDIO;
        case NV_FILE_VIDEO:   return LV_SYMBOL_VIDEO;
        case NV_FILE_APP:     return LV_SYMBOL_PLAY;
        case NV_FILE_ARCHIVE: return LV_SYMBOL_DRIVE;
        default:              return LV_SYMBOL_FILE;
    }
}

const char *nv_open_kind_label(nv_file_kind_t kind) {
    switch (kind) {
        case NV_FILE_DIR:     return nv_tr(NV_STR_KIND_FOLDER);
        case NV_FILE_TEXT:    return nv_tr(NV_STR_KIND_TEXT);
        case NV_FILE_IMAGE:   return nv_tr(NV_STR_KIND_IMAGE);
        case NV_FILE_AUDIO:   return nv_tr(NV_STR_KIND_AUDIO);
        case NV_FILE_VIDEO:   return nv_tr(NV_STR_KIND_VIDEO);
        case NV_FILE_APP:     return nv_tr(NV_STR_KIND_APP);
        case NV_FILE_ARCHIVE: return nv_tr(NV_STR_KIND_ARCHIVE);
        default:              return nv_tr(NV_STR_KIND_FILE);
    }
}

bool nv_open_mime_match(const char *patterns, const char *mime) {
    if (!patterns || !mime || !mime[0]) return false;
    const size_t mlen = strlen(mime);
    const char *p = patterns;
    while (*p) {
        while (*p == ' ') p++;
        const char *s = p;
        while (*p && *p != ' ') p++;
        const size_t len = (size_t)(p - s);
        if (!len) continue;
        if (len == 3 && !strncmp(s, "*/*", 3)) return true;
        if (len >= 3 && s[len - 1] == '*' && s[len - 2] == '/') {          // "type/*"
            if (mlen > len - 1 && !strncasecmp(s, mime, len - 1)) return true;
        } else if (len == mlen && !strncasecmp(s, mime, len)) {
            return true;
        }
    }
    return false;
}

bool nv_open_register_type(const char *ext, const char *mime, nv_file_kind_t kind) {
    if (!ext || !ext[0] || !valid_mime(mime, false) || kind >= NV_FILE_KIND_COUNT) return false;
    if (strlen(ext) >= (size_t)kExtMax || s_extra_n >= kMaxExtra) return false;
    char lo[kExtMax];
    size_t i = 0;
    for (; ext[i]; i++) {
        if (!isalnum((unsigned char)ext[i])) return false;
        lo[i] = (char)tolower((unsigned char)ext[i]);
    }
    lo[i] = '\0';
    char probe[kExtMax + 2];
    snprintf(probe, sizeof probe, ".%s", lo);
    if (strcmp(nv_open_mime(probe), kOctet) != 0) return false;   // known: the built-in type wins
    ExtraType &t = s_extra[s_extra_n];
    snprintf(t.ext, sizeof t.ext, "%s", lo);
    snprintf(t.mime, sizeof t.mime, "%s", mime);
    for (char *c = t.mime; *c; c++) *c = (char)tolower((unsigned char)*c);
    t.kind = kind;
    s_extra_n++;
    return true;
}

int nv_open_type_count(void) { return kBuiltinN + s_extra_n; }

bool nv_open_type_at(int index, const char **ext, const char **mime, nv_file_kind_t *kind) {
    const char *e = nullptr, *m = nullptr;
    nv_file_kind_t k = NV_FILE_OTHER;
    if (index >= 0 && index < kBuiltinN) {
        e = kTypes[index].ext; m = kTypes[index].mime; k = kTypes[index].kind;
    } else if (index >= kBuiltinN && index < kBuiltinN + s_extra_n) {
        const ExtraType &t = s_extra[index - kBuiltinN];
        e = t.ext; m = t.mime; k = t.kind;
    } else {
        return false;
    }
    if (ext) *ext = e;
    if (mime) *mime = m;
    if (kind) *kind = k;
    return true;
}

// ================================================================== handlers (public)
bool nv_open_register(const NvOpenHandler *h) {
    if (!h || !h->id || !h->id[0] || strlen(h->id) >= NV_OPEN_ID_MAX || !h->types || !h->types[0])
        return false;
    if (h->role == NV_OPEN_OPENER && (!h->app_id || !h->app_id[0])) return false;
    if (h->role == NV_OPEN_ACTION && !h->run) return false;
    for (int i = 0; i < s_hn; i++)
        if (!strcmp(s_h[i]->id, h->id)) { s_h[i] = h; return true; }
    if (s_hn >= kMaxHandlers) {
        NV_LOGW(TAG, "handler table full, '%s' dropped", h->id);
        return false;
    }
    s_h[s_hn++] = h;
    return true;
}

int nv_open_unregister_app(const char *app_id) {
    if (!app_id || !app_id[0]) return 0;
    int w = 0, removed = 0;
    for (int i = 0; i < s_hn; i++) {
        if (s_h[i]->app_id && !strcmp(s_h[i]->app_id, app_id)) { removed++; continue; }
        s_h[w++] = s_h[i];
    }
    s_hn = w;
    return removed;
}

const NvOpenHandler *nv_open_find(const char *handler_id) {
    if (!handler_id || !handler_id[0]) return nullptr;
    for (int i = 0; i < s_hn; i++)
        if (!strcmp(s_h[i]->id, handler_id)) return s_h[i];
    return nullptr;
}

int nv_open_handlers_mime(const char *mime, nv_open_role_t role, const NvOpenHandler **out, int max) {
    return collect(mime, role, out, max);
}

int nv_open_handlers(const char *path, nv_open_role_t role, const NvOpenHandler **out, int max) {
    return collect(nv_open_mime(path), role, out, max);
}

const NvOpenHandler *nv_open_preferred_mime(const char *mime) {
    const NvOpenHandler *v[2];
    const int n = collect(mime, NV_OPEN_OPENER, v, 2);
    if (n == 0) return nullptr;
    const int d = defs_find(mime);
    if (d >= 0 && !strcmp(v[0]->id, s_defs[d].id)) return v[0];   // user default (still installed)
    if (n == 1 || v[0]->priority > v[1]->priority) return v[0];
    return nullptr;
}

const NvOpenHandler *nv_open_preferred(const char *path) { return nv_open_preferred_mime(nv_open_mime(path)); }

const char *nv_open_handler_label(const NvOpenHandler *h) {
    if (!h) return "";
    if (h->label_id >= 0) return nv_tr((nv_str_id_t)h->label_id);
    if (const NvApp *a = h->app_id ? nv_ui_find_app(h->app_id) : nullptr)
        return a->name_id >= 0 ? nv_tr((nv_str_id_t)a->name_id) : a->name;
    return h->label ? h->label : h->id;
}

const char *nv_open_handler_symbol(const NvOpenHandler *h, const char *mime) {
    if (h && h->symbol) return h->symbol;
    return nv_open_kind_symbol(nv_open_kind_of_mime(mime));
}

// ================================================================== opening (public)
bool nv_open_file(const char *path) {
    if (!path_ok(path)) return false;
    struct stat st;
    if (stat(path, &st) != 0) { nv_toast(NV_NOTE_WARN, nv_tr(NV_STR_FILE_NOT_FOUND)); return false; }
    if (!S_ISDIR(st.st_mode)) {
        const NvOpenHandler *h;
        if (collect(nv_open_mime(path), NV_OPEN_OPENER, &h, 1) == 0) {
            nv_toast(NV_NOTE_WARN, nv_tr(NV_STR_NO_APP_FOR_FILE));
            return false;
        }
    }
    return post(req_new(Op::Open, path, nullptr));
}

bool nv_open_with(const char *path) {
    if (!path_ok(path)) return false;
    const NvOpenHandler *h;
    if (collect(nv_open_mime(path), NV_OPEN_OPENER, &h, 1) == 0) {
        nv_toast(NV_NOTE_WARN, nv_tr(NV_STR_NO_APP_FOR_FILE));
        return false;
    }
    return post(req_new(Op::With, path, nullptr));
}

bool nv_open_run(const NvOpenHandler *h, const char *path) {
    if (!h || !path_ok(path)) return false;
    if (h->role == NV_OPEN_ACTION) return h->run ? h->run(path, h->ctx) : false;
    return post(req_new(Op::Run, path, h->id));
}

bool nv_open_run_id(const char *handler_id, const char *path) {
    return nv_open_run(nv_open_find(handler_id), path);
}

bool nv_open_reveal(const char *path) {
    if (!path_ok(path)) return false;
    return post(req_new(Op::Reveal, path, nullptr));
}

bool nv_open_file_async(const char *path, const char *handler_id) {
    if (!path_ok(path)) return false;
    Req *r = req_new(handler_id && handler_id[0] ? Op::Run : Op::Open, path, handler_id);
    if (!r) return false;
    r->caller_known = false;   // the foreground app when the request lands becomes the caller
    if (!lvgl_port_lock(1000)) { heap_caps_free(r); return false; }
    const lv_result_t res = lv_async_call(req_apply, r);
    lvgl_port_unlock();
    if (res != LV_RESULT_OK) { heap_caps_free(r); return false; }
    return true;
}

// ================================================================== default apps (public)
void nv_open_get_default(const char *mime, char *out_id, size_t n) {
    if (!out_id || !n) return;
    out_id[0] = '\0';
    if (!mime) return;
    const int d = defs_find(mime);
    if (d >= 0) snprintf(out_id, n, "%s", s_defs[d].id);
}

void nv_open_set_default(const char *mime, const char *handler_id) {
    if (!valid_mime(mime, false)) return;
    const int d = defs_find(mime);
    if (!handler_id || !handler_id[0]) {
        if (d < 0) return;
        for (int i = d; i < s_defs_n - 1; i++) s_defs[i] = s_defs[i + 1];
        s_defs_n--;
    } else {
        if (strlen(handler_id) >= NV_OPEN_ID_MAX) return;
        int slot = d;
        if (slot < 0) {
            if (s_defs_n >= kMaxDefs) {   // full: forget the oldest choice
                for (int i = 0; i < s_defs_n - 1; i++) s_defs[i] = s_defs[i + 1];
                s_defs_n--;
            }
            slot = s_defs_n++;
            snprintf(s_defs[slot].mime, NV_OPEN_MIME_MAX, "%s", mime);
        } else if (!strcmp(s_defs[slot].id, handler_id)) {
            return;   // unchanged: no NVS write
        }
        snprintf(s_defs[slot].id, NV_OPEN_ID_MAX, "%s", handler_id);
    }
    defs_save();
}

void nv_open_clear_defaults(void) {
    defs_load();
    if (s_defs_n == 0) return;
    s_defs_n = 0;
    defs_save();
}

void nv_open_pick_default(const char *mime, void (*done)(void *ctx), void *ctx) {
    if (!valid_mime(mime, false)) return;
    chooser_show(ChMode::PickDefault, nullptr, mime, nullptr, done, ctx);
}

// ================================================================== intents (public)
const NvIntent *nv_open_intent(void) {
    const char *cur = cur_app();
    if (!cur[0]) return nullptr;
    if (s_pending.valid && !strcmp(s_pending.target, cur)) return &s_pending.in;
    if (s_active.valid && !strcmp(s_active.target, cur)) return &s_active.in;
    return nullptr;
}

void nv_open_finish(void) {
    Req *r = req_new(Op::Finish, nullptr, nullptr);
    if (!r) { nv_ui_close_app(); return; }
    post(r);
}

void nv_open_intent_drop(void) {
    const char *cur = cur_app();
    if (s_active.valid && cur[0] && !strcmp(s_active.target, cur)) s_active.valid = false;
}

// ================================================================== SystemUI hooks
bool nv_open_on_back(bool at_root) {
    if (s_ch.scrim) { nv_open_dismiss(); return true; }
    if (!at_root) return false;
    const char *cur = cur_app();
    if (!cur[0] || !s_active.valid || strcmp(s_active.target, cur) != 0) return false;
    if (!s_active.in.caller[0] || !nv_ui_find_app(s_active.in.caller)) return false;
    nv_open_finish();
    return true;
}

void nv_open_on_app_closed(const char *app_id) {
    nv_open_dismiss();
    if (app_id && s_active.valid && !strcmp(s_active.target, app_id)) s_active.valid = false;
}

void nv_open_dismiss(void) {
    lv_async_call_cancel(pick_apply, nullptr);
    lv_async_call_cancel(close_apply, nullptr);
    s_ch.pick_pending = s_ch.close_pending = false;
    chooser_close_now();
}
