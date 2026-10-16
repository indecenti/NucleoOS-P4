// dmd_menu — the DMD app's touch menu of PinUP Popper tables, served over USB by tools/popper_bridge
// while no table runs. See docs/DMD_POPPER.md. LVGL thread only.
#pragma once
#include "dmd_core.h"
#include "lvgl.h"

// Build the (hidden) menu under the app's root. open_settings: the DMD panel (the menu covers the area
// a long press would use, so it has its own settings button).
void dmd_menu_build(lv_obj_t *root, void (*open_settings)(void));
// 10 Hz: take the bridge's messages, keep the menu's data current. `allowed`: the app lets the menu be
// on screen (no DMD frames, no panel). Returns true while the menu is on screen.
bool dmd_menu_tick(bool allowed);
// Esc / Back inside the menu: closes a sub-page. False at the menu's top level (or when hidden).
bool dmd_menu_back(void);
// A table was launched or is running (from the bridge): the app shows the DMD as soon as frames come.
bool dmd_menu_in_game(void);
const char *dmd_menu_game(void);   // its name ("" = none)
// In game, a tap on the DMD: quick actions (back to the DMD, the tables, quit the game, settings).
void dmd_menu_quick_open(void);
// A modal or the tables browsed during a game want the screen: the DMD steps aside. Any thread.
bool dmd_menu_covering(void);
// Its backglass (sent by the bridge as the game starts), 1024 x h: 344 above the DMD (backbox), 600 the
// whole screen. NULL when not there. LVGL thread.
const lv_image_dsc_t *dmd_menu_game_art(int h);
// Its backglass with the grill (1024 x 600, the whole picture fitted) and the DMD's housing found in the
// grill. NULL when it has no grill or no housing was found. LVGL thread.
const lv_image_dsc_t *dmd_menu_game_grill(dmd_rect_t *win);
// Its FullDMD art (1024 x 600: made for a big DMD screen, sent by the bridge as the game starts when asked)
// and the window in it where the DMD goes. NULL when not there or it has no window. LVGL thread.
const lv_image_dsc_t *dmd_menu_game_full(dmd_rect_t *win);
// Its backglass as B2S shows it (1024 x 600, the picture fitted whole, with or without a grill): box = where
// the picture is in it, grill = its grill's rows at the bottom of the box (0: none), housing = the DMD's
// housing found there (w 0: none). NULL when not there. LVGL thread.
const lv_image_dsc_t *dmd_menu_game_b2s(dmd_rect_t *box, int *grill, dmd_rect_t *housing);
// Its FullDMD art's true shape (16:9 when the bridge did not say).
void dmd_menu_game_full_shape(int *aw, int *ah);
// The PC's second screen (the backglass monitor) as the bridge reported it: false when none / not known.
bool dmd_menu_s2(int *w, int *h);
// A request to the bridge now: false when the cable is not the bridge's (a game runs).
bool dmd_menu_send(const char *json);
// Quit the running table over Wi-Fi and start it again once it has ended (a new composition then).
void dmd_menu_quit_and_relaunch(void);
// dmd_comp.cpp: the composition of both screens for the running table (the PC's second screen and this
// panel). Open (false when it cannot), close, open?; flush: send the compositions waiting for the cable;
// on_result: the bridge's answer.
bool dmd_comp_open(lv_obj_t *root);
void dmd_comp_close(void);
bool dmd_comp_is_open(void);
void dmd_comp_flush(void);
void dmd_comp_on_result(int game_id, bool ok, const char *err);
// "Fit" is FullDMD: ask the bridge for that art as games start (told now and at every hello). LVGL thread.
void dmd_menu_want_fulldmd(bool on);
// The running table's Popper id (0: none).
int dmd_menu_game_id(void);
// From dmd_app.cpp: a table's own "Fit" (-1: as every table, else one of the panel's fits) and
// setting it (-1 clears it). LVGL thread.
int dmd_app_table_fit(int game_id);
void dmd_app_set_table_fit(int game_id, int fit);
// The running game's DMD only lights the middle half of its rows (a 128x16 one dmdext centres in 128x32).
bool dmd_app_dmd_half(void);
// A table's 128x16 DMD rebuilt to the full height (default) or shown as it comes, and setting it.
bool dmd_app_table_rebuild(int game_id);
void dmd_app_set_table_rebuild(int game_id, bool on);
// A table's dots: bit 0 original (square, whole pixels, never stretched), bits 1-2 a margin of 0-3 rows and
// columns of unlit dots around the DMD; and setting them. LVGL thread.
int dmd_app_table_dot(int game_id);
void dmd_app_set_table_dot(int game_id, int dot);
// The DMD on screen now: its size (0 x 0: none), a copy of its latest frame (0x00RRGGBB, at most cap
// pixels; false when there is none), where a fit puts it on the panel with those dots (for previews).
int dmd_app_fit_all(void);   // the panel's "Fit" (every table)
void dmd_app_dmd_size(int *w, int *h);
bool dmd_app_snapshot(uint32_t *dst, size_t cap, int *w, int *h);
bool dmd_app_fit_rect(int fit, int dot, dmd_rect_t *out);
// The fit to suggest for the running table: its FullDMD art with the DMD in its window when it has one
// (has_art), else the rebuilt
// full screen for a 128x16 DMD, else the classic DMD (with its ambient bands).
int dmd_app_fit_suggested(bool has_art, bool has_grill);
// The bridge is there and no table runs: the menu wants the screen (the idle clock gives way to it).
bool dmd_menu_ready(void);
// The look: 0 cabinet (Popper's chrome and cyan), 1 DMD (coloured dot-matrix). Before or after build: the
// menu is rebuilt in place, its data kept.
void dmd_menu_set_theme(int theme);
// The root is being deleted: drop images, timers and pointers.
void dmd_menu_destroy(void);
