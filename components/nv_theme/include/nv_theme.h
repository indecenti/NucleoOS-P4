// nv_theme — NucleoOS Anima design-token engine.
// A single cached NvTheme struct of semantic color tokens + the inherited default font,
// recomposed from three persisted config keys ("thmode","thaccent","thfont"). Dark mode
// reproduces the original hardcoded palette exactly, so it is visually identical to before.
// Every setter persists its key, recomposes the cached struct, and publishes
// NV_EV_THEME_CHANGED so SystemUI re-renders live (same async-rebuild path as i18n).
#pragma once
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

// Semantic design tokens. Named by role, not hue, so a light palette is a pure data swap.
// The extra tokens beyond the base list (surface2/surface3/control_alt/on_primary/
// success_solid/shadow) exist so every distinct chrome color maps 1:1 to a token.
typedef struct {
    lv_color_t bg;             // app canvas / launcher / screen background
    lv_color_t surface;        // card / list row / status bar
    lv_color_t surface2;       // raised control (chip, close btn, calc number key)
    lv_color_t surface3;       // tertiary control (calc fn keys, sci-pill unchecked)
    lv_color_t header;         // app top bar
    lv_color_t shade_bg;       // notification shade panel
    lv_color_t control_alt;    // back-button / neutral header control
    lv_color_t text;           // primary body text
    lv_color_t text_strong;    // hi-emphasis (titles, calc output, key labels)
    lv_color_t text_dim;       // secondary / captions / chevrons
    lv_color_t accent;         // icons, links, active marks (lighter brand)
    lv_color_t primary;        // filled brand button, operator key, checked state
    lv_color_t on_primary;     // text/icon drawn on primary & success fills
    lv_color_t success;        // success text (diagnostics)
    lv_color_t success_solid;  // success fill (calc equals key)
    lv_color_t danger;         // destructive text/ink (calc "C")
    lv_color_t scrim;          // overlays, photo caption bg, viewer black
    lv_color_t shadow;         // drop-shadow color (unused: P4 renderer bans shadow draw layers)
    lv_color_t divider;        // hairline separators / borders, distinct from any surface tone
    lv_color_t text_disabled;  // ink for disabled controls / low-emphasis chrome
    uint8_t    scrim_opa;      // scrim dim level (0-255) for shade + modal overlays, theme-tuned
    const lv_font_t *font_default;  // inherited default font (font-scale: normal vs large)
} NvTheme;

typedef enum { NV_THEME_DARK = 0, NV_THEME_LIGHT } nv_theme_mode_t;

typedef enum {
    NV_ACCENT_BLUE = 0,
    NV_ACCENT_GREEN,
    NV_ACCENT_PURPLE,
    NV_ACCENT_ORANGE,
    NV_ACCENT_COUNT
} nv_accent_t;

typedef enum { NV_FONT_NORMAL = 0, NV_FONT_LARGE } nv_font_scale_t;

// Load persisted keys ("thmode","thaccent","thfont", all clamped) and compose s_theme.
void nv_theme_init(void);

// The live active theme. Never NULL.
const NvTheme *nv_theme_get(void);

// Setters: persist the key, recompose s_theme, publish NV_EV_THEME_CHANGED. No-op if unchanged.
void nv_theme_set_mode(nv_theme_mode_t mode);
void nv_theme_set_accent(nv_accent_t accent);
void nv_theme_set_font_scale(nv_font_scale_t scale);

nv_theme_mode_t nv_theme_get_mode(void);
nv_accent_t     nv_theme_get_accent(void);
nv_font_scale_t nv_theme_get_font_scale(void);

// Human-readable accent name ("Blue","Green","Purple","Orange"). "" if out of range.
const char *nv_accent_name(nv_accent_t accent);

#ifdef __cplusplus
}
#endif
