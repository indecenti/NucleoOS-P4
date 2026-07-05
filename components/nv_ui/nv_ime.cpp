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
#include "nv_theme.h"
#include "nv_audio.h"   // soft key-press tick
#include "nv_i18n.h"
#include "nv_fonts.h"   // nv_font_20 carries Latin-1 accents (built-in montserrat is ASCII only)
#include "nv_event_bus.h"

namespace {

// ~42% of the display height, docked bottom.
constexpr int   kKbHeightPct = 42;
constexpr uint32_t kSlideMs  = 200;

lv_obj_t *s_kb = nullptr;   // the single shared keyboard

// ---- per-field registry: input class + return action, keyed by the bound textarea ----------
constexpr int kMaxFields = 24;
struct FieldCfg { lv_obj_t *ta; uint8_t type; uint8_t ret; };
FieldCfg s_fields[kMaxFields] = {};

FieldCfg *field_find(lv_obj_t *ta) {
    for (auto &f : s_fields) if (f.ta == ta) return &f;
    return nullptr;
}
void field_store(lv_obj_t *ta, nv_ime_type_t type, nv_ime_return_t ret) {
    FieldCfg *f = field_find(ta);
    if (!f) for (auto &c : s_fields) if (!c.ta) { f = &c; break; }
    if (!f) return;  // registry full — field still works, just as TEXT/DEFAULT via lookup default
    f->ta = ta; f->type = (uint8_t)type; f->ret = (uint8_t)ret;
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
void ready_action(void) {
    lv_obj_t *ta = lv_keyboard_get_textarea(s_kb);
    switch (s_active_ret) {
        case NV_IME_RET_NEXT:
            if (ta) {
                if (lv_obj_t *nx = next_bound_field(ta)) { focus_field(nx); return; }
            }
            break;  // no next field -> fall through and dismiss
        case NV_IME_RET_GO:
        case NV_IME_RET_SEARCH:
        case NV_IME_RET_SEND:
            if (s_submit_cb && ta) s_submit_cb(ta, s_submit_user);
            break;
        case NV_IME_RET_DEFAULT:
        case NV_IME_RET_DONE:
        default:
            break;
    }
    kb_slide_down();
    lv_keyboard_set_textarea(s_kb, nullptr);
}

void kb_ready_cb(lv_event_t *) { ready_action(); }

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
    if      (!lv_strcmp(txt, LV_SYMBOL_BACKSPACE)) lv_textarea_delete_char(ta);
    else if (!lv_strcmp(txt, LV_SYMBOL_LEFT))      lv_textarea_cursor_left(ta);
    else if (!lv_strcmp(txt, LV_SYMBOL_RIGHT))     lv_textarea_cursor_right(ta);
}

// ---------------------------------------------------------------- per-textarea events
void ta_event_cb(lv_event_t *e) {
    const lv_event_code_t code = lv_event_get_code(e);
    lv_obj_t *ta = lv_event_get_target_obj(e);

    if (code == LV_EVENT_FOCUSED) {
        if (nv_ui_shade_is_open()) return;  // never raise the keyboard over the open shade
        // Tapped this field: bind + open the class-appropriate plane, then slide up.
        lv_keyboard_set_textarea(s_kb, ta);
        apply_plane_on_focus(ta, field_type(ta));
        s_active_ret = field_ret(ta);
        kb_slide_up();
        // Shrink the field's parent by the keyboard height so the field sits fully above it,
        // then scroll the cursor line into the now-reduced area.
        field_shift_apply(ta);
        lv_obj_scroll_to_view(ta, LV_ANIM_ON);
    } else if (code == LV_EVENT_DEFOCUSED) {
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

void nv_ime_bind_ex(lv_obj_t *textarea, nv_ime_type_t type, nv_ime_return_t ret) {
    if (!textarea) return;
    configure_field(textarea, type);
    field_store(textarea, type, ret);
    lv_obj_add_event_cb(textarea, ta_event_cb, LV_EVENT_FOCUSED,   nullptr);
    lv_obj_add_event_cb(textarea, ta_event_cb, LV_EVENT_DEFOCUSED, nullptr);
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
bool nv_ime_inject_text(const char *utf8) {
    if (!s_kb || !utf8 || !utf8[0]) return false;
    lv_obj_t *ta = lv_keyboard_get_textarea(s_kb);
    if (!ta) return false;
    lv_textarea_add_text(ta, utf8);
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

    switch (key) {
        case NV_IME_RK_ENTER:
            // Multi-line DEFAULT field: the ⏎ key inserts a newline. Everything else
            // (one-line, DONE/NEXT/GO/SEARCH/SEND) acts as the return/OK key.
            if (s_active_ret == NV_IME_RET_DEFAULT && !lv_textarea_get_one_line(ta))
                lv_textarea_add_char(ta, '\n');
            else
                ready_action();
            break;
        case NV_IME_RK_ESC:
            kb_slide_down();
            lv_keyboard_set_textarea(s_kb, nullptr);  // unbind -> clears FOCUSED cue
            break;
        case NV_IME_RK_BACKSPACE: lv_textarea_delete_char(ta);         break;
        case NV_IME_RK_DELETE:    lv_textarea_delete_char_forward(ta); break;
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
        default: return false;
    }
    nv_audio_click();
    return true;
}
