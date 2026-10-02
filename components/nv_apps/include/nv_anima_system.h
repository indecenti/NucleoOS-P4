// nv_anima_system — live ANIMA_ACT_SYSTEM value resolver, shared by every ANIMA caller.
// The engine (nv_anima) answers system intents with a reply TEMPLATE carrying a {value}
// placeholder; only the OS layer knows the live value (clock, SD, Wi-Fi, registry...). This is
// the single place that fills it, so the native chat app and the web REST handler can't drift.
#pragma once
#include <stddef.h>
#include <stdbool.h>
#include "nucleo_anima.h"   // anima_result_t

#ifdef __cplusplus
extern "C" {
#endif

// Resolve the live value for a SYSTEM key ("time", "date", "storage", "capabilities", ...).
// Returns false for unknown keys (out gets a localized "unavailable").
bool nv_anima_system_value(const char *key, bool en, char *out, size_t cap);

// Splice the resolved value into the engine's reply template: replaces {value} in `tmpl`
// (or copies tmpl verbatim when there is no placeholder). Always writes `out`.
void nv_anima_system_reply(const char *key, const char *tmpl, bool en, char *out, size_t cap);

// Execute an ANIMA_ACT_TOOL proposal against the OS (the engine only PROPOSES typed actions —
// "set_volume"/"set_brightness" with arg "70" | "+10" | "-10"; see tool_setting in the engine).
// Returns true when the tool was recognized and applied. Callable from any task (no LVGL calls).
bool nv_anima_os_exec(const char *intent, const char *arg);

// Run a TOOL result for real and say what happened. Covers every tool the engine proposes:
// set_volume / set_brightness / open_file (as nv_anima_os_exec), add_event (an event in the Calendar
// app's /system/config/calendar.json), create_file (the composed text on the SD, never overwriting
// an existing file), close_app (stops music playback / closes the foreground app). `note` gets a
// short line for the transcript ("calendario: 2026-10-02 09:00", or why it failed). Call it on the
// task that ran nucleo_anima_query, STILL HOLDING the spine gate: the composed payload
// (nucleo_anima_tool_content) is engine state the next query overwrites. Also tells the engine the
// outcome (nucleo_anima_observe, nucleo_anima_note_file). True when the action really happened.
// A compound request (r->nsteps > 0) runs its extra steps too, in order, whatever the primary action
// is: call it whenever nucleo_anima_has_tool_work(r). The caller still opens a LAUNCH itself.
bool nv_anima_os_run(const anima_result_t *r, bool en, char *note, size_t cap);

// Reminder service: rings the Calendar app's timed events of today when their minute comes (a toast
// stored in the notification center + a chime, unless "do not disturb"). Events already past at
// start-up stay silent. Call once, on the LVGL thread (or under lvgl_port_lock), after nv_ui_start.
void nv_anima_reminders_start(void);
// The same service runs the heartbeat (OpenClaw-style): every "anima.hb" minutes (default 30, 0 off)
// ANIMA reads /data/anima/HEARTBEAT.md with the model and posts a notification only if something needs
// the user. Minutes to the next one, or -1 when off / no HEARTBEAT.md.
int nv_anima_heartbeat_next_min(void);

// ANIMA's channels: the Telegram bot (nucleo_anima_tg_*). Starts the polling task; it idles until the
// channel is configured and enabled. Call once at boot.
void nv_anima_channels_start(void);

// Hands-free ANIMA: starts the wake-word service (nv_wake) and wires its trigger to the ANIMA app —
// chime, open ANIMA, record the question until silence, answer (aloud when a voice is installed).
// Call once at boot after nv_audio_init. Harmless when the build has no detector.
void nv_anima_handsfree_start(void);

// "Apro calc." -> "Apro Calcolatrice.": the engine only knows app IDS; swap in the launcher's
// (translated) display name in place. No-op when the app/id isn't in the reply.
void nv_anima_pretty_launch(char *reply, size_t cap, const char *id);
// The same for every app a result names: the one it opens, the one it closes, and those in a
// compound request's steps ("Chiudo music e apro notes." -> "Chiudo Musica e apro Note.").
void nv_anima_pretty_reply(char *reply, size_t cap, const anima_result_t *r);

#ifdef __cplusplus
}
#endif
