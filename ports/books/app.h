// app.h — the state and the shared pieces of the Libri app's screens (app.c: the loop, input,
// widgets, the on-screen keyboard; library.c: the library; reader.c: reading and its panels;
// catalog.c: the store's catalog of free books).
#pragma once
#include "bk.h"

#define BAR_H 64
#define BTN 52
#define REG_MAX 192
#define OSK_H 248
#define BACK_MAX 16
#define RES_MAX 200

enum { SCR_LIBRARY, SCR_READER, SCR_CATALOG };
enum { SH_NONE, SH_CHROME, SH_TOC, SH_LOOK, SH_SEARCH, SH_NOTE, SH_VIEWER, SH_INFO, SH_SORT, SH_BOOK, SH_CAT_BOOK,
       SH_CAT_LANGS, SH_CAT_DLS, SH_CAT_CATS };
enum { R_ALL = 1, R_PAGE = 2 };

// actions of the touch regions (and of Enter on a focused one)
enum {
    A_NONE, A_BACK, A_OPEN, A_BOOK_INFO, A_CONTINUE, A_SORT, A_SORT_SET, A_CATALOG, A_SEARCH, A_TOC, A_LOOK, A_MARK,
    A_INFO, A_CLOSE, A_TOC_TAB, A_TOC_ITEM, A_MARK_ITEM, A_MARK_DEL, A_THEME, A_FONT, A_SIZE, A_SPACING, A_MARGIN,
    A_TOGGLE, A_ANIM, A_EINK_N, A_COLS, A_RESULT, A_OSK, A_FIELD, A_NOTE_GO, A_SLIDER, A_CHAP_PREV, A_CHAP_NEXT,
    A_BACK_JUMP, A_DELETE, A_LIB_SCROLL, A_CAT_CHIP, A_CAT_ROW, A_CAT_GET, A_CAT_READ, A_CAT_RETRY, A_CAT_LANGS,
    A_CAT_LANG, A_CAT_DLS, A_CAT_CANCEL, A_CAT_SEARCH, A_PAGE_TAP, A_ORIENT, A_LIBRARY,
};
enum { TG_JUSTIFY, TG_HYPHEN, TG_INDENT, TG_TAP, TG_STATUS };

typedef struct { int x, y, w, h, act, arg; } reg_t;

typedef struct { int chap; int32_t off; char snip[160]; int sl, sn; } result_t;   // sl, sn: the match in snip

typedef struct {
    int screen, sheet;
    int redraw;
    int quit;
    reg_t regs[REG_MAX];
    int nregs, focus, pressed;
    int now;
    // the open book
    book_t *book;
    int lib_idx;
    char file[200];
    doc_t doc, side;         // the current chapter, and the one laid out before it (back and forth is free)
    int chap, side_chap;
    int page;                // the first page of the spread on screen
    int cols;                // pages side by side
    lopt_t lo;               // the layout the docs were made with
    int col_x[2], text_y, text_h;
    surf_t cur, nxt;         // the spread on screen; the next (or previous) one drawn ahead
    int nxt_chap, nxt_page, nxt_ok, cur_ok;
    int turns;               // since the last full e-ink refresh
    int idle_since;
    int focus_link;
    pos_t back[BACK_MAX];    // where links were followed from
    int nback;
    int lay_chap;            // the chapter being laid out (lay_image_size reads its pictures)
    // panels
    int list_tab;            // contents (0) / bookmarks (1)
    int32_t list_scroll;
    int list_sel;
    mark_t *marks;
    int nmarks;
    char q[160];
    result_t *res;
    int nres, searched;      // results; the query was run
    char *note;              // the text of a note's popup
    int note_chap;
    int32_t note_off;
    int view_img;            // the picture viewer: index in doc.imgs
    int zoom100, pan_x, pan_y;
    int slider_drag;
    // library
    int32_t lib_scroll;
    int *order, norder;
    int book_sel;            // the book of the info sheet
    int del_armed;
    int meta_next;           // the next entry whose metadata / cover is read while idle
    uint16_t **covers;       // per library entry: its cover, NULL when none (yet)
    uint8_t *cover_tried;
    // input
    int touch_down, tx0, ty0, tx, ty, t0, moved, long_fired, drag_turn, drag_dir;
    int32_t drag_scroll0;
    int vx_t[6], vx_x[6], vy_y[6], nvel;
    float vel;
    int pinch, pinch_d0, pinch_size0;
    int last_tap_t, last_tap_x, last_tap_y;
    int osk_on, osk_layer, osk_shift;
    char toast[300];
    int toast_until;
} app_t;

extern app_t A;

// app.c
void perf(const char *fmt, ...);
void toast(const char *fmt, const char *arg);
int pump(void);
int reg_add(int x, int y, int w, int h, int act, int arg);
int reg_state(int idx);
void button(int x, int y, int ic, int act, int arg, int enabled, uint16_t fg);
int row_box(int x, int y, int w, int h, int act, int arg, uint16_t bg);
int pill(int x, int y, const char *lbl, int act, int arg, int on);
void segmented(int x, int y, int w, const char *const *labels, int n, int sel, int act);
void bar_bg(void);
void scrim(void);
void draw_toast(void);
void sheet_frame(int x, int y, int w, int h);
void draw_osk(void);
void draw_field(int x, int y, int w, const char *placeholder);
void q_append(uint32_t c);
void q_backspace(void);
void theme_apply(void);
void wrap_text(int f, int px, int x, int y, int w, int lh, const char *s, int max_lines, uint16_t c);
void draw_cover(int i, int x, int y, int w, int h);   // a library entry's cover (picture or drawn)
void draw_typo_cover(const char *title, const char *author, uint32_t hue, int x, int y, int w, int h);

// library.c
void lib_draw(void);
int lib_height(void);
void lib_order(void);
void lib_idle(void);
void book_sheet_draw(void);
void library_rescan(void);      // ~/books read again (a download finished): covers dropped, order made again

// reader.c
int reader_open(int lib_idx);
void reader_close(void);
void reader_draw(void);         // the screen: the spread and the panel on it
void reader_frame(void);        // per-frame work (pictures, drawing ahead, saving the position)
void reader_turn(int dir, int animate);
void reader_key(const kev_t *k);
void reader_touch(int down, int x, int y);
int reader_action(int act, int arg);
void reader_save(void);
void reader_search_run(void);
void reader_reshape(void);      // the screen changed shape (orientation): surfaces and pages again

// catalog.c
void cat_draw(void);
void cat_open(void);
int cat_action(int act, int arg);
void cat_frame(void);
void cat_key(const kev_t *k);
void cat_bg_poll(void);         // every frame: finished downloads join the library
int32_t *cat_scroll(void);
int cat_height(void);
