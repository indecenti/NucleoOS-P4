// bk.h — the Libri app (docs/BOOKS_PLAN.md): an ebook reader for EPUB 2/3 (reflowable and fixed
// layout), FB2, comic archives (CBZ), plain text, HTML and Markdown, with its library in ~/books and
// the catalog of free books of the NucleoOS store. A WASI reactor drawing into its own RGB565 frame
// buffer (nv_gfx_blit of the rows that changed); pages are laid out by lay.c, drawn into off-screen
// surfaces and turned with animations made of whole frames.
//
// Modules: gfx.c (frame buffer, surfaces, the grey and 1-bit render modes), font.c (Noto Sans/Serif
// rasterised at run time by stb_truetype), lay.c (rich text -> pages), hyph.c (hyphenation),
// zip.c (ZIP members), xml.c (a tolerant XML scanner), book.c (the formats -> chapters of rich
// text), img.c (pictures), kbd.c (keyboard), lib.c (library, positions, bookmarks, settings),
// cat.c (the store catalog), i18n.c (the five languages of the system), app.c (screens, input).
#pragma once
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// ------------------------------------------------------------------------------------ frame buffer
extern uint16_t **g_rows;   // the rows being drawn: the screen's, or a surface's (gfx_target)
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
static inline int lum565(uint16_t c) {   // 0..255
    const int r = (c >> 11) << 3, g = ((c >> 5) & 63) << 2, b = (c & 31) << 3;
    return (r * 77 + g * 150 + b * 29) >> 8;
}

// A full-screen picture kept off the screen (a page drawn ahead, the page under an animation).
typedef struct {
    uint16_t *px;
    uint16_t **rows;
} surf_t;

// How colours reach the pixels: RM_COLOR as they are; RM_GREY 16 levels of grey (the e-ink
// simulation: glyph coverage in 16 steps, pictures dithered); RM_MONO black and white only (text
// coverage cut at a threshold, pictures dithered with Atkinson's error diffusion).
enum { RM_COLOR, RM_GREY, RM_MONO };
extern int g_rmode;
extern int g_portrait;   // the app's screen is upright on a device held vertically (gfx_set_portrait)

int gfx_init(void);
int gfx_set_portrait(int on);          // 0 ok; the screen is cleared, surfaces must be made again
void gfx_touch_map(int *x, int *y);    // a touch on the canvas -> the app's screen
int surf_init(surf_t *s);              // 0 ok; the pixels are not cleared
void surf_free(surf_t *s);
void gfx_target(surf_t *s);            // draw into s from now on; NULL = the screen
void gfx_dirty(int y0, int y1);
void gfx_flush(void);
void gfx_clip(int x0, int y0, int x1, int y1);   // drawing limited to this rectangle (x1, y1 exclusive)
void gfx_noclip(void);
void gfx_fill(int x, int y, int w, int h, uint16_t c);
void gfx_blend_rect(int x, int y, int w, int h, uint16_t c, unsigned a);   // translucent fill
void gfx_rrect(int x, int y, int w, int h, int r, uint16_t c);              // rounded, antialiased corners
void gfx_ring(int x, int y, int w, int h, int r, int t, uint16_t c);        // rounded outline, t px thick
void gfx_mask(const uint8_t *m, int mw, int mh, int x, int y, uint16_t c);  // 8-bit coverage mask
void gfx_row(int y, int x, const uint16_t *px, const uint8_t *a, int w);   // one row of pixels with alpha, clipped
// The screen from two surfaces side by side: columns [0, split) show `left` from its column
// left_x, the rest `right` from its column right_x (page turns). Rows [y0, y1).
void gfx_compose(const surf_t *left, int left_x, const surf_t *right, int right_x, int split, int y0, int y1);
// A soft vertical shadow, `w` px wide, darkest at x (dir +1: fading to the right).
void gfx_shadow(int x, int w, int dir, int y0, int y1, unsigned strength);
void gfx_copy(const surf_t *s, int y0, int y1);   // rows of a surface onto the screen as they are
// A page turn's frame sent to the canvas straight from two surfaces (the screen buffer untouched).
void gfx_turn_frame(const surf_t *left, int left_x, const surf_t *right, int right_x, int split, int shadow_x, int sw,
                    int shadow_dir);
void gfx_invert(int y0, int y1);                  // the e-ink refresh flash
int gfx_shot(const char *path);                   // the screen to a file (tests on the board)

// ------------------------------------------------------------------------------------------ fonts
// The interface uses Sans; the reader Serif or Sans (setting). F_SERIF + style = the serif faces.
enum { F_SANS, F_BOLD, F_ITALIC, F_BOLDITALIC, F_MONO, F_SERIF, F_SERIF_BOLD, F_SERIF_ITALIC, F_SERIF_BI, F_COUNT };
int font_init(void);
int font_ascent(int f, int px);
int font_descent(int f, int px);   // positive
int font_xheight(int f, int px);
int text_width(int f, int px, const char *s, int n);
int32_t text_width16(int f, int px, const char *s, int n);   // in 1/16 px, as drawn
int text_draw(int f, int px, int x, int y_base, const char *s, int n, uint16_t c);   // returns the advance
int text_fit(int f, int px, const char *s, int n, int maxw);                       // bytes that fit in maxw
// One line cut to maxw with an ellipsis; returns the width drawn.
int text_draw_fit(int f, int px, int x, int y_base, const char *s, int n, int maxw, uint16_t c);
uint32_t utf8_next(const char **p, const char *end);
int utf8_put(uint32_t cp, char *out);   // bytes written (1..4)
void font_cache_stats(int *glyphs, size_t *bytes);
void font_cache_clear(void);            // a new render mode quantises coverage differently

// ----------------------------------------------------------------------------------------- theme
enum { C_BG, C_TEXT, C_DIM, C_LINK, C_HEAD, C_RULE, C_CARD, C_BAR, C_BAR_TEXT, C_ACCENT, C_ON_ACCENT,
       C_CODE_BG, C_CELL_BG, C_FOCUS, C_PRESS, C_SCRIM, C_MARK, C_SEL, C_COUNT };
extern uint16_t g_col[C_COUNT];
enum { TH_WHITE, TH_SEPIA, TH_DARK, TH_BLACK, TH_EINK, TH_MONO, TH_COUNT };
void theme_set(int theme);
int theme_dark(int theme);

// --------------------------------------------------------------------------------- rich text
// The text of a chapter: the rich text of nvhtml (nvhtml.h, opts.rich), one block per line, plus
// RT_ID: "\x18id\x1A" anywhere in a line, the target of "#id" links (FB2 sections and notes, EPUB
// elements with an id), and RT_FIXED, a line holding one picture that fills a page of its own.
#define RT_ID      '\x18'
#define RT_ID_END  '\x1A'
#define RT_FIXED   '\x1B'

// ------------------------------------------------------------------------------------ the layout
enum { OP_TEXT, OP_RECT, OP_STATIC, OP_IMAGE };
typedef struct {
    int32_t y0, y1;          // vertical extent, chapter coordinates
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

typedef struct {             // a picture of the chapter (an OP_IMAGE's off is its index)
    int32_t src_off, alt_off;   // in doc->text
    uint16_t src_len, alt_len;
    int16_t w, h;            // as laid out, px
    uint8_t mask, block, fixed;   // mask: a formula, drawn in the text colour; fixed: a page of its own
} dimg_t;

typedef struct {             // a heading of the chapter
    int32_t y;
    int32_t off, len;        // its text
    uint8_t level;
} head_t;

typedef struct {             // an "#id" target
    int32_t off, len;        // the id, in doc->text
    int32_t y;               // where it is
    int32_t toff;            // the text offset there (for notes: the paragraph to show)
} anchor_t;

typedef struct {             // a line as laid out: what pagination needs
    int32_t y0, y1;
    int32_t off;             // text offset of its first byte
    int32_t para;            // the block it belongs to
    uint8_t flags;           // LF_*
} dline_t;
enum { LF_FIRST = 1, LF_LAST = 2, LF_HEAD = 4, LF_IMAGE = 8, LF_PAGE = 16 };   // LF_PAGE: a page of its own

typedef struct {
    int width;               // the text column, px
    int page_h;              // the text area of a page, px
    int body;                // body text size, px
    int serif;               // the reading font: serif (1) or sans (0)
    int justify;
    int hyphen;              // hyphenate (the patterns of `lang`)
    int line_h;              // line height, percent of the font's height (120..200)
    int indent;              // paragraphs: first-line indent (1) or a gap between them (0)
    int center_heads;        // headings centred
    char lang[8];            // the book's language, for hyphenation
} lopt_t;

typedef struct {
    char *text;              // the rich text (owned)
    size_t n;
    op_t *ops;
    int nops, cap;
    link_t *links;
    int nlinks, lcap;
    head_t *heads;
    int nheads, hcap;
    anchor_t *anchors;
    int nanchors, acap;
    dimg_t *imgs;
    int nimgs, icap;
    dline_t *lines;
    int nlines, lncap;
    int32_t *page_top;       // pages: [page_top[k], page_top[k + 1]) of the chapter
    int32_t *page_off;       // the text offset each page starts at
    int npages;
    int32_t height;
    int max_op_h;
    lopt_t opt;
} doc_t;

int doc_layout(doc_t *d, char *text, size_t n, const lopt_t *opt);   // takes text
int doc_relayout(doc_t *d, const lopt_t *opt);
void doc_free(doc_t *d);
// Draws page k at screen row y, column x (into the current target).
void doc_draw_page(const doc_t *d, int k, int x, int y, int focus_link);
int doc_page_of_off(const doc_t *d, int32_t off);   // the page showing that text offset
int doc_page_of_y(const doc_t *d, int32_t y);
int doc_link_at(const doc_t *d, int k, int px, int py);    // a page's coordinates -> link or -1
int doc_image_at(const doc_t *d, int k, int px, int py);   // -> picture or -1
int doc_links_on(const doc_t *d, int k, int *out, int max);   // reading order
int doc_anchor(const doc_t *d, const char *id, int n);      // index in d->anchors, or -1
// The paragraph at text offset `off` as plain text (a note's popup); malloc'd.
char *doc_para_text(const doc_t *d, int32_t off, size_t max);
// Drawing a picture is the app's (it knows the book): x, y on screen, w x h as laid out.
void doc_draw_image(const doc_t *d, int idx, int x, int y, int w, int h);

// ---------------------------------------------------------------------------- hyphenation
// Liang's patterns of a language (hyph.c): the positions in a word where it may break.
// Returns how many: out[i] = byte offset of a break point (the hyphen goes before that byte).
int hyph_word(const char *lang, const char *w, int n, int *out, int max);
int hyph_has(const char *lang);

// ------------------------------------------------------------------------------------------- zip
typedef struct zip_s zip_t;
int zip_open(const char *path, zip_t **out);   // 0 ok, < 0 not a ZIP (or unreadable)
void zip_close(zip_t *z);
int zip_count(const zip_t *z);
const char *zip_name(const zip_t *z, int i);
uint32_t zip_size(const zip_t *z, int i);       // uncompressed
int zip_find(const zip_t *z, const char *name); // exact; then any case; -1
// The member, uncompressed and NUL-terminated (free it); max: refuse larger. 0 ok.
int zip_read(zip_t *z, int i, uint8_t **data, size_t *n, size_t max);
int zip_read_head(zip_t *z, int i, uint8_t **data, size_t *n, size_t want);   // the first `want` bytes

// ------------------------------------------------------------------------------------------- xml
enum { X_END, X_OPEN, X_CLOSE, X_EMPTY, X_TEXT };
typedef struct {
    int type;
    const char *name;        // X_OPEN/X_CLOSE/X_EMPTY: the tag name (with its prefix)
    int nlen;
    const char *attrs;       // the attributes, raw
    int alen;
    const char *text;        // X_TEXT: raw (entities not decoded)
    int tlen;
} xtok_t;
typedef struct { const char *p, *end; } xml_t;
void xml_init(xml_t *x, const char *s, size_t n);
int xml_next(xml_t *x, xtok_t *t);   // the token type; comments, PIs and doctypes are skipped
int xml_is(const xtok_t *t, const char *local);   // the local name (no prefix), any case
// An attribute by local name (any prefix); 1 found. The value is raw (entities not decoded).
int xml_attr(const xtok_t *t, const char *local, const char **v, int *vn);
// Entities decoded, white space collapsed and trimmed; returns the length (out NUL-terminated).
size_t xml_text(const char *s, int n, char *out, size_t cap);
// Skips to the end of the element just opened (X_OPEN); its inner text appended to out (cap).
size_t xml_inner_text(xml_t *x, const xtok_t *open, char *out, size_t cap);

// ------------------------------------------------------------------------------------------ book
enum { BK_EPUB, BK_FB2, BK_CBZ, BK_TXT, BK_HTML, BK_MD, BK_KINDS };
enum { BKE_OK = 0, BKE_OPEN = -1, BKE_FORMAT = -2, BKE_MEM = -3, BKE_EMPTY = -4, BKE_DRM = -5, BKE_BIG = -6 };

typedef struct {
    char *path;              // EPUB/CBZ: the member; other formats: NULL
    uint32_t size;           // bytes of source, for progress
    uint32_t start;          // whole-text formats: where it begins in book->rt
    uint8_t linear;
    uint8_t fixed;           // a pre-paginated page (fixed-layout EPUB, a comic picture)
} chap_t;

typedef struct {
    char *title;
    int chap;
    char *anchor;            // "" = the start of the chapter
    uint8_t level;           // 0 = top
} tocent_t;

typedef struct {
    int kind;
    char file[400];
    int64_t file_size;
    zip_t *zip;
    char title[256], author[200], lang[16], publisher[160], date[40], rights[240], ident[160], subject[200];
    char *desc;              // malloc'd or NULL
    char cover[300];         // EPUB/CBZ: the member holding the cover picture; FB2: "#binary-id"
    chap_t *ch;
    int nch;
    tocent_t *toc;
    int ntoc;
    int fixed;               // every chapter is a pre-paginated page (comics, picture books)
    int rtl;                 // page progression right to left
    uint64_t total;          // sum of chapter sizes
    char *rt;                // whole-text formats: the converted rich text, chapters cut in it
    size_t rtn;
    struct bin_s *bins;      // FB2: the pictures (base64 decoded on demand)
    int nbins;
    char *src;               // FB2: the file text (pictures are read from it)
    size_t srcn;
} book_t;

int book_open(const char *path, book_t **out);   // BKE_*
void book_close(book_t *b);
const char *book_kind_name(int kind);
int book_kind_of(const char *path);   // by extension; -1 unknown
// The rich text of chapter i (malloc'd, NUL-terminated); BKE_*.
int book_chapter(book_t *b, int i, char **text, size_t *n);
// A resource referenced from chapter i (a picture): the bytes (free them); BKE_*.
int book_resource(book_t *b, int chap, const char *href, int hn, uint8_t **data, size_t *n);
// The size of a picture of chapter `chap` from its header (w, h px); 0 ok.
int book_image_size(book_t *b, int chap, const char *href, int hn, int *w, int *h);
// Where a link of chapter `chap` goes: a chapter of the book (its index, the anchor into `anchor`),
// -1 outside the book (an external link: the URL into `anchor`), -2 nowhere.
int book_link(const book_t *b, int chap, const char *href, int hn, char *anchor, size_t cap);
// The cover picture's bytes (free them); BKE_*.
int book_cover(book_t *b, uint8_t **data, size_t *n);

// FB2 / TXT / Markdown converters (book.c uses them; tests call them directly).
char *fb2_to_rich(const char *xml, size_t n, book_t *meta, size_t *out_len);
char *txt_to_rich(const char *s, size_t n, size_t *out_len);   // any encoding: UTF-8, UTF-16, CP1252
char *md_to_rich(const char *s, size_t n, size_t *out_len);
char *to_utf8(const char *s, size_t n, size_t *out_len);        // detects UTF-16 / CP1252

// ------------------------------------------------------------------------------------ pictures
enum { IMG_CACHE_BYTES = 3u << 19 };   // 1.5 MB
uint8_t *img_decode_rgba(const uint8_t *data, size_t n, int w, int h);   // any supported file -> w x h RGBA
int img_size(const uint8_t *data, size_t n, int *w, int *h);             // 0 ok: the file's own size
int img_lookup(const char *key, int w, int h, int mask);   // 1 ready, -1 failed, 0 queued for img_work
int img_draw(const char *key, int w, int h, int x, int y, uint16_t tint);   // 0 drawn
int img_work(int (*read)(void *ctx, const char *key, uint8_t **data, size_t *n), void *ctx);
int img_pending(void);
void img_clear(void);
void img_stats(int *n, size_t *bytes);
const char *img_last(void);
// A picture's pixels for the cover cache and the viewer: RGB565 w x h fitted and centred on bg.
uint16_t *img_render565(const uint8_t *data, size_t n, int w, int h, uint16_t bg, int fill);

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
int kbd_present(void);
int kbd_poll(kev_t *out, int max, int now_ms, int italian);

// ------------------------------------------------------------------------------------- library
#define LIB_MAX 2000
typedef struct {
    char file[200];          // relative to ~/books
    int64_t size, mtime;
    char title[160], author[120], lang[12];
    uint8_t kind, bad, fixed;
    int32_t nch;
    int64_t opened;          // last opened (unix time), 0 never
    int16_t permille;        // reading progress, -1 never opened
    uint32_t hue;            // the colour of a cover drawn without a picture
} entry_t;

extern entry_t *g_lib;
extern int g_nlib;
const char *bk_root(void);             // "" on the device; a folder on the PC tests
const char *books_dir(void);           // "<root>/home/books"
int lib_scan(int (*progress)(void *ctx, int done, int total), void *ctx);
int lib_find(const char *file);
void lib_update(int i, const book_t *b);   // metadata after a successful open
void lib_save(void);
// Covers: COVER_W x COVER_H RGB565 in /appdata/covers; NULL when there is none (yet).
#define COVER_W 132
#define COVER_H 192
uint16_t *lib_cover(int i, book_t *b);  // b may be NULL: only the cache
int lib_remove(int i);                  // deletes the file and what the app kept about it

typedef struct {             // a place in a book
    int32_t chap, off;       // chapter, text offset in it (survives a new font size)
    int16_t permille;        // of the whole book, for lists
} pos_t;
int pos_load(const char *file, pos_t *p);   // 0 found
void pos_save(const char *file, const pos_t *p);

typedef struct {
    pos_t at;
    int64_t when;
    char note[200];          // the text where it was set
} mark_t;
int marks_load(const char *file, mark_t **out);   // count (free *out)
int marks_add(const char *file, const mark_t *m);
int marks_remove(const char *file, const pos_t *at);
int marks_find(const char *file, int32_t chap, int32_t off0, int32_t off1);   // a mark on [off0, off1), -1

typedef struct {
    int theme;               // TH_*
    int size;                // body text, px
    int serif;
    int justify, hyphen, indent;
    int spacing;             // 0..3: line height 125/140/155/175 %
    int margin;              // 0..3
    int anim;                // 0 slide, 1 cover, 2 none
    int cols;                // pages side by side: 0 automatic, 1, 2
    int portrait;            // the device is held upright (the system's portrait, rotation 90)
    int eink_every;          // e-ink: a full refresh every N pages (1..10)
    int sort;                // library order: 0 recent, 1 title, 2 author, 3 progress
    int tap_zones;           // tapping the left/right of a page turns it
    int status;              // the page's own status line (chapter, page, clock)
    char last_file[200];
    char cat_langs[64];      // catalog languages, "it,en"
} settings_t;
extern settings_t g_set;
void settings_load(void);
void settings_save(void);

// ------------------------------------------------------------------------------------------- i18n
enum {
    S_LIBRARY, S_EMPTY_TITLE, S_EMPTY_1, S_EMPTY_2, S_CONTINUE, S_OPEN_FAILED, S_FORMAT_BAD, S_DRM, S_LOADING,
    S_SCANNING, S_CONTENTS, S_BOOKMARKS, S_NO_BOOKMARKS, S_BOOKMARK_ADDED, S_BOOKMARK_REMOVED, S_SEARCH_IN,
    S_SEARCHING, S_NO_RESULTS, S_RESULTS, S_APPEARANCE, S_THEME, S_TH_WHITE, S_TH_SEPIA, S_TH_DARK, S_TH_BLACK,
    S_TH_EINK, S_TH_MONO, S_FONT, S_SERIF, S_SANS, S_SIZE, S_SPACING, S_MARGINS, S_JUSTIFY, S_HYPHEN,
    S_INDENT, S_TURN, S_TURN_SLIDE, S_TURN_COVER, S_TURN_NONE, S_EINK_REFRESH, S_TAP_ZONES, S_STATUS_LINE,
    S_ON, S_OFF, S_INFO, S_F_TITLE, S_F_AUTHOR, S_F_LANG, S_F_PUBLISHER, S_F_DATE, S_F_RIGHTS, S_F_SOURCE,
    S_F_FILE, S_F_SIZE, S_F_FORMAT, S_F_CHAPTERS, S_F_SUBJECT, S_DELETE, S_DELETE_SURE, S_DELETED, S_PAGE_OF,
    S_CHAPTER_N, S_PAGES_LEFT, S_PERCENT_READ, S_GO_TO, S_EXTERNAL_LINK, S_NOT_FOUND, S_NOTE, S_SORT,
    S_SORT_RECENT, S_SORT_TITLE, S_SORT_AUTHOR, S_SORT_PROGRESS, S_NEW, S_FINISHED, S_UNTITLED, S_UNKNOWN_AUTHOR,
    S_CATALOG, S_CAT_LOADING, S_CAT_OFFLINE, S_CAT_RETRY, S_CAT_ALL, S_CAT_LANGS, S_CAT_DOWNLOAD, S_CAT_QUEUED,
    S_CAT_HAVE, S_CAT_READ, S_CAT_PROGRESS, S_CAT_FAILED, S_CAT_NO_ROOM, S_CAT_SOURCE, S_CAT_LICENSE,
    S_CAT_SEARCH, S_CAT_DONE, S_CAT_DOWNLOADS, S_CAT_NONE, S_CAT_CANCEL, S_CAT_BOOKS, S_CAT_POPULAR,
    S_BACK_TO, S_ZOOM_HINT, S_BOOK_END, S_DOWNLOADING, S_COLUMNS, S_AUTO, S_CAT_CATEGORY, S_CAT_PD,
    S_COUNT
};
void i18n_init(void);
const char *tr(int id);
const char *ui_lang(void);     // "it", "en"...
int i18n_italian_keys(void);
const char *lang_name(const char *code);   // ISO 639-1/2 -> the language's name in the interface language
void fmt_size(uint64_t bytes, char *out, size_t cap);
void fmt_date(int64_t unix_s, char *out, size_t cap);

// ------------------------------------------------------------------------------------------ icons
typedef struct { const uint8_t *m; int px; } icon_t;
enum { IC_BACK, IC_SEARCH, IC_TOC, IC_BOOKMARK, IC_BOOKMARK_ON, IC_MORE, IC_LIBRARY, IC_INFO, IC_CLOSE,
       IC_TEXTSIZE, IC_CHECK, IC_BACKSPACE, IC_ENTER, IC_SHIFT, IC_KEYBOARD, IC_BOOK, IC_DOWNLOAD, IC_STORE,
       IC_SORT, IC_TRASH, IC_PREV, IC_NEXT, IC_ROTATE, IC_COUNT };
extern const icon_t g_icons[IC_COUNT];
void icon_draw(int ic, int x, int y, uint16_t c);

#ifdef __cplusplus
}
#endif
