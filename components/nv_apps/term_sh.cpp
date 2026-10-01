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
// cp mv touch xxd basename dirname realpath edit less), text tools (sed awk cut tr tee rev tac nl seq
// printf base64), shell built-ins (cd pwd echo env export unset history which type command help
// man true false test [ expr sleep time watch xargs clear reset stty exit), system (uname hostname whoami id
// nproc date uptime free ps dmesg sensors ip i2cdetect usb bl apps open reboot top), network (curl
// wget ping host), hashes (md5sum sha1sum sha256sum) and WASI terminal programs (Lua, SQLite, ...)
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
#include "nv_app.h"          // store remove: drop the Home tile live
#include "nv_sysmon.h"
#include "nv_ui.h"
#include "nv_i18n.h"        // ha say: the language for Assist         // launch: open an app by id
#include "nv_appstore.h"   // store: search / install apps from the app store
#include "nv_apps.h"       // nv_apps_store_installed: the launcher tile after an install
#include "nv_ime.h"        // type / key: text and keys into the focused field (GUI automation)
#include "nv_mem_attr.h"   // NV_PSRAM_BSS
#include "nv_notify.h"     // notify: a system notification
#include "nv_anima_system.h" // vol: the same path as ANIMA's set_volume (persisted)
#include "nucleo_anima.h"  // tg: a Telegram message to the paired chat
#include "cJSON.h"         // jq
#include "mdns.h"          // dev scan: Shelly / WLED on the LAN
#include "esp_lvgl_port.h"

#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "ping/ping_sock.h"
#include "lwip/netdb.h"
#include "lwip/inet.h"
#include "lwip/ip_addr.h"
#include "mbedtls/md.h"
#include "mbedtls/base64.h"
#include "freertos/semphr.h"

#include "driver/i2c_master.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <atomic>
#include <cctype>
#include <climits>
#include <cstdarg>
#include <cstdlib>
#include <cstring>
#include <new>
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
std::atomic<int>      s_last_status{0};   // S->status of the last finished line, for any task
// Headless run (sh_exec_capture, ANIMA's shell tool): while set, everything bound for the screen goes
// here instead, plain (tty() is false, so no colours or column layout), and nothing reads the keys.
ShBuf *volatile s_capture = nullptr;
constexpr size_t kCaptureCap = 64 * 1024;

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

bool tty(const Ctx &c) { return c.out.k == SH_TTY && !s_capture; }
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

// Defined with the text tools below.
int getopts(Ctx &c, const char *allowed, const char *valued, Flags &f, const char **vals);
char esc_byte(const char *p, int *used);

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
    int i = getflags(c, "nqvc", f, "nc", &nv);
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
        if (f.has('c')) {   // bytes: -c N, tail -c +N
            const size_t k = (size_t)n < len ? (size_t)n : len;
            if (!tail) wr(c.out, p, k);
            else if (from_start) { const size_t s0 = n > 0 ? (size_t)n - 1 : 0; if (s0 < len) wr(c.out, p + s0, len - s0); }
            else wr(c.out, p + len - k, k);
        } else if (!tail) {
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

// A match of re in [from, e) on the line starting at `line`; under -w only one standing between
// non-word characters.
bool grep_match(const Re &re, const Flags &f, const char *line, const char *from, const char *e,
                const char **ms, const char **me) {
    auto word = [](char ch) { return isalnum((unsigned char)ch) || ch == '_'; };
    for (const char *p = from; p <= e;) {
        if (!re_search(re, p, e, ms, me)) return false;
        if (!f.has('w')) return true;
        const bool lok = *ms == line || !word((*ms)[-1]);
        const bool rok = *me >= e || !word(**me);
        if (lok && rok && *me > *ms) return true;
        if (re.bol) return false;
        p = *ms + 1;
    }
    return false;
}

// grep PATTERN [FILE...]: -i -v -n -c -l -r -F -q -H -h -o -w -e PATTERN -E(accepted)
int grep_buf(Ctx &c, const Re &re, const Flags &f, const char *p, size_t n, const char *name,
             bool show_name, uint64_t &hits) {
    uint64_t count = 0;
    unsigned ln = 0;
    each_line(p, n, [&](const char *s, size_t len) {
        ln++;
        const char *ms = s, *me = s;
        const bool hit = grep_match(re, f, s, s, s + len, &ms, &me) != f.has('v');
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
        if (f.has('o') && !f.has('v') && !tty(c)) {   // every match on its own line
            const char *q = s, *e = s + len;
            while (q <= e && grep_match(re, f, s, q, e, &ms, &me)) {
                if (me > ms) { wr(c.out, ms, (size_t)(me - ms)); wr(c.out, "\n", 1); }
                q = me > ms ? me : me + 1;
                if (re.bol) break;
            }
            return true;
        }
        if (f.has('v') || !tty(c)) {
            wr(c.out, s, len);
        } else {
            // Highlight every match on the line, GNU grep --color style.
            const char *q = s, *e = s + len;
            while (q <= e && grep_match(re, f, s, q, e, &ms, &me)) {
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
    const char *pat = nullptr;
    int i = getflags(c, "ivnclrRFqHhoEwe", f, "e", &pat);
    if (i < 0) return 2;
    if (!pat) {
        if (i >= c.argc) { errf(c, "usage: grep [-ivnclrFqHhow] [-e] PATTERN [FILE...]\n"); return 2; }
        pat = c.argv[i++];
    }
    Re *re = (Re *)ps_alloc(sizeof(Re));
    if (!re) return 2;
    const char *err = nullptr;
    if (!re_compile(*re, pat, f.has('i'), f.has('F'), &err)) {
        errf(c, "grep: %s\n", err);
        heap_caps_free(re);
        return 2;
    }
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
bool g_sort_num, g_sort_fold, g_sort_human;
int  g_sort_key, g_sort_key_end;   // -k K[,E]: fields K..E (0 = the whole line / to the end)
char g_sort_sep;                   // -t C; 0 = runs of blanks separate fields

// Start and end of field k (1-based) of a line.
void sort_field(const char *p, size_t n, int k, const char **fs, const char **fe) {
    const char *s = p, *e = p + n;
    if (g_sort_sep) {
        for (int f = 1; f < k && s < e; f++) {
            const char *m = (const char *)memchr(s, g_sort_sep, (size_t)(e - s));
            s = m ? m + 1 : e;
        }
        const char *m = (const char *)memchr(s, g_sort_sep, (size_t)(e - s));
        *fs = s;
        *fe = m ? m : e;
        return;
    }
    for (int f = 1;; f++) {   // leading blanks belong to no field (as with -b)
        while (s < e && (*s == ' ' || *s == '\t')) s++;
        const char *b = s;
        while (s < e && *s != ' ' && *s != '\t') s++;
        if (f == k || s >= e) { *fs = f == k ? b : e; *fe = f == k ? s : e; return; }
    }
}

void sort_key(const LineRef *l, const char **ks, size_t *kn) {
    if (!g_sort_key) { *ks = l->p; *kn = l->n; return; }
    const char *s, *e, *s2, *e2;
    sort_field(l->p, l->n, g_sort_key, &s, &e);
    if (g_sort_key_end >= g_sort_key) sort_field(l->p, l->n, g_sort_key_end, &s2, &e2);
    else e2 = l->p + l->n;
    if (e2 < s) e2 = s;
    *ks = s;
    *kn = (size_t)(e2 - s);
}

double sort_num(const char *p, size_t n) {
    char b[64];
    const size_t m = n < sizeof b - 1 ? n : sizeof b - 1;
    memcpy(b, p, m);
    b[m] = '\0';
    char *e;
    double v = strtod(b, &e);
    if (g_sort_human) {
        static const char kU[] = "KMGTPE";
        if (*e) if (const char *u = strchr(kU, toupper((unsigned char)*e))) for (long k = 0; k <= u - kU; k++) v *= 1024.0;
    }
    return v;
}

int line_cmp(const void *a, const void *b) {
    const LineRef *x = (const LineRef *)a, *y = (const LineRef *)b;
    const char *xs, *ys;
    size_t xn, yn;
    sort_key(x, &xs, &xn);
    sort_key(y, &ys, &yn);
    if (g_sort_num || g_sort_human) {
        const double u = sort_num(xs, xn), v = sort_num(ys, yn);
        if (u != v) return u < v ? -1 : 1;
    }
    const size_t n = xn < yn ? xn : yn;
    int r = g_sort_fold ? strncasecmp(xs, ys, n) : memcmp(xs, ys, n);
    if (!r) r = xn < yn ? -1 : xn > yn ? 1 : 0;
    if (r || !g_sort_key) return r;
    // Equal keys: the whole line decides (GNU's last-resort comparison).
    const size_t m = x->n < y->n ? x->n : y->n;
    r = memcmp(x->p, y->p, m);
    return r ? r : x->n < y->n ? -1 : x->n > y->n ? 1 : 0;
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
    const char *vals[64] = {};
    int i = getopts(c, "rnufhbkt", "kt", f, vals);
    if (i < 0) return 2;
    g_sort_key = g_sort_key_end = 0;
    g_sort_sep = 0;
    if (const char *k = vals['k' - 'A']) {
        char *e;
        g_sort_key = (int)strtol(k, &e, 10);
        while (isalpha((unsigned char)*e)) {   // -k2n, -k2,2r: per-key flags apply to the sort
            if (*e == 'n') f.m |= 1ull << ('n' - 'A');
            if (*e == 'r') f.m |= 1ull << ('r' - 'A');
            if (*e == 'h') f.m |= 1ull << ('h' - 'A');
            e++;
        }
        if (*e == '.') { strtol(e + 1, &e, 10); }
        if (*e == ',') g_sort_key_end = (int)strtol(e + 1, &e, 10);
        while (isalpha((unsigned char)*e)) {
            if (*e == 'n') f.m |= 1ull << ('n' - 'A');
            if (*e == 'r') f.m |= 1ull << ('r' - 'A');
            if (*e == 'h') f.m |= 1ull << ('h' - 'A');
            e++;
        }
        if (g_sort_key < 1) { errf(c, "sort: invalid number at field start: invalid count at start of '%s'\n", k); return 2; }
    }
    if (const char *t = vals['t' - 'A']) {
        if (strlen(t) != 1 && strcmp(t, "\\t")) { errf(c, "sort: multi-character tab '%s'\n", t); return 2; }
        g_sort_sep = strcmp(t, "\\t") ? t[0] : '\t';
    }
    ShBuf all;
    LineRef *l = nullptr;
    size_t n = 0;
    const int st = collect_lines(c, i, all, &l, &n);
    g_sort_num = f.has('n');
    g_sort_human = f.has('h');
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
    g_sort_num = g_sort_human = false;
    g_sort_key = g_sort_key_end = 0;
    g_sort_sep = 0;
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

// stat [-c FORMAT] FILE...: FORMAT directives %n %s %Y %y %F %%.
int b_stat(Ctx &c) {
    const char *fmt = nullptr;
    int i = 1;
    for (; i < c.argc && c.argv[i][0] == '-' && c.argv[i][1]; i++) {
        const char *a = c.argv[i];
        if (!strcmp(a, "-c") && i + 1 < c.argc) fmt = c.argv[++i];
        else if (!strncmp(a, "-c", 2) && a[2]) fmt = a + 2;
        else if (!strncmp(a, "--format=", 9)) fmt = a + 9;
        else if (!strncmp(a, "--printf=", 9)) fmt = a + 9;
        else if (!strcmp(a, "-L")) {}
        else { errf(c, "stat: invalid option '%s'\n", a); return 1; }
    }
    if (i >= c.argc) { errf(c, "stat: missing operand\n"); return 1; }
    int st = 0;
    for (; i < c.argc; i++) {
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
        if (!fmt) {
            outf(c, "  File: %s\n  Size: %-12llu %s\nModify: %s\n", p, (unsigned long long)s.st_size,
                 dir ? "directory" : "regular file", tm);
            continue;
        }
        for (const char *f = fmt; *f; f++) {
            if (*f == '\\' && f[1]) {
                int used;
                const char b = esc_byte(f + 1, &used);
                if (used) { wr(c.out, &b, 1); f += used; continue; }
            }
            if (*f != '%' || !f[1]) { wr(c.out, f, 1); continue; }
            switch (*++f) {
                case 'n': wr(c.out, c.argv[i]); break;
                case 's': outf(c, "%llu", (unsigned long long)(dir ? 0 : s.st_size)); break;
                case 'Y': outf(c, "%lld", (long long)s.st_mtime); break;
                case 'y': wr(c.out, tm); break;
                case 'F': wr(c.out, dir ? "directory" : "regular file"); break;
                case '%': wr(c.out, "%", 1); break;
                default: wr(c.out, f - 1, 2); break;
            }
        }
        wr(c.out, "\n", 1);
    }
    return st;
}

int b_find(Ctx &c);
int b_tree(Ctx &c);
int b_du(Ctx &c);

struct FindOpt {
    const char *name = nullptr;
    bool iname = false;
    char type = 0;
    int maxd = 1 << 20, mind = 0;
    char szop = 0;           // -size: '+' more, '-' less, '=' exactly (in units, rounded up)
    uint64_t sz = 0, unit = 512;
    char mop = 0;            // -mtime / -mmin
    long mval = 0, munit = 86400;
    bool empty = false;
    time_t newer = 0;        // -newer FILE
    bool has_newer = false;
    bool need_stat() const { return szop || mop || empty || has_newer; }
};

bool find_cmp(char op, uint64_t v, uint64_t ref) { return op == '+' ? v > ref : op == '-' ? v < ref : v == ref; }

void find_walk(Ctx &c, const FindOpt &o, char *path, size_t cap, char *shown, size_t scap, int depth) {
    if (cancelled()) return;
    const bool dir = is_dir(path);
    bool match = depth >= o.mind;
    if (o.name) match = match && wild(o.name, base_name(shown), o.iname);
    if (o.type == 'f') match = match && !dir;
    if (o.type == 'd') match = match && dir;
    if (match && o.need_stat()) {
        struct stat s = {};
        const bool ok = stat(path, &s) == 0 || dir;
        if (!ok) match = false;
        if (match && o.szop) match = !dir && find_cmp(o.szop, ((uint64_t)s.st_size + o.unit - 1) / o.unit, o.sz);
        if (match && o.mop) {
            const time_t now = time(nullptr);
            const long age = s.st_mtime && now > s.st_mtime ? (long)((now - s.st_mtime) / o.munit) : 0;
            match = find_cmp(o.mop, (uint64_t)age, (uint64_t)o.mval);
        }
        if (match && o.has_newer) match = s.st_mtime > o.newer;
        if (match && o.empty) {
            if (dir) {
                DIR *d = opendir(path);
                bool any = false;
                if (d) {
                    while (struct dirent *e = readdir(d)) {
                        if (strcmp(e->d_name, ".") && strcmp(e->d_name, "..")) { any = true; break; }
                    }
                    closedir(d);
                }
                match = !any;
            } else {
                match = s.st_size == 0;
            }
        }
    }
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
        else if (!strcmp(a, "-size") && v) {
            o.szop = v[0] == '+' || v[0] == '-' ? v[0] : '=';
            char *e;
            o.sz = strtoull(v + (o.szop != '=' ? 1 : 0), &e, 10);
            switch (*e) {
                case 'c': o.unit = 1; break;
                case 'w': o.unit = 2; break;
                case 'k': o.unit = 1024; break;
                case 'M': o.unit = 1024 * 1024; break;
                case 'G': o.unit = 1024ull * 1024 * 1024; break;
                default: o.unit = 512; break;
            }
            i++;
        } else if ((!strcmp(a, "-mtime") || !strcmp(a, "-mmin")) && v) {
            o.mop = v[0] == '+' || v[0] == '-' ? v[0] : '=';
            o.mval = atol(v + (o.mop != '=' ? 1 : 0));
            o.munit = a[2] == 'm' && a[3] == 'i' ? 60 : 86400;
            i++;
        } else if (!strcmp(a, "-newer") && v) {
            char np[kPath];
            resolve(v, np, sizeof np);
            struct stat s = {};
            if (stat(np, &s) != 0) { errf(c, "find: '%s': No such file or directory\n", v); return 1; }
            o.newer = s.st_mtime;
            o.has_newer = true;
            i++;
        }
        else if (!strcmp(a, "-empty")) o.empty = true;
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

int b_clear(Ctx &c) { wr(c.out, "\x1b[H\x1b[2J\x1b[3J"); return 0; }   // screen and scrollback, as clear(1)
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

// update [status|check|install|sd [FILE]|restart] - the Settings > Update actions from a shell, so an
// update can be driven and read as text (tools/nsh.py) instead of through screenshots.
int b_update(Ctx &c) {
    const char *op = c.argc > 1 ? c.argv[1] : "status";
    auto wait_done = [&]() {
        for (int i = 0; i < 600; i++) {   // <= 5 min
            const nv_ota_state_t st = nv_ota_state();
            if (st != NV_OTA_CHECKING && st != NV_OTA_DOWNLOADING) break;
            vTaskDelay(pdMS_TO_TICKS(500));
        }
    };
    const bool acts = !strcmp(op, "check") || !strcmp(op, "install") || !strcmp(op, "sd");
    if (acts && nv_ota_busy()) {   // the boot auto-check runs ~1 min after boot: wait, don't drop it
        wr(c.out, "update: updater busy, waiting...\n");
        for (int i = 0; i < 240 && nv_ota_busy(); i++) vTaskDelay(pdMS_TO_TICKS(500));
        if (nv_ota_busy()) { wr(c.err, "update: still busy, try again later\n"); return 1; }
    }
    if (!strcmp(op, "check")) {
        char url[256];
        nv_ota_get_url(url, sizeof url);
        nv_ota_check(url);
        vTaskDelay(pdMS_TO_TICKS(300));
        wait_done();
    } else if (!strcmp(op, "install")) {
        nv_ota_update();
        vTaskDelay(pdMS_TO_TICKS(300));
        wait_done();
    } else if (!strcmp(op, "sd")) {
        nv_ota_install_sd(c.argc > 2 ? c.argv[2] : nullptr);
        vTaskDelay(pdMS_TO_TICKS(300));
        wait_done();
    } else if (!strcmp(op, "restart")) {
        if (nv_ota_state() != NV_OTA_SUCCESS) { wr(c.err, "update: nothing ready to install\n"); return 1; }
        nv_ota_reboot();
        return 0;
    } else if (strcmp(op, "status")) {
        wr(c.err, "usage: update [status|check|install|sd [FILE]|restart]\n");
        return 2;
    }
    static const char *const kSt[] = {"idle", "checking", "up-to-date", "available", "downloading", "ready", "failed"};
    const nv_ota_state_t st = nv_ota_state();
    outf(c, "running %s  layout %s  state %s", nv_ota_running_version(), nv_ota_layout_ok() ? "v2" : "v1 (reinstall needed)",
         (unsigned)st < 7 ? kSt[st] : "?");
    if (nv_ota_available_version()[0]) outf(c, "  offered %s", nv_ota_available_version());
    if (nv_ota_message()[0]) outf(c, "\n%s", nv_ota_message());
    wr(c.out, "\n", 1);
    return st == NV_OTA_FAILED ? 1 : 0;
}

int b_hostname(Ctx &c) {
    if (c.argc > 1 && (!strcmp(c.argv[1], "-I") || !strcmp(c.argv[1], "-i"))) {
        nv_wifi_link_t lk;
        if (!nv_wifi_is_enabled() || !nv_wifi_get_link(&lk)) { wr(c.out, "\n", 1); return 0; }
        outf(c, "%s \n", lk.ip);
        return 0;
    }
    outf(c, "%s\n", kHost);
    return 0;
}
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
    constexpr size_t kSnap = 32 * 1024;
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


// ---- store: the app store from the shell (people and ANIMA's shell tool alike) -----------------------
// The catalog is fetched on first use (Wi-Fi); `store search` ranks name > category > description.
bool store_catalog(Ctx &c) {
    if (nv_appstore_count() > 0 && nv_appstore_state() != NV_STORE_FETCHING) return true;
    if (nv_appstore_state() != NV_STORE_FETCHING) nv_appstore_refresh();
    for (int t = 0; t < 200 && nv_appstore_state() == NV_STORE_FETCHING && !cancelled(); t++) vTaskDelay(pdMS_TO_TICKS(100));
    if (nv_appstore_count() > 0) return true;
    const char *m = nv_appstore_message();
    errf(c, "store: catalog unavailable%s%s\n", m && m[0] ? ": " : " (Wi-Fi?)", m ? m : "");
    return false;
}

void lower_into(char *dst, size_t cap, const char *src) {
    size_t i = 0;
    for (; src[i] && i + 1 < cap; i++) dst[i] = (char)tolower((unsigned char)src[i]);
    dst[i] = 0;
}

int store_score(const nv_store_entry_t &e, char words[][32], int nw) {
    char name[64], cat[80], desc[300];
    lower_into(name, sizeof name, e.name);
    snprintf(cat, sizeof cat, "%s %s %s", e.category_name, e.subcategory_name, e.platform);
    lower_into(cat, sizeof cat, cat);
    lower_into(desc, sizeof desc, e.desc);
    int sc = 0;
    for (int i = 0; i < nw; i++) {
        if (!strcmp(e.id, words[i])) sc += 10;
        if (strstr(name, words[i])) sc += 4;
        if (strstr(cat, words[i])) sc += 2;
        if (strstr(desc, words[i])) sc += 1;
    }
    return sc;
}

void store_row(Ctx &c, const nv_store_entry_t &e) {
    outf(c, "%-18s %-24.24s %-12.12s %5uK%s%s\n", e.id, e.name, e.category_name[0] ? e.category_name : e.category,
         (unsigned)(e.size / 1024), e.installed ? (e.update ? " update" : " installed") : "", e.is_game ? " game" : "");
}

int b_store(Ctx &c) {
    const char *sub = c.argc > 1 ? c.argv[1] : "";
    if (!sub[0] || !strcmp(sub, "-h") || !strcmp(sub, "--help")) {
        outf(c, "usage: store search WORDS... | store list [CATEGORY] | store info ID | store install ID | store remove ID\n");
        return sub[0] ? 0 : 1;
    }
    if (!strcmp(sub, "remove") || !strcmp(sub, "uninstall") || !strcmp(sub, "rm")) {
        // Same path as the Store's uninstall button: system apps, a running app and a package
        // other apps depend on are refused by nv_wasm_uninstall itself.
        if (c.argc < 3) { errf(c, "usage: store remove ID\n"); return 1; }
        char err[112] = "";
        if (!nv_wasm_uninstall(c.argv[2], err, sizeof err)) { errf(c, "store: %s: %s\n", c.argv[2], err); return 1; }
        if (lvgl_port_lock(2000)) { nv_app_unregister(c.argv[2]); nv_open_unregister_app(c.argv[2]); lvgl_port_unlock(); }
        outf(c, "removed %s\n", c.argv[2]);
        return 0;
    }
    if (!store_catalog(c)) return 1;
    auto *e = (nv_store_entry_t *)ps_alloc(sizeof(nv_store_entry_t));
    if (!e) return 1;
    const int n = nv_appstore_count();
    int rc = 0;
    if (!strcmp(sub, "search") || !strcmp(sub, "find")) {
        char words[8][32]; int nw = 0;
        for (int i = 2; i < c.argc && nw < 8; i++) if (strlen(c.argv[i]) >= 2) lower_into(words[nw++], 32, c.argv[i]);
        if (!nw) { errf(c, "usage: store search WORDS...\n"); heap_caps_free(e); return 1; }
        int best[8] = {-1, -1, -1, -1, -1, -1, -1, -1}, bsc[8] = {0};
        for (int i = 0; i < n; i++) {
            if (!nv_appstore_get(i, e) || e->library) continue;
            const int sc = store_score(*e, words, nw);
            if (sc <= 0) continue;
            for (int k = 0; k < 8; k++) if (best[k] < 0 || sc > bsc[k]) {
                for (int j = 7; j > k; j--) { best[j] = best[j - 1]; bsc[j] = bsc[j - 1]; }
                best[k] = i; bsc[k] = sc; break;
            }
        }
        int shown = 0;
        for (int k = 0; k < 8 && best[k] >= 0; k++) if (nv_appstore_get(best[k], e)) { store_row(c, *e); shown++; }
        if (!shown) { outf(c, "no app matches\n"); rc = 1; }
    } else if (!strcmp(sub, "list") || !strcmp(sub, "ls")) {
        char want[32] = "";
        if (c.argc > 2) lower_into(want, sizeof want, c.argv[2]);
        int shown = 0;
        for (int i = 0; i < n && shown < 60; i++) {
            if (!nv_appstore_get(i, e) || e->library) continue;
            if (want[0]) {
                char cat[64]; snprintf(cat, sizeof cat, "%s %s", e->category, e->category_name);
                lower_into(cat, sizeof cat, cat);
                if (!strstr(cat, want)) continue;
            }
            store_row(c, *e); shown++;
        }
        if (shown == 60) outf(c, "... (more: store search WORDS)\n");
    } else if (!strcmp(sub, "info") || !strcmp(sub, "install")) {
        if (c.argc < 3) { errf(c, "usage: store %s ID\n", sub); heap_caps_free(e); return 1; }
        int at = -1;
        for (int i = 0; i < n && at < 0; i++) if (nv_appstore_get(i, e) && !strcmp(e->id, c.argv[2])) at = i;
        if (at < 0) { errf(c, "store: %s: no such app (try: store search WORDS)\n", c.argv[2]); heap_caps_free(e); return 1; }
        if (!strcmp(sub, "info")) {
            outf(c, "%s (%s) %s by %s\n%s\ncategory: %s  size: %uK  %s\n", e->name, e->id, e->version, e->author, e->desc,
                 e->category_name, (unsigned)(e->size / 1024), e->installed ? (e->update ? "installed, update available" : "installed") : "not installed");
        } else if (e->installed && !e->update) {
            outf(c, "%s is already installed\n", e->name);
        } else if (!nv_appstore_install(e->id)) {
            errf(c, "store: cannot start the install (busy?) %s\n", nv_appstore_message());
            rc = 1;
        } else {
            int last = -1;
            while (nv_appstore_state() == NV_STORE_INSTALLING && !cancelled()) {
                const int p = nv_appstore_progress();
                if (p / 25 != last / 25 && tty(c)) outf(c, "installing %s %d%%\n", e->id, p);
                last = p;
                vTaskDelay(pdMS_TO_TICKS(200));
            }
            if (nv_appstore_state() == NV_STORE_READY) {
                if (lvgl_port_lock(2000)) { nv_apps_store_installed(e->id); lvgl_port_unlock(); }
                outf(c, "installed %s (%s)\n", e->name, e->id);
            } else {
                errf(c, "store: install failed: %s\n", nv_appstore_message());
                rc = 1;
            }
        }
    } else {
        errf(c, "store: unknown command '%s' (search, list, info, install, remove)\n", sub);
        rc = 1;
    }
    heap_caps_free(e);
    return rc;
}

// launch ID: open an app on the screen (native or WASM), as a tap on its tile would.
int b_launch(Ctx &c) {
    if (c.argc < 2) { errf(c, "usage: launch APP_ID   (e.g. launch luaapp, launch notes)\n"); return 1; }
    if (!nv_ui_open_app_id_async(c.argv[1])) { errf(c, "launch: %s: no such app\n", c.argv[1]); return 1; }
    outf(c, "opened %s\n", c.argv[1]);
    return 0;
}

// ---------------------------------------------------------------- commands for language models
// Each saves a model several commands or a whole file in its context: diff (check an edit), jq
// (one field out of a JSON), sysinfo (the board in one call), vol, notify, tg.

// diff [-u] A B: unified diff of two text files (LCS, up to 1500 lines each), 3 lines of context.
int b_diff(Ctx &c) {
    int i = 1;
    if (i < c.argc && (!strcmp(c.argv[i], "-u") || !strcmp(c.argv[i], "-U3"))) i++;
    if (c.argc - i != 2) { errf(c, "usage: diff [-u] FILE1 FILE2\n"); return 2; }
    ShBuf A, B;
    if (!read_all(c, c.argv[i], A) || !read_all(c, c.argv[i + 1], B)) { buf_free(A); buf_free(B); return 2; }
    constexpr int kMaxL = 1500;
    auto split = [](ShBuf &b, const char **ln, int *len) {
        int n = 0;
        const char *p = b.p ? b.p : "", *e = p + b.n;
        while (p < e && n < kMaxL) {
            const char *q = (const char *)memchr(p, '\n', (size_t)(e - p));
            const char *end = q ? q : e;
            ln[n] = p; len[n] = (int)(end - p); n++;
            p = q ? q + 1 : e;
        }
        return n;
    };
    auto *la = (const char **)ps_alloc(sizeof(char *) * kMaxL * 2);
    auto *lens = (int *)ps_alloc(sizeof(int) * kMaxL * 2);
    if (!la || !lens) { free(la); free(lens); buf_free(A); buf_free(B); errf(c, "diff: out of memory\n"); return 2; }
    const char **lb = la + kMaxL;
    int *na = lens, *nb = lens + kMaxL;
    const int n = split(A, la, na), m = split(B, lb, nb);
    auto eq = [&](int x, int y) { return na[x] == nb[y] && !memcmp(la[x], lb[y], (size_t)na[x]); };
    auto *L = (uint16_t *)ps_alloc(sizeof(uint16_t) * (size_t)(n + 1) * (size_t)(m + 1));
    if (!L) { free(la); free(lens); buf_free(A); buf_free(B); errf(c, "diff: files too big\n"); return 2; }
    for (int x = n; x >= 0; x--)
        for (int y = m; y >= 0; y--)
            L[x * (m + 1) + y] = (x == n || y == m) ? 0 : eq(x, y) ? L[(x + 1) * (m + 1) + y + 1] + 1
                               : (L[(x + 1) * (m + 1) + y] > L[x * (m + 1) + y + 1] ? L[(x + 1) * (m + 1) + y] : L[x * (m + 1) + y + 1]);
    // the edit script: ' ' same, '-' only in A, '+' only in B
    struct Op { char k; int a, b; };
    auto *ops = (Op *)ps_alloc(sizeof(Op) * (size_t)(n + m + 1));
    int no = 0, x = 0, y = 0;
    while (ops && (x < n || y < m)) {
        if (x < n && y < m && eq(x, y)) { ops[no++] = {' ', x++, y++}; }
        else if (x < n && (y == m || L[(x + 1) * (m + 1) + y] >= L[x * (m + 1) + y + 1])) { ops[no++] = {'-', x++, y}; }   // removals first, as GNU
        else { ops[no++] = {'+', x, y++}; }
    }
    int changes = 0;
    if (ops) {
        for (int k = 0; k < no; k++) changes += ops[k].k != ' ';
        if (changes) outf(c, "--- %s\n+++ %s\n", c.argv[i], c.argv[i + 1]);
        for (int k = 0; k < no; ) {                              // hunks with 3 lines of context
            if (ops[k].k == ' ') { k++; continue; }
            int s0 = k - 3 < 0 ? 0 : k - 3, e0 = k;
            while (e0 < no) {                                    // extend while changes are within 6 lines
                int z = e0;
                while (z < no && ops[z].k != ' ') z++;
                int same = 0;
                while (z + same < no && ops[z + same].k == ' ' && same < 7) same++;
                e0 = z + (same < 7 && z + same < no ? same : (same > 3 ? 3 : same));
                if (same >= 7 || z + same >= no) break;
            }
            int ca = 0, cb = 0;
            for (int q = s0; q < e0; q++) { ca += ops[q].k != '+'; cb += ops[q].k != '-'; }
            outf(c, "@@ -%d,%d +%d,%d @@\n", ops[s0].a + 1, ca, ops[s0].b + 1, cb);
            for (int q = s0; q < e0; q++) {
                const char *t = ops[q].k == '+' ? lb[ops[q].b] : la[ops[q].a];
                const int tl = ops[q].k == '+' ? nb[ops[q].b] : na[ops[q].a];
                char pre[2] = { ops[q].k, 0 };
                wr(c.out, pre); wr(c.out, t, (size_t)tl); wr(c.out, "\n");
            }
            k = e0;
        }
    }
    free(ops); free(L); free(la); free(lens); buf_free(A); buf_free(B);
    return changes ? 1 : 0;
}

namespace {
// jq: the filters models use: . .a .a.b .[N] .[] .a[] keys length, chained with |; -r raw strings,
// -c compact. Values are cJSON nodes of one parsed document (no copies).
constexpr int kJqMax = 256;
struct JqSet { cJSON *v[kJqMax]; int n = 0; cJSON *made[kJqMax]; int nm = 0; };

void jq_add(JqSet &o, cJSON *x) { if (x && o.n < kJqMax) o.v[o.n++] = x; }

bool jq_stage(const char *f, JqSet &in, JqSet &out) {
    while (*f == ' ') f++;
    if (!strncmp(f, "keys", 4) || !strncmp(f, "length", 6)) {
        const bool keys = f[0] == 'k';
        for (int i = 0; i < in.n; i++) {
            cJSON *x = in.v[i], *r;
            if (keys) {
                r = cJSON_CreateArray();
                if (cJSON_IsObject(x)) { cJSON *it; cJSON_ArrayForEach(it, x) cJSON_AddItemToArray(r, cJSON_CreateString(it->string)); }
                else if (cJSON_IsArray(x)) for (int k = 0; k < cJSON_GetArraySize(x); k++) cJSON_AddItemToArray(r, cJSON_CreateNumber(k));
            } else {
                r = cJSON_CreateNumber(cJSON_IsString(x) ? (double)strlen(x->valuestring)
                                       : (cJSON_IsArray(x) || cJSON_IsObject(x)) ? cJSON_GetArraySize(x) : 0);
            }
            if (out.nm < kJqMax) out.made[out.nm++] = r;
            jq_add(out, r);
        }
        return true;
    }
    if (*f != '.') return false;
    auto **cur = (cJSON **)ps_alloc(sizeof(cJSON *) * kJqMax * 2);   // PSRAM: the shell stack is small
    if (!cur) return false;
    cJSON **nx = cur + kJqMax;
    for (int i = 0; i < in.n; i++) {
        int nc = 0;
        cur[nc++] = in.v[i];
        const char *p = f + 1;
        bool ok = true;
        while (*p && *p != ' ' && ok) {
            int nn = 0;
            if (*p == '[') {
                const char *e = strchr(p, ']');
                if (!e) { ok = false; break; }
                for (int k = 0; k < nc; k++) {
                    if (e == p + 1) {                               // []: every element / value
                        cJSON *it;
                        cJSON_ArrayForEach(it, cur[k]) if (nn < kJqMax) nx[nn++] = it;
                    } else {
                        const int idx = atoi(p + 1), sz = cJSON_GetArraySize(cur[k]);
                        cJSON *it = cJSON_GetArrayItem(cur[k], idx < 0 ? sz + idx : idx);
                        if (it && nn < kJqMax) nx[nn++] = it;
                    }
                }
                p = e + 1;
            } else {
                if (*p == '.') p++;
                if (*p == '[') continue;                           // ".[0]" after a key: ".a.[0]"
                char key[64];
                int kl = 0;
                if (*p == '"') { p++; while (*p && *p != '"' && kl < 63) key[kl++] = *p++; if (*p == '"') p++; }
                else while (*p && *p != '.' && *p != '[' && *p != ' ' && kl < 63) key[kl++] = *p++;
                key[kl] = 0;
                if (!kl) break;                                     // a lone "." = identity
                for (int k = 0; k < nc; k++) {
                    cJSON *it = cJSON_GetObjectItemCaseSensitive(cur[k], key);
                    if (it && nn < kJqMax) nx[nn++] = it;
                }
            }
            memcpy(cur, nx, sizeof(cJSON *) * (size_t)nn);
            nc = nn;
        }
        for (int k = 0; k < nc; k++) jq_add(out, cur[k]);
    }
    free(cur);
    return true;
}
}  // namespace

int b_jq(Ctx &c) {
    bool raw = false, compact = false;
    int i = 1;
    for (; i < c.argc && c.argv[i][0] == '-' && c.argv[i][1]; i++) {
        for (const char *q = c.argv[i] + 1; *q; q++) { if (*q == 'r') raw = true; else if (*q == 'c') compact = true; }
    }
    if (i >= c.argc) { errf(c, "usage: jq [-rc] FILTER [FILE]   (. .a.b .[0] .[] keys length, | chains)\n"); return 2; }
    const char *filter = c.argv[i++];
    ShBuf b;
    if (!read_all(c, i < c.argc ? c.argv[i] : "-", b)) return 2;
    cJSON *doc = b.p ? cJSON_ParseWithLength(b.p, b.n) : nullptr;
    buf_free(b);
    if (!doc) { errf(c, "jq: invalid JSON input\n"); return 2; }
    auto *A = (JqSet *)ps_alloc(sizeof(JqSet) * 2);
    if (!A) { cJSON_Delete(doc); return 2; }
    new (&A[0]) JqSet(); new (&A[1]) JqSet();
    jq_add(A[0], doc);
    int cur = 0, st = 0;
    char stage[160];
    for (const char *p = filter; *p; ) {
        const char *bar = strchr(p, '|');
        const size_t l = bar ? (size_t)(bar - p) : strlen(p);
        snprintf(stage, sizeof stage, "%.*s", (int)l, p);
        A[1 - cur].n = 0;
        if (!jq_stage(stage, A[cur], A[1 - cur])) { errf(c, "jq: unsupported filter '%s'\n", stage); st = 3; break; }
        cur = 1 - cur;
        p = bar ? bar + 1 : p + l;
    }
    for (int k = 0; !st && k < A[cur].n; k++) {
        cJSON *x = A[cur].v[k];
        if (raw && cJSON_IsString(x)) { wr(c.out, x->valuestring); wr(c.out, "\n"); continue; }
        char *t = compact ? cJSON_PrintUnformatted(x) : cJSON_Print(x);
        if (t) { wr(c.out, t); wr(c.out, "\n"); cJSON_free(t); }
    }
    for (int s2 = 0; s2 < 2; s2++) for (int k = 0; k < A[s2].nm; k++) cJSON_Delete(A[s2].made[k]);
    free(A);
    cJSON_Delete(doc);
    return st;
}

// sysinfo: the board in one call (what a model would otherwise ask with 6-7 commands).
int b_sysinfo(Ctx &c) {
    char now[24];
    nv_time_format(now, sizeof now, "%Y-%m-%d %H:%M");
    const uint32_t up = (uint32_t)(esp_timer_get_time() / 1000000);
    char app[32] = "";
    if (lvgl_port_lock(500)) { snprintf(app, sizeof app, "%s", nv_ui_current_app_id()); lvgl_port_unlock(); }
    outf(c, "time %s, up %uh%02um; screen %s\n", now, (unsigned)(up / 3600), (unsigned)(up / 60 % 60), app[0] ? app : "home");
    char ssid[33] = "", ip[20] = "";
    int8_t rssi = 0;
    if (nv_wifi_get_connected(ssid, sizeof ssid, ip, sizeof ip, &rssi)) outf(c, "wifi %s %s %ddBm\n", ssid, ip, rssi);
    else outf(c, "wifi off/disconnected\n");
    uint64_t tot = 0, fr = 0;
    char a[16], d[16];
    if (nv_sd_info(&tot, &fr)) { human(fr, a, sizeof a); human(tot, d, sizeof d); outf(c, "sd %s free of %s\n", a, d); }
    human(heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT), a, sizeof a);
    human(heap_caps_get_free_size(MALLOC_CAP_SPIRAM), d, sizeof d);
    outf(c, "ram free: internal %s, psram %s\n", a, d);
    outf(c, "volume %d%s, brightness %d\n", nv_config_get_int("volume", 60), nv_config_get_bool("mute", false) ? " (muted)" : "",
         nv_config_get_int("brightness", 90));
    return 0;
}

// vol [0-100]: the volume, persisted like ANIMA's set_volume; no argument prints it.
int b_vol(Ctx &c) {
    if (c.argc < 2) { outf(c, "%d\n", nv_config_get_int("volume", 60)); return 0; }
    char v[8];
    snprintf(v, sizeof v, "%d", atoi(c.argv[1]) < 0 ? 0 : atoi(c.argv[1]) > 100 ? 100 : atoi(c.argv[1]));
    if (!nv_anima_os_exec("set_volume", v)) { errf(c, "vol: audio unavailable\n"); return 1; }
    outf(c, "volume %s\n", v);
    return 0;
}

// notify [-t TITLE] TEXT: a system notification (toast + notification center).
int b_notify(Ctx &c) {
    int i = 1;
    const char *title = "ANIMA";
    if (i + 1 < c.argc && !strcmp(c.argv[i], "-t")) { title = c.argv[i + 1]; i += 2; }
    if (i >= c.argc) { errf(c, "usage: notify [-t TITLE] TEXT\n"); return 1; }
    char t[300] = "";
    for (; i < c.argc; i++) snprintf(t + strlen(t), sizeof t - strlen(t), "%s%s", t[0] ? " " : "", c.argv[i]);
    if (!lvgl_port_lock(1000)) { errf(c, "notify: the screen is busy\n"); return 1; }
    nv_notify_post(NV_NOTE_INFO, title, t);
    lvgl_port_unlock();
    return 0;
}

// tg TEXT: a message to the Telegram chat paired with ANIMA (stdin when no text).
int b_tg(Ctx &c) {
    char t[1000] = "";
    for (int i = 1; i < c.argc; i++) snprintf(t + strlen(t), sizeof t - strlen(t), "%s%s", t[0] ? " " : "", c.argv[i]);
    if (!t[0] && c.has_in) snprintf(t, sizeof t, "%.*s", (int)(c.in_len < sizeof t - 1 ? c.in_len : sizeof t - 1), c.in);
    if (!t[0]) { errf(c, "usage: tg TEXT   (or: cmd | tg)\n"); return 1; }
    if (!nucleo_anima_tg_notify(t)) { errf(c, "tg: Telegram is not paired (Settings > IA)\n"); return 1; }
    outf(c, "sent\n");
    return 0;
}


// ---------------------------------------------------------------- settings / Wi-Fi
// cfg [KEY [VALUE]]: the system settings (nv_config) — the same keys Settings writes, applied
// live through NV_EV_SETTINGS_CHANGED. Only listed keys; secrets are never printed.
struct CfgKey { const char *k; char t; const char *what; };   // t: i int, b bool, s string, x secret
const CfgKey kCfg[] = {
    {"brightness", 'i', "screen 5..100"},   {"volume", 'i', "0..100"},
    {"mute", 'b', ""},                      {"dnd", 'b', "do not disturb"},
    {"scr_timeout", 'i', "screen sleep s (0 never)"}, {"rotation", 'i', "0|90"},
    {"thmode", 'i', "0 dark 1 light 2 auto"}, {"thaccent", 'i', "accent color index"},
    {"lang", 'i', "language index"},         {"tz_ix", 'i', "timezone index"},
    {"clk24", 'b', "24h clock"},             {"keyclick", 'b', ""},
    {"chime", 'b', ""},                      {"wifi_on", 'b', ""},
    {"bt_on", 'b', ""},                      {"usbhost", 'b', ""},
    {"mqtt_en", 'b', ""},                    {"mqtt_host", 's', ""},
    {"mqtt_port", 'i', ""},                  {"mqtt_user", 's', ""},
    {"mqtt_pass", 'x', ""},                  {"ha_url", 's', "Home Assistant URL"},
    {"ha_token", 'x', ""},                   {"ota_url", 's', "update server"},
    {"store_url", 's', "app store server"},  {"store_region", 's', ""},
    {"wake.on", 'b', "wake word"},           {"wake.sens", 'i', ""},
    {"lock_en", 'b', "lock screen"},         {"lockpin", 'x', ""},
    {"ss_auto", 'b', "second screen auto"},  {"ui_classic", 'b', "classic UI"},
};

const CfgKey *cfg_find(const char *k) {
    for (const CfgKey &e : kCfg) if (!strcmp(e.k, k)) return &e;
    return nullptr;
}

void cfg_print(Ctx &c, const CfgKey &e) {
    char s[160];
    switch (e.t) {
    case 'i': outf(c, "%s=%d", e.k, nv_config_get_int(e.k, 0)); break;
    case 'b': outf(c, "%s=%d", e.k, nv_config_get_bool(e.k, false) ? 1 : 0); break;
    case 'x': nv_config_get_str(e.k, "", s, sizeof s); outf(c, "%s=%s", e.k, s[0] ? "***" : ""); break;
    default:  nv_config_get_str(e.k, "", s, sizeof s); outf(c, "%s=%s", e.k, s); break;
    }
    outf(c, e.what[0] ? "  # %s\n" : "\n", e.what);
}

// Security: where firmware/apps come from and the lock screen are changed only by hand in
// Settings, never from a shell a model or a remote channel may drive (prompt injection).
bool cfg_ro(const char *k) {
    static const char *const kRo[] = {"ota_url", "store_url", "lock_en", "lockpin", nullptr};
    for (int i = 0; kRo[i]; i++) if (!strcmp(k, kRo[i])) return true;
    return false;
}

bool cfg_set(Ctx &c, const CfgKey &e, const char *val) {
    if (cfg_ro(e.k)) { errf(c, "cfg: %s is read-only here (change it in Settings)\n", e.k); return false; }
    if ((e.t == 'i' || e.t == 'b') && !(isdigit((unsigned char)val[0]) || val[0] == '-')) {
        errf(c, "cfg: %s wants a number\n", e.k); return false;
    }
    if (!lvgl_port_lock(2000)) { errf(c, "cfg: the screen is busy\n"); return false; }
    if (e.t == 'i') nv_config_set_int(e.k, atoi(val));
    else if (e.t == 'b') nv_config_set_bool(e.k, atoi(val) != 0);
    else nv_config_set_str(e.k, val);
    lvgl_port_unlock();
    return true;
}

// cfg export: KEY=VALUE lines for a backup (secrets and read-only keys left out);
// cfg import FILE: applies such lines (# comments, unknown keys reported and skipped).
int cfg_export(Ctx &c) {
    char s[160];
    for (const CfgKey &e : kCfg) {
        if (e.t == 'x' || cfg_ro(e.k)) continue;
        if (e.t == 'i') outf(c, "%s=%d\n", e.k, nv_config_get_int(e.k, 0));
        else if (e.t == 'b') outf(c, "%s=%d\n", e.k, nv_config_get_bool(e.k, false) ? 1 : 0);
        else { nv_config_get_str(e.k, "", s, sizeof s); outf(c, "%s=%s\n", e.k, s); }
    }
    return 0;
}

int cfg_import(Ctx &c, const char *path) {
    char p[kPath];
    resolve(path, p, sizeof p);
    FILE *f = fopen(p, "r");
    if (!f) { errf(c, "cfg: %s: No such file or directory\n", path); return 1; }
    char line[256]; int n = 0, bad = 0;
    while (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = 0;
        char *eq = strchr(line, '=');
        if (line[0] == '#' || !eq) continue;
        *eq = 0;
        const CfgKey *e = cfg_find(line);
        if (!e || e->t == 'x') { errf(c, "cfg: %s: skipped\n", line); bad++; continue; }
        if (cfg_set(c, *e, eq + 1)) n++; else bad++;
    }
    fclose(f);
    outf(c, "imported %d settings%s\n", n, bad ? " (some skipped)" : "");
    return bad ? 1 : 0;
}

int b_cfg(Ctx &c) {
    if (c.argc < 2) { for (const CfgKey &e : kCfg) cfg_print(c, e); return 0; }
    if (!strcmp(c.argv[1], "export")) return cfg_export(c);
    if (!strcmp(c.argv[1], "import")) {
        if (c.argc < 3) { errf(c, "usage: cfg import FILE\n"); return 1; }
        return cfg_import(c, c.argv[2]);
    }
    // accept both "cfg key value" and "cfg key=value"
    char key[32]; const char *val = c.argc > 2 ? c.argv[2] : nullptr;
    snprintf(key, sizeof key, "%s", c.argv[1]);
    if (char *eq = strchr(key, '=')) { *eq = 0; val = c.argv[1] + (eq - key) + 1; }
    const CfgKey *e = cfg_find(key);
    if (!e) { errf(c, "cfg: %s: unknown key (cfg lists them)\n", key); return 1; }
    if (!val) { cfg_print(c, *e); return 0; }
    if (!cfg_set(c, *e, val)) return 1;
    cfg_print(c, *e);
    return 0;
}

// wifi [status|scan|on|off|join SSID [PASS]|leave|forget SSID]
int b_wifi(Ctx &c) {
    const char *sub = c.argc > 1 ? c.argv[1] : "status";
    if (!strcmp(sub, "status")) {
        char ssid[33], ip[20]; int8_t rssi = 0;
        if (!nv_wifi_is_enabled()) outf(c, "off\n");
        else if (nv_wifi_get_connected(ssid, sizeof ssid, ip, sizeof ip, &rssi)) outf(c, "up %s %s %ddBm\n", ssid, ip, rssi);
        else outf(c, "on, not connected\n");
        return 0;
    }
    if (!strcmp(sub, "on") || !strcmp(sub, "off")) { nv_wifi_set_enabled(sub[1] == 'n'); outf(c, "wifi %s\n", sub); return 0; }
    if (!nv_wifi_is_enabled()) { errf(c, "wifi: off (wifi on)\n"); return 1; }
    if (!strcmp(sub, "scan")) {
        const uint32_t g = nv_wifi_scan_generation();
        nv_wifi_start_scan();
        for (int i = 0; i < 40 && nv_wifi_scan_generation() == g; i++) vTaskDelay(pdMS_TO_TICKS(250));
        static nv_wifi_ap_t aps[24];
        const int n = nv_wifi_copy_aps(aps, 24);
        for (int i = 0; i < n; i++)
            outf(c, "%4d %-5s %s%s\n", aps[i].rssi, nv_wifi_auth_label(aps[i].auth), aps[i].ssid, aps[i].saved ? " *" : "");
        return 0;
    }
    if (!strcmp(sub, "join") && c.argc > 2) {
        nv_wifi_connect(c.argv[2], c.argc > 3 ? c.argv[3] : "");
        char ssid[33] = "", ip[20] = ""; int8_t rssi = 0;
        for (int i = 0; i < 60; i++) {
            if (nv_wifi_get_connected(ssid, sizeof ssid, ip, sizeof ip, &rssi) && !strcmp(ssid, c.argv[2])) {
                outf(c, "up %s %s %ddBm\n", ssid, ip, rssi); return 0;
            }
            vTaskDelay(pdMS_TO_TICKS(250));
        }
        errf(c, "wifi: %s: no link after 15 s (wrong password?)\n", c.argv[2]); return 1;
    }
    if (!strcmp(sub, "leave")) { nv_wifi_disconnect(); return 0; }
    if (!strcmp(sub, "forget") && c.argc > 2) { nv_wifi_forget(c.argv[2]); return 0; }
    errf(c, "usage: wifi [status|scan|on|off|join SSID [PASS]|leave|forget SSID]\n");
    return 1;
}
// ---------------------------------------------------------------- home automation (Home Assistant)
// ha: Home Assistant from the shell, with the URL + token already saved in Settings > Casa (never
// printed). Built for models: compact lines filtered by Home Assistant itself through /api/template
// (no multi-MB /api/states on the board), and `ha say` hands a sentence to Assist, which knows the
// home's names, rooms and Italian.
namespace {
// One HTTP request with an optional bearer token and JSON body. Body -> out. HTTP status or -1.
int http_call(const char *method, const char *url, const char *bearer, const char *body, ShBuf &out, char *err, size_t errn) {
    esp_http_client_config_t cfg = {};
    cfg.url = url;
    cfg.timeout_ms = 15000;
    cfg.buffer_size = 4096;
    cfg.buffer_size_tx = 2048;
    cfg.crt_bundle_attach = esp_crt_bundle_attach;
    cfg.user_agent = "NucleoOS";
    cfg.method = !strcmp(method, "POST") ? HTTP_METHOD_POST : HTTP_METHOD_GET;
    esp_http_client_handle_t h = esp_http_client_init(&cfg);
    if (!h) { snprintf(err, errn, "out of memory"); return -1; }
    if (bearer && bearer[0]) {
        char *auth = (char *)ps_alloc(strlen(bearer) + 8);
        if (auth) { snprintf(auth, strlen(bearer) + 8, "Bearer %s", bearer); esp_http_client_set_header(h, "Authorization", auth); free(auth); }
    }
    const int bl = body ? (int)strlen(body) : 0;
    if (bl) esp_http_client_set_header(h, "Content-Type", "application/json");
    int status = -1;
    if (esp_http_client_open(h, bl) != ESP_OK) snprintf(err, errn, "could not connect");
    else if (bl && esp_http_client_write(h, body, bl) != bl) snprintf(err, errn, "send failed");
    else {
        esp_http_client_fetch_headers(h);
        status = esp_http_client_get_status_code(h);
        char *buf = (char *)ps_alloc(4096);
        int n;
        while (buf && !cancelled() && (n = esp_http_client_read(h, buf, 4096)) > 0) {
            buf_put(out, buf, (size_t)n);
            if (out.trunc) break;
        }
        heap_caps_free(buf);
    }
    esp_http_client_close(h);
    esp_http_client_cleanup(h);
    return status;
}

struct HaCfg { char url[160]; char token[300]; };

bool ha_cfg(Ctx &c, HaCfg &k) {
    nv_config_get_str("ha_url", "", k.url, sizeof k.url);
    nv_config_get_str("ha_token", "", k.token, sizeof k.token);
    for (int n = (int)strlen(k.url); n > 0 && k.url[n - 1] == '/'; ) k.url[--n] = 0;
    if (!k.url[0] || !k.token[0]) { errf(c, "ha: not configured (Settings > Casa: Home Assistant URL + token)\n"); return false; }
    return true;
}

// Request to HA; out gets the body. Prints the error itself. Returns true on 2xx.
bool ha_req(Ctx &c, const HaCfg &k, const char *method, const char *path, const char *body, ShBuf &out) {
    char url[240], err[64] = "";
    snprintf(url, sizeof url, "%s%s", k.url, path);
    const int st = http_call(method, url, k.token, body, out, err, sizeof err);
    if (st >= 200 && st < 300) return true;
    if (st == 401) errf(c, "ha: token refused (401): renew it in Settings > Casa\n");
    else if (st < 0) errf(c, "ha: %s (%s)\n", err, k.url);
    else errf(c, "ha: HTTP %d: %.*s\n", st, (int)(out.n < 200 ? out.n : 200), out.p ? out.p : "");
    return false;
}

// Render a Jinja template in HA: the filtering happens there, one compact line per entity here.
bool ha_template(Ctx &c, const HaCfg &k, const char *tpl, ShBuf &out) {
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "template", tpl);
    char *body = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);
    if (!body) return false;
    const bool ok = ha_req(c, k, "POST", "/api/template", body, out);
    cJSON_free(body);
    return ok;
}

// The line for one state: entity state [level%|unit] [now T] "Name" @Area
constexpr const char *kHaLine =
    "{{ s.entity_id }} {{ s.state }}"
    "{% if s.attributes.brightness %} {{ (s.attributes.brightness/2.55)|round|int }}%{% endif %}"
    "{% if s.attributes.unit_of_measurement %}{{ s.attributes.unit_of_measurement }}{% endif %}"
    "{% if s.attributes.current_temperature is defined %} now {{ s.attributes.current_temperature }}{% endif %}"
    "{% if s.attributes.current_position is defined %} pos {{ s.attributes.current_position }}{% endif %}"
    " \"{{ s.name }}\"{% if area_name(s.entity_id) %} @{{ area_name(s.entity_id) }}{% endif %}\n";

// Entities whose id, name or area contains `q` (lowercase), in `domains` ("" = any). Max `lim`.
void ha_list_tpl(char *tpl, size_t cap, const char *q, const char *domains, int lim) {
    char qq[64] = "";
    for (int i = 0; q && q[i] && i < 63; i++) qq[i] = (q[i] == '\'' || q[i] == '{' || q[i] == '}') ? ' ' : (char)tolower((unsigned char)q[i]);
    snprintf(tpl, cap,
             "{%% set q = '%s' %%}{%% set n = namespace(c=0) %%}"
             "{%% for s in states %s%%}"
             "{%% if q == '' or q in s.entity_id or q in (s.name|lower) or q in ((area_name(s.entity_id) or '')|lower) %%}"
             "{%% set n.c = n.c + 1 %%}{%% if n.c <= %d %%}%s{%% endif %%}{%% endif %%}{%% endfor %%}"
             "{%% if n.c > %d %%}...(+{{ n.c - %d }} more: narrow the filter)\n{%% endif %%}"
             "{%% if n.c == 0 %%}(no matching entities)\n{%% endif %%}",
             qq, domains[0] ? domains : "", lim, kHaLine, lim, lim);
}

// An entity argument: "light.cucina" as is, otherwise the first entity whose name/area matches.
bool ha_resolve(Ctx &c, const HaCfg &k, const char *arg, char *id, size_t cap) {
    if (strchr(arg, '.')) { snprintf(id, cap, "%s", arg); return true; }
    char *tpl = (char *)ps_alloc(4096);
    if (!tpl) return false;
    char qq[64] = "";
    for (int i = 0; arg[i] && i < 63; i++) qq[i] = (arg[i] == '\'' || arg[i] == '{') ? ' ' : (char)tolower((unsigned char)arg[i]);
    snprintf(tpl, 4096, "{%% set q = '%s' %%}{{ (states|selectattr('domain','in',['light','switch','cover','fan','climate','media_player','lock','scene','script','input_boolean','vacuum','valve','humidifier','water_heater'])"
             "|selectattr('name','search','(?i)'+q)|map(attribute='entity_id')|list + states|selectattr('entity_id','search',q)|map(attribute='entity_id')|list)|first|default('') }}", qq);
    ShBuf out;
    const bool ok = ha_template(c, k, tpl, out);
    free(tpl);
    if (ok && out.p) {
        int n = 0;
        for (size_t i = 0; i < out.n && n < (int)cap - 1 && out.p[i] > ' '; i++) id[n++] = out.p[i];
        id[n] = 0;
    }
    buf_free(out);
    if (!ok) return false;
    if (!id[0]) { errf(c, "ha: no entity matches '%s' (try: ha find %s)\n", arg, arg); return false; }
    return true;
}

// "k=v" -> JSON value (number, bool, or string)
void ha_kv(cJSON *o, const char *kv) {
    const char *eq = strchr(kv, '=');
    if (!eq || eq == kv) return;
    char key[48];
    snprintf(key, sizeof key, "%.*s", (int)(eq - kv), kv);
    const char *v = eq + 1;
    char *end;
    const double d = strtod(v, &end);
    if (*v && !*end) cJSON_AddNumberToObject(o, key, d);
    else if (!strcmp(v, "true") || !strcmp(v, "false")) cJSON_AddBoolToObject(o, key, v[0] == 't');
    else cJSON_AddStringToObject(o, key, v);
}

// Call a service with entity_id + k=v data; print the entity's new state line.
int ha_service(Ctx &c, const HaCfg &k, const char *domain, const char *service, const char *entity, int argc, char **argv) {
    cJSON *o = cJSON_CreateObject();
    if (entity && entity[0]) cJSON_AddStringToObject(o, "entity_id", entity);
    for (int i = 0; i < argc; i++) ha_kv(o, argv[i]);
    char *body = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);
    char path[160];
    snprintf(path, sizeof path, "/api/services/%s/%s", domain, service);
    ShBuf out;
    const bool ok = body && ha_req(c, k, "POST", path, body, out);
    cJSON_free(body);
    buf_free(out);
    if (!ok) return 1;
    if (!entity || !entity[0]) { outf(c, "ok %s.%s\n", domain, service); return 0; }
    vTaskDelay(pdMS_TO_TICKS(400));                      // let the device report its new state
    char *tpl = (char *)ps_alloc(2048);
    if (!tpl) return 0;
    snprintf(tpl, 2048, "{%% set s = states['%s'] %%}{%% if s %%}%s{%% else %%}ok\n{%% endif %%}", entity, kHaLine);
    ShBuf st;
    if (ha_template(c, k, tpl, st) && st.p) wr(c.out, st.p, st.n);
    free(tpl);
    buf_free(st);
    return 0;
}
}  // namespace

int b_ha(Ctx &c) {
    if (c.argc < 2 || !strcmp(c.argv[1], "help")) {
        outf(c, "usage: ha say TEXT | ls [FILTER] | find TEXT | get ENTITY | on|off|toggle ENTITY|NAME...\n"
                "       ha set ENTITY k=v... (light brightness_pct= color_name= | climate temperature= | cover position=)\n"
                "       ha call DOMAIN.SERVICE [ENTITY] [k=v...] | ha status\n");
        return c.argc < 2 ? 2 : 0;
    }
    HaCfg *k = (HaCfg *)ps_alloc(sizeof(HaCfg));
    if (!k) return 1;
    if (!ha_cfg(c, *k)) { free(k); return 1; }
    const char *cmd = c.argv[1];
    int rc = 0;
    if (!strcmp(cmd, "say") || !strcmp(cmd, "ask")) {          // Assist: a sentence, HA does the rest
        char text[400] = "";
        for (int i = 2; i < c.argc; i++) snprintf(text + strlen(text), sizeof text - strlen(text), "%s%s", text[0] ? " " : "", c.argv[i]);
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "text", text);
        cJSON_AddStringToObject(o, "language", nv_i18n_get_lang() == NV_LANG_IT ? "it" : "en");
        char *body = cJSON_PrintUnformatted(o);
        cJSON_Delete(o);
        ShBuf out;
        if (!text[0]) { errf(c, "usage: ha say TEXT\n"); rc = 2; }
        else if (body && ha_req(c, *k, "POST", "/api/conversation/process", body, out)) {
            cJSON *r = out.p ? cJSON_ParseWithLength(out.p, out.n) : nullptr;
            cJSON *resp = r ? cJSON_GetObjectItem(r, "response") : nullptr;
            cJSON *sp = resp ? cJSON_GetObjectItem(cJSON_GetObjectItem(cJSON_GetObjectItem(resp, "speech"), "plain"), "speech") : nullptr;
            cJSON *ty = resp ? cJSON_GetObjectItem(resp, "response_type") : nullptr;
            const char *t = cJSON_IsString(ty) ? ty->valuestring : "?";
            outf(c, "%s: %s\n", !strcmp(t, "error") ? "not understood" : t, cJSON_IsString(sp) ? sp->valuestring : "");
            rc = !strcmp(t, "error");
            cJSON_Delete(r);
        } else rc = 1;
        cJSON_free(body);
        buf_free(out);
    } else if (!strcmp(cmd, "ls") || !strcmp(cmd, "find")) {
        const bool find = cmd[0] == 'f';
        char q[64] = "";
        for (int i = 2; i < c.argc; i++) snprintf(q + strlen(q), sizeof q - strlen(q), "%s%s", q[0] ? " " : "", c.argv[i]);
        // ls: what can be controlled; a domain name as filter lists that domain (ha ls sensor)
        static const char *const kDomains[] = {"light","switch","cover","climate","fan","lock","media_player","scene",
            "script","sensor","binary_sensor","input_boolean","vacuum","camera","person","valve","humidifier",nullptr};
        const char *dom = find ? "" : "|selectattr('domain','in',['light','switch','cover','climate','fan','lock','media_player','scene','input_boolean','vacuum','valve','humidifier']) ";
        char domsel[96] = "";
        for (int i = 0; kDomains[i]; i++)
            if (!strcmp(q, kDomains[i]) || (!strcmp(q, "luci") && !strcmp(kDomains[i], "light")) || (!strcmp(q, "sensori") && !strcmp(kDomains[i], "sensor"))) {
                snprintf(domsel, sizeof domsel, "|selectattr('domain','eq','%s') ", kDomains[i]);
                dom = domsel; q[0] = 0;
            }
        char *tpl = (char *)ps_alloc(4096);
        ShBuf out;
        if (tpl) { ha_list_tpl(tpl, 4096, q, dom, find ? 25 : 60); if (ha_template(c, *k, tpl, out) && out.p) wr(c.out, out.p, out.n); else rc = 1; }
        free(tpl);
        buf_free(out);
    } else if (!strcmp(cmd, "get") && c.argc >= 3) {
        char id[96];
        if (!ha_resolve(c, *k, c.argv[2], id, sizeof id)) rc = 1;
        else {
            char path[140];
            snprintf(path, sizeof path, "/api/states/%s", id);
            ShBuf out;
            if (ha_req(c, *k, "GET", path, nullptr, out) && out.p) {
                cJSON *r = cJSON_ParseWithLength(out.p, out.n);
                cJSON *a = r ? cJSON_GetObjectItem(r, "attributes") : nullptr;
                cJSON_DeleteItemFromObject(a, "supported_features"); cJSON_DeleteItemFromObject(a, "icon");
                cJSON_DeleteItemFromObject(a, "supported_color_modes"); cJSON_DeleteItemFromObject(a, "effect_list");
                char *at = a ? cJSON_PrintUnformatted(a) : nullptr;
                cJSON *stt = r ? cJSON_GetObjectItem(r, "state") : nullptr, *lc = r ? cJSON_GetObjectItem(r, "last_changed") : nullptr;
                outf(c, "%s %s (since %.19s)\n", id, cJSON_IsString(stt) ? stt->valuestring : "?", cJSON_IsString(lc) ? lc->valuestring : "?");
                if (at) { wr(c.out, at, strlen(at) > 1500 ? 1500 : strlen(at)); wr(c.out, "\n"); cJSON_free(at); }
                cJSON_Delete(r);
            } else rc = 1;
            buf_free(out);
        }
    } else if ((!strcmp(cmd, "on") || !strcmp(cmd, "off") || !strcmp(cmd, "toggle")) && c.argc >= 3) {
        for (int i = 2; i < c.argc && !rc; i++) {
            char id[96];
            if (!ha_resolve(c, *k, c.argv[i], id, sizeof id)) { rc = 1; break; }
            char domain[32];
            snprintf(domain, sizeof domain, "%.*s", (int)(strchr(id, '.') - id), id);
            const bool scene = !strcmp(domain, "scene") || !strcmp(domain, "script");
            rc = ha_service(c, *k, scene ? domain : "homeassistant", scene ? "turn_on" : !strcmp(cmd, "on") ? "turn_on" : !strcmp(cmd, "off") ? "turn_off" : "toggle", id, 0, nullptr);
        }
    } else if (!strcmp(cmd, "set") && c.argc >= 4) {
        char id[96];
        if (!ha_resolve(c, *k, c.argv[2], id, sizeof id)) rc = 1;
        else {
            char domain[32];
            snprintf(domain, sizeof domain, "%.*s", (int)(strchr(id, '.') - id), id);
            const char *svc = !strcmp(domain, "climate") ? "set_temperature" : !strcmp(domain, "cover") ? "set_cover_position"
                            : !strcmp(domain, "media_player") ? "volume_set" : !strcmp(domain, "fan") ? "set_percentage"
                            : (!strcmp(domain, "number") || !strcmp(domain, "input_number")) ? "set_value"
                            : !strcmp(domain, "input_select") || !strcmp(domain, "select") ? "select_option" : "turn_on";
            rc = ha_service(c, *k, domain, svc, id, c.argc - 3, c.argv + 3);
        }
    } else if (!strcmp(cmd, "call") && c.argc >= 3 && strchr(c.argv[2], '.')) {
        char domain[32];
        snprintf(domain, sizeof domain, "%.*s", (int)(strchr(c.argv[2], '.') - c.argv[2]), c.argv[2]);
        const bool has_ent = c.argc >= 4 && !strchr(c.argv[3], '=');
        char id[96] = "";
        if (has_ent && !ha_resolve(c, *k, c.argv[3], id, sizeof id)) rc = 1;
        else rc = ha_service(c, *k, domain, strchr(c.argv[2], '.') + 1, id, c.argc - (has_ent ? 4 : 3), c.argv + (has_ent ? 4 : 3));
    } else if (!strcmp(cmd, "status")) {
        ShBuf out;
        if (ha_req(c, *k, "GET", "/api/config", nullptr, out) && out.p) {
            cJSON *r = cJSON_ParseWithLength(out.p, out.n);
            cJSON *v = r ? cJSON_GetObjectItem(r, "version") : nullptr, *l = r ? cJSON_GetObjectItem(r, "location_name") : nullptr;
            outf(c, "home assistant %s \"%s\" at %s\n", cJSON_IsString(v) ? v->valuestring : "?", cJSON_IsString(l) ? l->valuestring : "", k->url);
            cJSON_Delete(r);
        } else rc = 1;
        buf_free(out);
    } else {
        errf(c, "ha: unknown '%s' (ha help)\n", cmd);
        rc = 2;
    }
    free(k);
    return rc;
}

// dev: smart devices on the LAN without Home Assistant, through their documented local APIs:
// Shelly (Gen2+ RPC, Gen1 fallback), Tasmota (/cm?cmnd=), WLED (/json/state). `dev scan` finds
// Shelly and WLED by mDNS; Tasmota (no mDNS by default) is added by hand. /sdcard/data/devices.json.
namespace {
constexpr const char *kDevFile = "/sdcard/data/devices.json";

// The device list, or NULL when devices.json exists but cannot be read/parsed: callers must then
// refuse to save, or a later write would silently wipe the user's hand-added devices.
cJSON *dev_load() {
    FILE *f = fopen(kDevFile, "rb");
    if (!f) return cJSON_CreateArray();              // no file yet: start empty
    cJSON *a = nullptr;
    struct stat st;
    if (fstat(fileno(f), &st) == 0 && st.st_size < 256 * 1024) {
        char *b = (char *)ps_alloc((size_t)st.st_size + 1);
        if (b) { size_t n = fread(b, 1, (size_t)st.st_size, f); b[n] = 0; a = cJSON_Parse(b); heap_caps_free(b); }
    }
    fclose(f);
    if (a && !cJSON_IsArray(a)) { cJSON_Delete(a); a = nullptr; }
    return a;
}

bool dev_save(cJSON *a) {
    char *t = cJSON_Print(a);
    if (!t) return false;
    FILE *f = fopen(kDevFile, "wb");
    const bool ok = f && fputs(t, f) >= 0;
    if (f) fclose(f);
    cJSON_free(t);
    return ok;
}

const char *dstr(cJSON *o, const char *k) { cJSON *v = cJSON_GetObjectItem(o, k); return cJSON_IsString(v) ? v->valuestring : ""; }

cJSON *dev_find(cJSON *a, const char *name) {
    cJSON *d;
    cJSON_ArrayForEach(d, a) if (!strcasecmp(dstr(d, "name"), name)) return d;
    cJSON_ArrayForEach(d, a) if (strcasestr(dstr(d, "name"), name)) return d;
    return nullptr;
}

void dev_put(cJSON *a, const char *name, const char *type, const char *ip, int gen) {
    cJSON *d = dev_find(a, name);
    if (d && strcasecmp(dstr(d, "name"), name)) d = nullptr;
    if (!d) { d = cJSON_CreateObject(); cJSON_AddItemToArray(a, d); }
    cJSON_DeleteItemFromObject(d, "name"); cJSON_AddStringToObject(d, "name", name);
    cJSON_DeleteItemFromObject(d, "type"); cJSON_AddStringToObject(d, "type", type);
    cJSON_DeleteItemFromObject(d, "ip");   cJSON_AddStringToObject(d, "ip", ip);
    cJSON_DeleteItemFromObject(d, "gen");  if (gen) cJSON_AddNumberToObject(d, "gen", gen);
}

// GET/POST on a device; the body in out. Prints errors. true on 2xx.
bool dev_http(Ctx &c, const char *method, const char *ip, const char *path, const char *body, ShBuf &out) {
    char url[200], err[48] = "";
    snprintf(url, sizeof url, "http://%s%s", ip, path);
    const int st = http_call(method, url, nullptr, body, out, err, sizeof err);
    if (st >= 200 && st < 300) return true;
    if (st < 0) errf(c, "dev: %s: %s\n", ip, err);
    else errf(c, "dev: %s: HTTP %d\n", ip, st);
    return false;
}

// Short name from an mDNS instance/host: "shellyplus1pm-a8032ab1c2d4" -> as is, lowercase, no spaces.
void dev_name(const char *in, char *out, size_t cap) {
    size_t n = 0;
    for (const char *p = in; *p && n + 1 < cap; p++) out[n++] = (*p == ' ' || *p == '.') ? '-' : (char)tolower((unsigned char)*p);
    out[n] = 0;
}

// The device's state in one line.
void dev_state(Ctx &c, cJSON *d) {
    const char *t = dstr(d, "type"), *ip = dstr(d, "ip");
    ShBuf out;
    char line[160] = "?";
    if (!strcmp(t, "shelly")) {
        cJSON *g = cJSON_GetObjectItem(d, "gen");
        const bool gen1 = cJSON_IsNumber(g) && g->valueint == 1;
        if (dev_http(c, "GET", ip, gen1 ? "/status" : "/rpc/Switch.GetStatus?id=0", nullptr, out) && out.p) {
            cJSON *r = cJSON_ParseWithLength(out.p, out.n);
            cJSON *on = gen1 ? cJSON_GetObjectItem(cJSON_GetArrayItem(cJSON_GetObjectItem(r, "relays"), 0), "ison") : cJSON_GetObjectItem(r, "output");
            cJSON *pw = gen1 ? nullptr : cJSON_GetObjectItem(r, "apower");
            snprintf(line, sizeof line, "%s", cJSON_IsTrue(on) ? "on" : cJSON_IsFalse(on) ? "off" : "?");
            if (cJSON_IsNumber(pw)) snprintf(line + strlen(line), sizeof line - strlen(line), " %.1fW", pw->valuedouble);
            cJSON_Delete(r);
        }
    } else if (!strcmp(t, "tasmota")) {
        if (dev_http(c, "GET", ip, "/cm?cmnd=State", nullptr, out) && out.p) {
            cJSON *r = cJSON_ParseWithLength(out.p, out.n);
            const char *p = dstr(r, "POWER");
            cJSON *dim = cJSON_GetObjectItem(r, "Dimmer");
            snprintf(line, sizeof line, "%s", p[0] ? (strcasecmp(p, "ON") ? "off" : "on") : "?");
            if (cJSON_IsNumber(dim)) snprintf(line + strlen(line), sizeof line - strlen(line), " %d%%", dim->valueint);
            cJSON_Delete(r);
        }
    } else if (!strcmp(t, "wled")) {
        if (dev_http(c, "GET", ip, "/json/state", nullptr, out) && out.p) {
            cJSON *r = cJSON_ParseWithLength(out.p, out.n);
            cJSON *on = cJSON_GetObjectItem(r, "on"), *bri = cJSON_GetObjectItem(r, "bri");
            snprintf(line, sizeof line, "%s", cJSON_IsTrue(on) ? "on" : "off");
            if (cJSON_IsNumber(bri)) snprintf(line + strlen(line), sizeof line - strlen(line), " %d%%", bri->valueint * 100 / 255);
            cJSON_Delete(r);
        }
    }
    buf_free(out);
    outf(c, "%s %s %s %s\n", dstr(d, "name"), t, ip, line);
}
}  // namespace

int b_dev(Ctx &c) {
    if (c.argc < 2 || !strcmp(c.argv[1], "help")) {
        outf(c, "usage: dev scan | ls | add NAME shelly|tasmota|wled IP | rm NAME | get NAME\n"
                "       dev on|off|toggle NAME | set NAME bri=0-100 (dimmer, WLED)\n");
        return c.argc < 2 ? 2 : 0;
    }
    const char *cmd = c.argv[1];
    cJSON *a = dev_load();
    if (!a) { errf(c, "dev: %s is unreadable or not a JSON array (fix it: jq . %s)\n", kDevFile, kDevFile); return 1; }
    int rc = 0;
    if (!strcmp(cmd, "scan")) {
        int found = 0;
        static const char *const kSvc[][2] = { {"_shelly", "shelly"}, {"_wled", "wled"} };
        for (const auto &sv : kSvc) {
            mdns_result_t *res = nullptr;
            if (mdns_query_ptr(sv[0], "_tcp", 3000, 20, &res) != ESP_OK) continue;
            for (mdns_result_t *r = res; r; r = r->next) {
                if (!r->addr) continue;
                char ip[20], name[48];
                snprintf(ip, sizeof ip, IPSTR, IP2STR(&r->addr->addr.u_addr.ip4));
                dev_name(r->instance_name ? r->instance_name : r->hostname ? r->hostname : ip, name, sizeof name);
                int gen = 0;
                if (!strcmp(sv[1], "shelly")) {                    // Gen1 has /shelly without "gen"
                    ShBuf o;
                    if (dev_http(c, "GET", ip, "/shelly", nullptr, o) && o.p) {
                        cJSON *j = cJSON_ParseWithLength(o.p, o.n);
                        cJSON *g = cJSON_GetObjectItem(j, "gen");
                        gen = cJSON_IsNumber(g) ? g->valueint : 1;
                        cJSON_Delete(j);
                    }
                    buf_free(o);
                }
                dev_put(a, name, sv[1], ip, gen);
                outf(c, "found %s %s %s\n", name, sv[1], ip);
                found++;
            }
            mdns_query_results_free(res);
        }
        if (found) dev_save(a);
        else outf(c, "no Shelly/WLED announced on mDNS (add Tasmota or others: dev add NAME TYPE IP)\n");
    } else if (!strcmp(cmd, "ls")) {
        if (!cJSON_GetArraySize(a)) outf(c, "no devices (dev scan, or dev add NAME TYPE IP)\n");
        cJSON *d;
        cJSON_ArrayForEach(d, a) outf(c, "%s %s %s\n", dstr(d, "name"), dstr(d, "type"), dstr(d, "ip"));
    } else if (!strcmp(cmd, "add") && c.argc >= 5) {
        if (strcmp(c.argv[3], "shelly") && strcmp(c.argv[3], "tasmota") && strcmp(c.argv[3], "wled")) { errf(c, "dev: type must be shelly, tasmota or wled\n"); rc = 2; }
        else { dev_put(a, c.argv[2], c.argv[3], c.argv[4], 2); rc = dev_save(a) ? 0 : 1; if (!rc) outf(c, "added %s\n", c.argv[2]); }
    } else if (!strcmp(cmd, "rm") && c.argc >= 3) {
        cJSON *d = dev_find(a, c.argv[2]);
        if (!d) { errf(c, "dev: no device %s\n", c.argv[2]); rc = 1; }
        else { cJSON_DetachItemViaPointer(a, d); cJSON_Delete(d); dev_save(a); outf(c, "removed\n"); }
    } else if (c.argc >= 3 && (!strcmp(cmd, "get") || !strcmp(cmd, "on") || !strcmp(cmd, "off") || !strcmp(cmd, "toggle") || !strcmp(cmd, "set"))) {
        cJSON *d = dev_find(a, c.argv[2]);
        if (!d) { errf(c, "dev: no device %s (dev ls)\n", c.argv[2]); rc = 1; }
        else if (strcmp(cmd, "get")) {
            const char *t = dstr(d, "type"), *ip = dstr(d, "ip");
            cJSON *g = cJSON_GetObjectItem(d, "gen");
            const bool gen1 = cJSON_IsNumber(g) && g->valueint == 1;
            int bri = -1;
            for (int i = 3; i < c.argc; i++) if (!strncmp(c.argv[i], "bri=", 4)) bri = atoi(c.argv[i] + 4);
            if (!strcmp(cmd, "set") && (bri < 0 || bri > 100)) { errf(c, "dev: set NAME bri=0-100\n"); rc = 2; }
            char path[120] = "", body[96] = "";
            const char *act = !strcmp(cmd, "on") ? "on" : !strcmp(cmd, "off") ? "off" : "toggle";
            if (rc) {
            } else if (!strcmp(t, "shelly")) {
                if (bri >= 0) snprintf(path, sizeof path, gen1 ? "/light/0?brightness=%d" : "/rpc/Light.Set?id=0&brightness=%d", bri);
                else if (gen1) snprintf(path, sizeof path, "/relay/0?turn=%s", act);
                else if (act[0] == 't') snprintf(path, sizeof path, "/rpc/Switch.Toggle?id=0");
                else snprintf(path, sizeof path, "/rpc/Switch.Set?id=0&on=%s", act[1] == 'n' ? "true" : "false");
            } else if (!strcmp(t, "tasmota")) {
                if (bri >= 0) snprintf(path, sizeof path, "/cm?cmnd=Dimmer%%20%d", bri);
                else snprintf(path, sizeof path, "/cm?cmnd=Power%%20%s", act[0] == 't' ? "Toggle" : act[1] == 'n' ? "On" : "Off");
            } else if (!strcmp(t, "wled")) {
                snprintf(path, sizeof path, "/json/state");
                if (bri >= 0) snprintf(body, sizeof body, "{\"on\":true,\"bri\":%d}", bri * 255 / 100);
                else snprintf(body, sizeof body, "{\"on\":%s}", act[0] == 't' ? "\"t\"" : act[1] == 'n' ? "true" : "false");
            }
            ShBuf out;
            if (!rc && path[0]) rc = dev_http(c, body[0] ? "POST" : "GET", ip, path, body[0] ? body : nullptr, out) ? 0 : 1;
            buf_free(out);
        }
        if (d && !rc) dev_state(c, d);
    } else {
        errf(c, "dev: unknown '%s' (dev help)\n", cmd);
        rc = 2;
    }
    cJSON_Delete(a);
    return rc;
}

// ---------------------------------------------------------------- app: build, check, run (dev loop)
// The write -> check -> run -> read the error -> fix loop of coding agents (OpenCode's diagnostics
// after every edit, Aider's auto-lint/auto-test), on the board:
//   app check FILE   syntax of a .lua / .py / .json, the error with its line and the lines around
//   app run NAME     a Lua App script (~/lua/NAME.lua or ~/lua/NAME/) started directly (~/lua/.run),
//                    then after a few seconds its exact error (~/lua/.last_error, written by the
//                    engine) or "running" + a screenshot path for ACT see
//   app ls           the Lua App scripts
void join_args(char **argv, int argc, char *out, size_t cap);
namespace {
// The terminal program's view of a home path: /sdcard/home/x -> /x (its root is the home).
const char *home_rel(const char *abs) { return !strncmp(abs, "/sdcard/home/", 13) ? abs + 12 : abs; }

// Run a terminal program (lua/python) with argv, capture its output in b. Exit status.
int prog_capture(const char *id, const char *a0, const char *a1, ShBuf &b) {
    char *av[2] = { (char *)a0, (char *)a1 };
    char args[1024];
    join_args(av, a1 ? 2 : 1, args, sizeof args);
    ShSink sk;
    sk.k = SH_BUF;
    sk.buf = &b;
    return term_prog_run(id, args, nullptr, 0, &sk);
}

// "file:12: msg" -> 12 (the first ":<n>:" after the name), 0 if none
int err_line(const char *e) {
    for (const char *p = strchr(e, ':'); p; p = strchr(p + 1, ':')) {
        int n = 0; const char *q = p + 1;
        while (*q >= '0' && *q <= '9') n = n * 10 + (*q++ - '0');
        if (n > 0 && *q == ':') return n;
    }
    return 0;
}

// The lines around `line` of the file, numbered, the bad one marked (Aider shows errors this way).
void show_context(Ctx &c, const char *path, int line) {
    if (line <= 0) return;
    FILE *f = fopen(path, "r");
    if (!f) return;
    char l[200];
    for (int n = 1; fgets(l, sizeof l, f) && n <= line + 1; n++) {
        if (n < line - 1) continue;
        l[strcspn(l, "\r\n")] = 0;
        outf(c, "%s%4d | %s\n", n == line ? ">" : " ", n, l);
    }
    fclose(f);
}

int app_check(Ctx &c, const char *arg) {
    char p[kPath];
    resolve(arg, p, sizeof p);
    const char *ext = strrchr(p, '.');
    if (!ext) { errf(c, "app check: %s: no .lua/.py/.json extension\n", arg); return 2; }
    struct stat st;
    if (stat(p, &st) != 0) { errf(c, "app check: %s: no such file\n", arg); return 2; }
    char msg[600] = "";
    if (!strcmp(ext, ".json")) {
        ShBuf b;
        if (!read_all(c, p, b)) return 2;
        const char *end = nullptr;
        cJSON *j = b.p ? cJSON_ParseWithLengthOpts(b.p, b.n, &end, false) : nullptr;
        if (j) { cJSON_Delete(j); buf_free(b); outf(c, "ok %s\n", arg); return 0; }
        int line = 1;
        for (const char *q = b.p; q && end && q < end; q++) if (*q == '\n') line++;
        buf_free(b);
        outf(c, "%s:%d: invalid JSON\n", arg, line);
        show_context(c, p, line);
        return 1;
    }
    const bool py = !strcmp(ext, ".py");
    if (!py && strcmp(ext, ".lua")) { errf(c, "app check: only .lua, .py and .json\n"); return 2; }
    char code[700];
    const char *rel = home_rel(p);
    if (strchr(rel, '\'')) { errf(c, "app check: quote in the path\n"); return 2; }
    if (py) snprintf(code, sizeof code,             // MicroPython's SyntaxError has no lineno: read the traceback
                     "p='%s'\ntry:\n compile(open(p).read(),p,'exec')\n print('ok')\n"
                     "except SyntaxError as e:\n import sys,io,re\n b=io.StringIO()\n sys.print_exception(e,b)\n"
                     " m=re.search('line ([0-9]+)',b.getvalue())\n"
                     " print('%%s:%%s: %%s' %% (p, m.group(1) if m else '0', e.args[0] if e.args else 'syntax error'))", rel);
    else snprintf(code, sizeof code, "local f,e=loadfile('%s') print(f and 'ok' or e)", rel);
    ShBuf b;
    const int rc = prog_capture(py ? "python" : "lua", py ? "-c" : "-e", code, b);
    snprintf(msg, sizeof msg, "%.*s", (int)(b.n < sizeof msg - 1 ? b.n : sizeof msg - 1), b.p ? b.p : "");
    buf_free(b);
    msg[strcspn(msg, "\r\n")] = 0;
    if (rc == 127 || (!msg[0] && rc)) { errf(c, "app check: %s is not installed (store install %s)\n", py ? "python" : "lua", py ? "python" : "lua"); return 2; }
    if (!strncmp(msg, "ok", 2)) { outf(c, "ok %s\n", arg); return 0; }
    outf(c, "%s\n", msg);
    show_context(c, p, err_line(msg));
    return 1;
}

int app_run(Ctx &c, const char *name, int secs) {
    char rel[160];                                   // "/lua/x.lua" or "/lua/x" as the engine sees it
    const char *n = name;
    if (!strncmp(n, "~/", 2)) n += 2;
    if (!strncmp(n, "/sdcard/home/", 13)) n += 13;
    if (n[0] == '/') n++;
    if (!strncmp(n, "lua/", 4)) n += 4;
    snprintf(rel, sizeof rel, "/lua/%s", n);
    size_t rl = strlen(rel);
    while (rl > 5 && rel[rl - 1] == '/') rel[--rl] = 0;
    char abs[200];
    snprintf(abs, sizeof abs, "/sdcard/home%s", rel);
    struct stat st;
    if (stat(abs, &st) != 0) {                       // "x" -> x.lua
        snprintf(rel + rl, sizeof rel - rl, ".lua");
        snprintf(abs, sizeof abs, "/sdcard/home%s", rel);
        if (stat(abs, &st) != 0) { errf(c, "app run: no ~/lua/%s(.lua) (app ls)\n", n); return 2; }
    }
    const bool is_dir = S_ISDIR(st.st_mode);          // st is reused below: keep the answer
    if (is_dir) {
        char m[220];
        snprintf(m, sizeof m, "%s/main.lua", abs);
        if (stat(m, &st) != 0) { errf(c, "app run: %s has no main.lua\n", rel); return 2; }
    } else {
        const int r = app_check(c, abs);              // a syntax error is reported before running
        if (r) return r;
    }
    remove("/sdcard/home/lua/.last_error");
    FILE *f = fopen("/sdcard/home/lua/.run", "w");
    if (!f) { errf(c, "app run: cannot write ~/lua/.run\n"); return 1; }
    fprintf(f, "%s\n", rel);
    fclose(f);
    char cur[32] = "";
    if (lvgl_port_lock(500)) { snprintf(cur, sizeof cur, "%s", nv_ui_current_app_id()); lvgl_port_unlock(); }
    if (!strcmp(cur, "luaapp")) { nv_ui_go_home_async(); vTaskDelay(pdMS_TO_TICKS(700)); }   // restart the engine
    if (!nv_ui_open_app_id_async("luaapp")) { errf(c, "app run: cannot open the Lua App\n"); return 1; }
    for (int t = 0; t < secs * 10 && !cancelled(); t++) {
        vTaskDelay(pdMS_TO_TICKS(100));
        if (stat("/sdcard/home/lua/.last_error", &st) == 0) break;
    }
    FILE *e = fopen("/sdcard/home/lua/.last_error", "r");
    if (e) {
        char buf[1200];
        const size_t k = fread(buf, 1, sizeof buf - 1, e);
        fclose(e);
        buf[k] = 0;
        outf(c, "ERROR in %s\n", rel);
        const char *msg = strchr(buf, '\n') ? strchr(buf, '\n') + 1 : buf;
        wr(c.out, msg);
        // the bad line of the app's own file ("x.lua:12: ...")
        const int ln = err_line(msg);
        char src[200] = "";
        if (ln && !is_dir) snprintf(src, sizeof src, "%s", abs);
        if (ln) {
            char fn[96] = ""; int k2 = 0;
            for (const char *q = msg; *q && *q != ':' && k2 < 95; q++) fn[k2++] = *q;
            fn[k2] = 0;
            if (fn[0] && !strchr(fn, '/') && strcmp(fn, strrchr(abs, '/') + 1)) snprintf(src, sizeof src, "%s/%s", abs, fn);
            else if (!strncmp(fn, "/lua/", 5) && !strstr(fn, "..")) snprintf(src, sizeof src, "/sdcard/home%s", fn);   // engine path
            if (src[0]) show_context(c, src, ln);
        }
        return 1;
    }
    char shot[96];
    snprintf(shot, sizeof shot, "/sdcard/home/shots/app-run.jpg");
    mkdir("/sdcard/home/shots", 0777);
    const bool ok = nv_hal_screenshot(shot);
    outf(c, "running %s for %ds: no errors%s%s\n", rel, secs, ok ? "; screen: " : "", ok ? shot : "");
    return 0;
}
}  // namespace

int b_app(Ctx &c) {
    if (c.argc < 2 || !strcmp(c.argv[1], "help")) {
        outf(c, "usage: app check FILE | app run NAME [-t SECONDS] | app ls\n");
        return c.argc < 2 ? 2 : 0;
    }
    if (!strcmp(c.argv[1], "check") && c.argc >= 3) {
        int rc = 0;
        for (int i = 2; i < c.argc; i++) rc |= app_check(c, c.argv[i]);
        return rc;
    }
    if (!strcmp(c.argv[1], "run") && c.argc >= 3) {
        int secs = 4;
        if (c.argc >= 5 && !strcmp(c.argv[3], "-t")) secs = atoi(c.argv[4]);
        if (secs < 1) secs = 1;
        if (secs > 30) secs = 30;
        const char *e = strrchr(c.argv[2], '.');
        if (e && !strcmp(e, ".py")) { errf(c, "app run: a .py runs in the shell: python %s\n", c.argv[2]); return 2; }
        return app_run(c, c.argv[2], secs);
    }
    if (!strcmp(c.argv[1], "ls")) {
        DIR *d = opendir("/sdcard/home/lua");
        int n = 0;
        struct dirent *e;
        while (d && (e = readdir(d))) {
            if (e->d_name[0] == '.') continue;
            const size_t l = strlen(e->d_name);
            if (e->d_type == DT_DIR || (l > 4 && !strcmp(e->d_name + l - 4, ".lua"))) { outf(c, "%s%s\n", e->d_name, e->d_type == DT_DIR ? "/" : ""); n++; }
        }
        if (d) closedir(d);
        if (!n) outf(c, "no Lua App scripts in ~/lua\n");
        return 0;
    }
    errf(c, "app: unknown '%s' (app help)\n", c.argv[1]);
    return 2;
}

// ---------------------------------------------------------------- GUI automation (computer use)
// ui: the screen as text, an accessibility snapshot with numbered refs (the Playwright MCP idea):
//   [3] button "Salva" @820,540        [4] text "Wi-Fi spento"        [5] switch "Bluetooth" on
// tap @3 | tap "Salva" | tap X Y, type TEXT, key enter|esc|..., swipe X0 Y0 X1 Y1 [MS], home.
// Cheap for a language model (no image) and exact (centres from LVGL, not guessed from pixels).
// Run by the agent (headless capture), an action prints the new snapshot right away.
namespace {
constexpr int kUiMax = 160;
struct UiRef { int16_t x, y; };
// Allocated in PSRAM on the first snapshot (8 KB the static budget can't spare); never freed.
UiRef *s_ui_ref = nullptr;
char (*s_ui_txt)[48] = nullptr;
int s_ui_n = 0;

const char *ui_role(lv_obj_t *o) {
    if (lv_obj_check_type(o, &lv_button_class))   return "button";
    if (lv_obj_check_type(o, &lv_label_class))    return "text";
    if (lv_obj_check_type(o, &lv_textarea_class)) return "field";
    if (lv_obj_check_type(o, &lv_switch_class))   return "switch";
    if (lv_obj_check_type(o, &lv_checkbox_class)) return "checkbox";
    if (lv_obj_check_type(o, &lv_slider_class))   return "slider";
    if (lv_obj_check_type(o, &lv_dropdown_class)) return "dropdown";
    if (lv_obj_check_type(o, &lv_image_class))    return "image";
    return "item";
}

// The text a node shows: its own (label, field, checkbox, dropdown) or its first child labels'.
void ui_text(lv_obj_t *o, char *out, size_t cap, int depth = 0) {
    if (lv_obj_check_type(o, &lv_label_class)) { snprintf(out + strlen(out), cap - strlen(out), "%s%s", out[0] ? " " : "", lv_label_get_text(o)); return; }
    if (lv_obj_check_type(o, &lv_textarea_class)) {
        const char *t = lv_textarea_get_text(o);
        snprintf(out + strlen(out), cap - strlen(out), "%s", t && t[0] ? t : lv_textarea_get_placeholder_text(o));
        return;
    }
    if (lv_obj_check_type(o, &lv_checkbox_class)) { snprintf(out, cap, "%s", lv_checkbox_get_text(o)); return; }
    if (lv_obj_check_type(o, &lv_dropdown_class)) { lv_dropdown_get_selected_str(o, out, (uint32_t)cap); return; }
    if (depth > 3) return;
    const uint32_t n = lv_obj_get_child_count(o);
    for (uint32_t i = 0; i < n && strlen(out) + 4 < cap; i++) ui_text(lv_obj_get_child(o, (int32_t)i), out, cap, depth + 1);
}

void ui_walk(lv_obj_t *o, char *buf, size_t cap, size_t &len, bool in_click) {
    if (!o || lv_obj_has_flag(o, LV_OBJ_FLAG_HIDDEN) || !lv_obj_is_visible(o)) return;
    lv_area_t a;
    lv_obj_get_coords(o, &a);
    if (a.x2 < 0 || a.y2 < 0 || a.x1 > 1023 || a.y1 > 599 || a.x2 <= a.x1 || a.y2 <= a.y1) return;
    const bool click = lv_obj_has_flag(o, LV_OBJ_FLAG_CLICKABLE) && !lv_obj_check_type(o, &lv_label_class);
    const bool label = lv_obj_check_type(o, &lv_label_class);
    char t[64] = "";
    if (click || (label && !in_click)) {
        ui_text(o, t, sizeof t);
        for (char *c = t; *c; c++) if (*c == '\n' || *c == '"') *c = ' ';
    }
    const bool emit = (click && (t[0] || !lv_obj_check_type(o, &lv_obj_class))) || (label && !in_click && t[0]);
    if (emit && s_ui_n < kUiMax && len + 100 < cap) {
        const int cx = (LV_MAX(a.x1, 0) + LV_MIN(a.x2, 1023)) / 2, cy = (LV_MAX(a.y1, 0) + LV_MIN(a.y2, 599)) / 2;
        s_ui_ref[s_ui_n] = { (int16_t)cx, (int16_t)cy };
        snprintf(s_ui_txt[s_ui_n], sizeof s_ui_txt[0], "%s", t);
        const lv_state_t st = lv_obj_get_state(o);
        len += snprintf(buf + len, cap - len, "[%d] %s \"%.40s\"%s%s%s%s\n", s_ui_n, ui_role(o), t,
                        click ? "" : "",
                        (st & LV_STATE_CHECKED) ? " on" : (lv_obj_check_type(o, &lv_switch_class) ? " off" : ""),
                        (st & LV_STATE_DISABLED) ? " disabled" : "", (st & LV_STATE_FOCUSED) ? " focused" : "");
        if (click) {   // "@x,y" only where a tap does something
            len--;     // before the newline
            len += snprintf(buf + len, cap - len, " @%d,%d\n", cx, cy);
        }
        s_ui_n++;
    }
    if (label) return;
    const uint32_t n = lv_obj_get_child_count(o);
    for (uint32_t i = 0; i < n; i++) ui_walk(lv_obj_get_child(o, (int32_t)i), buf, cap, len, in_click || (click && t[0]));
}

// Snapshot into buf (malloc'd by the caller). Returns its length, 0 when the UI is busy.
size_t ui_snapshot(char *buf, size_t cap) {
    if (!s_ui_ref) {
        s_ui_ref = (UiRef *)heap_caps_malloc(kUiMax * sizeof(UiRef), MALLOC_CAP_SPIRAM);
        s_ui_txt = (char (*)[48])heap_caps_malloc(kUiMax * sizeof *s_ui_txt, MALLOC_CAP_SPIRAM);
        if (!s_ui_ref || !s_ui_txt) {
            free(s_ui_ref); free(s_ui_txt);
            s_ui_ref = nullptr; s_ui_txt = nullptr;
            return 0;
        }
    }
    if (!lvgl_port_lock(1000)) return 0;
    s_ui_n = 0;
    const char *app = nv_ui_current_app_id();
    size_t len = (size_t)snprintf(buf, cap, "screen: %s%s\n", app && app[0] ? "app " : "home", app ? app : "");
    ui_walk(lv_screen_active(), buf, cap, len, false);
    ui_walk(lv_layer_top(), buf, cap, len, false);       // dialogs, shade, keyboard
    lvgl_port_unlock();
    if (s_ui_n >= kUiMax) len += snprintf(buf + len, cap - len, "(more items not listed)\n");
    return len;
}

void ui_after(Ctx &c) {   // after an action: let the UI settle, then the new screen for the agent
    vTaskDelay(pdMS_TO_TICKS(500));
    if (!sh_capturing()) return;
    char *b = (char *)heap_caps_malloc(8192, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!b) return;
    const size_t n = ui_snapshot(b, 8192);
    if (n) outf(c, "%.*s", (int)n, b);
    free(b);
}
}  // namespace

int b_ui(Ctx &c) {
    char *b = (char *)heap_caps_malloc(8192, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!b) { errf(c, "ui: out of memory\n"); return 1; }
    const size_t n = ui_snapshot(b, 8192);
    if (!n) { free(b); errf(c, "ui: the screen is busy, try again\n"); return 1; }
    outf(c, "%.*s", (int)n, b);
    free(b);
    return 0;
}

int b_tap(Ctx &c) {
    int x = -1, y = -1;
    char what[64] = "";
    if (c.argc == 3) { x = atoi(c.argv[1]); y = atoi(c.argv[2]); snprintf(what, sizeof what, "%d,%d", x, y); }
    else if (c.argc >= 2) {
        if (s_ui_n == 0) { char *b = (char *)heap_caps_malloc(8192, MALLOC_CAP_SPIRAM); if (b) { ui_snapshot(b, 8192); free(b); } }
        if (c.argv[1][0] == '@') {
            const int r = atoi(c.argv[1] + 1);
            if (r >= 0 && r < s_ui_n) { x = s_ui_ref[r].x; y = s_ui_ref[r].y; snprintf(what, sizeof what, "[%d] \"%.40s\"", r, s_ui_txt[r]); }
        } else {
            char want[64] = "";
            for (int i = 1; i < c.argc; i++) snprintf(want + strlen(want), sizeof want - strlen(want), "%s%s", i > 1 ? " " : "", c.argv[i]);
            for (int r = 0; r < s_ui_n && x < 0; r++)
                if (strcasestr(s_ui_txt[r], want)) { x = s_ui_ref[r].x; y = s_ui_ref[r].y; snprintf(what, sizeof what, "[%d] \"%.40s\"", r, s_ui_txt[r]); }
        }
    }
    if (x < 0 || y < 0 || x > 1023 || y > 599) {
        errf(c, "usage: tap @REF | tap TEXT | tap X Y   (refs and texts from: ui)\n");
        return 1;
    }
    if (!lvgl_port_lock(1000)) { errf(c, "tap: the screen is busy\n"); return 1; }
    nv_ui_tap(x, y);
    lvgl_port_unlock();
    s_ui_n = 0;   // refs are stale after an action
    outf(c, "tapped %s @%d,%d\n", what, x, y);
    ui_after(c);
    return 0;
}

int b_swipe(Ctx &c) {
    if (c.argc < 5) { errf(c, "usage: swipe X0 Y0 X1 Y1 [MS]\n"); return 1; }
    const int ms = c.argc > 5 ? atoi(c.argv[5]) : 300;
    if (!lvgl_port_lock(1000)) { errf(c, "swipe: the screen is busy\n"); return 1; }
    nv_ui_swipe(atoi(c.argv[1]), atoi(c.argv[2]), atoi(c.argv[3]), atoi(c.argv[4]), ms);
    lvgl_port_unlock();
    vTaskDelay(pdMS_TO_TICKS(ms));
    s_ui_n = 0;
    outf(c, "swiped\n");
    ui_after(c);
    return 0;
}

int b_type(Ctx &c) {
    if (c.argc < 2) { errf(c, "usage: type TEXT   (into the focused field: tap it first)\n"); return 1; }
    char t[256] = "";
    for (int i = 1; i < c.argc; i++) snprintf(t + strlen(t), sizeof t - strlen(t), "%s%s", i > 1 ? " " : "", c.argv[i]);
    bool ok = false;
    if (lvgl_port_lock(1000)) { ok = nv_ime_inject_text(t); lvgl_port_unlock(); }
    if (!ok) { errf(c, "type: no text field is focused (tap one first)\n"); return 1; }
    outf(c, "typed %zu chars\n", strlen(t));
    ui_after(c);
    return 0;
}

int b_key(Ctx &c) {
    static const struct { const char *n; nv_ime_remote_key_t k; } kKeys[] = {
        {"enter", NV_IME_RK_ENTER}, {"esc", NV_IME_RK_ESC}, {"backspace", NV_IME_RK_BACKSPACE},
        {"delete", NV_IME_RK_DELETE}, {"tab", NV_IME_RK_TAB}, {"left", NV_IME_RK_LEFT},
        {"right", NV_IME_RK_RIGHT}, {"up", NV_IME_RK_UP}, {"down", NV_IME_RK_DOWN} };
    if (c.argc < 2) { errf(c, "usage: key enter|esc|backspace|delete|tab|left|right|up|down\n"); return 1; }
    for (const auto &k : kKeys) {
        if (strcmp(c.argv[1], k.n)) continue;
        bool ok = false;
        if (lvgl_port_lock(1000)) { ok = nv_ime_inject_key(k.k); lvgl_port_unlock(); }
        if (!ok) { errf(c, "key: no text field is focused\n"); return 1; }
        outf(c, "key %s\n", k.n);
        ui_after(c);
        return 0;
    }
    errf(c, "key: unknown key '%s'\n", c.argv[1]);
    return 1;
}

int b_home(Ctx &c);
// input tap|text|keyevent|swipe ...: Android's `adb shell input`, which models already know.
int b_input(Ctx &c) {
    if (c.argc < 2) { errf(c, "usage: input tap X Y | tap @REF | text TEXT | keyevent ENTER|ESCAPE|DEL|TAB|DPAD_* | swipe X0 Y0 X1 Y1 [MS]\n"); return 1; }
    Ctx s = c;
    s.argc = c.argc - 1;
    s.argv = c.argv + 1;
    const char *sub = c.argv[1];
    if (!strcmp(sub, "tap")) return b_tap(s);
    if (!strcmp(sub, "text")) return b_type(s);
    if (!strcmp(sub, "swipe")) return b_swipe(s);
    if (!strcmp(sub, "keyevent") || !strcmp(sub, "key")) {
        if (s.argc < 2) return b_key(s);
        const char *k = s.argv[1];
        if (!strncasecmp(k, "KEYCODE_", 8)) k += 8;
        static const char *const kMap[][2] = { {"ENTER", "enter"}, {"ESCAPE", "esc"}, {"BACK", "esc"}, {"DEL", "backspace"},
            {"FORWARD_DEL", "delete"}, {"TAB", "tab"}, {"DPAD_LEFT", "left"}, {"DPAD_RIGHT", "right"},
            {"DPAD_UP", "up"}, {"DPAD_DOWN", "down"} };
        char low[16];
        snprintf(low, sizeof low, "%s", k);
        for (const auto &m : kMap) if (!strcasecmp(k, m[0])) snprintf(low, sizeof low, "%s", m[1]);
        if (!strcasecmp(k, "HOME")) return b_home(s);
        for (char *q = low; *q; q++) *q = (char)tolower((unsigned char)*q);
        char *av[2] = { s.argv[0], low };
        Ctx k2 = s;
        k2.argc = 2;
        k2.argv = av;
        return b_key(k2);
    }
    errf(c, "input: unknown '%s' (tap, text, keyevent, swipe)\n", sub);
    return 1;
}

int b_home(Ctx &c) {
    if (!nv_ui_go_home_async()) { errf(c, "home: busy\n"); return 1; }
    s_ui_n = 0;
    outf(c, "home\n");
    ui_after(c);
    return 0;
}

// screenshot [-d SEC] [FILE]: the screen as a JPEG (hardware encoder, nv_hal_screenshot). Default
// ~/shots/shot-YYYYmmdd-HHMMSS.jpg; prints the path, so ANIMA can follow with "ACT see <path>".
int b_screenshot(Ctx &c) {
    int i = 1, delay = 0;
    if (i + 1 < c.argc && !strcmp(c.argv[i], "-d")) { delay = atoi(c.argv[i + 1]); i += 2; }
    if (delay < 0 || delay > 60) { errf(c, "screenshot: -d takes 0..60 seconds\n"); return 1; }
    char p[kPath];
    if (i < c.argc) {
        resolve(c.argv[i], p, sizeof p);
    } else {
        mkdir("/sdcard/home/shots", 0777);
        time_t now = time(nullptr);
        struct tm tm;
        localtime_r(&now, &tm);
        snprintf(p, sizeof p, "/sdcard/home/shots/shot-%04d%02d%02d-%02d%02d%02d.jpg", tm.tm_year + 1900,
                 tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec);
    }
    if (delay) vTaskDelay(pdMS_TO_TICKS(delay * 1000));
    if (!nv_hal_screenshot(p)) { errf(c, "screenshot: capture failed\n"); return 1; }
    struct stat st;
    outf(c, "%s (%ld KB, 1024x600)\n", p, stat(p, &st) == 0 ? (long)(st.st_size / 1024) : 0L);
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

// ---------------------------------------------------------------- full-screen programs (raw tty)

// Keys decoded from the raw byte stream (xterm sequences).
enum Key : int {
    KEY_NONE = -1, KEY_UP = 0x10000, KEY_DOWN, KEY_LEFT, KEY_RIGHT, KEY_HOME, KEY_END,
    KEY_PGUP, KEY_PGDN, KEY_DEL, KEY_ESC, KEY_GONE,
};

// Next key within timeout_ms: a byte, a Ctrl code (1..26), or a KEY_* for a sequence.
int read_key(int timeout_ms) {
    char c;
    const int r = term_tty_read(&c, 1, timeout_ms);
    if (r < 0) return KEY_GONE;
    if (r == 0) return KEY_NONE;
    if (c != 0x1b) return (unsigned char)c;
    char s[8];
    int n = 0;
    while (n < 6) {
        if (term_tty_read(&s[n], 1, 40) <= 0) break;
        n++;
        const char last = s[n - 1];
        if (n >= 2 && ((last >= 'A' && last <= 'Z') || last == '~')) break;
        if (n == 1 && s[0] != '[' && s[0] != 'O') break;
    }
    if (!n) return KEY_ESC;
    s[n] = '\0';
    const char *q = s + 1;
    switch (*q) {
        case 'A': return KEY_UP;
        case 'B': return KEY_DOWN;
        case 'C': return KEY_RIGHT;
        case 'D': return KEY_LEFT;
        case 'H': return KEY_HOME;
        case 'F': return KEY_END;
        default: break;
    }
    if (!strcmp(q, "1~") || !strcmp(q, "7~")) return KEY_HOME;
    if (!strcmp(q, "4~") || !strcmp(q, "8~")) return KEY_END;
    if (!strcmp(q, "5~")) return KEY_PGUP;
    if (!strcmp(q, "6~")) return KEY_PGDN;
    if (!strcmp(q, "3~")) return KEY_DEL;
    return KEY_NONE;
}

// Enter / leave the alternate screen with raw keys; the shell restores cooked mode afterwards too.
void tui_begin(Ctx &c) { term_tty_raw(true); wr(c.out, "\x1b[?1049h\x1b[H\x1b[2J"); }
void tui_end(Ctx &c)   { wr(c.out, "\x1b[0m\x1b[?25h\x1b[?1049l"); term_tty_raw(false); }

// A one-line prompt on row `row` (1-based): returns false on Esc / ^C.
bool tui_prompt(Ctx &c, int row, const char *label, char *out, size_t cap) {
    size_t n = strlen(out);
    for (;;) {
        char b[kPath + 64];
        const int k = snprintf(b, sizeof b, "\x1b[%d;1H\x1b[7m%s\x1b[0m %s\x1b[K", row, label, out);
        wr(c.out, b, (size_t)k);
        const int key = read_key(60000);
        if (key == KEY_GONE || key == KEY_ESC || key == 3 || key == 7) return false;
        if (key == '\r' || key == '\n') return true;
        if ((key == 0x7f || key == 8) && n) {
            do { n--; } while (n && ((unsigned char)out[n] & 0xC0) == 0x80);
            out[n] = '\0';
        } else if (key >= 0x20 && key < 0x100 && n + 1 < cap) {
            out[n++] = (char)key;
            out[n] = '\0';
        }
    }
}

// Printable width of text that may carry ANSI escapes (less -R).
int vis_len(const char *s, size_t n, size_t *cut, int max) {
    int w = 0;
    size_t i = 0;
    while (i < n) {
        if (s[i] == 0x1b) {
            i++;
            if (i < n && s[i] == '[') { i++; while (i < n && !(s[i] >= 0x40 && s[i] <= 0x7E)) i++; }
            if (i < n) i++;
            continue;
        }
        if (((unsigned char)s[i] & 0xC0) != 0x80) {
            if (w == max) break;
            w += s[i] == '\t' ? 8 - (w % 8) : 1;
        }
        i++;
    }
    if (cut) *cut = i;
    return w;
}

// less [FILE]: a pager on the alternate screen. Without a terminal it is cat.
int b_less(Ctx &c) {
    const char *name = c.argc > 1 ? c.argv[1] : "-";
    if (!tty(c)) {
        ShBuf b;
        if (!read_all(c, name, b)) return 1;
        wr(c.out, b.p ? b.p : "", b.n);
        buf_free(b);
        return 0;
    }
    ShBuf b;
    if (!read_all(c, name, b)) return 1;
    if (!b.n && !strcmp(name, "-")) { errf(c, "Missing filename (\"less --help\" for help)\n"); return 1; }
    const int cols = term_tty_cols();
    // Display lines: every file line cut into screen-wide pieces.
    struct DLine { uint32_t off, len; };
    size_t cap = 1024, nd = 0;
    DLine *dl = (DLine *)ps_alloc(sizeof(DLine) * cap);
    const char *p = b.p ? b.p : "";
    each_line(p, b.n, [&](const char *s, size_t len) {
        size_t o = 0;
        do {
            size_t cut = len - o;
            vis_len(s + o, len - o, &cut, cols);
            if (nd == cap) {
                DLine *g = (DLine *)ps_realloc(dl, sizeof(DLine) * cap * 2);
                if (!g) return false;
                dl = g;
                cap *= 2;
            }
            dl[nd++] = {(uint32_t)(s + o - p), (uint32_t)cut};
            o += cut ? cut : len - o;
        } while (o < len);
        return true;
    });
    if (!dl) { buf_free(b); return 1; }
    tui_begin(c);
    int top = 0;
    char search[128] = "";
    char msg[96] = "";
    const Re *re = nullptr;
    Re *rebuf = (Re *)ps_alloc(sizeof(Re));
    for (;;) {
        const int rows = term_tty_rows();
        const int page = rows - 1 > 1 ? rows - 1 : 1;
        const int maxtop = (int)nd > page ? (int)nd - page : 0;
        if (top > maxtop) top = maxtop;
        if (top < 0) top = 0;
        wr(c.out, "\x1b[?25l\x1b[H");
        for (int r = 0; r < page; r++) {
            const int i = top + r;
            if (i < (int)nd) {
                const DLine &d = dl[i];
                const char *s = p + d.off;
                const char *ms, *me;
                if (re && re_search(*re, s, s + d.len, &ms, &me) && me > ms) {
                    wr(c.out, s, (size_t)(ms - s));
                    wr(c.out, "\x1b[7m");
                    wr(c.out, ms, (size_t)(me - ms));
                    wr(c.out, "\x1b[27m");
                    wr(c.out, me, (size_t)(s + d.len - me));
                } else {
                    wr(c.out, s, d.len);
                }
                wr(c.out, "\x1b[0m");
            } else {
                wr(c.out, "\x1b[1;34m~\x1b[0m");
            }
            wr(c.out, "\x1b[K\r\n");
        }
        // Status line.
        char st[160];
        const int pct = nd ? (int)((top + page < (int)nd ? top + page : (int)nd) * 100 / (int)nd) : 100;
        if (msg[0]) snprintf(st, sizeof st, "%s", msg);
        else if (top >= maxtop) snprintf(st, sizeof st, "(END)");
        else snprintf(st, sizeof st, "%s lines %d-%d/%u %d%%", strcmp(name, "-") ? name : "standard input",
                      top + 1, top + page, (unsigned)nd, pct);
        msg[0] = '\0';
        outf(c, "\x1b[7m%s\x1b[0m\x1b[K", st);
        const int k = read_key(60000);
        if (k == KEY_NONE) continue;
        if (k == KEY_GONE || k == 'q' || k == 'Q' || k == 3) break;
        switch (k) {
            case ' ': case 'f': case 6: case KEY_PGDN: top += page; break;
            case 'b': case 2: case KEY_PGUP: top -= page; break;
            case 'j': case '\r': case 'e': case 14: case KEY_DOWN: top++; break;
            case 'k': case 'y': case 16: case KEY_UP: top--; break;
            case 'd': top += page / 2; break;
            case 'u': top -= page / 2; break;
            case 'g': case '<': case KEY_HOME: top = 0; break;
            case 'G': case '>': case KEY_END: top = maxtop; break;
            case '/': {
                char q[128] = "";
                if (tui_prompt(c, rows, "/", q, sizeof q) && q[0] && rebuf) {
                    const char *err = nullptr;
                    snprintf(search, sizeof search, "%s", q);
                    if (re_compile(*rebuf, search, false, false, &err)) re = rebuf;
                    else { snprintf(msg, sizeof msg, "Invalid pattern: %s", err); re = nullptr; break; }
                }
            }
            [[fallthrough]];
            case 'n': case 'N': {
                if (!re) { snprintf(msg, sizeof msg, "No previous search"); break; }
                const int dir = k == 'N' ? -1 : 1;
                int i = top + dir;
                const char *ms, *me;
                for (; i >= 0 && i < (int)nd; i += dir)
                    if (re_search(*re, p + dl[i].off, p + dl[i].off + dl[i].len, &ms, &me)) break;
                if (i >= 0 && i < (int)nd) top = i;
                else snprintf(msg, sizeof msg, "Pattern not found");
                break;
            }
            case 'h':
                snprintf(msg, sizeof msg, "q quit  space/b page  j/k line  g/G top/end  /pattern  n/N next/prev");
                break;
            default: break;
        }
    }
    tui_end(c);
    heap_caps_free(rebuf);
    heap_caps_free(dl);
    buf_free(b);
    return 0;
}

// ---- edit: a small nano-style editor

struct Ed {
    char   **ln = nullptr;   // lines, each a PSRAM string without '\n'
    int      n = 0, cap = 0;
    int      cy = 0, cx = 0; // cursor: line, byte offset
    int      top = 0, left = 0;
    bool     dirty = false;
    char     path[kPath] = "";
    char     shown[128] = "";
    char     msg[160] = "";
    char    *cut = nullptr;  // cut buffer (one or more lines joined by '\n')
    bool     cut_chain = false;
};

bool ed_insert_line(Ed &e, int at, const char *s, size_t len) {
    if (e.n == e.cap) {
        const int nc = e.cap ? e.cap * 2 : 64;
        char **g = (char **)ps_realloc(e.ln, sizeof(char *) * nc);
        if (!g) return false;
        e.ln = g;
        e.cap = nc;
    }
    char *l = (char *)ps_alloc(len + 1);
    if (!l) return false;
    memcpy(l, s, len);
    l[len] = '\0';
    memmove(e.ln + at + 1, e.ln + at, sizeof(char *) * (e.n - at));
    e.ln[at] = l;
    e.n++;
    return true;
}

void ed_delete_line(Ed &e, int at) {
    heap_caps_free(e.ln[at]);
    memmove(e.ln + at, e.ln + at + 1, sizeof(char *) * (e.n - at - 1));
    e.n--;
}

// Display column of byte offset x in line s (tabs to 8, one cell per character).
int ed_col(const char *s, int x) {
    int col = 0;
    for (int i = 0; i < x && s[i]; i++) {
        if (s[i] == '\t') col += 8 - col % 8;
        else if (((unsigned char)s[i] & 0xC0) != 0x80) col++;
    }
    return col;
}

int ed_prev(const char *s, int x) { do { x--; } while (x > 0 && ((unsigned char)s[x] & 0xC0) == 0x80); return x < 0 ? 0 : x; }
int ed_next(const char *s, int x) { if (!s[x]) return x; do { x++; } while (s[x] && ((unsigned char)s[x] & 0xC0) == 0x80); return x; }

bool ed_save(Ctx &c, Ed &e) {
    FILE *f = fopen(e.path, "wb");
    if (!f) { snprintf(e.msg, sizeof e.msg, "[ Error writing %s ]", e.shown); return false; }
    size_t bytes = 0;
    for (int i = 0; i < e.n; i++) {
        const size_t l = strlen(e.ln[i]);
        fwrite(e.ln[i], 1, l, f);
        fputc('\n', f);
        bytes += l + 1;
    }
    const bool ok = fclose(f) == 0;
    if (ok) { e.dirty = false; snprintf(e.msg, sizeof e.msg, "[ Wrote %d line%s ]", e.n, e.n == 1 ? "" : "s"); }
    else snprintf(e.msg, sizeof e.msg, "[ Error writing %s ]", e.shown);
    (void)c; (void)bytes;
    return ok;
}

void ed_draw(Ctx &c, Ed &e) {
    const int rows = term_tty_rows(), cols = term_tty_cols();
    const int text_rows = rows - 3 > 1 ? rows - 3 : 1;
    // Keep the cursor on screen.
    if (e.cy < e.top) e.top = e.cy;
    if (e.cy >= e.top + text_rows) e.top = e.cy - text_rows + 1;
    const int ccol = ed_col(e.ln[e.cy], e.cx);
    if (ccol < e.left) e.left = ccol;
    if (ccol >= e.left + cols) e.left = ccol - cols + 1;
    ShBuf o;
    char b[256];
    // Title bar.
    int k = snprintf(b, sizeof b, "\x1b[?25l\x1b[H\x1b[7m  edit 1.0");
    buf_put(o, b, (size_t)k);
    const int name_len = utf8_len(e.shown[0] ? e.shown : "New Buffer");
    const int mid = (cols - name_len) / 2;
    const int used = 10;
    for (int i = used; i < mid; i++) buf_put(o, " ", 1);
    buf_put(o, e.shown[0] ? e.shown : "New Buffer", strlen(e.shown[0] ? e.shown : "New Buffer"));
    int w = (mid > used ? mid : used) + name_len;
    const char *mod = e.dirty ? "Modified  " : "";
    const int ml = (int)strlen(mod);
    for (; w < cols - ml; w++) buf_put(o, " ", 1);
    buf_put(o, mod, (size_t)ml);
    buf_put(o, "\x1b[0m\r\n", 6);
    // Text.
    for (int r = 0; r < text_rows; r++) {
        const int i = e.top + r;
        if (i < e.n) {
            const char *s = e.ln[i];
            int col = 0;
            for (int x = 0; s[x];) {
                if (s[x] == '\t') {   // tabs expand to the next multiple of 8
                    const int nx = col + 8 - col % 8;
                    for (; col < nx; col++) if (col >= e.left && col < e.left + cols) buf_put(o, " ", 1);
                    x++;
                    continue;
                }
                const int nx = ed_next(s, x);   // one whole UTF-8 character, one cell
                if (col >= e.left && col < e.left + cols) buf_put(o, s + x, (size_t)(nx - x));
                col++;
                x = nx;
            }
        }
        buf_put(o, "\x1b[K\r\n", 5);
    }
    // Status line, then the help line (nano style).
    k = snprintf(b, sizeof b, "\x1b[K%s%s%s\r\n", e.msg[0] ? "\x1b[7m" : "", e.msg, e.msg[0] ? "\x1b[0m" : "");
    buf_put(o, b, (size_t)k);
    static const char *kHelp[][2] = {
        {"^S", "Save"}, {"^X", "Exit"}, {"^W", "Search"}, {"^K", "Cut"}, {"^U", "Paste"}, {"^G", "Go To Line"},
    };
    for (const auto &h : kHelp) {
        k = snprintf(b, sizeof b, "\x1b[7m%s\x1b[0m %-11s", h[0], h[1]);
        buf_put(o, b, (size_t)k);
    }
    buf_put(o, "\x1b[K", 3);
    k = snprintf(b, sizeof b, "\x1b[%d;%dH\x1b[?25h", e.cy - e.top + 2, ccol - e.left + 1);
    buf_put(o, b, (size_t)k);
    wr(c.out, o.p ? o.p : "", o.n);
    buf_free(o);
    e.msg[0] = '\0';
}

void ed_free(Ed &e) {
    for (int i = 0; i < e.n; i++) heap_caps_free(e.ln[i]);
    heap_caps_free(e.ln);
    heap_caps_free(e.cut);
}

int b_edit(Ctx &c) {
    if (!tty(c)) { errf(c, "edit: needs the terminal\n"); return 1; }
    Ed *ep = new Ed();
    Ed &e = *ep;
    if (c.argc > 1) {
        resolve(c.argv[1], e.path, sizeof e.path);
        snprintf(e.shown, sizeof e.shown, "%s", c.argv[1]);
        if (is_dir(e.path)) { errf(c, "edit: %s: Is a directory\n", c.argv[1]); delete ep; return 1; }
        ShBuf b;
        FILE *f = fopen(e.path, "rb");
        if (f) {
            char chunk[2048];
            size_t k;
            while ((k = fread(chunk, 1, sizeof chunk, f)) > 0 && !b.trunc) buf_put(b, chunk, k);
            fclose(f);
            if (b.trunc) { errf(c, "edit: %s: too large\n", c.argv[1]); buf_free(b); delete ep; return 1; }
            each_line(b.p ? b.p : "", b.n, [&](const char *s, size_t len) {
                if (len && s[len - 1] == '\r') len--;
                return ed_insert_line(e, e.n, s, len);
            });
            snprintf(e.msg, sizeof e.msg, "[ Read %d line%s ]", e.n, e.n == 1 ? "" : "s");
        } else {
            snprintf(e.msg, sizeof e.msg, "[ New File ]");
        }
        buf_free(b);
    }
    if (!e.n) ed_insert_line(e, 0, "", 0);
    tui_begin(c);
    bool quit = false;
    while (!quit) {
        ed_draw(c, e);
        const int k = read_key(60000);
        if (k == KEY_NONE) continue;
        if (k == KEY_GONE) break;
        const int rows = term_tty_rows();
        const int page = rows - 4 > 1 ? rows - 4 : 1;
        char *L = e.ln[e.cy];
        if (k != 11) e.cut_chain = false;
        switch (k) {
            case KEY_UP:    if (e.cy > 0) { const int col = ed_col(L, e.cx); e.cy--; e.cx = 0; while (e.ln[e.cy][e.cx] && ed_col(e.ln[e.cy], e.cx) < col) e.cx = ed_next(e.ln[e.cy], e.cx); } break;
            case KEY_DOWN:  if (e.cy + 1 < e.n) { const int col = ed_col(L, e.cx); e.cy++; e.cx = 0; while (e.ln[e.cy][e.cx] && ed_col(e.ln[e.cy], e.cx) < col) e.cx = ed_next(e.ln[e.cy], e.cx); } break;
            case KEY_LEFT:  if (e.cx > 0) e.cx = ed_prev(L, e.cx); else if (e.cy > 0) { e.cy--; e.cx = (int)strlen(e.ln[e.cy]); } break;
            case KEY_RIGHT: if (L[e.cx]) e.cx = ed_next(L, e.cx); else if (e.cy + 1 < e.n) { e.cy++; e.cx = 0; } break;
            case KEY_HOME: case 1: e.cx = 0; break;
            case KEY_END: case 5: e.cx = (int)strlen(L); break;
            case KEY_PGUP:  e.cy = e.cy > page ? e.cy - page : 0; e.cx = 0; break;
            case KEY_PGDN:  e.cy = e.cy + page < e.n ? e.cy + page : e.n - 1; e.cx = 0; break;
            case '\r': case '\n': {   // split the line
                const size_t tail = strlen(L + e.cx);
                ed_insert_line(e, e.cy + 1, L + e.cx, tail);
                L[e.cx] = '\0';
                e.cy++;
                e.cx = 0;
                e.dirty = true;
                break;
            }
            case 0x7f: case 8:   // Backspace
                if (e.cx > 0) {
                    const int p = ed_prev(L, e.cx);
                    memmove(L + p, L + e.cx, strlen(L + e.cx) + 1);
                    e.cx = p;
                    e.dirty = true;
                } else if (e.cy > 0) {
                    const size_t a = strlen(e.ln[e.cy - 1]), bl = strlen(L);
                    char *j = (char *)ps_alloc(a + bl + 1);
                    if (!j) break;
                    memcpy(j, e.ln[e.cy - 1], a);
                    memcpy(j + a, L, bl + 1);
                    heap_caps_free(e.ln[e.cy - 1]);
                    e.ln[e.cy - 1] = j;
                    ed_delete_line(e, e.cy);
                    e.cy--;
                    e.cx = (int)a;
                    e.dirty = true;
                }
                break;
            case KEY_DEL: case 4:
                if (L[e.cx]) {
                    const int nx = ed_next(L, e.cx);
                    memmove(L + e.cx, L + nx, strlen(L + nx) + 1);
                    e.dirty = true;
                } else if (e.cy + 1 < e.n) {
                    const size_t a = strlen(L), bl = strlen(e.ln[e.cy + 1]);
                    char *j = (char *)ps_alloc(a + bl + 1);
                    if (!j) break;
                    memcpy(j, L, a);
                    memcpy(j + a, e.ln[e.cy + 1], bl + 1);
                    heap_caps_free(e.ln[e.cy]);
                    e.ln[e.cy] = j;
                    ed_delete_line(e, e.cy + 1);
                    e.dirty = true;
                }
                break;
            case 19: case 15: {   // ^S / ^O: save
                if (!e.path[0]) {
                    char name[kPath] = "";
                    if (!tui_prompt(c, rows - 1, "File Name to Write:", name, sizeof name) || !name[0]) {
                        snprintf(e.msg, sizeof e.msg, "[ Cancelled ]");
                        break;
                    }
                    resolve(name, e.path, sizeof e.path);
                    snprintf(e.shown, sizeof e.shown, "%s", name);
                }
                VolsHold hold;
                ed_save(c, e);
                break;
            }
            case 24: case 17: case 3: {   // ^X / ^Q / ^C: exit
                if (!e.dirty) { quit = true; break; }
                char ans[8] = "";
                if (!tui_prompt(c, rows - 1, "Save modified buffer? (Y/N)", ans, sizeof ans)) {
                    snprintf(e.msg, sizeof e.msg, "[ Cancelled ]");
                    break;
                }
                if (ans[0] == 'n' || ans[0] == 'N') { quit = true; break; }
                if (ans[0] != 'y' && ans[0] != 'Y') break;
                if (!e.path[0]) {
                    char name[kPath] = "";
                    if (!tui_prompt(c, rows - 1, "File Name to Write:", name, sizeof name) || !name[0]) break;
                    resolve(name, e.path, sizeof e.path);
                    snprintf(e.shown, sizeof e.shown, "%s", name);
                }
                VolsHold hold;
                quit = ed_save(c, e);
                break;
            }
            case 11: {   // ^K: cut the line (consecutive cuts collect)
                const size_t l = strlen(L);
                const size_t have = (e.cut_chain && e.cut) ? strlen(e.cut) : 0;
                char *nc = (char *)ps_alloc(have + l + 2);
                if (!nc) break;
                if (have) memcpy(nc, e.cut, have);
                memcpy(nc + have, L, l);
                nc[have + l] = '\n';
                nc[have + l + 1] = '\0';
                heap_caps_free(e.cut);
                e.cut = nc;
                e.cut_chain = true;
                if (e.n > 1) ed_delete_line(e, e.cy);
                else { e.ln[0][0] = '\0'; }
                if (e.cy >= e.n) e.cy = e.n - 1;
                e.cx = 0;
                e.dirty = true;
                break;
            }
            case 21: {   // ^U: paste the cut lines above the cursor line
                if (!e.cut) break;
                int at = e.cy;
                each_line(e.cut, strlen(e.cut), [&](const char *s, size_t len) {
                    return ed_insert_line(e, at++, s, len);
                });
                e.cy = at;
                if (e.cy >= e.n) e.cy = e.n - 1;
                e.cx = 0;
                e.dirty = true;
                break;
            }
            case 23: {   // ^W: search forward (wraps)
                char q[128] = "";
                if (!tui_prompt(c, rows - 1, "Search:", q, sizeof q) || !q[0]) break;
                bool found = false;
                for (int step = 0; step <= e.n && !found; step++) {
                    const int i = (e.cy + step) % e.n;
                    const char *from = e.ln[i] + (step == 0 ? ed_next(e.ln[i], e.cx) : 0);
                    const char *hit = strstr(from, q);
                    if (hit) { e.cy = i; e.cx = (int)(hit - e.ln[i]); found = true; }
                }
                if (!found) snprintf(e.msg, sizeof e.msg, "[ \"%s\" not found ]", q);
                break;
            }
            case 7: {   // ^G: go to line
                char num[16] = "";
                if (tui_prompt(c, rows - 1, "Enter line number:", num, sizeof num) && num[0]) {
                    int l = atoi(num);
                    l = l < 1 ? 1 : l > e.n ? e.n : l;
                    e.cy = l - 1;
                    e.cx = 0;
                }
                break;
            }
            case '\t':
            default:
                if (k == '\t' || (k >= 0x20 && k < 0x100 && k != 0x7f)) {   // insert (UTF-8 bytes one by one)
                    const size_t l = strlen(L);
                    char *nl = (char *)ps_alloc(l + 2);
                    if (!nl) break;
                    memcpy(nl, L, (size_t)e.cx);
                    nl[e.cx] = (char)k;
                    memcpy(nl + e.cx + 1, L + e.cx, l - (size_t)e.cx + 1);
                    heap_caps_free(e.ln[e.cy]);
                    e.ln[e.cy] = nl;
                    e.cx++;
                    e.dirty = true;
                }
                break;
        }
    }
    tui_end(c);
    ed_free(e);
    delete ep;
    return 0;
}

// stty size / stty: the terminal geometry.
int b_stty(Ctx &c) {
    if (c.argc > 1 && !strcmp(c.argv[1], "size")) { outf(c, "%d %d\n", term_tty_rows(), term_tty_cols()); return 0; }
    outf(c, "speed 115200 baud; rows %d; columns %d; line = 0;\n", term_tty_rows(), term_tty_cols());
    return 0;
}

// reset: a full terminal reset (RIS), like reset(1).
int b_reset(Ctx &c) { wr(c.out, "\x1b" "c"); return 0; }

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
    if (!batch) tui_begin(c);
    bool quit = false;
    if (batch) vTaskDelay(pdMS_TO_TICKS(1000));
    else for (int64_t end = esp_timer_get_time() + 1000000; esp_timer_get_time() < end && !quit;) {
        const int k = read_key(100);
        quit = k == 'q' || k == 'Q' || k == 3 || k == KEY_GONE;
    }
    static const char kState[] = "RRBSDI";
    for (int it = 0; !quit && (iters < 0 || it < iters); it++) {
        if (cancelled()) break;
        nv_sys_perf_t perf;
        nv_sys_mem_t mem;
        nv_sysmon_perf(&perf);
        nv_sysmon_mem(&mem);
        const int n = nv_sysmon_tasks(rows, kRows);
        if (!batch) wr(c.out, "\x1b[?25l\x1b[H");
        char now[16];
        nv_time_format(now, sizeof now, "%H:%M:%S");
        const unsigned up = (unsigned)(perf.uptime_s / 60);
        outf(c, "top - %s up %u:%02u,  %u tasks,  cpu0 %.1f%%  cpu1 %.1f%%  %u MHz", now, up / 60, up % 60,
             (unsigned)perf.task_count, (double)perf.core_load[0], (double)perf.core_load[1],
             (unsigned)perf.freq_mhz);
        if (perf.temp_valid) outf(c, "  %.1f\xC2\xB0""C", (double)perf.temp_c);
        wr(c.out, "\x1b[K\n", 4);
        char a[16], b[16], d[16];
        human(mem.internal.total, a, sizeof a); human(mem.internal.used, b, sizeof b); human(mem.internal.largest, d, sizeof d);
        outf(c, "SRAM:  %6s total  %6s used  %6s largest  (min free %u K)\x1b[K\n", a, b, d, (unsigned)(mem.internal.min_free / 1024));
        human(mem.psram.total, a, sizeof a); human(mem.psram.used, b, sizeof b); human(mem.psram.largest, d, sizeof d);
        outf(c, "PSRAM: %6s total  %6s used  %6s largest\x1b[K\n\x1b[K\n", a, b, d);
        outf(c, "\x1b[7m  %-16s %s %4s %4s %9s %6s  \x1b[K\x1b[0m\n", "TASK", "S", "PRI", "CPU", "STACK", "%CPU");
        const int show = batch ? n : (n < term_tty_rows() - 6 ? n : term_tty_rows() - 6);
        for (int k = 0; k < show; k++) {
            const nv_task_row_t &r = rows[k];
            char core[4];
            if (r.core < 0) snprintf(core, sizeof core, "-");
            else snprintf(core, sizeof core, "%d", r.core);
            outf(c, "  %-16.16s %c %4u %4s %9u %6.1f\x1b[K\n", r.name, kState[r.state < 6 ? r.state : 5],
                 (unsigned)r.prio, core, (unsigned)r.stack_free, (double)r.cpu_pct);
        }
        if (!batch) wr(c.out, "\x1b[J\x1b[7mq\x1b[0m quit");
        if (iters >= 0 && it + 1 >= iters) break;
        const int64_t end = esp_timer_get_time() + (int64_t)(delay * 1e6);
        while (esp_timer_get_time() < end && !cancelled() && !quit) {
            if (batch) { vTaskDelay(pdMS_TO_TICKS(50)); continue; }
            const int k = read_key(100);
            quit = k == 'q' || k == 'Q' || k == 3 || k == KEY_GONE;
        }
    }
    if (!batch) tui_end(c);
    heap_caps_free(rows);
    return cancelled() && !quit ? 130 : 0;
}

// ---------------------------------------------------------------- built-ins: text tools

// -abc flags where the letters in `valued` take a value (the rest of the cluster or the next
// argument) into vals[letter - 'A'] (an array of 64). Returns the first operand index, -1 on a
// bad option. A negative number ("-5") is an operand.
int getopts(Ctx &c, const char *allowed, const char *valued, Flags &f, const char **vals) {
    int i = 1;
    for (; i < c.argc; i++) {
        const char *a = c.argv[i];
        if (a[0] != '-' || !a[1]) break;
        if (!strcmp(a, "--")) { i++; break; }
        if (isdigit((unsigned char)a[1])) break;
        for (const char *p = a + 1; *p; p++) {
            if (*p < 'A' || *p > 'z' || !strchr(allowed, *p)) {
                errf(c, "%s: invalid option -- '%c'\nTry 'help %s' for more information.\n",
                     c.argv[0], *p, c.argv[0]);
                return -1;
            }
            f.m |= 1ull << (*p - 'A');
            if (strchr(valued, *p)) {
                const char *v = p[1] ? p + 1 : (i + 1 < c.argc ? c.argv[++i] : nullptr);
                if (!v) { errf(c, "%s: option requires an argument -- '%c'\n", c.argv[0], *p); return -1; }
                vals[*p - 'A'] = v;
                break;
            }
        }
    }
    return i;
}
const char *optv(const char **vals, char letter) { return vals[letter - 'A']; }

// fn(bytes, len) for each operand file, or stdin when there is none ("-" is stdin too).
// Returns 1 if an input could not be read.
template <typename F> int each_input(Ctx &c, int i, F fn) {
    static const char *kDash[] = {"-"};
    char **ops = c.argv + i;
    int nops = c.argc - i;
    if (nops <= 0) { ops = (char **)kDash; nops = 1; }
    int st = 0;
    for (int k = 0; k < nops && !cancelled(); k++) {
        ShBuf b;
        if (!read_all(c, ops[k], b)) { st = 1; continue; }
        fn(b.p ? b.p : "", b.n, ops[k]);
        buf_free(b);
    }
    return st;
}

// The byte a backslash escape stands for; p points after the backslash, *used = characters
// consumed (0 = not an escape).
char esc_byte(const char *p, int *used) {
    *used = 1;
    switch (*p) {
        case 'n': return '\n';
        case 't': return '\t';
        case 'r': return '\r';
        case 'a': return '\a';
        case 'b': return '\b';
        case 'f': return '\f';
        case 'v': return '\v';
        case 'e': return '\x1b';
        case '\\': return '\\';
        case '0': {
            int v = 0, k = 1;
            while (k < 4 && p[k] >= '0' && p[k] <= '7') v = v * 8 + (p[k++] - '0');
            *used = k;
            return (char)v;
        }
        case 'x': {
            int v = 0, k = 1;
            while (k < 3 && isxdigit((unsigned char)p[k])) {
                const int d = (unsigned char)p[k];
                v = v * 16 + (isdigit(d) ? d - '0' : tolower(d) - 'a' + 10);
                k++;
            }
            if (k == 1) { *used = 0; return 0; }
            *used = k;
            return (char)v;
        }
        default:
            *used = 0;
            return 0;
    }
}

// One pass of printf's FORMAT over args[*ai..]: bash's conversions %s %b %c %d %i %u %o %x %X
// %e %f %g with flags, width and precision. Returns 1 when an argument was not a number.
int printf_core(Ctx &c, const char *fmt, char **args, int nargs, int *ai) {
    int st = 0;
    for (const char *p = fmt; *p; p++) {
        if (*p == '\\' && p[1]) {
            int used;
            const char b = esc_byte(p + 1, &used);
            if (used) { wr(c.out, &b, 1); p += used; }
            else { wr(c.out, p, 2); p++; }
            continue;
        }
        if (*p != '%') {
            const char *q = p;
            while (q[1] && q[1] != '%' && q[1] != '\\') q++;
            wr(c.out, p, (size_t)(q - p + 1));
            p = q;
            continue;
        }
        if (p[1] == '%') { wr(c.out, "%", 1); p++; continue; }
        char spec[40];
        size_t sn = 0;
        spec[sn++] = '%';
        const char *q = p + 1;
        while (*q && strchr("-+ #0", *q) && sn < 8) spec[sn++] = *q++;
        while (*q && (isdigit((unsigned char)*q) || *q == '.') && sn < 24) spec[sn++] = *q++;
        const char conv = *q;
        if (!conv) { wr(c.out, p); break; }
        p = q;
        const char *arg = *ai < nargs ? args[(*ai)++] : nullptr;
        char out[512];
        int n = 0;
        char *e = nullptr;
        switch (conv) {
            case 's':
                spec[sn++] = 's'; spec[sn] = '\0';
                n = snprintf(out, sizeof out, spec, arg ? arg : "");
                break;
            case 'b':
                for (const char *s = arg ? arg : ""; *s; s++) {
                    if (*s == '\\' && s[1]) {
                        int used;
                        const char b = esc_byte(s + 1, &used);
                        if (used) { wr(c.out, &b, 1); s += used; continue; }
                    }
                    wr(c.out, s, 1);
                }
                break;
            case 'c':
                if (arg && arg[0]) wr(c.out, arg, 1);
                break;
            case 'd': case 'i': {
                const long long v = arg && arg[0] ? strtoll(arg, &e, 0) : 0;
                if (arg && arg[0] && *e) { errf(c, "printf: '%s': invalid number\n", arg); st = 1; }
                spec[sn++] = 'l'; spec[sn++] = 'l'; spec[sn++] = 'd'; spec[sn] = '\0';
                n = snprintf(out, sizeof out, spec, v);
                break;
            }
            case 'u': case 'o': case 'x': case 'X': {
                const unsigned long long v = arg && arg[0] ? strtoull(arg, &e, 0) : 0;
                if (arg && arg[0] && *e) { errf(c, "printf: '%s': invalid number\n", arg); st = 1; }
                spec[sn++] = 'l'; spec[sn++] = 'l'; spec[sn++] = conv; spec[sn] = '\0';
                n = snprintf(out, sizeof out, spec, v);
                break;
            }
            case 'e': case 'E': case 'f': case 'F': case 'g': case 'G': {
                const double v = arg && arg[0] ? strtod(arg, &e) : 0.0;
                if (arg && arg[0] && *e) { errf(c, "printf: '%s': invalid number\n", arg); st = 1; }
                spec[sn++] = conv; spec[sn] = '\0';
                n = snprintf(out, sizeof out, spec, v);
                break;
            }
            default:
                errf(c, "printf: %%%c: invalid conversion specification\n", conv);
                return 1;
        }
        if (n > 0) wr(c.out, out, (size_t)n < sizeof out ? (size_t)n : sizeof out - 1);
    }
    return st;
}

int b_printf(Ctx &c) {
    if (c.argc < 2) { errf(c, "printf: usage: printf FORMAT [ARGUMENTS...]\n"); return 2; }
    int ai = 0, st = 0;
    char **args = c.argv + 2;
    const int nargs = c.argc - 2;
    do {   // the format is reused while arguments remain, as in bash
        const int before = ai;
        st |= printf_core(c, c.argv[1], args, nargs, &ai);
        if (ai == before) break;
    } while (ai < nargs);
    return st;
}

int decimals(const char *s) {
    const char *d = strchr(s, '.');
    if (!d) return 0;
    int n = 0;
    for (d++; isdigit((unsigned char)*d); d++) n++;
    return n;
}

int b_seq(Ctx &c) {
    const char *sep = "\n";
    bool w = false;
    int i = 1;
    for (; i < c.argc; i++) {
        const char *a = c.argv[i];
        if (!strcmp(a, "-s") && i + 1 < c.argc) sep = c.argv[++i];
        else if (!strncmp(a, "-s", 2) && a[2]) sep = a + 2;
        else if (!strcmp(a, "-w")) w = true;
        else if (!strcmp(a, "--")) { i++; break; }
        else break;
    }
    const int n = c.argc - i;
    if (n < 1) { errf(c, "seq: missing operand\n"); return 1; }
    if (n > 3) { errf(c, "seq: extra operand '%s'\n", c.argv[i + 3]); return 1; }
    double v[3];
    for (int k = 0; k < n; k++) {
        char *e = nullptr;
        v[k] = strtod(c.argv[i + k], &e);
        if (e == c.argv[i + k] || *e) { errf(c, "seq: invalid floating point argument: '%s'\n", c.argv[i + k]); return 1; }
    }
    const double first = n > 1 ? v[0] : 1.0, inc = n == 3 ? v[1] : 1.0, last = v[n - 1];
    int dec = n > 1 ? decimals(c.argv[i]) : 0;
    if (n == 3 && decimals(c.argv[i + 1]) > dec) dec = decimals(c.argv[i + 1]);
    if (inc == 0) { errf(c, "seq: invalid Zero increment value: '%s'\n", c.argv[i + 1]); return 1; }
    int width = 0;
    if (w) {
        char b[64];
        width = snprintf(b, sizeof b, "%.*f", dec, first);
        const int w2 = snprintf(b, sizeof b, "%.*f", dec, last);
        if (w2 > width) width = w2;
    }
    bool any = false;
    for (long k = 0; !cancelled(); k++) {
        const double x = first + (double)k * inc;
        if (inc > 0 ? x > last + 1e-9 : x < last - 1e-9) break;
        if (any) wr(c.out, sep);
        char b[64];
        const int m = w ? snprintf(b, sizeof b, "%0*.*f", width, dec, x) : snprintf(b, sizeof b, "%.*f", dec, x);
        wr(c.out, b, (size_t)(m < (int)sizeof b ? m : (int)sizeof b - 1));   // snprintf returns the untruncated length
        any = true;
    }
    if (any) wr(c.out, "\n", 1);
    return cancelled() ? 130 : 0;
}

int b_tee(Ctx &c) {
    Flags f;
    int i = getflags(c, "a", f);
    if (i < 0) return 1;
    int st = 0;
    for (; i < c.argc; i++) {
        if (!strcmp(c.argv[i], "/dev/null")) continue;
        char p[kPath];
        resolve(c.argv[i], p, sizeof p);
        FILE *fp = fopen(p, f.has('a') ? "ab" : "wb");
        if (!fp) {
            errf(c, "tee: %s: %s\n", c.argv[i], is_dir(p) ? "Is a directory" : "No such file or directory");
            st = 1;
            continue;
        }
        if (c.has_in && c.in_len && fwrite(c.in, 1, c.in_len, fp) != c.in_len) {
            errf(c, "tee: %s: No space left on device\n", c.argv[i]);
            st = 1;
        }
        fclose(fp);
    }
    if (c.has_in) wr(c.out, c.in, c.in_len);
    return st;
}

// cut LIST: "1,3-5,7-" -> ranges.
struct CutRange { long a, b; };
int cut_list(const char *s, CutRange *r, int max) {
    int n = 0;
    while (*s && n < max) {
        char *e;
        long a = 1, b = LONG_MAX;
        if (*s == '-') {
            s++;
            b = strtol(s, &e, 10);
            if (e == s) return -1;
            s = e;
        } else {
            a = strtol(s, &e, 10);
            if (e == s || a < 1) return -1;
            s = e;
            b = a;
            if (*s == '-') {
                s++;
                if (isdigit((unsigned char)*s)) { b = strtol(s, &e, 10); s = e; }
                else b = LONG_MAX;
            }
        }
        r[n++] = {a, b};
        if (*s == ',') s++;
        else if (*s) return -1;
    }
    return n;
}
bool cut_has(const CutRange *r, int n, long k) {
    for (int i = 0; i < n; i++) if (k >= r[i].a && k <= r[i].b) return true;
    return false;
}

int b_cut(Ctx &c) {
    Flags f;
    const char *vals[64] = {};
    const int i = getopts(c, "bcdfs", "bcdf", f, vals);
    if (i < 0) return 1;
    const bool fields = optv(vals, 'f') != nullptr;
    const char *list = fields ? optv(vals, 'f') : optv(vals, 'c') ? optv(vals, 'c') : optv(vals, 'b');
    if (!list) { errf(c, "cut: you must specify a list of bytes, characters, or fields\n"); return 1; }
    CutRange r[32];
    const int nr = cut_list(list, r, 32);
    if (nr <= 0) { errf(c, "cut: invalid byte, character or field list\n"); return 1; }
    char delim = '\t';
    if (const char *d = optv(vals, 'd')) {
        if (strlen(d) != 1) { errf(c, "cut: the delimiter must be a single character\n"); return 1; }
        delim = d[0];
    }
    ShBuf ob;
    const int st = each_input(c, i, [&](const char *p, size_t n, const char *) {
        each_line(p, n, [&](const char *s, size_t len) {
            ob.n = 0;
            if (!fields) {
                for (size_t k = 0; k < len; k++) if (cut_has(r, nr, (long)k + 1)) buf_put(ob, s + k, 1);
            } else if (!memchr(s, delim, len)) {
                if (f.has('s')) return true;
                buf_put(ob, s, len);
            } else {
                long fld = 1;
                size_t start = 0;
                bool first = true;
                for (size_t k = 0; k <= len; k++) {
                    if (k < len && s[k] != delim) continue;
                    if (cut_has(r, nr, fld)) {
                        if (!first) buf_put(ob, &delim, 1);
                        buf_put(ob, s + start, k - start);
                        first = false;
                    }
                    fld++;
                    start = k + 1;
                }
            }
            buf_put(ob, "\n", 1);
            wr(c.out, ob.p, ob.n);
            return true;
        });
    });
    buf_free(ob);
    return st;
}

// A tr SET: ranges (a-z), escapes (\n \t \\ \NNN) and classes ([:lower:] [:digit:] ...).
int tr_set(const char *s, unsigned char *out, int cap) {
    int n = 0;
    auto one = [&](const char *&p) -> unsigned char {
        if (*p == '\\' && p[1]) {
            int used;
            const char b = esc_byte(p + 1, &used);
            if (used) { p += 1 + used; return (unsigned char)b; }
            p += 2;
            return (unsigned char)p[-1];
        }
        return (unsigned char)*p++;
    };
    while (*s && n < cap) {
        if (s[0] == '[' && s[1] == ':') {
            if (const char *e = strstr(s + 2, ":]")) {
                char cls[12];
                snprintf(cls, sizeof cls, "%.*s", (int)(e - s - 2), s + 2);
                for (int x = 0; x < 256 && n < cap; x++) {
                    bool in = false;
                    if (!strcmp(cls, "lower")) in = islower(x);
                    else if (!strcmp(cls, "upper")) in = isupper(x);
                    else if (!strcmp(cls, "digit")) in = isdigit(x);
                    else if (!strcmp(cls, "space")) in = isspace(x);
                    else if (!strcmp(cls, "blank")) in = x == ' ' || x == '\t';
                    else if (!strcmp(cls, "alpha")) in = isalpha(x);
                    else if (!strcmp(cls, "alnum")) in = isalnum(x);
                    else if (!strcmp(cls, "punct")) in = ispunct(x);
                    else if (!strcmp(cls, "xdigit")) in = isxdigit(x);
                    else if (!strcmp(cls, "cntrl")) in = iscntrl(x);
                    if (in && x < 128) out[n++] = (unsigned char)x;
                }
                s = e + 2;
                continue;
            }
        }
        const unsigned char a = one(s);
        if (*s == '-' && s[1]) {
            s++;
            const unsigned char b = one(s);
            for (unsigned x = a; x <= b && n < cap; x++) out[n++] = (unsigned char)x;
        } else {
            out[n++] = a;
        }
    }
    return n;
}

int b_tr(Ctx &c) {
    Flags f;
    int i = getflags(c, "dscC", f);
    if (i < 0) return 1;
    const bool del = f.has('d'), sq = f.has('s'), comp = f.has('c') || f.has('C');
    const int nops = c.argc - i;
    if (nops < 1 || (!del && !sq && nops < 2)) {
        errf(c, "tr: missing operand\nTry 'help tr' for more information.\n");
        return 1;
    }
    unsigned char s1[256], s2[256];
    const int n1 = tr_set(c.argv[i], s1, 256);
    const int n2 = nops > 1 ? tr_set(c.argv[i + 1], s2, 256) : 0;
    bool in1[256] = {};
    for (int k = 0; k < n1; k++) in1[s1[k]] = true;
    if (comp) for (bool &b : in1) b = !b;
    unsigned char map[256];
    for (int x = 0; x < 256; x++) map[x] = (unsigned char)x;
    if (!del && nops > 1 && n2) {
        if (comp) { for (int x = 0; x < 256; x++) if (in1[x]) map[x] = s2[n2 - 1]; }
        else for (int k = 0; k < n1; k++) map[s1[k]] = s2[k < n2 ? k : n2 - 1];
    }
    bool sqs[256] = {};   // squeezed: SET2 when there is one, else SET1
    if (sq) {
        if (nops > 1) { for (int k = 0; k < n2; k++) sqs[s2[k]] = true; }
        else for (int x = 0; x < 256; x++) sqs[x] = in1[x];
    }
    const size_t n = c.has_in ? c.in_len : 0;
    char *o = (char *)ps_alloc(n + 1);
    if (!o) { errf(c, "tr: out of memory\n"); return 1; }
    size_t on = 0;
    int last = -1;
    for (size_t k = 0; k < n; k++) {
        unsigned ch = (unsigned char)c.in[k];
        if (del && in1[ch]) continue;
        ch = map[ch];
        if (sq && sqs[ch] && (int)ch == last) continue;
        last = (int)ch;
        o[on++] = (char)ch;
    }
    wr(c.out, o, on);
    heap_caps_free(o);
    return 0;
}

int b_rev(Ctx &c) {
    ShBuf ob;
    const int st = each_input(c, 1, [&](const char *p, size_t n, const char *) {
        each_line(p, n, [&](const char *s, size_t len) {
            ob.n = 0;
            size_t k = len;
            while (k > 0) {   // whole UTF-8 characters, last first
                size_t b = k - 1;
                while (b > 0 && ((unsigned char)s[b] & 0xC0) == 0x80) b--;
                buf_put(ob, s + b, k - b);
                k = b;
            }
            buf_put(ob, "\n", 1);
            wr(c.out, ob.p, ob.n);
            return true;
        });
    });
    buf_free(ob);
    return st;
}

int b_tac(Ctx &c) {
    ShBuf all;
    LineRef *l = nullptr;
    size_t n = 0;
    const int st = collect_lines(c, 1, all, &l, &n);
    for (size_t k = n; k > 0 && !cancelled(); k--) {
        wr(c.out, l[k - 1].p, l[k - 1].n);
        wr(c.out, "\n", 1);
    }
    heap_caps_free(l);
    buf_free(all);
    return st;
}

int b_nl(Ctx &c) {
    Flags f;
    const char *vals[64] = {};
    const int i = getopts(c, "b", "b", f, vals);
    if (i < 0) return 1;
    const bool all = optv(vals, 'b') && optv(vals, 'b')[0] == 'a';
    unsigned num = 1;
    return each_input(c, i, [&](const char *p, size_t n, const char *) {
        each_line(p, n, [&](const char *s, size_t len) {
            if (len || all) outf(c, "%6u\t", num++);
            wr(c.out, s, len);
            wr(c.out, "\n", 1);
            return true;
        });
    });
}

// ---------------------------------------------------------------- test / expr

bool is_int(const char *s, long long *v) {
    if (!*s) return false;
    char *e;
    *v = strtoll(s, &e, 10);
    while (*e == ' ') e++;
    return !*e;
}

// test EXPR / [ EXPR ]: -e -f -d -s -r -w -x -z -n, = != < >, -eq -ne -lt -le -gt -ge, ! -a -o ( ).
struct TestP {
    Ctx  *c;
    char **a;
    int   n, i;
    bool  err;
};
bool test_or(TestP &t);

bool test_primary(TestP &t) {
    if (t.i >= t.n) { t.err = true; return false; }
    const char *x = t.a[t.i];
    if (!strcmp(x, "!")) { t.i++; return !test_primary(t); }
    if (!strcmp(x, "(") ) {
        t.i++;
        const bool r = test_or(t);
        if (t.i >= t.n || strcmp(t.a[t.i], ")")) { errf(*t.c, "%s: ')' expected\n", t.c->argv[0]); t.err = true; return false; }
        t.i++;
        return r;
    }
    // binary: ARG OP ARG
    if (t.i + 2 < t.n) {
        const char *op = t.a[t.i + 1];
        static const char *kBin[] = {"=", "==", "!=", "<", ">", "-eq", "-ne", "-lt", "-le", "-gt", "-ge", "-nt", "-ot"};
        bool bin = false;
        for (const char *b : kBin) bin = bin || !strcmp(op, b);
        if (bin) {
            const char *l = x, *r = t.a[t.i + 2];
            t.i += 3;
            if (!strcmp(op, "=") || !strcmp(op, "==")) return !strcmp(l, r);
            if (!strcmp(op, "!=")) return strcmp(l, r) != 0;
            if (!strcmp(op, "<")) return strcmp(l, r) < 0;
            if (!strcmp(op, ">")) return strcmp(l, r) > 0;
            if (!strcmp(op, "-nt") || !strcmp(op, "-ot")) {
                char p1[kPath], p2[kPath];
                resolve(l, p1, sizeof p1);
                resolve(r, p2, sizeof p2);
                struct stat s1 = {}, s2 = {};
                const bool e1 = stat(p1, &s1) == 0, e2 = stat(p2, &s2) == 0;
                if (op[1] == 'n') return e1 && (!e2 || s1.st_mtime > s2.st_mtime);
                return e2 && (!e1 || s1.st_mtime < s2.st_mtime);
            }
            long long u = 0, v = 0;
            const bool lu = is_int(l, &u), rv = is_int(r, &v);
            if (!lu || !rv) {
                errf(*t.c, "%s: %s: integer expression expected\n", t.c->argv[0], lu ? r : l);
                t.err = true;
                return false;
            }
            if (!strcmp(op, "-eq")) return u == v;
            if (!strcmp(op, "-ne")) return u != v;
            if (!strcmp(op, "-lt")) return u < v;
            if (!strcmp(op, "-le")) return u <= v;
            if (!strcmp(op, "-gt")) return u > v;
            return u >= v;
        }
    }
    // unary: -X ARG
    if (x[0] == '-' && x[1] && !x[2] && strchr("efdsrwxznLh", x[1]) && t.i + 1 < t.n) {
        const char *arg = t.a[t.i + 1];
        t.i += 2;
        if (x[1] == 'z') return !arg[0];
        if (x[1] == 'n') return arg[0] != 0;
        char p[kPath];
        resolve(arg, p, sizeof p);
        if (!arg[0]) return false;
        const bool dir = is_dir(p);
        struct stat s = {};
        const bool ex = dir || stat(p, &s) == 0;
        switch (x[1]) {
            case 'e': case 'r': case 'w': return ex;
            case 'f': return ex && !dir;
            case 'd': return dir;
            case 's': return ex && (dir || s.st_size > 0);
            case 'x': return dir;
            default:  return false;   // -L -h: no symbolic links on FAT
        }
    }
    t.i++;
    return x[0] != 0;   // a lone string: true when not empty
}

bool test_and(TestP &t) {
    bool r = test_primary(t);
    while (!t.err && t.i < t.n && !strcmp(t.a[t.i], "-a")) {
        t.i++;
        const bool q = test_primary(t);
        r = r && q;
    }
    return r;
}

bool test_or(TestP &t) {
    bool r = test_and(t);
    while (!t.err && t.i < t.n && !strcmp(t.a[t.i], "-o")) {
        t.i++;
        const bool q = test_and(t);
        r = r || q;
    }
    return r;
}

int b_test(Ctx &c) {
    int n = c.argc - 1;
    if (!strcmp(c.argv[0], "[")) {
        if (c.argc < 2 || strcmp(c.argv[c.argc - 1], "]")) { errf(c, "[: missing ']'\n"); return 2; }
        n--;
    }
    if (n <= 0) return 1;
    TestP t{&c, c.argv + 1, n, 0, false};
    const bool r = test_or(t);
    if (!t.err && t.i < t.n) { errf(c, "%s: %s: unexpected argument\n", c.argv[0], t.a[t.i]); return 2; }
    if (t.err) return 2;
    return r ? 0 : 1;
}

// expr: | & = != < <= > >= + - * / % : and "length S", over integers and strings.
struct XVal {
    char s[96];
    long long n;
    bool num;
};
struct ExprP {
    Ctx  *c;
    char **a;
    int   n, i;
    bool  err;
};
XVal xv_str(const char *s) {
    XVal v;
    snprintf(v.s, sizeof v.s, "%s", s);
    v.num = is_int(s, &v.n);
    return v;
}
XVal xv_int(long long n) {
    XVal v;
    snprintf(v.s, sizeof v.s, "%lld", n);
    v.n = n;
    v.num = true;
    return v;
}
bool xv_null(const XVal &v) { return !v.s[0] || (v.num && v.n == 0); }
XVal expr_or(ExprP &p);

bool expr_is(ExprP &p, const char *op) { return p.i < p.n && !strcmp(p.a[p.i], op); }

XVal expr_atom(ExprP &p) {
    if (p.i >= p.n) { errf(*p.c, "expr: syntax error: missing argument\n"); p.err = true; return xv_str(""); }
    if (expr_is(p, "(")) {
        p.i++;
        XVal v = expr_or(p);
        if (!expr_is(p, ")")) { errf(*p.c, "expr: syntax error: expecting ')'\n"); p.err = true; return v; }
        p.i++;
        return v;
    }
    if (expr_is(p, "length") && p.i + 1 < p.n) {
        p.i++;
        const XVal v = expr_atom(p);
        return xv_int((long long)strlen(v.s));
    }
    return xv_str(p.a[p.i++]);
}

XVal expr_match(ExprP &p) {
    XVal v = expr_atom(p);
    while (!p.err && expr_is(p, ":")) {
        p.i++;
        const XVal r = expr_atom(p);
        Re *re = (Re *)ps_alloc(sizeof(Re));
        const char *err = nullptr;
        long long len = 0;
        char pat[100];
        snprintf(pat, sizeof pat, "^%s", r.s[0] == '^' ? r.s + 1 : r.s);
        if (re && re_compile(*re, pat, false, false, &err)) {
            const char *ms, *me;
            if (re_search(*re, v.s, v.s + strlen(v.s), &ms, &me)) len = me - ms;
        } else {
            errf(*p.c, "expr: %s\n", err ? err : "out of memory");
            p.err = true;
        }
        heap_caps_free(re);
        v = xv_int(len);
    }
    return v;
}

XVal expr_mul(ExprP &p) {
    XVal v = expr_match(p);
    while (!p.err && (expr_is(p, "*") || expr_is(p, "/") || expr_is(p, "%"))) {
        const char op = p.a[p.i++][0];
        const XVal r = expr_match(p);
        if (!v.num || !r.num) { errf(*p.c, "expr: non-integer argument\n"); p.err = true; break; }
        if (op != '*' && r.n == 0) { errf(*p.c, "expr: division by zero\n"); p.err = true; break; }
        v = xv_int(op == '*' ? v.n * r.n : op == '/' ? v.n / r.n : v.n % r.n);
    }
    return v;
}

XVal expr_add(ExprP &p) {
    XVal v = expr_mul(p);
    while (!p.err && (expr_is(p, "+") || expr_is(p, "-"))) {
        const char op = p.a[p.i++][0];
        const XVal r = expr_mul(p);
        if (!v.num || !r.num) { errf(*p.c, "expr: non-integer argument\n"); p.err = true; break; }
        v = xv_int(op == '+' ? v.n + r.n : v.n - r.n);
    }
    return v;
}

XVal expr_cmp(ExprP &p) {
    XVal v = expr_add(p);
    static const char *kOps[] = {"=", "==", "!=", "<", "<=", ">", ">="};
    for (;;) {
        const char *op = nullptr;
        for (const char *o : kOps) if (expr_is(p, o)) op = o;
        if (!op || p.err) break;
        p.i++;
        const XVal r = expr_add(p);
        const int d = (v.num && r.num) ? (v.n < r.n ? -1 : v.n > r.n) : strcmp(v.s, r.s);
        bool t;
        if (op[0] == '=') t = d == 0;
        else if (op[0] == '!') t = d != 0;
        else if (op[0] == '<') t = op[1] ? d <= 0 : d < 0;
        else t = op[1] ? d >= 0 : d > 0;
        v = xv_int(t);
    }
    return v;
}

XVal expr_and(ExprP &p) {
    XVal v = expr_cmp(p);
    while (!p.err && expr_is(p, "&")) {
        p.i++;
        const XVal r = expr_cmp(p);
        if (xv_null(v) || xv_null(r)) v = xv_int(0);
    }
    return v;
}

XVal expr_or(ExprP &p) {
    XVal v = expr_and(p);
    while (!p.err && expr_is(p, "|")) {
        p.i++;
        const XVal r = expr_and(p);
        if (xv_null(v)) v = xv_null(r) ? xv_int(0) : r;
    }
    return v;
}

int b_expr(Ctx &c) {
    if (c.argc < 2) { errf(c, "expr: missing operand\n"); return 2; }
    ExprP p{&c, c.argv + 1, c.argc - 1, 0, false};
    const XVal v = expr_or(p);
    if (!p.err && p.i < p.n) { errf(c, "expr: syntax error: unexpected argument '%s'\n", p.a[p.i]); return 2; }
    if (p.err) return 2;
    outf(c, "%s\n", v.s);
    return xv_null(v) ? 1 : 0;
}

// ---------------------------------------------------------------- sed (a working subset)
// sed [-n] [-i] [-E] [-e SCRIPT]... [SCRIPT] [FILE...]: commands s/RE/REPL/[gpI N], d, p, q, =,
// addresses N, $, /RE/ and ranges A,B, ! negation; ';' or newlines between commands. RE is
// grep's matcher (no groups: & in REPL is the whole match).

struct SedCmd {
    char  ak[2];        // address kinds: 0 none, 'n' line, '$' last line, '/' regex
    long  al[2];
    Re   *ar[2];
    bool  neg, active;
    char  cmd;
    Re   *re;
    char *rep;
    bool  g, p;
    long  nth;
};
constexpr int kSedMax = 24;

// Read up to the unescaped delimiter; "\<delim>" becomes delim, other escapes stay. In place.
char *sed_field(char *&s, char delim) {
    char *start = s, *o = s;
    while (*s && *s != delim) {
        if (*s == '\\' && s[1] == delim) { *o++ = delim; s += 2; continue; }
        if (*s == '\\' && s[1]) { *o++ = *s++; *o++ = *s++; continue; }
        *o++ = *s++;
    }
    if (*s != delim) return nullptr;
    s++;
    *o = '\0';
    return start;
}

Re *sed_re(const char *pat, bool icase, const char **err) {
    Re *re = (Re *)ps_alloc(sizeof(Re));
    if (!re) { *err = "out of memory"; return nullptr; }
    if (!re_compile(*re, pat, icase, false, err)) { heap_caps_free(re); return nullptr; }
    return re;
}

void sed_free(SedCmd *k, int n) {
    for (int i = 0; i < n; i++) {
        heap_caps_free(k[i].ar[0]);
        heap_caps_free(k[i].ar[1]);
        heap_caps_free(k[i].re);
    }
}

// Parse `script` (modified in place) into cmds. Returns the count, -1 with a message.
int sed_parse(Ctx &c, char *s, SedCmd *cmds) {
    int n = 0;
    const char *err = nullptr;
    auto fail = [&](const char *why) {
        errf(c, "sed: -e expression: %s\n", why);
        sed_free(cmds, n);
        return -1;
    };
    for (;;) {
        while (*s == ' ' || *s == '\t' || *s == ';' || *s == '\n') s++;
        if (!*s) break;
        if (n >= kSedMax) return fail("too many commands");
        SedCmd &k = cmds[n];
        k = SedCmd{};
        n++;   // from here on a failure frees this one too
        for (int a = 0; a < 2; a++) {
            if (isdigit((unsigned char)*s)) { k.ak[a] = 'n'; k.al[a] = strtol(s, &s, 10); }
            else if (*s == '$') { k.ak[a] = '$'; s++; }
            else if (*s == '/') {
                s++;
                char *pat = sed_field(s, '/');
                if (!pat) return fail("unterminated address regex");
                k.ak[a] = '/';
                k.ar[a] = sed_re(pat, false, &err);
                if (!k.ar[a]) return fail(err);
            } else break;
            if (a == 0 && *s == ',') { s++; continue; }
            break;
        }
        while (*s == ' ') s++;
        if (*s == '!') { k.neg = true; s++; while (*s == ' ') s++; }
        k.cmd = *s ? *s++ : 0;
        switch (k.cmd) {
            case 'd': case 'p': case 'q': case '=':
                break;
            case 's': {
                const char delim = *s;
                if (!delim || delim == '\n' || delim == '\\') return fail("unterminated `s' command");
                s++;
                char *pat = sed_field(s, delim);
                char *rep = pat ? sed_field(s, delim) : nullptr;
                if (!rep) return fail("unterminated `s' command");
                bool icase = false;
                while (*s && !strchr(" \t;\n}", *s)) {
                    if (*s == 'g') k.g = true;
                    else if (*s == 'p') k.p = true;
                    else if (*s == 'I' || *s == 'i') icase = true;
                    else if (isdigit((unsigned char)*s)) { k.nth = strtol(s, &s, 10); continue; }
                    else return fail("unknown option to `s'");
                    s++;
                }
                if (strstr(pat, "\\(")) return fail("groups \\( \\) and \\1 are not supported");
                k.re = sed_re(pat, icase, &err);
                if (!k.re) return fail(err);
                k.rep = rep;
                break;
            }
            default: {
                char why[48];
                snprintf(why, sizeof why, "unknown command: `%c'", k.cmd ? k.cmd : ' ');
                return fail(why);
            }
        }
    }
    return n;
}

bool sed_addr(const SedCmd &k, int a, long ln, bool last, const char *s, size_t n) {
    const char *ms, *me;
    switch (k.ak[a]) {
        case 'n': return ln == k.al[a];
        case '$': return last;
        case '/': return re_search(*k.ar[a], s, s + n, &ms, &me);
        default:  return true;
    }
}

bool sed_match(SedCmd &k, long ln, bool last, const char *s, size_t n) {
    bool m;
    if (!k.ak[0]) m = true;
    else if (!k.ak[1]) m = sed_addr(k, 0, ln, last, s, n);
    else if (k.active) {
        m = true;
        if (k.ak[1] == 'n' ? ln >= k.al[1] : sed_addr(k, 1, ln, last, s, n)) k.active = false;
    } else if (sed_addr(k, 0, ln, last, s, n)) {
        m = true;
        k.active = !(k.ak[1] == 'n' && k.al[1] <= ln);
    } else {
        m = false;
    }
    return m != k.neg;
}

// s///: pat -> tmp, then swapped. True when something was replaced.
bool sed_subst(const SedCmd &k, ShBuf &pat, ShBuf &tmp) {
    tmp.n = 0;
    const char *s = pat.p ? pat.p : "", *e = s + pat.n, *q = s;
    long count = 0;
    bool did = false;
    while (q <= e && !cancelled()) {
        const char *ms, *me;
        if (!re_search(*k.re, q, e, &ms, &me)) break;
        count++;
        if (!k.nth || count == k.nth) {
            buf_put(tmp, q, (size_t)(ms - q));
            for (const char *r = k.rep; *r; r++) {
                if (*r == '&') buf_put(tmp, ms, (size_t)(me - ms));
                else if (*r == '\\' && r[1]) {
                    r++;
                    const char ch = *r == 'n' ? '\n' : *r == 't' ? '\t' : *r;
                    buf_put(tmp, &ch, 1);
                } else buf_put(tmp, r, 1);
            }
            did = true;
        } else {
            buf_put(tmp, q, (size_t)(me - q));
        }
        if (me == ms) {
            if (ms < e) buf_put(tmp, ms, 1);
            q = me + 1;
        } else {
            q = me;
        }
        if ((did && !k.g) || k.re->bol) break;
    }
    if (!did) return false;
    if (q < e) buf_put(tmp, q, (size_t)(e - q));
    const ShBuf t = pat;
    pat = tmp;
    tmp = t;
    return true;
}

// Run the script over one stream. emit() takes the output. False after q.
template <typename E> bool sed_run(SedCmd *cmds, int nc, bool quiet, const char *p, size_t n, long &ln,
                                   bool final_stream, E emit) {
    ShBuf pat, tmp;
    bool go = true;
    size_t i = 0;
    while (i < n && go && !cancelled()) {
        size_t j = i;
        while (j < n && p[j] != '\n') j++;
        ln++;
        const bool last = final_stream && (j >= n || j + 1 >= n);
        pat.n = 0;
        buf_put(pat, p + i, j - i);
        bool del = false;
        for (int k = 0; k < nc && !del && go; k++) {
            SedCmd &cm = cmds[k];
            if (!sed_match(cm, ln, last, pat.p ? pat.p : "", pat.n)) continue;
            switch (cm.cmd) {
                case 'd': del = true; break;
                case 'p': emit(pat.p ? pat.p : "", pat.n); emit("\n", 1); break;
                case '=': { char b[24]; const int m = snprintf(b, sizeof b, "%ld\n", ln); emit(b, (size_t)m); break; }
                case 'q': go = false; break;
                case 's':
                    if (sed_subst(cm, pat, tmp) && cm.p) { emit(pat.p ? pat.p : "", pat.n); emit("\n", 1); }
                    break;
                default: break;
            }
        }
        if (!del && !quiet) { emit(pat.p ? pat.p : "", pat.n); emit("\n", 1); }
        i = j + 1;
    }
    buf_free(pat);
    buf_free(tmp);
    return go;
}

int b_sed(Ctx &c) {
    bool quiet = false, inplace = false;
    ShBuf script;
    int i = 1;
    for (; i < c.argc; i++) {
        const char *a = c.argv[i];
        if (a[0] != '-' || !a[1]) break;
        if (!strcmp(a, "--")) { i++; break; }
        if (!strcmp(a, "-e") || !strcmp(a, "--expression")) {
            if (i + 1 >= c.argc) { errf(c, "sed: option requires an argument -- 'e'\n"); buf_free(script); return 1; }
            if (script.n) buf_put(script, "\n", 1);
            i++;
            buf_put(script, c.argv[i], strlen(c.argv[i]));
            continue;
        }
        for (const char *p = a + 1; *p; p++) {
            if (*p == 'n') quiet = true;
            else if (*p == 'i') inplace = true;
            else if (*p == 'E' || *p == 'r' || *p == 's' || *p == 'u') {}
            else { errf(c, "sed: invalid option -- '%c'\n", *p); buf_free(script); return 1; }
        }
    }
    if (!script.n) {
        if (i >= c.argc) { errf(c, "Usage: sed [-n] [-i] [-e SCRIPT] SCRIPT [FILE...]\n"); return 1; }
        buf_put(script, c.argv[i], strlen(c.argv[i]));
        i++;
    }
    if (!script.p) { buf_free(script); return 0; }
    SedCmd *cmds = (SedCmd *)ps_alloc(sizeof(SedCmd) * kSedMax);
    if (!cmds) { buf_free(script); return 1; }
    const int nc = sed_parse(c, script.p, cmds);
    if (nc < 0) { heap_caps_free(cmds); buf_free(script); return 1; }
    int st = 0;
    long ln = 0;
    if (inplace) {
        if (i >= c.argc) { errf(c, "sed: no input files\n"); st = 1; }
        for (; i < c.argc && !cancelled(); i++) {
            ShBuf in, ob;
            if (!read_all(c, c.argv[i], in)) { st = 1; continue; }
            ln = 0;
            for (int k = 0; k < nc; k++) cmds[k].active = false;
            sed_run(cmds, nc, quiet, in.p ? in.p : "", in.n, ln, true,
                    [&](const char *p, size_t n) { buf_put(ob, p, n); });
            char p[kPath];
            resolve(c.argv[i], p, sizeof p);
            FILE *fp = fopen(p, "wb");
            if (!fp || (ob.n && fwrite(ob.p, 1, ob.n, fp) != ob.n)) { errf(c, "sed: couldn't write %s\n", c.argv[i]); st = 1; }
            if (fp) fclose(fp);
            buf_free(in);
            buf_free(ob);
        }
    } else {
        static const char *kDash[] = {"-"};
        char **ops = i < c.argc ? c.argv + i : (char **)kDash;
        const int nops = i < c.argc ? c.argc - i : 1;
        for (int k = 0; k < nops && !cancelled(); k++) {
            ShBuf in;
            if (!read_all(c, ops[k], in)) { st = 2; continue; }
            const bool more = sed_run(cmds, nc, quiet, in.p ? in.p : "", in.n, ln, k == nops - 1,
                                      [&](const char *p, size_t n) { wr(c.out, p, n); });
            buf_free(in);
            if (!more) break;
        }
    }
    sed_free(cmds, nc);
    heap_caps_free(cmds);
    buf_free(script);
    return st;
}

// ---------------------------------------------------------------- awk (a small subset)
// awk [-F SEP] [-v NAME=VALUE]... 'PROGRAM' [FILE...]. Rules "pattern { action }" with BEGIN and
// END; patterns /RE/, expressions, && || !; actions: print [EXPR, ...], printf FMT, EXPR..., if /
// else, next, NAME = += -= *= /= EXPR, NAME++ NAME--. Expressions: $N $0 $NF NF NR FNR FILENAME FS
// OFS, numbers, "strings", variables, + - * / %, concatenation, comparisons (== != < <= > >= ~ !~),
// length() substr() index() tolower() toupper() int(). The program is interpreted straight from
// its tokens (parsing and evaluating in one pass, a flag skipping what does not run).

enum AwkT : uint8_t { AT_EOF, AT_NUM, AT_STR, AT_RE, AT_NAME, AT_OP, AT_NL };
struct AwkTok {
    AwkT     t;
    uint16_t n;
    const char *s;     // name / op / string (unescaped) / regex source
    double   num;
    Re      *re;       // AT_RE, compiled on first use
};
struct AwkVal {
    double      n;
    const char *s;     // valid when k != AV_NUM
    uint32_t    len;
    uint8_t     k;
};
enum : uint8_t { AV_NUM, AV_STR, AV_STRNUM };
struct AwkVar {
    char     name[24];
    char    *s;        // PSRAM, kAwkVarCap
    uint32_t len;
    double   n;
    uint8_t  k;
};
constexpr int    kAwkToks   = 768;
constexpr int    kAwkVars   = 32;
constexpr int    kAwkFields = 128;
constexpr size_t kAwkVarCap = 256;
constexpr size_t kAwkArena  = 32 * 1024;

struct Awk {
    Ctx     *c;
    AwkTok  *t;
    int      nt, i;
    bool     err, next_rec, oom;
    AwkVar  *vars;
    int      nvars;
    char    *arena;
    size_t   an;
    // The current record and its fields.
    char    *rec;
    size_t   rec_len, rec_cap;
    const char *f[kAwkFields];
    uint32_t fl[kAwkFields];
    int      nf;
    long     nr, fnr;
    const char *filename;
    char     fs[16], ofs[16];
    ShBuf    ob;        // one print's output line
};

void awk_error(Awk &a, const char *what) {
    if (!a.err) errf(*a.c, "awk: %s\n", what);
    a.err = true;
}

char *awk_alloc(Awk &a, size_t n) {
    if (a.an + n + 1 > kAwkArena) { a.oom = true; return nullptr; }
    char *p = a.arena + a.an;
    a.an += n + 1;
    return p;
}

AwkVal av_num(double n) { AwkVal v; v.n = n; v.s = nullptr; v.len = 0; v.k = AV_NUM; return v; }
AwkVal av_str(const char *s, uint32_t len, uint8_t k = AV_STR) {
    AwkVal v;
    v.s = s;
    v.len = len;
    v.k = k;
    char b[48];
    const uint32_t m = len < sizeof b - 1 ? len : (uint32_t)sizeof b - 1;
    memcpy(b, s, m);
    b[m] = '\0';
    v.n = strtod(b, nullptr);
    return v;
}

// A field or input value that looks like a number compares as one.
bool av_looks_num(const AwkVal &v) {
    if (v.k == AV_NUM) return true;
    if (v.k != AV_STRNUM) return false;
    uint32_t i = 0;
    while (i < v.len && isspace((unsigned char)v.s[i])) i++;
    if (i == v.len) return false;
    char b[48];
    const uint32_t m = v.len - i < sizeof b - 1 ? v.len - i : (uint32_t)sizeof b - 1;
    memcpy(b, v.s + i, m);
    b[m] = '\0';
    char *e;
    strtod(b, &e);
    if (e == b) return false;
    while (*e && isspace((unsigned char)*e)) e++;
    return !*e;
}

// The string form of a value (numbers: integers exactly, else %.6g), in the arena.
AwkVal av_tostr(Awk &a, const AwkVal &v) {
    if (v.k != AV_NUM) return v;
    char b[40];
    int m;
    if (v.n == (double)(long long)v.n && v.n > -1e15 && v.n < 1e15) m = snprintf(b, sizeof b, "%lld", (long long)v.n);
    else m = snprintf(b, sizeof b, "%.6g", v.n);
    char *p = awk_alloc(a, (size_t)m);
    if (!p) return av_str("", 0);
    memcpy(p, b, (size_t)m + 1);
    AwkVal r = v;
    r.s = p;
    r.len = (uint32_t)m;
    r.k = AV_STR;
    r.n = v.n;
    return r;
}

bool av_true(const AwkVal &v) {
    if (v.k == AV_NUM) return v.n != 0;
    if (v.k == AV_STRNUM && av_looks_num(v)) return v.n != 0;
    return v.len > 0;
}

// ---- tokens

bool awk_tokenize(Awk &a, char *s) {
    a.nt = 0;
    auto push = [&](AwkT t, const char *p, size_t n, double num = 0) {
        if (a.nt >= kAwkToks - 1) { awk_error(a, "program too long"); return false; }
        a.t[a.nt++] = AwkTok{t, (uint16_t)n, p, num, nullptr};
        return true;
    };
    auto regex_ok = [&]() {   // a '/' here starts a regex, not a division
        if (!a.nt) return true;
        const AwkTok &p = a.t[a.nt - 1];
        if (p.t == AT_NL) return true;
        if (p.t != AT_OP) return false;
        return strchr("({},;!~&|=<>+-*%?:", p.s[0]) != nullptr;
    };
    while (*s && !a.err) {
        const char ch = *s;
        if (ch == ' ' || ch == '\t' || ch == '\r') { s++; continue; }
        if (ch == '\\' && s[1] == '\n') { s += 2; continue; }
        if (ch == '#') { while (*s && *s != '\n') s++; continue; }
        if (ch == '\n' || ch == ';') { push(AT_NL, s, 1); s++; continue; }
        if (isdigit((unsigned char)ch) || (ch == '.' && isdigit((unsigned char)s[1]))) {
            char *e;
            const double v = strtod(s, &e);
            push(AT_NUM, s, (size_t)(e - s), v);
            s = e;
            continue;
        }
        if (isalpha((unsigned char)ch) || ch == '_') {
            char *b = s;
            while (isalnum((unsigned char)*s) || *s == '_') s++;
            push(AT_NAME, b, (size_t)(s - b));
            continue;
        }
        if (ch == '"') {   // unescape in place
            char *b = ++s, *o = s;
            while (*s && *s != '"') {
                if (*s == '\\' && s[1]) {
                    int used;
                    const char e = esc_byte(s + 1, &used);
                    if (used) { *o++ = e; s += 1 + used; }
                    else { *o++ = s[1] == '/' || s[1] == '"' ? s[1] : '\\'; if (s[1] == '/' || s[1] == '"') s += 2; else s++; }
                    continue;
                }
                *o++ = *s++;
            }
            if (*s != '"') { awk_error(a, "unterminated string"); return false; }
            s++;
            push(AT_STR, b, (size_t)(o - b));
            continue;
        }
        if (ch == '/' && regex_ok()) {
            char *b = ++s, *o = s;
            while (*s && *s != '/') {
                if (*s == '\\' && s[1] == '/') { *o++ = '/'; s += 2; continue; }
                if (*s == '\\' && s[1]) { *o++ = *s++; }
                *o++ = *s++;
            }
            if (*s != '/') { awk_error(a, "unterminated regular expression"); return false; }
            s++;
            *o = '\0';
            push(AT_RE, b, (size_t)(o - b));
            continue;
        }
        static const char *kTwo[] = {"&&", "||", "==", "!=", "<=", ">=", "!~", "++", "--", "+=", "-=", "*=", "/=", "%="};
        bool two = false;
        for (const char *t : kTwo) if (s[0] == t[0] && s[1] == t[1]) { push(AT_OP, s, 2); s += 2; two = true; break; }
        if (two) continue;
        if (strchr("{}()$,+-*/%<>!~=?:", ch)) { push(AT_OP, s, 1); s++; continue; }
        char why[40];
        snprintf(why, sizeof why, "syntax error at '%c'", ch);
        awk_error(a, why);
        return false;
    }
    push(AT_EOF, s, 0);
    // NUL-terminate names / ops / numbers for strcmp convenience is avoided: compare with lengths.
    return !a.err;
}

bool tk_op(const Awk &a, const char *op) {
    const AwkTok &t = a.t[a.i];
    return t.t == AT_OP && t.n == strlen(op) && !strncmp(t.s, op, t.n);
}
bool tk_name(const Awk &a, const char *nm) {
    const AwkTok &t = a.t[a.i];
    return t.t == AT_NAME && t.n == strlen(nm) && !strncmp(t.s, nm, t.n);
}
void skip_nl(Awk &a) { while (a.t[a.i].t == AT_NL) a.i++; }

// ---- records, fields, variables

void awk_split(Awk &a) {
    a.nf = 0;
    const char *s = a.rec, *e = a.rec + a.rec_len;
    const bool ws = !strcmp(a.fs, " ");
    const size_t fl = strlen(a.fs);
    if (ws) {
        while (s < e && a.nf < kAwkFields) {
            while (s < e && (*s == ' ' || *s == '\t')) s++;
            if (s >= e) break;
            const char *b = s;
            while (s < e && *s != ' ' && *s != '\t') s++;
            a.f[a.nf] = b;
            a.fl[a.nf++] = (uint32_t)(s - b);
        }
        return;
    }
    if (s == e) return;
    while (a.nf < kAwkFields) {
        const char *m = nullptr;
        for (const char *p = s; p + fl <= e; p++) if (!memcmp(p, a.fs, fl)) { m = p; break; }
        a.f[a.nf] = s;
        a.fl[a.nf++] = (uint32_t)((m ? m : e) - s);
        if (!m) break;
        s = m + fl;
    }
}

void awk_set_record(Awk &a, const char *s, size_t n) {
    if (n + 1 > a.rec_cap) {
        char *p = (char *)ps_realloc(a.rec, n + 1);
        if (!p) { n = a.rec_cap ? a.rec_cap - 1 : 0; }
        else { a.rec = p; a.rec_cap = n + 1; }
    }
    if (a.rec) { memcpy(a.rec, s, n); a.rec[n] = '\0'; }
    a.rec_len = a.rec ? n : 0;
    awk_split(a);
}

AwkVar *awk_var(Awk &a, const char *name, size_t n, bool create) {
    for (int i = 0; i < a.nvars; i++)
        if (strlen(a.vars[i].name) == n && !strncmp(a.vars[i].name, name, n)) return &a.vars[i];
    if (!create) return nullptr;
    if (a.nvars >= kAwkVars || n >= sizeof a.vars[0].name) { awk_error(a, "too many variables"); return nullptr; }
    AwkVar &v = a.vars[a.nvars];
    snprintf(v.name, sizeof v.name, "%.*s", (int)n, name);
    v.s = (char *)ps_alloc(kAwkVarCap);
    if (!v.s) { awk_error(a, "out of memory"); return nullptr; }
    v.s[0] = '\0';
    v.len = 0;
    v.n = 0;
    v.k = AV_STRNUM;
    a.nvars++;
    return &v;
}

void awk_assign(Awk &a, AwkVar *v, const AwkVal &val) {
    if (!v) return;
    v->k = val.k;
    v->n = val.n;
    if (val.k == AV_NUM) { v->len = 0; v->s[0] = '\0'; return; }
    const uint32_t m = val.len < kAwkVarCap - 1 ? val.len : (uint32_t)kAwkVarCap - 1;
    memmove(v->s, val.s, m);
    v->s[m] = '\0';
    v->len = m;
    if (!strcmp(v->name, "FS")) snprintf(a.fs, sizeof a.fs, "%s", v->s);
    if (!strcmp(v->name, "OFS")) snprintf(a.ofs, sizeof a.ofs, "%s", v->s);
}

AwkVal awk_getvar(Awk &a, const char *name, size_t n) {
    auto is = [&](const char *k) { return strlen(k) == n && !strncmp(name, k, n); };
    if (is("NR")) return av_num((double)a.nr);
    if (is("FNR")) return av_num((double)a.fnr);
    if (is("NF")) return av_num(a.nf);
    if (is("FILENAME")) return av_str(a.filename ? a.filename : "", a.filename ? (uint32_t)strlen(a.filename) : 0);
    if (is("FS")) return av_str(a.fs, (uint32_t)strlen(a.fs));
    if (is("OFS")) return av_str(a.ofs, (uint32_t)strlen(a.ofs));
    AwkVar *v = awk_var(a, name, n, false);
    if (!v) return av_str("", 0, AV_STRNUM);
    if (v->k == AV_NUM) return av_num(v->n);
    AwkVal r = av_str(v->s, v->len, v->k);
    return r;
}

AwkVal awk_field(Awk &a, long k) {
    if (k == 0) return av_str(a.rec ? a.rec : "", (uint32_t)a.rec_len, AV_STRNUM);
    if (k < 0 || k > a.nf) return av_str("", 0, AV_STRNUM);
    return av_str(a.f[k - 1], a.fl[k - 1], AV_STRNUM);
}

// ---- expressions (parse + evaluate; ex = false only parses)

AwkVal awk_expr(Awk &a, bool ex);
AwkVal awk_unary(Awk &a, bool ex);

bool awk_re_match(Awk &a, AwkTok &t, const char *s, uint32_t n) {
    if (!t.re) {
        char pat[256];
        snprintf(pat, sizeof pat, "%.*s", (int)t.n, t.s);
        const char *err = nullptr;
        t.re = (Re *)ps_alloc(sizeof(Re));
        if (!t.re || !re_compile(*t.re, pat, false, false, &err)) {
            heap_caps_free(t.re);
            t.re = nullptr;
            awk_error(a, err ? err : "out of memory");
            return false;
        }
    }
    const char *ms, *me;
    return re_search(*t.re, s, s + n, &ms, &me);
}

AwkVal awk_call(Awk &a, bool ex, const AwkTok &fn) {
    auto is = [&](const char *k) { return fn.n == strlen(k) && !strncmp(fn.s, k, fn.n); };
    AwkVal args[3] = {};
    int na = 0;
    if (tk_op(a, "(")) {
        a.i++;
        while (!a.err && !tk_op(a, ")")) {
            const AwkVal v = awk_expr(a, ex);
            if (na < 3) args[na++] = v;
            if (tk_op(a, ",")) a.i++;
            else if (!tk_op(a, ")")) { awk_error(a, "syntax error in function call"); break; }
        }
        a.i++;
    } else if (is("length")) {
        args[na++] = awk_field(a, 0);
    }
    if (!ex || a.err) return av_num(0);
    for (int k = 0; k < na; k++) args[k] = av_tostr(a, args[k]);
    if (is("length")) return av_num(na ? args[0].len : 0);
    if (is("int")) return av_num(na ? (double)(long long)args[0].n : 0);
    if (is("substr")) {
        if (na < 2) { awk_error(a, "substr: needs 2 or 3 arguments"); return av_num(0); }
        long m = (long)args[1].n;
        long n = na > 2 ? (long)args[2].n : (long)args[0].len;
        if (m < 1) { n += m - 1; m = 1; }
        if (m > (long)args[0].len || n <= 0) return av_str("", 0);
        if (m - 1 + n > (long)args[0].len) n = (long)args[0].len - (m - 1);
        return av_str(args[0].s + m - 1, (uint32_t)n);
    }
    if (is("index")) {
        if (na < 2) { awk_error(a, "index: needs 2 arguments"); return av_num(0); }
        for (uint32_t k = 0; k + args[1].len <= args[0].len; k++)
            if (!memcmp(args[0].s + k, args[1].s, args[1].len)) return av_num(k + 1);
        return av_num(0);
    }
    if (is("tolower") || is("toupper")) {
        if (!na) return av_str("", 0);
        char *p = awk_alloc(a, args[0].len);
        if (!p) return av_str("", 0);
        for (uint32_t k = 0; k < args[0].len; k++)
            p[k] = (char)(is("tolower") ? tolower((unsigned char)args[0].s[k]) : toupper((unsigned char)args[0].s[k]));
        p[args[0].len] = '\0';
        return av_str(p, args[0].len);
    }
    char why[64];
    snprintf(why, sizeof why, "function %.*s never defined", (int)fn.n, fn.s);
    awk_error(a, why);
    return av_num(0);
}

AwkVal awk_primary(Awk &a, bool ex) {
    AwkTok &t = a.t[a.i];
    switch (t.t) {
        case AT_NUM: a.i++; return av_num(t.num);
        case AT_STR: a.i++; return av_str(t.s, t.n);
        case AT_RE:
            a.i++;
            if (!ex) return av_num(0);
            return av_num(awk_re_match(a, t, a.rec ? a.rec : "", (uint32_t)a.rec_len));
        case AT_NAME: {
            a.i++;
            static const char *kFns[] = {"length", "substr", "index", "tolower", "toupper", "int"};
            for (const char *f : kFns)
                if (t.n == strlen(f) && !strncmp(t.s, f, t.n)) return awk_call(a, ex, t);
            // assignment / increment
            if (tk_op(a, "=") || tk_op(a, "+=") || tk_op(a, "-=") || tk_op(a, "*=") || tk_op(a, "/=") || tk_op(a, "%=")) {
                const char op = a.t[a.i].s[0];
                a.i++;
                AwkVal r = awk_expr(a, ex);
                if (!ex) return r;
                AwkVar *v = awk_var(a, t.s, t.n, true);
                if (!v) return av_num(0);
                if (op != '=') {
                    const double l = awk_getvar(a, t.s, t.n).n;
                    double x = r.n;
                    if (op == '+') x = l + x;
                    else if (op == '-') x = l - x;
                    else if (op == '*') x = l * x;
                    else if (op == '/') { if (x == 0) { awk_error(a, "division by zero"); return av_num(0); } x = l / x; }
                    else { if ((long long)x == 0) { awk_error(a, "division by zero in %"); return av_num(0); } x = (double)((long long)l % (long long)x); }
                    r = av_num(x);
                }
                awk_assign(a, v, r);
                return r;
            }
            if (tk_op(a, "++") || tk_op(a, "--")) {
                const bool inc = a.t[a.i].s[0] == '+';
                a.i++;
                if (!ex) return av_num(0);
                const double old = awk_getvar(a, t.s, t.n).n;
                awk_assign(a, awk_var(a, t.s, t.n, true), av_num(inc ? old + 1 : old - 1));
                return av_num(old);
            }
            if (!ex) return av_num(0);
            return awk_getvar(a, t.s, t.n);
        }
        case AT_OP:
            if (tk_op(a, "(")) {
                a.i++;
                AwkVal v = awk_expr(a, ex);
                if (!tk_op(a, ")")) { awk_error(a, "syntax error: ')' expected"); return v; }
                a.i++;
                return v;
            }
            if (tk_op(a, "$")) {
                a.i++;
                const AwkVal k = awk_unary(a, ex);
                if (!ex) return av_num(0);
                return awk_field(a, (long)k.n);
            }
            if (tk_op(a, "++") || tk_op(a, "--")) {
                const bool inc = a.t[a.i].s[0] == '+';
                a.i++;
                const AwkTok &nm = a.t[a.i];
                if (nm.t != AT_NAME) { awk_error(a, "syntax error: ++ needs a variable"); return av_num(0); }
                a.i++;
                if (!ex) return av_num(0);
                const double v = awk_getvar(a, nm.s, nm.n).n + (inc ? 1 : -1);
                awk_assign(a, awk_var(a, nm.s, nm.n, true), av_num(v));
                return av_num(v);
            }
            break;
        default:
            break;
    }
    char why[48];
    snprintf(why, sizeof why, "syntax error at '%.*s'", t.n ? (int)t.n : 3, t.n ? t.s : "end");
    awk_error(a, why);
    if (t.t != AT_EOF) a.i++;
    return av_num(0);
}

AwkVal awk_unary(Awk &a, bool ex) {
    if (tk_op(a, "-")) { a.i++; const AwkVal v = awk_unary(a, ex); return av_num(-v.n); }
    if (tk_op(a, "+")) { a.i++; const AwkVal v = awk_unary(a, ex); return av_num(v.n); }
    if (tk_op(a, "!")) { a.i++; const AwkVal v = awk_unary(a, ex); return av_num(!av_true(v)); }
    return awk_primary(a, ex);
}

AwkVal awk_mul(Awk &a, bool ex) {
    AwkVal v = awk_unary(a, ex);
    while (!a.err && (tk_op(a, "*") || tk_op(a, "/") || tk_op(a, "%"))) {
        const char op = a.t[a.i++].s[0];
        const AwkVal r = awk_unary(a, ex);
        if (!ex) continue;
        if (op != '*' && r.n == 0) { awk_error(a, "division by zero"); return av_num(0); }
        v = av_num(op == '*' ? v.n * r.n : op == '/' ? v.n / r.n : (double)((long long)v.n % (long long)r.n));
    }
    return v;
}

AwkVal awk_add(Awk &a, bool ex) {
    AwkVal v = awk_mul(a, ex);
    while (!a.err && (tk_op(a, "+") || tk_op(a, "-"))) {
        const char op = a.t[a.i++].s[0];
        const AwkVal r = awk_mul(a, ex);
        v = av_num(op == '+' ? v.n + r.n : v.n - r.n);
    }
    return v;
}

// Can the token start an operand of a concatenation?
bool awk_starts_operand(const Awk &a) {
    const AwkTok &t = a.t[a.i];
    if (t.t == AT_NUM || t.t == AT_STR || t.t == AT_NAME) {
        if (t.t == AT_NAME) {
            static const char *kKw[] = {"if", "else", "print", "printf", "next", "BEGIN", "END", "in"};
            for (const char *k : kKw) if (t.n == strlen(k) && !strncmp(t.s, k, t.n)) return false;
        }
        return true;
    }
    return tk_op(a, "$") || tk_op(a, "(") || tk_op(a, "++") || tk_op(a, "--");
}

AwkVal awk_concat(Awk &a, bool ex) {
    AwkVal v = awk_add(a, ex);
    while (!a.err && awk_starts_operand(a)) {
        const AwkVal r = awk_add(a, ex);
        if (!ex) continue;
        const AwkVal ls = av_tostr(a, v), rs = av_tostr(a, r);
        char *p = awk_alloc(a, ls.len + rs.len);
        if (!p) return ls;
        memcpy(p, ls.s, ls.len);
        memcpy(p + ls.len, rs.s, rs.len);
        p[ls.len + rs.len] = '\0';
        v = av_str(p, ls.len + rs.len);
    }
    return v;
}

AwkVal awk_cmp(Awk &a, bool ex) {
    AwkVal v = awk_concat(a, ex);
    if (a.err) return v;
    if (tk_op(a, "~") || tk_op(a, "!~")) {
        const bool neg = a.t[a.i].s[0] == '!';
        a.i++;
        AwkTok &t = a.t[a.i];
        if (t.t != AT_RE && t.t != AT_STR) { awk_error(a, "~ needs /regex/ or a string on its right"); return v; }
        a.i++;
        if (!ex) return av_num(0);
        const AwkVal s = av_tostr(a, v);
        return av_num(awk_re_match(a, t, s.s, s.len) != neg);
    }
    static const char *kCmp[] = {"==", "!=", "<=", ">=", "<", ">"};
    const char *op = nullptr;
    for (const char *o : kCmp) if (tk_op(a, o)) { op = o; break; }
    if (!op) return v;
    a.i++;
    const AwkVal r = awk_concat(a, ex);
    if (!ex) return av_num(0);
    int d;
    if (av_looks_num(v) && av_looks_num(r)) d = v.n < r.n ? -1 : v.n > r.n ? 1 : 0;
    else {
        const AwkVal ls = av_tostr(a, v), rs = av_tostr(a, r);
        const uint32_t m = ls.len < rs.len ? ls.len : rs.len;
        d = memcmp(ls.s, rs.s, m);
        if (!d) d = ls.len < rs.len ? -1 : ls.len > rs.len ? 1 : 0;
    }
    bool res;
    if (op[0] == '=') res = d == 0;
    else if (op[0] == '!') res = d != 0;
    else if (op[0] == '<') res = op[1] ? d <= 0 : d < 0;
    else res = op[1] ? d >= 0 : d > 0;
    return av_num(res);
}

AwkVal awk_and(Awk &a, bool ex) {
    AwkVal v = awk_cmp(a, ex);
    while (!a.err && tk_op(a, "&&")) {
        a.i++;
        skip_nl(a);
        const bool l = av_true(v);
        const AwkVal r = awk_cmp(a, ex && l);
        v = av_num(l && av_true(r));
    }
    return v;
}

AwkVal awk_or(Awk &a, bool ex) {
    AwkVal v = awk_and(a, ex);
    while (!a.err && tk_op(a, "||")) {
        a.i++;
        skip_nl(a);
        const bool l = av_true(v);
        const AwkVal r = awk_and(a, ex && !l);
        v = av_num(l || av_true(r));
    }
    return v;
}

AwkVal awk_expr(Awk &a, bool ex) {
    AwkVal v = awk_or(a, ex);
    if (!a.err && tk_op(a, "?")) {   // cond ? x : y
        a.i++;
        const bool c = av_true(v);
        const AwkVal x = awk_expr(a, ex && c);
        if (!tk_op(a, ":")) { awk_error(a, "syntax error: ':' expected"); return x; }
        a.i++;
        const AwkVal y = awk_expr(a, ex && !c);
        return c ? x : y;
    }
    return v;
}

// ---- statements

void awk_stmt(Awk &a, bool ex);

void awk_block(Awk &a, bool ex) {   // { stmts } or one statement
    skip_nl(a);
    if (tk_op(a, "{")) {
        a.i++;
        for (;;) {
            skip_nl(a);
            if (a.err || tk_op(a, "}") || a.t[a.i].t == AT_EOF) break;
            awk_stmt(a, ex && !a.next_rec);
        }
        if (!tk_op(a, "}")) { awk_error(a, "syntax error: '}' expected"); return; }
        a.i++;
    } else {
        awk_stmt(a, ex);
    }
}

void awk_print(Awk &a, bool ex, bool fmt) {
    AwkVal vals[16];
    int n = 0;
    const bool paren = tk_op(a, "(");
    if (paren) a.i++;
    while (!a.err && a.t[a.i].t != AT_NL && a.t[a.i].t != AT_EOF && !tk_op(a, "}") && !(paren && tk_op(a, ")"))) {
        const AwkVal v = awk_expr(a, ex);
        if (n < 16) vals[n++] = v;
        if (tk_op(a, ",")) { a.i++; continue; }
        break;
    }
    if (paren && tk_op(a, ")")) a.i++;
    if (!ex || a.err) return;
    if (fmt) {
        if (!n) { awk_error(a, "printf: no format"); return; }
        char *args[16];
        for (int k = 0; k < n; k++) {
            const AwkVal s = av_tostr(a, vals[k]);
            char *p = awk_alloc(a, s.len);
            if (p) { memcpy(p, s.s, s.len); p[s.len] = '\0'; }
            args[k] = p ? p : (char *)"";
        }
        int ai = 0;
        printf_core(*a.c, args[0], args + 1, n - 1, &ai);
        return;
    }
    a.ob.n = 0;
    if (!n) buf_put(a.ob, a.rec ? a.rec : "", a.rec_len);
    for (int k = 0; k < n; k++) {
        if (k) buf_put(a.ob, a.ofs, strlen(a.ofs));
        const AwkVal s = av_tostr(a, vals[k]);
        buf_put(a.ob, s.s, s.len);
    }
    buf_put(a.ob, "\n", 1);
    wr(a.c->out, a.ob.p, a.ob.n);
}

void awk_print_record(Awk &a) {
    a.ob.n = 0;
    buf_put(a.ob, a.rec ? a.rec : "", a.rec_len);
    buf_put(a.ob, "\n", 1);
    wr(a.c->out, a.ob.p, a.ob.n);
}

void awk_stmt(Awk &a, bool ex) {
    skip_nl(a);
    if (tk_op(a, "{")) { awk_block(a, ex); return; }
    if (tk_name(a, "print") || tk_name(a, "printf")) {
        const bool f = a.t[a.i].n == 6;
        a.i++;
        awk_print(a, ex, f);
    } else if (tk_name(a, "next")) {
        a.i++;
        if (ex) a.next_rec = true;
    } else if (tk_name(a, "if")) {
        a.i++;
        if (!tk_op(a, "(")) { awk_error(a, "syntax error: '(' expected after if"); return; }
        a.i++;
        const bool c = av_true(awk_expr(a, ex));
        if (!tk_op(a, ")")) { awk_error(a, "syntax error: ')' expected"); return; }
        a.i++;
        awk_block(a, ex && c);
        const int save = a.i;
        skip_nl(a);
        if (tk_name(a, "else")) { a.i++; awk_block(a, ex && !c); }
        else a.i = save;
        return;
    } else {
        awk_expr(a, ex);
    }
    if (a.err) return;
    if (a.t[a.i].t == AT_NL) a.i++;
    else if (!tk_op(a, "}") && a.t[a.i].t != AT_EOF) awk_error(a, "syntax error: statement not terminated");
}

// Run every rule for one phase: 0 BEGIN, 1 a record, 2 END.
void awk_rules(Awk &a, int phase) {
    a.i = 0;
    a.next_rec = false;
    a.an = 0;
    for (;;) {
        skip_nl(a);
        if (a.err || a.t[a.i].t == AT_EOF) break;
        int kind = 1;
        if (tk_name(a, "BEGIN")) { kind = 0; a.i++; }
        else if (tk_name(a, "END")) { kind = 2; a.i++; }
        bool match = kind == phase && !a.next_rec;
        if (kind == 1 && !tk_op(a, "{")) {
            const AwkVal p = awk_expr(a, match);
            match = match && av_true(p);
        }
        if (a.err) break;
        if (tk_op(a, "{")) awk_block(a, match);
        else if (match) awk_print_record(a);          // a pattern alone prints the record
    }
}

int b_awk(Ctx &c) {
    Awk *a = (Awk *)heap_caps_calloc(1, sizeof(Awk), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!a) { errf(c, "awk: out of memory\n"); return 2; }
    a->c = &c;
    snprintf(a->fs, sizeof a->fs, " ");
    snprintf(a->ofs, sizeof a->ofs, " ");
    a->t = (AwkTok *)ps_alloc(sizeof(AwkTok) * kAwkToks);
    a->vars = (AwkVar *)heap_caps_calloc(kAwkVars, sizeof(AwkVar), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    a->arena = (char *)ps_alloc(kAwkArena);
    char *prog = nullptr;
    int st = 0;
    int i = 1;
    const char *assigns[16];
    int nassign = 0;
    if (!a->t || !a->vars || !a->arena) { errf(c, "awk: out of memory\n"); st = 2; goto out; }
    for (; i < c.argc; i++) {
        const char *o = c.argv[i];
        if (o[0] != '-' || !o[1]) break;
        if (!strcmp(o, "--")) { i++; break; }
        if (o[1] == 'F') {
            const char *v = o[2] ? o + 2 : (i + 1 < c.argc ? c.argv[++i] : nullptr);
            if (!v) { errf(c, "awk: option requires an argument -- F\n"); st = 2; goto out; }
            if (!strcmp(v, "\\t") || !strcmp(v, "t")) snprintf(a->fs, sizeof a->fs, "\t");
            else snprintf(a->fs, sizeof a->fs, "%s", v);
        } else if (o[1] == 'v') {
            const char *v = o[2] ? o + 2 : (i + 1 < c.argc ? c.argv[++i] : nullptr);
            if (!v || !strchr(v, '=')) { errf(c, "awk: -v needs NAME=VALUE\n"); st = 2; goto out; }
            if (nassign < 16) assigns[nassign++] = v;
        } else {
            errf(c, "awk: unknown option %s\n", o);
            st = 2;
            goto out;
        }
    }
    if (i >= c.argc) { errf(c, "usage: awk [-F SEP] [-v NAME=VALUE] 'PROGRAM' [FILE...]\n"); st = 2; goto out; }
    prog = (char *)ps_alloc(strlen(c.argv[i]) + 1);
    if (!prog) { st = 2; goto out; }
    strcpy(prog, c.argv[i++]);
    if (!awk_tokenize(*a, prog)) { st = 2; goto out; }
    for (int k = 0; k < nassign; k++) {
        const char *eq = strchr(assigns[k], '=');
        AwkVar *v = awk_var(*a, assigns[k], (size_t)(eq - assigns[k]), true);
        awk_assign(*a, v, av_str(eq + 1, (uint32_t)strlen(eq + 1), AV_STRNUM));
    }
    awk_set_record(*a, "", 0);
    awk_rules(*a, 0);
    {
        // Records: only when there are rules besides BEGIN (awk 'BEGIN{...}' reads no input).
        bool main_rules = false;   // a top-level rule that is not BEGIN (END reads the input too)
        bool begin_block = false;
        for (int k = 0, depth = 0; k < a->nt && !main_rules; k++) {
            const AwkTok &t = a->t[k];
            const bool open = t.t == AT_OP && t.s[0] == '{' && t.n == 1;
            if (t.t == AT_OP && t.s[0] == '}' && t.n == 1) { depth--; continue; }
            if (depth) { if (open) depth++; continue; }
            if (open) {
                depth++;
                if (begin_block) begin_block = false;
                else main_rules = true;
                continue;
            }
            if (t.t == AT_NL || t.t == AT_EOF) continue;
            if (t.t == AT_NAME && t.n == 5 && !strncmp(t.s, "BEGIN", 5)) { begin_block = true; continue; }
            main_rules = true;
        }
        static const char *kDash[] = {"-"};
        char **ops = i < c.argc ? c.argv + i : (char **)kDash;
        const int nops = i < c.argc ? c.argc - i : 1;
        for (int k = 0; main_rules && k < nops && !a->err && !cancelled(); k++) {
            ShBuf in;
            if (!read_all(c, ops[k], in)) { st = 2; continue; }
            a->filename = strcmp(ops[k], "-") ? ops[k] : "";
            a->fnr = 0;
            each_line(in.p ? in.p : "", in.n, [&](const char *s, size_t len) {
                a->nr++;
                a->fnr++;
                awk_set_record(*a, s, len);
                awk_rules(*a, 1);
                return !a->err;
            });
            buf_free(in);
        }
    }
    if (!a->err) awk_rules(*a, 2);
    if (a->oom) errf(c, "awk: string space exhausted: some values were cut\n");
    if (a->err) st = 2;
out:
    if (a) {
        if (a->t) for (int k = 0; k < a->nt; k++) heap_caps_free(a->t[k].re);
        if (a->vars) for (int k = 0; k < a->nvars; k++) heap_caps_free(a->vars[k].s);
        heap_caps_free(a->t);
        heap_caps_free(a->vars);
        heap_caps_free(a->arena);
        heap_caps_free(a->rec);
        buf_free(a->ob);
        heap_caps_free(a);
    }
    heap_caps_free(prog);
    return st;
}

// ---------------------------------------------------------------- small system commands

int b_id(Ctx &c) {
    outf(c, "uid=1000(%s) gid=1000(%s) groups=1000(%s)\n", kUser, kUser, kUser);
    return 0;
}

int b_nproc(Ctx &c) {
    outf(c, "%d\n", portNUM_PROCESSORS);
    return 0;
}

int b_realpath(Ctx &c) {
    int st = 0;
    int i = 1;
    while (i < c.argc && c.argv[i][0] == '-' && c.argv[i][1]) i++;   // -e -m -s: accepted
    if (i >= c.argc) { errf(c, "realpath: missing operand\n"); return 1; }
    for (; i < c.argc; i++) {
        char p[kPath];
        resolve(c.argv[i], p, sizeof p);
        if (!exists(p) && !is_mount_root(p)) { errf(c, "realpath: %s: No such file or directory\n", c.argv[i]); st = 1; continue; }
        outf(c, "%s\n", p);
    }
    return st;
}

// base64 [-d] [-w COLS] [FILE]: RFC 4648, lines of 76 by default (-w 0: one line).
int b_base64(Ctx &c) {
    Flags f;
    const char *vals[64] = {};
    const int i = getopts(c, "diw", "w", f, vals);
    if (i < 0) return 1;
    const long wrap = optv(vals, 'w') ? strtol(optv(vals, 'w'), nullptr, 10) : 76;
    ShBuf in;
    if (!read_all(c, i < c.argc ? c.argv[i] : "-", in)) return 1;
    int st = 0;
    if (f.has('d')) {
        // Drop whitespace (and with -i anything outside the alphabet) first.
        size_t k = 0;
        for (size_t x = 0; x < in.n; x++) {
            const char ch = in.p[x];
            if (isalnum((unsigned char)ch) || ch == '+' || ch == '/' || ch == '=') in.p[k++] = ch;
            else if (!isspace((unsigned char)ch) && !f.has('i')) { errf(c, "base64: invalid input\n"); st = 1; break; }
        }
        size_t olen = 0;
        unsigned char *o = (unsigned char *)ps_alloc(k / 4 * 3 + 4);
        if (!st && o && mbedtls_base64_decode(o, k / 4 * 3 + 4, &olen, (const unsigned char *)(in.p ? in.p : ""), k) == 0)
            wr(c.out, (const char *)o, olen);
        else if (!st) { errf(c, "base64: invalid input\n"); st = 1; }
        heap_caps_free(o);
    } else {
        const size_t cap = (in.n + 2) / 3 * 4 + 1;
        unsigned char *o = (unsigned char *)ps_alloc(cap);
        size_t olen = 0;
        if (!o || mbedtls_base64_encode(o, cap, &olen, (const unsigned char *)(in.p ? in.p : ""), in.n) != 0) {
            errf(c, "base64: out of memory\n");
            st = 1;
        } else if (wrap <= 0) {
            wr(c.out, (const char *)o, olen);
            wr(c.out, "\n", 1);
        } else {
            for (size_t x = 0; x < olen; x += (size_t)wrap) {
                const size_t n = olen - x < (size_t)wrap ? olen - x : (size_t)wrap;
                wr(c.out, (const char *)o + x, n);
                wr(c.out, "\n", 1);
            }
        }
        heap_caps_free(o);
    }
    buf_free(in);
    return st;
}

int b_time(Ctx &c);
int b_watch(Ctx &c);
int b_xargs(Ctx &c);

// ---------------------------------------------------------------- command table

struct Builtin {
    const char *name;
    int (*fn)(Ctx &);
    const char *usage;
    const char *desc;
};

const Builtin kBuiltins[] = {
    {"[", b_test, "[ EXPRESSION ]", "evaluate a condition (like test)"},
    {"apps", b_apps, "apps", "list installed terminal programs"},
    {"awk", b_awk, "awk [-F SEP] [-v N=V] 'PROGRAM' [FILE...]", "pattern scanning (subset)"},
    {"base64", b_base64, "base64 [-d] [-w COLS] [FILE]", "base64 encode / decode"},
    {"basename", b_basename, "basename NAME [SUFFIX]", "strip directory and suffix"},
    {"bl", b_bl, "bl 0-100", "set the backlight"},
    {"cat", b_cat, "cat [-n] [FILE...]", "print files"},
    {"cd", b_cd, "cd [DIR|-]", "change directory"},
    {"clear", b_clear, "clear", "clear the screen"},
    {"command", b_which, "command -v NAME...", "print how a name would run"},
    {"cp", b_cp, "cp [-rnv] SRC... DEST", "copy files and directories"},
    {"curl", b_curl, "curl [-sLfO] [-o FILE] URL", "transfer a URL (HTTP/HTTPS)"},
    {"cut", b_cut, "cut -f LIST [-d C] [-s] | -c LIST [FILE...]", "select fields or characters"},
    {"date", b_date, "date [+FORMAT]", "print the date and time"},
    {"df", b_df, "df [-h]", "free space on each volume"},
    {"dirname", b_dirname, "dirname NAME", "strip the last path component"},
    {"dmesg", b_dmesg, "dmesg", "kernel log"},
    {"du", b_du, "du [-shc] [PATH...]", "disk usage"},
    {"echo", b_echo, "echo [-ne] [TEXT...]", "print text"},
    {"edit", b_edit, "edit [FILE]", "text editor (nano keys: ^S save, ^X exit)"},
    {"env", b_env, "env", "print the environment"},
    {"exit", b_exit, "exit", "close the terminal"},
    {"export", b_export, "export NAME=VALUE...", "set variables"},
    {"expr", b_expr, "expr EXPRESSION", "evaluate an expression"},
    {"false", b_false, "false", "exit with status 1"},
    {"find", b_find, "find [PATH...] [-name|-iname PAT] [-type f|d] [-maxdepth N] [-mindepth N] [-size [+-]N[ckMG]] [-mtime|-mmin [+-]N] [-newer F] [-empty]", "search for files"},
    {"free", b_free, "free [-hkm]", "memory usage"},
    {"grep", b_grep, "grep [-ivnclrFqHhow] [-e] PATTERN [FILE...]", "print lines matching a pattern"},
    {"head", b_head, "head [-n N|-c N] [FILE...]", "first lines"},
    {"help", b_help, "help [COMMAND]", "list commands / show usage"},
    {"history", b_history, "history [-c]", "command history"},
    {"host", b_host, "host NAME", "DNS lookup"},
    {"hostname", b_hostname, "hostname [-I]", "print the host name"},
    {"i2cdetect", b_i2cdetect, "i2cdetect", "scan the I2C bus"},
    {"id", b_id, "id", "user and group ids"},
    {"ip", b_ip, "ip", "network address and link"},
    {"launch", b_launch, "launch APP_ID", "open an app on the screen"},
    {"ui", b_ui, "ui", "the screen as text: [ref] role \"text\" @x,y"},
    {"ha", b_ha, "ha say TEXT | ls [FILTER] | find T | get E | on|off|toggle E | set E k=v | call D.S", "Home Assistant (Settings > Casa)"},
    {"dev", b_dev, "dev scan | ls | add N TYPE IP | get N | on|off|toggle N | set N bri=", "Shelly / Tasmota / WLED on the LAN"},
    {"app", b_app, "app check FILE | app run NAME [-t S] | app ls", "check a .lua/.py/.json, run a Lua App script and get its error"},
    {"diff", b_diff, "diff [-u] FILE1 FILE2", "unified diff of two text files"},
    {"jq", b_jq, "jq [-rc] FILTER [FILE]", "JSON query: . .a.b .[0] .[] keys length, | chains"},
    {"sysinfo", b_sysinfo, "sysinfo", "the board in one call: time, app, wifi, sd, ram, volume"},
    {"vol", b_vol, "vol [0-100]", "volume (no argument: print it)"},
    {"notify", b_notify, "notify [-t TITLE] TEXT", "a system notification"},
    {"tg", b_tg, "tg TEXT", "a message to the paired Telegram chat (or: cmd | tg)"},
    {"tap", b_tap, "tap @REF | tap TEXT | tap X Y", "tap a control on the screen"},
    {"input", b_input, "input tap X Y|@REF | text TEXT | keyevent ENTER | swipe X0 Y0 X1 Y1", "touch and keys, as adb shell input"},
    {"home", b_home, "home", "back to the home screen"},
    {"screenshot", b_screenshot, "screenshot [-d SEC] [FILE]", "save the screen as a JPEG (~/shots)"},
    {"less", b_less, "less [FILE]", "page through text (q quits, / searches)"},
    {"ls", b_ls, "ls [-laAhtSr1dF] [PATH...]", "list directory contents"},
    {"man", b_help, "man COMMAND", "show usage"},
    {"md5sum", b_hash, "md5sum [FILE...]", "MD5 checksums"},
    {"mkdir", b_mkdir, "mkdir [-pv] DIR...", "make directories"},
    {"mv", b_mv, "mv [-nv] SRC... DEST", "move or rename"},
    {"nl", b_nl, "nl [-b a] [FILE...]", "number lines"},
    {"nproc", b_nproc, "nproc", "number of CPU cores"},
    {"open", b_open, "open FILE", "open a file in its app"},
    {"ping", b_ping, "ping [-c COUNT] HOST", "send ICMP echo requests"},
    {"printf", b_printf, "printf FORMAT [ARG...]", "formatted output"},
    {"ps", b_ps, "ps", "system services"},
    {"cfg", b_cfg, "cfg [KEY [VALUE]] | cfg export > F | cfg import F", "system settings (live): brightness, dnd, thmode, ha_url..."},
    {"wifi", b_wifi, "wifi [status|scan|on|off|join SSID [PASS]|leave|forget SSID]", "Wi-Fi networks"},
    {"pwd", b_pwd, "pwd", "print the working directory"},
    {"realpath", b_realpath, "realpath PATH...", "absolute path"},
    {"reboot", b_reboot, "reboot", "restart the device"},
    {"reset", b_reset, "reset", "reset the terminal"},
    {"rev", b_rev, "rev [FILE...]", "reverse each line"},
    {"rm", b_rm, "rm [-rfv] FILE...", "remove files or directories"},
    {"rmdir", b_rmdir, "rmdir DIR...", "remove empty directories"},
    {"sed", b_sed, "sed [-n] [-i] [-e SCRIPT] SCRIPT [FILE...]", "stream editor (s d p q =)"},
    {"sensors", b_sensors, "sensors", "chip temperature"},
    {"seq", b_seq, "seq [-s SEP] [-w] [FIRST [INC]] LAST", "print a sequence of numbers"},
    {"sha1sum", b_hash, "sha1sum [FILE...]", "SHA-1 checksums"},
    {"sha256sum", b_hash, "sha256sum [FILE...]", "SHA-256 checksums"},
    {"sleep", b_sleep, "sleep SECONDS", "wait"},
    {"sort", b_sort, "sort [-rnufh] [-k K[,E]] [-t SEP] [FILE...]", "sort lines"},
    {"stat", b_stat, "stat [-c FORMAT] FILE...", "file status"},
    {"store", b_store, "store search WORDS | list [CAT] | info ID | install ID | remove ID", "the app store: find and install apps"},
    {"stty", b_stty, "stty [size]", "terminal settings"},
    {"tac", b_tac, "tac [FILE...]", "print lines in reverse order"},
    {"tail", b_tail, "tail [-n N|+N] [-c N] [FILE...]", "last lines"},
    {"tee", b_tee, "tee [-a] [FILE...]", "copy stdin to files and stdout"},
    {"test", b_test, "test EXPRESSION", "evaluate a condition"},
    {"time", b_time, "time [-p] COMMAND [ARG...]", "time a command (wall clock)"},
    {"top", b_top, "top [-b] [-n N] [-d SECONDS]", "live task and CPU view"},
    {"touch", b_touch, "touch FILE...", "create a file / update its time"},
    {"tr", b_tr, "tr [-dsc] SET1 [SET2]", "translate or delete characters"},
    {"tree", b_tree, "tree [-ad] [-L N] [DIR]", "directory tree"},
    {"true", b_true, "true", "exit with status 0"},
    {"type", b_which, "type NAME...", "how a name would be run"},
    {"uname", b_uname, "uname [-asnrmo]", "system information"},
    {"update", b_update, "update [status|check|install|sd [FILE]|restart]", "firmware updates (Settings > Update)"},
    {"uniq", b_uniq, "uniq [-cdi] [FILE...]", "drop repeated lines"},
    {"unset", b_unset, "unset NAME...", "remove variables"},
    {"uptime", b_uptime, "uptime", "time since boot"},
    {"usb", b_usb, "usb [host|device]", "USB port mode"},
    {"watch", b_watch, "watch [-n SECONDS] [-t] COMMAND [ARG...]", "run a command repeatedly"},
    {"wc", b_wc, "wc [-lwc] [FILE...]", "count lines, words, bytes"},
    {"wget", b_wget, "wget [-q] [-O FILE] URL", "download a file"},
    {"which", b_which, "which NAME...", "locate a command"},
    {"whoami", b_whoami, "whoami", "print the user name"},
    {"xargs", b_xargs, "xargs [-n N] [-I R] [-0] [-d C] [-t] [-r] [COMMAND...]", "build commands from stdin"},
    {"xxd", b_xxd, "xxd [-l N] [FILE]", "hex dump"},
};

// Names that behave like their GNU twins.
const struct { const char *alias; const char *name; } kAliases[] = {
    {"cls", "clear"}, {"dir", "ls"}, {"log", "dmesg"}, {"temp", "sensors"}, {"neofetch", "sysinfo"},
    {"status", "sysinfo"},
    {"ifconfig", "ip"}, {"mem", "free"}, {"i2c", "i2cdetect"}, {"ver", "uname"},
    {"version", "uname"}, {"services", "ps"}, {"hexdump", "xxd"}, {"programs", "apps"},
    {"xdg-open", "open"}, {"logout", "exit"}, {"printenv", "env"}, {"set", "env"},
    {"restart", "reboot"}, {"nslookup", "host"}, {"htop", "top"}, {"readlink", "realpath"},
    {"egrep", "grep"}, {"gawk", "awk"},
    {"nano", "edit"}, {"more", "less"}, {"pico", "edit"},
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
    const bool command = !strcmp(c.argv[0], "command");   // command -v NAME
    int i = 1;
    if (command) {
        if (i < c.argc && (!strcmp(c.argv[i], "-v") || !strcmp(c.argv[i], "-V"))) i++;
        else { errf(c, "command: only 'command -v NAME' is supported\n"); return 2; }
    }
    int st = 0;
    for (; i < c.argc; i++) {
        const char *n = c.argv[i];
        nv_wasm_app_t app;
        if (find_builtin(n)) {
            if (type) outf(c, "%s is a shell builtin\n", n);
            else if (command) outf(c, "%s\n", n);
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
        // split_args() (nv_wasm_wasi.c) knows quotes but no escapes, and adjacent quoted
        // segments join into one word: quote with whichever of " ' the arg lacks, and switch
        // quote style around any char equal to the current one.
        const bool q = !argv[i][0] || strpbrk(argv[i], " \t\"'");
        char qc = strchr(argv[i], '"') ? '\'' : '"';
        if (q) out[n++] = qc;
        for (const char *s = argv[i]; *s && n + 5 < cap; s++) {
            if (q && *s == qc) {
                const char oc = qc == '"' ? '\'' : '"';
                out[n++] = qc;
                out[n++] = oc;
                out[n++] = *s;
                out[n++] = oc;
                out[n++] = qc;
                continue;
            }
            out[n++] = *s;
        }
        if (q && n + 1 < cap) out[n++] = qc;
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
    // Aliases that carry options, as in a usual ~/.bashrc (ll = ls -la, rg = grep -rn...).
    static const struct { const char *alias, *name, *opt; } kFlagAlias[] = {
        {"ll", "ls", "-la"}, {"la", "ls", "-A"}, {"l", "ls", "-lA"}, {"rg", "grep", "-rn"} };
    char *av2[kMaxArgs + 2];
    for (const auto &fa : kFlagAlias) {
        if (strcmp(st.argv[0], fa.alias) || st.argc >= kMaxArgs) continue;
        av2[0] = (char *)fa.name;
        av2[1] = (char *)fa.opt;
        for (int i = 1; i < st.argc; i++) av2[i + 1] = st.argv[i];
        av2[st.argc + 1] = nullptr;
        c.argc = st.argc + 1;
        c.argv = av2;
        break;
    }
    const char *name = c.argv[0];
    if (const Builtin *b = find_builtin(name)) {
        // "CMD --help" prints the usage line, as GNU tools do (echo/printf/test print their args).
        if (st.argc == 2 && !strcmp(st.argv[1], "--help") && strcmp(b->name, "echo") &&
            strcmp(b->name, "printf") && strcmp(b->name, "test") && strcmp(b->name, "[")) {
            outf(c, "Usage: %s\n%s\n", b->usage, b->desc);
            return 0;
        }
        VolsHold hold;
        const int r = b->fn(c);
        term_tty_raw(false);   // a full-screen built-in never leaves the keyboard raw
        return r;
    }
    // the names language models (and people) type for our programs
    static const char *const kAlias[][2] = { {"python3", "python"}, {"py", "python"}, {"micropython", "python"},
        {"node", "js"}, {"nodejs", "js"}, {"qjs", "js"}, {"lua5.4", "lua"}, {"sqlite", "sqlite3"},
        {"unzip", "zip"}, {"jq", "cjson"}, {"vi", "edit"}, {"vim", "edit"}, {"nano", "edit"} };
    for (const auto &a : kAlias) {
        if (strcmp(name, a[0])) continue;
        name = a[1];
        if (const Builtin *b = find_builtin(name)) { VolsHold hold; const int r = b->fn(c); term_tty_raw(false); return r; }
        break;
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
    // one line that lets a model correct itself without reading manuals
    if (!strcmp(name, "python") || !strcmp(name, "js") || !strcmp(name, "lua") || !strcmp(name, "sqlite3"))
        errf(c, "%s: not installed (store install %s)\n", name, name);
    else
        errf(c, "%s: command not found (commands: help; programs: apps; more: store search %s)\n", name, name);
    (void)interactive;
    return 127;
}

// ---------------------------------------------------------------- commands that run commands

// Run argv as one command with the caller's output (time, watch, xargs). No pipes or ; here: the
// words were expanded once already, as with bash's `time CMD` / `xargs CMD`.
int run_argv(Ctx &c, int argc, char **argv, const char *in, size_t in_len, bool has_in) {
    Stage *st = (Stage *)heap_caps_calloc(1, sizeof(Stage), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!st) { errf(c, "%s: out of memory\n", c.argv[0]); return 1; }
    const int n = argc < kMaxArgs ? argc : kMaxArgs;
    for (int k = 0; k < n; k++) st->argv[k] = argv[k];
    st->argv[n] = nullptr;
    st->argc = n;
    const int r = run_stage(*st, in, in_len, has_in, c.out, c.err, false);
    heap_caps_free(st);
    return r;
}

// time [-p] COMMAND: wall-clock time on stderr. There is no per-task CPU accounting to report
// user / sys from, so only "real" is printed.
int b_time(Ctx &c) {
    int i = 1;
    bool posix = false;
    if (i < c.argc && !strcmp(c.argv[i], "-p")) { posix = true; i++; }
    const int64_t t0 = esp_timer_get_time();
    int r = 0;
    if (i < c.argc) r = run_argv(c, c.argc - i, c.argv + i, c.in, c.in_len, c.has_in);
    const double s = (double)(esp_timer_get_time() - t0) / 1e6;
    if (posix) errf(c, "real %.2f\n", s);
    else {
        const int m = (int)(s / 60);
        errf(c, "\nreal\t%dm%.3fs\n", m, s - 60.0 * m);
    }
    return r;
}

// watch [-n SECONDS] [-t] COMMAND: run it again and again, the screen cleared each time, until ^C.
int b_watch(Ctx &c) {
    double iv = 2.0;
    bool title = true;
    int i = 1;
    for (; i < c.argc && c.argv[i][0] == '-'; i++) {
        const char *a = c.argv[i];
        if (!strcmp(a, "-t") || !strcmp(a, "--no-title")) title = false;
        else if (!strcmp(a, "-n") && i + 1 < c.argc) iv = strtod(c.argv[++i], nullptr);
        else if (!strncmp(a, "-n", 2) && a[2]) iv = strtod(a + 2, nullptr);
        else if (!strncmp(a, "--interval=", 11)) iv = strtod(a + 11, nullptr);
        else { errf(c, "watch: invalid option '%s'\n", a); return 1; }
    }
    if (i >= c.argc) { errf(c, "Usage: watch [-n SECONDS] [-t] COMMAND [ARG...]\n"); return 1; }
    if (iv < 0.1) iv = 0.1;
    char cmd[160];
    join_args(c.argv + i, c.argc - i, cmd, sizeof cmd);
    while (!cancelled()) {
        if (tty(c)) wr(c.out, "\x1b[H\x1b[2J");
        if (title) {
            char now[40];
            nv_time_format(now, sizeof now, "%a %b %e %H:%M:%S %Y");
            outf(c, "Every %.1fs: %s    %s: %s\n\n", iv, cmd, kHost, now);
        }
        run_argv(c, c.argc - i, c.argv + i, "", 0, true);
        const int64_t end = esp_timer_get_time() + (int64_t)(iv * 1e6);
        while (esp_timer_get_time() < end && !cancelled()) vTaskDelay(pdMS_TO_TICKS(20));
    }
    return 130;
}

// xargs [-n N] [-I REPL] [-0] [-d DELIM] [-t] [-r] [COMMAND [ARG...]]: run COMMAND with words read
// from stdin as extra arguments (echo when no COMMAND). Quotes '..' ".." and \ group words.
int b_xargs(Ctx &c) {
    long maxn = 0;
    const char *repl = nullptr;
    bool zero = false, trace = false, noempty = false;
    char delim = 0;
    int i = 1;
    for (; i < c.argc && c.argv[i][0] == '-' && c.argv[i][1]; i++) {
        const char *a = c.argv[i];
        if (!strcmp(a, "--")) { i++; break; }
        const char *v = a[2] ? a + 2 : (i + 1 < c.argc ? c.argv[i + 1] : nullptr);
        switch (a[1]) {
            case 'n': if (!v) goto usage; maxn = strtol(v, nullptr, 10); if (!a[2]) i++; break;
            case 'I': if (!v) goto usage; repl = v; if (!a[2]) i++; break;
            case 'd': if (!v) goto usage; { int used; delim = v[0] == '\\' && v[1] ? esc_byte(v + 1, &used) : v[0]; } if (!a[2]) i++; break;
            case '0': zero = true; break;
            case 't': trace = true; break;
            case 'r': noempty = true; break;
            default:
                errf(c, "xargs: invalid option -- '%c'\n", a[1]);
                return 1;
        }
    }
    {
        static const char *kEcho[] = {"echo"};
        char **cmd = i < c.argc ? c.argv + i : (char **)kEcho;
        const int ncmd = i < c.argc ? c.argc - i : 1;
        // Split stdin into items (in a private copy).
        const size_t n = c.has_in ? c.in_len : 0;
        char *buf = (char *)ps_alloc(n + 1);
        int cap = 256, nitems = 0;
        char **items = (char **)ps_alloc(sizeof(char *) * cap);
        char **av = (char **)ps_alloc(sizeof(char *) * (kMaxArgs + 1));
        char **made = (char **)ps_alloc(sizeof(char *) * (kMaxArgs + 1));   // -I strings to free
        int st = 0;
        if (!buf || !items || !av || !made) {
            errf(c, "xargs: out of memory\n");
            heap_caps_free(buf);
            heap_caps_free(items);
            heap_caps_free(av);
            heap_caps_free(made);
            return 1;
        }
        {
            size_t o = 0, k = 0;
            const char sep = zero ? '\0' : delim ? delim : (repl ? '\n' : 0);
            while (k < n) {
                if (!sep) while (k < n && isspace((unsigned char)c.in[k])) k++;
                else if (repl) while (k < n && (c.in[k] == ' ' || c.in[k] == '\t')) k++;
                if (k >= n) break;
                const size_t start = o;
                char q = 0;
                while (k < n) {
                    const char ch = c.in[k];
                    if (sep) { if (ch == sep) { k++; break; } buf[o++] = ch; k++; continue; }
                    if (q) { if (ch == q) q = 0; else buf[o++] = ch; k++; continue; }
                    if (ch == '\'' || ch == '"') { q = ch; k++; continue; }
                    if (ch == '\\' && k + 1 < n) { buf[o++] = c.in[k + 1]; k += 2; continue; }
                    if (isspace((unsigned char)ch)) break;
                    buf[o++] = ch;
                    k++;
                }
                buf[o++] = '\0';
                if (sep && !zero && o - start == 1) { o = start; continue; }   // an empty line
                if (nitems == cap) {
                    char **g = (char **)ps_realloc(items, sizeof(char *) * cap * 2);
                    if (!g) break;
                    items = g;
                    cap *= 2;
                }
                items[nitems++] = buf + start;
            }
        }
        auto run = [&](int argc) {
            av[argc] = nullptr;
            if (trace) {
                for (int k = 0; k < argc; k++) errf(c, "%s%s", k ? " " : "", av[k]);
                errf(c, "\n");
            }
            const int r = run_argv(c, argc, av, "", 0, true);
            if (r == 127 || r == 126) st = r;
            else if (r && !st) st = 123;
        };
        if (!nitems && !noempty && !repl) {
            for (int k = 0; k < ncmd; k++) av[k] = cmd[k];
            run(ncmd);
        } else if (repl) {
            const size_t rl = strlen(repl);
            for (int it = 0; it < nitems && !cancelled() && st != 127; it++) {
                int nmade = 0;
                for (int k = 0; k < ncmd && k < kMaxArgs; k++) {
                    if (!rl || !strstr(cmd[k], repl)) { av[k] = cmd[k]; continue; }
                    ShBuf s;
                    for (const char *p = cmd[k]; *p;) {
                        const char *m = strstr(p, repl);
                        if (!m) { buf_put(s, p, strlen(p)); break; }
                        buf_put(s, p, (size_t)(m - p));
                        buf_put(s, items[it], strlen(items[it]));
                        p = m + rl;
                    }
                    av[k] = s.p ? s.p : (char *)"";
                    if (s.p) made[nmade++] = s.p;
                }
                run(ncmd < kMaxArgs ? ncmd : kMaxArgs);
                for (int k = 0; k < nmade; k++) heap_caps_free(made[k]);
            }
        } else {
            const int room = kMaxArgs - ncmd;
            const int per = maxn > 0 && maxn < room ? (int)maxn : room;
            for (int it = 0; it < nitems && !cancelled() && st != 127; it += per) {
                int argc = 0;
                for (int k = 0; k < ncmd; k++) av[argc++] = cmd[k];
                for (int k = it; k < nitems && k < it + per; k++) av[argc++] = items[k];
                run(argc);
            }
        }
        heap_caps_free(buf);
        heap_caps_free(items);
        heap_caps_free(av);
        heap_caps_free(made);
        return cancelled() ? 130 : st;
    }
usage:
    errf(c, "xargs: option requires an argument\n");
    return 1;
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
        s_last_status = S->status;
        s_busy = false;
        s_done++;
    }
}

}  // namespace

// ================================================================= public

void sh_sink_write(const ShSink &s, const char *p, size_t n) {
    switch (s.k) {
        case SH_TTY:
            if (ShBuf *cb = s_capture) { if (cb->n + n <= kCaptureCap) buf_put(*cb, p, n); else cb->trunc = true; }
            else term_tty_write(p, n);
            break;
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
int sh_last_status(void) { return s_last_status.load(); }

bool sh_run(const char *line) {
    if (!s_task || s_busy.load()) return false;
    snprintf(S->line, sizeof S->line, "%s", line);
    s_cancel = false;
    s_busy = true;
    xTaskNotifyGive(s_task);
    return true;
}

void sh_interrupt(void) { if (s_busy.load()) s_cancel = true; }

// Drop ANSI escape sequences (a program may still colour its own output).
static size_t strip_ansi(char *s, size_t n) {
    size_t o = 0;
    for (size_t i = 0; i < n; i++) {
        if (s[i] == 0x1b && i + 1 < n && s[i + 1] == '[') {
            i += 2;
            while (i < n && !(s[i] >= '@' && s[i] <= '~')) i++;
            continue;
        }
        if (s[i] == '\r') continue;
        s[o++] = s[i];
    }
    s[o] = 0;
    return o;
}

bool sh_capturing(void) { return s_capture != nullptr; }
bool sh_cancelled(void) { return s_cancel.load(); }

int sh_exec_capture(const char *line, char *out, size_t cap, uint32_t timeout_ms, bool *truncated) {
    if (out && cap) out[0] = 0;
    if (truncated) *truncated = false;
    if (!line || !out || cap < 2 || !s_task) return -1;
    ShBuf *cb = (ShBuf *)heap_caps_calloc(1, sizeof(ShBuf), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!cb) return -1;
    new (cb) ShBuf();
    // Take the shell only when it is idle (a person may be typing in the Terminal).
    bool expected = false;
    if (!s_busy.compare_exchange_strong(expected, true)) { heap_caps_free(cb); return -1; }
    snprintf(S->line, sizeof S->line, "%s", line);
    s_cancel = false;
    s_capture = cb;
    const uint32_t done0 = s_done.load();
    xTaskNotifyGive(s_task);
    const int64_t t0 = esp_timer_get_time();
    bool timed_out = false;
    while (s_done.load() == done0) {
        if (!timed_out && (esp_timer_get_time() - t0) / 1000 > (int64_t)timeout_ms) { s_cancel = true; timed_out = true; }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    s_capture = nullptr;
    const size_t n = cb->p ? strip_ansi(cb->p, cb->n) : 0;
    const bool trunc = cb->trunc || n >= cap;
    snprintf(out, cap, "%.*s", (int)(n < cap ? n : cap - 1), cb->p ? cb->p : "");
    if (truncated) *truncated = trunc;
    heap_caps_free(cb->p);
    heap_caps_free(cb);
    return timed_out ? -2 : s_last_status.load();
}

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
