// dmd_menu — the DMD app's touch menu of PinUP Popper tables, served over USB by tools/popper_bridge
// while no table runs. See docs/DMD_POPPER.md. LVGL thread only.
#pragma once
#include "dmd_core.h"
#include "lvgl.h"

// Build the (hidden) menu under the app's root. open_settings: the DMD panel (the menu covers the area
// a long press would use, so it has its own settings button). again: the app's screen rebuilt after
// dmd_menu_drop_ui (language, theme, rotation): only the widgets are made, back where they were.
void dmd_menu_build(lv_obj_t *root, void (*open_settings)(void), bool again = false);
// 10 Hz: take the bridge's messages, keep the menu's data current. `allowed`: the app lets the menu be
// on screen (no DMD frames, no panel). Returns true while the menu is on screen.
bool dmd_menu_tick(bool allowed);
// Esc / Back inside the menu: closes a sub-page. False at the menu's top level (or when hidden).
bool dmd_menu_back(void);
// A table was launched or is running (from the bridge): the app shows the DMD as soon as frames come.
bool dmd_menu_in_game(void);
// esp_timer time the running game started (0: none)
int64_t dmd_menu_game_start_us(void);
const char *dmd_menu_game(void);   // its name ("" = none)
// In game, a tap on the DMD: quick actions (back to the DMD, the tables, quit the game, settings).
void dmd_menu_quick_open(void);
// A modal or the tables browsed during a game want the screen: the DMD steps aside. Any thread.
bool dmd_menu_covering(void);
// Its backglass (sent by the bridge as the game starts), 1024 x h: 344 above the DMD (backbox), 600 the
// whole screen. NULL when not there. LVGL thread.
const lv_image_dsc_t *dmd_menu_game_art(int h);
// The same art for the composition screen, in a buffer of its own (any table).
const lv_image_dsc_t *dmd_menu_comp_art(int id, int h);
// The same pictures of any table (the composition screen's), NULL when not here (yet); fetch asks for the
// missing ones (the card, else the bridge) and keeps them while the screen is open.
const lv_image_dsc_t *dmd_menu_table_art(int id, int h);
const lv_image_dsc_t *dmd_menu_table_grill(int id, dmd_rect_t *win);
const lv_image_dsc_t *dmd_menu_table_full(int id, dmd_rect_t *win);
const lv_image_dsc_t *dmd_menu_table_b2s(int id, dmd_rect_t *box, int *grill, dmd_rect_t *housing);
void dmd_menu_table_full_shape(int id, int *aw, int *ah);
void dmd_menu_comp_fetch(int id);
int dmd_menu_selected(void);               // the table on the menu's stage (0: none)
const char *dmd_menu_selected_name(void);
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
// A request that could not go: "after the game" while dmdext has the cable, else "busy, try again".
void dmd_menu_send_fail_note(void);
// The Popper bridge as its hello described it: false when no hello came since the app opened.
bool dmd_menu_bridge_info(char *ver, size_t n, bool *popper_ok);
// Quit the running table over Wi-Fi and start it again once it has ended (a new composition then).
void dmd_menu_quit_and_relaunch(void);
// dmd_comp.cpp: the composition of both screens for the running table (the PC's second screen and this
// panel). Open (false when it cannot), close, open?; flush: send the compositions waiting for the cable;
// on_result: the bridge's answer.
bool dmd_comp_open(lv_obj_t *root);
// The same screen for every table (the panel's "Layout"): only this panel's fits, set for all tables;
// on_close runs when it closes (the panel shows the new choice).
bool dmd_comp_open_all(lv_obj_t *root, void (*on_close)(void));
// The same screen for any table, from the menu, without playing it (its pictures are asked for as it opens).
bool dmd_comp_open_table(lv_obj_t *root, int id, const char *name);
// A picture of the screen's table came: the previews again with it.
void dmd_comp_refresh(int id);
// What a table's backglass file (.directb2s) holds, as the bridge read it ('R' b2sinfo); the result of
// choosing its dual-backglass version ('R' dual).
struct DmdB2sInfo {
    int id;
    bool b2s, dual;            // a .directb2s; a dual one (Authentic and Fantasy versions)
    int bw, bh, grill;         // its backglass picture and its grill's rows
    int aw, ah;                // its FullDMD art (0: none): the second, smaller picture
    int mode;                  // the dual version in use: 1 Authentic, 2 Fantasy
    int s2;                    // the second screen's composition written for it (0: as it came)
    bool s2dmd;                // with a DMD on the second screen too
    bool fp;                   // a Future Pinball table played with its backbox off: Popper's backglass picture
    int fpfull;                // ... on the whole monitor, drawn by the bridge (1), or where Popper shows it (0)
};
void dmd_comp_on_info(const DmdB2sInfo *info);
void dmd_comp_on_dual(int id, int mode, bool ok, const char *err);
void dmd_comp_close(void);
void dmd_comp_abandon(void);   // closed with the app's screen: as close, without its on_close callback
bool dmd_comp_is_open(void);
void dmd_comp_tick(void);   // retries of what the screen waits for (from dmd_menu_tick)
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
void dmd_app_set_fit_all(int fit);
// The whole geometry a fit gives the DMD on screen with those dots (the composition's 1:1 lens), and the
// dots' look: shape, glow, brightness (%), rotation.
bool dmd_app_fit_geom(int fit, int dot, const dmd_rect_t *win, dmd_geom_t *g);
void dmd_app_look(int *shape, bool *glow, int *bright, int *rot);
void dmd_app_dmd_size(int *w, int *h);
bool dmd_app_snapshot(uint32_t *dst, size_t cap, int *w, int *h);
// win: the picture's window / housing for the fits drawn in one (7 art with window, 8 backglass with
// housing); NULL: the running game's.
bool dmd_app_fit_rect(int fit, int dot, const dmd_rect_t *win, dmd_rect_t *out);
// The fit to suggest for the running table: its FullDMD art with the DMD in its window when it has one
// (has_art), else the rebuilt
// full screen for a 128x16 DMD, else the classic DMD (with its ambient bands).
int dmd_app_fit_suggested(bool has_art, bool has_grill);
// The bridge is there and no table runs: the menu wants the screen (the idle clock gives way to it).
bool dmd_menu_ready(void);
// The look: 0 cabinet (Popper's chrome and cyan), 1 DMD (coloured dot-matrix). Before or after build: the
// menu is rebuilt in place, its data kept.
void dmd_menu_set_theme(int theme);
// The root's widgets are about to go but the app carries on (its screen rebuilt): drop what points at them
// and animates them; the lists, pictures, the game and the PC's state stay for dmd_menu_build(again).
void dmd_menu_drop_ui(void);
// The app stops (or its root is deleted): drop images, timers and pointers. save_place: where the user was
// (nv_ui_state_save), for a suspended task; not when the app is closed for good.
void dmd_menu_destroy(bool save_place = true);
