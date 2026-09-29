// files_ops — see files_ops.h.
#include "files_ops.h"

#include "nv_i18n.h"
#include "nv_log.h"
#include "nv_mem_attr.h"   // NV_PSRAM_BSS
#include "nv_notify.h"
#include "nv_sd.h"
#include "nv_usb_storage.h"

#include "esp_heap_caps.h"
#include "esp_lvgl_port.h"   // lvgl_port_lock: the end-of-job notification is posted cross-thread

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#include <utime.h>

static const char *TAG = "fileops";

namespace {

constexpr size_t kPathMax = 512;
constexpr size_t kBuf     = 64 * 1024;   // copy chunk (PSRAM)

struct Job {
    FopKind kind;
    char src[kPathMax];
    char dst_dir[kPathMax];
};

QueueHandle_t      s_q;
SemaphoreHandle_t  s_mtx;
std::atomic<bool>  s_busy{false};
std::atomic<bool>  s_cancel{false};
std::atomic<uint32_t> s_gen{1};
FopKind  s_kind = FOP_COPY;
uint64_t s_total = 0, s_done = 0;
char     s_cur[64];
uint8_t *s_buf = nullptr;

// ---- volumes: "/sdcard/..." or "/usbN/..." -> the session that keeps it mounted
enum { VOL_NONE = -2, VOL_SD = -1 };

int vol_of(const char *p) {
    const int u = nv_usb_storage_slot_of(p);
    if (u >= 0) return u;
    if (!strncmp(p, "/sdcard", 7) && (p[7] == 0 || p[7] == '/')) return VOL_SD;
    return VOL_NONE;
}
bool vol_begin(int v) { return v == VOL_SD ? nv_sd_session_begin() : nv_usb_storage_session_begin(v); }
void vol_end(int v)   { if (v == VOL_SD) nv_sd_session_end(); else nv_usb_storage_session_end(v); }

void set_current(const char *path) {
    const char *b = strrchr(path, '/');
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    snprintf(s_cur, sizeof s_cur, "%s", b ? b + 1 : path);
    xSemaphoreGive(s_mtx);
}

bool is_dir(const char *p) {
    struct stat st;
    return stat(p, &st) == 0 && S_ISDIR(st.st_mode);
}

// Sum of file sizes under p (progress denominator).
uint64_t tree_bytes(char *p, size_t cap) {
    struct stat st;
    if (stat(p, &st) != 0) return 0;
    if (!S_ISDIR(st.st_mode)) return (uint64_t)st.st_size;
    uint64_t sum = 0;
    DIR *d = opendir(p);
    if (!d) return 0;
    const size_t base = strlen(p);
    while (struct dirent *e = readdir(d)) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        if (base + 1 + strlen(e->d_name) >= cap) continue;
        snprintf(p + base, cap - base, "/%s", e->d_name);
        sum += tree_bytes(p, cap);
        p[base] = 0;
    }
    closedir(d);
    return sum;
}

// dst_dir/name, or "name (2).ext"... when taken. False if nothing fits.
bool unique_target(const char *dst_dir, const char *name, char *out, size_t cap) {
    struct stat st;
    if ((size_t)snprintf(out, cap, "%s/%s", dst_dir, name) >= cap) return false;
    if (stat(out, &st) != 0) return true;
    const char *dot = strrchr(name, '.');
    const int stem = (dot && dot != name) ? (int)(dot - name) : (int)strlen(name);
    for (int n = 2; n < 100; n++) {
        if ((size_t)snprintf(out, cap, "%s/%.*s (%d)%s", dst_dir, stem, name, n, dot && dot != name ? dot : "") >= cap)
            return false;
        if (stat(out, &st) != 0) return true;
    }
    return false;
}

bool copy_file(const char *src, const char *dst) {
    set_current(src);
    FILE *in = nv_sd_fopen(src, "rb");
    if (!in) return false;
    FILE *out = nv_sd_fopen(dst, "wb");
    if (!out) { nv_sd_fclose(in); return false; }
    bool ok = true;
    for (;;) {
        if (s_cancel) { ok = false; break; }
        const size_t n = fread(s_buf, 1, kBuf, in);
        if (n && fwrite(s_buf, 1, n, out) != n) { ok = false; break; }
        s_done += n;
        if (n < kBuf) { ok = !ferror(in); break; }
    }
    nv_sd_fclose(in);
    if (nv_sd_fclose(out) != 0) ok = false;
    if (!ok) { unlink(dst); return false; }
    struct stat st;
    if (stat(src, &st) == 0) {   // keep the original date (photos sort by it)
        struct utimbuf ut = {st.st_atime, st.st_mtime};
        utime(dst, &ut);
    }
    return true;
}

// Copy file or folder `src` to exact path `dst` (must not exist). Scratch buffers are per level.
bool copy_tree(const char *src, const char *dst) {
    if (s_cancel) return false;
    if (!is_dir(src)) return copy_file(src, dst);
    if (mkdir(dst, 0775) != 0) return false;
    DIR *d = opendir(src);
    if (!d) return false;
    char *a = (char *)malloc(kPathMax), *b = (char *)malloc(kPathMax);
    bool ok = a && b;
    while (ok) {
        struct dirent *e = readdir(d);
        if (!e) break;
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        if ((size_t)snprintf(a, kPathMax, "%s/%s", src, e->d_name) >= kPathMax ||
            (size_t)snprintf(b, kPathMax, "%s/%s", dst, e->d_name) >= kPathMax) { ok = false; break; }
        ok = copy_tree(a, b);
    }
    closedir(d);
    free(a);
    free(b);
    return ok;
}

bool delete_tree(const char *p) {
    if (s_cancel) return false;
    if (!is_dir(p)) { set_current(p); return unlink(p) == 0; }
    DIR *d = opendir(p);
    if (!d) return false;
    char *a = (char *)malloc(kPathMax);
    bool ok = a != nullptr;
    // Deleting while iterating is fine on FatFs (an entry is only marked free), so one pass works.
    while (ok) {
        struct dirent *e = readdir(d);
        if (!e) break;
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        if ((size_t)snprintf(a, kPathMax, "%s/%s", p, e->d_name) >= kPathMax) { ok = false; break; }
        ok = delete_tree(a);
    }
    closedir(d);
    free(a);
    return ok && rmdir(p) == 0;
}

void notify(nv_note_kind_t kind, nv_str_id_t msg) {
    if (lvgl_port_lock(2000)) {
        nv_notify_post(kind, nv_tr(NV_STR_APP_FILES), nv_tr(msg));
        lvgl_port_unlock();
    }
}

void run(Job &j) {
    const int vs = vol_of(j.src);
    const int vd = j.kind == FOP_DELETE ? vs : vol_of(j.dst_dir);
    bool ok = false;
    const bool have_s = vs != VOL_NONE && vol_begin(vs);
    const bool have_d = vd == vs ? have_s : (vd != VOL_NONE && vol_begin(vd));
    if (have_s && have_d) {
        char *scratch = (char *)malloc(kPathMax);
        if (scratch) {
            snprintf(scratch, kPathMax, "%s", j.src);
            s_total = j.kind == FOP_DELETE ? 0 : tree_bytes(scratch, kPathMax);
            free(scratch);
        }
        if (j.kind == FOP_DELETE) {
            ok = delete_tree(j.src);
        } else {
            const char *name = strrchr(j.src, '/');
            char *dst = (char *)malloc(kPathMax);
            if (dst && name && unique_target(j.dst_dir, name + 1, dst, kPathMax)) {
                if (j.kind == FOP_MOVE && vs == vd) {
                    set_current(j.src);
                    ok = rename(j.src, dst) == 0;   // same volume: instant
                } else {
                    ok = copy_tree(j.src, dst);
                    if (!ok) { const bool c = s_cancel; s_cancel = false; delete_tree(dst); s_cancel = c; }
                    else if (j.kind == FOP_MOVE) ok = delete_tree(j.src);
                }
            }
            free(dst);
        }
    }
    if (have_d && vd != vs) vol_end(vd);
    if (have_s) vol_end(vs);

    NV_LOGI(TAG, "%s %s -> %s: %s", j.kind == FOP_COPY ? "copy" : j.kind == FOP_MOVE ? "move" : "delete",
            j.src, j.dst_dir, ok ? "ok" : s_cancel ? "cancelled" : "FAILED");
    if (j.kind != FOP_DELETE) {
        if (ok) notify(NV_NOTE_OK, j.kind == FOP_COPY ? NV_STR_COPY_DONE : NV_STR_MOVE_DONE);
        else    notify(s_cancel ? NV_NOTE_INFO : NV_NOTE_ERROR, s_cancel ? NV_STR_FILEOP_CANCELLED : NV_STR_FILEOP_FAILED);
    } else if (!ok && !s_cancel) {
        notify(NV_NOTE_ERROR, NV_STR_SAVE_FAILED);
    }
}

void task(void *) {
    Job *j = (Job *)heap_caps_malloc(sizeof(Job), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    for (;;) {
        if (xQueueReceive(s_q, j, portMAX_DELAY) != pdTRUE) continue;
        run(*j);
        s_gen++;
        s_busy = false;
    }
}

bool ensure_started(void) {
    if (s_q) return true;
    s_mtx = xSemaphoreCreateMutex();
    s_buf = (uint8_t *)heap_caps_malloc(kBuf, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    QueueHandle_t q = xQueueCreate(1, sizeof(Job));
    if (!s_mtx || !s_buf || !q) return false;
    s_q = q;
    // Forever task, no internal-flash access (SD/USB only) -> PSRAM stack.
    if (xTaskCreateWithCaps(task, "fileops", 6144, nullptr, 2, nullptr,
                            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        s_q = nullptr;
        return false;
    }
    return true;
}

}  // namespace

FopResult fop_start(FopKind kind, const char *src, const char *dst_dir) {
    if (!src || vol_of(src) == VOL_NONE) return FOP_BAD_ARGS;
    if (kind != FOP_DELETE) {
        if (!dst_dir || vol_of(dst_dir) == VOL_NONE) return FOP_BAD_ARGS;
        const size_t n = strlen(src);
        if (!strncmp(dst_dir, src, n) && (dst_dir[n] == 0 || dst_dir[n] == '/')) return FOP_INTO_SELF;
    }
    if (!ensure_started()) return FOP_BAD_ARGS;
    bool expected = false;
    if (!s_busy.compare_exchange_strong(expected, true)) return FOP_BUSY;
    NV_PSRAM_BSS static Job j;   // copied into the queue; only touched under s_busy
    j.kind = kind;
    snprintf(j.src, sizeof j.src, "%s", src);
    snprintf(j.dst_dir, sizeof j.dst_dir, "%s", dst_dir ? dst_dir : "");
    s_kind = kind;
    s_total = s_done = 0;
    s_cancel = false;
    set_current(src);
    if (xQueueSend(s_q, &j, 0) != pdTRUE) { s_busy = false; return FOP_BUSY; }
    return FOP_OK;
}

void fop_cancel(void) { if (s_busy) s_cancel = true; }

void fop_status(FopStatus *o) {
    o->busy = s_busy;
    o->kind = s_kind;
    const uint64_t t = s_total, d = s_done;
    o->pct = t ? (unsigned)(d * 100 / t) : 0;
    if (o->pct > 100) o->pct = 100;
    o->current[0] = 0;
    if (s_mtx) {
        xSemaphoreTake(s_mtx, portMAX_DELAY);
        snprintf(o->current, sizeof o->current, "%s", s_cur);
        xSemaphoreGive(s_mtx);
    }
}

uint32_t fop_generation(void) { return s_gen.load(); }
