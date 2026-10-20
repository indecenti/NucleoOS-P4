// app.c — the Biblioteca app: the library of collections, the reader, search, sections, bookmarks,
// history and the information about a collection. Immediate-mode drawing into the frame buffer:
// every screen draws itself whole and registers its controls (touch targets and keyboard focus);
// scrolling the reader moves the frame buffer and draws only the band that comes into view.
#include "bib.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "nucleo_sdk.h"
#include "nvhtml.h"

#include "icons.h"

const icon_t g_icons[IC_COUNT] = {
    {ICON_BACK, ICON_BACK_PX}, {ICON_FORWARD, ICON_FORWARD_PX}, {ICON_SEARCH, ICON_SEARCH_PX},
    {ICON_TOC, ICON_TOC_PX}, {ICON_STAR, ICON_STAR_PX}, {ICON_STAR_ON, ICON_STAR_ON_PX},
    {ICON_MORE, ICON_MORE_PX}, {ICON_LIBRARY, ICON_LIBRARY_PX}, {ICON_RANDOM, ICON_RANDOM_PX},
    {ICON_INFO, ICON_INFO_PX}, {ICON_CLOSE, ICON_CLOSE_PX}, {ICON_HISTORY, ICON_HISTORY_PX},
    {ICON_TEXTSIZE, ICON_TEXTSIZE_PX}, {ICON_THEME, ICON_THEME_PX}, {ICON_CHECK, ICON_CHECK_PX},
    {ICON_BACKSPACE, ICON_BACKSPACE_PX}, {ICON_ENTER, ICON_ENTER_PX}, {ICON_SHIFT, ICON_SHIFT_PX},
    {ICON_KEYBOARD, ICON_KEYBOARD_PX}, {ICON_BOOK, ICON_BOOK_PX},
    {ICON_DOWNLOAD, ICON_DOWNLOAD_PX}, {ICON_PAUSE, ICON_PAUSE_PX}, {ICON_PLAY, ICON_PLAY_PX},
};

void icon_draw(int ic, int x, int y, uint16_t c) { gfx_mask(g_icons[ic].m, g_icons[ic].px, g_icons[ic].px, x, y, c); }

// ------------------------------------------------------------------------------------- theme

uint16_t g_col[C_COUNT];

void theme_set(int dark) {
    if (dark) {
        g_col[C_BG] = rgb565(0x16, 0x18, 0x1C);
        g_col[C_TEXT] = rgb565(0xE6, 0xE8, 0xEB);
        g_col[C_DIM] = rgb565(0x9A, 0xA1, 0xAB);
        g_col[C_LINK] = rgb565(0x8A, 0xB4, 0xF8);
        g_col[C_HEAD] = rgb565(0xF2, 0xF3, 0xF5);
        g_col[C_RULE] = rgb565(0x3A, 0x3F, 0x47);
        g_col[C_CARD] = rgb565(0x22, 0x25, 0x2B);
        g_col[C_BAR] = rgb565(0x1F, 0x22, 0x28);
        g_col[C_BAR_TEXT] = rgb565(0xE6, 0xE8, 0xEB);
        g_col[C_ACCENT] = rgb565(0x8A, 0xB4, 0xF8);
        g_col[C_ON_ACCENT] = rgb565(0x10, 0x14, 0x1C);
        g_col[C_CODE_BG] = rgb565(0x26, 0x2A, 0x31);
        g_col[C_CELL_BG] = rgb565(0x24, 0x28, 0x2F);
        g_col[C_FOCUS] = rgb565(0x8A, 0xB4, 0xF8);
        g_col[C_PRESS] = rgb565(0x33, 0x38, 0x41);
        g_col[C_SCRIM] = rgb565(0x00, 0x00, 0x00);
    } else {
        g_col[C_BG] = rgb565(0xFF, 0xFF, 0xFF);
        g_col[C_TEXT] = rgb565(0x20, 0x22, 0x26);
        g_col[C_DIM] = rgb565(0x5F, 0x66, 0x70);
        g_col[C_LINK] = rgb565(0x1A, 0x5C, 0xC8);
        g_col[C_HEAD] = rgb565(0x10, 0x12, 0x16);
        g_col[C_RULE] = rgb565(0xD5, 0xD9, 0xDF);
        g_col[C_CARD] = rgb565(0xF3, 0xF5, 0xF8);
        g_col[C_BAR] = rgb565(0xF6, 0xF7, 0xF9);
        g_col[C_BAR_TEXT] = rgb565(0x20, 0x22, 0x26);
        g_col[C_ACCENT] = rgb565(0x1A, 0x5C, 0xC8);
        g_col[C_ON_ACCENT] = rgb565(0xFF, 0xFF, 0xFF);
        g_col[C_CODE_BG] = rgb565(0xF1, 0xF3, 0xF5);
        g_col[C_CELL_BG] = rgb565(0xEA, 0xEE, 0xF3);
        g_col[C_FOCUS] = rgb565(0x1A, 0x5C, 0xC8);
        g_col[C_PRESS] = rgb565(0xE2, 0xE6, 0xEC);
        g_col[C_SCRIM] = rgb565(0x00, 0x00, 0x00);
    }
}

// -------------------------------------------------------------------------------------- state

#define BAR_H 60
#define BTN 52
#define NAV_MAX 48
#define REG_MAX 128
#define SUGG_MAX 9
#define FOUND_MAX 40

enum { SCR_LIBRARY, SCR_READER, SCR_GET };
enum { OV_NONE, OV_SEARCH, OV_TOC, OV_MENU, OV_MARKS, OV_INFO, OV_VIEWER, OV_GET_INFO, OV_DLS };
enum {
    A_NONE, A_BACK, A_FORWARD, A_LIBRARY, A_SEARCH, A_TOC, A_STAR, A_MORE, A_OPEN_COLL, A_INFO_COLL,
    A_CONTINUE, A_MARKS, A_CLOSE, A_SUGG, A_FOUND, A_SCAN, A_TOC_ITEM, A_RANDOM, A_SIZE_DEC, A_SIZE_INC,
    A_THEME, A_INFO, A_MARK_OPEN, A_MARK_DEL, A_VERIFY, A_MAIN_PAGE, A_OSK, A_FIELD,
    A_GET, A_GET_CHIP, A_GET_LANG, A_GET_ROW, A_GET_START, A_GET_RETRY, A_GET_SEARCH, A_DLS, A_DL_PAUSE,
    A_DL_RESUME, A_DL_CANCEL, A_DL_OPEN, A_DELETE,
};
#define GET_CATS 16

typedef struct {
    int x, y, w, h, act, arg;
} reg_t;

typedef struct {
    int coll;
    char path[NVZIM_PATH_MAX];
    int32_t scroll;
} nav_t;

static struct {
    int screen, overlay;
    int redraw;              // R_* flags
    int quit;
    // regions of the last full drawing
    reg_t regs[REG_MAX];
    int nregs;
    int focus;               // index in regs, or -1
    int focus_link;          // reader: a focused link, or -1
    int pressed;             // region under a finger, or -1
    // the open collection and page
    int coll;
    nvzim_t *z;
    doc_t doc;
    int has_doc;
    char path[NVZIM_PATH_MAX], title[NVZIM_TITLE_MAX];
    int32_t scroll, drawn_scroll;
    float vel;               // fling, px per ms
    int starred;
    nav_t back[NAV_MAX], fwd[NAV_MAX];
    int nback, nfwd;
    // library
    int32_t lib_scroll;
    // toast
    char toast[300];
    int toast_until;
    // search
    char q[160];
    nvzim_entry_t *sugg;
    int nsugg, sel;
    int osk_on, osk_layer, osk_shift;
    char (*found)[NVZIM_TITLE_MAX];
    char (*found_path)[NVZIM_PATH_MAX];
    int nfound, scanned;
    // lists (sections, bookmarks, history)
    int list_sel;
    int32_t list_scroll;
    mark_t *marks;
    int nmarks, marks_which;
    // the picture viewer
    int view_img;
    // info
    int info_coll;
    char info_msg[160];
    // the download screen
    char get_lang[8];        // the catalog's language ("ita"...)
    int get_chip;            // 0 Recommended, 1 All, 2.. a category
    int get_search;          // the search field instead of the chips
    int32_t get_scroll;
    int get_group;           // the collection of the detail page
    int *vis, nvis;          // the groups shown
    int vis_gen;             // the catalog they were counted for (get_gen)
    char cats[GET_CATS][24];
    int ncats;
    int dl_next;             // when to ask the downloader again
    int dl_armed;            // a download whose cancel was tapped once (its id), 0 none
    int del_armed;           // a collection whose delete was tapped once, -1 none
    // input
    int touch_down, tx0, ty0, tx, ty, t0, moved, long_fired;
    int32_t drag_scroll0;
    int vy_t[6], vy_y[6], nvel;
    int now;
} A;

enum { R_ALL = 1, R_BAR = 2, R_CONTENT = 4 };

static void save_position(void);
static void draw_all(void);
static int pill(int x, int y, const char *lbl, int act, int arg, int primary);
static int active_downloads(void);

static void to_library(void) {   // the page being read becomes "Continue reading"
    save_position();
    A.screen = SCR_LIBRARY;
    A.overlay = OV_NONE;
    A.focus = -1;
    A.redraw |= R_ALL;
}

static int content_top(void) { return BAR_H; }
static int text_px(void) { static const int k[5] = {16, 18, 20, 23, 26}; return k[g_set.text_size]; }
static int col_w(void) { int w = g_w - 2 * 32; return w > 900 ? 900 : w; }
static int col_x(void) { return (g_w - col_w()) / 2; }

// Timings in the system log (GET /api/logs): what docs/ZIM_READER_PLAN.md §5 measures.
#include <stdarg.h>
static void perf(const char *fmt, ...) {
    char b[200];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(b, sizeof b, fmt, ap);
    va_end(ap);
    nv_log(2, b);
}

static void toast(const char *fmt, const char *arg) {
    snprintf(A.toast, sizeof A.toast, fmt, arg ? arg : "");
    A.toast_until = A.now + 3200;
    A.redraw |= R_ALL;
}

// one frame shown now (long work: keeps the screen and the OS watchdog alive)
static int pump(void) {
    gfx_flush();
    if (!nv_gfx_present()) { A.quit = 1; return 0; }
    return 1;
}

// ---------------------------------------------------------------------------------- regions

static int reg_add(int x, int y, int w, int h, int act, int arg) {
    if (A.nregs >= REG_MAX) return -1;
    reg_t *r = &A.regs[A.nregs];
    r->x = x; r->y = y; r->w = w; r->h = h; r->act = act; r->arg = arg;
    return A.nregs++;
}

static int reg_at(int x, int y) {
    int best = -1;
    for (int i = A.nregs - 1; i >= 0; i--) {   // the last drawn is on top
        const reg_t *r = &A.regs[i];
        if (x >= r->x && x < r->x + r->w && y >= r->y && y < r->y + r->h) return i;
    }
    for (int i = A.nregs - 1; i >= 0 && best < 0; i--) {   // a little slack for small buttons
        const reg_t *r = &A.regs[i];
        if (r->w <= BTN && x >= r->x - 8 && x < r->x + r->w + 8 && y >= r->y - 8 && y < r->y + r->h + 8) best = i;
    }
    return best;
}

static int reg_state(int idx) { return idx >= 0 && idx == A.pressed ? 2 : idx >= 0 && idx == A.focus ? 1 : 0; }

// A square icon button: background on press, ring on keyboard focus.
static void button(int x, int y, int ic, int act, int arg, int enabled, uint16_t fg) {
    const int r = enabled ? reg_add(x, y, BTN, BTN, act, arg) : -1;
    const int st = reg_state(r);
    if (st == 2) gfx_rrect(x + 2, y + 2, BTN - 4, BTN - 4, 12, g_col[C_PRESS]);
    if (st == 1) gfx_ring(x + 2, y + 2, BTN - 4, BTN - 4, 12, 2, g_col[C_FOCUS]);
    const int px = g_icons[ic].px;
    icon_draw(ic, x + (BTN - px) / 2, y + (BTN - px) / 2, enabled ? fg : g_col[C_RULE]);
}

static void bar_bg(void) {
    gfx_fill(0, 0, g_w, BAR_H, g_col[C_BAR]);
    gfx_fill(0, BAR_H - 1, g_w, 1, g_col[C_RULE]);
}

// A text button / row: returns its region.
static int row_box(int x, int y, int w, int h, int act, int arg, uint16_t bg) {
    const int r = reg_add(x, y, w, h, act, arg);
    const int st = reg_state(r);
    gfx_rrect(x, y, w, h, 10, st == 2 ? g_col[C_PRESS] : bg);
    if (st == 1) gfx_ring(x, y, w, h, 10, 2, g_col[C_FOCUS]);
    return r;
}

static void scrim(void) { gfx_blend_rect(0, 0, g_w, g_h, g_col[C_SCRIM], g_set.dark ? 140 : 110); }

// ----------------------------------------------------------------------------- collections

static void close_coll(void) {
    img_clear();
    if (A.z) nvzim_close(A.z);
    A.z = NULL;
    A.coll = -1;
}

static int open_coll(int ci) {
    if (ci < 0 || ci >= g_nlib) return -1;
    if (A.z && A.coll == ci) return 0;
    close_coll();
    nvzim_config_t cfg;
    memset(&cfg, 0, sizeof cfg);
    cfg.clusters = 2;            // the page being read and the cluster of its pictures
    cfg.cache_pages = 64;
    const int t0 = nv_millis();
    const int r = nvzim_open(&A.z, g_lib[ci].path, &cfg);
    perf("collection '%s' open in %d ms", g_lib[ci].file, nv_millis() - t0);
    if (r != NVZIM_OK) {
        toast(tr(S_OPEN_FAILED), nvzim_strerror(r));
        A.z = NULL;
        return -1;
    }
    A.coll = ci;
    return 0;
}

static void draw_loading(void) {
    const char *s = tr(S_LOADING);
    const int w = text_width(F_SANS, 18, s, (int)strlen(s)) + 40;
    gfx_rrect((g_w - w) / 2, g_h / 2 - 26, w, 52, 26, g_col[C_CARD]);
    text_draw(F_SANS, 18, (g_w - w) / 2 + 20, g_h / 2 + 6, s, (int)strlen(s), g_col[C_TEXT]);
    pump();
}

// ----------------------------------------------------------------------------------- pages

static void percent_decode(char *s) {
    char *o = s;
    for (char *p = s; *p; p++) {
        if (p[0] == '%' && p[1] && p[2]) {
            int v = 0, ok = 1;
            for (int k = 1; k <= 2; k++) {
                const char c = p[k];
                v = v * 16 + (c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 :
                              c >= 'A' && c <= 'F' ? c - 'A' + 10 : (ok = 0));
            }
            if (ok) { *o++ = (char)v; p += 2; continue; }
        }
        *o++ = *p;
    }
    *o = 0;
}

static int has_ci(const char *s, const char *needle) {   // ASCII, any case
    const size_t n = strlen(needle);
    for (; *s; s++)
        if (!strncasecmp(s, needle, n)) return 1;
    return 0;
}

static int set_doc_from_entry(nvzim_entry_t *e) {
    const char *mime = nvzim_mime_name(A.z, e->mime);
    if (strncmp(mime, "text/html", 9) && strncmp(mime, "application/xhtml", 17) && strncmp(mime, "text/plain", 10)) {
        toast(tr(S_UNSUPPORTED), mime);
        return -1;
    }
    nvzim_blob_t b;
    const int t0 = nv_millis();
    int r = nvzim_read(A.z, e, &b);
    if (r < 0) { toast(tr(S_OPEN_FAILED), nvzim_strerror(r)); return -1; }
    const int t1 = nv_millis();
    const char *html = (const char *)b.data;
    size_t hn = (size_t)b.size;
    char *conv = NULL;
    if (has_ci(mime, "iso-8859-1")) {
        conv = nvhtml_latin1_to_utf8(html, hn, &hn);
        html = conv;
    }
    char *text = NULL;
    size_t tn = 0;
    if (html && !strncmp(mime, "text/plain", 10)) {   // plain text: preformatted lines
        text = (char *)malloc(hn * 2 + 2);
        if (text) {
            size_t k = 0;
            text[k++] = NVHTML_PRE;
            for (size_t i = 0; i < hn; i++) {
                const char c = html[i];
                if (c == '\r') continue;
                if ((unsigned char)c < 0x20 && c != '\n' && c != '\t') continue;
                text[k++] = c;
                if (c == '\n') text[k++] = NVHTML_PRE;
            }
            text[k] = 0;
            tn = k;
        }
    } else if (html) {
        nvhtml_opts_t o;
        memset(&o, 0, sizeof o);
        o.rich = 1;
        o.back_matter = 1;
        text = nvhtml_text(html, hn, &o, &tn);
    }
    free(conv);
    const size_t html_n = (size_t)b.size;
    nvzim_blob_release(A.z, &b);
    if (!text) { toast(tr(S_OPEN_FAILED), nvzim_strerror(NVZIM_ENOMEM)); return -1; }
    const int t2 = nv_millis();
    doc_layout(&A.doc, text, tn, col_w(), text_px());
    perf("open '%s': read %d ms, html %d ms, layout %d ms (%u KB, %d runs, %d px)", e->title, t1 - t0, t2 - t1,
         nv_millis() - t2, (unsigned)(html_n / 1024), A.doc.nops, (int)A.doc.height);
    A.has_doc = 1;
    snprintf(A.path, sizeof A.path, "%s", e->path);
    snprintf(A.title, sizeof A.title, "%s", e->title);
    return 0;
}

static void push_nav(nav_t *stack, int *n, int coll, const char *path, int32_t scroll) {
    if (*n == NAV_MAX) { memmove(stack, stack + 1, sizeof(nav_t) * (NAV_MAX - 1)); (*n)--; }
    stack[*n].coll = coll;
    snprintf(stack[*n].path, sizeof stack[*n].path, "%s", path);
    stack[*n].scroll = scroll;
    (*n)++;
}

// Opens a page of the open collection (already resolved). push: remember where we were.
static int show_entry(nvzim_entry_t *e, const char *anchor, int push) {
    const int had = A.has_doc && A.screen == SCR_READER;
    nav_t from;
    from.coll = A.coll;
    snprintf(from.path, sizeof from.path, "%s", A.path);
    from.scroll = A.scroll;
    draw_loading();
    if (set_doc_from_entry(e) < 0) return -1;
    if (push && had) {
        push_nav(A.back, &A.nback, from.coll, from.path, from.scroll);
        A.nfwd = 0;
    }
    A.screen = SCR_READER;
    A.overlay = OV_NONE;
    A.scroll = 0;
    A.vel = 0;
    A.focus = -1;
    A.focus_link = -1;
    if (anchor && anchor[0]) {
        const int y = doc_anchor_y(&A.doc, anchor, (int)strlen(anchor));
        if (y >= 0) A.scroll = y - 8;
    }
    A.starred = marks_has(MARK_BOOKMARKS, g_lib[A.coll].file, A.path);
    marks_add(MARK_HISTORY, g_lib[A.coll].file, A.path, A.title);
    save_position();   // the OS may close the app without letting it finish: "Continue reading" is saved now
    A.redraw |= R_ALL;
    return 0;
}

// A path of the collection -> its entry, redirects followed: an article, or "N/path" with a namespace
// in front (ZIM 5 pictures and links: "I/x.png", "A/Page"); the namespace-less path is tried too.
static int find_entry(const char *path, nvzim_entry_t *e) {
    int r = nvzim_find_article(A.z, path, e);
    if (r < 0 && strlen(path) > 2 && path[1] == '/') {
        r = nvzim_find(A.z, path[0], path + 2, e);
        if (r < 0) r = nvzim_find_article(A.z, path + 2, e);
    }
    if (r < 0) r = nvzim_find(A.z, 'C', path, e);   // a picture outside the listing of articles
    if (r >= 0) r = nvzim_resolve(A.z, e);
    return r;
}

static int open_path(int ci, const char *path, const char *anchor, int push, int32_t scroll) {
    if (open_coll(ci) < 0) return -1;
    nvzim_entry_t *e = (nvzim_entry_t *)malloc(sizeof *e);
    if (!e) return -1;
    int r = find_entry(path, e);
    if (r < 0) { toast(tr(S_NOT_FOUND), NULL); free(e); return -1; }
    r = show_entry(e, anchor, push);
    free(e);
    if (r == 0 && scroll > 0) A.scroll = scroll;
    return r;
}

static int open_main(int ci) {
    if (open_coll(ci) < 0) return -1;
    nvzim_entry_t *e = (nvzim_entry_t *)malloc(sizeof *e);
    if (!e) return -1;
    int r = nvzim_main_page(A.z, e);
    if (r < 0) r = nvzim_random(A.z, (uint32_t)nv_rand(), e);
    if (r >= 0) r = show_entry(e, NULL, 1);
    else toast(tr(S_NOT_FOUND), NULL);
    free(e);
    return r;
}

static void open_random(void) {
    if (!A.z) return;
    nvzim_entry_t *e = (nvzim_entry_t *)malloc(sizeof *e);
    if (!e) return;
    if (nvzim_random(A.z, (uint32_t)nv_rand() ^ (uint32_t)A.now, e) >= 0) show_entry(e, NULL, 1);
    free(e);
}

// An href or a src of the page (percent-decoded, no fragment) -> a path in the collection, relative to
// the page's own folder. ZIM 5 pages live in "A/" and point at "../I/..." too: the namespace stays in
// front ("I/x.png"), find_entry knows it.
static void rel_path(const char *href, char *out, size_t cap) {
    const nvzim_info_t *info = nvzim_get_info(A.z);
    char base[NVZIM_PATH_MAX + 2];
    if (info->new_scheme) snprintf(base, sizeof base, "%s", A.path);
    else snprintf(base, sizeof base, "%c/%s", info->article_ns, A.path);
    char full[NVZIM_PATH_MAX * 2];
    if (href[0] == '/') snprintf(full, sizeof full, "%s", href + 1);
    else {
        char *slash = strrchr(base, '/');
        snprintf(full, sizeof full, "%.*s%s", slash ? (int)(slash - base + 1) : 0, base, href);
    }
    size_t k = 0;   // . and .. segments
    for (char *p = full; *p;) {
        char *e = strchr(p, '/');
        const size_t n = e ? (size_t)(e - p) : strlen(p);
        if (n == 1 && p[0] == '.') { /* nothing */ }
        else if (n == 2 && p[0] == '.' && p[1] == '.') {
            while (k > 0 && out[k - 1] == '/') k--;
            while (k > 0 && out[k - 1] != '/') k--;
        } else if (n) {
            if (k + n + 2 >= cap) break;
            memcpy(out + k, p, n);
            k += n;
            if (e) out[k++] = '/';
        }
        p = e ? e + 1 : p + n;
    }
    out[k] = 0;
    if (k && out[k - 1] == '/') out[--k] = 0;
}

// A link of the page: another page of the collection, a section of this one, or the outside world.
static void follow(int link) {
    if (link < 0 || link >= A.doc.nlinks) return;
    const link_t *l = &A.doc.links[link];
    char href[NVHTML_HREF_MAX];
    snprintf(href, sizeof href, "%.*s", (int)l->len, A.doc.text + l->off);
    if (strstr(href, "://") || !strncmp(href, "mailto:", 7) || !strncmp(href, "//", 2) || !strncmp(href, "tel:", 4)) {
        toast(tr(S_EXTERNAL_LINK), href);
        return;
    }
    char *frag = strchr(href, '#');
    char anchor[256] = "";
    if (frag) {
        *frag = 0;
        snprintf(anchor, sizeof anchor, "%s", frag + 1);
        percent_decode(anchor);
    }
    char *q = strchr(href, '?');
    if (q) *q = 0;
    if (!href[0]) {   // a section of this page
        const int y = doc_anchor_y(&A.doc, anchor, (int)strlen(anchor));
        if (y >= 0) {
            push_nav(A.back, &A.nback, A.coll, A.path, A.scroll);
            A.nfwd = 0;
            A.scroll = y - 8;
            A.redraw |= R_CONTENT;
        }
        return;
    }
    percent_decode(href);
    char out[NVZIM_PATH_MAX * 2];
    rel_path(href, out, sizeof out);
    open_path(A.coll, out, anchor, 1, 0);
}

// ------------------------------------------------------------------------------------ pictures

static int image_path(const doc_t *d, int idx, char *out, size_t cap) {
    const dimg_t *im = &d->imgs[idx];
    char src[NVHTML_HREF_MAX];
    snprintf(src, sizeof src, "%.*s", (int)im->src_len, d->text + im->src_off);
    if (strstr(src, "://") || !strncmp(src, "//", 2) || !strncmp(src, "data:", 5)) return -1;   // not in the file
    char *q = strpbrk(src, "?#");
    if (q) *q = 0;
    percent_decode(src);
    rel_path(src, out, cap);
    return 0;
}

void doc_draw_image(const doc_t *d, int idx, int x, int y, int w, int h) {
    const dimg_t *im = &d->imgs[idx];
    char path[NVZIM_PATH_MAX * 2];
    int st = -1;
    if (A.z && image_path(d, idx, path, sizeof path) == 0) {
        st = img_lookup(path, w, h, im->mask);
        if (st == 1 && img_draw(path, w, h, x, y, g_col[C_TEXT]) == 0) return;
    }
    if (im->mask) {   // a formula not drawn (yet): its TeX, small, in the code font, cut to its box
        if (st < 0 && im->alt_len) {
            gfx_clip(x, y, x + w, y + h);
            const int px = h * 7 / 10 < 10 ? 10 : h * 7 / 10;
            text_draw(F_MONO, px, x, y + h - h / 4, d->text + im->alt_off, im->alt_len, g_col[C_DIM]);
            gfx_noclip();
        }
        return;
    }
    gfx_rrect(x, y, w, h, 6, g_col[C_CODE_BG]);   // a placeholder while it decodes (or when it cannot)
    if (st < 0 && w > 40 && h > 24) icon_draw(IC_BOOK, x + (w - 48) / 2, y + (h - 48) / 2, g_col[C_RULE]);
}

static int img_read(void *ctx, const char *path, const uint8_t **data, size_t *n, void **tok) {
    (void)ctx;
    nvzim_entry_t *e = (nvzim_entry_t *)malloc(sizeof *e);
    nvzim_blob_t *b = (nvzim_blob_t *)malloc(sizeof *b);
    if (!e || !b || !A.z || find_entry(path, e) < 0 || nvzim_read(A.z, e, b) < 0) {
        free(e);
        free(b);
        return -1;
    }
    free(e);
    *data = b->data;
    *n = (size_t)b->size;
    *tok = b;
    return 0;
}

static int img_cluster_ready(void *ctx, const char *path) {
    (void)ctx;
    nvzim_entry_t *e = (nvzim_entry_t *)malloc(sizeof *e);
    int r = -1;
    if (e && A.z && find_entry(path, e) >= 0) r = nvzim_cluster_cached(A.z, e->cluster);
    free(e);
    return r;
}

static void img_done(void *ctx, void *tok) {
    (void)ctx;
    nvzim_blob_release(A.z, (nvzim_blob_t *)tok);
    free(tok);
}

static void go_back(void) {
    if (A.overlay != OV_NONE) {
        A.overlay = OV_NONE;
        A.focus = -1;
        A.redraw |= R_ALL;
        return;
    }
    if (A.screen == SCR_GET) {
        if (A.get_search) { A.get_search = 0; A.q[0] = 0; A.osk_on = 0; A.redraw |= R_ALL; return; }
        A.screen = SCR_LIBRARY;
        A.focus = -1;
        A.redraw |= R_ALL;
        return;
    }
    if (A.screen == SCR_READER) {
        if (A.nback > 0) {
            const nav_t n = A.back[--A.nback];
            push_nav(A.fwd, &A.nfwd, A.coll, A.path, A.scroll);
            if (open_path(n.coll, n.path, NULL, 0, n.scroll) < 0) to_library();
            return;
        }
        to_library();
        return;
    }
    A.quit = 1;
}

static void go_forward(void) {
    if (!A.nfwd) return;
    const nav_t n = A.fwd[--A.nfwd];
    push_nav(A.back, &A.nback, A.coll, A.path, A.scroll);
    open_path(n.coll, n.path, NULL, 0, n.scroll);
}

static void toggle_star(void) {
    if (!A.has_doc || A.coll < 0) return;
    if (A.starred) {
        marks_remove(MARK_BOOKMARKS, g_lib[A.coll].file, A.path);
        toast(tr(S_BOOKMARK_REMOVED), NULL);
    } else {
        marks_add(MARK_BOOKMARKS, g_lib[A.coll].file, A.path, A.title);
        toast(tr(S_BOOKMARK_ADDED), NULL);
    }
    A.starred = !A.starred;
}

static void set_text_size(int s) {
    if (s < 0 || s > 4 || s == g_set.text_size) return;
    // keep the reading position: the same fraction of the page
    const float frac = A.doc.height > 0 ? (float)A.scroll / (float)A.doc.height : 0;
    g_set.text_size = s;
    settings_save();
    if (A.has_doc) {
        doc_relayout(&A.doc, col_w(), text_px());
        A.scroll = (int32_t)(frac * A.doc.height);
    }
    A.redraw |= R_ALL;
}

// ------------------------------------------------------------------------------------ search

static void search_update(void) {
    if (A.screen == SCR_GET) { A.get_scroll = 0; return; }   // the catalog filters as it draws
    A.nsugg = 0;
    A.sel = 0;
    A.nfound = 0;
    A.scanned = 0;
    if (!A.z || !A.q[0]) return;
    const int t0 = nv_millis();
    A.nsugg = nvzim_suggest(A.z, A.q, A.sugg, SUGG_MAX);
    if (A.nsugg < 0) A.nsugg = 0;
    perf("suggest '%s': %d in %d ms", A.q, A.nsugg, nv_millis() - t0);
}

static void lower_utf8(const char *s, char *out, size_t cap) {   // ASCII + Latin-1 letters
    size_t k = 0;
    for (const unsigned char *p = (const unsigned char *)s; *p && k + 2 < cap; p++) {
        unsigned char c = *p;
        if (c >= 'A' && c <= 'Z') c += 32;
        if (c == 0xC3 && p[1] >= 0x80 && p[1] <= 0x9E && p[1] != 0x97) { out[k++] = (char)c; out[k++] = (char)(p[1] + 0x20); p++; continue; }
        out[k++] = (char)c;
    }
    out[k] = 0;
}

typedef struct { char needle[200]; long seen; long total; int stop; } scan_ctx_t;

static int scan_cb(void *ctx, const nvzim_entry_t *e) {
    scan_ctx_t *c = (scan_ctx_t *)ctx;
    if ((++c->seen & 4095) == 0) {   // progress, and the heartbeat of the screen
        char msg[120];
        snprintf(msg, sizeof msg, tr(S_SEARCHING), (int)(c->total ? c->seen * 100 / c->total : 0));
        gfx_fill(0, g_h - 48, g_w, 48, g_col[C_CARD]);
        text_draw(F_SANS, 17, 24, g_h - 18, msg, (int)strlen(msg), g_col[C_TEXT]);
        if (!pump() || nv_gfx_back()) { c->stop = 1; return 1; }
    }
    if (nvzim_is_redirect(e)) return 0;
    char t[NVZIM_TITLE_MAX];
    lower_utf8(e->title, t, sizeof t);
    if (strstr(t, c->needle)) {
        snprintf(A.found[A.nfound], NVZIM_TITLE_MAX, "%s", e->title);
        snprintf(A.found_path[A.nfound], NVZIM_PATH_MAX, "%s", e->path);
        if (++A.nfound >= FOUND_MAX) return 1;
    }
    return 0;
}

static void search_scan(void) {
    if (!A.z || !A.q[0]) return;
    scan_ctx_t *c = (scan_ctx_t *)calloc(1, sizeof *c);
    if (!c) return;
    lower_utf8(A.q, c->needle, sizeof c->needle);
    c->total = nvzim_get_info(A.z)->entry_count;
    A.nfound = 0;
    nvzim_scan(A.z, nvzim_get_info(A.z)->article_ns, scan_cb, c);
    A.scanned = 1;
    free(c);
    A.redraw |= R_ALL;
}

static void q_append(uint32_t cp) {
    char u[5];
    int n = 0;
    if (cp < 0x80) u[n++] = (char)cp;
    else if (cp < 0x800) { u[n++] = (char)(0xC0 | cp >> 6); u[n++] = (char)(0x80 | (cp & 0x3F)); }
    else { u[n++] = (char)(0xE0 | cp >> 12); u[n++] = (char)(0x80 | (cp >> 6 & 0x3F)); u[n++] = (char)(0x80 | (cp & 0x3F)); }
    const size_t l = strlen(A.q);
    if (l + n + 1 >= sizeof A.q) return;
    memcpy(A.q + l, u, (size_t)n);
    A.q[l + n] = 0;
    search_update();
    A.redraw |= R_ALL;
}

static void q_backspace(void) {
    size_t l = strlen(A.q);
    if (!l) return;
    do l--; while (l > 0 && ((unsigned char)A.q[l] & 0xC0) == 0x80);
    A.q[l] = 0;
    search_update();
    A.redraw |= R_ALL;
}

static void open_search(void) {
    if (!A.z) return;
    A.overlay = OV_SEARCH;
    A.q[0] = 0;
    A.nsugg = A.nfound = A.scanned = 0;
    A.osk_on = !kbd_present();
    A.osk_layer = A.osk_shift = 0;
    A.focus = -1;
    // The screen first, then the title listing is located (half a second on a big collection):
    // the user is reaching for the first key meanwhile, and that key is answered at once.
    draw_all();
    A.redraw = 0;
    pump();
    const int t0 = nv_millis();
    if (nvzim_title_count(A.z) > 0) nvzim_suggest(A.z, "m", A.sugg, 1);   // the top of the title tree, cached
    perf("search: titles ready in %d ms", nv_millis() - t0);
}

static void search_choose(int i) {
    if (i < 0) return;
    char path[NVZIM_PATH_MAX];
    if (i < A.nsugg) snprintf(path, sizeof path, "%s", A.sugg[i].path);
    else if (i - A.nsugg < A.nfound) snprintf(path, sizeof path, "%s", A.found_path[i - A.nsugg]);
    else return;
    A.overlay = OV_NONE;
    open_path(A.coll, path, NULL, 1, 0);
}

// ------------------------------------------------------------------------------ the drawing

static void draw_toast(void) {
    if (!A.toast[0] || A.now > A.toast_until) return;
    const int maxw = g_w - 80;
    int w = text_width(F_SANS, 17, A.toast, (int)strlen(A.toast)) + 40;
    if (w > maxw) w = maxw;
    const int x = (g_w - w) / 2, y = g_h - 84;
    gfx_rrect(x, y, w, 48, 24, g_set.dark ? rgb565(0xE6, 0xE8, 0xEB) : rgb565(0x2A, 0x2E, 0x35));
    text_draw_fit(F_SANS, 17, x + 20, y + 30, A.toast, (int)strlen(A.toast), w - 40,
                  g_set.dark ? rgb565(0x16, 0x18, 0x1C) : rgb565(0xF5, 0xF6, 0xF8));
}

static void draw_bar_title(const char *t, int x, int maxw) {
    text_draw_fit(F_BOLD, 21, x, BAR_H / 2 + 8, t, (int)strlen(t), maxw, g_col[C_BAR_TEXT]);
}

static void draw_cover(const coll_t *c, int x, int y) {
    gfx_rrect(x, y, 64, 64, 12, g_set.dark ? rgb565(0x2C, 0x30, 0x37) : rgb565(0xFF, 0xFF, 0xFF));
    if (c->cover) {
        for (int j = 0; j < COVER_PX; j++)
            for (int i = 0; i < COVER_PX; i++) {
                const uint8_t *p = c->cover + (j * COVER_PX + i) * 4;
                if (!p[3]) continue;
                const int sx = x + 8 + i, sy = y + 8 + j;
                if (sx < 0 || sy < 0 || sx >= g_w || sy >= g_h) continue;
                uint16_t *d = g_rows[sy] + sx;
                *d = blend565(*d, rgb565(p[0], p[1], p[2]), p[3]);
            }
        gfx_dirty(y, y + 64);
    } else {
        icon_draw(IC_BOOK, x + 8, y + 8, g_col[C_ACCENT]);
    }
}

static void draw_library(void) {
    gfx_fill(0, 0, g_w, g_h, g_col[C_BG]);
    const int pad = 24, gap = 16;
    const int cols = g_w >= 900 ? 2 : 1;
    const int cw = (g_w - 2 * pad - (cols - 1) * gap) / cols, ch = 96;
    // the cards, under the bar
    gfx_clip(0, BAR_H, g_w, g_h);
    int y = BAR_H + 16 - A.lib_scroll;
    const int cont = g_set.last_file[0] ? lib_find(g_set.last_file) : -1;
    if (cont >= 0 && g_set.last_path[0]) {
        const int r = row_box(pad, y, g_w - 2 * pad, 64, A_CONTINUE, 0, g_col[C_CARD]);
        (void)r;
        icon_draw(IC_HISTORY, pad + 16, y + 20, g_col[C_ACCENT]);
        const char *lbl = tr(S_CONTINUE);
        const int lw = text_draw(F_BOLD, 18, pad + 56, y + 39, lbl, (int)strlen(lbl), g_col[C_ACCENT]);
        char t[400];
        mark_t *h = NULL;
        const int nh = marks_load(MARK_HISTORY, &h);
        const char *title = g_set.last_path;
        for (int i = 0; i < nh; i++)
            if (!strcmp(h[i].file, g_set.last_file) && !strcmp(h[i].path, g_set.last_path)) { title = h[i].title; break; }
        snprintf(t, sizeof t, "%s \xC2\xB7 %s", title, g_lib[cont].title[0] ? g_lib[cont].title : g_lib[cont].file);
        text_draw_fit(F_SANS, 18, pad + 56 + lw + 14, y + 39, t, (int)strlen(t), g_w - 2 * pad - lw - 100, g_col[C_TEXT]);
        free(h);
        y += 64 + gap;
    }
    if (!g_nlib) {
        y += 40;
        icon_draw(IC_BOOK, (g_w - 48) / 2, y, g_col[C_DIM]);
        y += 90;
        const char *t = tr(S_EMPTY_TITLE);
        text_draw(F_BOLD, 24, (g_w - text_width(F_BOLD, 24, t, (int)strlen(t))) / 2, y, t, (int)strlen(t), g_col[C_TEXT]);
        y += 26;
        for (int s = S_EMPTY_1; s <= S_EMPTY_3; s++) {   // wrapped by hand: a few short lines
            const char *p = tr(s);
            int n = (int)strlen(p);
            while (n > 0) {
                y += 30;
                int k = text_fit(F_SANS, 18, p, n, g_w - 200);
                if (k < n) { int b = k; while (b > 0 && p[b] != ' ') b--; if (b > 0) k = b; }
                text_draw(F_SANS, 18, (g_w - text_width(F_SANS, 18, p, k)) / 2, y, p, k, g_col[C_DIM]);
                p += k;
                n -= k;
                while (n > 0 && *p == ' ') { p++; n--; }
            }
            y += 10;
        }
        const char *lbl = tr(S_GET_TITLE);
        const int pw = text_width(F_BOLD, 17, lbl, (int)strlen(lbl)) + 40;
        pill((g_w - pw) / 2, y + 16, lbl, A_GET, 0, 1);
    }
    for (int i = 0; i < g_nlib; i++) {
        const coll_t *c = &g_lib[i];
        const int cx = pad + (i % cols) * (cw + gap), cy = y + (i / cols) * (ch + gap);
        if (cy > g_h || cy + ch < BAR_H) { continue; }
        row_box(cx, cy, cw - BTN - 4, ch, A_OPEN_COLL, i, g_col[C_CARD]);
        gfx_fill(cx + cw - BTN - 4, cy, BTN + 4, ch, g_col[C_BG]);
        gfx_rrect(cx + cw - BTN - 14, cy, BTN + 14, ch, 10, g_col[C_CARD]);
        draw_cover(c, cx + 14, cy + 16);
        const int tx = cx + 92, tw = cw - 92 - BTN - 16;
        const char *title = c->title[0] ? c->title : c->file;
        text_draw_fit(F_BOLD, 19, tx, cy + 32, title, (int)strlen(title), tw, g_col[C_TEXT]);
        if (c->bad) {
            const char *s = tr(S_DAMAGED);
            text_draw_fit(F_SANS, 15, tx, cy + 56, s, (int)strlen(s), tw, g_col[C_DIM]);
        } else if (c->desc[0]) {
            text_draw_fit(F_SANS, 15, tx, cy + 56, c->desc, (int)strlen(c->desc), tw, g_col[C_DIM]);
        }
        char meta[200], d[48], sz[32], n[32];
        fmt_date(c->date, d, sizeof d);
        fmt_size(c->size, sz, sizeof sz);
        fmt_count(c->articles, n, sizeof n);
        char lang[24];
        snprintf(lang, sizeof lang, "%s", c->lang);
        lang[strcspn(lang, ",")] = 0;
        snprintf(meta, sizeof meta, "%s \xC2\xB7 %s \xC2\xB7 %s %s \xC2\xB7 %s", lang_name(lang), d, n, tr(S_ARTICLES), sz);
        text_draw_fit(F_SANS, 14, tx, cy + 80, meta, (int)strlen(meta), tw, g_col[C_DIM]);
        button(cx + cw - BTN - 4, cy + (ch - BTN) / 2, IC_INFO, A_INFO_COLL, i, 1, g_col[C_DIM]);
    }
    gfx_noclip();
    // the bar on top
    bar_bg();
    icon_draw(IC_LIBRARY, 22, (BAR_H - 24) / 2, g_col[C_ACCENT]);
    draw_bar_title(tr(S_LIBRARY), 60, g_w - 300);
    button(g_w - 3 * BTN - 16, (BAR_H - BTN) / 2, IC_DOWNLOAD, A_GET, 0, 1, g_col[C_BAR_TEXT]);
    if (active_downloads()) {
        gfx_rrect(g_w - 3 * BTN - 16 + BTN - 18, 10, 12, 12, 6, g_col[C_ACCENT]);
    }
    button(g_w - 2 * BTN - 12, (BAR_H - BTN) / 2, IC_HISTORY, A_MARKS, MARK_HISTORY, 1, g_col[C_BAR_TEXT]);
    button(g_w - BTN - 8, (BAR_H - BTN) / 2, IC_STAR, A_MARKS, MARK_BOOKMARKS, 1, g_col[C_BAR_TEXT]);
}

static int32_t lib_height(void) {
    const int cols = g_w >= 900 ? 2 : 1;
    int32_t h = 16 + ((g_nlib + cols - 1) / cols) * (96 + 16) + 16;
    if (g_set.last_file[0]) h += 64 + 16;
    return h;
}

static int32_t max_scroll(void) {
    const int32_t m = A.doc.height - (g_h - content_top());
    return m > 0 ? m : 0;
}

static void draw_thumb(void) {   // where we are in the page
    const int top = content_top(), h = g_h - top;
    gfx_fill(g_w - 7, top, 7, h, g_col[C_BG]);
    if (A.doc.height <= h) return;
    int th = (int)((int64_t)h * h / A.doc.height);
    if (th < 28) th = 28;
    const int ty = top + (int)((int64_t)(h - th) * A.scroll / (max_scroll() ? max_scroll() : 1));
    gfx_rrect(g_w - 6, ty, 4, th, 2, g_col[C_RULE]);
}

static void draw_reader_content(void) {
    const int top = content_top();
    gfx_fill(0, top, g_w, g_h - top, g_col[C_BG]);
    doc_draw(&A.doc, col_x(), top, A.scroll, g_h - top, A.focus_link);
    draw_thumb();
    A.drawn_scroll = A.scroll;
}

static void draw_reader_bar(void) {
    const int saved = A.nregs;
    (void)saved;
    bar_bg();
    const int by = (BAR_H - BTN) / 2;
    int x = 6;
    button(x, by, IC_BACK, A_BACK, 0, 1, g_col[C_BAR_TEXT]);
    x += BTN;
    button(x, by, IC_FORWARD, A_FORWARD, 0, A.nfwd > 0, g_col[C_BAR_TEXT]);
    x += BTN;
    button(x, by, IC_LIBRARY, A_LIBRARY, 0, 1, g_col[C_BAR_TEXT]);
    x += BTN + 10;
    const int right = g_w - 4 * BTN - 10;
    draw_bar_title(A.title, x, right - x - 10);
    int rx = right;
    button(rx, by, IC_SEARCH, A_SEARCH, 0, 1, g_col[C_BAR_TEXT]);
    rx += BTN;
    button(rx, by, IC_TOC, A_TOC, 0, A.doc.ntoc > 0, g_col[C_BAR_TEXT]);
    rx += BTN;
    button(rx, by, A.starred ? IC_STAR_ON : IC_STAR, A_STAR, 0, 1, A.starred ? g_col[C_ACCENT] : g_col[C_BAR_TEXT]);
    rx += BTN;
    button(rx, by, IC_MORE, A_MORE, 0, 1, g_col[C_BAR_TEXT]);
}

// ---- overlays

static void draw_field(int x, int y, int w) {
    gfx_rrect(x, y, w, 44, 22, g_col[C_CARD]);
    gfx_ring(x, y, w, 44, 22, 2, g_col[C_ACCENT]);
    icon_draw(IC_SEARCH, x + 14, y + 10, g_col[C_DIM]);
    const int tx = x + 48, tw = w - 70;
    if (!A.q[0]) {
        char ph[200];
        const coll_t *c = A.coll >= 0 ? &g_lib[A.coll] : NULL;
        snprintf(ph, sizeof ph, tr(S_SEARCH_IN), c ? (c->title[0] ? c->title : c->file) : "");
        text_draw_fit(F_SANS, 18, tx, y + 29, ph, (int)strlen(ph), tw, g_col[C_DIM]);
        gfx_fill(tx - 1, y + 11, 2, 22, g_col[C_ACCENT]);
        return;
    }
    // the end of the query when it is long
    const char *q = A.q;
    int n = (int)strlen(q);
    while (n > 0 && text_width(F_SANS, 18, q, n) > tw - 6) {
        utf8_next(&q, A.q + strlen(A.q));
        n = (int)strlen(q);
    }
    const int w2 = text_draw(F_SANS, 18, tx, y + 29, q, n, g_col[C_TEXT]);
    gfx_fill(tx + w2 + 1, y + 11, 2, 22, g_col[C_ACCENT]);
}

// The on-screen keyboard: letters (QWERTY), accents and symbols.
typedef struct { const char *lbl; int w; int code; } okey_t;   // code: char, or < 0 special
enum { OK_SHIFT = -1, OK_BS = -2, OK_LAYER = -3, OK_SPACE = -4, OK_ENTER = -5, OK_HIDE = -6 };
static const okey_t kOsk[2][4][11] = {
    {{{"q", 10, 'q'}, {"w", 10, 'w'}, {"e", 10, 'e'}, {"r", 10, 'r'}, {"t", 10, 't'}, {"y", 10, 'y'}, {"u", 10, 'u'},
      {"i", 10, 'i'}, {"o", 10, 'o'}, {"p", 10, 'p'}, {0, 0, 0}},
     {{"a", 10, 'a'}, {"s", 10, 's'}, {"d", 10, 'd'}, {"f", 10, 'f'}, {"g", 10, 'g'}, {"h", 10, 'h'}, {"j", 10, 'j'},
      {"k", 10, 'k'}, {"l", 10, 'l'}, {"'", 10, '\''}, {0, 0, 0}},
     {{"", 15, OK_SHIFT}, {"z", 10, 'z'}, {"x", 10, 'x'}, {"c", 10, 'c'}, {"v", 10, 'v'}, {"b", 10, 'b'}, {"n", 10, 'n'},
      {"m", 10, 'm'}, {"", 15, OK_BS}, {0, 0, 0}},
     {{"?123", 15, OK_LAYER}, {"", 10, OK_HIDE}, {"-", 10, '-'}, {" ", 40, OK_SPACE}, {".", 10, '.'}, {"", 15, OK_ENTER},
      {0, 0, 0}}},
    {{{"1", 10, '1'}, {"2", 10, '2'}, {"3", 10, '3'}, {"4", 10, '4'}, {"5", 10, '5'}, {"6", 10, '6'}, {"7", 10, '7'},
      {"8", 10, '8'}, {"9", 10, '9'}, {"0", 10, '0'}, {0, 0, 0}},
     {{"\xC3\xA0", 10, 0xE0}, {"\xC3\xA8", 10, 0xE8}, {"\xC3\xA9", 10, 0xE9}, {"\xC3\xAC", 10, 0xEC}, {"\xC3\xB2", 10, 0xF2},
      {"\xC3\xB9", 10, 0xF9}, {"\xC3\xA7", 10, 0xE7}, {"\xC3\xB1", 10, 0xF1}, {"\xC3\xBC", 10, 0xFC}, {"\xC3\xB6", 10, 0xF6}, {0, 0, 0}},
     {{"(", 10, '('}, {")", 10, ')'}, {"/", 10, '/'}, {":", 10, ':'}, {",", 10, ','}, {"&", 10, '&'}, {"+", 10, '+'},
      {"\xC3\xA4", 10, 0xE4}, {"\xC3\x9F", 10, 0xDF}, {"", 10, OK_BS}, {0, 0, 0}},
     {{"abc", 15, OK_LAYER}, {"", 10, OK_HIDE}, {"\"", 10, '"'}, {" ", 40, OK_SPACE}, {"?", 10, '?'}, {"", 15, OK_ENTER},
      {0, 0, 0}}},
};
#define OSK_H 248

static void draw_osk(void) {
    const int y0 = g_h - OSK_H;
    gfx_fill(0, y0, g_w, OSK_H, g_set.dark ? rgb565(0x1B, 0x1E, 0x23) : rgb565(0xE4, 0xE7, 0xEC));
    const int kh = 52, gapy = 8, unit = (g_w - 24) / 100;
    for (int r = 0; r < 4; r++) {
        int tot = 0;
        for (int k = 0; kOsk[A.osk_layer][r][k].lbl; k++) tot += kOsk[A.osk_layer][r][k].w;
        int x = 12 + (100 - tot) * unit / 2;
        const int y = y0 + 10 + r * (kh + gapy);
        for (int k = 0; kOsk[A.osk_layer][r][k].lbl; k++) {
            const okey_t *o = &kOsk[A.osk_layer][r][k];
            const int w = o->w * unit - 6;
            const int special = o->code < 0 && o->code != OK_SPACE;
            const int idx = reg_add(x, y, w, kh, A_OSK, r * 16 + k);
            uint16_t bg = special ? (g_set.dark ? rgb565(0x2B, 0x2F, 0x36) : rgb565(0xCD, 0xD2, 0xDA))
                                  : (g_set.dark ? rgb565(0x3A, 0x3F, 0x48) : rgb565(0xFF, 0xFF, 0xFF));
            if (o->code == OK_ENTER) bg = g_col[C_ACCENT];
            if (o->code == OK_SHIFT && A.osk_shift) bg = g_col[C_ACCENT];
            if (idx == A.pressed) bg = g_col[C_PRESS];
            gfx_rrect(x, y, w, kh, 8, bg);
            const uint16_t fg = (o->code == OK_ENTER || (o->code == OK_SHIFT && A.osk_shift)) ? g_col[C_ON_ACCENT] : g_col[C_TEXT];
            int ic = -1;
            if (o->code == OK_SHIFT) ic = IC_SHIFT;
            if (o->code == OK_BS) ic = IC_BACKSPACE;
            if (o->code == OK_ENTER) ic = IC_SEARCH;
            if (o->code == OK_HIDE) ic = IC_KEYBOARD;
            if (ic >= 0) {
                icon_draw(ic, x + (w - 24) / 2, y + (kh - 24) / 2, fg);
            } else {
                char lbl[8];
                snprintf(lbl, sizeof lbl, "%s", o->lbl);
                if (A.osk_shift && lbl[0] >= 'a' && lbl[0] <= 'z' && !lbl[1]) lbl[0] = (char)(lbl[0] - 32);
                const int px = o->code == OK_LAYER ? 17 : 22;
                const int tw = text_width(F_SANS, px, lbl, (int)strlen(lbl));
                text_draw(F_SANS, px, x + (w - tw) / 2, y + kh / 2 + px * 36 / 100, lbl, (int)strlen(lbl), fg);
            }
            x += o->w * unit;
        }
    }
}

static void osk_press(int code) {
    const okey_t *o = &kOsk[A.osk_layer][code / 16][code % 16];
    switch (o->code) {
    case OK_SHIFT: A.osk_shift = !A.osk_shift; A.redraw |= R_ALL; return;
    case OK_BS: q_backspace(); return;
    case OK_LAYER: A.osk_layer = !A.osk_layer; A.redraw |= R_ALL; return;
    case OK_SPACE: q_append(' '); return;
    case OK_ENTER: if (A.screen == SCR_GET) { A.osk_on = 0; A.redraw |= R_ALL; } else search_choose(A.sel); return;
    case OK_HIDE: A.osk_on = 0; A.redraw |= R_ALL; return;
    default: break;
    }
    uint32_t c = (uint32_t)o->code;
    if (A.osk_shift && c >= 'a' && c <= 'z') c -= 32;
    else if (A.osk_shift && c >= 0xE0 && c <= 0xFE && c != 0xF7) c -= 32;
    q_append(c);
    if (A.osk_shift) { A.osk_shift = 0; A.redraw |= R_ALL; }
}

static void draw_search(void) {
    gfx_fill(0, 0, g_w, g_h, g_col[C_BG]);
    bar_bg();
    button(6, (BAR_H - BTN) / 2, IC_BACK, A_CLOSE, 0, 1, g_col[C_BAR_TEXT]);
    const int fx = 6 + BTN + 8, fw = g_w - fx - (A.osk_on ? 24 : BTN + 24);
    reg_add(fx, 8, fw, 44, A_FIELD, 0);
    draw_field(fx, 8, fw);
    if (!A.osk_on) button(g_w - BTN - 12, (BAR_H - BTN) / 2, IC_KEYBOARD, A_FIELD, 1, 1, g_col[C_DIM]);
    const int bottom = A.osk_on ? g_h - OSK_H : g_h;
    int y = BAR_H + 6;
    const int rh = 48;
    const int total = A.nsugg + A.nfound;
    for (int i = 0; i < total && y + rh <= bottom; i++, y += rh) {
        const char *t = i < A.nsugg ? A.sugg[i].title : A.found[i - A.nsugg];
        const int r = reg_add(16, y, g_w - 32, rh - 4, i < A.nsugg ? A_SUGG : A_FOUND, i);
        uint16_t bg = g_col[C_BG];
        if (r == A.pressed) bg = g_col[C_PRESS];
        else if (i == A.sel) bg = g_col[C_CARD];
        gfx_rrect(16, y, g_w - 32, rh - 4, 10, bg);
        if (i == A.sel && kbd_present()) gfx_ring(16, y, g_w - 32, rh - 4, 10, 2, g_col[C_FOCUS]);
        icon_draw(i < A.nsugg ? IC_SEARCH : IC_TOC, 32, y + 10, g_col[C_DIM]);
        text_draw_fit(i == 0 && i < A.nsugg ? F_BOLD : F_SANS, 19, 72, y + 30, t, (int)strlen(t), g_w - 120, g_col[C_TEXT]);
    }
    if (A.q[0] && !A.nsugg && y + rh <= bottom) {
        const char *s = tr(S_NO_RESULTS);
        text_draw(F_SANS, 17, 32, y + 28, s, (int)strlen(s), g_col[C_DIM]);
        y += rh;
    }
    if (A.q[0] && !A.scanned && y + rh <= bottom && nvzim_get_info(A.z)->entry_count <= 600000) {
        char lbl[300];
        snprintf(lbl, sizeof lbl, tr(S_SEARCH_TITLES), A.q);
        row_box(16, y, g_w - 32, rh - 4, A_SCAN, 0, g_col[C_CARD]);
        icon_draw(IC_TOC, 32, y + 10, g_col[C_ACCENT]);
        text_draw_fit(F_SANS, 18, 72, y + 29, lbl, (int)strlen(lbl), g_w - 120, g_col[C_ACCENT]);
    }
    if (A.osk_on) draw_osk();
}

static int toc_row_of(int idx) {   // a toc index -> its row in the list (0 = top of the page)
    int row = 0;
    for (int i = 0; i < A.doc.ntoc && i <= idx; i++)
        if (A.doc.toc[i].level >= 2) row++;
    return idx >= 0 && A.doc.toc[idx].level >= 2 ? row : 0;
}

static int toc_index_of_row(int row) {   // -1 = top of the page
    int r = 0;
    for (int i = 0; i < A.doc.ntoc; i++)
        if (A.doc.toc[i].level >= 2 && ++r == row) return i;
    return -1;
}

static int toc_current(void) {
    int cur = -1;
    for (int i = 0; i < A.doc.ntoc; i++)
        if (A.doc.toc[i].y <= A.scroll + 24) cur = i;
    return cur;
}

static void draw_list_panel(int x, int w, const char *title) {
    gfx_fill(x, 0, w, g_h, g_col[C_BG]);
    gfx_fill(x, 0, w, BAR_H, g_col[C_BAR]);
    gfx_fill(x, BAR_H - 1, w, 1, g_col[C_RULE]);
    button(x + 6, (BAR_H - BTN) / 2, IC_CLOSE, A_CLOSE, 0, 1, g_col[C_BAR_TEXT]);
    draw_bar_title(title, x + 6 + BTN + 8, w - BTN - 40);
}

static void draw_toc(void) {
    draw_reader_content();
    scrim();
    const int w = g_w * 45 / 100 < 420 ? 420 : g_w * 45 / 100;
    draw_list_panel(0, w, tr(S_SECTIONS));
    gfx_fill(w, 0, 1, g_h, g_col[C_RULE]);
    const int rh = 46;
    gfx_clip(0, BAR_H, w, g_h);
    const int y0 = BAR_H + 6 - A.list_scroll;
    const int cur = toc_current();
    int row = 0;
    for (int i = -1; i < A.doc.ntoc; i++) {
        const int lvl = i < 0 ? 1 : A.doc.toc[i].level;
        if (i >= 0 && lvl < 2) continue;
        const int y = y0 + row * rh;
        const int this_row = row++;
        if (y + rh < BAR_H || y > g_h) continue;
        const int r = reg_add(8, y, w - 16, rh - 4, A_TOC_ITEM, i);
        uint16_t bg = g_col[C_BG];
        if (r == A.pressed) bg = g_col[C_PRESS];
        else if (i == cur || (i < 0 && toc_row_of(cur) == 0)) bg = g_col[C_CARD];
        gfx_rrect(8, y, w - 16, rh - 4, 10, bg);
        if (this_row == A.list_sel && kbd_present()) gfx_ring(8, y, w - 16, rh - 4, 10, 2, g_col[C_FOCUS]);
        const int ind = 20 + (lvl > 2 ? (lvl - 2) * 22 : 0);
        if (i < 0) {
            const char *s = tr(S_TOP);
            text_draw_fit(F_ITALIC, 18, ind, y + 28, s, (int)strlen(s), w - ind - 24, g_col[C_DIM]);
        } else {
            const toc_t *t = &A.doc.toc[i];
            text_draw_fit(lvl == 2 ? F_BOLD : F_SANS, lvl == 2 ? 18 : 17, ind, y + 28, A.doc.text + t->off, t->len,
                          w - ind - 24, i == cur ? g_col[C_ACCENT] : g_col[C_TEXT]);
        }
    }
    gfx_noclip();
}

static void draw_menu(void) {
    draw_reader_content();
    scrim();
    draw_reader_bar();
    const int w = 340, x = g_w - w - 12, rh = 54;
    int y = BAR_H + 4;
    const int h = rh * 6 + 16;
    gfx_rrect(x, y, w, h, 16, g_col[C_CARD]);
    gfx_ring(x, y, w, h, 16, 1, g_col[C_RULE]);
    y += 8;
    struct { int ic, s, act; } items[] = {
        {IC_RANDOM, S_RANDOM, A_RANDOM}, {IC_THEME, g_set.dark ? S_THEME_LIGHT : S_THEME_DARK, A_THEME},
        {IC_STAR, S_BOOKMARKS, A_MARKS}, {IC_HISTORY, S_HISTORY, A_MARKS}, {IC_INFO, S_INFO, A_INFO},
    };
    for (int i = 0; i < 5; i++, y += rh) {
        row_box(x + 8, y, w - 16, rh - 4, items[i].act, i == 2 ? MARK_BOOKMARKS : i == 3 ? MARK_HISTORY : A.coll,
                g_col[C_CARD]);
        icon_draw(items[i].ic, x + 22, y + 13, g_col[C_TEXT]);
        const char *s = tr(items[i].s);
        text_draw_fit(F_SANS, 18, x + 62, y + 32, s, (int)strlen(s), w - 90, g_col[C_TEXT]);
    }
    // text size: smaller / larger
    icon_draw(IC_TEXTSIZE, x + 22, y + 13, g_col[C_TEXT]);
    const char *s = tr(S_TEXT_SIZE);
    text_draw(F_SANS, 18, x + 62, y + 32, s, (int)strlen(s), g_col[C_TEXT]);
    const int bx = x + w - 2 * 60 - 12;
    row_box(bx, y + 2, 56, rh - 8, A_SIZE_DEC, 0, g_col[C_BG]);
    text_draw(F_BOLD, 16, bx + 21, y + 32, "A", 1, g_set.text_size > 0 ? g_col[C_TEXT] : g_col[C_RULE]);
    row_box(bx + 62, y + 2, 56, rh - 8, A_SIZE_INC, 0, g_col[C_BG]);
    text_draw(F_BOLD, 24, bx + 62 + 19, y + 35, "A", 1, g_set.text_size < 4 ? g_col[C_TEXT] : g_col[C_RULE]);
}

static void draw_marks(void) {
    gfx_fill(0, 0, g_w, g_h, g_col[C_BG]);
    const int rh = 72;
    gfx_clip(0, BAR_H, g_w, g_h);
    int y = BAR_H + 8 - A.list_scroll;
    if (!A.nmarks) {
        const char *s = tr(A.marks_which == MARK_BOOKMARKS ? S_NO_BOOKMARKS : S_NO_HISTORY);
        text_draw(F_SANS, 18, (g_w - text_width(F_SANS, 18, s, (int)strlen(s))) / 2, g_h / 2, s, (int)strlen(s), g_col[C_DIM]);
    }
    for (int i = 0; i < A.nmarks; i++, y += rh) {
        if (y + rh < BAR_H || y > g_h) continue;
        const mark_t *m = &A.marks[i];
        const int bm = A.marks_which == MARK_BOOKMARKS;
        const int r = reg_add(16, y, g_w - 32 - (bm ? BTN + 8 : 0), rh - 6, A_MARK_OPEN, i);
        uint16_t bg = g_col[C_CARD];
        if (r == A.pressed) bg = g_col[C_PRESS];
        gfx_rrect(16, y, g_w - 32, rh - 6, 12, bg);
        if (i == A.list_sel && kbd_present()) gfx_ring(16, y, g_w - 32, rh - 6, 12, 2, g_col[C_FOCUS]);
        text_draw_fit(F_BOLD, 18, 36, y + 28, m->title, (int)strlen(m->title), g_w - 140, g_col[C_TEXT]);
        const int ci = lib_find(m->file);
        char sub[300];
        snprintf(sub, sizeof sub, "%s", ci >= 0 ? (g_lib[ci].title[0] ? g_lib[ci].title : g_lib[ci].file) : m->file);
        text_draw_fit(F_SANS, 15, 36, y + 52, sub, (int)strlen(sub), g_w - 140, g_col[C_DIM]);
        if (bm) button(g_w - 16 - BTN - 4, y + (rh - 6 - BTN) / 2, IC_CLOSE, A_MARK_DEL, i, 1, g_col[C_DIM]);
    }
    gfx_noclip();
    bar_bg();
    button(6, (BAR_H - BTN) / 2, IC_BACK, A_CLOSE, 0, 1, g_col[C_BAR_TEXT]);
    draw_bar_title(tr(A.marks_which == MARK_BOOKMARKS ? S_BOOKMARKS : S_HISTORY), 6 + BTN + 10, g_w - 200);
}

static void draw_info(void) {
    gfx_fill(0, 0, g_w, g_h, g_col[C_BG]);
    bar_bg();
    button(6, (BAR_H - BTN) / 2, IC_BACK, A_CLOSE, 0, 1, g_col[C_BAR_TEXT]);
    draw_bar_title(tr(S_INFO), 6 + BTN + 10, g_w - 200);
    const coll_t *c = &g_lib[A.info_coll];
    int y = BAR_H + 24;
    draw_cover(c, 32, y);
    const char *title = c->title[0] ? c->title : c->file;
    text_draw_fit(F_BOLD, 24, 116, y + 28, title, (int)strlen(title), g_w - 150, g_col[C_TEXT]);
    if (c->desc[0]) text_draw_fit(F_SANS, 17, 116, y + 56, c->desc, (int)strlen(c->desc), g_w - 150, g_col[C_DIM]);
    y += 96;
    // the metadata of the file itself (read now: the collection may not be the open one)
    nvzim_t *z = NULL;
    int own = 0;
    if (A.z && A.coll == A.info_coll) z = A.z;
    else {
        nvzim_config_t cfg;
        memset(&cfg, 0, sizeof cfg);
        cfg.clusters = 1;
        cfg.cache_pages = 8;
        if (nvzim_open(&z, c->path, &cfg) == NVZIM_OK) own = 1;
        else z = NULL;
    }
    struct { int label; const char *meta; } rows[] = {
        {S_F_LANG, "Language"}, {S_F_DATE, "Date"}, {S_F_CREATOR, "Creator"}, {S_F_PUBLISHER, "Publisher"},
        {S_F_SOURCE, "Source"}, {S_F_LICENSE, "License"}, {S_F_NAME, "Name"},
    };
    const int lx = 32, vx = 260;
    char v[600];
    for (size_t i = 0; i < sizeof rows / sizeof rows[0]; i++) {
        v[0] = 0;
        if (z) lib_meta(z, rows[i].meta, v, sizeof v);
        if (!v[0]) continue;
        if (rows[i].label == S_F_LANG) { char l[16]; snprintf(l, sizeof l, "%s", v); l[strcspn(l, ",")] = 0; snprintf(v, sizeof v, "%s", lang_name(l)); }
        if (rows[i].label == S_F_DATE) { char d[64]; fmt_date(v, d, sizeof d); snprintf(v, sizeof v, "%s", d); }
        const char *lb = tr(rows[i].label);
        text_draw(F_SANS, 17, lx, y, lb, (int)strlen(lb), g_col[C_DIM]);
        text_draw_fit(F_SANS, 17, vx, y, v, (int)strlen(v), g_w - vx - 32, g_col[C_TEXT]);
        y += 32;
    }
    char sz[32], n[32];
    fmt_size(c->size, sz, sizeof sz);
    fmt_count(c->articles, n, sizeof n);
    const char *lb;
    lb = tr(S_F_ENTRIES);
    text_draw(F_SANS, 17, lx, y, lb, (int)strlen(lb), g_col[C_DIM]);
    text_draw(F_SANS, 17, vx, y, n, (int)strlen(n), g_col[C_TEXT]);
    y += 32;
    lb = tr(S_F_FILE);
    text_draw(F_SANS, 17, lx, y, lb, (int)strlen(lb), g_col[C_DIM]);
    snprintf(v, sizeof v, "%s (%s)", c->file, sz);
    text_draw_fit(F_SANS, 17, vx, y, v, (int)strlen(v), g_w - vx - 32, g_col[C_TEXT]);
    y += 32;
    if (z) {
        const nvzim_info_t *in = nvzim_get_info(z);
        lb = tr(S_F_FORMAT);
        text_draw(F_SANS, 17, lx, y, lb, (int)strlen(lb), g_col[C_DIM]);
        snprintf(v, sizeof v, "ZIM %u.%u%s", in->major, in->minor, in->parts > 1 ? " (split)" : "");
        text_draw(F_SANS, 17, vx, y, v, (int)strlen(v), g_col[C_TEXT]);
        y += 32;
    }
    if (own) nvzim_close(z);
    y += 16;
    int x = lx;
    const char *b1 = tr(S_MAIN_PAGE), *b2 = tr(S_VERIFY);
    int w1 = text_width(F_BOLD, 18, b1, (int)strlen(b1)) + 48, w2 = text_width(F_BOLD, 18, b2, (int)strlen(b2)) + 48;
    const int r1 = reg_add(x, y, w1, 50, A_MAIN_PAGE, A.info_coll);
    gfx_rrect(x, y, w1, 50, 25, r1 == A.pressed ? g_col[C_PRESS] : g_col[C_ACCENT]);
    if (r1 == A.focus) gfx_ring(x - 3, y - 3, w1 + 6, 56, 28, 2, g_col[C_FOCUS]);
    text_draw(F_BOLD, 18, x + 24, y + 32, b1, (int)strlen(b1), g_col[C_ON_ACCENT]);
    x += w1 + 16;
    const int r2 = row_box(x, y, w2, 50, A_VERIFY, A.info_coll, g_col[C_CARD]);
    (void)r2;
    text_draw(F_BOLD, 18, x + 24, y + 32, b2, (int)strlen(b2), g_col[C_ACCENT]);
    x += w2 + 16;
    const char *b3 = tr(A.del_armed == A.info_coll ? S_DELETE_SURE : S_DELETE);
    const int w3 = text_width(F_BOLD, 18, b3, (int)strlen(b3)) + 48;
    row_box(x, y, w3, 50, A_DELETE, A.info_coll, A.del_armed == A.info_coll ? g_col[C_PRESS] : g_col[C_CARD]);
    text_draw(F_BOLD, 18, x + 24, y + 32, b3, (int)strlen(b3), g_col[C_TEXT]);
    if (A.info_msg[0]) text_draw_fit(F_SANS, 17, lx, y + 84, A.info_msg, (int)strlen(A.info_msg), g_w - 64, g_col[C_TEXT]);
}

// A picture of the page on the whole screen: fitted to it, at most three times its size in the page
// (formulas are vectors: they stay sharp), its description under it. A tap, Back or any key closes it.
static void viewer_size(const dimg_t *im, int *w, int *h) {
    const int maxw = g_w - 48, maxh = g_h - 120;
    float sc = (float)maxw / im->w;
    if ((float)maxh / im->h < sc) sc = (float)maxh / im->h;
    if (sc > 3.0f) sc = 3.0f;
    *w = (int)(im->w * sc);
    *h = (int)(im->h * sc);
    if (*w < 1) *w = 1;
    if (*h < 1) *h = 1;
}

static void draw_viewer(void) {
    gfx_fill(0, 0, g_w, g_h, rgb565(0x0C, 0x0D, 0x10));
    if (A.view_img < 0 || A.view_img >= A.doc.nimgs) return;
    const dimg_t *im = &A.doc.imgs[A.view_img];
    int w, h;
    viewer_size(im, &w, &h);
    const int x = (g_w - w) / 2, y = 24 + (g_h - 120 - h) / 2;
    char path[NVZIM_PATH_MAX * 2];
    int st = -1;
    if (image_path(&A.doc, A.view_img, path, sizeof path) == 0) {
        st = img_lookup(path, w, h, im->mask);
        if (st == 1) {
            if (im->mask) gfx_rrect(x - 16, y - 12, w + 32, h + 24, 12, rgb565(0xFA, 0xFA, 0xFA));   // a formula on paper
            img_draw(path, w, h, x, y, rgb565(0x10, 0x12, 0x16));
        }
    }
    if (st == 0) {   // still decoding
        const char *s = tr(S_LOADING);
        text_draw(F_SANS, 18, (g_w - text_width(F_SANS, 18, s, (int)strlen(s))) / 2, g_h / 2, s, (int)strlen(s),
                  rgb565(0xB0, 0xB4, 0xBB));
    }
    if (im->alt_len && !im->mask)   // a picture's description (a formula's alt is its TeX: not for reading)
        text_draw_fit(F_SANS, 17, 48, g_h - 40, A.doc.text + im->alt_off, im->alt_len, g_w - 96, rgb565(0xC8, 0xCC, 0xD2));
    button(g_w - BTN - 12, 8, IC_CLOSE, A_CLOSE, 0, 1, rgb565(0xE6, 0xE8, 0xEB));
}


// ------------------------------------------------------------------------- the download screen
// The Kiwix catalog (get.c) as collections with their editions, and the downloads of the system
// downloader: they go on when the app is closed, and a finished one joins the library.

#define GET_ROW 92
static const char *const kLang3[] = {"ita", "eng", "spa", "fra", "deu"};

static void cat_label(const char *raw, char *out, size_t cap) {   // "stack_exchange" -> "Stack exchange"
    if (!raw[0] || !strcmp(raw, "other")) { snprintf(out, cap, "%s", tr(S_GET_OTHER)); return; }
    snprintf(out, cap, "%s", raw);
    for (char *p = out; *p; p++) if (*p == '_') *p = ' ';
    if (out[0] >= 'a' && out[0] <= 'z') out[0] = (char)(out[0] - 32);
}

// The categories of the catalog, the ones with most collections first (the chips after "All").
static void get_cats(void) {
    int cnt[GET_CATS] = {0};
    A.ncats = 0;
    for (int g = 0; g < g_ngrp; g++) {
        const char *c = g_cat[g_grp[g].pick].category;
        int k = 0;
        while (k < A.ncats && strcmp(A.cats[k], c)) k++;
        if (k == A.ncats) {
            if (A.ncats == GET_CATS) continue;
            snprintf(A.cats[A.ncats++], sizeof A.cats[0], "%s", c);
        }
        cnt[k]++;
    }
    for (int i = 0; i < A.ncats; i++)   // most first; "other" and "" last
        for (int j = i + 1; j < A.ncats; j++) {
            const int oi = !A.cats[i][0] || !strcmp(A.cats[i], "other"), oj = !A.cats[j][0] || !strcmp(A.cats[j], "other");
            if (oj < oi || (oj == oi && cnt[j] > cnt[i])) {
                char t[24];
                memcpy(t, A.cats[i], sizeof t); memcpy(A.cats[i], A.cats[j], sizeof t); memcpy(A.cats[j], t, sizeof t);
                const int c = cnt[i]; cnt[i] = cnt[j]; cnt[j] = c;
            }
        }
}

static int get_matches(int g) {
    const cat_t *c = &g_cat[g_grp[g].pick];
    if (A.get_search && A.q[0]) {   // a search looks through everything
        char q[200], t[400];
        lower_utf8(A.q, q, sizeof q);
        snprintf(t, sizeof t, "%s %s %s", c->title, c->name, c->summary);
        lower_utf8(t, t, sizeof t);
        return strstr(t, q) != NULL;
    }
    if (A.get_chip == 0) return g_grp[g].rec >= 0;
    if (A.get_chip == 1) return 1;
    return A.get_chip - 2 < A.ncats && !strcmp(c->category, A.cats[A.get_chip - 2]);
}

static int get_visible(void) {   // the rows shown, in A.vis
    if (A.vis_gen != get_gen()) {    // a new catalog (arrived, refreshed, another language)
        free(A.vis);
        A.vis = (int *)malloc(sizeof(int) * (size_t)(g_ngrp > 0 ? g_ngrp : 1));
        A.vis_gen = get_gen();
        get_cats();
        if (A.get_chip >= 2 + A.ncats) A.get_chip = 0;
        int rec = 0;   // a language without a Recommended list starts from All
        for (int g = 0; g < g_ngrp && !rec; g++) rec = g_grp[g].rec >= 0;
        if (!rec && A.get_chip == 0 && g_ngrp) A.get_chip = 1;
    }
    A.nvis = 0;
    for (int g = 0; g < g_ngrp && A.vis; g++)
        if (get_matches(g)) A.vis[A.nvis++] = g;
    return A.nvis;
}

static void get_enter(void) {
    A.screen = SCR_GET;
    A.overlay = OV_NONE;
    A.get_scroll = 0;
    A.get_search = 0;
    A.q[0] = 0;
    A.focus = -1;
    if (get_state() == GET_IDLE || strcmp(get_lang3(), A.get_lang)) get_open(A.get_lang, 0);
    A.dl_next = 0;
    A.redraw |= R_ALL;
}

static void draw_rgba48(const uint8_t *rgba, int x, int y) {
    for (int j = 0; j < 48; j++) {
        const int sy = y + j;
        if (sy < 0 || sy >= g_h) continue;
        for (int i = 0; i < 48; i++) {
            const uint8_t *p = rgba + (j * 48 + i) * 4;
            const int sx = x + i;
            if (!p[3] || sx < 0 || sx >= g_w) continue;
            uint16_t *d = g_rows[sy] + sx;
            *d = blend565(*d, rgb565(p[0], p[1], p[2]), p[3]);
        }
    }
    gfx_dirty(y, y + 48);
}

static void draw_get_thumb(int ent, int x, int y) {
    gfx_rrect(x, y, 64, 64, 12, g_set.dark ? rgb565(0x2C, 0x30, 0x37) : rgb565(0xFF, 0xFF, 0xFF));
    const uint8_t *ic = get_icon(ent);
    if (ic) draw_rgba48(ic, x + 8, y + 8);
    else icon_draw(IC_BOOK, x + 8, y + 8, g_col[C_RULE]);
}

// A rounded text button; primary = filled with the accent. Returns its width.
static int pill(int x, int y, const char *lbl, int act, int arg, int primary) {
    const int w = text_width(F_BOLD, 17, lbl, (int)strlen(lbl)) + 40, h = 44;
    const int r = reg_add(x, y, w, h, act, arg), st = reg_state(r);
    gfx_rrect(x, y, w, h, 22, st == 2 ? g_col[C_PRESS] : primary ? g_col[C_ACCENT] : g_col[C_BG]);
    if (!primary) gfx_ring(x, y, w, h, 22, 1, g_col[C_RULE]);
    if (st == 1) gfx_ring(x - 3, y - 3, w + 6, h + 6, 25, 2, g_col[C_FOCUS]);
    text_draw(F_BOLD, 17, x + 20, y + 28, lbl, (int)strlen(lbl), primary && st != 2 ? g_col[C_ON_ACCENT] : g_col[C_ACCENT]);
    return w;
}

static const char *flavour_label(const cat_t *c) {
    if (!strcmp(c->flavour, "nopic")) return tr(S_GET_NOPIC);
    if (!strcmp(c->flavour, "mini")) return tr(S_GET_MINI);
    if (!strcmp(c->flavour, "maxi")) return tr(S_GET_MAXI);
    return "";
}

static void fmt_eta(int64_t secs, char *out, size_t cap) {
    if (secs >= 3600) snprintf(out, cap, "%d h %02d min", (int)(secs / 3600), (int)(secs / 60 % 60));
    else if (secs >= 60) snprintf(out, cap, "%d min", (int)((secs + 59) / 60));
    else snprintf(out, cap, "%d s", (int)(secs > 0 ? secs : 1));
}

// "1,2 GB di 3,4 GB" and, while it runs, "180 KB/s, mancano 2 h 05 min".
static void dl_text(const dl_t *d, char *a, size_t an, char *b, size_t bn) {
    char x[24], y[24], v[24], t[32];
    fmt_size((uint64_t)d->done, x, sizeof x);
    fmt_size((uint64_t)d->size, y, sizeof y);
    snprintf(a, an, tr(S_GET_PROGRESS), x, d->size ? y : "?");
    b[0] = 0;
    if (d->state == DL_RUNNING && d->bps > 0 && d->size > d->done) {
        fmt_speed((uint64_t)d->bps, v, sizeof v);
        fmt_eta((d->size - d->done) / d->bps, t, sizeof t);
        snprintf(b, bn, tr(S_GET_SPEED), v, t);
    }
}

static const char *dl_state_text(const dl_t *d) {
    switch (d->state) {
    case DL_QUEUED: return tr(S_GET_QUEUED);
    case DL_PAUSED: return tr(S_GET_PAUSED);
    case DL_DONE: return tr(S_GET_DONE);
    case DL_FAILED: return tr(S_GET_FAILED);
    default: return !strcmp(d->phase, "meta4") ? tr(S_GET_PREPARING) : !strcmp(d->phase, "check") ? tr(S_GET_CHECKING) : "";
    }
}

static void progress_bar(int x, int y, int w, const dl_t *d) {
    gfx_rrect(x, y, w, 8, 4, g_col[C_RULE]);
    const int f = d->size > 0 ? (int)((int64_t)w * d->done / d->size) : 0;
    if (f > 8) gfx_rrect(x, y, f, 8, 4, d->state == DL_FAILED ? g_col[C_DIM] : g_col[C_ACCENT]);
}

// The download of any edition of a collection (the most recent one asked for), or -1.
static int group_dl(const cat_group_t *gr) {
    int best = -1;
    for (int f = gr->first; f < gr->first + gr->n; f++) {
        const int d = get_dl_of(&g_cat[f]);
        if (d >= 0 && (best < 0 || g_dl[d].id > g_dl[best].id)) best = d;
    }
    return best;
}

// The right side of a row or of an edition: the state, or the button that starts it.
static void get_action(int x_right, int y, int ent, int d) {
    const cat_t *c = &g_cat[ent];
    if (d >= 0 && g_dl[d].state != DL_DONE && g_dl[d].state != DL_FAILED) {
        const int w = 180, x = x_right - w;
        char pct[16];
        snprintf(pct, sizeof pct, "%d%%", g_dl[d].size ? (int)(g_dl[d].done * 100 / g_dl[d].size) : 0);
        const char *s = g_dl[d].state == DL_RUNNING && !dl_state_text(&g_dl[d])[0] ? pct : dl_state_text(&g_dl[d]);
        text_draw_fit(F_BOLD, 16, x, y + 18, s, (int)strlen(s), w, g_col[C_ACCENT]);
        progress_bar(x, y + 30, w, &g_dl[d]);
        return;
    }
    const int have = get_have(c);
    if (have == 1) {
        const char *s = tr(S_GET_HAVE);
        const int w = text_width(F_SANS, 15, s, (int)strlen(s));
        icon_draw(IC_CHECK, x_right - w - 30, y + 10, g_col[C_DIM]);
        text_draw(F_SANS, 15, x_right - w, y + 28, s, (int)strlen(s), g_col[C_DIM]);
        return;
    }
    const char *lbl = tr(have == 2 ? S_GET_UPDATE : S_GET_DOWNLOAD);
    const int w = text_width(F_BOLD, 17, lbl, (int)strlen(lbl)) + 40;
    pill(x_right - w, y, lbl, A_GET_START, ent, 1);
}

static void draw_get_row(int g, int y) {
    const cat_group_t *gr = &g_grp[g];
    const cat_t *c = &g_cat[gr->pick];
    const int x = 24, w = g_w - 48, h = GET_ROW - 8;
    row_box(x, y, w, h, A_GET_ROW, g, g_col[C_CARD]);
    draw_get_thumb(gr->pick, x + 12, y + 10);
    const int tx = x + 92, tw = w - 92 - 230;
    text_draw_fit(F_BOLD, 19, tx, y + 28, c->title, (int)strlen(c->title), tw, g_col[C_TEXT]);
    text_draw_fit(F_SANS, 15, tx, y + 51, c->summary, (int)strlen(c->summary), tw, g_col[C_DIM]);
    char meta[240], d[48], sz[32], n[32];
    fmt_date(c->date, d, sizeof d);
    fmt_size(c->size, sz, sizeof sz);
    fmt_count(c->articles, n, sizeof n);
    const char *fl = flavour_label(c);
    snprintf(meta, sizeof meta, "%s \xC2\xB7 %s %s \xC2\xB7 %s%s%s%s", d, n, tr(S_ARTICLES), sz, fl[0] ? " \xC2\xB7 " : "", fl,
             gr->n > 1 ? " \xE2\x80\xA6" : "");
    text_draw_fit(F_SANS, 14, tx, y + 73, meta, (int)strlen(meta), tw, g_col[C_DIM]);
    get_action(x + w - 16, y + (h - 44) / 2, gr->pick, group_dl(gr));
}

static int active_downloads(void) {
    int n = 0;
    for (int i = 0; i < g_ndl; i++) n += g_dl[i].state == DL_QUEUED || g_dl[i].state == DL_RUNNING;
    return n;
}

static void dl_button(int x, int y) {   // the downloads, with how many are under way
    button(x, y, IC_DOWNLOAD, A_DLS, 0, 1, g_col[C_BAR_TEXT]);
    const int n = active_downloads();
    if (!n) return;
    char s[8];
    snprintf(s, sizeof s, "%d", n);
    gfx_rrect(x + BTN - 22, y + 4, 20, 20, 10, g_col[C_ACCENT]);
    const int tw = text_width(F_BOLD, 13, s, (int)strlen(s));
    text_draw(F_BOLD, 13, x + BTN - 12 - tw / 2, y + 19, s, (int)strlen(s), g_col[C_ON_ACCENT]);
}

static int32_t get_height(void) { return 60 + A.nvis * GET_ROW + 24; }

static void draw_get(void) {
    gfx_fill(0, 0, g_w, g_h, g_col[C_BG]);
    const int bottom = A.osk_on ? g_h - OSK_H : g_h;
    get_visible();
    gfx_clip(0, BAR_H, g_w, bottom);
    int y = BAR_H + 8 - A.get_scroll;
    // the chips: Recommended, All, the categories (or the search field)
    if (A.get_search) {
        reg_add(24, y, g_w - 48, 44, A_FIELD, 0);
        draw_field(24, y, g_w - 48);
    } else {
        int x = 24;
        for (int k = 0; k < 2 + A.ncats && x < g_w - 60; k++) {
            char lbl[40];
            if (k == 0) snprintf(lbl, sizeof lbl, "%s", tr(S_GET_RECOMMENDED));
            else if (k == 1) snprintf(lbl, sizeof lbl, "%s", tr(S_GET_ALL));
            else cat_label(A.cats[k - 2], lbl, sizeof lbl);
            const int w = text_width(F_SANS, 16, lbl, (int)strlen(lbl)) + 32;
            if (x + w > g_w - 24) break;
            const int on = k == A.get_chip;
            const int r = reg_add(x, y + 2, w, 40, A_GET_CHIP, k), st = reg_state(r);
            gfx_rrect(x, y + 2, w, 40, 20, st == 2 ? g_col[C_PRESS] : on ? g_col[C_ACCENT] : g_col[C_CARD]);
            if (st == 1) gfx_ring(x - 3, y - 1, w + 6, 46, 23, 2, g_col[C_FOCUS]);
            text_draw(on ? F_BOLD : F_SANS, 16, x + 16, y + 28, lbl, (int)strlen(lbl), on && st != 2 ? g_col[C_ON_ACCENT] : g_col[C_TEXT]);
            x += w + 10;
        }
    }
    y += 60;
    const int st = get_state();
    if (st == GET_LOADING || (st == GET_IDLE)) {
        const char *s = tr(S_GET_LOADING);
        text_draw(F_SANS, 19, (g_w - text_width(F_SANS, 19, s, (int)strlen(s))) / 2, y + 120, s, (int)strlen(s), g_col[C_DIM]);
    } else if (st == GET_ERROR) {
        const char *s = tr(S_GET_OFFLINE);
        text_draw_fit(F_SANS, 19, 48, y + 120, s, (int)strlen(s), g_w - 96, g_col[C_TEXT]);
        pill(48, y + 150, tr(S_GET_RETRY), A_GET_RETRY, 0, 1);
    } else if (!A.nvis) {
        const char *s = tr(S_GET_NO_MATCH);
        text_draw(F_SANS, 18, (g_w - text_width(F_SANS, 18, s, (int)strlen(s))) / 2, y + 100, s, (int)strlen(s), g_col[C_DIM]);
    }
    for (int i = 0; i < A.nvis; i++, y += GET_ROW) {
        if (y + GET_ROW < BAR_H || y > bottom) continue;
        draw_get_row(A.vis[i], y);
    }
    gfx_noclip();
    if (A.osk_on) draw_osk();
    // the bar on top
    bar_bg();
    button(6, (BAR_H - BTN) / 2, IC_BACK, A_CLOSE, 0, 1, g_col[C_BAR_TEXT]);
    draw_bar_title(tr(S_GET_TITLE), 6 + BTN + 10, g_w - 6 - BTN - 10 - 3 * BTN - 120);
    char l[8];   // the catalog's language: a tap moves to the next one
    snprintf(l, sizeof l, "%.2s", get_lang3()[0] ? get_lang3() : A.get_lang);
    for (char *p = l; *p; p++) if (*p >= 'a' && *p <= 'z') *p = (char)(*p - 32);
    const int lx = g_w - 3 * BTN - 24 - 64;
    const int r = reg_add(lx, (BAR_H - 40) / 2, 60, 40, A_GET_LANG, 0), rs = reg_state(r);
    gfx_rrect(lx, (BAR_H - 40) / 2, 60, 40, 20, rs == 2 ? g_col[C_PRESS] : g_col[C_CARD]);
    if (rs == 1) gfx_ring(lx - 3, (BAR_H - 40) / 2 - 3, 66, 46, 23, 2, g_col[C_FOCUS]);
    text_draw(F_BOLD, 16, lx + (60 - text_width(F_BOLD, 16, l, (int)strlen(l))) / 2, BAR_H / 2 + 6, l, (int)strlen(l), g_col[C_TEXT]);
    button(g_w - 3 * BTN - 16, (BAR_H - BTN) / 2, A.get_search ? IC_CLOSE : IC_SEARCH, A_GET_SEARCH, 0, 1, g_col[C_BAR_TEXT]);
    if (A.get_search && !A.osk_on) button(g_w - 2 * BTN - 12, (BAR_H - BTN) / 2, IC_KEYBOARD, A_FIELD, 1, 1, g_col[C_DIM]);
    dl_button(g_w - BTN - 8, (BAR_H - BTN) / 2);
    if (get_busy() && st == GET_READY) gfx_fill(0, BAR_H - 3, (A.now / 4) % g_w, 2, g_col[C_ACCENT]);   // refreshing
}

// Words of `s` on up to `max` lines of width w; returns the y after them.
static int draw_wrapped(int f, int px, int x, int y, int w, const char *s, int max, uint16_t col) {
    int n = (int)strlen(s), lines = 0;
    while (n > 0 && lines < max) {
        int k = text_fit(f, px, s, n, w);
        if (k < n) { int b = k; while (b > 0 && s[b] != ' ') b--; if (b > 0) k = b; }
        if (lines == max - 1 && k < n) { text_draw_fit(f, px, x, y, s, n, w, col); y += px + 10; return y; }
        text_draw(f, px, x, y, s, k, col);
        y += px + 10;
        s += k;
        n -= k;
        while (n > 0 && *s == ' ') { s++; n--; }
        lines++;
    }
    return y;
}

static void draw_get_info(void) {
    gfx_fill(0, 0, g_w, g_h, g_col[C_BG]);
    if (A.get_group < 0 || A.get_group >= g_ngrp) return;
    const cat_group_t *gr = &g_grp[A.get_group];
    const cat_t *c = &g_cat[gr->pick];
    gfx_clip(0, BAR_H, g_w, g_h);
    int y = BAR_H + 20 - A.list_scroll;
    draw_get_thumb(gr->pick, 32, y);
    text_draw_fit(F_BOLD, 24, 116, y + 30, c->title, (int)strlen(c->title), g_w - 150, g_col[C_TEXT]);
    char line[300], d[48], n[32];
    fmt_date(c->date, d, sizeof d);
    fmt_count(c->articles, n, sizeof n);
    snprintf(line, sizeof line, "%s \xC2\xB7 %s \xC2\xB7 %s %s%s%s", lang_name(c->lang), d, n, tr(S_ARTICLES),
             c->author[0] ? " \xC2\xB7 " : "", c->author);
    text_draw_fit(F_SANS, 16, 116, y + 58, line, (int)strlen(line), g_w - 150, g_col[C_DIM]);
    y += 96;
    y = draw_wrapped(F_SANS, 18, 32, y + 4, g_w - 64, c->summary, 3, g_col[C_TEXT]);
    y += 12;
    const char *h = tr(S_GET_EDITIONS);
    text_draw(F_BOLD, 18, 32, y + 4, h, (int)strlen(h), g_col[C_HEAD]);
    y += 20;
    const int64_t room = get_room();
    for (int f = gr->first; f < gr->first + gr->n; f++, y += 76) {
        const cat_t *e = &g_cat[f];
        gfx_rrect(24, y, g_w - 48, 68, 12, g_col[C_CARD]);
        char sz[32], lbl[120];
        fmt_size(e->size, sz, sizeof sz);
        const char *fl = flavour_label(e);
        snprintf(lbl, sizeof lbl, "%s%s%s", fl, fl[0] ? " \xC2\xB7 " : "", sz);
        text_draw_fit(F_BOLD, 18, 44, y + 30, lbl, (int)strlen(lbl), g_w - 360, g_col[C_TEXT]);
        fmt_date(e->date, d, sizeof d);
        fmt_count(e->articles, n, sizeof n);
        snprintf(lbl, sizeof lbl, "%s \xC2\xB7 %s %s", d, n, tr(S_ARTICLES));
        text_draw_fit(F_SANS, 15, 44, y + 54, lbl, (int)strlen(lbl), g_w - 360, g_col[C_DIM]);
        const int dl = get_dl_of(e);
        if (dl < 0 && get_have(e) != 1 && room >= 0 && (int64_t)e->size > room) {
            const char *s = tr(S_GET_NO_ROOM);
            text_draw_fit(F_SANS, 15, g_w - 330, y + 40, s, (int)strlen(s), 290, g_col[C_DIM]);
        } else {
            get_action(g_w - 40, y + 12, f, dl);
        }
    }
    if (room >= 0) {
        char r[32];
        fmt_size((uint64_t)room, r, sizeof r);
        snprintf(line, sizeof line, tr(S_GET_ROOM), r);
        text_draw(F_SANS, 16, 32, y + 24, line, (int)strlen(line), g_col[C_DIM]);
        y += 36;
    }
    draw_wrapped(F_SANS, 15, 32, y + 16, g_w - 64, tr(S_GET_NOTE), 2, g_col[C_DIM]);
    gfx_noclip();
    bar_bg();
    button(6, (BAR_H - BTN) / 2, IC_BACK, A_CLOSE, 0, 1, g_col[C_BAR_TEXT]);
    draw_bar_title(c->title, 6 + BTN + 10, g_w - 6 - BTN - 10 - BTN - 24);
    dl_button(g_w - BTN - 8, (BAR_H - BTN) / 2);
}

static void draw_dls(void) {
    gfx_fill(0, 0, g_w, g_h, g_col[C_BG]);
    gfx_clip(0, BAR_H, g_w, g_h);
    int y = BAR_H + 12 - A.list_scroll;
    y = draw_wrapped(F_SANS, 15, 32, y + 16, g_w - 64, tr(S_GET_NOTE), 2, g_col[C_DIM]) + 4;
    if (!g_ndl) {
        const char *s = tr(S_GET_NONE);
        text_draw(F_SANS, 18, (g_w - text_width(F_SANS, 18, s, (int)strlen(s))) / 2, g_h / 2, s, (int)strlen(s), g_col[C_DIM]);
    }
    for (int i = g_ndl - 1; i >= 0; i--, y += 104) {   // the newest first
        const dl_t *d = &g_dl[i];
        if (y + 104 < BAR_H || y > g_h) continue;
        const int done = d->state == DL_DONE;
        const int r = reg_add(24, y, g_w - 48 - 2 * BTN - 16, 96, done ? A_DL_OPEN : A_NONE, i);
        gfx_rrect(24, y, g_w - 48, 96, 12, r == A.pressed && done ? g_col[C_PRESS] : g_col[C_CARD]);
        text_draw_fit(F_BOLD, 18, 44, y + 30, d->title, (int)strlen(d->title), g_w - 280, g_col[C_TEXT]);
        char a[64], b[64], s[200];
        dl_text(d, a, sizeof a, b, sizeof b);
        const char *stt = dl_state_text(d);
        if (done) snprintf(s, sizeof s, "%s", stt);
        else snprintf(s, sizeof s, "%s%s%s%s%s", stt, stt[0] ? " \xC2\xB7 " : "", a, b[0] ? " \xC2\xB7 " : "", b);
        if (d->state == DL_FAILED && d->error[0]) snprintf(s, sizeof s, "%s: %s", stt, d->error);
        text_draw_fit(F_SANS, 15, 44, y + 56, s, (int)strlen(s), g_w - 280, d->state == DL_FAILED ? g_col[C_TEXT] : g_col[C_DIM]);
        if (!done) progress_bar(44, y + 72, g_w - 280 - 20, d);
        const int bx = g_w - 24 - 2 * BTN - 12, by = y + (96 - BTN) / 2;
        if (d->state == DL_RUNNING || d->state == DL_QUEUED) button(bx, by, IC_PAUSE, A_DL_PAUSE, i, 1, g_col[C_TEXT]);
        else if (d->state == DL_PAUSED || d->state == DL_FAILED) button(bx, by, IC_PLAY, A_DL_RESUME, i, 1, g_col[C_ACCENT]);
        button(bx + BTN + 4, by, IC_CLOSE, A_DL_CANCEL, i, 1, A.dl_armed == d->id ? g_col[C_ACCENT] : g_col[C_DIM]);
        if (A.dl_armed == d->id) {
            const char *sure = tr(S_GET_CANCEL_SURE);
            const int tw = text_width(F_SANS, 14, sure, (int)strlen(sure));
            gfx_rrect(g_w - 24 - tw - 24, y + 96 - 30, tw + 16, 24, 12, g_col[C_ACCENT]);
            text_draw(F_SANS, 14, g_w - 24 - tw - 16, y + 96 - 13, sure, (int)strlen(sure), g_col[C_ON_ACCENT]);
        }
    }
    gfx_noclip();
    bar_bg();
    button(6, (BAR_H - BTN) / 2, IC_BACK, A_CLOSE, 0, 1, g_col[C_BAR_TEXT]);
    draw_bar_title(tr(S_GET_DOWNLOADS), 6 + BTN + 10, g_w - 200);
}

// Rescan the library (a download finished, a collection was deleted) keeping the open one.
static void lib_rescan(void) {
    char open[128] = "";
    if (A.coll >= 0 && A.coll < g_nlib) snprintf(open, sizeof open, "%s", g_lib[A.coll].file);
    lib_scan(NULL, NULL);
    if (open[0]) {
        A.coll = lib_find(open);
        if (A.coll < 0) { close_coll(); A.has_doc = 0; if (A.screen == SCR_READER) to_library(); }
    }
}

// Downloads: polled every second on these screens, every few seconds elsewhere; a finished one is
// read into the library, said in a toast, and taken off the downloader's list.
static void dl_poll(void) {
    if (A.now < A.dl_next) return;
    A.dl_next = A.now + (A.screen == SCR_GET ? 1000 : 4000);
    int fin = 0, fail = 0;
    const int changed = get_dl_refresh(&fin, &fail);
    if (changed && (A.screen != SCR_READER || A.overlay != OV_NONE)) A.redraw |= R_ALL;
    if (fin) {
        lib_rescan();
        for (int i = g_ndl - 1; i >= 0; i--)
            if (g_dl[i].state == DL_DONE) { toast(tr(S_GET_FINISHED), g_dl[i].title); break; }
        A.redraw |= R_ALL;
    }
}

static void delete_coll(int ci) {
    if (ci < 0 || ci >= g_nlib) return;
    char path[200], title[160];
    snprintf(path, sizeof path, "%s", g_lib[ci].path);
    snprintf(title, sizeof title, "%s", g_lib[ci].title[0] ? g_lib[ci].title : g_lib[ci].file);
    if (A.coll == ci) { close_coll(); A.has_doc = 0; }
    if (!strcmp(g_set.last_file, g_lib[ci].file)) { g_set.last_file[0] = g_set.last_path[0] = 0; settings_save(); }
    const size_t n = strlen(path);
    if (n > 6 && !strcmp(path + n - 6, ".zimaa")) {   // every part of a split file
        for (int i = 0; i < 26 * 26; i++) {
            path[n - 2] = (char)('a' + i / 26);
            path[n - 1] = (char)('a' + i % 26);
            if (remove(path) != 0) break;
        }
    } else {
        remove(path);
    }
    lib_rescan();
    A.screen = SCR_LIBRARY;
    A.overlay = OV_NONE;
    A.del_armed = -1;
    toast(tr(S_DELETED), title);
}

static void draw_all(void) {
    A.nregs = 0;
    if (A.overlay == OV_GET_INFO) draw_get_info();
    else if (A.overlay == OV_DLS) draw_dls();
    else if (A.screen == SCR_GET && A.overlay == OV_NONE) draw_get();
    else if (A.screen == SCR_LIBRARY && A.overlay == OV_NONE) draw_library();
    else if (A.overlay == OV_SEARCH) draw_search();
    else if (A.overlay == OV_MARKS) draw_marks();
    else if (A.overlay == OV_INFO) draw_info();
    else if (A.overlay == OV_TOC) draw_toc();
    else if (A.overlay == OV_MENU) draw_menu();
    else if (A.overlay == OV_VIEWER) draw_viewer();
    else {
        draw_reader_content();
        draw_reader_bar();
    }
    draw_toast();
    if (A.focus >= A.nregs) A.focus = -1;
}

// ------------------------------------------------------------------------------------ actions

static void open_marks(int which) {
    free(A.marks);
    A.marks = NULL;
    A.nmarks = marks_load(which, &A.marks);
    A.marks_which = which;
    A.overlay = OV_MARKS;
    A.list_scroll = 0;
    A.list_sel = 0;
    A.focus = -1;
    A.redraw |= R_ALL;
}

static int verify_progress(void *ctx, uint64_t done, uint64_t total) {
    (void)ctx;
    snprintf(A.info_msg, sizeof A.info_msg, tr(S_VERIFYING), (int)(total ? done * 100 / total : 0));
    A.nregs = 0;
    draw_info();
    if (!pump() || nv_gfx_back()) return 1;
    return 0;
}

static void do_action(int act, int arg) {
    if (A.overlay == OV_MENU && act != A_THEME && act != A_SIZE_DEC && act != A_SIZE_INC) A.overlay = OV_NONE;
    switch (act) {
    case A_BACK: case A_CLOSE: go_back(); break;
    case A_FORWARD: go_forward(); break;
    case A_LIBRARY: to_library(); break;
    case A_SEARCH: open_search(); break;
    case A_TOC:
        A.overlay = OV_TOC;
        A.list_sel = toc_row_of(toc_current());
        A.list_scroll = A.list_sel * 46 - (g_h - BAR_H) / 2;
        if (A.list_scroll < 0) A.list_scroll = 0;
        A.focus = -1;
        A.redraw |= R_ALL;
        break;
    case A_STAR: toggle_star(); A.redraw |= R_ALL; break;
    case A_MORE: A.overlay = OV_MENU; A.focus = -1; A.redraw |= R_ALL; break;
    case A_OPEN_COLL: open_main(arg); break;
    case A_INFO_COLL: case A_INFO:
        if (arg < 0 || arg >= g_nlib) break;
        A.info_coll = arg;
        A.info_msg[0] = 0;
        A.overlay = OV_INFO;
        A.focus = -1;
        A.redraw |= R_ALL;
        break;
    case A_CONTINUE: {
        const int ci = lib_find(g_set.last_file);
        if (ci >= 0) open_path(ci, g_set.last_path, NULL, 1, g_set.last_scroll);
        break;
    }
    case A_MARKS: open_marks(arg); break;
    case A_SUGG: case A_FOUND: search_choose(arg); break;
    case A_SCAN: search_scan(); break;
    case A_TOC_ITEM:
        A.overlay = OV_NONE;
        A.scroll = arg < 0 ? 0 : A.doc.toc[arg].y - 8;
        if (A.scroll > max_scroll()) A.scroll = max_scroll();
        A.redraw |= R_ALL;
        break;
    case A_RANDOM: A.overlay = OV_NONE; open_random(); break;
    case A_SIZE_DEC: set_text_size(g_set.text_size - 1); break;
    case A_SIZE_INC: set_text_size(g_set.text_size + 1); break;
    case A_THEME:
        g_set.dark = !g_set.dark;
        theme_set(g_set.dark);
        settings_save();
        A.redraw |= R_ALL;
        break;
    case A_MARK_OPEN:
        if (arg >= 0 && arg < A.nmarks) {
            const int ci = lib_find(A.marks[arg].file);
            if (ci < 0) { toast(tr(S_NOT_FOUND), NULL); break; }
            A.overlay = OV_NONE;
            open_path(ci, A.marks[arg].path, NULL, 1, 0);
        }
        break;
    case A_MARK_DEL:
        if (arg >= 0 && arg < A.nmarks) {
            marks_remove(A.marks_which, A.marks[arg].file, A.marks[arg].path);
            if (A.has_doc && A.coll >= 0 && !strcmp(A.marks[arg].file, g_lib[A.coll].file) &&
                !strcmp(A.marks[arg].path, A.path))
                A.starred = 0;
            open_marks(A.marks_which);
        }
        break;
    case A_VERIFY: {
        nvzim_t *z = NULL;
        nvzim_config_t cfg;
        memset(&cfg, 0, sizeof cfg);
        cfg.clusters = 1;
        if (nvzim_open(&z, g_lib[arg].path, &cfg) != NVZIM_OK) { snprintf(A.info_msg, sizeof A.info_msg, "%s", tr(S_DAMAGED)); A.redraw |= R_ALL; break; }
        if (!nvzim_get_info(z)->has_checksum) snprintf(A.info_msg, sizeof A.info_msg, "%s", tr(S_VERIFY_NONE));
        else {
            const int r = nvzim_verify(z, verify_progress, NULL);
            snprintf(A.info_msg, sizeof A.info_msg, "%s", r == NVZIM_OK ? tr(S_VERIFY_OK) : r == NVZIM_ECANCELED ? "" :
                     r == NVZIM_EMISMATCH ? tr(S_VERIFY_BAD) : nvzim_strerror(r));
        }
        nvzim_close(z);
        A.redraw |= R_ALL;
        break;
    }
    case A_MAIN_PAGE: A.overlay = OV_NONE; open_main(arg); break;
    case A_OSK: osk_press(arg); break;
    case A_GET: get_enter(); break;
    case A_GET_CHIP: A.get_chip = arg; A.get_scroll = 0; A.redraw |= R_ALL; break;
    case A_GET_LANG:
        for (int i = 0; i < 5; i++)
            if (!strcmp(A.get_lang, kLang3[i])) { snprintf(A.get_lang, sizeof A.get_lang, "%s", kLang3[(i + 1) % 5]); break; }
        get_open(A.get_lang, 0);
        A.get_chip = 0;
        A.get_scroll = 0;
        A.redraw |= R_ALL;
        break;
    case A_GET_RETRY: get_open(A.get_lang, 1); A.redraw |= R_ALL; break;
    case A_GET_SEARCH:
        A.get_search = !A.get_search;
        A.q[0] = 0;
        A.osk_on = A.get_search && !kbd_present();
        A.osk_layer = A.osk_shift = 0;
        A.get_scroll = 0;
        A.redraw |= R_ALL;
        break;
    case A_GET_ROW:
        if (arg < 0 || arg >= g_ngrp) break;
        A.get_group = arg;
        A.overlay = OV_GET_INFO;
        A.list_scroll = 0;
        A.osk_on = 0;
        A.focus = -1;
        A.redraw |= R_ALL;
        break;
    case A_GET_START: {
        if (arg < 0 || arg >= g_ncat) break;
        const int id = get_dl_start(&g_cat[arg]);
        if (id > 0) {
            toast(tr(S_GET_STARTED), g_cat[arg].title);
            A.dl_next = 0;
        } else {
            char why[96];
            snprintf(why, sizeof why, "%s", id == NV_DL_E_EXISTS ? tr(S_GET_EXISTS) : "");
            if (!why[0]) snprintf(why, sizeof why, "%d", id);
            toast(tr(S_GET_NOT_STARTED), why);
        }
        A.redraw |= R_ALL;
        break;
    }
    case A_DLS: A.overlay = OV_DLS; A.list_scroll = 0; A.osk_on = 0; A.dl_next = 0; A.dl_armed = 0; A.focus = -1; A.redraw |= R_ALL; break;
    case A_DL_PAUSE: case A_DL_RESUME:
        if (arg >= 0 && arg < g_ndl) nv_dl_pause(g_dl[arg].id, act == A_DL_PAUSE);
        A.dl_next = 0;
        A.redraw |= R_ALL;
        break;
    case A_DL_CANCEL:
        if (arg < 0 || arg >= g_ndl) break;
        if (g_dl[arg].state == DL_DONE || A.dl_armed == g_dl[arg].id) { nv_dl_cancel(g_dl[arg].id); A.dl_armed = 0; }
        else A.dl_armed = g_dl[arg].id;
        A.dl_next = 0;
        A.redraw |= R_ALL;
        break;
    case A_DL_OPEN: {
        if (arg < 0 || arg >= g_ndl) break;
        const char *f = strrchr(g_dl[arg].dest, '/');
        const int ci = f ? lib_find(f + 1) : -1;
        if (ci >= 0) { A.overlay = OV_NONE; A.screen = SCR_LIBRARY; open_main(ci); }
        break;
    }
    case A_DELETE:
        if (A.del_armed == arg) delete_coll(arg);
        else A.del_armed = arg;
        A.redraw |= R_ALL;
        break;
    case A_FIELD:
        if (arg == 1 || !kbd_present()) { A.osk_on = 1; A.redraw |= R_ALL; }
        break;
    default: break;
    }
}

// --------------------------------------------------------------------------------------- input

static void scroll_to(int32_t s) {
    const int32_t m = A.screen == SCR_READER ? max_scroll() : 0;
    if (s > m) s = m;
    if (s < 0) s = 0;
    A.scroll = s;
}

static int list_len(void) {
    if (A.overlay == OV_TOC) {
        int n = 1;
        for (int i = 0; i < A.doc.ntoc; i++) n += A.doc.toc[i].level >= 2;
        return n;
    }
    if (A.overlay == OV_MARKS) return A.nmarks;
    return 0;
}

static void list_keep_visible(int rh) {
    const int y = A.list_sel * rh;
    if (y < A.list_scroll) A.list_scroll = y;
    if (y + rh > A.list_scroll + g_h - BAR_H - 16) A.list_scroll = y + rh - (g_h - BAR_H - 16);
}

static void focus_step(int dir) {   // Tab / Shift+Tab over the controls (and the links on screen)
    int links[64];
    int nl = 0;
    if (A.screen == SCR_READER && A.overlay == OV_NONE)
        nl = doc_links_in(&A.doc, A.scroll, A.scroll + g_h - content_top(), links, 64);
    const int total = A.nregs + nl;
    if (!total) return;
    int cur = A.focus >= 0 ? A.focus : -1;
    if (A.focus_link >= 0) {
        for (int i = 0; i < nl; i++)
            if (links[i] == A.focus_link) cur = A.nregs + i;
    }
    cur = cur < 0 ? (dir > 0 ? 0 : total - 1) : (cur + dir + total) % total;
    if (cur < A.nregs) { A.focus = cur; A.focus_link = -1; }
    else { A.focus = -1; A.focus_link = links[cur - A.nregs]; }
    A.redraw |= R_ALL;
}

static void activate_focus(void) {
    if (A.focus_link >= 0) { follow(A.focus_link); return; }
    if (A.focus >= 0 && A.focus < A.nregs) do_action(A.regs[A.focus].act, A.regs[A.focus].arg);
}

static void on_key(const kev_t *k) {
    const int ctrl = k->mods & M_CTRL, alt = k->mods & M_ALT;
    if (A.overlay == OV_SEARCH) {
        switch (k->key) {
        case K_ESC: go_back(); return;
        case K_ENTER: search_choose(A.sel); return;
        case K_BACKSPACE: q_backspace(); return;
        case K_UP: if (A.sel > 0) A.sel--; A.redraw |= R_ALL; return;
        case K_DOWN: if (A.sel + 1 < A.nsugg + A.nfound) A.sel++; A.redraw |= R_ALL; return;
        case K_TAB: focus_step(k->mods & M_SHIFT ? -1 : 1); return;
        case K_SPACE: q_append(' '); return;
        case K_CHAR: if (!ctrl && !alt) { A.osk_on = 0; q_append(k->ch); } return;
        default: return;
        }
    }
    if (A.overlay == OV_VIEWER) { go_back(); return; }   // any key closes the picture
    if (A.screen == SCR_GET && A.overlay == OV_NONE && A.get_search) {   // typing into the catalog's search
        switch (k->key) {
        case K_ESC: go_back(); return;
        case K_ENTER: A.osk_on = 0; A.redraw |= R_ALL; return;
        case K_BACKSPACE: q_backspace(); return;
        case K_SPACE: q_append(' '); return;
        case K_CHAR: if (!ctrl && !alt) { A.osk_on = 0; q_append(k->ch); return; } break;
        default: break;
        }
    }
    if (k->key == K_ESC || (alt && k->key == K_LEFT)) { go_back(); return; }
    if (alt && k->key == K_RIGHT) { go_forward(); return; }
    if (k->key == K_TAB) { focus_step(k->mods & M_SHIFT ? -1 : 1); return; }
    if (A.overlay == OV_TOC || A.overlay == OV_MARKS) {
        const int n = list_len(), rh = A.overlay == OV_TOC ? 46 : 72;
        switch (k->key) {
        case K_UP: if (A.list_sel > 0) A.list_sel--; break;
        case K_DOWN: if (A.list_sel + 1 < n) A.list_sel++; break;
        case K_PGUP: A.list_sel -= 8; if (A.list_sel < 0) A.list_sel = 0; break;
        case K_PGDN: A.list_sel += 8; if (A.list_sel >= n) A.list_sel = n - 1; break;
        case K_HOME: A.list_sel = 0; break;
        case K_END: A.list_sel = n - 1; break;
        case K_ENTER: case K_SPACE:
            if (A.focus >= 0) { activate_focus(); return; }
            if (A.overlay == OV_TOC) do_action(A_TOC_ITEM, toc_index_of_row(A.list_sel));
            else {
                do_action(A_MARK_OPEN, A.list_sel);
            }
            return;
        case K_DELETE: if (A.overlay == OV_MARKS && A.marks_which == MARK_BOOKMARKS) do_action(A_MARK_DEL, A.list_sel); return;
        default: return;
        }
        A.focus = -1;
        list_keep_visible(rh);
        A.redraw |= R_ALL;
        return;
    }
    if (k->key == K_ENTER || (k->key == K_SPACE && (A.focus >= 0 || A.focus_link >= 0))) {
        if (A.focus >= 0 || A.focus_link >= 0) activate_focus();
        return;
    }
    if (A.overlay == OV_MENU || A.overlay == OV_INFO || A.overlay == OV_GET_INFO || A.overlay == OV_DLS || A.screen == SCR_GET) {
        if (k->key == K_PGDN || k->key == K_PGUP) {
            int32_t *s = A.screen == SCR_GET && A.overlay == OV_NONE ? &A.get_scroll : &A.list_scroll;
            *s += k->key == K_PGDN ? g_h / 2 : -g_h / 2;
            if (*s < 0) *s = 0;
            A.redraw |= R_ALL;
            return;
        }
        if (k->key == K_DOWN || k->key == K_RIGHT) focus_step(1);
        if (k->key == K_UP || k->key == K_LEFT) focus_step(-1);
        return;
    }
    if (A.screen == SCR_LIBRARY) {
        switch (k->key) {
        case K_DOWN: case K_RIGHT: focus_step(1); return;
        case K_UP: case K_LEFT: focus_step(-1); return;
        case K_PGDN: A.lib_scroll += g_h / 2; A.redraw |= R_ALL; return;
        case K_PGUP: A.lib_scroll -= g_h / 2; if (A.lib_scroll < 0) A.lib_scroll = 0; A.redraw |= R_ALL; return;
        default: return;
        }
    }
    // the reader
    const int page = g_h - content_top() - 48, line = text_px() * 3;
    switch (k->key) {
    case K_DOWN: scroll_to(A.scroll + line); return;
    case K_UP: scroll_to(A.scroll - line); return;
    case K_PGDN: scroll_to(A.scroll + page); return;
    case K_PGUP: scroll_to(A.scroll - page); return;
    case K_SPACE: scroll_to(A.scroll + (k->mods & M_SHIFT ? -page : page)); return;
    case K_HOME: scroll_to(0); return;
    case K_END: scroll_to(max_scroll()); return;
    case K_BACKSPACE: go_back(); return;
    case K_CHAR:
        if ((ctrl && (k->ch == 'f' || k->ch == 'k')) || k->ch == '/') { open_search(); return; }
        if (ctrl && k->ch == 'd') { toggle_star(); A.redraw |= R_ALL; return; }
        if (ctrl) return;
        switch (k->ch) {
        case 't': case 'T': do_action(A_TOC, 0); return;
        case 'r': case 'R': open_random(); return;
        case '+': case '=': set_text_size(g_set.text_size + 1); return;
        case '-': set_text_size(g_set.text_size - 1); return;
        case 'b': case 'B': toggle_star(); A.redraw |= R_ALL; return;
        case 'm': case 'M': do_action(A_MORE, 0); return;
        default: return;
        }
    default: return;
    }
}

static int scrollable_y(int y) { return y >= BAR_H && (A.overlay != OV_SEARCH || !A.osk_on || y < g_h - OSK_H); }

static void on_touch(int down, int x, int y) {
    if (down && !A.touch_down) {   // press
        A.touch_down = 1;
        A.tx0 = A.tx = x;
        A.ty0 = A.ty = y;
        A.t0 = A.now;
        A.moved = A.long_fired = 0;
        A.vel = 0;
        A.nvel = 0;
        A.drag_scroll0 = A.screen == SCR_READER && A.overlay == OV_NONE ? A.scroll
                         : A.overlay == OV_TOC || A.overlay == OV_MARKS || A.overlay == OV_GET_INFO || A.overlay == OV_DLS
                             ? A.list_scroll
                         : A.screen == SCR_GET ? A.get_scroll : A.lib_scroll;
        A.pressed = reg_at(x, y);
        if (A.pressed >= 0) A.redraw |= A.screen == SCR_READER && A.overlay == OV_NONE ? R_BAR : R_ALL;
        A.focus = -1;
        return;
    }
    if (down) {   // move
        A.tx = x;
        A.ty = y;
        if (!A.moved && (abs(y - A.ty0) > 12 || abs(x - A.tx0) > 12)) {
            A.moved = 1;
            if (A.pressed >= 0 && !((A.overlay == OV_SEARCH || A.screen == SCR_GET) && A.regs[A.pressed].act == A_OSK)) {
                A.pressed = -1;
                A.redraw |= R_ALL;
            }
        }
        if (A.moved && scrollable_y(A.ty0) && A.pressed < 0) {
            const int32_t s = A.drag_scroll0 - (y - A.ty0);
            if (A.screen == SCR_READER && A.overlay == OV_NONE) scroll_to(s);
            else if (A.overlay == OV_TOC || A.overlay == OV_MARKS || A.overlay == OV_GET_INFO || A.overlay == OV_DLS) {
                A.list_scroll = s < 0 ? 0 : s;
                A.redraw |= R_ALL;
            } else if (A.screen == SCR_GET && A.overlay == OV_NONE) {
                const int32_t m = get_height() - (g_h - BAR_H);
                A.get_scroll = s > m ? (m > 0 ? m : 0) : s < 0 ? 0 : s;
                A.redraw |= R_ALL;
            } else if (A.screen == SCR_LIBRARY && A.overlay == OV_NONE) {
                const int32_t m = lib_height() - (g_h - BAR_H);
                A.lib_scroll = s > m ? (m > 0 ? m : 0) : s < 0 ? 0 : s;
                A.redraw |= R_ALL;
            }
            // velocity samples
            if (A.nvel == 6) { memmove(A.vy_t, A.vy_t + 1, 5 * sizeof(int)); memmove(A.vy_y, A.vy_y + 1, 5 * sizeof(int)); A.nvel--; }
            A.vy_t[A.nvel] = A.now;
            A.vy_y[A.nvel] = y;
            A.nvel++;
        }
        if (!A.moved && !A.long_fired && A.now - A.t0 > 650) {   // long press on a link: where it goes
            A.long_fired = 1;
            if (A.screen == SCR_READER && A.overlay == OV_NONE && A.pressed < 0 && y >= content_top()) {
                const int l = doc_link_at(&A.doc, x - col_x(), (int)(y - content_top() + A.scroll));
                if (l >= 0) {
                    char h[NVHTML_HREF_MAX];
                    snprintf(h, sizeof h, "%.*s", (int)A.doc.links[l].len, A.doc.text + A.doc.links[l].off);
                    percent_decode(h);
                    toast("%s", h);
                }
            }
        }
        return;
    }
    // release
    x = A.tx;   // a released pointer may report no position: where the finger last was
    y = A.ty;
    A.touch_down = 0;
    const int was = A.pressed;
    A.pressed = -1;
    if (A.long_fired) { A.redraw |= R_ALL; return; }
    if (A.moved) {
        if (A.screen == SCR_READER && A.overlay == OV_NONE && A.nvel >= 2) {
            const int dt = A.vy_t[A.nvel - 1] - A.vy_t[0];
            if (dt > 0 && A.now - A.vy_t[A.nvel - 1] < 80) A.vel = -(float)(A.vy_y[A.nvel - 1] - A.vy_y[0]) / (float)dt;
        }
        if (was >= 0 && (A.overlay == OV_SEARCH || A.screen == SCR_GET) && A.regs[was].act == A_OSK) do_action(A_OSK, A.regs[was].arg);
        A.redraw |= R_ALL;
        return;
    }
    if (was >= 0) {
        const reg_t r = A.regs[was];
        A.redraw |= R_ALL;
        if (r.act != A_DL_CANCEL) A.dl_armed = 0;   // a second tap on cancel must be the very next one
        if (r.act != A_DELETE) A.del_armed = -1;
        do_action(r.act, r.arg);
        return;
    }
    A.dl_armed = 0;
    A.del_armed = -1;
    if (A.overlay == OV_TOC || A.overlay == OV_MENU || A.overlay == OV_VIEWER) {   // a tap outside closes it
        go_back();
        return;
    }
    if (A.screen == SCR_READER && A.overlay == OV_NONE && y >= content_top()) {
        const int dx = x - col_x(), dy = (int)(y - content_top() + A.scroll);
        const int l = doc_link_at(&A.doc, dx, dy);
        const int im = l >= 0 ? -1 : doc_image_at(&A.doc, dx, dy);
        if (l >= 0) follow(l);
        else if (im >= 0) {
            A.view_img = im;
            A.overlay = OV_VIEWER;
            A.focus = -1;
        }
    }
    A.redraw |= R_ALL;
}

// ----------------------------------------------------------------------------------- the loop

static int scan_progress(void *ctx, int done, int total) {
    (void)ctx;
    char msg[120];
    snprintf(msg, sizeof msg, tr(S_SCANNING), done + 1, total);
    gfx_fill(0, 0, g_w, g_h, g_col[C_BG]);
    text_draw(F_SANS, 20, (g_w - text_width(F_SANS, 20, msg, (int)strlen(msg))) / 2, g_h / 2, msg, (int)strlen(msg), g_col[C_TEXT]);
    return !pump();
}

static int s_scroll_frames, s_scroll_t0, s_scroll_last, s_scroll_ms, s_scroll_move, s_scroll_band, s_scroll_flush,
    s_scroll_fill, s_scroll_doc, s_scroll_rows;

static void scroll_meter(int moved) {   // logs the frame rate of each scroll, once it stops
    if (moved) {
        if (!s_scroll_frames) s_scroll_t0 = A.now;
        s_scroll_frames++;
        s_scroll_last = A.now;
    } else if (s_scroll_frames && A.now - s_scroll_last > 400) {
        const int ms = s_scroll_last - s_scroll_t0;
        if (s_scroll_frames > 5 && ms > 0)
            perf("scroll: %d frames in %d ms (%d fps), draw %d ms/frame (move %d, band %d [fill %d doc %d, %d rows], "
                 "flush %d)", s_scroll_frames, ms, s_scroll_frames * 1000 / ms, s_scroll_ms / s_scroll_frames,
                 s_scroll_move / s_scroll_frames, s_scroll_band / s_scroll_frames, s_scroll_fill / s_scroll_frames,
                 s_scroll_doc / s_scroll_frames, s_scroll_rows / s_scroll_frames, s_scroll_flush / s_scroll_frames);
        s_scroll_frames = s_scroll_ms = s_scroll_move = s_scroll_band = s_scroll_flush = 0;
        s_scroll_fill = s_scroll_doc = s_scroll_rows = 0;
    }
}

static void frame(void) {
    if (A.redraw & R_ALL) {
        draw_all();
        A.redraw = 0;
        return;
    }
    if (A.screen == SCR_READER && A.overlay == OV_NONE) {
        scroll_meter(A.scroll != A.drawn_scroll);
        if (A.scroll != A.drawn_scroll) {   // move what is on screen, draw what comes in
            const int ts = nv_millis();
            const int top = content_top(), h = g_h - top;
            const int dy = (int)(A.drawn_scroll - A.scroll);
            if (dy > -h && dy < h && A.focus_link < 0) {
                gfx_scroll(top, g_h, dy);
                const int tm = nv_millis();
                s_scroll_move += tm - ts;
                const int band_y = dy > 0 ? top : g_h + dy, band_h = dy > 0 ? dy : -dy;
                gfx_fill(0, band_y, g_w - 7, band_h, g_col[C_BG]);
                const int tf0 = nv_millis();
                doc_draw(&A.doc, col_x(), band_y, A.scroll + (band_y - top), band_h, A.focus_link);
                const int tf1 = nv_millis();
                draw_thumb();
                s_scroll_band += nv_millis() - tm;
                s_scroll_fill += tf0 - tm;
                s_scroll_doc += tf1 - tf0;
                s_scroll_rows += band_h;
                A.drawn_scroll = A.scroll;
            } else {
                draw_reader_content();
            }
            if (A.toast[0] && A.now <= A.toast_until) draw_toast();
            s_scroll_ms += nv_millis() - ts;
        }
        if (A.redraw & R_CONTENT) draw_reader_content();
        if (A.redraw & R_BAR) { A.nregs = 0; draw_reader_bar(); }
        A.redraw = 0;
    }
}

static int32_t s_saved_scroll = -1;

static void save_position(void) {
    if (A.has_doc && A.coll >= 0) {
        snprintf(g_set.last_file, sizeof g_set.last_file, "%s", g_lib[A.coll].file);
        snprintf(g_set.last_path, sizeof g_set.last_path, "%s", A.path);
        g_set.last_scroll = A.scroll;
    }
    settings_save();
    s_saved_scroll = A.scroll;
}

// The reading position, saved once the page has been still for two seconds (a closed app keeps it).
static void save_position_when_still(void) {
    static int32_t last_scroll = -1;
    static int still_since;
    if (!A.has_doc || A.screen != SCR_READER || A.scroll == s_saved_scroll) return;
    if (A.scroll != last_scroll) {
        last_scroll = A.scroll;
        still_since = A.now;
        return;
    }
    if (A.now - still_since > 2000 && !A.touch_down && A.vel == 0) save_position();
}

// A repeatable measurement on the board: with /appdata/bench.txt holding "FILE<TAB>PATH" the app opens
// that page and scrolls it by itself, BENCH_STEP px per frame, down then up; the frame rate lands in
// the system log ("scroll: ..."). Delete the file to use the app normally.
#define BENCH_STEP 24
static int s_bench;   // frames of scrolling left (> 0 down, < 0 up)

static void bench_start(void) {
    char p[220], line[700];
    snprintf(p, sizeof p, "%s/appdata/bench.txt", bib_root());
    FILE *f = fopen(p, "r");
    if (!f) return;
    const int ok = fgets(line, sizeof line, f) != NULL;
    fclose(f);
    if (!ok) return;
    line[strcspn(line, "\r\n")] = 0;
    char *tab = strchr(line, '\t');
    if (!tab) return;
    *tab = 0;
    const int ci = lib_find(line);
    if (ci < 0 || open_path(ci, tab + 1, NULL, 0, 0) < 0) return;
    perf("bench: '%s' (%d px), %d px per frame", A.title, (int)A.doc.height, BENCH_STEP);
    s_bench = 90;
}

static void bench_step(void) {
    if (!s_bench || A.screen != SCR_READER || A.overlay != OV_NONE) return;
    const int32_t before = A.scroll;
    scroll_to(A.scroll + (s_bench > 0 ? BENCH_STEP : -BENCH_STEP));
    if (s_bench > 0 && (--s_bench == 0 || A.scroll == before)) s_bench = -90;
    else if (s_bench < 0 && (++s_bench == 0 || A.scroll == 0)) s_bench = 0;
}

// Opened with a file (Files > Open): a collection in ~/zim opens at its main page; one elsewhere
// is read where it is, through the OS (read-only).
static void opened_with_file(void) {
    char p[400];
    if (nv_open_path(p, sizeof p) <= 0) return;
    const char *name = strrchr(p, '/');
    name = name ? name + 1 : p;
    const int ci = lib_find(name);
    if (ci >= 0) { open_main(ci); return; }
    toast(tr(S_MOVED_HINT), NULL);
}

// The runtime grows the app's linear memory by reallocating it whole in PSRAM: later on, with the
// system's own allocations around it, a grow can find no block big enough and malloc fails (seen
// as "out of memory" or a cluster that "is damaged"). Right after the start the big block the app
// was loaded into is still free: grow to nearly the whole budget once (malloc keeps what it got).
static void *volatile s_grown;   // the block escapes: the compiler may not drop the malloc/free pair

static void pregrow(void) {
    for (size_t mb = 10; mb >= 3; mb--) {
        char *p = (char *)malloc(mb << 20);
        if (!p) continue;
        ((volatile char *)p)[(mb << 20) - 1] = 0;
        s_grown = p;
        free(p);
        return;
    }
}

NV_EXPORT("run")
void run(void) {
#ifndef BIB_NO_PREGROW   // the PC test of the memory budget measures what the app itself uses
    pregrow();
#endif
    memset(&A, 0, sizeof A);
    A.coll = -1;
    A.focus = A.focus_link = A.pressed = -1;
    A.del_armed = -1;
    A.get_group = -1;
    if (gfx_init() < 0 || font_init() < 0) return;
    i18n_init();
    snprintf(A.get_lang, sizeof A.get_lang, "%s", i18n_lang3());
    settings_load();
    theme_set(g_set.dark);
    A.sugg = (nvzim_entry_t *)calloc(SUGG_MAX, sizeof(nvzim_entry_t));
    A.found = (char(*)[NVZIM_TITLE_MAX])calloc(FOUND_MAX, NVZIM_TITLE_MAX);
    A.found_path = (char(*)[NVZIM_PATH_MAX])calloc(FOUND_MAX, NVZIM_PATH_MAX);
    if (!A.sugg || !A.found || !A.found_path) return;
    A.now = nv_millis();
    const int t0 = nv_millis();
    lib_scan(scan_progress, NULL);
    perf("library: %d collections in %d ms", g_nlib, nv_millis() - t0);
    opened_with_file();
    bench_start();
    A.redraw = R_ALL;
    int last = nv_millis();
    while (!A.quit) {
        A.now = nv_millis();
        const int dt = A.now - last;
        last = A.now;
        if (nv_gfx_back()) go_back();
        kev_t keys[8];
        const int nk = kbd_poll(keys, 8, A.now, i18n_italian_keys());
        for (int i = 0; i < nk && !A.quit; i++) on_key(&keys[i]);
        int x, y;
        const int down = nv_touch(&x, &y) != 0;
        if (down || A.touch_down) on_touch(down, x, y);
        bench_step();
        save_position_when_still();
        if (A.screen == SCR_GET && get_poll()) A.redraw |= R_ALL;   // the catalog and its thumbnails
        dl_poll();
        if (A.vel != 0 && !A.touch_down) {   // fling
            const int32_t before = A.scroll;
            scroll_to(A.scroll + (int32_t)(A.vel * (float)dt));
            A.vel *= 0.94f;
            if ((A.vel > -0.03f && A.vel < 0.03f) || A.scroll == before) A.vel = 0;
        }
        if (A.toast[0] && A.now > A.toast_until) { A.toast[0] = 0; A.redraw |= R_ALL; }
        if (A.quit) break;
        // pictures: one decoded per frame, while the page is still (then it shows up)
        if (img_pending() && !A.touch_down && A.vel == 0 && A.screen == SCR_READER &&
            (A.overlay == OV_NONE || A.overlay == OV_VIEWER)) {
            const int ti = nv_millis();
            if (img_work(img_read, img_done, img_cluster_ready, NULL)) {
                perf("picture: %s in %d ms", img_last(), nv_millis() - ti);
                A.redraw |= A.overlay == OV_VIEWER ? R_ALL : R_CONTENT;
            }
        }
        frame();
        const int tf = nv_millis();
        gfx_flush();
        if (s_scroll_frames) s_scroll_flush += nv_millis() - tf;
        if (!nv_gfx_present()) break;
    }
    save_position();
    close_coll();
}
