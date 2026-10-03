// nv_clipboard — the system clipboard: one store for every app, the shell and the web OS.
//
// Formats: TEXT (UTF-8), IMAGE (RGB565 pixels in PSRAM + an optional encoded file on the SD for
// path-based consumers such as ANIMA or Files) and FILES (paths + copy/cut, the Files app's
// "pending paste"). Setting one format replaces the current content, as on a desktop OS; the last
// kHistory entries stay in a history (Win+V). Every change publishes NV_EV_CLIPBOARD (data: const
// nv_clip_change_t*) — subscribers may run off the LVGL thread: set a flag, act in a UI tick.
//
// Thread-safe (one mutex), no LVGL: callable from the UI thread, httpd and workers alike.
// See docs/SCREENSHOT_CLIPBOARD.md.
#pragma once
#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum { NV_CLIP_NONE = 0, NV_CLIP_TEXT, NV_CLIP_IMAGE, NV_CLIP_FILES } nv_clip_kind_t;

#define NV_CLIP_TEXT_MAX   (256 * 1024)  // a single text item (a log, a file's contents)
#define NV_CLIP_PATH_MAX   192
#define NV_CLIP_FILES_MAX  64
#define NV_CLIP_HISTORY    20

// NV_EV_CLIPBOARD payload.
typedef struct {
    nv_clip_kind_t kind;   // what the clipboard holds now (NONE after a clear)
    uint32_t seq;          // nv_clip_seq() after the change
} nv_clip_change_t;

// ---- write (each replaces the content and becomes the newest history entry) ----
// `source` names the app that copied ("files", "anima", "capture", "web"...), may be NULL.
bool nv_clip_set_text(const char *utf8, const char *source);
// Pixels are copied (RGB565, `stride_px` pixels per row, 0 = w). `file` (optional) is an already
// encoded PNG/JPEG of the same image on the SD; consumers that need a path get it from
// nv_clip_image_file(). Either may be absent, not both.
bool nv_clip_set_image(const uint16_t *px, int w, int h, int stride_px, const char *file, const char *source);
bool nv_clip_set_files(const char *const *paths, int n, bool cut, const char *source);
void nv_clip_clear(void);

// ---- read ----
nv_clip_kind_t nv_clip_kind(void);
uint32_t nv_clip_seq(void);                    // bumps on every change: cheap "did it change?"
bool nv_clip_has_text(void);
// A malloc'd copy of the text (caller free()s), NULL when the clipboard holds no text.
char *nv_clip_get_text(void);

typedef struct {
    uint16_t *px;          // RGB565, tightly packed (stride = w); NULL when only a file is held
    int w, h;
    char file[NV_CLIP_PATH_MAX];   // "" when no encoded file exists
    void *ref;             // internal: the borrowed block (nv_clip_image_release)
} nv_clip_image_t;
// Borrow the current image: the pixels stay valid until nv_clip_image_release(), even if the
// clipboard changes meanwhile (reference counted). False when the clipboard holds no image.
bool nv_clip_image_get(nv_clip_image_t *out);
void nv_clip_image_release(const nv_clip_image_t *img);
// The encoded file of the current image ("" / false when there is none).
bool nv_clip_image_file(char *path, size_t n);

// Files: count (0 when none), and copies of the paths into `out` (up to `max`).
int  nv_clip_get_files(char (*out)[NV_CLIP_PATH_MAX], int max, bool *cut);

// ---- history (newest first) ----
typedef struct {
    nv_clip_kind_t kind;
    uint32_t ts;                   // seconds since boot
    char preview[96];              // text: first chars; image: "WxH"; files: "N files"
    char source[16];
} nv_clip_entry_t;
int  nv_clip_history(nv_clip_entry_t *out, int max);
// Make history entry `i` the current content again (text and image entries that still have their
// data; returns false otherwise).
bool nv_clip_history_restore(int i);

#ifdef __cplusplus
}
#endif
