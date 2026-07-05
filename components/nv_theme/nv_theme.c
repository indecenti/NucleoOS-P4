// nv_theme — see nv_theme.h. C so designated initializers can lay the palettes out clearly
// (mirrors nv_i18n.c's style). Includes nv_fonts.h for the extern font declarations.
#include "nv_theme.h"

#include "nv_config.h"
#include "nv_event_bus.h"
#include "nv_fonts.h"
#include "nv_log.h"

// C(r,g,b) — shorthand for LV_COLOR_MAKE so the palette tables read like hex triplets.
#define C(r, g, b) LV_COLOR_MAKE(r, g, b)

// -------------------------------------------------------------- base palettes (mode)
// Dark == the original hardcoded values verbatim, so dark mode is pixel-identical to before.
// accent/primary/on_primary are placeholders here; compose() overrides them per accent+mode.
static const NvTheme kDark = {
    .bg            = C(0x0E, 0x11, 0x16),
    .surface       = C(0x16, 0x1B, 0x22),
    .surface2      = C(0x21, 0x26, 0x2D),
    .surface3      = C(0x30, 0x36, 0x3D),
    .header        = C(0x0A, 0x0D, 0x11),
    .shade_bg      = C(0x11, 0x15, 0x1B),
    .control_alt   = C(0x1F, 0x27, 0x33),
    .text          = C(0xE6, 0xED, 0xF3),
    .text_strong   = C(0xF2, 0xF4, 0xF8),
    .text_dim      = C(0xA7, 0xB4, 0xC0),   // lifted from #8B98A5 for WCAG contrast on surface/bg
    .accent        = C(0x58, 0xA6, 0xFF),
    .primary       = C(0x1F, 0x6F, 0xEB),
    .on_primary    = C(0xF2, 0xF4, 0xF8),
    .success       = C(0x9D, 0xE3, 0x9D),
    .success_solid = C(0x23, 0x86, 0x36),
    .danger        = C(0xF8, 0x51, 0x49),
    .scrim         = C(0x00, 0x00, 0x00),
    .shadow        = C(0x00, 0x00, 0x00),
    .divider       = C(0x2A, 0x31, 0x3A),
    .text_disabled = C(0x5A, 0x64, 0x70),
    .scrim_opa     = 178,                    // ~70% dim behind shade/modals in dark
    .font_default  = NULL,  // set by compose()
};

static const NvTheme kLight = {
    .bg            = C(0xF5, 0xF6, 0xF8),
    .surface       = C(0xFF, 0xFF, 0xFF),
    .surface2      = C(0xEA, 0xEC, 0xEF),
    .surface3      = C(0xDC, 0xDF, 0xE4),
    .header        = C(0xFF, 0xFF, 0xFF),
    .shade_bg      = C(0xF0, 0xF2, 0xF5),
    .control_alt   = C(0xE1, 0xE6, 0xEC),
    .text          = C(0x1F, 0x23, 0x28),
    .text_strong   = C(0x0B, 0x0E, 0x11),
    .text_dim      = C(0x5B, 0x66, 0x72),
    .accent        = C(0x09, 0x69, 0xDA),
    .primary       = C(0x1F, 0x6F, 0xEB),
    .on_primary    = C(0xFF, 0xFF, 0xFF),
    .success       = C(0x1A, 0x7F, 0x37),
    .success_solid = C(0x2D, 0xA4, 0x4E),
    .danger        = C(0xCF, 0x22, 0x2E),
    .scrim         = C(0x00, 0x00, 0x00),
    .shadow        = C(0x1F, 0x23, 0x28),
    .divider       = C(0xD8, 0xDC, 0xE1),
    .text_disabled = C(0x9A, 0xA4, 0xB0),
    .scrim_opa     = 128,                    // ~50% dim behind shade/modals in light
    .font_default  = NULL,
};

// -------------------------------------------------------------- accent overrides
// primary (filled buttons/checked) + accent (icons/links). success/danger are status colors
// and DO NOT change with accent. Per accent, per mode (dark, light).
typedef struct { lv_color_t primary, accent; } accent_pair_t;
static const accent_pair_t kAccents[NV_ACCENT_COUNT][2] = {
    // [accent][0]=dark, [accent][1]=light
    [NV_ACCENT_BLUE]   = { { C(0x1F,0x6F,0xEB), C(0x58,0xA6,0xFF) }, { C(0x1F,0x6F,0xEB), C(0x09,0x69,0xDA) } },
    [NV_ACCENT_GREEN]  = { { C(0x2E,0xA0,0x43), C(0x56,0xD3,0x64) }, { C(0x1A,0x7F,0x37), C(0x1A,0x7F,0x37) } },
    [NV_ACCENT_PURPLE] = { { C(0x89,0x57,0xE5), C(0xBC,0x8C,0xFF) }, { C(0x82,0x50,0xDF), C(0x66,0x39,0xBA) } },
    [NV_ACCENT_ORANGE] = { { C(0xE3,0x62,0x09), C(0xFB,0x8F,0x44) }, { C(0xBC,0x4C,0x00), C(0x9A,0x3F,0x00) } },
};

static const char *const kAccentName[NV_ACCENT_COUNT] = {
    [NV_ACCENT_BLUE]   = "Blue",
    [NV_ACCENT_GREEN]  = "Green",
    [NV_ACCENT_PURPLE] = "Purple",
    [NV_ACCENT_ORANGE] = "Orange",
};

// -------------------------------------------------------------- active state
static NvTheme         s_theme;
static nv_theme_mode_t s_mode  = NV_THEME_DARK;
static nv_accent_t     s_accent = NV_ACCENT_BLUE;
static nv_font_scale_t s_fscale = NV_FONT_NORMAL;

static void compose(void) {
    s_theme = (s_mode == NV_THEME_LIGHT) ? kLight : kDark;

    const accent_pair_t *ap = &kAccents[s_accent][(s_mode == NV_THEME_LIGHT) ? 1 : 0];
    s_theme.primary = ap->primary;
    s_theme.accent  = ap->accent;

    s_theme.font_default = (s_fscale == NV_FONT_LARGE) ? &nv_font_20 : &nv_font_14;
}

void nv_theme_init(void) {
    int m = nv_config_get_int("thmode", NV_THEME_DARK);
    int a = nv_config_get_int("thaccent", NV_ACCENT_BLUE);
    int f = nv_config_get_int("thfont", NV_FONT_NORMAL);
    if (m < 0 || m > NV_THEME_LIGHT)      m = NV_THEME_DARK;
    if (a < 0 || a >= NV_ACCENT_COUNT)    a = NV_ACCENT_BLUE;
    if (f < 0 || f > NV_FONT_LARGE)       f = NV_FONT_NORMAL;
    s_mode   = (nv_theme_mode_t)m;
    s_accent = (nv_accent_t)a;
    s_fscale = (nv_font_scale_t)f;
    compose();
    NV_LOGI("theme", "mode=%d accent=%s fontscale=%d", (int)s_mode, kAccentName[s_accent],
            (int)s_fscale);
}

const NvTheme *nv_theme_get(void) { return &s_theme; }

void nv_theme_set_mode(nv_theme_mode_t mode) {
    if (mode < 0 || mode > NV_THEME_LIGHT || mode == s_mode) return;
    s_mode = mode;
    nv_config_set_int("thmode", (int)mode);
    compose();
    nv_event_publish(NV_EV_THEME_CHANGED, NULL);
}

void nv_theme_set_accent(nv_accent_t accent) {
    if (accent < 0 || accent >= NV_ACCENT_COUNT || accent == s_accent) return;
    s_accent = accent;
    nv_config_set_int("thaccent", (int)accent);
    compose();
    nv_event_publish(NV_EV_THEME_CHANGED, NULL);
}

void nv_theme_set_font_scale(nv_font_scale_t scale) {
    if (scale < 0 || scale > NV_FONT_LARGE || scale == s_fscale) return;
    s_fscale = scale;
    nv_config_set_int("thfont", (int)scale);
    compose();
    nv_event_publish(NV_EV_THEME_CHANGED, NULL);
}

nv_theme_mode_t nv_theme_get_mode(void)       { return s_mode; }
nv_accent_t     nv_theme_get_accent(void)     { return s_accent; }
nv_font_scale_t nv_theme_get_font_scale(void) { return s_fscale; }

const char *nv_accent_name(nv_accent_t accent) {
    if (accent < 0 || accent >= NV_ACCENT_COUNT) return "";
    return kAccentName[accent];
}
