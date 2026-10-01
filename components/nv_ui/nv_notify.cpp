// nv_notify — system toasts, desktop popups + notification-center ring. See include/nv_notify.h.
// Rendering is layer-free (position animation + solid fills only) — the ESP32-P4 software
// renderer stalls on shadow/transform/opacity draw layers (see memory notes).

#include "nv_notify.h"
#include "nv_ui.h"
#include "nv_ui_internal.h"
#include "nv_ui_scale.h"
#include "nv_theme.h"
#include "nv_fonts.h"
#include "nv_config.h"
#include "nv_time.h"
#include "nv_audio.h"
#include "nv_log.h"
#include "nv_mem_attr.h"

#include "lvgl.h"

#include <cstring>

namespace {

constexpr const char *TAG = "nv_notify";

// ---------------------------------------------------------------- ring store
NV_PSRAM_BSS NvNote s_ring[NV_NOTIFY_CAP];   // LVGL thread only (posters off-thread hold the port lock)
int s_count = 0;      // valid entries
int s_head = 0;       // index of newest entry
int s_unread = 0;
void (*s_listener)(void) = nullptr;
bool s_desktop = false;   // classic desktop shell: posts show as popups over the taskbar

// The same title + text again within this window folds into the stored note without a popup.
constexpr uint32_t kFoldMs = 60 * 1000;

void notify_listener(void) { if (s_listener) s_listener(); }

bool dnd_on(void) { return nv_config_get_bool("qs_dnd", false); }

NvNote *ring_at(int i) { return &s_ring[(s_head + i) % NV_NOTIFY_CAP]; }

// Remove entry i (0 = newest): every older entry moves one step toward the head.
void ring_remove_at(int at) {
    for (int i = at; i < s_count - 1; i++) *ring_at(i) = *ring_at(i + 1);
    s_count--;
    if (s_unread > s_count) s_unread = s_count;
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

void slide_y_cb(void *o, int32_t v) { lv_obj_set_style_translate_y((lv_obj_t *)o, v, 0); }
void slide_x_cb(void *o, int32_t v) { lv_obj_set_style_translate_x((lv_obj_t *)o, v, 0); }

void slide(lv_obj_t *o, lv_anim_exec_xcb_t cb, int32_t from, int32_t to, uint32_t ms, bool in,
           lv_anim_completed_cb_t done) {
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, o);
    lv_anim_set_exec_cb(&a, cb);
    lv_anim_set_values(&a, from, to);
    lv_anim_set_duration(&a, ms);
    lv_anim_set_path_cb(&a, in ? lv_anim_path_ease_out : lv_anim_path_ease_in);
    if (done) lv_anim_set_completed_cb(&a, done);
    lv_anim_start(&a);
}

void delete_done(lv_anim_t *a) { lv_obj_delete((lv_obj_t *)a->var); }

// Async tap: the tapped popup / card may be deleted by what the tap does.
void activate_async(uint32_t id) {
    if (lv_async_call([](void *p) { nv_notify_activate((uint32_t)(uintptr_t)p); },
                      (void *)(uintptr_t)id) != LV_RESULT_OK)
        nv_notify_activate(id);
}

// ---------------------------------------------------------------- pending queue
// Posts that arrive while the snackbar is busy wait here (oldest first) instead of overwriting it.
struct Pending { nv_note_kind_t kind; char title[32]; char text[96]; };
constexpr int kQueueCap = 4;
Pending s_q[kQueueCap];
int s_qn = 0;

void queue_push(nv_note_kind_t kind, const char *title, const char *msg) {
    if (s_qn == kQueueCap) { memmove(&s_q[0], &s_q[1], sizeof(Pending) * (kQueueCap - 1)); s_qn--; }
    Pending &p = s_q[s_qn++];
    p.kind = kind;
    lv_strlcpy(p.title, title ? title : "", sizeof p.title);
    lv_strlcpy(p.text, msg, sizeof p.text);
}

// ---------------------------------------------------------------- snackbar (tablet shell)
lv_obj_t  *s_toast = nullptr;
lv_timer_t *s_toast_timer = nullptr;
uint32_t   s_toast_until = 0;     // lv_tick when the visible toast is due to leave

void toast_show(nv_note_kind_t kind, const char *title, const char *msg);

void toast_next(void) {
    if (!s_qn) return;
    Pending p = s_q[0];
    memmove(&s_q[0], &s_q[1], sizeof(Pending) * (kQueueCap - 1));
    s_qn--;
    toast_show(p.kind, p.title[0] ? p.title : nullptr, p.text);
}

void toast_deleted(lv_event_t *e) {
    if (lv_event_get_target_obj(e) != s_toast) return;
    s_toast = nullptr;
    if (s_toast_timer) { lv_timer_delete(s_toast_timer); s_toast_timer = nullptr; }
}

void toast_dismiss_done(lv_anim_t *a) {
    lv_obj_t *t = (lv_obj_t *)a->var;
    if (t == s_toast) s_toast = nullptr;
    lv_obj_delete(t);
    if (!s_toast) toast_next();
}

void toast_timer_cb(lv_timer_t *tm) {
    lv_timer_delete(tm);
    if (s_toast_timer == tm) s_toast_timer = nullptr;
    if (!s_toast) return;
    slide(s_toast, slide_y_cb, 0, lv_obj_get_height(s_toast) + 60, 180, false, toast_dismiss_done);
}

// Max text column width in PIXELS. Must be px, not a percentage: the toast is width
// LV_SIZE_CONTENT, so a percentage would resolve against a content-sized (zero) parent and
// collapse the label — the exact bug that hid the message text. A px cap lets the message
// wrap to at most a few readable lines instead of one char-per-line sliver (the "too tall").
constexpr int kToastTextMaxPx = 640;

// Core renderer. title == NULL => message-only toast; else a bold source line above the body.
// Replaces whatever toast is visible.
void toast_show(nv_note_kind_t kind, const char *title, const char *msg) {
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

    // On the desktop the taskbar owns the bottom edge: sit above it.
    lv_obj_align(t, LV_ALIGN_BOTTOM_MID, 0, s_desktop ? -(nvclassic::kTaskH + NV_SP_4) : -40);
    lv_obj_update_layout(t);

    // Slide up from below the edge (layer-free translate), hold, then auto-dismiss.
    const int32_t rise = lv_obj_get_height(t) + 60 + (s_desktop ? nvclassic::kTaskH : 0);
    lv_obj_set_style_translate_y(t, rise, 0);
    slide(t, slide_y_cb, rise, 0, 200, true, nullptr);

    s_toast = t;
    // Errors/warnings linger a bit longer so the message can actually be read; a queue behind
    // this one shortens it (see present()).
    uint32_t ms = kind >= NV_NOTE_WARN ? 3500 : 2200;
    if (s_qn) ms = 1600;
    s_toast_until = lv_tick_get() + ms;
    s_toast_timer = lv_timer_create(toast_timer_cb, ms, nullptr);
}

// ---------------------------------------------------------------- popups (classic desktop)
// Cards over the taskbar's right corner, newest at the bottom, at most kPopMax at once: a fourth
// pushes the oldest out. Each one has its own timer, held while the pointer is over it.
constexpr int kPopMax  = 3;
constexpr int kPopW    = 360;
constexpr int kPopGap  = 8;
constexpr int kPopEdge = 12;

struct Pop { lv_obj_t *card; lv_obj_t *close; lv_timer_t *timer; uint32_t id; bool leaving; };
Pop s_pop[kPopMax];   // oldest first
int s_pop_n = 0;

int pop_find_card(lv_obj_t *card) {
    for (int i = 0; i < s_pop_n; i++) if (s_pop[i].card == card) return i;
    return -1;
}

// Stack the visible popups up from the taskbar: the newest sits lowest.
void pop_restack(void) {
    int32_t y = nvclassic::kTaskH + kPopEdge;
    for (int i = s_pop_n - 1; i >= 0; i--) {
        lv_obj_t *c = s_pop[i].card;
        lv_obj_update_layout(c);
        lv_obj_align(c, LV_ALIGN_BOTTOM_RIGHT, -kPopEdge, -y);
        y += lv_obj_get_height(c) + kPopGap;
    }
}

void pop_deleted(lv_event_t *e) {
    const int i = pop_find_card(lv_event_get_target_obj(e));
    if (i < 0) return;
    if (s_pop[i].timer) lv_timer_delete(s_pop[i].timer);
    for (int j = i; j < s_pop_n - 1; j++) s_pop[j] = s_pop[j + 1];
    s_pop_n--;
    pop_restack();
}

void pop_close(int i, bool animated) {
    if (i < 0 || i >= s_pop_n) return;
    Pop &p = s_pop[i];
    if (p.timer) { lv_timer_delete(p.timer); p.timer = nullptr; }
    if (!animated) { lv_obj_delete(p.card); return; }   // DELETE handler unlinks + restacks
    if (p.leaving) return;
    p.leaving = true;
    lv_obj_remove_flag(p.card, LV_OBJ_FLAG_CLICKABLE);
    slide(p.card, slide_x_cb, 0, lv_obj_get_width(p.card) + kPopEdge + 4, 160, false, delete_done);
}

void pop_close_id(uint32_t id) {
    for (int i = 0; i < s_pop_n; i++) if (s_pop[i].id == id) { pop_close(i, true); return; }
}

void pops_clear(void) {
    while (s_pop_n) pop_close(s_pop_n - 1, false);
}

void pop_timer_cb(lv_timer_t *tm) {
    for (int i = 0; i < s_pop_n; i++) {
        Pop &p = s_pop[i];
        if (p.timer != tm) continue;
        // The pointer resting on it (or a press in progress) holds it: read it, then it goes.
        if (lv_obj_has_state(p.card, LV_STATE_HOVERED) || lv_obj_has_state(p.card, LV_STATE_PRESSED) ||
            lv_obj_has_state(p.close, LV_STATE_HOVERED)) {
            lv_timer_set_period(tm, 1500);
            return;
        }
        pop_close(i, true);
        return;
    }
    lv_timer_delete(tm);   // orphan (its popup is gone)
}

void pop_click_cb(lv_event_t *e) {
    const int i = pop_find_card(lv_event_get_current_target_obj(e));
    if (i < 0) return;
    const Pop p = s_pop[i];
    if (p.timer) lv_timer_delete(p.timer);
    // Unlinked now (nothing else may touch it again), but not deleted inside its own CLICKED
    // dispatch: hidden, and gone on the next cycle.
    for (int j = i; j < s_pop_n - 1; j++) s_pop[j] = s_pop[j + 1];
    s_pop_n--;
    lv_obj_add_flag(p.card, LV_OBJ_FLAG_HIDDEN);
    lv_obj_delete_async(p.card);
    pop_restack();
    activate_async(p.id);
}

void pop_close_cb(lv_event_t *e) {
    lv_obj_t *card = lv_obj_get_parent(lv_obj_get_parent(lv_event_get_current_target_obj(e)));
    pop_close(pop_find_card(card), true);   // dismissed from view only: it stays in the shade
}

lv_obj_t *pop_label(lv_obj_t *parent, const char *txt, const lv_font_t *font, lv_color_t c) {
    lv_obj_t *l = lv_label_create(parent);
    lv_label_set_text(l, txt);
    lv_obj_set_style_text_font(l, font, 0);   // explicit: top layer has no inherited font
    lv_obj_set_style_text_color(l, c, 0);
    return l;
}

void popup_show(const NvNote *n) {
    if (n->kind == NV_NOTE_ERROR) nv_audio_alert();
    if (s_pop_n == kPopMax) pop_close(0, false);   // the oldest makes room

    const NvTheme *th = nv_theme_get();
    const int32_t sw = lv_obj_get_width(lv_layer_top());
    const int32_t w = sw - 2 * kPopEdge < kPopW ? sw - 2 * kPopEdge : kPopW;

    lv_obj_t *c = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(c);
    lv_obj_set_width(c, w);
    lv_obj_set_height(c, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(c, th->surface3, 0);
    lv_obj_set_style_bg_color(c, th->surface2, LV_STATE_HOVERED);
    lv_obj_set_style_bg_color(c, th->surface2, LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(c, NV_RAD_SM, 0);
    lv_obj_set_style_border_color(c, th->divider, 0);
    lv_obj_set_style_border_width(c, 1, 0);
    lv_obj_set_style_pad_all(c, NV_SP_3, 0);
    lv_obj_set_style_pad_row(c, NV_SP_1, 0);
    lv_obj_set_flex_flow(c, LV_FLEX_FLOW_COLUMN);
    lv_obj_clear_flag(c, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(c, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(c, pop_click_cb, LV_EVENT_CLICKED, nullptr);
    lv_obj_add_event_cb(c, pop_deleted, LV_EVENT_DELETE, nullptr);

    // Header: [icon] [source .......] [HH:MM] [x]
    lv_obj_t *hdr = lv_obj_create(c);
    lv_obj_remove_style_all(hdr);
    lv_obj_set_size(hdr, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(hdr, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(hdr, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(hdr, NV_SP_2, 0);
    lv_obj_clear_flag(hdr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(hdr, LV_OBJ_FLAG_CLICKABLE);

    const lv_color_t kc = kind_color(n->kind, th);
    pop_label(hdr, kind_symbol(n->kind), th->font_default, kc);
    lv_obj_t *src = pop_label(hdr, n->title, &nv_font_14, kc);
    lv_obj_set_flex_grow(src, 1);
    lv_label_set_long_mode(src, LV_LABEL_LONG_MODE_DOTS);
    char when[16];
    if (n->repeat > 1) lv_snprintf(when, sizeof when, "x%u  %s", (unsigned)n->repeat, n->when);
    else               lv_snprintf(when, sizeof when, "%s", n->when);
    pop_label(hdr, when, &nv_font_14, th->text_dim);

    lv_obj_t *x = lv_obj_create(hdr);
    lv_obj_remove_style_all(x);
    lv_obj_set_size(x, 24, 24);
    lv_obj_set_style_radius(x, 12, 0);
    lv_obj_set_style_bg_color(x, th->surface, LV_STATE_HOVERED);
    lv_obj_set_style_bg_opa(x, LV_OPA_COVER, LV_STATE_HOVERED);
    lv_obj_set_ext_click_area(x, 8);
    lv_obj_add_flag(x, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(x, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(x, pop_close_cb, LV_EVENT_CLICKED, nullptr);
    lv_obj_t *xl = pop_label(x, LV_SYMBOL_CLOSE, &nv_font_14, th->text_dim);
    lv_obj_center(xl);

    lv_obj_t *body = pop_label(c, n->text, th->font_default, th->text_strong);
    lv_obj_set_width(body, lv_pct(100));
    lv_label_set_long_mode(body, LV_LABEL_LONG_MODE_WRAP);

    Pop &p = s_pop[s_pop_n++];
    p.card = c;
    p.close = x;
    p.id = n->id;
    p.leaving = false;
    const uint32_t ms = n->kind == NV_NOTE_ERROR ? 10000 : n->kind == NV_NOTE_WARN ? 7000 : 5000;
    p.timer = lv_timer_create(pop_timer_cb, ms, nullptr);
    pop_restack();

    // Slide in from the right edge (layer-free translate).
    const int32_t off = w + kPopEdge + 4;
    lv_obj_set_style_translate_x(c, off, 0);
    slide(c, slide_x_cb, off, 0, 200, true, nullptr);
}

// ---------------------------------------------------------------- presenter
// A stored post: popup on the desktop, else the snackbar (queued behind a visible one).
void present(const NvNote *n) {
    if (s_desktop) { popup_show(n); return; }
    if (s_toast) {
        queue_push(n->kind, n->title, n->text);
        // Hurry the visible one along so the queue doesn't lag far behind.
        const uint32_t now = lv_tick_get();
        if (s_toast_timer && (int32_t)(s_toast_until - now) > 1200) {
            lv_timer_set_period(s_toast_timer, 1200);
            lv_timer_reset(s_toast_timer);
            s_toast_until = now + 1200;
        }
        return;
    }
    toast_show(n->kind, n->title, n->text);
}

// Should this post interrupt the user? Errors always do; the rest yields to what's on screen.
bool should_pop(nv_note_kind_t kind, const char *app, bool quiet) {
    if (kind == NV_NOTE_ERROR) return true;
    if (quiet || dnd_on()) return false;
    if (nvui::asleep() || nv_ui_is_locked() || nvui::fullscreen()) return false;
    // Its tap would open the app already in front: that app shows the news itself.
    if (app && app[0] && !nvui::minimized() && !strcmp(app, nv_ui_current_app_id())) return false;
    return true;
}

}  // namespace

namespace nvnotify {
void set_desktop(bool on) {
    if (on == s_desktop) return;
    if (!on) pops_clear();
    s_desktop = on;
}
}  // namespace nvnotify

extern "C" {

void nv_toast(nv_note_kind_t kind, const char *msg) {
    if (!msg || !msg[0]) return;
    // DND: silence non-error toasts entirely.
    if (kind != NV_NOTE_ERROR && dnd_on()) return;
    toast_show(kind, nullptr, msg);   // direct feedback on a user action: replaces, never queues
}

void nv_notify_post(nv_note_kind_t kind, const char *title, const char *msg) {
    nv_notify_post_ex(kind, title, msg, nullptr);
}

void nv_notify_post_ex(nv_note_kind_t kind, const char *title, const char *msg, const nv_note_opts_t *o) {
    if (!msg || !msg[0]) return;
    if (!title || !title[0]) title = "System";
    const char *tag  = o && o->tag  ? o->tag  : "";
    const char *app  = o && o->app  ? o->app  : "";
    const char *page = o && o->page ? o->page : "";
    const uint32_t now = lv_tick_get();

    // One live note per subject (tag), and an identical untagged post folds into the stored one.
    uint16_t repeat = 1;
    bool folded_recent = false;
    for (int i = 0; i < s_count; i++) {
        NvNote *n = ring_at(i);
        const bool same_tag = tag[0] && !strcmp(n->tag, tag);
        const bool same = !tag[0] && !n->tag[0] && !strcmp(n->title, title) && !strncmp(n->text, msg, sizeof n->text - 1);
        if (!same_tag && !same) continue;
        if (same) {
            repeat = n->repeat < 999 ? n->repeat + 1 : n->repeat;
            folded_recent = lv_tick_diff(now, n->tick) < kFoldMs;
        }
        pop_close_id(n->id);
        ring_remove_at(i);
        break;
    }

    s_head = (s_head + NV_NOTIFY_CAP - 1) % NV_NOTIFY_CAP;   // step backwards: head = newest
    NvNote *n = &s_ring[s_head];
    n->kind = kind;
    lv_strlcpy(n->title, title, sizeof n->title);
    lv_strlcpy(n->text, msg, sizeof n->text);
    lv_strlcpy(n->tag, tag, sizeof n->tag);
    lv_strlcpy(n->app, app, sizeof n->app);
    lv_strlcpy(n->page, page, sizeof n->page);
    nv_time_format(n->when, sizeof n->when, "%H:%M");
    static uint32_t s_next_id = 1;
    n->id = s_next_id++;
    n->repeat = repeat;
    n->tick = now;
    if (s_count < NV_NOTIFY_CAP) s_count++;
    if (!folded_recent && s_unread < s_count) s_unread++;
    NV_LOGI(TAG, "post [%d] %s: %s%s", (int)kind, n->title, n->text, folded_recent ? " (folded)" : "");

    if (!folded_recent && should_pop(kind, app, o && o->quiet)) present(n);
    notify_listener();                 // badge + live shade list
}

int nv_notify_count(void) { return s_count; }

const NvNote *nv_notify_get(int index) {
    if (index < 0 || index >= s_count) return nullptr;
    return ring_at(index);
}

int nv_notify_unread(void) { return s_unread; }

void nv_notify_mark_read(void) {
    pops_clear();   // the shade shows them now
    if (!s_unread) return;
    s_unread = 0;
    notify_listener();
}

void nv_notify_clear(void) {
    pops_clear();
    s_qn = 0;
    if (!s_count && !s_unread) return;
    s_count = 0;
    s_unread = 0;
    notify_listener();
}

void nv_notify_remove(uint32_t id) {
    for (int i = 0; i < s_count; i++) {
        if (ring_at(i)->id != id) continue;
        pop_close_id(id);
        ring_remove_at(i);
        notify_listener();
        return;
    }
}

void nv_notify_remove_tag(const char *tag) {
    if (!tag || !tag[0]) return;
    for (int i = 0; i < s_count; i++)
        if (!strcmp(ring_at(i)->tag, tag)) { nv_notify_remove(ring_at(i)->id); return; }
}

void nv_notify_activate(uint32_t id) {
    char app[sizeof(NvNote::app)] = "", page[sizeof(NvNote::page)] = "";
    for (int i = 0; i < s_count; i++) {
        const NvNote *n = ring_at(i);
        if (n->id != id) continue;
        lv_strlcpy(app, n->app, sizeof app);
        lv_strlcpy(page, n->page, sizeof page);
        break;
    }
    if (!app[0]) { nvui::open_shade(); return; }   // nothing to open: show it among the others
    nv_notify_remove(id);                          // acted upon: done
    nvui::close_shade();
    const bool ok = page[0] ? nv_ui_open_app_page(app, page) : nv_ui_open_app_id(app);
    if (!ok) NV_LOGW(TAG, "tap: app '%s' not found", app);
}

void nv_notify_set_listener(void (*cb)(void)) { s_listener = cb; }

// Shared severity mapping for the shade list renderer (kept here so it lives exactly once).
const char *nv_note_kind_symbol(nv_note_kind_t k) { return kind_symbol(k); }
lv_color_t  nv_note_kind_color(nv_note_kind_t k)  { return kind_color(k, nv_theme_get()); }

}  // extern "C"
