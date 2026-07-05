// nv_notify — system toasts + notification-center ring. See include/nv_notify.h.
// Toast rendering is layer-free (position animation + solid fills only) — the ESP32-P4
// software renderer stalls on shadow/transform/opacity draw layers (see memory notes).

#include "nv_notify.h"
#include "nv_ui_scale.h"
#include "nv_theme.h"
#include "nv_fonts.h"
#include "nv_config.h"
#include "nv_time.h"
#include "nv_audio.h"
#include "nv_log.h"

#include "lvgl.h"

#include <cstring>

namespace {

constexpr const char *TAG = "nv_notify";

// ---------------------------------------------------------------- ring store
NvNote s_ring[NV_NOTIFY_CAP];
int s_count = 0;      // valid entries
int s_head = 0;       // index of newest entry
int s_unread = 0;
void (*s_listener)(void) = nullptr;

void notify_listener(void) { if (s_listener) s_listener(); }

bool dnd_on(void) { return nv_config_get_bool("qs_dnd", false); }

// ---------------------------------------------------------------- toast (single, replace-on-post)
lv_obj_t  *s_toast = nullptr;
lv_timer_t *s_toast_timer = nullptr;

void toast_slide_cb(void *o, int32_t v) { lv_obj_set_style_translate_y((lv_obj_t *)o, v, 0); }

void toast_deleted(lv_event_t *e) {
    if (lv_event_get_target_obj(e) != s_toast) return;
    s_toast = nullptr;
    if (s_toast_timer) { lv_timer_delete(s_toast_timer); s_toast_timer = nullptr; }
}

void toast_dismiss_done(lv_anim_t *a) {
    lv_obj_t *t = (lv_obj_t *)a->var;
    if (t == s_toast) s_toast = nullptr;
    lv_obj_delete(t);
}

void toast_timer_cb(lv_timer_t *tm) {
    lv_timer_delete(tm);
    if (s_toast_timer == tm) s_toast_timer = nullptr;
    if (!s_toast) return;
    const int32_t drop = lv_obj_get_height(s_toast) + 60;
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, s_toast);
    lv_anim_set_exec_cb(&a, toast_slide_cb);
    lv_anim_set_values(&a, 0, drop);
    lv_anim_set_duration(&a, 180);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_in);
    lv_anim_set_completed_cb(&a, toast_dismiss_done);
    lv_anim_start(&a);
}

// Severity -> stripe color + glyph (theme tokens only; no hardcoded hues).
lv_color_t kind_color(nv_note_kind_t k, const NvTheme *th) {
    switch (k) {
        case NV_NOTE_OK:    return th->success_solid;
        case NV_NOTE_WARN:
        case NV_NOTE_ERROR: return th->danger;
        default:            return th->accent;
    }
}
const char *kind_symbol(nv_note_kind_t k) {
    switch (k) {
        case NV_NOTE_OK:    return LV_SYMBOL_OK;
        case NV_NOTE_WARN:  return LV_SYMBOL_WARNING;
        case NV_NOTE_ERROR: return LV_SYMBOL_CLOSE;
        default:            return LV_SYMBOL_BELL;
    }
}

// Max text column width in PIXELS. Must be px, not a percentage: the toast is width
// LV_SIZE_CONTENT, so a percentage would resolve against a content-sized (zero) parent and
// collapse the label — the exact bug that hid the message text. A px cap lets the message
// wrap to at most a few readable lines instead of one char-per-line sliver (the "too tall").
constexpr int kToastTextMaxPx = 640;

// Core renderer. title == NULL => message-only toast; else a bold source line above the body.
void toast_show(nv_note_kind_t kind, const char *title, const char *msg) {
    if (!msg || !msg[0]) return;
    // DND: silence non-error toasts entirely (posts still land in the shade).
    if (kind != NV_NOTE_ERROR && dnd_on()) return;
    if (kind == NV_NOTE_ERROR) nv_audio_alert();   // gated internally by mute

    // Replace any visible toast immediately (its DELETE handler clears the statics + timer).
    if (s_toast) { lv_obj_delete(s_toast); s_toast = nullptr; }
    if (s_toast_timer) { lv_timer_delete(s_toast_timer); s_toast_timer = nullptr; }

    const NvTheme *th = nv_theme_get();
    // Parent = TOP LAYER, not the active screen: guarantees the toast sits above the IME
    // keyboard, the shade, the edge strips and any open app — a screen child could be covered.
    lv_obj_t *t = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(t);
    lv_obj_set_style_bg_color(t, th->surface3, 0);
    lv_obj_set_style_bg_opa(t, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(t, NV_RAD_MD, 0);
    // Hairline full border for contrast on any background (layer-free, unlike a shadow).
    // Severity is conveyed by the colored icon + title line, not a border color.
    lv_obj_set_style_border_color(t, th->divider, 0);
    lv_obj_set_style_border_width(t, 1, 0);
    lv_obj_set_style_pad_hor(t, NV_SP_4, 0);
    lv_obj_set_style_pad_ver(t, NV_SP_3, 0);
    lv_obj_set_style_pad_column(t, NV_SP_3, 0);
    lv_obj_set_size(t, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_max_width(t, lv_pct(88), 0);       // top layer is screen-sized: pct is safe here
    lv_obj_set_flex_flow(t, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(t, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(t, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(t, LV_OBJ_FLAG_CLICKABLE);        // never eats touches (top layer is global)
    lv_obj_add_event_cb(t, toast_deleted, LV_EVENT_DELETE, nullptr);

    // Severity glyph, tinted by kind (info/ok/warn/error).
    lv_obj_t *ico = lv_label_create(t);
    lv_label_set_text(ico, kind_symbol(kind));
    lv_obj_set_style_text_font(ico, th->font_default, 0);  // explicit: top layer has no inherited font
    lv_obj_set_style_text_color(ico, kind_color(kind, th), 0);

    // Text column (title optional + message). Content-sized; the message label carries the
    // px cap + wrap, so the column hugs it and the whole toast height stays bounded.
    lv_obj_t *col = lv_obj_create(t);
    lv_obj_remove_style_all(col);
    lv_obj_set_size(col, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(col, 2, 0);
    lv_obj_clear_flag(col, LV_OBJ_FLAG_SCROLLABLE);

    if (title && title[0]) {
        lv_obj_t *tl = lv_label_create(col);
        lv_label_set_text(tl, title);
        lv_obj_set_style_text_font(tl, &nv_font_14, 0);
        lv_obj_set_style_text_color(tl, kind_color(kind, th), 0);   // colored source line
    }

    lv_obj_t *l = lv_label_create(col);
    lv_label_set_text(l, msg);
    lv_obj_set_style_text_color(l, th->text_strong, 0);
    lv_obj_set_style_text_font(l, th->font_default, 0);  // explicit: top layer has no inherited font
    lv_obj_set_style_max_width(l, kToastTextMaxPx, 0);   // px cap (see constant note)
    lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_WRAP);

    lv_obj_align(t, LV_ALIGN_BOTTOM_MID, 0, -40);
    lv_obj_update_layout(t);

    // Slide up from below the edge (layer-free translate), hold, then auto-dismiss.
    const int32_t rise = lv_obj_get_height(t) + 60;
    lv_obj_set_style_translate_y(t, rise, 0);
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, t);
    lv_anim_set_exec_cb(&a, toast_slide_cb);
    lv_anim_set_values(&a, rise, 0);
    lv_anim_set_duration(&a, 200);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_start(&a);

    s_toast = t;
    // Errors/warnings linger a bit longer so the message can actually be read.
    s_toast_timer = lv_timer_create(toast_timer_cb, kind >= NV_NOTE_WARN ? 3500 : 2200, nullptr);
}

}  // namespace

extern "C" {

void nv_toast(nv_note_kind_t kind, const char *msg) { toast_show(kind, nullptr, msg); }

void nv_notify_post(nv_note_kind_t kind, const char *title, const char *msg) {
    if (!msg || !msg[0]) return;

    s_head = (s_head + NV_NOTIFY_CAP - 1) % NV_NOTIFY_CAP;   // step backwards: head = newest
    NvNote *n = &s_ring[s_head];
    n->kind = kind;
    lv_strlcpy(n->title, (title && title[0]) ? title : "System", sizeof n->title);
    lv_strlcpy(n->text, msg, sizeof n->text);
    nv_time_format(n->when, sizeof n->when, "%H:%M");
    if (s_count < NV_NOTIFY_CAP) s_count++;
    if (s_unread < NV_NOTIFY_CAP) s_unread++;
    NV_LOGI(TAG, "post [%d] %s: %s", (int)kind, n->title, n->text);

    toast_show(kind, n->title, msg);   // toast shows the source title; DND/sound policy inside
    notify_listener();                 // badge + live shade list
}

int nv_notify_count(void) { return s_count; }

const NvNote *nv_notify_get(int index) {
    if (index < 0 || index >= s_count) return nullptr;
    return &s_ring[(s_head + index) % NV_NOTIFY_CAP];
}

int nv_notify_unread(void) { return s_unread; }

void nv_notify_mark_read(void) {
    if (!s_unread) return;
    s_unread = 0;
    notify_listener();
}

void nv_notify_clear(void) {
    if (!s_count && !s_unread) return;
    s_count = 0;
    s_unread = 0;
    notify_listener();
}

void nv_notify_set_listener(void (*cb)(void)) { s_listener = cb; }

// Shared severity mapping for the shade list renderer (kept here so it lives exactly once).
const char *nv_note_kind_symbol(nv_note_kind_t k) { return kind_symbol(k); }
lv_color_t  nv_note_kind_color(nv_note_kind_t k)  { return kind_color(k, nv_theme_get()); }

}  // extern "C"
