// nv_term — remote text control of the Terminal (the /api/term/* web routes).
//
// Everything the shell and its terminal programs print on the screen is also teed, ANSI escapes
// stripped, into a 64 KB PSRAM capture ring addressed by a monotonic byte sequence ("seq"): a
// reader keeps its cursor and asks for what came after it. Lines run through here are echoed on
// the screen like typed commands, so the person at the device sees what a remote tool does.
//
// All functions: any task except the LVGL thread (they take the LVGL port lock or wait on the UI).
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NV_TERM_CAPTURE_BYTES (64 * 1024)   // capture ring size: older output is lost

typedef enum {
    NV_TERM_OK = 0,
    NV_TERM_BUSY,        // a command line is running (run) / nothing reads input (input)
    NV_TERM_CLOSED,      // the Terminal screen is not open
    NV_TERM_UI_BUSY,     // the UI lock could not be taken in time
} nv_term_rc_t;

typedef struct {
    bool     open;       // the Terminal screen is up and its shell running
    bool     idle;       // no command line running, no program running or starting
    bool     reading;    // a terminal program runs and reads the keyboard (send it input)
    int      status;     // exit status of the last finished command line ($?)
    uint32_t jobs;       // bumped each time a command line finishes
    uint64_t seq;        // capture cursor: total bytes captured since boot
} nv_term_state_t;

void nv_term_state(nv_term_state_t *st);

// Open the Terminal app when it is not the foreground app; waits up to timeout_ms for its shell.
bool nv_term_open(uint32_t timeout_ms);

// Echo `line` at the prompt and run it, exactly as if typed. On NV_TERM_OK, *seq0 is the capture
// cursor before its output and *jobs0 the job counter to wait past (done: jobs != *jobs0 && idle).
nv_term_rc_t nv_term_run(const char *line, uint64_t *seq0, uint32_t *jobs0);

// Feed text to the running program's stdin (echoed like typed input); a '\n' is appended when the
// text does not end with one. eof: close its stdin afterwards (^D). NV_TERM_BUSY = no program
// reads the keyboard.
nv_term_rc_t nv_term_input(const char *text, size_t len, bool eof, size_t *written);

// ^C: interrupt the running command line / program. False when nothing was running.
bool nv_term_interrupt(void);

// Copy captured output from cursor `since` into out (up to cap bytes). *next = the cursor after the
// copied bytes; *lost = output between `since` and the oldest byte still in the ring was dropped
// (or `since` is from before a reboot). Returns the byte count.
size_t nv_term_read(uint64_t since, char *out, size_t cap, uint64_t *next, bool *lost);

#ifdef __cplusplus
}
#endif
