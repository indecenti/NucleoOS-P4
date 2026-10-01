// Desktop manners for lists and grids, system-wide (any shell, any native app).
//
//   nv_sel_attach(list);            // the container: rubber-band selection, background click clears
//   nv_sel_item(row);               // each row / tile: hover + selected look, drag starts a band
//   row CLICKED cb:  if (nv_sel_click(e)) return;   // mouse click selected it: don't open yet
//
// Mouse: click selects, Ctrl+click toggles, Shift+click selects a range, double click (or Enter)
// opens, drag (from a row or the empty area) draws a selection rectangle; the list scrolls by
// itself at its edges. A finger and the keyboard behave as before: a tap opens. Selected = the
// LV_STATE_CHECKED state; the container gets LV_EVENT_VALUE_CHANGED when the selection changes.
//
// Context menus: an object listening to nv_ui_event_context() gets it on a right click (param =
// lv_point_t *, the pointer) or on the Menu key / Shift+F10 while it has the keyboard focus; it
// answers with nv_ui_menu_open(). A right click on an unselected item selects it first.
//
// LVGL thread only. Uses LV_STATE_USER_1 on items during a band.
#pragma once
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

void nv_sel_attach(lv_obj_t *cont);
void nv_sel_item(lv_obj_t *item);
bool nv_sel_click(lv_event_t *e);
int  nv_sel_count(lv_obj_t *cont);
lv_obj_t *nv_sel_nth(lv_obj_t *cont, int n);   // n-th selected item, in child order
void nv_sel_clear(lv_obj_t *cont);
void nv_sel_all(lv_obj_t *cont);
// The pointer acting now is the mouse (a long press from it is a slow drag, not a context action).
bool nv_sel_mouse(void);

// ---- context menus
uint32_t nv_ui_event_context(void);
typedef struct {
    const char *icon;       // LV_SYMBOL_* or NULL
    const char *text;
    const char *hint;       // right-aligned shortcut ("Ctrl+C") or NULL
    void (*fn)(void *ud);   // runs after the menu closed
    void *ud;
    bool sep_before;
    bool disabled;
} nv_menu_item_t;
// Up to 12 items, at panel point (x, y), kept on screen. Esc / a click outside closes it.
void nv_ui_menu_open(int32_t x, int32_t y, const nv_menu_item_t *items, int n);
// Shell: deliver the context event to `hit` or its nearest ancestor that listens. False if none.
bool nv_sel_context_at(lv_obj_t *hit, lv_point_t p);

#ifdef __cplusplus
}
#endif
