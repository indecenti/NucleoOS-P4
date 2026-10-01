// nv_notify — NucleoOS system notification service (toasts + notification center).
//
// THE SYSTEM STANDARD for user-facing feedback. Apps and services never draw their own
// toasts or banners; they call one of:
//
//   nv_toast(kind, msg)                       transient snackbar only (not stored)
//   nv_notify_post(kind, title, msg)          popup + stored in the shade notification center
//   nv_notify_post_ex(kind, title, msg, &o)   the same, with a tag, a tap action or quiet
//
// Severity drives the accent stripe/icon and the sound: NV_NOTE_ERROR also plays the alert
// tone. Posts are always stored; whether they also pop up is the system's call, so the user
// is told what matters without being buried:
//   - "Do not disturb" (config "qs_dnd", quick-settings chip) silences INFO/OK/WARN popups;
//   - a fullscreen app (game, video), the lock screen and a blanked screen get no popups either;
//   - opts.quiet stores the post with the badge only (news that can wait);
//   - a post whose tap action is the app already in front stays quiet (it is on screen);
//   - the same title + text again within a minute folds into the stored one ("x2"), no popup;
//   - a tag keeps ONE live note per subject: a newer post with that tag replaces the old one.
// Errors always pop up.
//
// Presentation: the tablet shell shows a snackbar at the bottom; the classic desktop shows
// popup cards over the taskbar's right corner (stacked, newest at the bottom, click = the tap
// action or the shade, x = dismiss, the pointer over one holds it). Several posts in a row
// queue up instead of overwriting each other.
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

#define NV_NOTIFY_CAP 12

typedef struct {
    nv_note_kind_t kind;
    char title[32];     // short source/app name shown bold
    char text[96];      // body (clipped)
    char when[8];       // "HH:MM" formatted at post time
    uint32_t id;        // unique per post (stable handle for nv_notify_remove)
    char tag[16];       // subject: a newer post with the same tag replaces this one ("" = none)
    char app[24];       // tap opens this app id ("" = no action: the tap just opens the shade)
    char page[16];      // ...on this page (nv_ui_open_app_page), "" = its start page
    uint16_t repeat;    // identical posts folded into this one (1 = just this one)
    uint32_t tick;      // lv_tick at the last (re)post, for the fold window
} NvNote;

// Optional extras for nv_notify_post_ex. Every field may be NULL / false.
typedef struct {
    const char *tag;    // one live note per subject ("store-updates", "wifi", ...)
    const char *app;    // tap action: open this app id...
    const char *page;   // ...on this page
    bool quiet;         // store + badge only: no popup, no sound
} nv_note_opts_t;

// Transient toast only. NULL/empty msg is a no-op.
void nv_toast(nv_note_kind_t kind, const char *msg);

// Popup + store in the notification center (ring, newest first). title may be NULL ("System").
void nv_notify_post(nv_note_kind_t kind, const char *title, const char *msg);
void nv_notify_post_ex(nv_note_kind_t kind, const char *title, const char *msg, const nv_note_opts_t *opts);

// Drop the stored note with this tag (its subject is over: e.g. every update installed).
void nv_notify_remove_tag(const char *tag);

// Tap on a note (shade card or desktop popup): opens its app (closing the shade) and removes the
// note; a note without an action opens the shade. Must not run inside the tapped object's own
// event (it may delete it): defer with lv_async_call.
void nv_notify_activate(uint32_t id);

// Ring access for the SystemUI. index 0 = newest; NULL when out of range.
int nv_notify_count(void);
const NvNote *nv_notify_get(int index);

// Unread tracking for the status-bar badge: posts increment, mark_read zeroes (shade opened).
// mark_read also takes down the desktop popups: the shade now shows the same notes.
int  nv_notify_unread(void);
void nv_notify_mark_read(void);

// Remove all stored notifications (shade "Clear all").
void nv_notify_clear(void);

// Remove one stored notification by its id (shade swipe-to-dismiss). No-op if it's gone already.
void nv_notify_remove(uint32_t id);

// SystemUI hook, fired after every post/clear/mark_read: refresh badge + live shade list.
void nv_notify_set_listener(void (*cb)(void));

// Severity mapping (glyph + theme color) shared with the SystemUI shade renderer.
const char *nv_note_kind_symbol(nv_note_kind_t kind);
lv_color_t  nv_note_kind_color(nv_note_kind_t kind);

#ifdef __cplusplus
}
#endif
