// nv_secondscreen boot glue: transport init + the "bring the app forward" nudge.
#include "ss_internal.h"
#include "nv_ss_links.h"

#include "nv_usb.h"
#include "nv_event_bus.h"
#include "esp_timer.h"

void nv_ss_init(void) {
    static bool done = false;
    if (done) return;
    done = true;
    ss_usb_init();
    ss_cast_init();
    ss_vnc_init();
}

// SystemUI already auto-opens Second Screen when a PC streams over USB with nobody consuming it
// (NV_EV_USB_DISPLAY{streaming_unclaimed}); network senders reuse the same path, so the gating
// (ss_auto, lock screen, another app in front -> notification instead) stays in one place.
void ss_request_foreground(void) {
    if (nv_ss_is_open()) return;   // already in front: the app shows the request itself
    const nv_usb_display_ev_t ev = { .mounted = true, .streaming_unclaimed = true, .network = true };
    nv_event_publish(NV_EV_USB_DISPLAY, &ev);
}

uint32_t ss_now_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }
