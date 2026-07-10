// Wedge Test — deliberate hostile guest for the THREAD_MGR kill path. NOT a game: it draws a
// 3-2-1 countdown (one second per digit via present pacing) and then enters while(1){} WITHOUT
// ever calling nv_gfx_present() again — the exact wedge that used to freeze the WASM engine
// until reboot. Expected behavior with the fix:
//   ~8 s after the countdown ends, the gv_poll heartbeat watchdog fires nv_wasm_exec_abort();
//   THREAD_MGR terminate interrupts the interpreter (suspend flag checked at every br), the run
//   collects with "terminated by user", a toast reports the kill, and the engine is IDLE again:
//   launching any other WASM app right away must work (no "busy", no reboot).
// Keep this on the SD for regression testing; it is harmless to run.
#include "nucleo_sdk.h"

void run(void) {
    int W = nv_gfx_width(), H = nv_gfx_height();
    for (int s = 3; s >= 1; s--) {
        for (int f = 0; f < 60; f++) {                 // ~1 s per digit at the 60 Hz pacing
            nv_gfx_clear(NV_RGB(20, 24, 40));
            nv_gfx_text(W / 2 - 60, H / 2 - 30, "WEDGE IN", NV_RGB(255, 200, 60), 2);
            char d[2] = {(char)('0' + s), 0};
            nv_gfx_text(W / 2 - 6, H / 2 + 4, d, NV_RGB(255, 80, 80), 3);
            if (!nv_gfx_present()) return;             // still cooperative during the countdown
        }
    }
    for (;;) { /* hostile: never presents, never returns, never calls an import */ }
}
