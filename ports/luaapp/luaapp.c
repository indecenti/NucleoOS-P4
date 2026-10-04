// luaapp.c — "Lua App": the NucleoOS engine that runs graphical Lua 5.4 apps.
//
// A store package with "engine": "luaapp" ships no module of its own. Its code is a bundle,
// apps/<id>/app.lpk (main.lua + modules + images, made by tools/lua_pack.py), whose SHA-256 is the
// first entry of the package manifest's "args" — the manifest is covered by the store signature,
// so the bundle is too. The engine downloads the bundle on first start (like the Doom engine does
// with its WADs), keeps it in the package's private folder, checks the hash on every start and
// fetches it again when an update changed it.
// From firmware "wasi" 1.3 the store installs the bundle with the signed package instead and the
// engine reads it from "/package/app.lpk" (the package folder, preopened read-only): no download,
// no "net" permission. The download stays as the fallback for older firmware.
//
// Started on its own (the "Lua App" tile) the engine runs launcher.lua: it lists the apps in
// /sdcard/home/lua/<name>/main.lua (or single .lua files there) — the developer's sideload path.
//
// The Lua side: the runtime prelude (lib/nvrt.lua) owns the frame loop, input dispatch, timers and
// network polling; this file gives it the raw host calls (module _nv), the renderer (gfx) and the
// bundle (require from the bundle, images). See docs/LUA_APPS.md.
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <wasi/api.h>

#include "lauxlib.h"
#include "lua.h"
#include "lualib.h"

#include "nucleo_sdk.h"
#include "gfx.h"
#include "sha256.h"
#include "gen/lua_libs.h"

#define ENGINE_VERSION "1.2.1"   // = apps/luaapp/manifest.json "version" (nv.engine; lua_pack writes "requires luaapp 1.2")
#ifndef STORE_BASE
#define STORE_BASE "https://indecenti.github.io/nucleoos-p4-store/apps/"
#endif
#define MAX_BUNDLE (1024 * 1024)       // the host's HTTP response cap (nv_wasm_net kHardMaxResp)

static bool g_it;                      // Italian UI strings
static char g_app[40] = "luaapp";
static char g_data[16] = "/";          // the package's private folder ("/" or "/appdata/")
static bool g_home;                    // "home" permission: /sdcard/home is "/"
static bool g_exit;

#define MINIZ_NO_TIME
#include "miniz_tinfl.c"     // compressed bundle entries (miniz 3.1.2, MIT)

// ---- the bundle ---------------------------------------------------------------------------------
typedef struct { char name[96]; uint32_t off, size; } res_t;
static uint8_t *g_pkg;
static size_t g_pkg_n;
static res_t *g_res;
static int g_nres;
static char g_dir[192];                // directory mode (sideloaded): files come from here
static bool g_dirmode;
static char g_main[64] = "main.lua";   // the file to run

static bool lpk_index(void) {
    free(g_res); g_res = NULL; g_nres = 0;
    if (g_pkg_n < 8 || memcmp(g_pkg, "LPK1", 4)) return false;
    uint32_t n = g_pkg[4] | g_pkg[5] << 8 | g_pkg[6] << 16 | (uint32_t)g_pkg[7] << 24;
    if (n > 4096) return false;
    g_res = calloc(n ? n : 1, sizeof *g_res);
    if (!g_res) return false;
    size_t p = 8;
    for (uint32_t i = 0; i < n; i++) {
        if (p + 2 > g_pkg_n) return false;
        unsigned nl = g_pkg[p] | g_pkg[p + 1] << 8;
        p += 2;
        if (!nl || nl >= sizeof g_res[0].name || p + nl + 4 > g_pkg_n) return false;
        memcpy(g_res[i].name, g_pkg + p, nl);
        p += nl;
        uint32_t sz = g_pkg[p] | g_pkg[p + 1] << 8 | g_pkg[p + 2] << 16 | (uint32_t)g_pkg[p + 3] << 24;
        p += 4;
        if (sz > g_pkg_n - p) return false;
        g_res[i].off = (uint32_t)p;
        g_res[i].size = sz;
        p += sz;
        g_nres = (int)i + 1;
    }
    return true;
}
static bool name_ok(const char *n) {
    if (!n || !*n || n[0] == '/' || strstr(n, "..") || strlen(n) > 90) return false;
    return true;
}
// A resource of the running app. *owned = the caller must free() it (directory mode).
static const uint8_t *res_get(const char *name, size_t *n, bool *owned) {
    *owned = false;
    if (!name_ok(name)) return NULL;
    if (g_dirmode) {
        char path[300];
        snprintf(path, sizeof path, "%s/%s", g_dir, name);
        FILE *f = fopen(path, "rb");
        if (!f) return NULL;
        fseek(f, 0, SEEK_END);
        long sz = ftell(f);
        fseek(f, 0, SEEK_SET);
        uint8_t *b = sz >= 0 && sz < 8 * 1024 * 1024 ? malloc((size_t)sz + 1) : NULL;
        if (b && fread(b, 1, (size_t)sz, f) != (size_t)sz) { free(b); b = NULL; }
        fclose(f);
        if (!b) return NULL;
        b[sz] = 0;
        *n = (size_t)sz;
        *owned = true;
        return b;
    }
    for (int i = 0; i < g_nres; i++)
        if (!strcmp(g_res[i].name, name)) {
            const uint8_t *d = g_pkg + g_res[i].off;
            size_t sz = g_res[i].size;
            // "LZD1" + u32 size + raw deflate: a compressed entry (tools/lua_pack.py, engine 1.1)
            if (sz >= 8 && !memcmp(d, "LZD1", 4)) {
                size_t raw = d[4] | d[5] << 8 | d[6] << 16 | (size_t)d[7] << 24;
                if (raw > 16 * 1024 * 1024) return NULL;
                uint8_t *b = malloc(raw + 1);
                if (!b) return NULL;
                size_t got = tinfl_decompress_mem_to_mem(b, raw, d + 8, sz - 8, 0);
                if (got != raw) { free(b); return NULL; }
                b[raw] = 0;
                *n = raw;
                *owned = true;
                return b;
            }
            *n = sz;
            return d;
        }
    // games written on Windows or macOS often get the case of a file name wrong
    for (int i = 0; i < g_nres; i++)
        if (!strcasecmp(g_res[i].name, name)) return res_get(g_res[i].name, n, owned);
    return NULL;
}

// ---- screen flush -------------------------------------------------------------------------------
static void flush(void) {
    if (g_dirty_y1 <= g_dirty_y0) return;
    int y0 = g_dirty_y0, y1 = g_dirty_y1;
    if (y0 < 0) y0 = 0;
    if (y1 > g_screen.h) y1 = g_screen.h;
    if (y1 > y0)
        nv_gfx_blit_raw(g_screen.px + (size_t)y0 * g_screen.w, (int32_t)((size_t)(y1 - y0) * g_screen.w * 2),
                        0, y0, g_screen.w, y1 - y0);
    g_dirty_y0 = 1 << 30;
    g_dirty_y1 = -1;
}

// ---- C-drawn screens (before Lua runs, errors) --------------------------------------------------
#define C_BG 0x101418
#define C_FG 0xE8ECF2
#define C_DIM 0x8A93A3
#define C_ACC 0x4C8DFF
#define C_ERR 0xFF6B6B
static void message(const char *title, const char *l1, const char *l2, int progress) {
    gfx_set_target(NULL);
    gfx_origin();
    g_alpha = 255;
    gfx_clear(C_BG);
    int W = g_screen.w, H = g_screen.h, s = H >= 400 ? 2 : 1;
    int ts = s == 2 ? 32 : 16, bs = s == 2 ? 20 : 12;
    int y = H / 2 - 60 * s / 2;
    gfx_text((W - gfx_text_width(title, ts)) / 2.0f, y, title, ts, C_FG);
    y += ts + 16 * s;
    if (l1) { gfx_text((W - gfx_text_width(l1, bs)) / 2.0f, y, l1, bs, C_DIM); y += bs + 8 * s; }
    if (l2) { gfx_text((W - gfx_text_width(l2, bs)) / 2.0f, y, l2, bs, C_DIM); y += bs + 8 * s; }
    if (progress >= 0) {
        int bw = W / 2, bx = (W - bw) / 2;
        y += 12 * s;
        gfx_rect(bx, y, bw, 8 * s, 0x2A313C, 4 * s);
        gfx_rect(bx, y, bw * (progress > 100 ? 100 : progress) / 100.0f, 8 * s, C_ACC, 4 * s);
    }
    flush();
}
// Wait for a tap / key / back; returns 1 = tap or key (retry), 0 = back or close.
static int wait_input(void) {
    int phase = 0;                         // 0 wait for release, 1 wait for press, 2 wait for release
    for (;;) {
        if (!nv_gfx_present() || nv_gfx_back()) return 0;
        int down = nv_touch_count() > 0 || (nv_gfx_input_raw() >> 24 & 1);
        if (phase == 0 && !down) phase = 1;
        else if (phase == 1 && down) phase = 2;
        else if (phase == 2 && !down) return 1;
        if (phase == 1 && (nv_gfx_pad() & 0xFFF)) return 1;
    }
}

// ---- bundle download ----------------------------------------------------------------------------
static bool file_sha(const uint8_t *d, size_t n, const char *want) {
    sha256_t s;
    char hex[65];
    sha256_init(&s);
    sha256_update(&s, d, n);
    sha256_hex(&s, hex);
    return !strcmp(hex, want);
}
static uint8_t *read_all(const char *path, size_t *n) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *b = sz > 0 && sz <= MAX_BUNDLE ? malloc((size_t)sz) : NULL;
    if (b && fread(b, 1, (size_t)sz, f) != (size_t)sz) { free(b); b = NULL; }
    fclose(f);
    if (b) *n = (size_t)sz;
    return b;
}
// -1 closed by the user, 0 failed, 1 ok (g_pkg holds it)
static int download(const char *url, const char *sha) {
    char spec[400];
    snprintf(spec, sizeof spec, "{\"url\":\"%s\",\"timeout\":30000,\"max\":%d}", url, MAX_BUNDLE);
    const char *t = g_it ? "Scarico l'app..." : "Downloading the app...";
    message(t, NULL, NULL, 0);
    int h = nv_http_req(spec, NULL, 0);
    if (h < 0) return 0;
    uint8_t *buf = malloc(MAX_BUNDLE);
    size_t got = 0;
    int st;
    if (!buf) { nv_http_close(h); return 0; }
    for (;;) {
        if (!nv_gfx_present() || nv_gfx_back()) { nv_http_close(h); free(buf); return -1; }
        st = nv_http_state(h);
        if (st != 0) break;
        message(t, NULL, NULL, (int)(nv_millis() / 40 % 100));
    }
    if (st < 0 || nv_http_status(h) != 200) { nv_http_close(h); free(buf); return 0; }
    for (;;) {
        int n = nv_http_read(h, buf + got, (uint32_t)(MAX_BUNDLE - got));
        if (n <= 0) break;
        got += (size_t)n;
        if (got >= MAX_BUNDLE) break;
    }
    nv_http_close(h);
    if (!got || !file_sha(buf, got, sha)) { free(buf); return 0; }
    free(g_pkg);
    g_pkg = buf;
    g_pkg_n = got;
    return 1;
}
// The package's bundle: the cached copy when its hash matches the manifest, else a fresh download.
// Firmware with "wasi" 1.3 installs the bundle with the signed package and preopens the package
// folder read-only as "/package": that copy is used (no network, nothing cached). Older firmware,
// or a package installed before, falls back to the private cache and then to the download (which
// needs the "net" permission).
static int obtain_bundle(const char *sha, const char *url_override) {
    char cache[64];
    snprintf(cache, sizeof cache, "%s.app.lpk", g_data);
    size_t n = 0;
    uint8_t *b = read_all("/package/app.lpk", &n);
    if (b && file_sha(b, n, sha)) {
        g_pkg = b; g_pkg_n = n;
        remove(cache);                     // a download from an older install: not needed any more
        return 1;
    }
    free(b);
    n = 0;
    b = read_all(cache, &n);
    if (b && file_sha(b, n, sha)) { g_pkg = b; g_pkg_n = n; return 1; }
    free(b);
    char url[256];
    if (url_override && !strncmp(url_override, "http", 4))
        snprintf(url, sizeof url, "%s", url_override);
    else
        snprintf(url, sizeof url, STORE_BASE "%s/app.lpk", g_app);
    for (;;) {
        int r = download(url, sha);
        if (r < 0) return 0;
        if (r > 0) {
            FILE *f = fopen(cache, "wb");
            if (f) {
                bool ok = fwrite(g_pkg, 1, g_pkg_n, f) == g_pkg_n;
                if (fclose(f) != 0 || !ok) remove(cache);
            }
            return 1;
        }
        message(g_it ? "Download non riuscito" : "Download failed",
                g_it ? "Il primo avvio scarica l'app: serve il Wi-Fi." : "The first start downloads the app: Wi-Fi is needed.",
                g_it ? "Tocca per riprovare, indietro per uscire." : "Tap to retry, back to leave.", -1);
        if (!wait_input()) return 0;
    }
}

// ---- Lua: gfx -----------------------------------------------------------------------------------
#define NUM(i) ((float)luaL_checknumber(L, i))
#define OPTN(i, d) ((float)luaL_optnumber(L, i, d))
#define COL(i) ((uint32_t)luaL_checkinteger(L, i))
#define SURF "luaapp.surface"

static surface_t *check_surf(lua_State *L, int i) {
    surface_t **p = luaL_checkudata(L, i, SURF);
    if (!*p) luaL_error(L, "surface was freed");
    return *p;
}
static int l_clear(lua_State *L) {
    if (lua_isnoneornil(L, 1)) gfx_clear_transparent();
    else gfx_clear(COL(1));
    return 0;
}
static int l_rect(lua_State *L) { gfx_rect(NUM(1), NUM(2), NUM(3), NUM(4), COL(5), OPTN(6, 0)); return 0; }
static int l_frame(lua_State *L) { gfx_frame(NUM(1), NUM(2), NUM(3), NUM(4), COL(5), OPTN(6, 1), OPTN(7, 0)); return 0; }
static int l_circle(lua_State *L) { gfx_circle(NUM(1), NUM(2), NUM(3), COL(4)); return 0; }
static int l_ring(lua_State *L) { gfx_ring(NUM(1), NUM(2), NUM(3), COL(4), OPTN(5, 1)); return 0; }
static int l_arc(lua_State *L) { gfx_arc(NUM(1), NUM(2), NUM(3), NUM(4), NUM(5), COL(6), OPTN(7, 0)); return 0; }
static int l_line(lua_State *L) { gfx_line(NUM(1), NUM(2), NUM(3), NUM(4), COL(5), OPTN(6, 1)); return 0; }
static int l_pixel(lua_State *L) { gfx_pixel(NUM(1), NUM(2), COL(3)); return 0; }
// gfx.get_pixel(x, y) -> color, alpha (of the current target, in target pixels)
static int l_get_pixel(lua_State *L) {
    int x = (int)NUM(1), y = (int)NUM(2);
    lua_pushinteger(L, gfx_get_pixel(x, y));
    int a = 255;
    if (g_tgt->a && x >= 0 && y >= 0 && x < g_tgt->w && y < g_tgt->h) a = g_tgt->a[(size_t)y * g_tgt->w + x];
    lua_pushinteger(L, a);
    return 2;
}
static int l_tri(lua_State *L) {
    float p[6];
    for (int i = 0; i < 6; i++) p[i] = NUM(i + 1);
    gfx_poly(p, 3, COL(7));
    return 0;
}
static int l_poly(lua_State *L) {
    luaL_checktype(L, 1, LUA_TTABLE);
    static float p[1024];
    int n = (int)luaL_len(L, 1);
    if (n > 1024) n = 1024;
    for (int i = 0; i < n; i++) { lua_rawgeti(L, 1, i + 1); p[i] = (float)lua_tonumber(L, -1); lua_pop(L, 1); }
    gfx_poly(p, n / 2, COL(2));
    return 0;
}
static int align_of(lua_State *L, int i) {
    if (lua_type(L, i) == LUA_TSTRING) {
        const char *a = lua_tostring(L, i);
        return a[0] == 'c' ? 1 : a[0] == 'r' ? 2 : 0;
    }
    return (int)luaL_optinteger(L, i, 0);
}
// gfx.text(x, y, s, size=16, color=white, align="left") -> width
static int l_text(lua_State *L) {
    lua_settop(L, 6);                     // luaL_tolstring pushes: keep the optional args below it
    float x = NUM(1), y = NUM(2);
    const char *s = luaL_tolstring(L, 3, NULL);
    float size = OPTN(4, 16);
    uint32_t c = (uint32_t)luaL_optinteger(L, 5, 0xFFFFFF);
    int al = align_of(L, 6), w = 0;
    float lh = (float)gfx_font_line(size);
    for (const char *p = s;;) {           // multi-line: one row per '\n'
        int lw = gfx_text_width(p, size);
        if (lw > w) w = lw;
        gfx_text(al == 1 ? x - lw / 2.0f : al == 2 ? x - lw : x, y, p, size, c);
        const char *nl = strchr(p, '\n');
        if (!nl) break;
        p = nl + 1;
        y += lh;
    }
    lua_pushinteger(L, w);
    return 1;
}
static int l_text_width(lua_State *L) {
    lua_settop(L, 2);
    const char *s = luaL_tolstring(L, 1, NULL);
    lua_pushinteger(L, gfx_text_width(s, OPTN(2, 16)));
    return 1;
}
static int l_font_height(lua_State *L) {
    lua_pushinteger(L, gfx_font_line(OPTN(1, 16)));
    lua_pushinteger(L, gfx_font_ascent(OPTN(1, 16)));
    return 2;
}
static int l_alpha(lua_State *L) {
    if (!lua_isnoneornil(L, 1)) {
        int a = (int)luaL_checknumber(L, 1);
        g_alpha = a < 0 ? 0 : a > 255 ? 255 : a;
    }
    lua_pushinteger(L, g_alpha);
    return 1;
}
static int l_clip(lua_State *L) {
    if (lua_isnoneornil(L, 1)) gfx_reset_clip();
    else gfx_clip((int)NUM(1), (int)NUM(2), (int)NUM(3), (int)NUM(4));
    return 0;
}
static int l_width(lua_State *L) { lua_pushinteger(L, g_tgt->w); return 1; }
static int l_height(lua_State *L) { lua_pushinteger(L, g_tgt->h); return 1; }
static int l_rgb(lua_State *L) {
    int r = (int)NUM(1), g = (int)NUM(2), b = (int)NUM(3);
    #define CL(v) ((v) < 0 ? 0 : (v) > 255 ? 255 : (v))
    lua_pushinteger(L, CL(r) << 16 | CL(g) << 8 | CL(b));
    return 1;
}
static int l_push(lua_State *L) { if (!gfx_push()) return luaL_error(L, "transform stack overflow (more than 32 push without pop)"); return 0; }
static int l_pop(lua_State *L) { (void)L; gfx_pop(); return 0; }
static int l_translate(lua_State *L) { gfx_translate(NUM(1), NUM(2)); return 0; }
static int l_scale(lua_State *L) { float s = NUM(1); gfx_scale(s, OPTN(2, s)); return 0; }
static int l_origin(lua_State *L) { (void)L; gfx_identity(); return 0; }   // keeps the push/pop stack

static void push_surf(lua_State *L, surface_t *s) {
    surface_t **p = lua_newuserdatauv(L, sizeof *p, 0);
    *p = s;
    luaL_setmetatable(L, SURF);
}
// gfx.surface(w, h) -> an off-screen canvas with transparency
static int l_surface(lua_State *L) {
    surface_t *s = surface_new((int)NUM(1), (int)NUM(2), true);
    if (!s) return luaL_error(L, "out of memory for a %dx%d surface", (int)NUM(1), (int)NUM(2));
    push_surf(L, s);
    return 1;
}
// gfx.image(name) -> surface | nil, err   (from the bundle: "img/logo.png" is packed as LIMG)
static int l_load_image(lua_State *L) {
    const char *name = luaL_checkstring(L, 1);
    size_t n;
    bool owned;
    const uint8_t *d = res_get(name, &n, &owned);
    if (!d) { lua_pushnil(L); lua_pushfstring(L, "no image %s", name); return 2; }
    surface_t *s = surface_from_limg(d, n);
    if (owned) free((void *)d);
    if (!s) { lua_pushnil(L); lua_pushfstring(L, "%s: not a packed image (use tools/lua_pack.py)", name); return 2; }
    push_surf(L, s);
    return 1;
}
// gfx.draw(surface, x, y [, w, h [, qx, qy, qw, qh [, rot, ox, oy]]]): the (qx,qy,qw,qh) part
// (default: all) scaled to w x h (negative = mirrored), rotated by rot radians around (x,y) after
// moving it by -(ox,oy) (destination units)
static int l_draw(lua_State *L) {
    surface_t *s = check_surf(L, 1);
    int qx = (int)OPTN(6, 0), qy = (int)OPTN(7, 0), qw = (int)OPTN(8, (float)s->w), qh = (int)OPTN(9, (float)s->h);
    gfx_draw_ex(s, NUM(2), NUM(3), OPTN(4, (float)qw), OPTN(5, (float)qh), qx, qy, qw, qh,
                OPTN(10, 0), OPTN(11, 0), OPTN(12, 0));
    return 0;
}
// gfx.tint(color | nil): images drawn after it are multiplied by color (nil = white = off)
static int l_tint(lua_State *L) { g_tint = lua_isnoneornil(L, 1) ? 0xFFFFFF : COL(1); return 0; }
static int l_rotate(lua_State *L) { gfx_rotate(NUM(1)); return 0; }
static int l_identity(lua_State *L) { (void)L; gfx_identity(); return 0; }
// gfx.blend("alpha" | "add" | "subtract" | "multiply" | "screen" | "replace" | "lighten" | "darken")
static int l_blend(lua_State *L) {
    static const char *const modes[] = { "alpha", "add", "subtract", "multiply", "screen", "replace", "lighten", "darken", NULL };
    g_blend = luaL_checkoption(L, 1, "alpha", modes);
    return 0;
}
static int l_shear(lua_State *L) { gfx_shear(NUM(1), OPTN(2, 0)); return 0; }
// gfx.target(surface | nil)
static int l_target(lua_State *L) {
    gfx_set_target(lua_isnoneornil(L, 1) ? NULL : check_surf(L, 1));
    return 0;
}
static int s_size(lua_State *L) { surface_t *s = check_surf(L, 1); lua_pushinteger(L, s->w); lua_pushinteger(L, s->h); return 2; }
static int s_free(lua_State *L) {
    surface_t **p = luaL_checkudata(L, 1, SURF);
    surface_free(*p);
    *p = NULL;
    return 0;
}

// ---- Lua: _nv (raw host calls; lib/nvrt.lua builds the friendly API on top) ----------------------
// The host clock is 32-bit (wraps after ~49.7 days): extend it, so nv.clock and timers never stall.
static int n_millis(lua_State *L) {
    static uint32_t last, high;
    const uint32_t now = (uint32_t)nv_millis();
    if (now < last) high++;
    last = now;
    lua_pushinteger(L, (lua_Integer)(((uint64_t)high << 32) | now));
    return 1;
}
static int n_time(lua_State *L) { lua_pushinteger(L, (lua_Integer)nv_time_unix()); return 1; }
static int n_lang(lua_State *L) { char b[8] = {0}; nv_lang(b, sizeof b - 1); lua_pushstring(L, b); return 1; }
static int n_rand(lua_State *L) { lua_pushinteger(L, (uint32_t)nv_rand()); return 1; }
static int n_toast(lua_State *L) { lua_settop(L, 2); nv_toast((int)luaL_optinteger(L, 2, 0), luaL_tolstring(L, 1, NULL)); return 0; }
static int n_log(lua_State *L) { lua_settop(L, 2); nv_log((int)luaL_optinteger(L, 2, 1), luaL_tolstring(L, 1, NULL)); return 0; }
static int n_tone(lua_State *L) { nv_gfx_tone((int)NUM(1), (int)OPTN(2, 100)); return 0; }
static int n_sound(lua_State *L) { nv_sound(luaL_checkstring(L, 1)); return 0; }
static int n_speak(lua_State *L) { nv_speak(luaL_checkstring(L, 1), luaL_optstring(L, 2, g_it ? "it" : "en")); return 0; }
static int n_backlight(lua_State *L) { nv_backlight((int)NUM(1)); return 0; }
static int n_exit(lua_State *L) { (void)L; g_exit = true; return 0; }
static int n_back(lua_State *L) { lua_pushinteger(L, nv_gfx_back()); return 1; }
static int n_pad(lua_State *L) { lua_pushinteger(L, nv_gfx_pad()); return 1; }
// touches() -> n, x1, y1, x2, y2, ...
static int n_touches(lua_State *L) {
    int n = nv_touch_count(), k = 0;
    if (n > 5) n = 5;
    lua_pushinteger(L, 0);                // the count, fixed below: a touch that cannot be read is skipped
    for (int i = 0; i < n; i++) {
        int x, y;
        if (!nv_touch_at(i, &x, &y)) continue;
        lua_pushinteger(L, x); lua_pushinteger(L, y);
        k++;
    }
    if (k == 0) {                         // single-touch fallback (and the OS pointer)
        int x, y;
        if (nv_touch(&x, &y)) { lua_pushinteger(L, x); lua_pushinteger(L, y); k = 1; }
    }
    lua_pushinteger(L, k);                // the real count, as many pairs as follow it
    lua_replace(L, -(2 * k) - 2);
    return 1 + 2 * k;
}
// kbd() -> nil (no keyboard) | modifiers, usage1, usage2, ...
static int n_kbd(lua_State *L) {
    uint8_t b[8] = {0};
    int n = nv_kbd_state(b, sizeof b);
    if (n < 0) return 0;
    lua_pushinteger(L, b[0]);
    for (int i = 0; i < n && i < 6; i++) lua_pushinteger(L, b[1 + i]);
    return 1 + (n < 6 ? n : 6);
}
// pad_state(i) -> nil | buttons, lx, ly, rx, ry
static int n_pad_state(lua_State *L) {
    nv_pad_state_t st;
    if (nv_pad_state((int)luaL_checkinteger(L, 1), &st, sizeof st) <= 0) return 0;
    lua_pushinteger(L, st.buttons);
    lua_pushinteger(L, st.lx); lua_pushinteger(L, st.ly);
    lua_pushinteger(L, st.rx); lua_pushinteger(L, st.ry);
    return 5;
}
static int n_pad_count(lua_State *L) { lua_pushinteger(L, nv_pad_count()); return 1; }
static int n_mouse(lua_State *L) {
    nv_mouse_t m;
    if (nv_mouse_read(&m, sizeof m) != 1) return 0;
    lua_pushinteger(L, m.dx); lua_pushinteger(L, m.dy); lua_pushinteger(L, m.wheel); lua_pushinteger(L, m.buttons);
    return 4;
}
// network: handles are the host's (at most 4 open)
static int n_http_req(lua_State *L) {
    size_t bl = 0;
    const char *spec = luaL_checkstring(L, 1);
    const char *body = luaL_optlstring(L, 2, NULL, &bl);
    lua_pushinteger(L, nv_http_req(spec, body, (uint32_t)bl));
    return 1;
}
static int n_http_state(lua_State *L) { lua_pushinteger(L, nv_http_state((int)luaL_checkinteger(L, 1))); return 1; }
static int n_http_status(lua_State *L) { lua_pushinteger(L, nv_http_status((int)luaL_checkinteger(L, 1))); return 1; }
static int n_http_body(lua_State *L) {       // the whole (finished) body
    int h = (int)luaL_checkinteger(L, 1);
    luaL_Buffer b;
    luaL_buffinit(L, &b);
    for (;;) {
        char *p = luaL_prepbuffsize(&b, 8192);
        int n = nv_http_read(h, p, 8192);
        if (n <= 0) break;
        luaL_addsize(&b, (size_t)n);
    }
    luaL_pushresult(&b);
    return 1;
}
static int n_http_close(lua_State *L) { nv_http_close((int)luaL_checkinteger(L, 1)); return 0; }
static int n_ws_open(lua_State *L) { lua_pushinteger(L, nv_ws_open(luaL_checkstring(L, 1), luaL_optstring(L, 2, NULL))); return 1; }
static int n_ws_state(lua_State *L) { lua_pushinteger(L, nv_ws_state((int)luaL_checkinteger(L, 1))); return 1; }
static int n_ws_send(lua_State *L) {
    size_t n;
    const char *s = luaL_checklstring(L, 2, &n);
    lua_pushinteger(L, nv_ws_send((int)luaL_checkinteger(L, 1), s, (uint32_t)n, lua_toboolean(L, 3)));
    return 1;
}
static char g_rx[32768];
static int n_ws_recv(lua_State *L) {
    int n = nv_ws_recv((int)luaL_checkinteger(L, 1), g_rx, sizeof g_rx);
    if (n <= 0) return 0;
    lua_pushlstring(L, g_rx, (size_t)(n > (int)sizeof g_rx ? (int)sizeof g_rx : n));
    return 1;
}
static int n_ws_close(lua_State *L) { nv_ws_close((int)luaL_checkinteger(L, 1)); return 0; }
static int n_mqtt_sub(lua_State *L) { lua_pushinteger(L, nv_mqtt_sub(luaL_checkstring(L, 1))); return 1; }
static int n_mqtt_pub(lua_State *L) {
    size_t n;
    const char *p = luaL_checklstring(L, 2, &n);
    lua_pushinteger(L, nv_mqtt_pub(luaL_checkstring(L, 1), p, (uint32_t)n, lua_toboolean(L, 3)));
    return 1;
}
static int n_mqtt_recv(lua_State *L) {
    static char topic[256];
    int n = nv_mqtt_recv(topic, sizeof topic, g_rx, 8192);
    if (n < 0) return 0;
    lua_pushstring(L, topic);
    lua_pushlstring(L, g_rx, (size_t)(n > 8192 ? 8192 : n));
    return 2;
}
static int n_ha_available(lua_State *L) { lua_pushboolean(L, nv_ha_available() > 0); return 1; }
static int n_ha_req(lua_State *L) {
    size_t bl = 0;
    const char *body = luaL_optlstring(L, 3, NULL, &bl);
    lua_pushinteger(L, nv_ha_req(luaL_checkstring(L, 1), luaL_checkstring(L, 2), body, (uint32_t)bl));
    return 1;
}
static int n_ha_ws(lua_State *L) { lua_pushinteger(L, nv_ha_ws()); return 1; }
static int n_mdns(lua_State *L) { lua_pushinteger(L, nv_mdns_browse(luaL_checkstring(L, 1), luaL_optstring(L, 2, "_tcp"))); return 1; }
// res(name) -> contents | nil   (a file of the running app's bundle)
extern int nv_lua51_numbers;   // gen/lobject.c: integral floats print as "10", not "10.0"
static int n_lua51_numbers(lua_State *L) { nv_lua51_numbers = lua_toboolean(L, 1); return 0; }
static int n_res(lua_State *L) {
    size_t n;
    bool owned;
    const uint8_t *d = res_get(luaL_checkstring(L, 1), &n, &owned);
    if (!d) return 0;
    lua_pushlstring(L, (const char *)d, n);
    if (owned) free((void *)d);
    return 1;
}
static int n_res_list(lua_State *L) {
    lua_newtable(L);
    for (int i = 0; i < g_nres; i++) { lua_pushstring(L, g_res[i].name); lua_rawseti(L, -2, i + 1); }
    return 1;
}
// ls(path) -> { {name=, dir=bool}, ... } | nil   (the launcher's folder scan)
static int n_ls(lua_State *L) {
    DIR *d = opendir(luaL_checkstring(L, 1));
    if (!d) return 0;
    lua_newtable(L);
    int i = 0;
    struct dirent *e;
    while ((e = readdir(d)) && i < 512) {
        if (e->d_name[0] == '.') continue;
        lua_newtable(L);
        lua_pushstring(L, e->d_name); lua_setfield(L, -2, "name");
        lua_pushboolean(L, e->d_type == DT_DIR); lua_setfield(L, -2, "dir");
        lua_rawseti(L, -2, ++i);
    }
    closedir(d);
    return 1;
}
static char g_next[192];                    // launcher: the folder to run next
static int n_launch(lua_State *L) {
    snprintf(g_next, sizeof g_next, "%s", luaL_checkstring(L, 1));
    g_exit = true;
    return 0;
}
static int n_info(lua_State *L) {
    lua_newtable(L);
    lua_pushstring(L, g_app); lua_setfield(L, -2, "id");
    lua_pushstring(L, g_data); lua_setfield(L, -2, "data");
    lua_pushboolean(L, g_home); lua_setfield(L, -2, "home");
    lua_pushstring(L, ENGINE_VERSION); lua_setfield(L, -2, "engine");
    lua_pushboolean(L, g_dirmode); lua_setfield(L, -2, "sideloaded");
    lua_pushstring(L, g_dir); lua_setfield(L, -2, "dir");
    lua_pushstring(L, g_main); lua_setfield(L, -2, "main");
    return 1;
}

// require() from the bundle: "a.b" -> "a/b.lua" (or "a/b/init.lua")
static int searcher(lua_State *L) {
    const char *mod = luaL_checkstring(L, 1);
    char path[128], alt[140];
    size_t k = 0;
    for (const char *p = mod; *p && k < sizeof path - 5; p++) path[k++] = *p == '.' ? '/' : *p;
    path[k] = 0;
    snprintf(alt, sizeof alt, "%s/init.lua", path);
    strcat(path, ".lua");
    const char *names[2] = { path, alt };
    for (int i = 0; i < 2; i++) {
        size_t n;
        bool owned;
        const uint8_t *d = res_get(names[i], &n, &owned);
        if (!d) continue;
        char chunk[150];
        snprintf(chunk, sizeof chunk, "@%s", names[i]);
        int rc = luaL_loadbufferx(L, (const char *)d, n, chunk, "t");
        if (owned) free((void *)d);
        if (rc != LUA_OK) return lua_error(L);
        lua_pushstring(L, names[i]);
        return 2;
    }
    lua_pushfstring(L, "\n\tno file '%s' in the app", path);
    return 1;
}
// the engine's own Lua libraries (lib/*.lua, compiled in)
static int load_lib(lua_State *L, const char *name) {
    for (int i = 0; i < LUA_LIBS_N; i++) {
        if (strcmp(lua_libs[i].name, name)) continue;
        char chunk[48];
        snprintf(chunk, sizeof chunk, "@engine/%s.lua", name);
        if (luaL_loadbufferx(L, lua_libs[i].src, lua_libs[i].len, chunk, "t") != LUA_OK) return 0;
        return 1;
    }
    return 0;
}
static int engine_searcher(lua_State *L) {
    const char *mod = luaL_checkstring(L, 1);
    if (load_lib(L, mod)) { lua_pushstring(L, mod); return 2; }
    lua_pushfstring(L, "\n\tno engine library '%s'", mod);
    return 1;
}

static const luaL_Reg k_gfx[] = {
    {"clear", l_clear}, {"rect", l_rect}, {"frame", l_frame}, {"circle", l_circle}, {"ring", l_ring},
    {"arc", l_arc}, {"line", l_line}, {"tri", l_tri}, {"poly", l_poly}, {"pixel", l_pixel},
    {"get_pixel", l_get_pixel}, {"text", l_text}, {"text_width", l_text_width},
    {"font_height", l_font_height}, {"alpha", l_alpha}, {"clip", l_clip}, {"width", l_width},
    {"height", l_height}, {"rgb", l_rgb}, {"push", l_push}, {"pop", l_pop},
    {"translate", l_translate}, {"scale", l_scale}, {"origin", l_origin}, {"surface", l_surface},
    {"image", l_load_image}, {"draw", l_draw}, {"target", l_target}, {"tint", l_tint},
    {"rotate", l_rotate}, {"shear", l_shear}, {"identity", l_identity}, {"blend", l_blend}, {NULL, NULL}};
static const luaL_Reg k_nv[] = {
    {"millis", n_millis}, {"time", n_time}, {"lang", n_lang}, {"rand", n_rand}, {"toast", n_toast},
    {"log", n_log}, {"tone", n_tone}, {"sound", n_sound}, {"speak", n_speak},
    {"backlight", n_backlight}, {"exit", n_exit}, {"back", n_back}, {"pad", n_pad},
    {"touches", n_touches}, {"kbd", n_kbd}, {"pad_state", n_pad_state}, {"pad_count", n_pad_count},
    {"mouse", n_mouse}, {"http_req", n_http_req}, {"http_state", n_http_state},
    {"http_status", n_http_status}, {"http_body", n_http_body}, {"http_close", n_http_close},
    {"ws_open", n_ws_open}, {"ws_state", n_ws_state}, {"ws_send", n_ws_send}, {"ws_recv", n_ws_recv},
    {"ws_close", n_ws_close}, {"mqtt_sub", n_mqtt_sub}, {"mqtt_pub", n_mqtt_pub},
    {"mqtt_recv", n_mqtt_recv}, {"ha_available", n_ha_available}, {"ha_req", n_ha_req},
    {"ha_ws", n_ha_ws}, {"mdns", n_mdns}, {"res", n_res}, {"res_list", n_res_list}, {"lua51_numbers", n_lua51_numbers},
    {"launch", n_launch}, {"ls", n_ls}, {"info", n_info}, {NULL, NULL}};

// ---- running a Lua app --------------------------------------------------------------------------
static char g_err[2048];
static int traceback(lua_State *L) {
    const char *m = lua_tostring(L, 1);
    luaL_traceback(L, L, m ? m : "(error object is not a string)", 1);
    return 1;
}
static bool pcall_tb(lua_State *L, int nargs) {
    int base = lua_gettop(L) - nargs;
    lua_pushcfunction(L, traceback);
    lua_insert(L, base);
    int rc = lua_pcall(L, nargs, 0, base);
    lua_remove(L, base);
    if (rc != LUA_OK) {
        snprintf(g_err, sizeof g_err, "%s", lua_tostring(L, -1) ? lua_tostring(L, -1) : "error");
        lua_pop(L, 1);
        return false;
    }
    return true;
}
static void error_screen(void) {
    gfx_set_target(NULL);
    gfx_origin();
    g_alpha = 255;
    gfx_clear(0x1A0E10);
    int s = g_screen.h >= 400 ? 2 : 1, ts = 14 * s / 2 + 6, y = 12 * s;
    if (ts > 16) ts = 16;
    gfx_text(12 * s, y, g_it ? "Errore nello script Lua" : "Lua script error", 16 + 8 * (s - 1), C_ERR);
    y += 32 * s;
    // wrap the message to the screen
    char line[160];
    const char *p = g_err;
    while (*p && y < g_screen.h - 40) {
        size_t k = 0;
        while (p[k] && p[k] != '\n' && k < sizeof line - 1) {
            line[k] = p[k] == '\t' ? ' ' : p[k];
            line[k + 1] = 0;
            if (gfx_text_width(line, ts) > g_screen.w - 24 * s) break;
            k++;
        }
        line[k] = 0;
        gfx_text(12 * s, y, line, ts, C_FG);
        y += gfx_font_line(ts);
        p += k;
        if (*p == '\n') p++;
    }
    gfx_text(12 * s, g_screen.h - 28 * s, g_it ? "Indietro per uscire" : "Back to leave", 12 + 2 * s, C_DIM);
    flush();
    nv_log(NV_LOG_ERROR, g_err);
}

// One app from start to finish. main = the bundle file to run ("main.lua"), or NULL with
// lib_main = an engine library (the launcher).
static void run_app(const char *lib_main) {
    lua_State *L = luaL_newstate();
    if (!L) { message("Out of memory", NULL, NULL, -1); wait_input(); return; }
    nv_lua51_numbers = 0;
    luaL_openlibs(L);
    luaL_newmetatable(L, SURF);
    lua_pushcfunction(L, s_free); lua_setfield(L, -2, "__gc");
    lua_newtable(L);
    lua_pushcfunction(L, s_size); lua_setfield(L, -2, "size");
    lua_pushcfunction(L, s_free); lua_setfield(L, -2, "free");
    lua_setfield(L, -2, "__index");
    lua_pop(L, 1);
    luaL_newlib(L, k_gfx); lua_setglobal(L, "gfx");
    luaL_newlib(L, k_nv); lua_setglobal(L, "_nv");
    // searchers: engine libraries first, then the app's own files
    lua_getglobal(L, "package");
    lua_getfield(L, -1, "searchers");
    lua_pushcfunction(L, engine_searcher); lua_rawseti(L, -2, 2);
    lua_pushcfunction(L, searcher); lua_rawseti(L, -2, 3);
    lua_pushnil(L); lua_rawseti(L, -2, 4);
    lua_pop(L, 2);

    gfx_set_target(NULL);
    gfx_origin();
    g_alpha = 255;
    g_exit = false;
    if (g_home && g_dirmode) remove("/lua/.last_error");   // a fresh run: no stale error
    bool ok = load_lib(L, "nvrt") && pcall_tb(L, 0);
    if (!ok && !g_err[0]) snprintf(g_err, sizeof g_err, "engine runtime failed to load");
    if (ok) {                               // nv._start(main): runs main.lua and the app's init
        lua_getglobal(L, "nv");
        lua_getfield(L, -1, "_start");
        lua_remove(L, -2);
        lua_pushstring(L, lib_main ? lib_main : "");
        ok = pcall_tb(L, 1);
    }
    while (ok && !g_exit) {
        lua_getglobal(L, "nv");
        lua_getfield(L, -1, "_frame");
        lua_remove(L, -2);
        ok = pcall_tb(L, 0);
        gfx_set_target(NULL);
        gfx_origin();                       // a frame never leaks its transform stack
        g_tint = 0xFFFFFF;
        g_blend = GFX_BLEND_ALPHA;
        flush();
        if (g_exit) break;
        if (!nv_gfx_present()) { g_exit = true; g_next[0] = 0; break; }
    }
    if (!ok) {
        if (g_home && g_dirmode) {          // for tools and ANIMA: the error, readable without the screen
            FILE *f = fopen("/lua/.last_error", "w");
            if (f) { fprintf(f, "%s/%s\n%s\n", g_dir, g_main, g_err); fclose(f); }
        }
        error_screen();
        while (nv_gfx_present() && !nv_gfx_back()) {}
        g_next[0] = 0;
    } else {
        lua_getglobal(L, "nv");
        lua_getfield(L, -1, "_stop");
        lua_remove(L, -2);
        pcall_tb(L, 0);
    }
    lua_close(L);
    g_err[0] = 0;
}

// argv of this run (WASI args: the manifest "args" of the package)
static int nv_args(char ***out) {
    __wasi_size_t argc = 0, sz = 0;
    if (__wasi_args_sizes_get(&argc, &sz) != 0 || argc == 0) return 0;
    char **argv = calloc(argc + 1, sizeof *argv);
    char *buf = malloc(sz ? sz : 1);
    if (!argv || !buf || __wasi_args_get((uint8_t **)argv, (uint8_t *)buf) != 0) { free(argv); free(buf); return 0; }
    *out = argv;
    return (int)argc;
}

static bool is_dir(const char *p) { struct stat st; return stat(p, &st) == 0 && S_ISDIR(st.st_mode); }

NV_EXPORT("run")
void run(void) {
    char lang[8] = {0};
    nv_lang(lang, sizeof lang - 1);
    g_it = lang[0] == 'i' && lang[1] == 't';
    g_screen.w = nv_gfx_width();
    g_screen.h = nv_gfx_height();
    g_screen.px = calloc((size_t)g_screen.w * g_screen.h, 2);
    if (!g_screen.px) return;
    nv_gfx_persist(1);
    gfx_set_target(NULL);
    g_home = is_dir("/appdata");
    if (g_home) snprintf(g_data, sizeof g_data, "/appdata/");
    const char *id = getenv("NUCLEO_APP");
    if (id && *id) snprintf(g_app, sizeof g_app, "%s", id);
    // Reading and unpacking the bundle and compiling the game takes seconds on a big LOVE port:
    // say so at once instead of a black screen (the game's first frame replaces it).
    message(g_it ? "Caricamento..." : "Loading...", NULL, NULL, -1);

    if (strcmp(g_app, "luaapp") != 0) {
        // a store package: argv[1] = the bundle's sha256 (signed manifest "args"), argv[2] = URL
        char **argv = NULL;
        int argc = nv_args(&argv);
        if (argc < 2 || strlen(argv[1]) != 64) {
            // no hash: a sideloaded package, its files sit in its private folder
            g_dirmode = true;
            snprintf(g_dir, sizeof g_dir, "%s", g_home ? "/appdata" : "");
            char probe[220];
            struct stat st;
            snprintf(probe, sizeof probe, "%s/main.lua", g_dir);
            if (stat(probe, &st) != 0) {
                message(g_it ? "App incompleta" : "Incomplete app",
                        g_it ? "Manca l'impronta del pacchetto e main.lua." : "The package has no bundle hash and no main.lua.",
                        NULL, -1);
                wait_input();
                return;
            }
        } else if (!obtain_bundle(argv[1], argc > 2 ? argv[2] : NULL) || !lpk_index()) {
            if (g_pkg && !g_nres) { message(g_it ? "Pacchetto rovinato" : "Damaged package", NULL, NULL, -1); wait_input(); }
            return;
        }
        run_app(NULL);
        return;
    }
    // the engine on its own: the launcher, and whatever it starts
    for (;;) {
        g_dirmode = false;
        g_next[0] = 0;
        // ~/lua/.run (one line: "/lua/<name>.lua" or "/lua/<dir>"), written by the shell's `app run`:
        // start that script at once, then come back to the launcher as usual. One-shot.
        FILE *rf = g_home ? fopen("/lua/.run", "r") : NULL;
        if (rf) {
            if (fgets(g_next, sizeof g_next, rf)) g_next[strcspn(g_next, "\r\n")] = 0;
            fclose(rf);
            remove("/lua/.run");
            if (strncmp(g_next, "/lua/", 5) || strstr(g_next, "..")) g_next[0] = 0;
        }
        if (!g_next[0]) run_app("launcher");
        if (!g_next[0]) break;
        g_dirmode = true;
        snprintf(g_dir, sizeof g_dir, "%s", g_next);
        snprintf(g_main, sizeof g_main, "main.lua");
        size_t nl = strlen(g_dir);
        char *slash = strrchr(g_dir, '/');
        if (nl > 4 && !strcmp(g_dir + nl - 4, ".lua") && slash) {   // a single script
            snprintf(g_main, sizeof g_main, "%s", slash + 1);
            *slash = 0;
        }
        snprintf(g_app, sizeof g_app, "luaapp");
        run_app(NULL);
        if (!nv_gfx_present()) break;
    }
}
