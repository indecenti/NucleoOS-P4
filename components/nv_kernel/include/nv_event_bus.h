// nv_event_bus — NucleoOS Anima lightweight pub/sub IPC.
// Fixed subscriber tables (no heap on the hot path), mutex-protected, synchronous dispatch.
// Subscribers run in the publisher's task context — keep callbacks short and non-blocking.
#pragma once
#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    NV_EV_LOW_MEMORY = 0,   // data: const size_t* free-internal bytes
    NV_EV_SERVICE_STATE,    // data: const nv_service_event_t*
    NV_EV_APP_LIFECYCLE,    // data: app-defined
    NV_EV_SETTINGS_CHANGED, // data: const char* key
    NV_EV_LANG_CHANGED,     // data: const nv_lang_t* new language (may be NULL)
    NV_EV_THEME_CHANGED,    // data: NULL (read nv_theme_get() for the active theme)
    NV_EV_IME_VISIBILITY,   // data: const nv_ime_visibility_t* (on-screen keyboard shown/hidden)
    NV_EV_USB_DISPLAY,      // data: const nv_usb_display_ev_t* (nv_usb.h — PC display link state)
    NV_EV__COUNT
} nv_event_t;

// Payload for NV_EV_IME_VISIBILITY. `height` is the docked pixel height of the on-screen
// keyboard while visible, 0 when hidden. Apps/overlays subscribe to reflow around it (e.g.
// lift a floating button clear of the keyboard).
typedef struct {
    bool    visible;
    int32_t height;
} nv_ime_visibility_t;

typedef void (*nv_event_cb_t)(nv_event_t ev, const void *data, void *user);

void nv_event_init(void);
// Returns false if the event is invalid or the table for it is full.
bool nv_event_subscribe(nv_event_t ev, nv_event_cb_t cb, void *user);
void nv_event_unsubscribe(nv_event_t ev, nv_event_cb_t cb, void *user);
// Synchronously dispatch to all subscribers of `ev`.
void nv_event_publish(nv_event_t ev, const void *data);

#ifdef __cplusplus
}
#endif
