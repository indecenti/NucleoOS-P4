// term_sh — the Terminal's shell: a small POSIX-flavoured command language and GNU-style core
// utilities, running on its own task with the Terminal screen as its tty (see term_sh.h).
//
// Language: 'single' / "double" quotes and \ escapes, $VAR ${VAR} $? expansion, ~ for the home
// directory, * ? [..] globs, pipes (|), redirections (> >> < 2> 2>&1, /dev/null), lists (; && ||)
// and # comments. NAME=value / export / unset set variables. There is one working directory
// (cd / pwd); relative paths resolve against it. "/" is a virtual directory listing the volumes
// (/sdcard, /usb0..6).
//
// Commands: file utilities (ls cat head tail wc grep sort uniq find tree du df stat mkdir rmdir rm
// cp mv touch xxd basename dirname), shell built-ins (cd pwd echo env export unset history which
// type help man true false sleep clear exit), system (uname hostname whoami date uptime free ps
// dmesg sensors ip i2cdetect usb bl apps open reboot top), network (curl wget ping host), hashes
// (md5sum sha1sum sha256sum) and WASI terminal programs (Lua, SQLite, ...)
// run through the Terminal. Pipeline stages run one after another over in-memory buffers (1 MB
// cap), so any stage — a program too — can read the previous one's output.
//
// Every card / USB access happens inside removal-safe sessions (nv_sd_session_*), held for the
// duration of each command. The shell task's stack is in PSRAM, so anything that writes NVS or
// restarts the chip is handed to the LVGL thread (term_ui_call).
#include "term_sh.h"

#include "nv_sd.h"
#include "nv_usb_storage.h"
#include "nv_wasm.h"
#include "nv_ota.h"
#include "nv_time.h"
#include "nv_wifi.h"
#include "nv_hal.h"
#include "nv_service_mgr.h"
#include "nv_memory_broker.h"
#include "nv_log.h"
#include "nv_config.h"
#include "nv_usb_audio.h"
#include "nv_hid_host.h"
#include "nv_open.h"
#include "nv_sysmon.h"

#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "ping/ping_sock.h"
#include "lwip/netdb.h"
#include "lwip/inet.h"
#include "lwip/ip_addr.h"
#include "mbedtls/md.h"
#include "freertos/semphr.h"

#include "driver/i2c_master.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <atomic>
#include <cctype>
#include <cstdarg>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <dirent.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>
#include <utime.h>

// Paths and names are cut to fixed buffers on purpose (FAT paths are bounded); the truncation
// warnings would flag every one of those snprintf calls.
#pragma GCC diagnostic ignored "-Wformat-truncation"

namespace {

constexpr size_t kPath     = 512;
constexpr size_t kPipeCap  = 1024 * 1024;   // pipe / capture / file-read buffer limit
constexpr size_t kLineCap  = 1024;
constexpr size_t kArena    = 64 * 1024;     // per-line scratch: tokens, words, glob results
constexpr int    kMaxTok   = 192;
constexpr int    kMaxArgs  = 256;
constexpr int    kMaxStage = 8;
constexpr int    kVars     = 32;
constexpr int    kMaxDepth = 32;            // recursion limit (rm -r, cp -r, find, tree, du)
constexpr size_t kCopyBuf  = 32 * 1024;

const char *const kHome = "/sdcard/home";
const char *const kUser = "nucleo";
const char *const kHost = "anima";

struct Var { char name[32]; char val[192]; };

// Shell state, one PSRAM block allocated on the first start.
struct State {
    char cwd[kPath];
    char oldpwd[kPath];
    char line[kLineCap];
    char arena[kArena];
    size_t arena_n;
    Var  vars[kVars];
    int  status;              // $?
    // Tab completion: installed terminal program ids, scanned once.
    char progs[64][32];
    int  nprogs;              // -1 = not scanned yet
};
State *S = nullptr;

TaskHandle_t s_task = nullptr;
std::atomic<bool>     s_busy{false};
std::atomic<bool>     s_cancel{false};
std::atomic<uint32_t> s_done{0};

bool cancelled(void) { return s_cancel.load(); }

// ---------------------------------------------------------------- memory

char *a_alloc(size_t n) {
    n = (n + 3) & ~(size_t)3;
    if (S->arena_n + n > kArena) return nullptr;
    char *p = S->arena + S->arena_n;
    S->arena_n += n;
    return p;
}

char *a_strndup(const char *s, size_t n) {
    char *p = a_alloc(n + 1);
    if (!p) return nullptr;
    memcpy(p, s, n);
    p[n] = '\0';
    return p;
}

void *ps_alloc(size_t n) { return heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT); }
void *ps_realloc(void *p, size_t n) {
    return heap_caps_realloc(p, n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}

bool buf_put(ShBuf &b, const char *s, size_t n) {
    if (b.n + n > kPipeCap) { n = kPipeCap - b.n; b.trunc = true; }
    if (!n) return false;
    if (b.n + n + 1 > b.cap) {
        size_t cap = b.cap ? b.cap : 4096;
        while (cap < b.n + n + 1) cap *= 2;
        if (cap > kPipeCap + 1) cap = kPipeCap + 1;
        char *p = (char *)ps_realloc(b.p, cap);
        if (!p) { b.trunc = true; return false; }
        b.p = p;
        b.cap = cap;
    }
    memcpy(b.p + b.n, s, n);
    b.n += n;
    b.p[b.n] = '\0';
    return true;
}

void buf_free(ShBuf &b) {
    heap_caps_free(b.p);
    b = ShBuf{};
}

// ---------------------------------------------------------------- volumes

// Hold a removal-safe session on every mounted volume while a command runs, so the card or a
// pendrive is never unmounted under an open file.
struct VolsHold {
    bool sd = false;
    bool usb[NV_USB_STOR_SLOTS] = {};
    VolsHold() {
        sd = nv_sd_session_begin();
        for (int i = 0; i < NV_USB_STOR_SLOTS; i++) {
            nv_usb_stor_info_t inf;
            if (nv_usb_storage_get(i, &inf) && inf.state == NV_USB_STOR_MOUNTED)
                usb[i] = nv_usb_storage_session_begin(i);
        }
    }
    ~VolsHold() {
        if (sd) nv_sd_session_end();
        for (int i = 0; i < NV_USB_STOR_SLOTS; i++) if (usb[i]) nv_usb_storage_session_end(i);
    }
};

// Mount points present right now ("sdcard", "usb0", ...) — the virtual root's entries.
int mounts(char out[][8], int max) {
    int n = 0;
    uint64_t t, f;
    if (n < max && nv_sd_info(&t, &f)) snprintf(out[n++], 8, "sdcard");
    for (int i = 0; i < NV_USB_STOR_SLOTS && n < max; i++) {
        nv_usb_stor_info_t inf;
        if (nv_usb_storage_get(i, &inf) && inf.state == NV_USB_STOR_MOUNTED)
            snprintf(out[n++], 8, "%.7s", inf.path + 1);
    }
    return n;
}

bool is_mount_root(const char *p) {
    if (!strcmp(p, "/")) return true;
    if (!strcmp(p, "/sdcard")) return true;
    return !strncmp(p, "/usb", 4) && isdigit((unsigned char)p[4]) && !p[5];
}

// ---------------------------------------------------------------- paths

// Absolute, normalized path of `in` (relative to the working directory, "." and ".." folded).
void resolve(const char *in, char *out, size_t cap) {
    char tmp[kPath];
    if (in[0] == '/') snprintf(tmp, sizeof tmp, "%s", in);
    else snprintf(tmp, sizeof tmp, "%s/%s", S->cwd, in);
    size_t len = 0;
    out[0] = '\0';
    const char *p = tmp;
    while (*p) {
        while (*p == '/') p++;
        if (!*p) break;
        const char *s = p;
        while (*p && *p != '/') p++;
        const size_t n = (size_t)(p - s);
        if (n == 1 && s[0] == '.') continue;
        if (n == 2 && s[0] == '.' && s[1] == '.') {
            while (len > 0 && out[len - 1] != '/') len--;
            if (len > 0) len--;
            out[len] = '\0';
            continue;
        }
        if (len + 1 + n + 1 > cap) break;
        out[len++] = '/';
        memcpy(out + len, s, n);
        len += n;
        out[len] = '\0';
    }
    if (!len) { out[0] = '/'; out[1] = '\0'; }
}

bool is_dir(const char *p) {
    if (!strcmp(p, "/")) return true;
    struct stat st;
    if (stat(p, &st) == 0) return S_ISDIR(st.st_mode);
    DIR *d = opendir(p);   // FAT mount roots have no directory entry to stat
    if (d) { closedir(d); return true; }
    return false;
}

bool exists(const char *p) {
    struct stat st;
    return stat(p, &st) == 0 || is_dir(p);
}

const char *base_name(const char *p) {
    const char *s = strrchr(p, '/');
    return (s && s[1]) ? s + 1 : p;
}

// ---------------------------------------------------------------- variables

const char *var_get(const char *name) {
    static char num[12];
    if (!strcmp(name, "?")) { snprintf(num, sizeof num, "%d", S->status); return num; }
    if (!strcmp(name, "HOME")) return kHome;
    if (!strcmp(name, "PWD")) return S->cwd;
    if (!strcmp(name, "OLDPWD")) return S->oldpwd;
    if (!strcmp(name, "USER") || !strcmp(name, "LOGNAME")) return kUser;
    if (!strcmp(name, "HOSTNAME")) return kHost;
    if (!strcmp(name, "SHELL")) return "/bin/sh";
    if (!strcmp(name, "TERM")) return "xterm-256color";
    if (!strcmp(name, "COLUMNS")) { snprintf(num, sizeof num, "%d", term_tty_cols()); return num; }
    for (const Var &v : S->vars) if (v.name[0] && !strcmp(v.name, name)) return v.val;
    return nullptr;
}

bool var_name_ok(const char *s, size_t n) {
    if (!n || n >= sizeof(Var::name) || !(isalpha((unsigned char)s[0]) || s[0] == '_')) return false;
    for (size_t i = 1; i < n; i++) if (!(isalnum((unsigned char)s[i]) || s[i] == '_')) return false;
    return true;
}

bool var_set(const char *name, const char *val) {
    Var *slot = nullptr;
    for (Var &v : S->vars) {
        if (v.name[0] && !strcmp(v.name, name)) { slot = &v; break; }
        if (!v.name[0] && !slot) slot = &v;
    }
    if (!slot) return false;
    snprintf(slot->name, sizeof slot->name, "%s", name);
    snprintf(slot->val, sizeof slot->val, "%s", val);
    return true;
}

void var_unset(const char *name) {
    for (Var &v : S->vars) if (v.name[0] && !strcmp(v.name, name)) v.name[0] = '\0';
}

// ---------------------------------------------------------------- wildcards

bool wild(const char *p, const char *s, bool icase) {
    auto eq = [icase](char a, char b) {
        return icase ? tolower((unsigned char)a) == tolower((unsigned char)b) : a == b;
    };
    const char *star_p = nullptr, *star_s = nullptr;
    while (*s) {
        if (*p == '*') { star_p = ++p; star_s = s; continue; }
        if (*p == '[') {
            const char *q = p + 1;
            bool neg = (*q == '!' || *q == '^');
            if (neg) q++;
            bool hit = false, first = true;
            while (*q && (*q != ']' || first)) {
                first = false;
                if (q[1] == '-' && q[2] && q[2] != ']') {
                    if ((unsigned char)*s >= (unsigned char)q[0] && (unsigned char)*s <= (unsigned char)q[2]) hit = true;
                    q += 3;
                } else {
                    if (eq(*q, *s)) hit = true;
                    q++;
                }
            }
            if (*q == ']' && hit != neg) { p = q + 1; s++; continue; }
        } else if (*p == '?' || (*p && eq(*p, *s))) { p++; s++; continue; }
        if (!star_p) return false;
        p = star_p;
        s = ++star_s;
    }
    while (*p == '*') p++;
    return !*p;
}

// ---------------------------------------------------------------- output

struct Ctx {
    int    argc = 0;
    char **argv = nullptr;
    const char *in = nullptr;   // stdin bytes (pipe or < file)
    size_t in_len = 0;
    bool   has_in = false;
    ShSink out, err;
};

void wr(const ShSink &s, const char *p, size_t n) { sh_sink_write(s, p, n); }
void wr(const ShSink &s, const char *p) { sh_sink_write(s, p, strlen(p)); }

void vfmt(const ShSink &s, const char *fmt, va_list ap) {
    char b[512];
    const int n = vsnprintf(b, sizeof b, fmt, ap);
    if (n > 0) wr(s, b, (size_t)n < sizeof b ? (size_t)n : sizeof b - 1);
}
void outf(Ctx &c, const char *fmt, ...) {
    va_list ap; va_start(ap, fmt); vfmt(c.out, fmt, ap); va_end(ap);
}
void errf(Ctx &c, const char *fmt, ...) {
    va_list ap; va_start(ap, fmt); vfmt(c.err, fmt, ap); va_end(ap);
}

bool tty(const Ctx &c) { return c.out.k == SH_TTY; }
// SGR colour, only on the screen (like --color=auto).
void sgr(Ctx &c, const char *code) {
    if (!tty(c)) return;
    char b[16];
    const int n = snprintf(b, sizeof b, "\x1b[%sm", code);
    wr(c.out, b, (size_t)n);
}

// GNU dircolors defaults for a directory entry.
const char *ls_color(const char *name, bool dir) {
    if (dir) return "01;34";
    const char *dot = strrchr(name, '.');
    if (!dot) return nullptr;
    static const struct { const char *ext; const char *col; } kCol[] = {
        {"wasm", "01;32"}, {"aot", "01;32"}, {"sh", "01;32"},
        {"zip", "01;31"}, {"tar", "01;31"}, {"gz", "01;31"}, {"tgz", "01;31"}, {"7z", "01;31"},
        {"rar", "01;31"}, {"bz2", "01;31"}, {"xz", "01;31"}, {"bin", "01;31"},
        {"jpg", "01;35"}, {"jpeg", "01;35"}, {"png", "01;35"}, {"gif", "01;35"}, {"bmp", "01;35"},
        {"webp", "01;35"}, {"mp4", "01;35"}, {"avi", "01;35"}, {"mkv", "01;35"}, {"mpg", "01;35"},
        {"mpeg", "01;35"}, {"mov", "01;35"}, {"mjpeg", "01;35"},
        {"mp3", "00;36"}, {"wav", "00;36"}, {"flac", "00;36"}, {"aac", "00;36"}, {"ogg", "00;36"},
        {"m4a", "00;36"},
    };
    for (const auto &k : kCol) if (!strcasecmp(dot + 1, k.ext)) return k.col;
    return nullptr;
}

void put_name(Ctx &c, const char *name, bool dir, bool slash = false) {
    const char *col = ls_color(name, dir);
    if (col) sgr(c, col);
    wr(c.out, name);
    if (col) sgr(c, "0");
    if (slash && dir) wr(c.out, "/");
}

int utf8_len(const char *s) {
    int n = 0;
    for (; *s; s++) n += ((unsigned char)*s & 0xC0) != 0x80;
    return n;
}

void human(uint64_t v, char *out, size_t cap) {
    static const char kU[] = "BKMGT";
    if (v < 1024) { snprintf(out, cap, "%u", (unsigned)v); return; }
    double d = (double)v;
    int u = 0;
    while (d >= 1024.0 && u < 4) { d /= 1024.0; u++; }
    if (d < 10.0) snprintf(out, cap, "%.1f%c", d, kU[u]);
    else snprintf(out, cap, "%.0f%c", d + 0.49, kU[u]);
}

// ---------------------------------------------------------------- files

// Whole file (or the stdin buffer for "-") into a PSRAM buffer.
bool read_all(Ctx &c, const char *arg, ShBuf &b) {
    if (!strcmp(arg, "-")) {
        if (c.has_in) buf_put(b, c.in, c.in_len);
        return true;
    }
    char p[kPath];
    resolve(arg, p, sizeof p);
    if (is_dir(p)) { errf(c, "%s: %s: Is a directory\n", c.argv[0], arg); return false; }
    FILE *f = fopen(p, "rb");
    if (!f) { errf(c, "%s: %s: No such file or directory\n", c.argv[0], arg); return false; }
    char chunk[2048];
    size_t n;
    while (!cancelled() && (n = fread(chunk, 1, sizeof chunk, f)) > 0) {
        buf_put(b, chunk, n);
        if (b.trunc) break;
    }
    fclose(f);
    if (b.trunc) errf(c, "%s: %s: truncated at %u KB\n", c.argv[0], arg, (unsigned)(kPipeCap / 1024));
    return true;
}

// Call fn for every line of buf (without the '\n'). Stops when fn returns false.
template <typename F> void each_line(const char *p, size_t n, F fn) {
    size_t i = 0;
    while (i < n && !cancelled()) {
        size_t j = i;
        while (j < n && p[j] != '\n') j++;
        if (!fn(p + i, j - i)) break;
        i = j + 1;
    }
}

struct Ent {
    char    *name;
    bool     dir;
    uint64_t size;
    time_t   mtime;
};

// Entries of a directory (arena names, PSRAM array the caller frees). all: dot files too.
int read_dir(const char *path, bool all, bool want_stat, Ent **out) {
    *out = nullptr;
    int cap = 64, n = 0;
    Ent *e = (Ent *)ps_alloc(sizeof(Ent) * cap);
    if (!e) return -1;
    if (!strcmp(path, "/")) {
        char m[NV_USB_STOR_SLOTS + 1][8];
        const int k = mounts(m, NV_USB_STOR_SLOTS + 1);
        for (int i = 0; i < k; i++) {
            e[n].name = a_strndup(m[i], strlen(m[i]));
            e[n].dir = true; e[n].size = 0; e[n].mtime = 0;
            if (e[n].name) n++;
        }
        *out = e;
        return n;
    }
    DIR *d = opendir(path);
    if (!d) { heap_caps_free(e); return -1; }
    char full[kPath];
    while (struct dirent *de = readdir(d)) {
        if (cancelled()) break;
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
        if (!all && de->d_name[0] == '.') continue;
        if (n == cap) {
            if (cap >= 4096) break;
            Ent *g = (Ent *)ps_realloc(e, sizeof(Ent) * cap * 2);
            if (!g) break;
            e = g;
            cap *= 2;
        }
        Ent &x = e[n];
        x.name = a_strndup(de->d_name, strlen(de->d_name));
        if (!x.name) break;   // arena full: list what we have
        x.dir = de->d_type == DT_DIR;
        x.size = 0;
        x.mtime = 0;
        if (want_stat) {
            snprintf(full, sizeof full, "%s/%s", strcmp(path, "/") ? path : "", de->d_name);
            struct stat st;
            if (stat(full, &st) == 0) {
                x.dir = S_ISDIR(st.st_mode);
                x.size = (uint64_t)st.st_size;
                x.mtime = st.st_mtime;
            }
        }
        n++;
    }
    closedir(d);
    *out = e;
    return n;
}

int ent_cmp_name(const void *a, const void *b) {
    const char *x = ((const Ent *)a)->name, *y = ((const Ent *)b)->name;
    while (*x == '.') x++;   // GNU order ignores leading dots
    while (*y == '.') y++;
    const int r = strcasecmp(x, y);
    return r ? r : strcmp(((const Ent *)a)->name, ((const Ent *)b)->name);
}

// Names in columns across the terminal (ls, completion lists), filled down each column.
void columns(Ctx &c, Ent *e, int n, bool slash) {
    const int width = term_tty_cols();
    int longest = 1;
    for (int i = 0; i < n; i++) {
        const int l = utf8_len(e[i].name) + (slash && e[i].dir);
        if (l > longest) longest = l;
    }
    const int colw = longest + 2;
    int cols = width / colw;
    if (cols < 1) cols = 1;
    const int rows = (n + cols - 1) / cols;
    for (int r = 0; r < rows && !cancelled(); r++) {
        for (int k = 0; k < cols; k++) {
            const int i = k * rows + r;
            if (i >= n) break;
            put_name(c, e[i].name, e[i].dir, slash);
            if ((k + 1) * rows + r < n) {
                for (int pad = colw - utf8_len(e[i].name) - (slash && e[i].dir); pad > 0; pad--)
                    wr(c.out, " ", 1);
            }
        }
        wr(c.out, "\n", 1);
    }
}

// ---------------------------------------------------------------- regex (grep)
// A compact backtracking matcher: literals, ., [...] (ranges, ^ negation), \d \w \s, \x escapes,
// the * + ? repeats and ^ $ anchors. -F makes every character literal.

enum : uint8_t { RN_LIT, RN_ANY, RN_SET };
enum : uint8_t { RR_ONE, RR_STAR, RR_PLUS, RR_OPT };
struct ReNode { uint8_t kind, rep, ch; uint8_t set[32]; };
struct Re {
    ReNode n[64];
    int  cnt = 0;
    bool bol = false, eol = false, icase = false;
};

void set_add(uint8_t *set, unsigned c) { set[c >> 3] |= (uint8_t)(1u << (c & 7)); }
bool set_has(const uint8_t *set, unsigned c) { return set[c >> 3] & (1u << (c & 7)); }

bool re_compile(Re &re, const char *p, bool icase, bool fixed, const char **err) {
    re = Re{};
    re.icase = icase;
    if (!fixed && *p == '^') { re.bol = true; p++; }
    while (*p) {
        if (!fixed && *p == '$' && !p[1]) { re.eol = true; break; }
        if (re.cnt >= 64) { *err = "pattern too long"; return false; }
        ReNode &n = re.n[re.cnt];
        n = ReNode{};
        if (fixed) { n.kind = RN_LIT; n.ch = (uint8_t)*p++; }
        else if (*p == '.') { n.kind = RN_ANY; p++; }
        else if (*p == '[') {
            p++;
            n.kind = RN_SET;
            bool neg = false, first = true;
            if (*p == '^') { neg = true; p++; }
            while (*p && (*p != ']' || first)) {
                first = false;
                const unsigned a = (uint8_t)*p++;
                if (*p == '-' && p[1] && p[1] != ']') {
                    const unsigned b = (uint8_t)p[1];
                    p += 2;
                    for (unsigned x = a; x <= b; x++) set_add(n.set, x);
                } else {
                    set_add(n.set, a);
                }
            }
            if (*p != ']') { *err = "unmatched [ in pattern"; return false; }
            p++;
            if (neg) for (uint8_t &b : n.set) b = (uint8_t)~b;
        } else if (*p == '\\' && p[1]) {
            p++;
            const char c = *p++;
            if (c == 'd' || c == 'w' || c == 's') {
                n.kind = RN_SET;
                for (unsigned x = 0; x < 256; x++) {
                    const bool in = c == 'd' ? isdigit(x) : c == 'w' ? (isalnum(x) || x == '_') : isspace(x);
                    if (in) set_add(n.set, x);
                }
            } else {
                n.kind = RN_LIT;
                n.ch = (uint8_t)c;
            }
        } else {
            n.kind = RN_LIT;
            n.ch = (uint8_t)*p++;
        }
        if (!fixed) {
            if (*p == '*') { n.rep = RR_STAR; p++; }
            else if (*p == '+') { n.rep = RR_PLUS; p++; }
            else if (*p == '?') { n.rep = RR_OPT; p++; }
        }
        if (icase) {
            if (n.kind == RN_LIT) n.ch = (uint8_t)tolower(n.ch);
            else if (n.kind == RN_SET)
                for (unsigned x = 'a'; x <= 'z'; x++)
                    if (set_has(n.set, x) || set_has(n.set, x - 32)) { set_add(n.set, x); set_add(n.set, x - 32); }
        }
        re.cnt++;
    }
    return true;
}

bool re_node(const Re &re, const ReNode &n, unsigned c) {
    switch (n.kind) {
        case RN_ANY: return c != '\n';
        case RN_LIT: return (re.icase ? (unsigned)tolower(c) : c) == n.ch;
        default:     return set_has(n.set, c);
    }
}

const char *re_here(const Re &re, int i, const char *s, const char *e) {
    if (i == re.cnt) return (!re.eol || s == e) ? s : nullptr;
    const ReNode &n = re.n[i];
    if (n.rep == RR_ONE) {
        if (s < e && re_node(re, n, (uint8_t)*s)) return re_here(re, i + 1, s + 1, e);
        return nullptr;
    }
    if (n.rep == RR_OPT) {
        if (s < e && re_node(re, n, (uint8_t)*s))
            if (const char *r = re_here(re, i + 1, s + 1, e)) return r;
        return re_here(re, i + 1, s, e);
    }
    const char *t = s;
    while (t < e && re_node(re, n, (uint8_t)*t)) t++;
    const char *min = n.rep == RR_PLUS ? s + 1 : s;
    for (;;) {
        if (t < min) return nullptr;
        if (const char *r = re_here(re, i + 1, t, e)) return r;
        if (t == s) return nullptr;
        t--;
    }
}

bool re_search(const Re &re, const char *s, const char *e, const char **ms, const char **me) {
    for (const char *p = s; p <= e; p++) {
        if (const char *r = re_here(re, 0, p, e)) { *ms = p; *me = r; return true; }
        if (re.bol) break;
    }
    return false;
}

// ---------------------------------------------------------------- option parsing

struct Flags {
    uint64_t m = 0;
    bool has(char c) const { return c >= 'A' && c <= 'z' && (m >> (c - 'A')) & 1; }
};

// Leading -abc flags from `allowed`; returns the first operand index, -1 on an unknown flag.
// Letters in `valued` take the next argument (or the rest of the cluster) into *val.
int getflags(Ctx &c, const char *allowed, Flags &f, const char *valued = "", const char **val = nullptr) {
    int i = 1;
    for (; i < c.argc; i++) {
        const char *a = c.argv[i];
        if (a[0] != '-' || !a[1]) break;
        if (!strcmp(a, "--")) { i++; break; }
        if (isdigit((unsigned char)a[1]) && strchr(valued, 'n')) {   // head -5
            if (val) *val = a + 1;
            f.m |= 1ull << ('n' - 'A');
            continue;
        }
        for (const char *p = a + 1; *p; p++) {
            if (!strchr(allowed, *p) || *p < 'A' || *p > 'z') {
                errf(c, "%s: invalid option -- '%c'\nTry 'help %s' for more information.\n",
                     c.argv[0], *p, c.argv[0]);
                return -1;
            }
            f.m |= 1ull << (*p - 'A');
            if (strchr(valued, *p)) {
                const char *v = p[1] ? p + 1 : (i + 1 < c.argc ? c.argv[++i] : nullptr);
                if (!v) { errf(c, "%s: option requires an argument -- '%c'\n", c.argv[0], *p); return -1; }
                if (val) *val = v;
                break;
            }
        }
    }
    return i;
}

// ---------------------------------------------------------------- built-ins: navigation / files

int b_cd(Ctx &c) {
    const char *t = c.argc > 1 ? c.argv[1] : kHome;
    bool print = false;
    if (!strcmp(t, "-")) {
        if (!S->oldpwd[0]) { errf(c, "cd: OLDPWD not set\n"); return 1; }
        t = S->oldpwd;
        print = true;
    }
    char p[kPath];
    resolve(t, p, sizeof p);
    if (!exists(p)) { errf(c, "cd: %s: No such file or directory\n", t); return 1; }
    if (!is_dir(p)) { errf(c, "cd: %s: Not a directory\n", t); return 1; }
    snprintf(S->oldpwd, sizeof S->oldpwd, "%s", S->cwd);
    snprintf(S->cwd, sizeof S->cwd, "%s", p);
    if (print) outf(c, "%s\n", S->cwd);
    return 0;
}

int b_pwd(Ctx &c) { outf(c, "%s\n", S->cwd); return 0; }

void ls_long(Ctx &c, const Ent &e, bool h, int szw) {
    char sz[16], tm[20];
    if (h) human(e.size, sz, sizeof sz);
    else snprintf(sz, sizeof sz, "%llu", (unsigned long long)e.size);
    struct tm lt;
    time_t t = e.mtime;
    localtime_r(&t, &lt);
    const time_t now = time(nullptr);
    if (!e.mtime) snprintf(tm, sizeof tm, "            ");
    else if (now - t < 180L * 24 * 3600 && t <= now + 3600) strftime(tm, sizeof tm, "%b %e %H:%M", &lt);
    else strftime(tm, sizeof tm, "%b %e  %Y", &lt);
    outf(c, "%s 1 %s %s %*s %s ", e.dir ? "drwxr-xr-x" : "-rw-r--r--", kUser, kUser, szw, sz, tm);
    put_name(c, e.name, e.dir);
    wr(c.out, "\n", 1);
}

int ls_sort_t(const void *a, const void *b) {
    const time_t x = ((const Ent *)a)->mtime, y = ((const Ent *)b)->mtime;
    return x < y ? 1 : x > y ? -1 : ent_cmp_name(a, b);
}
int ls_sort_S(const void *a, const void *b) {
    const uint64_t x = ((const Ent *)a)->size, y = ((const Ent *)b)->size;
    return x < y ? 1 : x > y ? -1 : ent_cmp_name(a, b);
}

void ls_print(Ctx &c, Ent *e, int n, const Flags &f) {
    qsort(e, n, sizeof(Ent), f.has('t') ? ls_sort_t : f.has('S') ? ls_sort_S : ent_cmp_name);
    if (f.has('r')) for (int i = 0; i < n / 2; i++) { Ent t = e[i]; e[i] = e[n - 1 - i]; e[n - 1 - i] = t; }
    if (f.has('l')) {
        int szw = 1;
        uint64_t total = 0;
        for (int i = 0; i < n; i++) {
            char sz[16];
            if (f.has('h')) human(e[i].size, sz, sizeof sz);
            else snprintf(sz, sizeof sz, "%llu", (unsigned long long)e[i].size);
            const int l = (int)strlen(sz);
            if (l > szw) szw = l;
            total += (e[i].size + 1023) / 1024;
        }
        outf(c, "total %llu\n", (unsigned long long)total);
        for (int i = 0; i < n && !cancelled(); i++) ls_long(c, e[i], f.has('h'), szw);
    } else if (tty(c) && !f.has('1')) {
        columns(c, e, n, f.has('F'));
    } else {
        for (int i = 0; i < n && !cancelled(); i++) {
            put_name(c, e[i].name, e[i].dir, f.has('F'));
            wr(c.out, "\n", 1);
        }
    }
}

int b_ls(Ctx &c) {
    Flags f;
    int i = getflags(c, "laAh1tSrdF", f);
    if (i < 0) return 2;
    const bool all = f.has('a') || f.has('A');
    const bool want_stat = f.has('l') || f.has('t') || f.has('S');
    static const char *kDot[] = {"."};
    char **ops = c.argv + i;
    int nops = c.argc - i;
    if (!nops) { ops = (char **)kDot; nops = 1; }
    int st = 0;
    // Files first (as one listing), then each directory.
    Ent *files = (Ent *)ps_alloc(sizeof(Ent) * nops);
    int nf = 0;
    for (int k = 0; k < nops && files; k++) {
        char p[kPath];
        resolve(ops[k], p, sizeof p);
        if (!exists(p)) {
            errf(c, "ls: cannot access '%s': No such file or directory\n", ops[k]);
            st = 2;
            continue;
        }
        if (is_dir(p) && !f.has('d')) continue;
        Ent &e = files[nf++];
        e.name = ops[k];
        e.dir = is_dir(p);
        e.size = 0; e.mtime = 0;
        struct stat s;
        if (stat(p, &s) == 0) { e.size = (uint64_t)s.st_size; e.mtime = s.st_mtime; }
    }
    if (nf) ls_print(c, files, nf, f);
    heap_caps_free(files);
    bool first = nf == 0;
    int ndirs = 0;
    for (int k = 0; k < nops; k++) {
        char p[kPath];
        resolve(ops[k], p, sizeof p);
        if (exists(p) && is_dir(p) && !f.has('d')) ndirs++;
    }
    for (int k = 0; k < nops && !cancelled(); k++) {
        char p[kPath];
        resolve(ops[k], p, sizeof p);
        if (!exists(p) || !is_dir(p) || f.has('d')) continue;
        if (nops > 1) outf(c, "%s%s:\n", first ? "" : "\n", ops[k]);
        first = false;
        Ent *e;
        const size_t mark = S->arena_n;
        const int n = read_dir(p, all, want_stat, &e);
        if (n < 0) {
            errf(c, "ls: cannot open directory '%s'\n", ops[k]);
            st = 2;
            continue;
        }
        ls_print(c, e, n, f);
        heap_caps_free(e);
        S->arena_n = mark;
    }
    (void)ndirs;
    return st;
}

// Stream a file (or stdin) to stdout; -n numbers lines.
int b_cat(Ctx &c) {
    Flags f;
    int i = getflags(c, "n", f);
    if (i < 0) return 1;
    int st = 0;
    unsigned line = 1;
    bool bol = true;
    auto emit = [&](const char *p, size_t n) {
        if (!f.has('n')) { wr(c.out, p, n); return; }
        size_t s = 0;
        for (size_t k = 0; k < n; k++) {
            if (bol) { outf(c, "%6u\t", line++); bol = false; }
            if (p[k] == '\n') { wr(c.out, p + s, k - s + 1); s = k + 1; bol = true; }
        }
        if (s < n) wr(c.out, p + s, n - s);
    };
    if (i == c.argc) {
        if (c.has_in) emit(c.in, c.in_len);
        return 0;
    }
    for (; i < c.argc && !cancelled(); i++) {
        if (!strcmp(c.argv[i], "-")) { if (c.has_in) emit(c.in, c.in_len); continue; }
        char p[kPath];
        resolve(c.argv[i], p, sizeof p);
        if (is_dir(p)) { errf(c, "cat: %s: Is a directory\n", c.argv[i]); st = 1; continue; }
        FILE *fp = fopen(p, "rb");
        if (!fp) { errf(c, "cat: %s: No such file or directory\n", c.argv[i]); st = 1; continue; }
        char chunk[1024];
        size_t n;
        while (!cancelled() && (n = fread(chunk, 1, sizeof chunk, fp)) > 0) emit(chunk, n);
        fclose(fp);
    }
    return st;
}

// head / tail: -n N (or -N), several files get ==> name <== headers.
int head_tail(Ctx &c, bool tail) {
    Flags f;
    const char *nv = nullptr;
    int i = getflags(c, "nqv", f, "n", &nv);
    if (i < 0) return 1;
    long n = 10;
    bool from_start = false;   // tail -n +K
    if (nv) {
        if (tail && nv[0] == '+') from_start = true;
        n = strtol(nv[0] == '+' ? nv + 1 : nv, nullptr, 10);
        if (n < 0) n = -n;
    }
    static const char *kDash[] = {"-"};
    char **ops = c.argv + i;
    int nops = c.argc - i;
    if (!nops) { ops = (char **)kDash; nops = 1; }
    int st = 0;
    for (int k = 0; k < nops && !cancelled(); k++) {
        ShBuf b;
        if (!read_all(c, ops[k], b)) { st = 1; continue; }
        if ((nops > 1 && !f.has('q')) || f.has('v'))
            outf(c, "%s==> %s <==\n", k ? "\n" : "", strcmp(ops[k], "-") ? ops[k] : "standard input");
        const char *p = b.p ? b.p : "";
        const size_t len = b.n;
        if (!tail) {
            long left = n;
            size_t e = 0;
            while (e < len && left > 0) { if (p[e] == '\n') left--; e++; }
            wr(c.out, p, e);
        } else if (from_start) {
            long skip = n > 0 ? n - 1 : 0;
            size_t s = 0;
            while (s < len && skip > 0) { if (p[s] == '\n') skip--; s++; }
            wr(c.out, p + s, len - s);
        } else if (n > 0) {
            size_t s = len;
            long left = n;
            if (s && p[s - 1] == '\n') s--;   // the final newline doesn't start a line
            while (s > 0 && left > 0) { s--; if (p[s] == '\n') { left--; if (!left) { s++; break; } } }
            wr(c.out, p + s, len - s);
        }
        buf_free(b);
    }
    return st;
}
int b_head(Ctx &c) { return head_tail(c, false); }
int b_tail(Ctx &c) { return head_tail(c, true); }

int b_wc(Ctx &c) {
    Flags f;
    int i = getflags(c, "lwc", f);
    if (i < 0) return 1;
    const bool all = !f.has('l') && !f.has('w') && !f.has('c');
    static const char *kDash[] = {"-"};
    char **ops = c.argv + i;
    int nops = c.argc - i;
    const bool std_in = !nops;
    if (!nops) { ops = (char **)kDash; nops = 1; }
    uint64_t tl = 0, tw = 0, tc = 0;
    int st = 0;
    auto line = [&](uint64_t l, uint64_t w, uint64_t ch, const char *name) {
        if (all || f.has('l')) outf(c, "%7llu", (unsigned long long)l);
        if (all || f.has('w')) outf(c, "%s%7llu", (all || f.has('l')) ? " " : "", (unsigned long long)w);
        if (all || f.has('c')) outf(c, "%s%7llu", (all || f.has('l') || f.has('w')) ? " " : "", (unsigned long long)ch);
        outf(c, "%s%s\n", name ? " " : "", name ? name : "");
    };
    for (int k = 0; k < nops; k++) {
        ShBuf b;
        if (!read_all(c, ops[k], b)) { st = 1; continue; }
        uint64_t l = 0, w = 0;
        bool inw = false;
        for (size_t x = 0; x < b.n; x++) {
            const unsigned char ch = (unsigned char)b.p[x];
            if (ch == '\n') l++;
            if (isspace(ch)) inw = false;
            else if (!inw) { inw = true; w++; }
        }
        line(l, w, b.n, std_in ? nullptr : ops[k]);
        tl += l; tw += w; tc += b.n;
        buf_free(b);
    }
    if (nops > 1) line(tl, tw, tc, "total");
    return st;
}

// grep PATTERN [FILE...]: -i -v -n -c -l -r -F -q -H -h -o -w(ignored) -E(accepted)
int grep_buf(Ctx &c, const Re &re, const Flags &f, const char *p, size_t n, const char *name,
             bool show_name, uint64_t &hits) {
    uint64_t count = 0;
    unsigned ln = 0;
    each_line(p, n, [&](const char *s, size_t len) {
        ln++;
        const char *ms = s, *me = s;
        const bool hit = re_search(re, s, s + len, &ms, &me) != f.has('v');
        if (!hit) return true;
        count++;
        if (f.has('q') || f.has('l') || f.has('c')) return !f.has('q') && !f.has('l');
        if (show_name) {
            if (tty(c)) sgr(c, "35");
            wr(c.out, name);
            if (tty(c)) { sgr(c, "36"); wr(c.out, ":"); sgr(c, "0"); } else wr(c.out, ":");
        }
        if (f.has('n')) {
            if (tty(c)) sgr(c, "32");
            outf(c, "%u", ln);
            if (tty(c)) { sgr(c, "36"); wr(c.out, ":"); sgr(c, "0"); } else wr(c.out, ":");
        }
        if (f.has('v') || !tty(c)) {
            if (f.has('o') && !f.has('v')) wr(c.out, ms, (size_t)(me - ms));
            else wr(c.out, s, len);
        } else {
            // Highlight every match on the line, GNU grep --color style.
            const char *q = s, *e = s + len;
            while (q <= e && re_search(re, q, e, &ms, &me)) {
                if (!f.has('o')) wr(c.out, q, (size_t)(ms - q));
                sgr(c, "01;31");
                wr(c.out, ms, (size_t)(me - ms));
                sgr(c, "0");
                if (f.has('o')) wr(c.out, "\n", 1);
                q = me > ms ? me : me + 1;
                if (re.bol) break;
            }
            if (f.has('o')) return true;
            if (q < e) wr(c.out, q, (size_t)(e - q));
        }
        wr(c.out, "\n", 1);
        return true;
    });
    if (f.has('c')) {
        if (show_name) outf(c, "%s:", name);
        outf(c, "%llu\n", (unsigned long long)count);
    } else if (f.has('l') && count) {
        outf(c, "%s\n", name);
    }
    hits += count;
    return 0;
}

void grep_tree(Ctx &c, const Re &re, const Flags &f, char *path, size_t cap, const char *shown,
               uint64_t &hits, int depth) {
    if (cancelled() || depth > kMaxDepth) return;
    if (!is_dir(path)) {
        ShBuf b;
        FILE *fp = fopen(path, "rb");
        if (!fp) return;
        char chunk[2048];
        size_t n;
        while (!cancelled() && (n = fread(chunk, 1, sizeof chunk, fp)) > 0 && !b.trunc) buf_put(b, chunk, n);
        fclose(fp);
        if (b.n && !memchr(b.p, '\0', b.n < 4096 ? b.n : 4096))   // skip binary files
            grep_buf(c, re, f, b.p, b.n, shown, !f.has('h'), hits);
        buf_free(b);
        return;
    }
    DIR *d = opendir(path);
    if (!d) return;
    const size_t base = strlen(path);
    char *sub = (char *)ps_alloc(kPath);
    const size_t sbase = strlen(shown);
    while (sub && !cancelled()) {
        struct dirent *e = readdir(d);
        if (!e) break;
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        if (base + 1 + strlen(e->d_name) >= cap) continue;
        snprintf(path + base, cap - base, "%s%s", base > 1 ? "/" : "", e->d_name);
        snprintf(sub, kPath, "%s%s%s", shown, sbase && shown[sbase - 1] != '/' ? "/" : "", e->d_name);
        grep_tree(c, re, f, path, cap, sub, hits, depth + 1);
        path[base] = '\0';
    }
    heap_caps_free(sub);
    closedir(d);
}

int b_grep(Ctx &c) {
    Flags f;
    int i = getflags(c, "ivnclrRFqHhoEw", f);
    if (i < 0) return 2;
    if (i >= c.argc) { errf(c, "usage: grep [-ivnclrFqHho] PATTERN [FILE...]\n"); return 2; }
    Re *re = (Re *)ps_alloc(sizeof(Re));
    if (!re) return 2;
    const char *err = nullptr;
    if (!re_compile(*re, c.argv[i], f.has('i'), f.has('F'), &err)) {
        errf(c, "grep: %s\n", err);
        heap_caps_free(re);
        return 2;
    }
    i++;
    const bool rec = f.has('r') || f.has('R');
    uint64_t hits = 0;
    int st = 0;
    const int nops = c.argc - i;
    if (!nops && !rec) {
        if (c.has_in) grep_buf(c, *re, f, c.in, c.in_len, "(standard input)", f.has('H'), hits);
    } else {
        static const char *kDot[] = {"."};
        char **ops = nops ? c.argv + i : (char **)kDot;
        const int n = nops ? nops : 1;
        const bool names = (n > 1 || rec || f.has('H')) && !f.has('h');
        for (int k = 0; k < n && !cancelled(); k++) {
            char p[kPath];
            resolve(ops[k], p, sizeof p);
            if (!exists(p)) { errf(c, "grep: %s: No such file or directory\n", ops[k]); st = 2; continue; }
            if (is_dir(p)) {
                if (!rec) { errf(c, "grep: %s: Is a directory\n", ops[k]); continue; }
                Flags g = f;
                if (!names) g.m |= 1ull << ('h' - 'A');
                grep_tree(c, *re, g, p, sizeof p, ops[k], hits, 0);
                continue;
            }
            ShBuf b;
            if (!read_all(c, ops[k], b)) { st = 2; continue; }
            grep_buf(c, *re, f, b.p ? b.p : "", b.n, ops[k], names, hits);
            buf_free(b);
        }
    }
    heap_caps_free(re);
    if (st) return st;
    return hits ? 0 : 1;
}

// sort [-rnuf] and uniq [-c]: over lines of files or stdin.
struct LineRef { const char *p; size_t n; };
bool g_sort_num, g_sort_fold;
int line_cmp(const void *a, const void *b) {
    const LineRef *x = (const LineRef *)a, *y = (const LineRef *)b;
    if (g_sort_num) {
        const double u = strtod(x->p, nullptr), v = strtod(y->p, nullptr);
        if (u != v) return u < v ? -1 : 1;
    }
    const size_t n = x->n < y->n ? x->n : y->n;
    const int r = g_sort_fold ? strncasecmp(x->p, y->p, n) : memcmp(x->p, y->p, n);
    if (r) return r;
    return x->n < y->n ? -1 : x->n > y->n ? 1 : 0;
}

int collect_lines(Ctx &c, int i, ShBuf &all, LineRef **lines, size_t *count) {
    int st = 0;
    if (i == c.argc) { if (c.has_in) buf_put(all, c.in, c.in_len); }
    for (; i < c.argc; i++) {
        ShBuf b;
        if (!read_all(c, c.argv[i], b)) { st = 1; continue; }
        buf_put(all, b.p ? b.p : "", b.n);
        if (b.n && b.p[b.n - 1] != '\n') buf_put(all, "\n", 1);
        buf_free(b);
    }
    size_t n = 0, cap = 256;
    LineRef *l = (LineRef *)ps_alloc(sizeof(LineRef) * cap);
    if (!l) return 1;
    each_line(all.p ? all.p : "", all.n, [&](const char *s, size_t len) {
        if (n == cap) {
            LineRef *g = (LineRef *)ps_realloc(l, sizeof(LineRef) * cap * 2);
            if (!g) return false;
            l = g;
            cap *= 2;
        }
        l[n++] = {s, len};
        return true;
    });
    *lines = l;
    *count = n;
    return st;
}

int b_sort(Ctx &c) {
    Flags f;
    int i = getflags(c, "rnuf", f);
    if (i < 0) return 2;
    ShBuf all;
    LineRef *l = nullptr;
    size_t n = 0;
    const int st = collect_lines(c, i, all, &l, &n);
    g_sort_num = f.has('n');
    g_sort_fold = f.has('f');
    if (l) qsort(l, n, sizeof(LineRef), line_cmp);
    for (size_t k = 0; k < n && !cancelled(); k++) {
        const size_t x = f.has('r') ? n - 1 - k : k;
        if (f.has('u') && k && !line_cmp(&l[x], &l[f.has('r') ? x + 1 : x - 1])) continue;
        wr(c.out, l[x].p, l[x].n);
        wr(c.out, "\n", 1);
    }
    heap_caps_free(l);
    buf_free(all);
    return st;
}

int b_uniq(Ctx &c) {
    Flags f;
    int i = getflags(c, "cdi", f);
    if (i < 0) return 2;
    ShBuf all;
    LineRef *l = nullptr;
    size_t n = 0;
    const int st = collect_lines(c, i, all, &l, &n);
    g_sort_num = false;
    g_sort_fold = f.has('i');
    for (size_t k = 0; k < n && !cancelled();) {
        size_t j = k + 1;
        while (j < n && !line_cmp(&l[k], &l[j])) j++;
        if (!f.has('d') || j - k > 1) {
            if (f.has('c')) outf(c, "%7u ", (unsigned)(j - k));
            wr(c.out, l[k].p, l[k].n);
            wr(c.out, "\n", 1);
        }
        k = j;
    }
    heap_caps_free(l);
    buf_free(all);
    return st;
}

int b_mkdir(Ctx &c) {
    Flags f;
    int i = getflags(c, "pv", f);
    if (i < 0) return 1;
    if (i == c.argc) { errf(c, "mkdir: missing operand\n"); return 1; }
    int st = 0;
    for (; i < c.argc; i++) {
        char p[kPath];
        resolve(c.argv[i], p, sizeof p);
        if (f.has('p')) {
            for (char *s = p + 1; ; s++) {
                if (*s == '/' || !*s) {
                    const char save = *s;
                    *s = '\0';
                    if (!is_dir(p) && mkdir(p, 0775) != 0 && !is_dir(p)) {
                        errf(c, "mkdir: cannot create directory '%s': No such file or directory\n", c.argv[i]);
                        st = 1;
                        *s = save;
                        break;
                    }
                    *s = save;
                    if (!save) break;
                }
            }
            continue;
        }
        if (exists(p)) { errf(c, "mkdir: cannot create directory '%s': File exists\n", c.argv[i]); st = 1; continue; }
        if (mkdir(p, 0775) != 0) {
            errf(c, "mkdir: cannot create directory '%s': No such file or directory\n", c.argv[i]);
            st = 1;
        } else if (f.has('v')) {
            outf(c, "mkdir: created directory '%s'\n", c.argv[i]);
        }
    }
    return st;
}

int b_rmdir(Ctx &c) {
    int st = 0;
    if (c.argc < 2) { errf(c, "rmdir: missing operand\n"); return 1; }
    for (int i = 1; i < c.argc; i++) {
        char p[kPath];
        resolve(c.argv[i], p, sizeof p);
        if (is_mount_root(p)) { errf(c, "rmdir: failed to remove '%s': Device or resource busy\n", c.argv[i]); st = 1; continue; }
        if (!is_dir(p)) { errf(c, "rmdir: failed to remove '%s': Not a directory\n", c.argv[i]); st = 1; continue; }
        if (rmdir(p) != 0) { errf(c, "rmdir: failed to remove '%s': Directory not empty\n", c.argv[i]); st = 1; }
    }
    return st;
}

bool delete_tree(char *p, size_t cap, int depth) {
    if (cancelled() || depth > kMaxDepth) return false;
    if (!is_dir(p)) return unlink(p) == 0;
    DIR *d = opendir(p);
    if (!d) return false;
    const size_t base = strlen(p);
    bool ok = true;
    // Deleting while iterating is fine on FatFs (an entry is only marked free), so one pass works.
    while (ok) {
        struct dirent *e = readdir(d);
        if (!e) break;
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        if (base + 1 + strlen(e->d_name) >= cap) { ok = false; break; }
        snprintf(p + base, cap - base, "/%s", e->d_name);
        ok = delete_tree(p, cap, depth + 1);
        p[base] = '\0';
    }
    closedir(d);
    return ok && rmdir(p) == 0;
}

int b_rm(Ctx &c) {
    Flags f;
    int i = getflags(c, "rRfv", f);
    if (i < 0) return 1;
    if (i == c.argc) {
        if (f.has('f')) return 0;
        errf(c, "rm: missing operand\n");
        return 1;
    }
    const bool rec = f.has('r') || f.has('R');
    int st = 0;
    for (; i < c.argc && !cancelled(); i++) {
        char p[kPath];
        resolve(c.argv[i], p, sizeof p);
        if (is_mount_root(p)) { errf(c, "rm: refusing to remove '%s'\n", c.argv[i]); st = 1; continue; }
        if (!exists(p)) {
            if (!f.has('f')) { errf(c, "rm: cannot remove '%s': No such file or directory\n", c.argv[i]); st = 1; }
            continue;
        }
        if (is_dir(p) && !rec) { errf(c, "rm: cannot remove '%s': Is a directory\n", c.argv[i]); st = 1; continue; }
        if (!delete_tree(p, sizeof p, 0)) {
            if (cancelled()) break;
            errf(c, "rm: cannot remove '%s'\n", c.argv[i]);
            st = 1;
        } else if (f.has('v')) {
            outf(c, "removed '%s'\n", c.argv[i]);
        }
    }
    return st;
}

bool copy_file(const char *src, const char *dst) {
    FILE *in = fopen(src, "rb");
    if (!in) return false;
    FILE *out = fopen(dst, "wb");
    if (!out) { fclose(in); return false; }
    char *buf = (char *)ps_alloc(kCopyBuf);
    bool ok = buf != nullptr;
    while (ok) {
        if (cancelled()) { ok = false; break; }
        const size_t n = fread(buf, 1, kCopyBuf, in);
        if (n && fwrite(buf, 1, n, out) != n) { ok = false; break; }
        if (n < kCopyBuf) { ok = !ferror(in); break; }
    }
    heap_caps_free(buf);
    fclose(in);
    if (fclose(out) != 0) ok = false;
    if (!ok) { unlink(dst); return false; }
    struct stat st;
    if (stat(src, &st) == 0) {   // keep the original date
        struct utimbuf ut = {st.st_atime, st.st_mtime};
        utime(dst, &ut);
    }
    return true;
}

bool copy_tree(const char *src, const char *dst, int depth) {
    if (cancelled() || depth > kMaxDepth) return false;
    if (!is_dir(src)) return copy_file(src, dst);
    if (!is_dir(dst) && mkdir(dst, 0775) != 0) return false;
    DIR *d = opendir(src);
    if (!d) return false;
    char *a = (char *)ps_alloc(kPath), *b = (char *)ps_alloc(kPath);
    bool ok = a && b;
    while (ok) {
        struct dirent *e = readdir(d);
        if (!e) break;
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        if ((size_t)snprintf(a, kPath, "%s/%s", src, e->d_name) >= kPath ||
            (size_t)snprintf(b, kPath, "%s/%s", dst, e->d_name) >= kPath) { ok = false; break; }
        ok = copy_tree(a, b, depth + 1);
    }
    closedir(d);
    heap_caps_free(a);
    heap_caps_free(b);
    return ok;
}

// cp / mv: SRC DST, or SRC... DIR.
int cp_mv(Ctx &c, bool move) {
    Flags f;
    int i = getflags(c, move ? "fvn" : "rRfvn", f);
    if (i < 0) return 1;
    const char *cmd = move ? "mv" : "cp";
    if (c.argc - i < 2) {
        errf(c, "%s: missing %s operand\n", cmd, c.argc - i ? "destination file" : "file");
        return 1;
    }
    char dst[kPath];
    resolve(c.argv[c.argc - 1], dst, sizeof dst);
    const bool dst_dir = is_dir(dst);
    if (c.argc - i > 2 && !dst_dir) {
        errf(c, "%s: target '%s' is not a directory\n", cmd, c.argv[c.argc - 1]);
        return 1;
    }
    int st = 0;
    for (; i < c.argc - 1 && !cancelled(); i++) {
        char src[kPath], to[kPath];
        resolve(c.argv[i], src, sizeof src);
        if (!exists(src)) { errf(c, "%s: cannot stat '%s': No such file or directory\n", cmd, c.argv[i]); st = 1; continue; }
        if (dst_dir) snprintf(to, sizeof to, "%s/%s", strcmp(dst, "/") ? dst : "", base_name(src));
        else snprintf(to, sizeof to, "%s", dst);
        const size_t sl = strlen(src);
        if (!strncmp(to, src, sl) && (to[sl] == '/' || !to[sl])) {
            errf(c, "%s: cannot %s '%s' to a subdirectory of itself\n", cmd, move ? "move" : "copy", c.argv[i]);
            st = 1;
            continue;
        }
        if (f.has('n') && exists(to)) continue;
        const bool sdir = is_dir(src);
        if (!move && sdir && !f.has('r') && !f.has('R')) {
            errf(c, "cp: -r not specified; omitting directory '%s'\n", c.argv[i]);
            st = 1;
            continue;
        }
        if (move && is_mount_root(src)) { errf(c, "mv: cannot move '%s'\n", c.argv[i]); st = 1; continue; }
        bool ok;
        if (move) {
            if (exists(to) && !is_dir(to)) unlink(to);
            ok = rename(src, to) == 0;
            if (!ok) {   // another volume: copy, then delete
                ok = copy_tree(src, to, 0);
                if (ok) { char p[kPath]; snprintf(p, sizeof p, "%s", src); ok = delete_tree(p, sizeof p, 0); }
            }
        } else {
            ok = copy_tree(src, to, 0);
        }
        if (!ok) {
            if (cancelled()) break;
            errf(c, "%s: cannot %s '%s' to '%s'\n", cmd, move ? "move" : "copy", c.argv[i], c.argv[c.argc - 1]);
            st = 1;
        } else if (f.has('v')) {
            outf(c, "'%s' -> '%s'\n", c.argv[i], to);
        }
    }
    return st;
}
int b_cp(Ctx &c) { return cp_mv(c, false); }
int b_mv(Ctx &c) { return cp_mv(c, true); }

int b_touch(Ctx &c) {
    if (c.argc < 2) { errf(c, "touch: missing file operand\n"); return 1; }
    int st = 0;
    for (int i = 1; i < c.argc; i++) {
        char p[kPath];
        resolve(c.argv[i], p, sizeof p);
        if (exists(p)) {
            const time_t now = time(nullptr);
            struct utimbuf ut = {now, now};
            utime(p, &ut);
            continue;
        }
        FILE *f = fopen(p, "ab");
        if (!f) { errf(c, "touch: cannot touch '%s': No such file or directory\n", c.argv[i]); st = 1; continue; }
        fclose(f);
    }
    return st;
}

int b_stat(Ctx &c) {
    if (c.argc < 2) { errf(c, "stat: missing operand\n"); return 1; }
    int st = 0;
    for (int i = 1; i < c.argc; i++) {
        char p[kPath];
        resolve(c.argv[i], p, sizeof p);
        struct stat s = {};
        const bool dir = is_dir(p);
        if (stat(p, &s) != 0 && !dir) { errf(c, "stat: cannot stat '%s': No such file or directory\n", c.argv[i]); st = 1; continue; }
        char tm[40] = "-";
        if (s.st_mtime) {
            struct tm lt;
            localtime_r(&s.st_mtime, &lt);
            strftime(tm, sizeof tm, "%Y-%m-%d %H:%M:%S", &lt);
        }
        outf(c, "  File: %s\n  Size: %-12llu %s\nModify: %s\n", p, (unsigned long long)s.st_size,
             dir ? "directory" : "regular file", tm);
    }
    return st;
}

int b_find(Ctx &c);
int b_tree(Ctx &c);
int b_du(Ctx &c);

struct FindOpt { const char *name = nullptr; bool iname = false; char type = 0; int maxd = 1 << 20, mind = 0; };

void find_walk(Ctx &c, const FindOpt &o, char *path, size_t cap, char *shown, size_t scap, int depth) {
    if (cancelled()) return;
    const bool dir = is_dir(path);
    bool match = depth >= o.mind;
    if (o.name) match = match && wild(o.name, base_name(shown), o.iname);
    if (o.type == 'f') match = match && !dir;
    if (o.type == 'd') match = match && dir;
    if (match) { wr(c.out, shown); wr(c.out, "\n", 1); }
    if (!dir || depth >= o.maxd || depth >= kMaxDepth) return;
    DIR *d = opendir(path);
    if (!d) return;
    const size_t b1 = strlen(path), b2 = strlen(shown);
    while (!cancelled()) {
        struct dirent *e = readdir(d);
        if (!e) break;
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        const size_t l = strlen(e->d_name);
        if (b1 + 1 + l >= cap || b2 + 1 + l >= scap) continue;
        snprintf(path + b1, cap - b1, "%s%s", b1 > 1 ? "/" : "", e->d_name);
        snprintf(shown + b2, scap - b2, "%s%s", shown[b2 - 1] == '/' ? "" : "/", e->d_name);
        find_walk(c, o, path, cap, shown, scap, depth + 1);
        path[b1] = '\0';
        shown[b2] = '\0';
    }
    closedir(d);
}

int b_find(Ctx &c) {
    int i = 1;
    FindOpt o;
    const char *paths[16];
    int np = 0;
    while (i < c.argc && c.argv[i][0] != '-' && np < 16) paths[np++] = c.argv[i++];
    for (; i < c.argc; i++) {
        const char *a = c.argv[i];
        const char *v = i + 1 < c.argc ? c.argv[i + 1] : nullptr;
        if ((!strcmp(a, "-name") || !strcmp(a, "-iname")) && v) { o.name = v; o.iname = a[1] == 'i'; i++; }
        else if (!strcmp(a, "-type") && v) { o.type = v[0]; i++; }
        else if (!strcmp(a, "-maxdepth") && v) { o.maxd = atoi(v); i++; }
        else if (!strcmp(a, "-mindepth") && v) { o.mind = atoi(v); i++; }
        else if (!strcmp(a, "-print")) {}
        else { errf(c, "find: unknown predicate '%s'\n", a); return 1; }
    }
    if (!np) paths[np++] = ".";
    int st = 0;
    char *p = (char *)ps_alloc(kPath), *s = (char *)ps_alloc(kPath);
    for (int k = 0; k < np && p && s && !cancelled(); k++) {
        resolve(paths[k], p, kPath);
        if (!exists(p)) { errf(c, "find: '%s': No such file or directory\n", paths[k]); st = 1; continue; }
        snprintf(s, kPath, "%s", paths[k]);
        find_walk(c, o, p, kPath, s, kPath, 0);
    }
    heap_caps_free(p);
    heap_caps_free(s);
    return st;
}

struct TreeCount { unsigned dirs = 0, files = 0; };

void tree_walk(Ctx &c, char *path, size_t cap, char *prefix, size_t pcap, int depth, int maxd,
               bool all, bool dirs_only, TreeCount &tc) {
    if (cancelled() || depth >= maxd || depth >= kMaxDepth) return;
    Ent *e;
    const size_t mark = S->arena_n;
    int n = read_dir(path, all, false, &e);
    if (n < 0) return;
    qsort(e, n, sizeof(Ent), ent_cmp_name);
    const size_t b1 = strlen(path), b2 = strlen(prefix);
    int shown = 0;
    for (int k = 0; k < n; k++) if (!dirs_only || e[k].dir) shown++;
    for (int k = 0, idx = 0; k < n && !cancelled(); k++) {
        if (dirs_only && !e[k].dir) continue;
        const bool last = ++idx == shown;
        wr(c.out, prefix);
        wr(c.out, last ? "\xE2\x94\x94\xE2\x94\x80\xE2\x94\x80 " : "\xE2\x94\x9C\xE2\x94\x80\xE2\x94\x80 ");   // └── ├──
        put_name(c, e[k].name, e[k].dir);
        wr(c.out, "\n", 1);
        if (e[k].dir) {
            tc.dirs++;
            if (b1 + 1 + strlen(e[k].name) < cap && b2 + 8 < pcap) {
                snprintf(path + b1, cap - b1, "%s%s", b1 > 1 ? "/" : "", e[k].name);
                snprintf(prefix + b2, pcap - b2, "%s", last ? "    " : "\xE2\x94\x82   ");   // │
                tree_walk(c, path, cap, prefix, pcap, depth + 1, maxd, all, dirs_only, tc);
                path[b1] = '\0';
                prefix[b2] = '\0';
            }
        } else {
            tc.files++;
        }
    }
    heap_caps_free(e);
    S->arena_n = mark;
}

int b_tree(Ctx &c) {
    Flags f;
    const char *lv = nullptr;
    int i = getflags(c, "adL", f, "L", &lv);
    if (i < 0) return 1;
    const int maxd = lv ? atoi(lv) : kMaxDepth;
    const char *root = i < c.argc ? c.argv[i] : ".";
    char *p = (char *)ps_alloc(kPath), *pre = (char *)ps_alloc(kPath);
    if (!p || !pre) { heap_caps_free(p); heap_caps_free(pre); return 1; }
    resolve(root, p, kPath);
    if (!is_dir(p)) { errf(c, "tree: %s: not a directory\n", root); heap_caps_free(p); heap_caps_free(pre); return 1; }
    pre[0] = '\0';
    sgr(c, "01;34");
    outf(c, "%s", root);
    sgr(c, "0");
    wr(c.out, "\n", 1);
    TreeCount tc;
    tree_walk(c, p, kPath, pre, kPath, 0, maxd, f.has('a'), f.has('d'), tc);
    outf(c, "\n%u director%s", tc.dirs, tc.dirs == 1 ? "y" : "ies");
    if (!f.has('d')) outf(c, ", %u file%s", tc.files, tc.files == 1 ? "" : "s");
    wr(c.out, "\n", 1);
    heap_caps_free(p);
    heap_caps_free(pre);
    return 0;
}

uint64_t du_walk(Ctx &c, char *path, size_t cap, char *shown, size_t scap, bool print, bool h, int depth) {
    if (cancelled() || depth > kMaxDepth) return 0;
    struct stat st;
    if (!is_dir(path)) return stat(path, &st) == 0 ? (uint64_t)st.st_size : 0;
    uint64_t sum = 0;
    DIR *d = opendir(path);
    if (!d) return 0;
    const size_t b1 = strlen(path), b2 = strlen(shown);
    while (!cancelled()) {
        struct dirent *e = readdir(d);
        if (!e) break;
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        const size_t l = strlen(e->d_name);
        if (b1 + 1 + l >= cap || b2 + 1 + l >= scap) continue;
        snprintf(path + b1, cap - b1, "%s%s", b1 > 1 ? "/" : "", e->d_name);
        snprintf(shown + b2, scap - b2, "%s%s", shown[b2 - 1] == '/' ? "" : "/", e->d_name);
        sum += du_walk(c, path, cap, shown, scap, print, h, depth + 1);
        path[b1] = '\0';
        shown[b2] = '\0';
    }
    closedir(d);
    if (print) {
        char sz[16];
        if (h) human(sum, sz, sizeof sz);
        else snprintf(sz, sizeof sz, "%llu", (unsigned long long)((sum + 1023) / 1024));
        outf(c, "%-7s %s\n", sz, shown);
    }
    return sum;
}

int b_du(Ctx &c) {
    Flags f;
    int i = getflags(c, "shc", f);
    if (i < 0) return 1;
    static const char *kDot[] = {"."};
    char **ops = i < c.argc ? c.argv + i : (char **)kDot;
    const int n = i < c.argc ? c.argc - i : 1;
    char *p = (char *)ps_alloc(kPath), *s = (char *)ps_alloc(kPath);
    uint64_t total = 0;
    int st = 0;
    for (int k = 0; k < n && p && s && !cancelled(); k++) {
        resolve(ops[k], p, kPath);
        if (!exists(p)) { errf(c, "du: cannot access '%s': No such file or directory\n", ops[k]); st = 1; continue; }
        snprintf(s, kPath, "%s", ops[k]);
        const uint64_t sum = du_walk(c, p, kPath, s, kPath, !f.has('s'), f.has('h'), 0);
        total += sum;
        if (f.has('s') || !is_dir(p)) {
            char sz[16];
            if (f.has('h')) human(sum, sz, sizeof sz);
            else snprintf(sz, sizeof sz, "%llu", (unsigned long long)((sum + 1023) / 1024));
            outf(c, "%-7s %s\n", sz, ops[k]);
        }
    }
    if (f.has('c')) {
        char sz[16];
        if (f.has('h')) human(total, sz, sizeof sz);
        else snprintf(sz, sizeof sz, "%llu", (unsigned long long)((total + 1023) / 1024));
        outf(c, "%-7s total\n", sz);
    }
    heap_caps_free(p);
    heap_caps_free(s);
    return st;
}

int b_df(Ctx &c) {
    Flags f;
    if (getflags(c, "hT", f) < 0) return 1;
    const bool h = f.has('h');
    outf(c, "%-12s %9s %9s %9s %4s  %s\n", "Filesystem", h ? "Size" : "1K-blocks", "Used",
         h ? "Avail" : "Available", "Use%", "Mounted on");
    auto row = [&](const char *fs, uint64_t total, uint64_t freeb, const char *mnt) {
        const uint64_t used = total - freeb;
        char a[16], b[16], d[16];
        if (h) { human(total, a, sizeof a); human(used, b, sizeof b); human(freeb, d, sizeof d); }
        else {
            snprintf(a, sizeof a, "%llu", (unsigned long long)(total / 1024));
            snprintf(b, sizeof b, "%llu", (unsigned long long)(used / 1024));
            snprintf(d, sizeof d, "%llu", (unsigned long long)(freeb / 1024));
        }
        const int pct = total ? (int)((used * 100 + total - 1) / total) : 0;
        outf(c, "%-12s %9s %9s %9s %3d%%  %s\n", fs, a, b, d, pct, mnt);
    };
    uint64_t t, fr;
    if (nv_sd_info(&t, &fr)) row("/dev/mmcblk0", t, fr, "/sdcard");
    for (int i = 0; i < NV_USB_STOR_SLOTS; i++) {
        nv_usb_stor_info_t inf;
        if (!nv_usb_storage_get(i, &inf) || inf.state != NV_USB_STOR_MOUNTED) continue;
        char dev[16];
        snprintf(dev, sizeof dev, "/dev/sd%c1", 'a' + i);
        row(dev, inf.total_bytes, inf.free_bytes == UINT64_MAX ? 0 : inf.free_bytes, inf.path);
    }
    return 0;
}

int b_xxd(Ctx &c) {
    Flags f;
    const char *lim = nullptr;
    int i = getflags(c, "l", f, "l", &lim);
    if (i < 0) return 1;
    ShBuf b;
    if (!read_all(c, i < c.argc ? c.argv[i] : "-", b)) return 1;
    size_t n = b.n;
    if (lim && (size_t)atol(lim) < n) n = (size_t)atol(lim);
    for (size_t off = 0; off < n && !cancelled(); off += 16) {
        char line[96];
        int k = snprintf(line, sizeof line, "%08x: ", (unsigned)off);
        for (size_t j = 0; j < 16; j++) {
            if (off + j < n) k += snprintf(line + k, sizeof line - k, "%02x", (uint8_t)b.p[off + j]);
            else k += snprintf(line + k, sizeof line - k, "  ");
            if (j & 1) line[k++] = ' ';
        }
        line[k++] = ' ';
        for (size_t j = 0; j < 16 && off + j < n; j++) {
            const unsigned char ch = (unsigned char)b.p[off + j];
            line[k++] = (ch >= 0x20 && ch < 0x7F) ? (char)ch : '.';
        }
        line[k++] = '\n';
        wr(c.out, line, (size_t)k);
    }
    buf_free(b);
    return 0;
}

int b_basename(Ctx &c) {
    if (c.argc < 2) { errf(c, "basename: missing operand\n"); return 1; }
    char b[kPath];
    snprintf(b, sizeof b, "%s", c.argv[1]);
    size_t l = strlen(b);
    while (l > 1 && b[l - 1] == '/') b[--l] = '\0';
    const char *s = strrchr(b, '/');
    s = s && s[1] ? s + 1 : b;
    size_t n = strlen(s);
    if (c.argc > 2) {
        const size_t sl = strlen(c.argv[2]);
        if (sl < n && !strcmp(s + n - sl, c.argv[2])) n -= sl;
    }
    wr(c.out, s, n);
    wr(c.out, "\n", 1);
    return 0;
}

int b_dirname(Ctx &c) {
    if (c.argc < 2) { errf(c, "dirname: missing operand\n"); return 1; }
    char b[kPath];
    snprintf(b, sizeof b, "%s", c.argv[1]);
    size_t l = strlen(b);
    while (l > 1 && b[l - 1] == '/') b[--l] = '\0';
    char *s = strrchr(b, '/');
    if (!s) outf(c, ".\n");
    else if (s == b) outf(c, "/\n");
    else { *s = '\0'; outf(c, "%s\n", b); }
    return 0;
}

// ---------------------------------------------------------------- built-ins: shell

int b_echo(Ctx &c) {
    int i = 1;
    bool nl = true, esc = false;
    for (; i < c.argc && c.argv[i][0] == '-' && c.argv[i][1]; i++) {
        const char *a = c.argv[i] + 1;
        if (strspn(a, "neE") != strlen(a)) break;
        for (; *a; a++) { if (*a == 'n') nl = false; else if (*a == 'e') esc = true; else esc = false; }
    }
    for (int k = i; k < c.argc; k++) {
        if (k > i) wr(c.out, " ", 1);
        if (!esc) { wr(c.out, c.argv[k]); continue; }
        for (const char *p = c.argv[k]; *p; p++) {
            char ch = *p;
            if (ch == '\\' && p[1]) {
                p++;
                switch (*p) {
                    case 'n': ch = '\n'; break;
                    case 't': ch = '\t'; break;
                    case 'e': ch = '\x1b'; break;
                    case '\\': ch = '\\'; break;
                    case 'c': return 0;
                    default: wr(c.out, "\\", 1); ch = *p; break;
                }
            }
            wr(c.out, &ch, 1);
        }
    }
    if (nl) wr(c.out, "\n", 1);
    return 0;
}

int b_env(Ctx &c) {
    static const char *kFixed[] = {"HOME", "PWD", "OLDPWD", "USER", "HOSTNAME", "SHELL", "TERM", "COLUMNS"};
    for (const char *n : kFixed) {
        const char *v = var_get(n);
        if (v && v[0]) outf(c, "%s=%s\n", n, v);
    }
    for (const Var &v : S->vars) if (v.name[0]) outf(c, "%s=%s\n", v.name, v.val);
    return 0;
}

int b_export(Ctx &c) {
    if (c.argc < 2) return b_env(c);
    int st = 0;
    for (int i = 1; i < c.argc; i++) {
        const char *eq = strchr(c.argv[i], '=');
        const size_t n = eq ? (size_t)(eq - c.argv[i]) : strlen(c.argv[i]);
        char name[32];
        snprintf(name, sizeof name, "%.*s", (int)n, c.argv[i]);
        if (!var_name_ok(name, strlen(name))) { errf(c, "export: '%s': not a valid identifier\n", c.argv[i]); st = 1; continue; }
        if (eq && !var_set(name, eq + 1)) { errf(c, "export: too many variables\n"); st = 1; }
    }
    return st;
}

int b_unset(Ctx &c) {
    for (int i = 1; i < c.argc; i++) var_unset(c.argv[i]);
    return 0;
}

int b_history(Ctx &c) {
    if (c.argc > 1 && !strcmp(c.argv[1], "-c")) { term_hist_clear(); return 0; }
    const int n = term_hist_count();
    for (int i = 0; i < n; i++) outf(c, "%5d  %s\n", i + 1, term_hist_at(i));
    return 0;
}

int b_true(Ctx &) { return 0; }
int b_false(Ctx &) { return 1; }

int b_sleep(Ctx &c) {
    if (c.argc < 2) { errf(c, "sleep: missing operand\n"); return 1; }
    const int64_t end = esp_timer_get_time() + (int64_t)(strtod(c.argv[1], nullptr) * 1e6);
    while (esp_timer_get_time() < end) {
        if (cancelled()) return 130;
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    return 0;
}

int b_clear(Ctx &c) { wr(c.out, "\x1b[H\x1b[2J"); return 0; }
int b_exit(Ctx &) { term_request_exit(); return 0; }

// ---------------------------------------------------------------- built-ins: system

int b_uname(Ctx &c) {
    Flags f;
    if (getflags(c, "asnrmo", f) < 0) return 1;
    const bool a = f.has('a');
    const bool none = !f.m;
    bool sp = false;
    auto part = [&](bool on, const char *s) {
        if (!on) return;
        outf(c, "%s%s", sp ? " " : "", s);
        sp = true;
    };
    part(a || none || f.has('s'), "NucleoOS");
    part(a || f.has('n'), kHost);
    part(a || f.has('r'), nv_ota_running_version());
    part(a || f.has('m'), "riscv32");
    part(a || f.has('o'), "ESP32-P4");
    wr(c.out, "\n", 1);
    return 0;
}

int b_hostname(Ctx &c) { outf(c, "%s\n", kHost); return 0; }
int b_whoami(Ctx &c) { outf(c, "%s\n", kUser); return 0; }

int b_date(Ctx &c) {
    const char *fmt = "%a %b %e %H:%M:%S %Z %Y";
    if (c.argc > 1 && c.argv[1][0] == '+') fmt = c.argv[1] + 1;
    char t[128];
    nv_time_format(t, sizeof t, fmt);
    outf(c, "%s\n", t);
    return 0;
}

int b_uptime(Ctx &c) {
    const uint32_t s = (uint32_t)(esp_timer_get_time() / 1000000);
    char now[16];
    nv_time_format(now, sizeof now, "%H:%M:%S");
    const unsigned d = s / 86400u, h = (s / 3600u) % 24u, m = (s / 60u) % 60u;
    if (d) outf(c, " %s up %u day%s, %2u:%02u\n", now, d, d == 1 ? "" : "s", h, m);
    else if (h) outf(c, " %s up %2u:%02u\n", now, h, m);
    else outf(c, " %s up %u min\n", now, m);
    return 0;
}

int b_free(Ctx &c) {
    Flags f;
    if (getflags(c, "hkm", f) < 0) return 1;
    auto num = [&](size_t v, char *o, size_t cap) {
        if (f.has('h')) human(v, o, cap);
        else if (f.has('m')) snprintf(o, cap, "%u", (unsigned)(v / (1024 * 1024)));
        else snprintf(o, cap, "%u", (unsigned)(v / 1024));
    };
    outf(c, "%-10s %10s %10s %10s %10s\n", "", "total", "used", "free", "largest");
    struct { const char *name; uint32_t caps; } kPools[] = {
        {"Internal:", MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT}, {"PSRAM:", MALLOC_CAP_SPIRAM},
    };
    for (const auto &p : kPools) {
        const size_t tot = heap_caps_get_total_size(p.caps), fr = heap_caps_get_free_size(p.caps);
        const size_t big = heap_caps_get_largest_free_block(p.caps);
        char a[16], b[16], d[16], e[16];
        num(tot, a, sizeof a); num(tot - fr, b, sizeof b); num(fr, d, sizeof d); num(big, e, sizeof e);
        outf(c, "%-10s %10s %10s %10s %10s\n", p.name, a, b, d, e);
    }
    return 0;
}

const char *svc_state_str(nv_service_state_t st) {
    switch (st) {
        case NV_SVC_RUNNING:   return "running";
        case NV_SVC_SUSPENDED: return "suspended";
        default:               return "stopped";
    }
}

int b_ps(Ctx &c) {
    outf(c, "%5s %-16s %s\n", "ID", "SERVICE", "STATE");
    const int n = nv_service_count();
    for (int id = 0; id < n; id++) {
        const char *nm = nv_service_name(id);
        if (!nm) continue;
        const nv_service_state_t st = nv_service_state(id);
        outf(c, "%5d %-16s ", id, nm);
        sgr(c, st == NV_SVC_RUNNING ? "32" : "33");
        outf(c, "%s", svc_state_str(st));
        sgr(c, "0");
        wr(c.out, "\n", 1);
    }
    return 0;
}

int b_dmesg(Ctx &c) {
    constexpr size_t kSnap = 8192;
    char *snap = (char *)ps_alloc(kSnap);
    if (!snap) return 1;
    const size_t k = nv_log_snapshot(snap, kSnap);
    if (k) wr(c.out, snap, strnlen(snap, kSnap));
    heap_caps_free(snap);
    return 0;
}

int b_sensors(Ctx &c) {
    float t;
    if (!nv_hal_temp_read(&t)) { errf(c, "sensors: no sensors found\n"); return 1; }
    outf(c, "esp32p4-tsens\nAdapter: on-die\ntemp1:        %+.1f\xC2\xB0""C\n", (double)t);
    return 0;
}

int b_ip(Ctx &c) {
    if (!nv_wifi_is_enabled()) { outf(c, "wlan0: <DOWN>  wifi off\n"); return 0; }
    nv_wifi_link_t lk;
    if (!nv_wifi_get_link(&lk)) { outf(c, "wlan0: <NO-CARRIER>  not connected\n"); return 0; }
    outf(c, "wlan0: <BROADCAST,MULTICAST,UP,LOWER_UP>\n");
    outf(c, "    inet %s\n", lk.ip);
    outf(c, "    ssid \"%s\"  signal %d dBm  channel %u  %s\n", lk.ssid, (int)lk.rssi,
         (unsigned)lk.channel, nv_wifi_gen_label(lk.gen));
    return 0;
}

int b_i2cdetect(Ctx &c) {
    i2c_master_bus_handle_t bus = nv_hal_i2c_bus();
    if (!bus) { errf(c, "i2cdetect: no bus\n"); return 1; }
    outf(c, "     0  1  2  3  4  5  6  7  8  9  a  b  c  d  e  f\n");
    for (unsigned row = 0; row < 8 && !cancelled(); row++) {
        outf(c, "%02x:", row * 16);
        for (unsigned col = 0; col < 16; col++) {
            const unsigned a = row * 16 + col;
            if (a < 0x08 || a > 0x77) { outf(c, "   "); continue; }
            if (i2c_master_probe(bus, (uint16_t)a, 20) == ESP_OK) outf(c, " %02x", a);
            else outf(c, " --");
        }
        wr(c.out, "\n", 1);
    }
    return 0;
}

struct UsbSet { bool host; };
void usb_set_ui(void *arg) { nv_config_set_bool("usbhost", ((UsbSet *)arg)->host); }

int b_usb(Ctx &c) {
    const bool host = nv_config_get_bool("usbhost", true);
    if (c.argc < 2) {
        outf(c, "mode:      %s\n", host ? "host (speaker, keyboard, storage)" : "device (second screen)");
        if (host) {
            outf(c, "devices:   %d\n", nv_usb_audio_bus_devices());
            outf(c, "speaker:   %s\n", nv_usb_audio_present() ? "connected" : "none");
            outf(c, "keyboard:  %s\nmouse:     %s\n", nv_hid_host_keyboard_present() ? "yes" : "no",
                 nv_hid_host_mouse_present() ? "yes" : "no");
            if (nv_usb_audio_bus_devices() == 0)
                outf(c, "no devices: wrong port or a charge-only adapter\n");
        }
        outf(c, "usage: usb host|device   (applies after reboot)\n");
        return 0;
    }
    UsbSet s;
    if (!strcmp(c.argv[1], "host")) s.host = true;
    else if (!strcmp(c.argv[1], "device")) s.host = false;
    else { errf(c, "usb: 'host' or 'device'\n"); return 1; }
    if (!term_ui_call(usb_set_ui, &s)) return 1;
    outf(c, "saved; 'reboot' to apply\n");
    return 0;
}

int b_bl(Ctx &c) {
    if (c.argc < 2) { errf(c, "usage: bl 0-100\n"); return 1; }
    int pct = atoi(c.argv[1]);
    pct = pct < 0 ? 0 : pct > 100 ? 100 : pct;
    nv_hal_backlight_set(pct);
    return 0;
}

int b_apps(Ctx &c) {
    constexpr int kMax = 64;
    auto *apps = (nv_wasm_app_t *)ps_alloc(sizeof(nv_wasm_app_t) * kMax);
    if (!apps) return 1;
    const int n = nv_wasm_scan(apps, kMax);
    int shown = 0;
    for (int i = 0; i < n; i++) {
        if (!apps[i].console) continue;
        sgr(c, "01;32");
        outf(c, "%-12s", apps[i].id);
        sgr(c, "0");
        outf(c, " %s %s\n", apps[i].name, apps[i].version);
        shown++;
    }
    if (!shown) outf(c, "no terminal programs yet: Lua, JavaScript and SQLite install by themselves\n"
                        "shortly after boot (Wi-Fi needed); more are in the Store\n");
    heap_caps_free(apps);
    S->nprogs = -1;   // completion rescans
    return 0;
}

int b_open(Ctx &c) {
    if (c.argc < 2) { errf(c, "usage: open FILE\n"); return 1; }
    char p[kPath];
    resolve(c.argv[1], p, sizeof p);
    if (!exists(p)) { errf(c, "open: %s: No such file or directory\n", c.argv[1]); return 1; }
    if (!nv_open_file_async(p, nullptr)) { errf(c, "open: %s: no app opens this file\n", c.argv[1]); return 1; }
    return 0;
}

void reboot_ui(void *) { esp_restart(); }
int b_reboot(Ctx &c) {
    outf(c, "rebooting...\n");
    vTaskDelay(pdMS_TO_TICKS(600));   // let the line reach the screen
    term_ui_call(reboot_ui, nullptr);
    return 0;
}

int b_help(Ctx &c);
int b_which(Ctx &c);

// ---------------------------------------------------------------- built-ins: network / hashes / top

// curl / wget over esp_http_client (HTTPS with the certificate bundle, redirects followed).
struct Fetch {
    Ctx     *c;
    ShSink   sink;        // where the body goes
    bool     progress;    // wget-style progress line on the screen
    uint64_t got = 0;
    int64_t  total = -1;
    int64_t  t0 = 0, last = 0;
};

void fetch_progress(Fetch &f, bool final) {
    const int64_t now = esp_timer_get_time();
    if (!final && now - f.last < 250000) return;
    f.last = now;
    const double secs = (now - f.t0) / 1e6;
    char got[16], rate[16];
    human(f.got, got, sizeof got);
    human(secs > 0.05 ? (uint64_t)(f.got / secs) : 0, rate, sizeof rate);
    char bar[32];
    if (f.total > 0) {
        const int pct = (int)(f.got * 100 / (uint64_t)f.total);
        const int fill = pct * 20 / 100;
        for (int i = 0; i < 20; i++) bar[i] = i < fill ? '=' : (i == fill ? '>' : ' ');
        bar[20] = '\0';
        errf(*f.c, "\r%3d%%[%s] %7s  %7s/s", pct, bar, got, rate);
    } else {
        errf(*f.c, "\r    [ <=>                ] %7s  %7s/s", got, rate);
    }
    if (final) errf(*f.c, "    in %.1fs\n", secs);
}

// Fetch url into f.sink. Returns an HTTP status (>= 400 is an error) or -1 on a network error.
int fetch(Fetch &f, const char *url, bool follow, char *err, size_t errn) {
    esp_http_client_config_t cfg = {};
    cfg.url = url;
    cfg.timeout_ms = 15000;
    cfg.buffer_size = 4096;
    cfg.buffer_size_tx = 1024;
    cfg.crt_bundle_attach = esp_crt_bundle_attach;
    cfg.user_agent = "curl/8 (NucleoOS)";
    esp_http_client_handle_t h = esp_http_client_init(&cfg);
    if (!h) { snprintf(err, errn, "out of memory"); return -1; }
    int status = -1;
    for (int hop = 0; hop < 6 && !cancelled(); hop++) {
        if (esp_http_client_open(h, 0) != ESP_OK) { snprintf(err, errn, "could not connect"); break; }
        const int64_t len = esp_http_client_fetch_headers(h);
        status = esp_http_client_get_status_code(h);
        if (follow && status >= 300 && status < 400 && status != 304) {
            esp_http_client_set_redirection(h);
            esp_http_client_close(h);
            status = -1;
            continue;
        }
        f.total = len > 0 ? len : -1;
        f.t0 = f.last = esp_timer_get_time();
        char *buf = (char *)ps_alloc(4096);
        while (buf && !cancelled()) {
            const int n = esp_http_client_read(h, buf, 4096);
            if (n < 0) { snprintf(err, errn, "connection lost"); status = -1; break; }
            if (n == 0) {
                if (esp_http_client_is_complete_data_received(h) || f.total < 0) break;
                snprintf(err, errn, "connection closed early");
                status = -1;
                break;
            }
            sh_sink_write(f.sink, buf, (size_t)n);
            f.got += (uint64_t)n;
            if (f.progress) fetch_progress(f, false);
        }
        heap_caps_free(buf);
        esp_http_client_close(h);
        break;
    }
    esp_http_client_cleanup(h);
    if (cancelled()) { snprintf(err, errn, "interrupted"); return -1; }
    return status;
}

// Last path component of a URL, for wget / curl -O ("index.html" for a bare host).
void url_name(const char *url, char *out, size_t cap) {
    const char *p = strstr(url, "://");
    p = p ? p + 3 : url;
    const char *slash = strchr(p, '/');
    const char *q = slash ? slash : p + strlen(p);
    const char *end = q + strcspn(q, "?#");
    const char *b = end;
    while (b > q && b[-1] != '/') b--;
    if (b == end) snprintf(out, cap, "index.html");
    else snprintf(out, cap, "%.*s", (int)(end - b), b);
}

int web_get(Ctx &c, bool wget) {
    Flags f;
    const char *outname = nullptr;
    int i = getflags(c, wget ? "qO" : "sSLfOo", f, wget ? "O" : "o", &outname);
    if (i < 0) return 2;
    if (i >= c.argc) { errf(c, "usage: %s\n", wget ? "wget [-q] [-O FILE] URL" : "curl [-sLfO] [-o FILE] URL"); return 2; }
    const char *url = c.argv[i];
    char full[512];
    if (!strstr(url, "://")) { snprintf(full, sizeof full, "http://%s", url); url = full; }
    char name[128];
    url_name(url, name, sizeof name);
    const bool to_file = wget || outname || f.has('O');
    const char *file = outname ? outname : name;
    const bool to_stdout = to_file && !strcmp(file, "-");
    Fetch ft;
    ft.c = &c;
    ft.progress = wget ? !f.has('q') : (to_file && !to_stdout && !f.has('s'));
    FILE *fp = nullptr;
    char path[kPath];
    if (to_file && !to_stdout) {
        resolve(file, path, sizeof path);
        fp = fopen(path, "wb");
        if (!fp) { errf(c, "%s: %s: cannot write\n", c.argv[0], file); return 23; }
        ft.sink.k = SH_FILE;
        ft.sink.f = fp;
    } else {
        ft.sink = c.out;
    }
    if (wget && !f.has('q')) {
        char when[24];
        nv_time_format(when, sizeof when, "%Y-%m-%d %H:%M:%S");
        errf(c, "--%s--  %s\n", when, url);
    }
    char err[64] = "";
    const int st = fetch(ft, url, wget || f.has('L'), err, sizeof err);
    if (ft.progress && st >= 0) fetch_progress(ft, true);
    if (fp) fclose(fp);
    const bool failed = st < 0 || (st >= 400 && (wget || f.has('f')));
    if (failed && fp) unlink(path);
    if (st < 0) {
        if (!f.has('s') || f.has('S')) errf(c, "%s: %s: %s\n", c.argv[0], url, err);
        return wget ? 4 : 7;
    }
    if (st >= 400) {
        if (wget) { errf(c, "ERROR %d.\n", st); return 8; }
        if (f.has('f')) { errf(c, "curl: (22) The requested URL returned error: %d\n", st); return 22; }
    }
    if (wget && !f.has('q')) {
        char sz[16];
        human(ft.got, sz, sizeof sz);
        errf(c, "'%s' saved [%s]\n", file, sz);
    }
    return 0;
}
int b_curl(Ctx &c) { return web_get(c, false); }
int b_wget(Ctx &c) { return web_get(c, true); }

// First IPv4 address of a host name (or a literal address).
bool resolve_host(const char *host, ip_addr_t *out, char *txt, size_t cap) {
    struct addrinfo hints = {};
    hints.ai_family = AF_INET;
    struct addrinfo *res = nullptr;
    if (getaddrinfo(host, nullptr, &hints, &res) != 0 || !res) return false;
    const struct sockaddr_in *sa = (const struct sockaddr_in *)res->ai_addr;
    inet_ntop(AF_INET, &sa->sin_addr, txt, cap);
    freeaddrinfo(res);
    return ipaddr_aton(txt, out) != 0;
}

int b_host(Ctx &c) {
    if (c.argc < 2) { errf(c, "usage: %s NAME\n", c.argv[0]); return 1; }
    struct addrinfo hints = {};
    hints.ai_family = AF_UNSPEC;
    struct addrinfo *res = nullptr;
    if (getaddrinfo(c.argv[1], nullptr, &hints, &res) != 0 || !res) {
        errf(c, "Host %s not found: 3(NXDOMAIN)\n", c.argv[1]);
        return 1;
    }
    for (struct addrinfo *r = res; r; r = r->ai_next) {
        char a[48] = "";
        if (r->ai_family == AF_INET)
            inet_ntop(AF_INET, &((const struct sockaddr_in *)r->ai_addr)->sin_addr, a, sizeof a);
        else if (r->ai_family == AF_INET6)
            inet_ntop(AF_INET6, &((const struct sockaddr_in6 *)r->ai_addr)->sin6_addr, a, sizeof a);
        else continue;
        outf(c, "%s has %saddress %s\n", c.argv[1], r->ai_family == AF_INET6 ? "IPv6 " : "", a);
    }
    freeaddrinfo(res);
    return 0;
}

struct PingCtx {
    Ctx *c;
    const char *host;
    char ip[20];
    uint32_t sent = 0, recv = 0;
    uint32_t tmin = UINT32_MAX, tmax = 0, tsum = 0;
    SemaphoreHandle_t done;
};

void ping_ok(esp_ping_handle_t h, void *arg) {
    PingCtx *p = (PingCtx *)arg;
    uint16_t seq; uint8_t ttl; uint32_t ms, size;
    esp_ping_get_profile(h, ESP_PING_PROF_SEQNO, &seq, sizeof seq);
    esp_ping_get_profile(h, ESP_PING_PROF_TTL, &ttl, sizeof ttl);
    esp_ping_get_profile(h, ESP_PING_PROF_TIMEGAP, &ms, sizeof ms);
    esp_ping_get_profile(h, ESP_PING_PROF_SIZE, &size, sizeof size);
    p->recv++;
    p->tsum += ms;
    if (ms < p->tmin) p->tmin = ms;
    if (ms > p->tmax) p->tmax = ms;
    outf(*p->c, "%u bytes from %s: seq=%u ttl=%u time=%u ms\n", (unsigned)size, p->ip, seq, ttl, (unsigned)ms);
}

void ping_timeout(esp_ping_handle_t h, void *arg) {
    PingCtx *p = (PingCtx *)arg;
    uint16_t seq;
    esp_ping_get_profile(h, ESP_PING_PROF_SEQNO, &seq, sizeof seq);
    outf(*p->c, "Request timeout for seq=%u\n", seq);
}

void ping_end(esp_ping_handle_t h, void *arg) {
    PingCtx *p = (PingCtx *)arg;
    esp_ping_get_profile(h, ESP_PING_PROF_REQUEST, &p->sent, sizeof p->sent);
    xSemaphoreGive(p->done);
}

int b_ping(Ctx &c) {
    Flags f;
    const char *cnt = nullptr;
    int i = getflags(c, "c", f, "c", &cnt);
    if (i < 0) return 2;
    if (i >= c.argc) { errf(c, "usage: ping [-c COUNT] HOST\n"); return 2; }
    PingCtx p;
    p.c = &c;
    p.host = c.argv[i];
    ip_addr_t target = {};
    if (!resolve_host(p.host, &target, p.ip, sizeof p.ip)) {
        errf(c, "ping: bad address '%s'\n", p.host);
        return 2;
    }
    esp_ping_config_t cfg = ESP_PING_DEFAULT_CONFIG();
    cfg.target_addr = target;
    cfg.count = cnt ? (uint32_t)atoi(cnt) : 4;
    cfg.task_stack_size = 4096;
    esp_ping_callbacks_t cb = {};
    cb.cb_args = &p;
    cb.on_ping_success = ping_ok;
    cb.on_ping_timeout = ping_timeout;
    cb.on_ping_end = ping_end;
    p.done = xSemaphoreCreateBinary();
    esp_ping_handle_t h = nullptr;
    if (!p.done || esp_ping_new_session(&cfg, &cb, &h) != ESP_OK) {
        if (p.done) vSemaphoreDelete(p.done);
        errf(c, "ping: cannot start\n");
        return 2;
    }
    outf(c, "PING %s (%s): %u data bytes\n", p.host, p.ip, (unsigned)cfg.data_size);
    esp_ping_start(h);
    bool stopped = false;
    while (xSemaphoreTake(p.done, pdMS_TO_TICKS(100)) != pdTRUE) {
        if (cancelled() && !stopped) { esp_ping_stop(h); stopped = true; xSemaphoreGive(p.done); }
    }
    if (stopped) esp_ping_get_profile(h, ESP_PING_PROF_REQUEST, &p.sent, sizeof p.sent);
    esp_ping_delete_session(h);
    vSemaphoreDelete(p.done);
    outf(c, "\n--- %s ping statistics ---\n", p.host);
    const unsigned loss = p.sent ? (unsigned)((p.sent - p.recv) * 100 / p.sent) : 0;
    outf(c, "%u packets transmitted, %u packets received, %u%% packet loss\n", (unsigned)p.sent,
         (unsigned)p.recv, loss);
    if (p.recv)
        outf(c, "round-trip min/avg/max = %u/%u/%u ms\n", (unsigned)p.tmin, (unsigned)(p.tsum / p.recv),
             (unsigned)p.tmax);
    return p.recv ? 0 : 1;
}

// md5sum / sha1sum / sha256sum [FILE...]
int b_hash(Ctx &c) {
    const char *cmd = c.argv[0];
    const mbedtls_md_type_t type = !strcmp(cmd, "md5sum") ? MBEDTLS_MD_MD5
                                 : !strcmp(cmd, "sha1sum") ? MBEDTLS_MD_SHA1 : MBEDTLS_MD_SHA256;
    const mbedtls_md_info_t *info = mbedtls_md_info_from_type(type);
    if (!info) { errf(c, "%s: not available\n", cmd); return 1; }
    static const char *kDash[] = {"-"};
    char **ops = c.argc > 1 ? c.argv + 1 : (char **)kDash;
    const int n = c.argc > 1 ? c.argc - 1 : 1;
    int st = 0;
    char *buf = (char *)ps_alloc(kCopyBuf);
    if (!buf) return 1;
    for (int k = 0; k < n && !cancelled(); k++) {
        mbedtls_md_context_t md;
        mbedtls_md_init(&md);
        if (mbedtls_md_setup(&md, info, 0) != 0) { mbedtls_md_free(&md); st = 1; break; }
        mbedtls_md_starts(&md);
        bool ok = true;
        if (!strcmp(ops[k], "-")) {
            if (c.has_in) mbedtls_md_update(&md, (const unsigned char *)c.in, c.in_len);
        } else {
            char p[kPath];
            resolve(ops[k], p, sizeof p);
            FILE *fp = is_dir(p) ? nullptr : fopen(p, "rb");
            if (!fp) {
                errf(c, "%s: %s: %s\n", cmd, ops[k], is_dir(p) ? "Is a directory" : "No such file or directory");
                ok = false;
            } else {
                size_t r;
                while (!cancelled() && (r = fread(buf, 1, kCopyBuf, fp)) > 0)
                    mbedtls_md_update(&md, (const unsigned char *)buf, r);
                fclose(fp);
            }
        }
        if (ok) {
            unsigned char dig[32];
            mbedtls_md_finish(&md, dig);
            const int len = mbedtls_md_get_size(info);
            for (int b = 0; b < len; b++) outf(c, "%02x", dig[b]);
            outf(c, "  %s\n", ops[k]);
        } else {
            st = 1;
        }
        mbedtls_md_free(&md);
    }
    heap_caps_free(buf);
    return st;
}

// top [-b] [-n N] [-d SECONDS]: the live task table (nv_sysmon), refreshed until ^C.
int b_top(Ctx &c) {
    Flags f;
    const char *v = nullptr;
    int i = getflags(c, "bnd", f, "nd", &v);
    if (i < 0) return 1;
    int iters = -1;
    double delay = 2.0;
    // getflags keeps only the last valued option: scan again for both.
    for (int k = 1; k < c.argc; k++) {
        if (!strcmp(c.argv[k], "-n") && k + 1 < c.argc) iters = atoi(c.argv[++k]);
        else if (!strcmp(c.argv[k], "-d") && k + 1 < c.argc) delay = strtod(c.argv[++k], nullptr);
    }
    if (delay < 0.5) delay = 0.5;
    const bool batch = f.has('b') || !tty(c);
    if (batch && iters < 0) iters = 1;
    constexpr int kRows = 48;
    auto *rows = (nv_task_row_t *)ps_alloc(sizeof(nv_task_row_t) * kRows);
    if (!rows) return 1;
    nv_sysmon_tasks(rows, kRows);   // baseline for the CPU deltas
    vTaskDelay(pdMS_TO_TICKS(1000));
    static const char kState[] = "RRBSDI";
    for (int it = 0; iters < 0 || it < iters; it++) {
        if (cancelled()) break;
        nv_sys_perf_t perf;
        nv_sys_mem_t mem;
        nv_sysmon_perf(&perf);
        nv_sysmon_mem(&mem);
        const int n = nv_sysmon_tasks(rows, kRows);
        if (!batch) wr(c.out, "\x1b[H\x1b[2J");
        char now[16];
        nv_time_format(now, sizeof now, "%H:%M:%S");
        const unsigned up = (unsigned)(perf.uptime_s / 60);
        outf(c, "top - %s up %u:%02u,  %u tasks,  cpu0 %.1f%%  cpu1 %.1f%%  %u MHz", now, up / 60, up % 60,
             (unsigned)perf.task_count, (double)perf.core_load[0], (double)perf.core_load[1],
             (unsigned)perf.freq_mhz);
        if (perf.temp_valid) outf(c, "  %.1f\xC2\xB0""C", (double)perf.temp_c);
        wr(c.out, "\n", 1);
        char a[16], b[16], d[16];
        human(mem.internal.total, a, sizeof a); human(mem.internal.used, b, sizeof b); human(mem.internal.largest, d, sizeof d);
        outf(c, "SRAM:  %6s total  %6s used  %6s largest  (min free %u K)\n", a, b, d, (unsigned)(mem.internal.min_free / 1024));
        human(mem.psram.total, a, sizeof a); human(mem.psram.used, b, sizeof b); human(mem.psram.largest, d, sizeof d);
        outf(c, "PSRAM: %6s total  %6s used  %6s largest\n\n", a, b, d);
        if (tty(c)) sgr(c, "01");
        outf(c, "  %-16s %s %4s %4s %9s %6s  \n", "TASK", "S", "PRI", "CPU", "STACK", "%CPU");
        if (tty(c)) sgr(c, "0");
        const int show = batch ? n : (n < 16 ? n : 16);
        for (int k = 0; k < show; k++) {
            const nv_task_row_t &r = rows[k];
            char core[4];
            if (r.core < 0) snprintf(core, sizeof core, "-");
            else snprintf(core, sizeof core, "%d", r.core);
            outf(c, "  %-16.16s %c %4u %4s %9u %6.1f\n", r.name, kState[r.state < 6 ? r.state : 5],
                 (unsigned)r.prio, core, (unsigned)r.stack_free, (double)r.cpu_pct);
        }
        if (iters >= 0 && it + 1 >= iters) break;
        const int64_t end = esp_timer_get_time() + (int64_t)(delay * 1e6);
        while (esp_timer_get_time() < end && !cancelled()) vTaskDelay(pdMS_TO_TICKS(50));
    }
    heap_caps_free(rows);
    return cancelled() ? 130 : 0;
}

// ---------------------------------------------------------------- command table

struct Builtin {
    const char *name;
    int (*fn)(Ctx &);
    const char *usage;
    const char *desc;
};

const Builtin kBuiltins[] = {
    {"apps", b_apps, "apps", "list installed terminal programs"},
    {"basename", b_basename, "basename NAME [SUFFIX]", "strip directory and suffix"},
    {"bl", b_bl, "bl 0-100", "set the backlight"},
    {"cat", b_cat, "cat [-n] [FILE...]", "print files"},
    {"cd", b_cd, "cd [DIR|-]", "change directory"},
    {"clear", b_clear, "clear", "clear the screen"},
    {"cp", b_cp, "cp [-rnv] SRC... DEST", "copy files and directories"},
    {"curl", b_curl, "curl [-sLfO] [-o FILE] URL", "transfer a URL (HTTP/HTTPS)"},
    {"date", b_date, "date [+FORMAT]", "print the date and time"},
    {"df", b_df, "df [-h]", "free space on each volume"},
    {"dirname", b_dirname, "dirname NAME", "strip the last path component"},
    {"dmesg", b_dmesg, "dmesg", "kernel log"},
    {"du", b_du, "du [-shc] [PATH...]", "disk usage"},
    {"echo", b_echo, "echo [-ne] [TEXT...]", "print text"},
    {"env", b_env, "env", "print the environment"},
    {"exit", b_exit, "exit", "close the terminal"},
    {"export", b_export, "export NAME=VALUE...", "set variables"},
    {"false", b_false, "false", "exit with status 1"},
    {"find", b_find, "find [PATH...] [-name PAT] [-iname PAT] [-type f|d] [-maxdepth N]", "search for files"},
    {"free", b_free, "free [-hkm]", "memory usage"},
    {"grep", b_grep, "grep [-ivnclrFqHho] PATTERN [FILE...]", "print lines matching a pattern"},
    {"head", b_head, "head [-n N] [FILE...]", "first lines"},
    {"help", b_help, "help [COMMAND]", "list commands / show usage"},
    {"history", b_history, "history [-c]", "command history"},
    {"host", b_host, "host NAME", "DNS lookup"},
    {"hostname", b_hostname, "hostname", "print the host name"},
    {"i2cdetect", b_i2cdetect, "i2cdetect", "scan the I2C bus"},
    {"ip", b_ip, "ip", "network address and link"},
    {"ls", b_ls, "ls [-laAhtSr1dF] [PATH...]", "list directory contents"},
    {"man", b_help, "man COMMAND", "show usage"},
    {"md5sum", b_hash, "md5sum [FILE...]", "MD5 checksums"},
    {"mkdir", b_mkdir, "mkdir [-pv] DIR...", "make directories"},
    {"mv", b_mv, "mv [-nv] SRC... DEST", "move or rename"},
    {"open", b_open, "open FILE", "open a file in its app"},
    {"ping", b_ping, "ping [-c COUNT] HOST", "send ICMP echo requests"},
    {"ps", b_ps, "ps", "system services"},
    {"pwd", b_pwd, "pwd", "print the working directory"},
    {"reboot", b_reboot, "reboot", "restart the device"},
    {"rm", b_rm, "rm [-rfv] FILE...", "remove files or directories"},
    {"rmdir", b_rmdir, "rmdir DIR...", "remove empty directories"},
    {"sensors", b_sensors, "sensors", "chip temperature"},
    {"sha1sum", b_hash, "sha1sum [FILE...]", "SHA-1 checksums"},
    {"sha256sum", b_hash, "sha256sum [FILE...]", "SHA-256 checksums"},
    {"sleep", b_sleep, "sleep SECONDS", "wait"},
    {"sort", b_sort, "sort [-rnuf] [FILE...]", "sort lines"},
    {"stat", b_stat, "stat FILE...", "file status"},
    {"tail", b_tail, "tail [-n N|+N] [FILE...]", "last lines"},
    {"top", b_top, "top [-b] [-n N] [-d SECONDS]", "live task and CPU view"},
    {"touch", b_touch, "touch FILE...", "create a file / update its time"},
    {"tree", b_tree, "tree [-ad] [-L N] [DIR]", "directory tree"},
    {"true", b_true, "true", "exit with status 0"},
    {"type", b_which, "type NAME...", "how a name would be run"},
    {"uname", b_uname, "uname [-asnrmo]", "system information"},
    {"uniq", b_uniq, "uniq [-cdi] [FILE...]", "drop repeated lines"},
    {"unset", b_unset, "unset NAME...", "remove variables"},
    {"uptime", b_uptime, "uptime", "time since boot"},
    {"usb", b_usb, "usb [host|device]", "USB port mode"},
    {"wc", b_wc, "wc [-lwc] [FILE...]", "count lines, words, bytes"},
    {"wget", b_wget, "wget [-q] [-O FILE] URL", "download a file"},
    {"which", b_which, "which NAME...", "locate a command"},
    {"whoami", b_whoami, "whoami", "print the user name"},
    {"xxd", b_xxd, "xxd [-l N] [FILE]", "hex dump"},
};

// Names that behave like their GNU twins.
const struct { const char *alias; const char *name; } kAliases[] = {
    {"cls", "clear"}, {"dir", "ls"}, {"ll", "ls"}, {"log", "dmesg"}, {"temp", "sensors"},
    {"ifconfig", "ip"}, {"wifi", "ip"}, {"mem", "free"}, {"i2c", "i2cdetect"}, {"ver", "uname"},
    {"version", "uname"}, {"services", "ps"}, {"hexdump", "xxd"}, {"programs", "apps"},
    {"xdg-open", "open"}, {"logout", "exit"}, {"printenv", "env"}, {"set", "env"},
    {"restart", "reboot"}, {"nslookup", "host"}, {"htop", "top"},
};

const Builtin *find_builtin(const char *name) {
    for (const auto &a : kAliases) if (!strcmp(a.alias, name)) { name = a.name; break; }
    for (const Builtin &b : kBuiltins) if (!strcmp(b.name, name)) return &b;
    return nullptr;
}

int b_help(Ctx &c) {
    if (c.argc > 1) {
        const Builtin *b = find_builtin(c.argv[1]);
        if (!b) { errf(c, "help: no help topics match '%s'\n", c.argv[1]); return 1; }
        outf(c, "%s: %s\n    %s\n", b->name, b->usage, b->desc);
        return 0;
    }
    outf(c, "NucleoOS shell. Built-in commands (help NAME for usage):\n\n");
    const int n = (int)(sizeof kBuiltins / sizeof kBuiltins[0]);
    const int half = (n + 1) / 2;
    const bool wide = term_tty_cols() >= 90 || !tty(c);
    for (int i = 0; i < (wide ? half : n); i++) {
        outf(c, "  %-10s %-34.34s", kBuiltins[i].name, kBuiltins[i].desc);
        if (wide && i + half < n) outf(c, "  %-10s %s", kBuiltins[i + half].name, kBuiltins[i + half].desc);
        wr(c.out, "\n", 1);
    }
    outf(c, "\nSyntax: 'quotes' \"$VAR\" ~ * ? [..]  |  > >> < 2> 2>&1  ; && ||  NAME=value  # comment\n");
    outf(c, "Programs: 'apps' lists terminal programs (Lua, SQLite, ...); they read and write under\n"
            "/sdcard/home, which they see as '/'. ^C interrupts, ^D ends their input.\n");
    return 0;
}

int b_which(Ctx &c) {
    const bool type = !strcmp(c.argv[0], "type");
    int st = 0;
    for (int i = 1; i < c.argc; i++) {
        const char *n = c.argv[i];
        nv_wasm_app_t app;
        if (find_builtin(n)) {
            if (type) outf(c, "%s is a shell builtin\n", n);
            else outf(c, "%s: shell built-in command\n", n);
        } else if (nv_wasm_load_manifest(n, &app)) {
            if (type) outf(c, "%s is /sdcard/apps/%s\n", n, app.id);
            else outf(c, "/sdcard/apps/%s\n", app.id);
        } else {
            if (type) errf(c, "type: %s: not found\n", n);
            st = 1;
        }
    }
    return st;
}

// ---------------------------------------------------------------- lexer

enum TokT : uint8_t { T_WORD, T_PIPE, T_AND, T_OR, T_SEMI, T_GT, T_GTGT, T_LT, T_ERR, T_ERRAPP, T_ERR2OUT, T_BG };
struct Tok {
    TokT  t;
    char *w;     // T_WORD: the expanded word
    char *q;     // per character: 1 = came from quotes (not a glob character)
    bool  quoted_any;
};

// Lex + expand one line into tokens. Returns the count, -1 with *err on a syntax error.
int lex(const char *s, Tok *toks, int max, const char **err) {
    int n = 0;
    char *w = (char *)ps_alloc(kLineCap * 2), *q = (char *)ps_alloc(kLineCap * 2);
    if (!w || !q) { heap_caps_free(w); heap_caps_free(q); *err = "out of memory"; return -1; }
    const size_t cap = kLineCap * 2 - 1;
    auto op = [&](TokT t) { if (n < max) { toks[n] = Tok{t, nullptr, nullptr, false}; n++; } };
    int rc = 0;
    while (true) {
        while (*s == ' ' || *s == '\t') s++;
        if (!*s || *s == '#') break;
        if (*s == '|') { if (s[1] == '|') { op(T_OR); s += 2; } else { op(T_PIPE); s++; } continue; }
        if (*s == '&') { if (s[1] == '&') { op(T_AND); s += 2; } else { op(T_BG); s++; } continue; }
        if (*s == ';') { op(T_SEMI); s++; continue; }
        if (*s == '>') { if (s[1] == '>') { op(T_GTGT); s += 2; } else { op(T_GT); s++; } continue; }
        if (*s == '<') { op(T_LT); s++; continue; }
        if (*s == '2' && s[1] == '>') {
            if (s[2] == '&' && s[3] == '1') { op(T_ERR2OUT); s += 4; }
            else if (s[2] == '>') { op(T_ERRAPP); s += 3; }
            else { op(T_ERR); s += 2; }
            continue;
        }
        size_t len = 0;
        bool any = false;
        auto put = [&](char ch, char quoted) { if (len < cap) { w[len] = ch; q[len] = quoted; len++; } };
        auto put_str = [&](const char *v, char quoted) { if (v) while (*v) put(*v++, quoted); };
        // $NAME ${NAME} $? — returns the new position.
        auto dollar = [&](const char *p, char quoted) -> const char * {
            p++;
            char name[32];
            size_t k = 0;
            if (*p == '?') { put_str(var_get("?"), 1); return p + 1; }
            if (*p == '{') {
                p++;
                while (*p && *p != '}' && k < sizeof name - 1) name[k++] = *p++;
                if (*p == '}') p++;
            } else {
                while ((isalnum((unsigned char)*p) || *p == '_') && k < sizeof name - 1) name[k++] = *p++;
            }
            name[k] = '\0';
            if (!k) { put('$', quoted); return p; }
            put_str(var_get(name), 1);
            return p;
        };
        while (*s && !strchr(" \t|&;<>", *s)) {
            if (*s == '\'') {
                s++;
                any = true;
                while (*s && *s != '\'') put(*s++, 1);
                if (!*s) { *err = "unexpected EOF while looking for matching `''"; rc = -1; break; }
                s++;
            } else if (*s == '"') {
                s++;
                any = true;
                while (*s && *s != '"') {
                    if (*s == '\\' && s[1] && strchr("\"\\$`", s[1])) { put(s[1], 1); s += 2; continue; }
                    if (*s == '$') { s = dollar(s, 1); continue; }
                    put(*s++, 1);
                }
                if (!*s) { *err = "unexpected EOF while looking for matching `\"'"; rc = -1; break; }
                s++;
            } else if (*s == '\\') {
                if (s[1]) { put(s[1], 1); s += 2; } else s++;
                any = true;
            } else if (*s == '$') {
                s = dollar(s, 0);
            } else if (*s == '~' && len == 0 && (!s[1] || s[1] == '/' || strchr(" \t|&;<>", s[1]))) {
                put_str(kHome, 1);
                s++;
            } else {
                put(*s++, 0);
            }
        }
        if (rc) break;
        if (!len && !any) continue;   // $EMPTY expands to nothing
        if (n >= max) { *err = "line too long"; rc = -1; break; }
        Tok &t = toks[n];
        t.t = T_WORD;
        t.w = a_strndup(w, len);
        t.q = a_strndup(q, len);
        t.quoted_any = any;
        if (!t.w || !t.q) { *err = "line too long"; rc = -1; break; }
        n++;
    }
    heap_caps_free(w);
    heap_caps_free(q);
    return rc ? -1 : n;
}

bool has_glob(const Tok &t) {
    for (size_t i = 0; t.w[i]; i++)
        if (!t.q[i] && (t.w[i] == '*' || t.w[i] == '?' || t.w[i] == '[')) return true;
    return false;
}

// Expand an unquoted glob in the last path component into argv. Returns the count added
// (0 = no match: the word is kept as typed, like bash).
int glob_expand(const Tok &t, char **argv, int room) {
    const char *slash = strrchr(t.w, '/');
    char dir_show[kPath] = "", dir[kPath];
    const char *pat = t.w;
    if (slash) {
        snprintf(dir_show, sizeof dir_show, "%.*s", (int)(slash - t.w + 1), t.w);
        pat = slash + 1;
    }
    for (const char *p = dir_show; *p; p++) if (*p == '*' || *p == '?' || *p == '[') return 0;
    resolve(dir_show[0] ? dir_show : ".", dir, sizeof dir);
    Ent *e;
    const int n = read_dir(dir, pat[0] == '.', false, &e);
    if (n <= 0) { if (n == 0) heap_caps_free(e); return 0; }
    qsort(e, n, sizeof(Ent), ent_cmp_name);
    int added = 0;
    for (int i = 0; i < n && added < room; i++) {
        if (!wild(pat, e[i].name, false)) continue;
        const size_t l = strlen(dir_show) + strlen(e[i].name);
        char *s = a_alloc(l + 1);
        if (!s) break;
        snprintf(s, l + 1, "%s%s", dir_show, e[i].name);
        argv[added++] = s;
    }
    heap_caps_free(e);
    return added;
}

// ---------------------------------------------------------------- executor

struct Stage {
    int   argc = 0;
    char *argv[kMaxArgs + 1];
    const char *in_file = nullptr, *out_file = nullptr, *err_file = nullptr;
    bool  out_append = false, err_append = false, err_to_out = false;
};

FILE *open_out(Ctx &c, const char *name, bool append, ShSink &sink) {
    if (!strcmp(name, "/dev/null")) { sink.k = SH_NULL; return nullptr; }
    char p[kPath];
    resolve(name, p, sizeof p);
    FILE *f = fopen(p, append ? "ab" : "wb");
    if (!f) { errf(c, "sh: %s: %s\n", name, is_dir(p) ? "Is a directory" : "No such file or directory"); return nullptr; }
    sink.k = SH_FILE;
    sink.f = f;
    return f;
}

// Quote argv back into the command line a WASI program parses ("double quotes group words").
void join_args(char **argv, int argc, char *out, size_t cap) {
    size_t n = 0;
    out[0] = '\0';
    for (int i = 0; i < argc && n + 4 < cap; i++) {
        if (i) out[n++] = ' ';
        const bool q = !argv[i][0] || strpbrk(argv[i], " \t\"'");
        if (q) out[n++] = '"';
        for (const char *s = argv[i]; *s && n + 3 < cap; s++) {
            if (*s == '"' || *s == '\\') out[n++] = '\\';
            out[n++] = *s;
        }
        if (q && n + 1 < cap) out[n++] = '"';
        out[n] = '\0';
    }
}

int run_stage(Stage &st, const char *in, size_t in_len, bool has_in, const ShSink &out,
              const ShSink &err, bool interactive) {
    Ctx c;
    c.argc = st.argc;
    c.argv = st.argv;
    c.in = in;
    c.in_len = in_len;
    c.has_in = has_in;
    c.out = out;
    c.err = err;
    const char *name = st.argv[0];
    if (const Builtin *b = find_builtin(name)) {
        VolsHold hold;
        return b->fn(c);
    }
    nv_wasm_app_t app;
    if (!strchr(name, '/') && nv_wasm_load_manifest(name, &app)) {
        char args[256];
        join_args(st.argv + 1, st.argc - 1, args, sizeof args);
        const int r = term_prog_run(app.id, args, has_in ? (in ? in : "") : nullptr,
                                    has_in ? in_len : 0, out.k == SH_TTY ? nullptr : &out);
        if (r == 126) errf(c, "%s: graphical app - open it from Home\n", name);
        return r;
    }
    errf(c, "%s: command not found\n", name);
    (void)interactive;
    return 127;
}

int run_pipeline(Stage *stages, int n) {
    ShBuf prev;
    bool has_prev = false;
    int status = 0;
    for (int i = 0; i < n && !cancelled(); i++) {
        Stage &st = stages[i];
        Ctx ec;               // for redirection errors
        ec.err = ShSink{};
        ShBuf filein, cap;
        const char *in = has_prev ? prev.p : nullptr;
        size_t in_len = has_prev ? prev.n : 0;
        bool has_in = has_prev;
        if (st.in_file) {
            if (strcmp(st.in_file, "/dev/null")) {
                char p[kPath];
                resolve(st.in_file, p, sizeof p);
                VolsHold hold;
                FILE *f = fopen(p, "rb");
                if (!f) {
                    errf(ec, "sh: %s: No such file or directory\n", st.in_file);
                    status = 1;
                    buf_free(prev);
                    has_prev = false;
                    continue;
                }
                char chunk[2048];
                size_t k;
                while ((k = fread(chunk, 1, sizeof chunk, f)) > 0 && !filein.trunc) buf_put(filein, chunk, k);
                fclose(f);
            }
            in = filein.p;
            in_len = filein.n;
            has_in = true;
        }
        ShSink out, err;
        FILE *of = nullptr, *ef = nullptr;
        VolsHold *hold = nullptr;
        if (st.out_file || st.err_file) hold = new VolsHold();
        bool bad = false;
        if (st.out_file) { of = open_out(ec, st.out_file, st.out_append, out); bad = !of && out.k != SH_NULL; }
        else if (i < n - 1) { out.k = SH_BUF; out.buf = &cap; }
        if (st.err_to_out) err = out;
        else if (st.err_file && !bad) { ef = open_out(ec, st.err_file, st.err_append, err); bad = !ef && err.k != SH_NULL; }
        if (bad) status = 1;
        else status = run_stage(st, in, in_len, has_in, out, err, i == 0 && !has_in);
        if (of) fclose(of);
        if (ef) fclose(ef);
        delete hold;
        buf_free(filein);
        buf_free(prev);
        prev = cap;
        has_prev = i < n - 1;
    }
    buf_free(prev);
    return status;
}

// Split a line at its top-level ; && || (outside quotes, escapes and # comments): segment k is
// seg[k] / len[k], con[k] the connector before it (T_SEMI for the first).
int split_list(const char *s, const char **seg, size_t *len, TokT *con, int max) {
    int n = 0;
    const char *start = s;
    TokT c = T_SEMI;
    char q = 0;
    for (const char *p = s;; p++) {
        if (*p && q) {
            if (*p == '\\' && q == '"' && p[1]) p++;
            else if (*p == q) q = 0;
            continue;
        }
        if (*p == '\\' && p[1]) { p++; continue; }
        if (*p == '\'' || *p == '"') { q = *p; continue; }
        const bool comment = *p == '#' && (p == s || p[-1] == ' ' || p[-1] == '\t');
        const bool end = !*p || comment;
        TokT nc = T_SEMI;
        int adv = 1;
        if (!end) {
            if (*p == ';') nc = T_SEMI;
            else if (*p == '&' && p[1] == '&') { nc = T_AND; adv = 2; }
            else if (*p == '|' && p[1] == '|') { nc = T_OR; adv = 2; }
            else continue;
        }
        if (n < max) { seg[n] = start; len[n] = (size_t)(p - start); con[n] = c; n++; }
        if (end) break;
        c = nc;
        p += adv - 1;
        start = p + 1;
    }
    return n;
}

// Build the pipeline of one list segment (stages separated by |) and run it.
void run_tokens(Tok *toks, int nt, Stage *stages) {
    int ns = 0;
    stages[0] = Stage{};
    bool assign_only = true;
    for (int i = 0; i < nt; i++) {
        Tok &t = toks[i];
        Stage &st = stages[ns];
        switch (t.t) {
            case T_PIPE:
                if (ns + 1 < kMaxStage) stages[++ns] = Stage{};   // more stages join the last one
                continue;
            case T_GT: case T_GTGT: st.out_file = toks[++i].w; st.out_append = t.t == T_GTGT; continue;
            case T_LT: st.in_file = toks[++i].w; continue;
            case T_ERR: case T_ERRAPP: st.err_file = toks[++i].w; st.err_append = t.t == T_ERRAPP; continue;
            case T_ERR2OUT: st.err_to_out = true; continue;
            case T_WORD: break;
            default: continue;
        }
        if (st.argc >= kMaxArgs) continue;
        if (has_glob(t)) {
            const int k = glob_expand(t, st.argv + st.argc, kMaxArgs - st.argc);
            if (k) { st.argc += k; assign_only = false; continue; }
        }
        const char *eq = strchr(t.w, '=');
        if (!(st.argc == 0 && eq && var_name_ok(t.w, (size_t)(eq - t.w)) && !t.q[0])) assign_only = false;
        st.argv[st.argc++] = t.w;
    }
    ns++;
    if (assign_only && ns == 1 && stages[0].argc) {   // NAME=value [NAME=value...]
        for (int k = 0; k < stages[0].argc; k++) {
            char *a = stages[0].argv[k];
            char *eq = strchr(a, '=');
            *eq = '\0';
            var_set(a, eq + 1);
        }
        S->status = 0;
        return;
    }
    for (int k = 0; k < ns; k++) if (!stages[k].argc) { S->status = 0; return; }
    for (int k = 0; k < ns; k++) stages[k].argv[stages[k].argc] = nullptr;
    S->status = run_pipeline(stages, ns);
}

void run_line(const char *line) {
    S->arena_n = 0;
    Tok *toks = (Tok *)a_alloc(sizeof(Tok) * kMaxTok);
    Stage *stages = (Stage *)ps_alloc(sizeof(Stage) * kMaxStage);
    const char *err = nullptr;
    const int nt = toks && stages ? lex(line, toks, kMaxTok, &err) : -1;
    ShSink tty_err;
    if (nt < 0) {
        char b[160];
        const int k = snprintf(b, sizeof b, "sh: %s\n", err ? err : "out of memory");
        sh_sink_write(tty_err, b, (size_t)k);
        S->status = 2;
        heap_caps_free(stages);
        return;
    }
    // Syntax check: operators need words around them.
    for (int i = 0; i < nt; i++) {
        const TokT t = toks[i].t;
        const bool redir = t == T_GT || t == T_GTGT || t == T_LT || t == T_ERR || t == T_ERRAPP;
        const bool conn = t == T_PIPE || t == T_AND || t == T_OR;
        const char *bad = nullptr;
        if (t == T_BG) bad = "&";
        else if (redir && (i + 1 >= nt || toks[i + 1].t != T_WORD)) bad = "newline";
        else if (conn && (i == 0 || i + 1 >= nt || (toks[i - 1].t != T_WORD && toks[i - 1].t != T_ERR2OUT))) bad = t == T_PIPE ? "|" : t == T_AND ? "&&" : "||";
        else if (t == T_SEMI && i == 0) bad = ";";
        if (bad) {
            char b[120];
            const int k = !strcmp(bad, "&")
                ? snprintf(b, sizeof b, "sh: background jobs (&) are not supported\n")
                : snprintf(b, sizeof b, "sh: syntax error near unexpected token `%s'\n", bad);
            sh_sink_write(tty_err, b, (size_t)k);
            S->status = 2;
            heap_caps_free(stages);
            return;
        }
    }
    // Run the list one segment at a time, expanding each only when it runs: `ls x; echo $?`
    // must see the status ls left.
    constexpr int kMaxSeg = 32;
    const char *seg[kMaxSeg];
    size_t seg_len[kMaxSeg];
    TokT seg_con[kMaxSeg];
    const int nseg = split_list(line, seg, seg_len, seg_con, kMaxSeg);
    char *text = (char *)ps_alloc(kLineCap);
    for (int k = 0; k < nseg && text && !cancelled(); k++) {
        const TokT conn = seg_con[k];
        const bool run = conn == T_SEMI || (conn == T_AND && S->status == 0) || (conn == T_OR && S->status != 0);
        if (!run) continue;
        snprintf(text, kLineCap, "%.*s", (int)seg_len[k], seg[k]);
        S->arena_n = 0;
        toks = (Tok *)a_alloc(sizeof(Tok) * kMaxTok);
        const int n = toks ? lex(text, toks, kMaxTok, &err) : -1;
        if (n <= 0) continue;
        run_tokens(toks, n, stages);
    }
    heap_caps_free(text);
    if (cancelled()) S->status = 130;
    heap_caps_free(stages);
}

void sh_task(void *) {
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        run_line(S->line);
        s_busy = false;
        s_done++;
    }
}

}  // namespace

// ================================================================= public

void sh_sink_write(const ShSink &s, const char *p, size_t n) {
    switch (s.k) {
        case SH_TTY:  term_tty_write(p, n); break;
        case SH_BUF:  if (s.buf) buf_put(*s.buf, p, n); break;
        case SH_FILE: if (s.f) fwrite(p, 1, n, s.f); break;
        default: break;
    }
}

bool sh_start(void) {
    if (s_task) return true;
    if (!S) {
        S = (State *)heap_caps_calloc(1, sizeof(State), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!S) return false;
        S->nprogs = -1;
        // Start at home when the card is there (created on first use), else at the root.
        uint64_t t, f;
        if (nv_sd_info(&t, &f) && nv_sd_session_begin()) {
            if (!is_dir(kHome)) mkdir(kHome, 0775);
            const bool home = is_dir(kHome);
            nv_sd_session_end();
            snprintf(S->cwd, sizeof S->cwd, "%s", home ? kHome : "/sdcard");
        } else {
            snprintf(S->cwd, sizeof S->cwd, "/");
        }
    }
    // Never writes flash/NVS itself (term_ui_call does that on the LVGL thread) -> PSRAM stack.
    if (xTaskCreateWithCaps(sh_task, "sh", 24576, nullptr, 3, &s_task,
                            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        s_task = nullptr;
        return false;
    }
    return true;
}

bool sh_busy(void) { return s_busy.load(); }
uint32_t sh_jobs_done(void) { return s_done.load(); }

bool sh_run(const char *line) {
    if (!s_task || s_busy.load()) return false;
    snprintf(S->line, sizeof S->line, "%s", line);
    s_cancel = false;
    s_busy = true;
    xTaskNotifyGive(s_task);
    return true;
}

void sh_interrupt(void) { if (s_busy.load()) s_cancel = true; }

void sh_prompt_dir(char *out, size_t cap) {
    if (!S) { snprintf(out, cap, "~"); return; }
    const size_t hl = strlen(kHome);
    if (!strncmp(S->cwd, kHome, hl) && (S->cwd[hl] == '/' || !S->cwd[hl]))
        snprintf(out, cap, "~%s", S->cwd + hl);
    else
        snprintf(out, cap, "%s", S->cwd);
}

int sh_complete(const char *line, size_t cursor, char *ins, size_t ins_cap, char *list, size_t list_cap) {
    ins[0] = '\0';
    list[0] = '\0';
    if (!S || s_busy.load()) return 0;
    // The word under completion: back from the cursor to a space or an operator.
    size_t ws = cursor;
    while (ws > 0 && !strchr(" \t|&;<>", line[ws - 1])) ws--;
    size_t k = ws;
    while (k > 0 && (line[k - 1] == ' ' || line[k - 1] == '\t')) k--;
    const bool command = k == 0 || strchr("|&;", line[k - 1]);
    char word[kPath];
    snprintf(word, sizeof word, "%.*s", (int)(cursor - ws), line + ws);

    // Candidates (arena names); `dirs` marks directories.
    S->arena_n = 0;
    constexpr int kMaxC = 512;
    const char **cand = (const char **)a_alloc(sizeof(char *) * kMaxC);
    bool *dirs = (bool *)a_alloc(sizeof(bool) * kMaxC);
    if (!cand || !dirs) return 0;
    int n = 0;
    const char *prefix;   // the part of the word being matched
    if (command && !strchr(word, '/')) {
        prefix = word;
        const size_t pl = strlen(prefix);
        for (const Builtin &b : kBuiltins)
            if (!strncmp(b.name, prefix, pl) && n < kMaxC) { dirs[n] = false; cand[n++] = b.name; }
        if (S->nprogs < 0) {
            constexpr int kMax = 64;
            auto *apps = (nv_wasm_app_t *)ps_alloc(sizeof(nv_wasm_app_t) * kMax);
            S->nprogs = 0;
            if (apps) {
                const int m = nv_wasm_scan(apps, kMax);
                for (int i = 0; i < m && S->nprogs < 64; i++)
                    if (apps[i].console) snprintf(S->progs[S->nprogs++], sizeof S->progs[0], "%s", apps[i].id);
                heap_caps_free(apps);
            }
        }
        for (int i = 0; i < S->nprogs; i++)
            if (!strncmp(S->progs[i], prefix, pl) && n < kMaxC) { dirs[n] = false; cand[n++] = S->progs[i]; }
    } else {
        // A path: list the directory part, match the rest.
        const char *slash = strrchr(word, '/');
        char dir_typed[kPath] = "", dir[kPath];
        prefix = slash ? slash + 1 : word;
        if (slash) snprintf(dir_typed, sizeof dir_typed, "%.*s", (int)(slash - word + 1), word);
        char expanded[kPath];
        if (dir_typed[0] == '~') snprintf(expanded, sizeof expanded, "%s%s", kHome, dir_typed + 1);
        else snprintf(expanded, sizeof expanded, "%s", dir_typed);
        resolve(expanded[0] ? expanded : ".", dir, sizeof dir);
        if (!strcmp(word, "~")) { snprintf(ins, ins_cap, "/"); return 1; }
        VolsHold hold;
        Ent *e;
        const int m = read_dir(dir, prefix[0] == '.', false, &e);
        if (m > 0) {
            qsort(e, m, sizeof(Ent), ent_cmp_name);
            const size_t pl = strlen(prefix);
            for (int i = 0; i < m && n < kMaxC; i++)
                if (!strncmp(e[i].name, prefix, pl)) { dirs[n] = e[i].dir; cand[n++] = e[i].name; }
        }
        if (m >= 0) heap_caps_free(e);
    }
    if (!n) return 0;
    // Longest common prefix beyond what is typed.
    const size_t pl = strlen(prefix);
    size_t common = strlen(cand[0]);
    for (int i = 1; i < n; i++) {
        size_t j = 0;
        while (j < common && cand[i][j] == cand[0][j]) j++;
        common = j;
    }
    // Never cut a UTF-8 sequence in half.
    while (common > pl && ((unsigned char)cand[0][common] & 0xC0) == 0x80) common--;
    size_t o = 0;
    auto add = [&](char ch) { if (o + 1 < ins_cap) { ins[o++] = ch; ins[o] = '\0'; } };
    for (size_t j = pl; j < common; j++) {
        const char ch = cand[0][j];
        if (strchr(" \t'\"\\|&;<>*?[$", ch)) add('\\');   // shell-quote as bash does
        add(ch);
    }
    if (n == 1) {
        add(dirs[0] ? '/' : ' ');
        return 1;
    }
    if (o) return n;   // progress made: the list waits for the next Tab
    size_t l = 0;
    for (int i = 0; i < n && l + 2 < list_cap; i++) {
        l += (size_t)snprintf(list + l, list_cap - l, "%s%s\n", cand[i], dirs[i] ? "/" : "");
        if (l >= list_cap) { l = list_cap - 1; break; }
    }
    return n;
}
