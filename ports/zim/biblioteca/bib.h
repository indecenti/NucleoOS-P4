// bib.h — the Biblioteca app (docs/ZIM_READER_PLAN.md, F3): a reader for the ZIM files of Kiwix /
// openZIM kept in ~/zim. A WASI reactor drawing into its own RGB565 frame buffer (nv_gfx_blit of the
// rows that changed); nvzim reads the files, nvhtml turns pages into rich text, doc.c lays it out.
//
// Modules: gfx.c (frame buffer, shapes), font.c (Noto fonts rasterised at run time by stb_truetype,
// glyph cache), doc.c (rich text -> positioned runs, links, sections), kbd.c (USB/Bluetooth keyboard,
// on-screen keyboard), lib.c (the collections, covers, history, bookmarks, settings), i18n.c (the
// five languages of the system), app.c (screens, input, navigation).
#pragma once
#include <stddef.h>
#include <stdint.h>

#include "nvzim.h"

#ifdef __cplusplus
extern "C" {
#endif

// ------------------------------------------------------------------------------------ frame buffer
extern uint16_t **g_rows;   // the frame buffer by row: scrolling a band rotates these, no pixel moves
extern int g_w, g_h;

static inline uint16_t rgb565(int r, int g, int b) {
    return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}
static inline uint16_t blend565(uint16_t d, uint16_t s, unsigned a) {   // a = 0..255 of s over d
    const uint32_t a5 = (a + 4) >> 3;
    uint32_t bg = (d | (uint32_t)d << 16) & 0x07E0F81Fu;
    const uint32_t fg = (s | (uint32_t)s << 16) & 0x07E0F81Fu;
    bg += (fg - bg) * a5 >> 5;
    bg &= 0x07E0F81Fu;
    return (uint16_t)(bg | bg >> 16);
}

int gfx_init(void);
void gfx_dirty(int y0, int y1);
void gfx_flush(void);
void gfx_clip(int x0, int y0, int x1, int y1);   // drawing limited to this rectangle (x1, y1 exclusive)
void gfx_noclip(void);
void gfx_fill(int x, int y, int w, int h, uint16_t c);
void gfx_blend_rect(int x, int y, int w, int h, uint16_t c, unsigned a);   // translucent fill
void gfx_rrect(int x, int y, int w, int h, int r, uint16_t c);              // rounded, antialiased corners
void gfx_ring(int x, int y, int w, int h, int r, int t, uint16_t c);        // rounded outline, t px thick
void gfx_mask(const uint8_t *m, int mw, int mh, int x, int y, uint16_t c);  // 8-bit coverage mask
void gfx_image(const uint16_t *px, int iw, int ih, int x, int y);
void gfx_scroll(int y0, int y1, int dy);   // rows [y0, y1) move by dy (> 0: down); the gap holds old rows
void gfx_row(int y, int x, const uint16_t *px, const uint8_t *a, int w);   // one row of pixels with alpha, clipped

// ------------------------------------------------------------------------------------------ fonts
enum { F_SANS, F_BOLD, F_ITALIC, F_BOLDITALIC, F_MONO, F_COUNT };
int font_init(void);
int font_ascent(int f, int px);
int font_descent(int f, int px);   // positive
int text_width(int f, int px, const char *s, int n);
int32_t text_width16(int f, int px, const char *s, int n);   // in 1/16 px, as drawn
int text_draw(int f, int px, int x, int y_base, const char *s, int n, uint16_t c);   // returns the advance
int text_fit(int f, int px, const char *s, int n, int maxw);                       // bytes that fit in maxw
// One line cut to maxw with an ellipsis; returns the width drawn.
int text_draw_fit(int f, int px, int x, int y_base, const char *s, int n, int maxw, uint16_t c);
uint32_t utf8_next(const char **p, const char *end);
void font_cache_stats(int *glyphs, size_t *bytes);

// ----------------------------------------------------------------------------------------- theme
enum { C_BG, C_TEXT, C_DIM, C_LINK, C_HEAD, C_RULE, C_CARD, C_BAR, C_BAR_TEXT, C_ACCENT, C_ON_ACCENT,
       C_CODE_BG, C_CELL_BG, C_FOCUS, C_PRESS, C_SCRIM, C_COUNT };
extern uint16_t g_col[C_COUNT];
void theme_set(int dark);

// -------------------------------------------------------------------------------------- documents
enum { OP_TEXT, OP_RECT, OP_STATIC, OP_IMAGE };
typedef struct {
    int32_t y0, y1;          // vertical extent, document coordinates
    int16_t x, w;
    int32_t base;            // text: baseline
    uint8_t kind, font, px, color;
    int32_t off;             // text: bytes in doc->text
    uint16_t len;
    int16_t link;            // index in doc->links, or -1
} op_t;

typedef struct {
    int32_t off;             // the href, in doc->text
    uint16_t len;
} link_t;

typedef struct {             // a picture of the page (an OP_IMAGE's off is its index)
    int32_t src_off, alt_off;   // in doc->text
    uint16_t src_len, alt_len;
    int16_t w, h;            // as laid out, px
    uint8_t mask, block;     // mask: a formula, drawn in the text colour
} dimg_t;

typedef struct {
    int32_t y;               // top of the heading
    int32_t off, len;        // its text
    int32_t aoff, alen;      // its anchor (id), len 0 = none
    uint8_t level;
} toc_t;

typedef struct {
    char *text;              // the rich text (owned)
    size_t n;
    op_t *ops;
    int nops, cap;
    link_t *links;
    int nlinks, lcap;
    toc_t *toc;
    int ntoc, tcap;
    dimg_t *imgs;
    int nimgs, icap;
    int32_t height;
    int width, body, max_op_h;
} doc_t;

// Lays out rich text (nvhtml, opts.rich) at `width` px with body text of `body` px. Takes text.
int doc_layout(doc_t *d, char *text, size_t n, int width, int body);
int doc_relayout(doc_t *d, int width, int body);
void doc_free(doc_t *d);
// Draws the part [top, top + h) of the document at screen row y and column x, clipped to that band.
// focus_link: a link drawn with the focus ring (-1 none).
void doc_draw(const doc_t *d, int x, int y, int top, int h, int focus_link);
int doc_link_at(const doc_t *d, int dx, int dy);            // document coordinates -> link or -1
int doc_image_at(const doc_t *d, int dx, int dy);           // document coordinates -> picture or -1
int doc_link_rect(const doc_t *d, int link, int32_t from_y, int32_t *y0, int32_t *y1);   // first op at/after from_y
int doc_anchor_y(const doc_t *d, const char *id, int n);     // "#id" -> y, or -1
int doc_links_in(const doc_t *d, int32_t top, int32_t bottom, int *out, int max);   // reading order
// Drawing a picture is the app's (it knows the collection): x, y on screen, w x h as laid out.
void doc_draw_image(const doc_t *d, int idx, int x, int y, int w, int h);

// ------------------------------------------------------------------------------------ pictures
uint8_t *img_decode_rgba(const uint8_t *data, size_t n, int w, int h);   // any supported file -> w x h RGBA
int img_lookup(const char *path, int w, int h, int mask);   // 1 ready, -1 failed, 0 queued for img_work
int img_draw(const char *path, int w, int h, int x, int y, uint16_t tint);   // 0 drawn
// Decodes one queued picture; 0 = none queued. cluster_ready (may be NULL): > 0 when the cluster that
// stores the picture is in memory already (such a picture goes first), 0 when not, < 0 unknown.
int img_work(int (*read)(void *ctx, const char *path, const uint8_t **data, size_t *n, void **tok),
             void (*done)(void *ctx, void *tok), int (*cluster_ready)(void *ctx, const char *path), void *ctx);
int img_pending(void);
void img_clear(void);
void img_stats(int *n, size_t *bytes);
const char *img_last(void);   // the last decode, for the log

// --------------------------------------------------------------------------------------- keyboard
enum { K_NONE, K_CHAR, K_ENTER, K_ESC, K_BACKSPACE, K_TAB, K_SPACE, K_UP, K_DOWN, K_LEFT, K_RIGHT, K_PGUP,
       K_PGDN, K_HOME, K_END, K_DELETE, K_F };
enum { M_CTRL = 1, M_SHIFT = 2, M_ALT = 4, M_ALTGR = 8, M_GUI = 16 };
typedef struct {
    int key;                 // K_*
    uint32_t ch;             // K_CHAR: the code point
    int mods;
    int fn;                  // K_F: 1..12
} kev_t;
int kbd_present(void);       // a physical keyboard is connected
int kbd_poll(kev_t *out, int max, int now_ms, int italian);

// ---------------------------------------------------------------------------------- collections
#define LIB_MAX 64
typedef struct {
    char file[128];          // in ~/zim
    char path[160];          // as the app opens it
    uint64_t size;
    int64_t mtime;
    char title[96], desc[192], lang[24], date[16], name[96];
    long articles;
    int bad;                 // could not be opened
    int retry;               // something could not be read now (memory): not cached, read again next time
    uint8_t *cover;          // COVER_PX x COVER_PX RGBA, or NULL
} coll_t;
#define COVER_PX 48

extern coll_t *g_lib;
extern int g_nlib;
int lib_scan(int (*progress)(void *ctx, int done, int total), void *ctx);   // ~/zim -> g_lib
int lib_find(const char *file);
const char *bib_root(void);   // "" on the device; a folder on the PC tests
int lib_meta(nvzim_t *z, const char *name, char *buf, size_t cap);

typedef struct {
    char file[128], path[NVZIM_PATH_MAX], title[160];
    int64_t when;
} mark_t;
enum { MARK_HISTORY, MARK_BOOKMARKS };
int marks_load(int which, mark_t **out);   // count
int marks_add(int which, const char *file, const char *path, const char *title);
int marks_remove(int which, const char *file, const char *path);
int marks_has(int which, const char *file, const char *path);

typedef struct {
    int text_size;           // 0..4
    int dark;
    char last_file[128], last_path[NVZIM_PATH_MAX];
    int32_t last_scroll;
} settings_t;
extern settings_t g_set;
void settings_load(void);
void settings_save(void);

// -------------------------------------------------------------------------- the catalog (cat.c)
typedef struct {
    char name[64];           // "wikipedia_it_top": the same for every flavour and every month
    char flavour[12];        // "maxi" (pictures), "nopic", "mini" (introductions), "" (only one)
    char title[96], summary[192], lang[16], category[24], date[11], author[48];
    char meta4[224];         // https://lb.download.kiwix.org/zim/.../name_flavour_date.zim.meta4
    char icon[112];          // "/catalog/v2/illustration/<uuid>/?size=48" on the catalog server
    uint64_t size;
    int32_t articles, media;
    uint8_t pictures, videos;
} cat_t;
// Entries of an OPDS v2 feed; *total = the feed's totalResults (-1 unknown). Returns how many.
int cat_parse(const char *xml, size_t n, cat_t *out, int max, int *total);
int cat_file_name(const cat_t *c, char *out, size_t cap);   // the .zim the download becomes; 0 if unusable
int cat_recommended(const char *name, const char *lang2);   // rank in Recommended, -1 = not there
int cat_flavour_rank(const cat_t *c);                        // which flavour a list offers first (lower)

// ------------------------------------------------------------ the download screen's data (get.c)
typedef struct {             // one collection of the catalog: its flavours are g_cat[first .. first + n)
    int first, n;
    int pick;                // the flavour a list offers (index in g_cat)
    int rec;                 // rank in Recommended, -1 = not there
} cat_group_t;
extern cat_t *g_cat;         // sorted by name, then by the order flavours are offered
extern int g_ncat;
extern cat_group_t *g_grp;   // Recommended first, then by title
extern int g_ngrp;
enum { GET_IDLE, GET_LOADING, GET_READY, GET_ERROR };
int get_state(void);
int get_gen(void);           // bumps whenever g_grp is rebuilt
int get_busy(void);          // a catalog page is on its way
int get_error(void);         // the last failure: an NV_NET_E_* or an HTTP status
const char *get_lang3(void);
void get_open(const char *lang3, int force);   // the catalog of a language: the cache, the network
int get_poll(void);          // one step of the network work; 1 = something to redraw
const uint8_t *get_icon(int ent);              // 48x48 RGBA, NULL until it is there (asked now)

#define DL_MAX 16
enum { DL_QUEUED, DL_PAUSED, DL_DONE, DL_FAILED, DL_RUNNING };
typedef struct {             // a download of this app, as the system downloader reports it
    int id, state;
    char title[96], dest[160], error[96], phase[12];
    int64_t size, done;
    int32_t bps;
} dl_t;
extern dl_t g_dl[DL_MAX];
extern int g_ndl;
int get_dl_refresh(int *finished, int *failed);   // 1 = anything changed; counts the new ends
int get_dl_start(const cat_t *c);                 // the download id, or NV_DL_E_*
int get_dl_of(const cat_t *c);                    // index in g_dl of the download of that file, -1
int64_t get_room(void);                           // bytes a new download may use, -1 unknown
int get_have(const cat_t *c);                     // 1 that file is here, 2 an older edition is

// ------------------------------------------------------------------------------------------- i18n
enum {
    S_LIBRARY, S_EMPTY_TITLE, S_EMPTY_1, S_EMPTY_2, S_EMPTY_3, S_ARTICLES, S_SEARCH_IN, S_NO_RESULTS,
    S_SEARCH_TITLES, S_SEARCHING, S_SECTIONS, S_BOOKMARKS, S_HISTORY, S_NO_BOOKMARKS, S_NO_HISTORY,
    S_RANDOM, S_TEXT_SIZE, S_THEME_DARK, S_THEME_LIGHT, S_INFO, S_MAIN_PAGE, S_VERIFY, S_VERIFYING,
    S_VERIFY_OK, S_VERIFY_BAD, S_VERIFY_NONE, S_BOOKMARK_ADDED, S_BOOKMARK_REMOVED, S_EXTERNAL_LINK,
    S_NOT_FOUND, S_UNSUPPORTED, S_OPEN_FAILED, S_DAMAGED, S_CONTINUE, S_LOADING, S_SCANNING,
    S_F_TITLE, S_F_DESC, S_F_LANG, S_F_DATE, S_F_CREATOR, S_F_PUBLISHER, S_F_SOURCE, S_F_LICENSE,
    S_F_FILE, S_F_SIZE, S_F_ENTRIES, S_F_FORMAT, S_F_NAME, S_MONTHS, S_TOP, S_COPY_HINT, S_MOVED_HINT,
    S_GET_TITLE, S_GET_RECOMMENDED, S_GET_ALL, S_GET_DOWNLOADS, S_GET_OTHER, S_GET_LOADING, S_GET_OFFLINE, S_GET_RETRY, S_GET_MAXI, S_GET_NOPIC, S_GET_MINI, S_GET_DOWNLOAD, S_GET_QUEUED, S_GET_HAVE, S_GET_UPDATE, S_GET_ROOM, S_GET_NO_ROOM, S_GET_PAUSE, S_GET_RESUME, S_GET_RETRY_DL, S_GET_CANCEL_SURE, S_GET_DONE, S_GET_FAILED, S_GET_PAUSED, S_GET_CHECKING, S_GET_PREPARING, S_GET_PROGRESS, S_GET_SPEED, S_GET_NONE, S_GET_NOTE, S_GET_STARTED, S_GET_FINISHED, S_GET_NOT_STARTED, S_GET_EXISTS, S_GET_EDITIONS, S_GET_WAIT_NET, S_DELETE, S_DELETE_SURE, S_DELETED, S_GET_NO_MATCH,
    S_COUNT
};
void i18n_init(void);
const char *tr(int id);
int i18n_italian_keys(void);   // the keyboard layout to assume (Italian for the Italian UI)
const char *i18n_lang3(void);  // the interface language as the catalog names it ("ita", "eng"...)
const char *lang_name(const char *iso639_3);
void fmt_date(const char *ymd, char *out, size_t cap);
void fmt_size(uint64_t bytes, char *out, size_t cap);
void fmt_count(long n, char *out, size_t cap);
void fmt_speed(uint64_t bytes_per_s, char *out, size_t cap);

// --------------------------------------------------------------------------------------- the icons
typedef struct { const uint8_t *m; int px; } icon_t;
enum { IC_BACK, IC_FORWARD, IC_SEARCH, IC_TOC, IC_STAR, IC_STAR_ON, IC_MORE, IC_LIBRARY, IC_RANDOM,
       IC_INFO, IC_CLOSE, IC_HISTORY, IC_TEXTSIZE, IC_THEME, IC_CHECK, IC_BACKSPACE, IC_ENTER, IC_SHIFT,
       IC_KEYBOARD, IC_BOOK, IC_DOWNLOAD, IC_PAUSE, IC_PLAY, IC_COUNT };
extern const icon_t g_icons[IC_COUNT];
void icon_draw(int ic, int x, int y, uint16_t c);   // top-left

#ifdef __cplusplus
}
#endif
