// nv_anima_system — live ANIMA_ACT_SYSTEM value resolver, shared by every ANIMA caller.
// The engine (nv_anima) answers system intents with a reply TEMPLATE carrying a {value}
// placeholder; only the OS layer knows the live value (clock, SD, Wi-Fi, registry...). This is
// the single place that fills it, so the native chat app and the web REST handler can't drift.
#pragma once
#include <stddef.h>
#include <stdbool.h>

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

// "Apro calc." -> "Apro Calcolatrice.": the engine only knows app IDS; swap in the launcher's
// (translated) display name in place. No-op when the app/id isn't in the reply.
void nv_anima_pretty_launch(char *reply, size_t cap, const char *id);

#ifdef __cplusplus
}
#endif
