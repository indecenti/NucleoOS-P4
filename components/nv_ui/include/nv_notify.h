// nv_notify — NucleoOS system notification service (toasts + notification center).
//
// THE SYSTEM STANDARD for user-facing feedback. Apps and services never draw their own
// toasts or banners; they call one of:
//
//   nv_toast(kind, msg)                 transient snackbar only (not stored)
//   nv_notify_post(kind, title, msg)    toast + stored in the shade notification center
//
// Severity drives the accent stripe/icon and the sound: NV_NOTE_ERROR also plays the alert
// tone. "Do not disturb" (config "qs_dnd", quick-settings chip) suppresses toasts and sounds
// for INFO/OK/WARN — errors still surface. Posts are always stored regardless of DND.
//
// Storage is a fixed ring (newest first, no heap): NV_NOTIFY_CAP entries, each with a
// timestamp formatted at post time. The SystemUI renders the ring in the notification shade
// and shows an unread badge in the status bar via the listener hook.
//
// Threading: call ONLY from the LVGL thread (event/timer handlers, or inside lvgl_port_lock)
// — same contract as the rest of nv_ui.
#pragma once

#include <stdbool.h>
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    NV_NOTE_INFO = 0,   // neutral info (accent stripe)
    NV_NOTE_OK,         // success/confirmation (success stripe)
    NV_NOTE_WARN,       // warning (danger stripe, warning icon)
    NV_NOTE_ERROR       // error (danger stripe; bypasses DND, plays alert tone)
} nv_note_kind_t;

#define NV_NOTIFY_CAP 8

typedef struct {
    nv_note_kind_t kind;
    char title[32];     // short source/app name shown bold
    char text[96];      // body (clipped)
    char when[8];       // "HH:MM" formatted at post time
} NvNote;

// Transient toast only. NULL/empty msg is a no-op.
void nv_toast(nv_note_kind_t kind, const char *msg);

// Toast + store in the notification center (ring, newest first). title may be NULL ("System").
void nv_notify_post(nv_note_kind_t kind, const char *title, const char *msg);

// Ring access for the SystemUI. index 0 = newest; NULL when out of range.
int nv_notify_count(void);
const NvNote *nv_notify_get(int index);

// Unread tracking for the status-bar badge: posts increment, mark_read zeroes (shade opened).
int  nv_notify_unread(void);
void nv_notify_mark_read(void);

// Remove all stored notifications (shade "Clear all").
void nv_notify_clear(void);

// SystemUI hook, fired after every post/clear/mark_read: refresh badge + live shade list.
void nv_notify_set_listener(void (*cb)(void));

// Severity mapping (glyph + theme color) shared with the SystemUI shade renderer.
const char *nv_note_kind_symbol(nv_note_kind_t kind);
lv_color_t  nv_note_kind_color(nv_note_kind_t kind);

#ifdef __cplusplus
}
#endif
