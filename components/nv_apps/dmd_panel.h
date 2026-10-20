// dmd_panel — the DMD app's settings panel (a long press on the DMD, Enter, or the menu's settings button):
// the state of the link, the screen's layout (chosen with previews in the composition screen), the dots,
// the menu's style, the system. Built here with the app's own widgets (dmd_ui.h), so the simulator shows it
// as it is; what each setting does stays in dmd_app.cpp, behind DmdPanelIo. LVGL thread.
#pragma once
#include "lvgl.h"

#include <cstdint>

struct DmdPanelIo {
    // the settings as they are when the panel is built
    int theme = 0, color = 0, bright = 100, shape = 0, rot = 0;
    bool glow = false, clock = true, boot = false;
    const int32_t *colors = nullptr;   // the colour choices (0x00RRGGBB, -1: as the PC sends them)
    int ncolors = 0;
    // what the panel's controls do
    void (*resume)(void) = nullptr;   // back to the DMD
    void (*test)(void) = nullptr;     // the test screen
    void (*exit)(void) = nullptr;     // close the app (the app asks first)
    void (*layout)(void) = nullptr;   // the composition screen for every table
    void (*set_theme)(int) = nullptr;
    void (*set_color)(int) = nullptr;
    void (*set_bright)(int value, bool done) = nullptr;   // done: the finger left the slider (save it)
    void (*set_shape)(int) = nullptr;
    void (*set_glow)(bool) = nullptr;
    void (*set_rot)(int) = nullptr;
    void (*set_clock)(bool) = nullptr;
    void (*set_boot)(bool) = nullptr;
    void (*make_port)(lv_obj_t *row) = nullptr;   // the network port's field, put into row
};

// The panel, hidden, filling parent. Its "back to the DMD" button (the keyboard's first focus) in *resume.
lv_obj_t *dmd_panel_build(lv_obj_t *parent, const DmdPanelIo *io, lv_obj_t **resume);
// The link in one line, and the address a PC on Wi-Fi uses (in the header: what DmdDevice.ini needs).
void dmd_panel_status(const char *line, const char *address);
// The first button's name: back to the DMD while a PC drives it, else just back.
void dmd_panel_resume_label(const char *text);
// A line under the port's field: why a port was refused or cannot be opened ("" or NULL: none).
void dmd_panel_port_note(const char *text);
// The layout card: the fit for every table, the running table's own (-1: none), a note on what the screen
// really shows (a rotated screen; NULL: none).
void dmd_panel_layout(int fit_all, int table_fit, const char *note = nullptr);
void dmd_panel_destroyed(void);   // the panel's objects are gone (the app's page was deleted)

// A question in the app's look: a card on a dim veil over parent, two buttons. The first (stay) is the safe
// choice, in *first for the keyboard's focus; each button runs its callback, which closes the question
// (lv_obj_delete_async on the veil: it is deleted from its own button's event). Returns the veil, NULL
// without memory.
lv_obj_t *dmd_panel_ask(lv_obj_t *parent, const char *title, const char *body, const char *stay, const char *ok,
                        void (*on_stay)(void), void (*on_ok)(void), lv_obj_t **first);
