// term_sh — the Terminal's shell and its tty contract.
//
// The shell (term_sh.cpp) runs every command line on its own task, so a slow `find` or `cp -r`
// never blocks the UI and ^C can interrupt it. The Terminal screen (terminal_app.cpp) is its tty:
// it echoes typed lines, shows what the shell writes (a byte stream with ANSI colours), and runs
// WASI terminal programs on the shell's behalf (the WASM engine is driven from the LVGL thread).
#pragma once
#include <cstddef>
#include <cstdint>
#include <cstdio>

// ---------------------------------------------------------------- output sinks
// Where a command's stdout / stderr goes: the screen, a pipe buffer, a file or /dev/null.
struct ShBuf {
    char  *p = nullptr;
    size_t n = 0, cap = 0;
    bool   trunc = false;   // hit the buffer limit: the tail was dropped
};
enum ShSinkKind : uint8_t { SH_TTY, SH_BUF, SH_FILE, SH_NULL };
struct ShSink {
    ShSinkKind k = SH_TTY;
    ShBuf *buf = nullptr;
    FILE  *f = nullptr;
};
// Any task. SH_TTY goes through term_tty_write.
void sh_sink_write(const ShSink &s, const char *p, size_t n);

// ---------------------------------------------------------------- shell (called by the Terminal)
// All LVGL-thread calls; the shell itself runs on its own task.
bool sh_start(void);                  // create the shell task (once); false when out of memory
bool sh_busy(void);                   // a command line is running
bool sh_run(const char *line);        // run a line (already echoed); false while busy
void sh_interrupt(void);              // ^C: the running command stops at its next check
uint32_t sh_jobs_done(void);          // bumped each time a command line finishes
// The prompt's working directory, "~" for the home directory ("~/notes", "/usb0").
void sh_prompt_dir(char *out, size_t cap);
// Tab completion of the word ending at `cursor` in `line`: the text to insert goes to `ins`
// (possibly empty); when the word stays ambiguous the candidates go to `list`, '\n' separated,
// directories with a trailing '/'. Returns the number of candidates. Only while not busy.
int sh_complete(const char *line, size_t cursor, char *ins, size_t ins_cap,
                char *list, size_t list_cap);

// ---------------------------------------------------------------- tty (implemented by the Terminal)
// Any task. Blocks while the screen's buffer is full (flow control); drops output once the
// Terminal screen is gone.
void term_tty_write(const char *s, size_t n);
int  term_tty_cols(void);             // terminal width in character cells
int  term_tty_rows(void);             // terminal height in character cells
// Raw keys for full-screen built-ins (edit, less, top): while on, every key the user presses is
// delivered as its xterm byte sequence to term_tty_read (Enter = CR, arrows = ESC [ A ...),
// with no echo and no line editing. term_tty_read returns the bytes read (0 on timeout), -1
// once the screen is gone. The shell turns raw mode off again after every command.
void term_tty_raw(bool on);
int  term_tty_read(char *buf, size_t n, int timeout_ms);
// Run a WASI terminal program and wait for it (shell task only). `in` != nullptr: fed as stdin,
// then end of input; nullptr: the user types its input. `out` nullptr: the screen. Returns the
// exit status: 0 ok, 1 failed, 126 graphical app, 130 interrupted.
int  term_prog_run(const char *id, const char *args, const char *in, size_t in_len,
                   const ShSink *out);
// Run fn(arg) on the LVGL thread and wait (shell task only): for NVS writes and restarts, which
// must not run on the shell's PSRAM stack. Returns false when the screen is gone.
bool term_ui_call(void (*fn)(void *), void *arg);
void term_request_exit(void);         // `exit`: close the Terminal
// Command history (the Terminal owns it).
int         term_hist_count(void);
const char *term_hist_at(int i);
void        term_hist_clear(void);
