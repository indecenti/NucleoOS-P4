// nv_ime — the shared SystemUI on-screen keyboard (IME).
//
// One hidden lv_keyboard is parented to the active screen, docked at the bottom (~42% high),
// and parked off-screen. nv_ime_bind[_ex]() wires each textarea so tapping it (LV_EVENT_FOCUSED,
// which a touch/pointer indev fires without any group — LVGL sets CLICK_FOCUSABLE on every
// object by default) binds the keyboard to that field and slides it up; tapping elsewhere
// (LV_EVENT_DEFOCUSED) or the keyboard's checkmark/close (LV_EVENT_READY / LV_EVENT_CANCEL)
// slides it back down. Keyboard keys are non-click-focusable (LVGL removes that flag in the
// keyboard constructor), so typing never defocuses the field.
//
// Per-field behavior: a small registry maps each bound textarea to an input class
// (nv_ime_type_t) and a return-key action (nv_ime_return_t). On focus the class picks the key
// plane (numeric keypad for NUMBER/PIN, letters otherwise) and auto-capitalizes the first
// letter of TEXT fields; the return key then dismisses, advances to the next field (NEXT), or
// fires the submit callback (GO/SEARCH/SEND). Field masking / accepted-chars / one-line are set
// once from the class at bind time.
//
// Convenience while typing: backspace and the ◀ ▶ cursor keys auto-repeat when held; after the
// first character of an auto-capitalized field the plane drops back to lowercase (one-shot
// shift, as on a phone).
//
// OS integration: every show/hide publishes NV_EV_IME_VISIBILITY {visible,height} on the event
// bus so any app or overlay can reflow around the keyboard, independent of the focused field's
// own parent-padding shift (below).
//
// Dangling safety: the keyboard stores a raw textarea pointer. Each bound field also carries an
// LV_EVENT_DELETE handler that, if the deleted field is the active one, clears the binding and
// hides — and drops it from the registry — so a teardown can never leave a stale pointer that
// the next key press would dereference.
//
// Theme-aware: colored from nv_theme_get() tokens; nv_ime_retheme() re-applies.
// Language-aware: nv_ime_relayout() installs a custom QWERTY/QWERTZ/AZERTY map for the active
// language (LOWER + UPPER planes; SPECIAL + NUMBER stay at LVGL defaults). The extra accent row
// (Latin-1 diacritics) renders via the nv_font_* fonts, which carry Latin-1.
#include "nv_ime.h"
#include "nv_keyboard_layouts.h"  // language-aware maps (C TU; see the file's header comment)
#include "nv_ui.h"                // nv_ui_shade_is_open()

#include "lvgl.h"
#include <string.h>   // clipboard: memcpy / strlen
#include <stdlib.h>
#include "nv_clipboard.h"   // the system clipboard
#include "nv_app.h"         // nv_ui_current_app: who copied
static bool ime_sel_delete(lv_obj_t *ta);   // a selection, deleted (Backspace / Delete)
#include "nv_theme.h"
#include "nv_audio.h"   // soft key-press tick
#include "nv_i18n.h"
#include "nv_fonts.h"   // nv_font_20 carries Latin-1 accents (built-in montserrat is ASCII only)
#include "nv_event_bus.h"
#include "nv_hid_host.h" // physical keyboard present -> keep the on-screen one down

namespace {

// ~42% of the display height, docked bottom.
constexpr int   kKbHeightPct = 42;
constexpr uint32_t kSlideMs  = 200;

lv_obj_t *s_kb = nullptr;   // the single shared keyboard

// ---- per-field registry: input class + return action, keyed by the bound textarea ----------
constexpr int kMaxFields = 24;
struct FieldCfg { lv_obj_t *ta; uint8_t type; uint8_t ret; nv_ime_key_hook_t hook; };
FieldCfg s_fields[kMaxFields] = {};

FieldCfg *field_find(lv_obj_t *ta) {
    for (auto &f : s_fields) if (f.ta == ta) return &f;
    return nullptr;
}
void field_store(lv_obj_t *ta, nv_ime_type_t type, nv_ime_return_t ret) {
    FieldCfg *f = field_find(ta);
    if (!f) for (auto &c : s_fields) if (!c.ta) { f = &c; break; }
    if (!f) return;  // registry full — field still works, just as TEXT/DEFAULT via lookup default
    f->ta = ta; f->type = (uint8_t)type; f->ret = (uint8_t)ret;   // a set hook stays
}
void field_drop(lv_obj_t *ta) {
    if (FieldCfg *f = field_find(ta)) *f = {};
}
nv_ime_type_t   field_type(lv_obj_t *ta) { FieldCfg *f = field_find(ta); return f ? (nv_ime_type_t)f->type : NV_IME_TEXT; }
nv_ime_return_t field_ret (lv_obj_t *ta) { FieldCfg *f = field_find(ta); return f ? (nv_ime_return_t)f->ret : NV_IME_RET_DEFAULT; }

// The active field's return action, captured on focus so the keyboard's READY handler (which
// only knows the keyboard) can act without re-reading the registry.
nv_ime_return_t s_active_ret = NV_IME_RET_DEFAULT;

nv_ime_submit_cb_t s_submit_cb   = nullptr;
void              *s_submit_user = nullptr;

// While the keyboard is up we pad the focused field's parent by the keyboard height, so a
// full-height field (e.g. Notes) shrinks/scrolls fully above the keyboard instead of being
// covered. Restored on hide. Guarded with lv_obj_is_valid in case the parent is torn down.
lv_obj_t *s_shift_target = nullptr;
int32_t   s_shift_saved  = 0;

void field_shift_clear(void) {
    if (s_shift_target && lv_obj_is_valid(s_shift_target))
        lv_obj_set_style_pad_bottom(s_shift_target, s_shift_saved, LV_PART_MAIN);
    s_shift_target = nullptr;
}
void field_shift_apply(lv_obj_t *ta) {
    field_shift_clear();
    lv_obj_t *p = lv_obj_get_parent(ta);
    if (!p) return;
    s_shift_target = p;
    s_shift_saved  = lv_obj_get_style_pad_bottom(p, LV_PART_MAIN);
    lv_obj_set_style_pad_bottom(p, s_shift_saved + lv_obj_get_height(s_kb), LV_PART_MAIN);
}

// ---------------------------------------------------------------- OS event
void publish_visibility(bool visible, int32_t height) {
    nv_ime_visibility_t v = { visible, height };
    nv_event_publish(NV_EV_IME_VISIBILITY, &v);
}

// ---------------------------------------------------------------- show / hide + slide anim
bool kb_is_hidden(void) { return lv_obj_has_flag(s_kb, LV_OBJ_FLAG_HIDDEN); }

void kb_anim_y_cb(void *obj, int32_t v) { lv_obj_set_y((lv_obj_t *)obj, v); }

void kb_hide_done(lv_anim_t *a) {
    lv_obj_add_flag((lv_obj_t *)a->var, LV_OBJ_FLAG_HIDDEN);
    publish_visibility(false, 0);
}

void kb_slide_up(void) {
    // Cancel any slide in flight (e.g. a hide just started by a previous field's DEFOCUSED) so a
    // field switch while the keyboard is up animates from the current y instead of snapping down.
    lv_anim_delete(s_kb, kb_anim_y_cb);
    lv_obj_remove_flag(s_kb, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_kb);  // overlay launcher/shade/app plane
    lv_obj_update_layout(s_kb);    // resolve pct height before measuring

    const int32_t scr_h  = lv_display_get_vertical_resolution(lv_display_get_default());
    const int32_t h      = lv_obj_get_height(s_kb);
    const int32_t target = scr_h - h;               // docked at bottom
    const int32_t start  = lv_obj_get_y(s_kb);
    publish_visibility(true, h);
    if (start == target) return;                    // already docked (field switch) — no re-slide

    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, s_kb);
    lv_anim_set_exec_cb(&a, kb_anim_y_cb);
    lv_anim_set_values(&a, start, target);
    lv_anim_set_duration(&a, kSlideMs);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_start(&a);
}

void kb_slide_down(void) {
    field_shift_clear();  // give the focused field's parent its height back
    if (kb_is_hidden()) return;
    lv_anim_delete(s_kb, kb_anim_y_cb);
    const int32_t scr_h = lv_display_get_vertical_resolution(nullptr);
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, s_kb);
    lv_anim_set_exec_cb(&a, kb_anim_y_cb);
    lv_anim_set_values(&a, lv_obj_get_y(s_kb), scr_h);  // slide fully off-screen
    lv_anim_set_duration(&a, kSlideMs);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_in);
    lv_anim_set_completed_cb(&a, kb_hide_done);          // add HIDDEN + publish only after slide-out
    lv_anim_start(&a);
}

// ---------------------------------------------------------------- input class -> key plane
bool field_is_empty(lv_obj_t *ta) {
    const char *t = lv_textarea_get_text(ta);
    return !t || t[0] == '\0';
}

// Configure the field itself (masking / accepted chars / single-line) from its input class.
// Run once at bind time. TEXT leaves the field as the caller made it (Notes stays multi-line).
void configure_field(lv_obj_t *ta, nv_ime_type_t type) {
    switch (type) {
        case NV_IME_PASSWORD:
            lv_textarea_set_password_mode(ta, true);
            lv_textarea_set_one_line(ta, true);
            break;
        case NV_IME_PIN:
            lv_textarea_set_password_mode(ta, true);
            lv_textarea_set_one_line(ta, true);
            lv_textarea_set_accepted_chars(ta, "0123456789");
            break;
        case NV_IME_NUMBER:
            lv_textarea_set_one_line(ta, true);
            lv_textarea_set_accepted_chars(ta, "0123456789.,-+");
            break;
        case NV_IME_EMAIL:
        case NV_IME_URL:
            lv_textarea_set_one_line(ta, true);
            break;
        case NV_IME_TEXT:
        default:
            break;
    }
}

// Pick the key plane on focus: numeric keypad for NUMBER/PIN, else letters with the first
// character auto-capitalized on an empty TEXT field (one-shot; see the value-changed handler).
void apply_plane_on_focus(lv_obj_t *ta, nv_ime_type_t type) {
    if (type == NV_IME_NUMBER || type == NV_IME_PIN) {
        lv_keyboard_set_mode(s_kb, LV_KEYBOARD_MODE_NUMBER);
        return;
    }
    const bool cap = (type == NV_IME_TEXT) && field_is_empty(ta);
    lv_keyboard_set_mode(s_kb, cap ? LV_KEYBOARD_MODE_TEXT_UPPER : LV_KEYBOARD_MODE_TEXT_LOWER);
}

// Bring `ta` into focus programmatically (used by the NEXT return key) without a hide/show flap:
// rebind, reapply the plane + parent shift, keep the keyboard docked. LVGL fires no FOCUSED for
// a programmatic move, so hand off the visible focus state (cursor) from the old field to `ta`.
void focus_field(lv_obj_t *ta) {
    lv_obj_t *old = lv_keyboard_get_textarea(s_kb);
    if (old && old != ta) lv_obj_remove_state(old, LV_STATE_FOCUSED);
    lv_keyboard_set_textarea(s_kb, ta);
    lv_obj_add_state(ta, LV_STATE_FOCUSED);
    apply_plane_on_focus(ta, field_type(ta));
    s_active_ret = field_ret(ta);
    kb_slide_up();                      // idempotent: already docked -> no re-slide
    field_shift_apply(ta);
    lv_obj_scroll_to_view(ta, LV_ANIM_ON);
}

// The next registered (bound) field after `cur` within the same parent, in child order.
lv_obj_t *next_bound_field(lv_obj_t *cur) {
    lv_obj_t *p = lv_obj_get_parent(cur);
    if (!p) return nullptr;
    const uint32_t n = lv_obj_get_child_count(p);
    bool after = false;
    for (uint32_t i = 0; i < n; i++) {
        lv_obj_t *c = lv_obj_get_child(p, i);
        if (c == cur) { after = true; continue; }
        if (after && field_find(c)) return c;
    }
    return nullptr;
}

// ---------------------------------------------------------------- keyboard READY / CANCEL
// The return key's action, shared by the on-screen OK key and the remote ENTER
// (nv_ime_inject_key): advance to the next field, fire the submit callback, or dismiss.
// `remote`: a hardware/KeyDeck Enter (nv_ime_inject_key). The on-screen OK key has already sent
// LV_EVENT_READY to the bound field itself (lv_keyboard), a remote Enter has not.
void ready_action(bool remote) {
    lv_obj_t *ta = lv_keyboard_get_textarea(s_kb);
    switch (s_active_ret) {
        case NV_IME_RET_ENTER:
            if (remote && ta) lv_obj_send_event(ta, LV_EVENT_READY, nullptr);
            return;   // keyboard stays up for the next line
        case NV_IME_RET_NEXT:
            if (ta) {
                if (lv_obj_t *nx = next_bound_field(ta)) { focus_field(nx); return; }
            }
            break;  // no next field -> fall through and dismiss
        case NV_IME_RET_GO:
        case NV_IME_RET_SEARCH:
        case NV_IME_RET_SEND:
            if (s_submit_cb && ta) s_submit_cb(ta, s_submit_user);
            // A physical Enter with no submit callback: the field's READY handler is the action
            // (what the on-screen OK key would have sent).
            else if (remote && ta) lv_obj_send_event(ta, LV_EVENT_READY, nullptr);
            break;
        case NV_IME_RET_DEFAULT:
        case NV_IME_RET_DONE:
        default:
            if (remote && ta) lv_obj_send_event(ta, LV_EVENT_READY, nullptr);
            break;
    }
    kb_slide_down();
    lv_keyboard_set_textarea(s_kb, nullptr);
}

void kb_ready_cb(lv_event_t *) { ready_action(false); }

void kb_cancel_cb(lv_event_t *) {
    kb_slide_down();
    lv_keyboard_set_textarea(s_kb, nullptr);  // unbind -> clears the field's FOCUSED state
}

// ---------------------------------------------------------------- convenience while typing
// Which control tokens are NOT insertable characters (so auto-lower ignores them).
bool is_char_key(const char *txt) {
    return lv_strcmp(txt, "abc") && lv_strcmp(txt, "ABC") && lv_strcmp(txt, "1#") &&
           lv_strcmp(txt, LV_SYMBOL_BACKSPACE) && lv_strcmp(txt, LV_SYMBOL_NEW_LINE) &&
           lv_strcmp(txt, LV_SYMBOL_LEFT)      && lv_strcmp(txt, LV_SYMBOL_RIGHT)    &&
           lv_strcmp(txt, LV_SYMBOL_OK)        && lv_strcmp(txt, LV_SYMBOL_KEYBOARD);
}

// After the default handler inserts a character while in UPPER, drop back to LOWER — so the
// auto-capitalized first letter (and any manual shift) is one-shot, like a phone keyboard.
void kb_value_changed_cb(lv_event_t *) {
    nv_audio_click();   // soft tick on every key (no-op when muted / no speaker)
    if (lv_keyboard_get_mode(s_kb) != LV_KEYBOARD_MODE_TEXT_UPPER) return;
    const uint32_t id = lv_keyboard_get_selected_button(s_kb);
    const char *txt = lv_keyboard_get_button_text(s_kb, id);
    if (txt && is_char_key(txt))
        lv_keyboard_set_mode(s_kb, LV_KEYBOARD_MODE_TEXT_LOWER);
}

// Hold-to-repeat for backspace and the cursor keys.
void kb_long_repeat_cb(lv_event_t *) {
    lv_obj_t *ta = lv_keyboard_get_textarea(s_kb);
    if (!ta) return;
    const uint32_t id = lv_keyboard_get_selected_button(s_kb);
    const char *txt = lv_keyboard_get_button_text(s_kb, id);
    if (!txt) return;
    if      (!lv_strcmp(txt, LV_SYMBOL_BACKSPACE)) { if (!ime_sel_delete(ta)) lv_textarea_delete_char(ta); }
    else if (!lv_strcmp(txt, LV_SYMBOL_LEFT))      lv_textarea_cursor_left(ta);
    else if (!lv_strcmp(txt, LV_SYMBOL_RIGHT))     lv_textarea_cursor_right(ta);
}

// ---------------------------------------------------------------- per-textarea events
// When a field was focused with a physical keyboard present (the tap that focuses it also ends in
// SHORT_CLICKED: only a later tap may raise the on-screen keyboard).
uint32_t s_hw_focus_tick = 0;

void ta_event_cb(lv_event_t *e) {
    const lv_event_code_t code = lv_event_get_code(e);
    lv_obj_t *ta = lv_event_get_target_obj(e);

    if (code == LV_EVENT_FOCUSED) {
        lv_obj_add_state(ta, LV_STATE_FOCUSED);   // the cursor shows (see field_edit_styles)
        if (nv_ui_shade_is_open()) return;  // never raise the keyboard over the open shade
        // Tapped this field: bind + open the class-appropriate plane, then slide up.
        lv_keyboard_set_textarea(s_kb, ta);
        apply_plane_on_focus(ta, field_type(ta));
        s_active_ret = field_ret(ta);
        // A physical keyboard (USB / Bluetooth) types into the bound field already: don't cover
        // 42% of the screen with a second one. Tapping the focused field again brings it up.
        if (nv_hid_host_keyboard_present()) {
            s_hw_focus_tick = lv_tick_get();
            kb_slide_down();
            lv_obj_scroll_to_view(ta, LV_ANIM_ON);
            return;
        }
        kb_slide_up();
        // Shrink the field's parent by the keyboard height so the field sits fully above it,
        // then scroll the cursor line into the now-reduced area.
        field_shift_apply(ta);
        lv_obj_scroll_to_view(ta, LV_ANIM_ON);
    } else if (code == LV_EVENT_SHORT_CLICKED) {
        // Physical keyboard case: the field is bound but the on-screen keyboard stayed down.
        if (lv_keyboard_get_textarea(s_kb) == ta && kb_is_hidden() && !nv_ui_shade_is_open() &&
            lv_tick_elaps(s_hw_focus_tick) > 500) {
            kb_slide_up();
            field_shift_apply(ta);
            lv_obj_scroll_to_view(ta, LV_ANIM_ON);
        }
    } else if (code == LV_EVENT_DEFOCUSED) {
        lv_obj_remove_state(ta, LV_STATE_FOCUSED);
        // Tapped elsewhere (another focusable obj / empty background): slide down + unbind.
        if (lv_keyboard_get_textarea(s_kb) == ta) {
            kb_slide_down();
            lv_keyboard_set_textarea(s_kb, nullptr);
        }
    } else if (code == LV_EVENT_DELETE) {
        // Dangling-safe: if the active field is being deleted, drop the binding + hide.
        if (lv_keyboard_get_textarea(s_kb) == ta) {
            lv_keyboard_set_textarea(s_kb, nullptr);
            kb_slide_down();
        }
        field_drop(ta);
    }
}

// ---------------------------------------------------------------- theming
void apply_theme(void) {
    const NvTheme *th = nv_theme_get();
    // Keyboard body.
    lv_obj_set_style_bg_color(s_kb, th->surface, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_kb, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(s_kb, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(s_kb, 8, LV_PART_MAIN);
    lv_obj_set_style_pad_gap(s_kb, 7, LV_PART_MAIN);
    // Keys (button-matrix items). nv_font_20 so accented keys (à é ñ ç …) render instead of tofu.
    lv_obj_set_style_text_font(s_kb, &nv_font_20, LV_PART_ITEMS);
    lv_obj_set_style_bg_color(s_kb, th->surface2, LV_PART_ITEMS);
    lv_obj_set_style_bg_opa(s_kb, LV_OPA_COVER, LV_PART_ITEMS);
    lv_obj_set_style_text_color(s_kb, th->text_strong, LV_PART_ITEMS);
    lv_obj_set_style_radius(s_kb, 10, LV_PART_ITEMS);
    lv_obj_set_style_border_width(s_kb, 0, LV_PART_ITEMS);
    // Pressed: brand highlight with legible ink. (cast: enum|enum selector is deprecated in C++20)
    lv_obj_set_style_bg_color(s_kb, th->primary, (uint32_t)LV_PART_ITEMS | (uint32_t)LV_STATE_PRESSED);
    lv_obj_set_style_text_color(s_kb, th->on_primary, (uint32_t)LV_PART_ITEMS | (uint32_t)LV_STATE_PRESSED);
    // Control / special keys (CHECKED = mode/space/OK/punct): a distinct darker key with light
    // text — the old primary fill washed out to near-white and swallowed the glyphs.
    lv_obj_set_style_bg_color(s_kb, th->surface3, (uint32_t)LV_PART_ITEMS | (uint32_t)LV_STATE_CHECKED);
    lv_obj_set_style_text_color(s_kb, th->text_strong, (uint32_t)LV_PART_ITEMS | (uint32_t)LV_STATE_CHECKED);
}

// ---------------------------------------------------------------- language map
void apply_language(void) {
    // QWERTZ for DE, AZERTY for FR, else QWERTY — resolved in the C layouts TU. SPECIAL +
    // NUMBER planes intentionally stay at LVGL defaults.
    nv_kb_layout_t lay;
    nv_kb_layout_get((int)nv_i18n_get_lang(), &lay);
    lv_keyboard_set_map(s_kb, LV_KEYBOARD_MODE_TEXT_LOWER, lay.lc_map, lay.lc_ctrl);
    lv_keyboard_set_map(s_kb, LV_KEYBOARD_MODE_TEXT_UPPER, lay.uc_map, lay.uc_ctrl);
    lv_keyboard_set_mode(s_kb, LV_KEYBOARD_MODE_TEXT_LOWER);
}

}  // namespace

// ================================================================= public API
void nv_ime_init(lv_obj_t *screen) {
    if (s_kb) return;  // idempotent

    s_kb = lv_keyboard_create(screen);
    lv_obj_set_height(s_kb, lv_pct(kKbHeightPct));   // ~42% of the screen
    lv_obj_set_width(s_kb, lv_pct(100));
    // Absolute positioning: the slide animation drives y directly. (A BOTTOM_MID align would add
    // its own offset on top of every set_y, pushing the keyboard off-screen -> never visible.)
    lv_obj_set_align(s_kb, LV_ALIGN_TOP_LEFT);
    lv_obj_set_x(s_kb, 0);
    lv_keyboard_set_textarea(s_kb, nullptr);          // not bound yet
    lv_obj_add_flag(s_kb, LV_OBJ_FLAG_HIDDEN);        // start hidden
    // Park off-screen so the first slide-up starts from below the display.
    lv_obj_set_y(s_kb, lv_display_get_vertical_resolution(lv_display_get_default()));

    lv_obj_add_event_cb(s_kb, kb_ready_cb,          LV_EVENT_READY,               nullptr);
    lv_obj_add_event_cb(s_kb, kb_cancel_cb,         LV_EVENT_CANCEL,              nullptr);
    lv_obj_add_event_cb(s_kb, kb_value_changed_cb,  LV_EVENT_VALUE_CHANGED,       nullptr);
    lv_obj_add_event_cb(s_kb, kb_long_repeat_cb,    LV_EVENT_LONG_PRESSED_REPEAT, nullptr);

    apply_theme();
    apply_language();
}

void nv_ime_bind(lv_obj_t *textarea) {
    nv_ime_bind_ex(textarea, NV_IME_TEXT, NV_IME_RET_DEFAULT);
}

// Every bound field: a visible blinking cursor while it has the focus, mouse-drag text selection
// painted in the accent. (The IME binds fields without LVGL's own focus state, which is what the
// theme's cursor style keys on — so the cursor never showed anywhere.)
static void field_edit_styles(lv_obj_t *ta) {
    const NvTheme *th = nv_theme_get();
    lv_textarea_set_text_selection(ta, true);
    lv_obj_set_style_border_side(ta, LV_BORDER_SIDE_LEFT, LV_PART_CURSOR | LV_STATE_FOCUSED);
    lv_obj_set_style_border_width(ta, 2, LV_PART_CURSOR | LV_STATE_FOCUSED);
    lv_obj_set_style_border_color(ta, th->accent, LV_PART_CURSOR | LV_STATE_FOCUSED);
    lv_obj_set_style_border_opa(ta, LV_OPA_COVER, LV_PART_CURSOR | LV_STATE_FOCUSED);
    lv_obj_set_style_bg_color(ta, th->accent, LV_PART_SELECTED);
    lv_obj_set_style_text_color(ta, th->on_primary, LV_PART_SELECTED);
}

void nv_ime_bind_ex(lv_obj_t *textarea, nv_ime_type_t type, nv_ime_return_t ret) {
    if (!textarea) return;
    field_edit_styles(textarea);
    configure_field(textarea, type);
    field_store(textarea, type, ret);
    lv_obj_add_event_cb(textarea, ta_event_cb, LV_EVENT_FOCUSED,   nullptr);
    lv_obj_add_event_cb(textarea, ta_event_cb, LV_EVENT_DEFOCUSED, nullptr);
    lv_obj_add_event_cb(textarea, ta_event_cb, LV_EVENT_SHORT_CLICKED, nullptr);
    lv_obj_add_event_cb(textarea, ta_event_cb, LV_EVENT_DELETE,    nullptr);
}

void nv_ime_set_submit_cb(nv_ime_submit_cb_t cb, void *user) {
    s_submit_cb   = cb;
    s_submit_user = user;
}

void nv_ime_hide(void) {
    if (!s_kb) return;
    kb_slide_down();
    lv_keyboard_set_textarea(s_kb, nullptr);
}

void nv_ime_retheme(void) {
    if (s_kb) apply_theme();
}

void nv_ime_relayout(void) {
    if (s_kb) apply_language();
}

// ── Remote input injection (KeyDeck) ── LVGL-thread only; see the header contract.
void nv_ime_set_key_hook(lv_obj_t *ta, nv_ime_key_hook_t hook) {
    FieldCfg *f = field_find(ta);
    if (!f) {
        field_store(ta, NV_IME_TEXT, NV_IME_RET_DEFAULT);
        f = field_find(ta);
    }
    if (f) f->hook = hook;
}

// ---- clipboard + selection
namespace {

// Byte offset of character `ci` in a UTF-8 string.
size_t utf8_byte(const char *t, uint32_t ci) {
    size_t b = 0;
    for (uint32_t c = 0; t[b] && c < ci; c++) {
        b++;
        while (t[b] && ((unsigned char)t[b] & 0xC0) == 0x80) b++;
    }
    return b;
}

// Selected range in bytes; false when nothing is selected.
bool sel_range(lv_obj_t *ta, size_t *b0, size_t *b1) {
    lv_obj_t *l = lv_textarea_get_label(ta);
    uint32_t s = lv_label_get_text_selection_start(l), e = lv_label_get_text_selection_end(l);
    if (s == LV_LABEL_TEXT_SELECTION_OFF || e == LV_LABEL_TEXT_SELECTION_OFF || s == e) return false;
    if (s > e) { const uint32_t t = s; s = e; e = t; }
    const char *txt = lv_textarea_get_text(ta);
    *b0 = utf8_byte(txt, s);
    *b1 = utf8_byte(txt, e);
    return *b1 > *b0;
}

void sel_clear(lv_obj_t *ta) {
    lv_obj_t *l = lv_textarea_get_label(ta);
    lv_label_set_text_selection_start(l, LV_LABEL_TEXT_SELECTION_OFF);
    lv_label_set_text_selection_end(l, LV_LABEL_TEXT_SELECTION_OFF);
}

// Replace bytes [b0, b1) with `ins` (may be empty), cursor after it.
void replace_range(lv_obj_t *ta, size_t b0, size_t b1, const char *ins) {
    const char *txt = lv_textarea_get_text(ta);
    const size_t n = strlen(txt), il = strlen(ins);
    char *out = (char *)lv_malloc(n - (b1 - b0) + il + 1);
    if (!out) return;
    memcpy(out, txt, b0);
    memcpy(out + b0, ins, il);
    memcpy(out + b0 + il, txt + b1, n - b1 + 1);
    uint32_t cur = 0;                                 // cursor: characters before the end of `ins`
    for (size_t i = 0; i < b0 + il; i++) if (((unsigned char)out[i] & 0xC0) != 0x80) cur++;
    sel_clear(ta);
    lv_textarea_set_text(ta, out);
    lv_textarea_set_cursor_pos(ta, (int32_t)cur);
    lv_free(out);
}

}  // namespace

// Delete the selected text; false when nothing is selected.
static bool ime_sel_delete(lv_obj_t *ta) {
    size_t b0, b1;
    if (!sel_range(ta, &b0, &b1)) return false;
    replace_range(ta, b0, b1, "");
    return true;
}

bool nv_ime_has_selection(lv_obj_t *ta) {
    if (!ta) ta = s_kb ? lv_keyboard_get_textarea(s_kb) : nullptr;
    size_t a, b;
    return ta && sel_range(ta, &a, &b);
}

// The system clipboard (nv_clipboard): a text field pastes only text — an image or a file list on
// the clipboard is for apps that understand it (ANIMA, Files), never garbage in a field.
bool nv_ime_clipboard_empty(void) { return !nv_clip_has_text(); }

void nv_ime_focus(lv_obj_t *ta) {
    if (!ta || !s_kb || lv_keyboard_get_textarea(s_kb) == ta) return;
    lv_obj_send_event(ta, LV_EVENT_FOCUSED, nullptr);
}

bool nv_ime_edit(lv_obj_t *ta, nv_ime_edit_t op) {
    if (!ta) ta = s_kb ? lv_keyboard_get_textarea(s_kb) : nullptr;
    if (!ta) return false;
    size_t b0 = 0, b1 = 0;
    const bool sel = sel_range(ta, &b0, &b1);
    switch (op) {
        case NV_IME_EDIT_SELECT_ALL: {
            lv_obj_t *l = lv_textarea_get_label(ta);
            const char *t = lv_textarea_get_text(ta);
            uint32_t chars = 0;
            for (const char *p = t; *p; p++) if (((unsigned char)*p & 0xC0) != 0x80) chars++;
            if (!chars) return false;
            lv_label_set_text_selection_start(l, 0);
            lv_label_set_text_selection_end(l, chars);
            lv_obj_invalidate(ta);
            return true;
        }
        case NV_IME_EDIT_COPY:
        case NV_IME_EDIT_CUT: {
            if (!sel) return false;
            char *c = (char *)lv_malloc(b1 - b0 + 1);
            if (!c) return false;
            memcpy(c, lv_textarea_get_text(ta) + b0, b1 - b0);
            c[b1 - b0] = 0;
            const NvApp *app = nv_ui_current_app();
            const bool ok = nv_clip_set_text(c, app ? app->id : "text");
            lv_free(c);
            if (!ok) return false;
            if (op == NV_IME_EDIT_CUT) replace_range(ta, b0, b1, "");
            nv_audio_click();
            return true;
        }
        case NV_IME_EDIT_PASTE: {
            char *clip = nv_clip_get_text();
            if (!clip) return false;
            if (sel) replace_range(ta, b0, b1, clip);
            else lv_textarea_add_text(ta, clip);
            free(clip);
            nv_audio_click();
            return true;
        }
    }
    return false;
}

bool nv_ime_bound(void) { return s_kb && lv_keyboard_get_textarea(s_kb) != nullptr; }
lv_obj_t *nv_ime_keyboard_obj(void) { return s_kb; }

bool nv_ime_inject_text(const char *utf8) {
    if (!s_kb || !utf8 || !utf8[0]) return false;
    lv_obj_t *ta = lv_keyboard_get_textarea(s_kb);
    if (!ta) return false;
    // A control character (Ctrl+letter on a hardware keyboard) is never inserted: it goes to the
    // field's key hook as a shortcut, or is dropped.
    const unsigned char c0 = (unsigned char)utf8[0];
    if (c0 >= 1 && c0 <= 26 && !utf8[1]) {
        FieldCfg *f = field_find(ta);
        if (f && f->hook && f->hook(ta, -1, (char)('a' + c0 - 1))) { nv_audio_click(); return true; }
        switch (c0) {                                  // the system editing shortcuts
            case 3:  return nv_ime_edit(ta, NV_IME_EDIT_COPY);
            case 24: return nv_ime_edit(ta, NV_IME_EDIT_CUT);
            case 22: return nv_ime_edit(ta, NV_IME_EDIT_PASTE);
            case 1:  return nv_ime_edit(ta, NV_IME_EDIT_SELECT_ALL);
            default: return false;
        }
    }
    size_t b0, b1;
    if (sel_range(ta, &b0, &b1)) replace_range(ta, b0, b1, utf8);   // typing replaces the selection
    else lv_textarea_add_text(ta, utf8);
    // One-shot shift parity with on-screen typing: an auto-capitalized plane drops back
    // to lowercase after the first remotely-typed character too.
    if (lv_keyboard_get_mode(s_kb) == LV_KEYBOARD_MODE_TEXT_UPPER)
        lv_keyboard_set_mode(s_kb, LV_KEYBOARD_MODE_TEXT_LOWER);
    nv_audio_click();
    return true;
}

bool nv_ime_inject_key(nv_ime_remote_key_t key) {
    if (!s_kb) return false;
    lv_obj_t *ta = lv_keyboard_get_textarea(s_kb);
    if (!ta) return false;
    if (FieldCfg *f = field_find(ta)) {
        if (f->hook && f->hook(ta, (int)key, 0)) { nv_audio_click(); return true; }
    }

    switch (key) {
        case NV_IME_RK_ENTER:
            // Multi-line DEFAULT field: the ⏎ key inserts a newline. Everything else
            // (one-line, DONE/NEXT/GO/SEARCH/SEND) acts as the return/OK key.
            if (s_active_ret == NV_IME_RET_DEFAULT && !lv_textarea_get_one_line(ta))
                lv_textarea_add_char(ta, '\n');
            else
                ready_action(true);
            break;
        case NV_IME_RK_ESC:
            kb_slide_down();
            lv_keyboard_set_textarea(s_kb, nullptr);  // unbind -> clears FOCUSED cue
            break;
        // With a selection (Ctrl+A, a mouse drag) both delete the selection, as on any desktop.
        case NV_IME_RK_BACKSPACE: if (!ime_sel_delete(ta)) lv_textarea_delete_char(ta);         break;
        case NV_IME_RK_DELETE:    if (!ime_sel_delete(ta)) lv_textarea_delete_char_forward(ta); break;
        case NV_IME_RK_TAB: {
            lv_obj_t *nx = next_bound_field(ta);
            if (!nx) return false;
            focus_field(nx);
            break;
        }
        case NV_IME_RK_LEFT:  lv_textarea_cursor_left(ta);  break;
        case NV_IME_RK_RIGHT: lv_textarea_cursor_right(ta); break;
        case NV_IME_RK_UP:    lv_textarea_cursor_up(ta);    break;
        case NV_IME_RK_DOWN:  lv_textarea_cursor_down(ta);  break;
        case NV_IME_RK_HOME:  lv_textarea_set_cursor_pos(ta, 0); break;
        case NV_IME_RK_END:   lv_textarea_set_cursor_pos(ta, LV_TEXTAREA_CURSOR_LAST); break;
        default: return false;
    }
    nv_audio_click();
    return true;
}
