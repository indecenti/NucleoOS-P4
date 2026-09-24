// nv_wasm_wasi — WASI filesystem/stdio/sleep glue for WAMR on FATFS. See nv_wasm_wasi.h.
#include "nv_wasm_wasi.h"

#if CONFIG_WAMR_ENABLE_LIBC_WASI

#include "nv_log.h"
#include "nv_mem_attr.h"

#include "esp_vfs.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/socket.h>   // struct iovec (lwip defines it; newlib's sys/uio.h only declares it)
#include <sys/uio.h>
#include <time.h>
#include <unistd.h>

#ifndef O_DIRECTORY
#define O_DIRECTORY 0x200000
#endif
#ifndef O_NOFOLLOW
#define O_NOFOLLOW 0x100000
#endif
#ifndef AT_REMOVEDIR
#define AT_REMOVEDIR 8
#endif

static const char *TAG = "wasi";

#define WASI_VFS      "/wasi"
#define PATH_CAP      256            // FATFS LFN max is 255
#define DIR_SLOTS     16             // directory descriptors open at once (one run at a time)
#define LFD_OUT       100            // local fds of the stdio sinks (dirs use 0..DIR_SLOTS-1)
#define LFD_ERR       101
#define LFD_NULL      102
#define LFD_IN        103            // console stdin pipe
#define F_NV_DIRSLOT  0x4E57         // private fcntl: dir fd -> its slot index (else EBADF)
#define SLEEP_CHUNK_MS 20            // a sleeping guest re-checks the abort flag this often
#define STDIN_CAP     4096           // console stdin pipe (bytes typed but not read yet)

typedef struct {
    bool  used;
    DIR  *dir;                       // stream from fdopendir(), owned together with the fd
    int   gfd;                       // the global fd that fdopendir() consumed (for closedir)
    char  path[PATH_CAP];            // absolute host path, no trailing '/'
} dslot_t;

NV_PSRAM_BSS static dslot_t s_slot[DIR_SLOTS];
static SemaphoreHandle_t s_mu;       // guards s_slot (closedir may run on any task)
static bool s_vfs_ok;

static nv_wasi_sink_fn s_sink;
static void           *s_sink_ctx;
static TaskHandle_t    s_run_task;   // the worker running a WASI guest (nanosleep abort check)
static volatile bool   s_abort;

// Console stdin: a byte ring the UI fills (nv_wasi_stdin_write) and the guest drains through
// read()/readv() on fd s_in_gfd. Indices only grow (position = index % STDIN_CAP); guarded by
// s_mu. s_in_sem is a doorbell: given on every write/close/abort so a blocked read re-checks.
NV_PSRAM_BSS static char s_in_buf[STDIN_CAP];
static size_t            s_in_r, s_in_w;
static bool              s_in_open;  // a console run owns the pipe
static bool              s_in_eof;   // end of input requested; delivered once as a 0-byte read
static int               s_in_gfd = -1;
static SemaphoreHandle_t s_in_sem;

static inline void lock(void)   { if (s_mu) xSemaphoreTake(s_mu, portMAX_DELAY); }
static inline void unlock(void) { if (s_mu) xSemaphoreGive(s_mu); }

// Copy of a slot's path, taken under the lock. -1/EBADF if lfd is not a live directory slot.
static int slot_path(int lfd, char *out) {
    if (lfd < 0 || lfd >= DIR_SLOTS) { errno = EBADF; return -1; }
    lock();
    const bool ok = s_slot[lfd].used;
    if (ok) memcpy(out, s_slot[lfd].path, PATH_CAP);
    unlock();
    if (!ok) { errno = EBADF; return -1; }
    return 0;
}

// ---- the /wasi VFS -----------------------------------------------------------------------------
// Paths arrive with the "/wasi" prefix stripped, i.e. as the real host path ("/sdcard/...").

static int wv_open(const char *path, int flags, int mode) {
    (void)mode;
    if (!strcmp(path, "/.out"))  return LFD_OUT;
    if (!strcmp(path, "/.err"))  return LFD_ERR;
    if (!strcmp(path, "/.null")) return LFD_NULL;
    if (!strcmp(path, "/.in"))   return LFD_IN;
    if ((flags & O_ACCMODE) != O_RDONLY || (flags & O_CREAT)) { errno = EISDIR; return -1; }
    size_t n = strlen(path);
    while (n > 1 && path[n - 1] == '/') n--;          // "/a/b/" -> "/a/b"
    if (n == 0 || n >= PATH_CAP) { errno = ENAMETOOLONG; return -1; }
    char p[PATH_CAP];
    memcpy(p, path, n);
    p[n] = '\0';
    struct stat st;
    if (stat(p, &st) != 0) return -1;
    if (!S_ISDIR(st.st_mode)) { errno = ENOTDIR; return -1; }
    lock();
    int lfd = -1;
    for (int i = 0; i < DIR_SLOTS; i++) {
        if (s_slot[i].used) continue;
        s_slot[i].used = true;
        s_slot[i].dir  = NULL;
        s_slot[i].gfd  = -1;
        memcpy(s_slot[i].path, p, n + 1);
        lfd = i;
        break;
    }
    unlock();
    if (lfd < 0) errno = EMFILE;
    return lfd;
}

static int wv_close(int fd) {
    if (fd == LFD_OUT || fd == LFD_ERR || fd == LFD_NULL || fd == LFD_IN) return 0;
    if (fd < 0 || fd >= DIR_SLOTS) { errno = EBADF; return -1; }
    lock();
    DIR *d = s_slot[fd].used ? s_slot[fd].dir : NULL;
    const bool was = s_slot[fd].used;
    s_slot[fd].used = false;
    s_slot[fd].dir  = NULL;
    unlock();
    if (!was) { errno = EBADF; return -1; }
    if (d) closedir(d);   // fd closed behind its stream's back: drop the stream too
    return 0;
}

static ssize_t wv_write(int fd, const void *data, size_t size) {
    if (fd != LFD_OUT && fd != LFD_ERR) { errno = EBADF; return -1; }
    nv_wasi_sink_fn sink = s_sink;
    if (sink && size) sink(s_sink_ctx, fd == LFD_OUT ? 1 : 2, (const char *)data, size);
    return (ssize_t)size;
}

// Take up to `size` typed bytes. Blocking: waits for the first byte, end of input or an abort
// (both read as 0 = EOF). Non-blocking: returns 0 when nothing is buffered.
static size_t stdin_take(char *dst, size_t size, bool block) {
    for (;;) {
        lock();
        const size_t avail = s_in_w - s_in_r;
        if (avail) {
            const size_t k = avail < size ? avail : size;
            for (size_t i = 0; i < k; i++) dst[i] = s_in_buf[(s_in_r + i) % STDIN_CAP];
            s_in_r += k;
            unlock();
            return k;
        }
        const bool eof = s_in_eof || !s_in_open;
        if (block && s_in_eof) s_in_eof = false;      // a tty EOF is one-shot (Ctrl-D)
        unlock();
        if (!block || eof || s_abort) return 0;
        xSemaphoreTake(s_in_sem, pdMS_TO_TICKS(SLEEP_CHUNK_MS));
    }
}

static ssize_t wv_read(int fd, void *dst, size_t size) {
    if (fd == LFD_NULL) return 0;                     // non-console stdin is always at EOF
    if (fd == LFD_IN) return size ? (ssize_t)stdin_take((char *)dst, size, true) : 0;
    errno = (fd >= 0 && fd < DIR_SLOTS) ? EISDIR : EBADF;
    return -1;
}

static off_t wv_lseek(int fd, off_t off, int whence) {
    (void)fd; (void)off; (void)whence;
    errno = ESPIPE;
    return -1;
}

static int wv_fstat(int fd, struct stat *st) {
    memset(st, 0, sizeof(*st));
    if (fd == LFD_OUT || fd == LFD_ERR || fd == LFD_NULL || fd == LFD_IN) {
        st->st_mode = S_IFCHR | 0666;                 // a tty to WASI: line-buffered, no seek
        return 0;
    }
    char p[PATH_CAP];
    if (slot_path(fd, p) != 0) return -1;
    st->st_mode = S_IFDIR | 0777;
    return 0;
}

static int wv_fcntl(int fd, int cmd, int arg) {
    (void)arg;
    const bool dir = fd >= 0 && fd < DIR_SLOTS;
    switch (cmd) {
    case F_NV_DIRSLOT: {
        char p[PATH_CAP];
        return (dir && slot_path(fd, p) == 0) ? fd : -1;
    }
    case F_GETFL:
        if (fd == LFD_OUT || fd == LFD_ERR) return O_WRONLY | O_APPEND;
        return O_RDONLY;
    case F_SETFL:
    case F_GETFD:
    case F_SETFD:
        return 0;
    default:
        errno = EINVAL;
        return -1;
    }
}

static int wv_fsync(int fd) { (void)fd; return 0; }

bool nv_wasi_init(void) {
    if (s_vfs_ok) return true;
    if (!s_mu) s_mu = xSemaphoreCreateMutex();
    if (!s_in_sem) s_in_sem = xSemaphoreCreateBinary();
    if (!s_mu || !s_in_sem) return false;
    static const esp_vfs_fs_ops_t ops = {
        .write = wv_write,
        .lseek = wv_lseek,
        .read  = wv_read,
        .open  = wv_open,
        .close = wv_close,
        .fstat = wv_fstat,
        .fcntl = wv_fcntl,
        .fsync = wv_fsync,
    };
    const esp_err_t e = esp_vfs_register_fs(WASI_VFS, &ops, ESP_VFS_FLAG_STATIC, NULL);
    if (e != ESP_OK) {
        NV_LOGE(TAG, "VFS %s register failed: %s", WASI_VFS, esp_err_to_name(e));
        return false;
    }
    s_vfs_ok = true;
    return true;
}

// ---- path helpers -------------------------------------------------------------------------------

// Slot of a directory fd handed out by the VFS (the global fd, as WAMR holds it). -1 if not ours.
static int dir_slot_of(int gfd) {
    if (gfd < 0) return -1;
    const int saved = errno;
    const int s = fcntl(gfd, F_NV_DIRSLOT, 0);
    errno = saved;
    return (s >= 0 && s < DIR_SLOTS) ? s : -1;
}

// base + "/" + rel, normalised: empty and "." components dropped, ".." and absolute paths refused
// (libc-wasi already resolved them against the sandbox; this is defence in depth).
static int join_path(const char *base, const char *rel, char *out, size_t n) {
    if (!rel || rel[0] == '/') { errno = EPERM; return -1; }
    size_t w = (size_t)snprintf(out, n, "%s", base);
    if (w >= n) { errno = ENAMETOOLONG; return -1; }
    const char *p = rel;
    while (*p) {
        while (*p == '/') p++;
        const char *s = p;
        while (*p && *p != '/') p++;
        const size_t len = (size_t)(p - s);
        if (len == 0 || (len == 1 && s[0] == '.')) continue;
        if (len == 2 && s[0] == '.' && s[1] == '.') { errno = EPERM; return -1; }
        if (w + 1 + len >= n) { errno = ENAMETOOLONG; return -1; }
        out[w++] = '/';
        memcpy(out + w, s, len);
        w += len;
        out[w] = '\0';
    }
    return 0;
}

static int resolve_at(int dirfd, const char *rel, char *out) {
    const int s = dir_slot_of(dirfd);
    if (s < 0) { errno = EBADF; return -1; }
    char base[PATH_CAP];
    if (slot_path(s, base) != 0) return -1;
    return join_path(base, rel, out, PATH_CAP);
}

// ---- linker --wrap replacements (see CMakeLists.txt) -------------------------------------------
// Every one falls through to the original (the WAMR stub or newlib) for descriptors we don't own.

int  __real_openat(int fd, const char *path, int flags, ...);
int  __real_fstatat(int fd, const char *path, struct stat *st, int flag);
int  __real_mkdirat(int fd, const char *path, mode_t mode);
int  __real_unlinkat(int fd, const char *path, int flag);
int  __real_renameat(int ofd, const char *from, int nfd, const char *to);
DIR *__real_fdopendir(int fd);
int  __real_closedir(DIR *d);
ssize_t __real_readv(int fd, const struct iovec *iov, int iovcnt);

// WAMR's ESP-IDF readv() keeps calling read() until every iovec is full, which on a terminal
// means "until the user has typed a whole buffer". The console stdin returns what is there
// instead, like a tty: block for the first byte, then take whatever else is already buffered.
ssize_t __wrap_readv(int fd, const struct iovec *iov, int iovcnt) {
    if (fd < 0 || fd != s_in_gfd) return __real_readv(fd, iov, iovcnt);
    ssize_t total = 0;
    for (int i = 0; i < iovcnt; i++) {
        if (!iov[i].iov_len) continue;
        const size_t k = stdin_take((char *)iov[i].iov_base, iov[i].iov_len, total == 0);
        total += (ssize_t)k;
        if (k < iov[i].iov_len) break;
    }
    return total;
}

int __wrap_openat(int dirfd, const char *path, int flags, ...) {
    int mode = 0;
    if (flags & O_CREAT) {
        va_list ap;
        va_start(ap, flags);
        mode = va_arg(ap, int);
        va_end(ap);
    }
    if (dir_slot_of(dirfd) < 0) return __real_openat(dirfd, path, flags, mode);

    char full[PATH_CAP];
    if (resolve_at(dirfd, path, full) != 0) return -1;
    struct stat st;
    const bool exists = stat(full, &st) == 0;
    const bool is_dir = exists && S_ISDIR(st.st_mode);
    if ((flags & O_DIRECTORY) && exists && !is_dir) { errno = ENOTDIR; return -1; }
    if (is_dir) {
        if ((flags & O_ACCMODE) != O_RDONLY) { errno = EISDIR; return -1; }
        if ((flags & (O_CREAT | O_EXCL)) == (O_CREAT | O_EXCL)) { errno = EEXIST; return -1; }
        char vpath[PATH_CAP + sizeof(WASI_VFS)];
        snprintf(vpath, sizeof vpath, WASI_VFS "%s", full);
        return open(vpath, O_RDONLY);
    }
    if (flags & O_DIRECTORY) { errno = ENOENT; return -1; }   // O_DIRECTORY never creates
    return open(full, flags & ~(O_DIRECTORY | O_NOFOLLOW | O_NONBLOCK), mode);
}

int __wrap_fstatat(int dirfd, const char *path, struct stat *st, int flag) {
    if (dir_slot_of(dirfd) < 0) return __real_fstatat(dirfd, path, st, flag);
    char full[PATH_CAP];
    if (resolve_at(dirfd, path, full) != 0) return -1;
    return stat(full, st);
}

int __wrap_mkdirat(int dirfd, const char *path, mode_t mode) {
    if (dir_slot_of(dirfd) < 0) return __real_mkdirat(dirfd, path, mode);
    char full[PATH_CAP];
    if (resolve_at(dirfd, path, full) != 0) return -1;
    return mkdir(full, mode);
}

int __wrap_unlinkat(int dirfd, const char *path, int flag) {
    if (dir_slot_of(dirfd) < 0) return __real_unlinkat(dirfd, path, flag);
    char full[PATH_CAP];
    if (resolve_at(dirfd, path, full) != 0) return -1;
    struct stat st;
    if (stat(full, &st) != 0) return -1;
    // FATFS f_unlink() also removes empty directories: enforce POSIX file-vs-dir semantics here.
    if (flag & AT_REMOVEDIR) {
        if (!S_ISDIR(st.st_mode)) { errno = ENOTDIR; return -1; }
        return rmdir(full);
    }
    if (S_ISDIR(st.st_mode)) { errno = EISDIR; return -1; }
    return unlink(full);
}

int __wrap_renameat(int ofd, const char *from, int nfd, const char *to) {
    if (dir_slot_of(ofd) < 0 || dir_slot_of(nfd) < 0) return __real_renameat(ofd, from, nfd, to);
    char a[PATH_CAP], b[PATH_CAP];
    if (resolve_at(ofd, from, a) != 0 || resolve_at(nfd, to, b) != 0) return -1;
    struct stat sa, sb;
    if (stat(a, &sa) != 0) return -1;
    // POSIX rename replaces the target; FATFS refuses with EEXIST. Emulate (not atomic).
    if (stat(b, &sb) == 0) {
        if (S_ISDIR(sa.st_mode) != S_ISDIR(sb.st_mode)) {
            errno = S_ISDIR(sb.st_mode) ? EISDIR : ENOTDIR;
            return -1;
        }
        if (S_ISDIR(sb.st_mode) ? rmdir(b) != 0 : unlink(b) != 0) return -1;
    }
    return rename(a, b);
}

DIR *__wrap_fdopendir(int gfd) {
    const int s = dir_slot_of(gfd);
    if (s < 0) return __real_fdopendir(gfd);
    char p[PATH_CAP];
    if (slot_path(s, p) != 0) return NULL;
    DIR *d = opendir(p);
    if (!d) return NULL;
    // As in POSIX, the stream now owns the descriptor: closedir() closes both.
    lock();
    const bool ok = s_slot[s].used && !s_slot[s].dir;
    if (ok) {
        s_slot[s].dir = d;
        s_slot[s].gfd = gfd;
    }
    unlock();
    if (!ok) {
        closedir(d);
        errno = EBADF;
        return NULL;
    }
    return d;
}

int __wrap_closedir(DIR *d) {
    int gfd = -1;
    if (d && s_mu) {
        lock();
        for (int i = 0; i < DIR_SLOTS; i++) {
            if (s_slot[i].used && s_slot[i].dir == d) {
                s_slot[i].dir = NULL;    // wv_close must not close the stream a second time
                gfd = s_slot[i].gfd;
                break;
            }
        }
        unlock();
    }
    const int rc = __real_closedir(d);
    if (gfd >= 0) close(gfd);
    return rc;
}

// The WAMR platform's nanosleep is an ENOSYS stub, and it wins the link for the whole firmware.
// This one really sleeps, and a WASI guest (poll_oneoff -> nanosleep) sleeps in short chunks so
// nv_wasi_abort() gets it out within SLEEP_CHUNK_MS.
int __wrap_nanosleep(const struct timespec *req, struct timespec *rem) {
    if (!req || req->tv_sec < 0 || req->tv_nsec < 0 || req->tv_nsec >= 1000000000L) {
        errno = EINVAL;
        return -1;
    }
    const bool guest = s_run_task && xTaskGetCurrentTaskHandle() == s_run_task;
    const int64_t end = esp_timer_get_time() + (int64_t)req->tv_sec * 1000000 + req->tv_nsec / 1000;
    for (;;) {
        if (guest && s_abort) break;
        const int64_t left_us = end - esp_timer_get_time();
        if (left_us <= 0) break;
        TickType_t t = pdMS_TO_TICKS((left_us + 999) / 1000);
        if (t == 0) t = 1;
        if (guest && t > pdMS_TO_TICKS(SLEEP_CHUNK_MS)) t = pdMS_TO_TICKS(SLEEP_CHUNK_MS);
        vTaskDelay(t);
    }
    if (rem) { rem->tv_sec = 0; rem->tv_nsec = 0; }
    return 0;
}

// libc-wasi (posix.c path_put) calls this platform hook, which WAMR 2.4.3 implements for the
// POSIX platforms but not for esp-idf. Descriptors are plain ints here.
bool os_compare_file_handle(int handle1, int handle2) { return handle1 == handle2; }

// ---- per-run API ------------------------------------------------------------------------------

bool nv_wasi_module_uses_wasi(wasm_module_t module) {
    const int32_t n = wasm_runtime_get_import_count(module);
    for (int32_t i = 0; i < n; i++) {
        wasm_import_t imp;
        memset(&imp, 0, sizeof imp);
        wasm_runtime_get_import_type(module, i, &imp);
        if (imp.module_name && !strcmp(imp.module_name, "wasi_snapshot_preview1")) return true;
    }
    return false;
}

static bool app_id_ok(const char *id) {
    if (!id || !id[0] || strlen(id) > 32) return false;
    for (const char *c = id; *c; c++) {
        const bool ok = (*c >= 'a' && *c <= 'z') || (*c >= 'A' && *c <= 'Z') ||
                        (*c >= '0' && *c <= '9') || *c == '_' || *c == '-';
        if (!ok) return false;
    }
    return true;
}

static void set_err(char *err, size_t n, const char *msg) {
    NV_LOGE(TAG, "%s", msg);
    if (err && n) snprintf(err, n, "%s", msg);
}

// Split `line` into st->args (NUL-separated) after argv[0]. Blanks separate words; "double" or
// 'single' quotes group them (no escapes). Returns argc, or -1 when it doesn't fit.
static int split_args(nv_wasi_run_t *st, const char *app_id, const char *line) {
    char *w = st->args;
    char *const end = st->args + sizeof st->args;
    int argc = 0;
    const int n0 = snprintf(w, (size_t)(end - w), "%s", app_id);
    st->argv[argc++] = w;
    w += n0 + 1;
    const char *p = line ? line : "";
    for (;;) {
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) break;
        if (argc >= NV_WASI_ARGV_MAX) return -1;
        st->argv[argc++] = w;
        while (*p && *p != ' ' && *p != '\t') {
            if (*p == '"' || *p == '\'') {
                const char q = *p++;
                while (*p && *p != q) {
                    if (w >= end - 1) return -1;
                    *w++ = *p++;
                }
                if (*p) p++;
            } else {
                if (w >= end - 1) return -1;
                *w++ = *p++;
            }
        }
        if (w >= end) return -1;
        *w++ = '\0';
    }
    return argc;
}

static bool ensure_dir(const char *dir) {
    struct stat sb;
    return stat(dir, &sb) == 0 || mkdir(dir, 0777) == 0;
}

bool nv_wasi_prepare(nv_wasi_run_t *st, wasm_module_t module, const nv_wasi_opts_t *o,
                     nv_wasi_sink_fn sink, void *sink_ctx, char *err, size_t err_n) {
    memset(st, 0, sizeof(*st));
    st->fd_in = st->fd_out = st->fd_err = -1;
    if (!s_vfs_ok && !nv_wasi_init()) { set_err(err, err_n, "WASI VFS unavailable"); return false; }
    if (!o || !app_id_ok(o->app_id)) { set_err(err, err_n, "WASI: bad app id"); return false; }
    const int argc = split_args(st, o->app_id, o->args);
    if (argc < 0) { set_err(err, err_n, "WASI: command line too long"); return false; }

    st->fd_in  = open(o->console ? WASI_VFS "/.in" : WASI_VFS "/.null", O_RDONLY);
    st->fd_out = open(WASI_VFS "/.out", O_WRONLY);
    st->fd_err = open(WASI_VFS "/.err", O_WRONLY);
    if (st->fd_in < 0 || st->fd_out < 0 || st->fd_err < 0) {
        nv_wasi_finish(st);
        set_err(err, err_n, "WASI: stdio open failed");
        return false;
    }

    // "/" is the shared workspace with "home", else the private folder with "fs"; with both,
    // the private folder moves to "/appdata" (the longest preopen prefix wins in wasi-libc).
    char data[80];
    snprintf(data, sizeof data, "/sdcard/apps/%s/data", o->app_id);
    if ((o->allow_fs && !ensure_dir(data)) || (o->allow_home && !ensure_dir(NV_WASI_HOME))) {
        nv_wasi_finish(st);
        set_err(err, err_n, "WASI: cannot create the app's folders");
        return false;
    }
    uint32_t nmap = 0;
    if (o->allow_home) {
        snprintf(st->map0, sizeof st->map0, "/::" WASI_VFS "%s", NV_WASI_HOME);
        st->map[nmap++] = st->map0;
        if (o->allow_fs) {
            snprintf(st->map1, sizeof st->map1, "/appdata::" WASI_VFS "%s", data);
            st->map[nmap++] = st->map1;
        }
    } else if (o->allow_fs) {
        snprintf(st->map0, sizeof st->map0, "/::" WASI_VFS "%s", data);
        st->map[nmap++] = st->map0;
    }
    snprintf(st->env0, sizeof st->env0, "NUCLEO_APP=%s", o->app_id);
    st->env[0] = st->env0;
    st->env[1] = "HOME=/";
    st->env[2] = "TERM=dumb";   // plain text: the terminal renders no escape sequences

    wasm_runtime_set_wasi_args_ex(module, NULL, 0, nmap ? st->map : NULL, nmap, st->env, 3,
                                  st->argv, (uint32_t)argc, st->fd_in, st->fd_out, st->fd_err);
    s_sink     = sink;
    s_sink_ctx = sink_ctx;
    s_abort    = false;
    s_run_task = xTaskGetCurrentTaskHandle();
    if (o->console) {
        lock();
        s_in_r = s_in_w = 0;
        s_in_eof  = false;
        s_in_open = true;
        s_in_gfd  = st->fd_in;
        unlock();
        xSemaphoreTake(s_in_sem, 0);   // drop a stale doorbell from the previous run
    }
    st->active = true;
    return true;
}

void nv_wasi_finish(nv_wasi_run_t *st) {
    if (!st) return;
    if (st->active) {
        s_run_task = NULL;
        s_sink     = NULL;
        s_sink_ctx = NULL;
        st->active = false;
    }
    if (st->fd_in >= 0 && st->fd_in == s_in_gfd) {
        lock();
        s_in_open = false;
        s_in_gfd  = -1;
        unlock();
    }
    if (st->fd_in >= 0)  close(st->fd_in);
    if (st->fd_out >= 0) close(st->fd_out);
    if (st->fd_err >= 0) close(st->fd_err);
    st->fd_in = st->fd_out = st->fd_err = -1;

    // deinstantiate closed every guest descriptor; anything still here leaked past WAMR.
    int leaked = 0;
    lock();
    for (int i = 0; i < DIR_SLOTS; i++) if (s_slot[i].used) leaked++;
    unlock();
    if (leaked) NV_LOGW(TAG, "%d directory descriptor(s) still open after the run", leaked);
}

size_t nv_wasi_stdin_write(const char *data, size_t n) {
    if (!s_mu || !data || !n) return 0;
    lock();
    size_t k = 0;
    if (s_in_open) {
        const size_t room = STDIN_CAP - (s_in_w - s_in_r);
        k = n < room ? n : room;
        for (size_t i = 0; i < k; i++) s_in_buf[(s_in_w + i) % STDIN_CAP] = data[i];
        s_in_w += k;
    }
    unlock();
    if (k) xSemaphoreGive(s_in_sem);
    return k;
}

void nv_wasi_stdin_close(void) {
    if (!s_mu) return;
    lock();
    if (s_in_open) s_in_eof = true;
    unlock();
    if (s_in_sem) xSemaphoreGive(s_in_sem);
}

void nv_wasi_abort(void) {
    s_abort = true;
    if (s_in_sem) xSemaphoreGive(s_in_sem);   // a guest blocked on stdin sees EOF now
}

bool nv_wasi_exited(wasm_module_inst_t inst, uint32_t *code) {
    const char *ex = wasm_runtime_get_exception(inst);
    if (!ex || !strstr(ex, "wasi proc exit")) return false;
    if (code) *code = wasm_runtime_get_wasi_exit_code(inst);
    return true;
}

#endif  // CONFIG_WAMR_ENABLE_LIBC_WASI
