// nv_capture — the screenshot tool (Lightshot / Win+Shift+S style). See docs/SCREENSHOT_CLIPBOARD.md.
//
// REGION: the screen freezes and dims, the user drags a rectangle (drag inside it to move it, a tap
// without dragging takes the whole screen), then Copy (Enter) / Save (Ctrl+S) / Ask ANIMA / Cancel
// (Esc). FULL: the whole screen straight to the clipboard (Alt+PrtSc). Every result lands on the
// system clipboard (nv_clipboard) as an image; Save also writes ~/shots/shot-YYYYmmdd-HHMMSS.jpg.
// LVGL-thread only.
#pragma once
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum { NV_CAPTURE_REGION = 0, NV_CAPTURE_FULL } nv_capture_mode_t;

// Start a capture now (`delay_ms` > 0: after a pause, e.g. to let a closing shade leave the screen).
void nv_capture_start(nv_capture_mode_t mode, uint32_t delay_ms);
bool nv_capture_active(void);
// Keyboard while the overlay is up (HID usage + modifiers); true = consumed.
bool nv_capture_key(uint8_t usage, uint8_t mods);
// Esc / back: cancels an open overlay. True when there was one.
bool nv_capture_escape(void);

#ifdef __cplusplus
}
#endif
