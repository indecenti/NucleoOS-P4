// nv_clipboard — see include/nv_clipboard.h.
//
// Model: the history IS the clipboard. Entry 0 is the current content (unless cleared); a new copy
// pushes onto the front and the oldest falls off. Big payloads live in PSRAM: text as a heap copy,
// images as a reference-counted pixel block, so a reader borrowing the image (a paste in progress
// on another task) never sees it freed under it. Only the newest kImgKeep images keep their pixels
// in history; older image entries keep just their file.
#include "nv_clipboard.h"

#include <cstdlib>
#include <cstring>
#include <cstdio>

#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "nv_event_bus.h"
#include "nv_log.h"
#include "nv_mem_attr.h"

namespace {

constexpr const char *TAG = "clip";
constexpr int kImgKeep = 3;                 // images whose pixels stay in history (1.2 MB each at most)
constexpr size_t kHistTextMax = 16 * 1024;  // older text entries are trimmed to this

struct ImgBlock {
    uint16_t *px;
    int w, h;
    int refs;
};

struct Entry {
    nv_clip_kind_t kind;
    uint32_t ts;
    char source[16];
    char *text;                              // TEXT
    ImgBlock *img;                           // IMAGE (may be NULL: file only)
    char file[NV_CLIP_PATH_MAX];             // IMAGE
    char (*files)[NV_CLIP_PATH_MAX];         // FILES
    int nfiles;
    bool cut;
};

SemaphoreHandle_t s_mx;
NV_PSRAM_BSS Entry s_hist[NV_CLIP_HISTORY];   // ~4.7 KB, task context under s_mx: PSRAM
int s_n;                                     // entries in s_hist
bool s_cleared;                              // current content is NONE (history kept)
uint32_t s_seq;

bool lock() {
    if (!s_mx) {
        static StaticSemaphore_t buf;
        s_mx = xSemaphoreCreateMutexStatic(&buf);   // first use may race only before the scheduler runs apps
    }
    return xSemaphoreTake(s_mx, pdMS_TO_TICKS(1000)) == pdTRUE;
}
void unlock() { xSemaphoreGive(s_mx); }

void *ps_alloc(size_t n) { return heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT); }

void img_unref(ImgBlock *b) {
    if (!b || --b->refs > 0) return;
    heap_caps_free(b->px);
    heap_caps_free(b);
}

void entry_free(Entry &e) {
    heap_caps_free(e.text);
    img_unref(e.img);
    heap_caps_free(e.files);
    memset(&e, 0, sizeof e);
}

// Push `e` as the newest entry (takes ownership), drop the oldest, trim older payloads.
void push(Entry &e) {
    if (s_n == NV_CLIP_HISTORY) entry_free(s_hist[--s_n]);
    memmove(&s_hist[1], &s_hist[0], sizeof(Entry) * (size_t)s_n);
    s_hist[0] = e;
    s_n++;
    s_cleared = false;
    int imgs = 0;
    for (int i = 0; i < s_n; i++) {
        Entry &h = s_hist[i];
        if (h.kind == NV_CLIP_IMAGE && h.img && ++imgs > kImgKeep && h.file[0]) { img_unref(h.img); h.img = nullptr; }
        if (i > 0 && h.kind == NV_CLIP_TEXT && h.text && strlen(h.text) > kHistTextMax) h.text[kHistTextMax] = 0;
    }
    s_seq++;
}

void stamp(Entry &e, nv_clip_kind_t k, const char *source) {
    memset(&e, 0, sizeof e);
    e.kind = k;
    e.ts = (uint32_t)(esp_timer_get_time() / 1000000);
    snprintf(e.source, sizeof e.source, "%s", source ? source : "");
}

void publish(nv_clip_kind_t k) {
    nv_clip_change_t ch = {k, s_seq};
    nv_event_publish(NV_EV_CLIPBOARD, &ch);
}

const Entry *current() { return (!s_cleared && s_n) ? &s_hist[0] : nullptr; }

}  // namespace

bool nv_clip_set_text(const char *utf8, const char *source) {
    if (!utf8 || !utf8[0]) return false;
    size_t n = strlen(utf8);
    if (n > NV_CLIP_TEXT_MAX) n = NV_CLIP_TEXT_MAX;
    while (n && ((unsigned char)utf8[n] & 0xC0) == 0x80) n--;   // never cut a UTF-8 sequence
    Entry e;
    stamp(e, NV_CLIP_TEXT, source);
    e.text = (char *)ps_alloc(n + 1);
    if (!e.text) return false;
    memcpy(e.text, utf8, n);
    e.text[n] = 0;
    if (!lock()) { heap_caps_free(e.text); return false; }
    // Copying the same text again only moves nothing: no duplicate at the top of the history.
    if (current() && s_hist[0].kind == NV_CLIP_TEXT && s_hist[0].text && !strcmp(s_hist[0].text, e.text)) {
        unlock(); heap_caps_free(e.text); return true;
    }
    push(e);
    unlock();
    publish(NV_CLIP_TEXT);
    return true;
}

bool nv_clip_set_image(const uint16_t *px, int w, int h, int stride_px, const char *file, const char *source) {
    if ((!px || w <= 0 || h <= 0) && !(file && file[0])) return false;
    Entry e;
    stamp(e, NV_CLIP_IMAGE, source);
    if (file) snprintf(e.file, sizeof e.file, "%s", file);
    if (px && w > 0 && h > 0) {
        if (stride_px <= 0) stride_px = w;
        ImgBlock *b = (ImgBlock *)ps_alloc(sizeof *b);
        uint16_t *copy = (uint16_t *)ps_alloc((size_t)w * h * 2);
        if (!b || !copy) { heap_caps_free(b); heap_caps_free(copy); if (!e.file[0]) return false; }
        else {
            for (int y = 0; y < h; y++) memcpy(copy + (size_t)y * w, px + (size_t)y * stride_px, (size_t)w * 2);
            *b = {copy, w, h, 1};
            e.img = b;
        }
    }
    if (!lock()) { img_unref(e.img); return false; }
    push(e);
    unlock();
    NV_LOGI(TAG, "image %dx%d%s%s", w, h, file && file[0] ? " -> " : "", file ? file : "");
    publish(NV_CLIP_IMAGE);
    return true;
}

bool nv_clip_set_files(const char *const *paths, int n, bool cut, const char *source) {
    if (!paths || n <= 0) return false;
    if (n > NV_CLIP_FILES_MAX) n = NV_CLIP_FILES_MAX;
    Entry e;
    stamp(e, NV_CLIP_FILES, source);
    e.files = (char (*)[NV_CLIP_PATH_MAX])ps_alloc(sizeof *e.files * (size_t)n);
    if (!e.files) return false;
    for (int i = 0; i < n; i++) snprintf(e.files[i], NV_CLIP_PATH_MAX, "%s", paths[i] ? paths[i] : "");
    e.nfiles = n;
    e.cut = cut;
    if (!lock()) { heap_caps_free(e.files); return false; }
    push(e);
    unlock();
    publish(NV_CLIP_FILES);
    return true;
}

void nv_clip_clear(void) {
    if (!lock()) return;
    const bool had = current() != nullptr;
    // A "cut" that was pasted (moved) must not be pasted again: dropping it from history too.
    if (had && s_hist[0].kind == NV_CLIP_FILES) { entry_free(s_hist[0]); memmove(&s_hist[0], &s_hist[1], sizeof(Entry) * (size_t)--s_n); memset(&s_hist[s_n], 0, sizeof(Entry)); }
    s_cleared = true;
    if (had) s_seq++;
    unlock();
    if (had) publish(NV_CLIP_NONE);
}

nv_clip_kind_t nv_clip_kind(void) {
    if (!lock()) return NV_CLIP_NONE;
    const Entry *c = current();
    const nv_clip_kind_t k = c ? c->kind : NV_CLIP_NONE;
    unlock();
    return k;
}

uint32_t nv_clip_seq(void) { return s_seq; }

bool nv_clip_has_text(void) { return nv_clip_kind() == NV_CLIP_TEXT; }

char *nv_clip_get_text(void) {
    if (!lock()) return nullptr;
    const Entry *c = current();
    char *out = nullptr;
    if (c && c->kind == NV_CLIP_TEXT && c->text) {
        const size_t n = strlen(c->text);
        out = (char *)malloc(n + 1);
        if (out) memcpy(out, c->text, n + 1);
    }
    unlock();
    return out;
}

bool nv_clip_image_get(nv_clip_image_t *out) {
    if (!out || !lock()) return false;
    memset(out, 0, sizeof *out);
    const Entry *c = current();
    const bool ok = c && c->kind == NV_CLIP_IMAGE;
    if (ok) {
        if (c->img) { c->img->refs++; out->px = c->img->px; out->w = c->img->w; out->h = c->img->h; out->ref = c->img; }
        snprintf(out->file, sizeof out->file, "%s", c->file);
    }
    unlock();
    return ok;
}

void nv_clip_image_release(const nv_clip_image_t *img) {
    if (!img || !img->ref || !lock()) return;
    img_unref((ImgBlock *)img->ref);          // the last reference frees it, in history or not
    unlock();
}

bool nv_clip_image_file(char *path, size_t n) {
    if (!path || !n || !lock()) return false;
    path[0] = 0;
    const Entry *c = current();
    if (c && c->kind == NV_CLIP_IMAGE) snprintf(path, n, "%s", c->file);
    unlock();
    return path[0] != 0;
}

int nv_clip_get_files(char (*out)[NV_CLIP_PATH_MAX], int max, bool *cut) {
    if (!lock()) return 0;
    const Entry *c = current();
    int n = 0;
    if (c && c->kind == NV_CLIP_FILES) {
        n = c->nfiles < max ? c->nfiles : max;
        if (out) for (int i = 0; i < n; i++) memcpy(out[i], c->files[i], NV_CLIP_PATH_MAX);
        if (cut) *cut = c->cut;
    }
    unlock();
    return n;
}

int nv_clip_history(nv_clip_entry_t *out, int max) {
    if (!out || max <= 0 || !lock()) return 0;
    int n = 0;
    for (int i = 0; i < s_n && n < max; i++, n++) {
        const Entry &e = s_hist[i];
        nv_clip_entry_t &o = out[n];
        o.kind = e.kind;
        o.ts = e.ts;
        snprintf(o.source, sizeof o.source, "%s", e.source);
        if (e.kind == NV_CLIP_TEXT) {
            size_t k = 0;
            for (const char *p = e.text; p && *p && k < sizeof o.preview - 4; p++) o.preview[k++] = (*p == '\n' || *p == '\t') ? ' ' : *p;
            while (k && ((unsigned char)o.preview[k] & 0xC0) == 0x80) k--;
            o.preview[k] = 0;
        } else if (e.kind == NV_CLIP_IMAGE) {
            if (e.img) snprintf(o.preview, sizeof o.preview, "%dx%d", e.img->w, e.img->h);
            else snprintf(o.preview, sizeof o.preview, "%.90s", e.file);
        } else {
            snprintf(o.preview, sizeof o.preview, "%d", e.nfiles);
        }
    }
    unlock();
    return n;
}

bool nv_clip_history_restore(int i) {
    if (!lock()) return false;
    if (i < 0 || i >= s_n) { unlock(); return false; }
    Entry e = s_hist[i];
    memmove(&s_hist[i], &s_hist[i + 1], sizeof(Entry) * (size_t)(s_n - i - 1));
    memmove(&s_hist[1], &s_hist[0], sizeof(Entry) * (size_t)i);
    s_hist[0] = e;
    s_cleared = false;
    s_seq++;
    const nv_clip_kind_t k = e.kind;
    unlock();
    publish(k);
    return true;
}
