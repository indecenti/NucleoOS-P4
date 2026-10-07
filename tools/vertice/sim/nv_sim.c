// nv_sim.c — NucleoOS app simulator for the PC: the "nv" imports of sdk/include/nucleo_sdk.h
// implemented natively over the REAL Vertice engine (components/vertice, built with -DVX_SIM_CLOCK
// and its vx_* API renamed vxe_*, see build.sh). An app's main.c compiles unchanged with -DNV_SIM.
//
//   vxsim <app dir> <out dir> [frames]
//   env VX_TOUCH="f0-f1:x,y;..."   scripted fingers (canvas px), active for frames f0..f1 inclusive
//   env VX_DUMP="n,n,..."           frames to save as PPM (default: every 30th + the last)
//   env VX_DUMP_RANGE="a-b"         every frame from a to b (for a video: ffmpeg -i frame_%05d.ppm)
//   env VX_FPS=30                   simulated frame rate (nv_millis and the engine clock)
//
// Frames are saved as <out>/frame_NNNNN.ppm at canvas size; tools/vertice/sim/run.sh turns them
// into PNGs at panel size (the OS PPA-scales the canvas the same way).
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nucleo_sdk.h"

// ---- the engine, renamed vxe_* in this build (see build.sh) -------------------------------------
int  vxe_open(int w, int h);
void vxe_close(void);
int  vxe_texture(const uint16_t *px, int w, int h, int flags);
int  vxe_material(uint32_t color565, int shading, int alpha, int tex, int specular);
int  vxe_texture_new(int w, int h, uint32_t color565, int flags);
void vxe_texture_write(int tex, int x, int y, int w, int h, const uint16_t *px);
void vxe_mat_color(int mat, uint32_t color565);
int  vxe_prim(int kind, int a, int b, int c, int mat, int mat2);
int  vxe_mesh(const int32_t *xyz, int nverts, const uint16_t *idx, int ntris, const int16_t *uv,
              const uint8_t *tri_mat, int mat, int flags);
int  vxe_model(const uint8_t *data, size_t len, int flags);
int  vxe_clone(int id);
void vxe_obj_free(int id);
void vxe_obj_pos(int id, int x, int y, int z);
void vxe_obj_rot(int id, int rx, int ry, int rz);
void vxe_obj_show(int id, bool on);
void vxe_obj_depth(int id, int bias, int flags);
int  vxe_obj_lod(int id, int lod, int dist);
void vxe_obj_scale(int id, int percent);
void vxe_camera(int x, int y, int z, int rx, int ry, int rz);
void vxe_look_at(int x, int y, int z);
void vxe_lens(int fov_deg, int znear, int zfar);
void vxe_sun(int azimuth, int elevation, uint32_t rgb888, int intensity);
void vxe_ambient(uint32_t rgb888);
void vxe_sky(uint16_t top, uint16_t bottom);
void vxe_fog(int znear, int zfar);
void vxe_depth(bool on);
void vxe_floor(int y, int tex, int repeat, uint32_t color565);
void vxe_panorama(int tex, int horizon_row);
void vxe_water(int strength, int wave);
void vxe_caustics(int strength, int speed);
void vxe_shafts(int strength, int slope);
void vxe_ceiling(int y, int tex, int repeat);
int  vxe_emitter(int max, uint32_t c0, uint32_t c1, int s0, int s1, int life_ms, int gravity, int flags);
void vxe_emit(int em, int x, int y, int z, int vx, int vy, int vz, int spread, int count);
void vxe_reset(void);
int  vxe_render(uint16_t *target);
void vxe_pick_at(int x, int y);
int  vxe_picked(void);
int  vxe_stat(int what);
bool vxe_is_open(void);

#include "font5x7.inc"

// ---- simulator state ---------------------------------------------------------------------------
static int       W = 512, H = 300;
static uint16_t *fb;
static const char *app_dir, *out_dir;
static int       frame = 0, max_frames = 300, fps = 30;
static int       dump_every = 30;
static int       dump_list[256], n_dump = 0;
struct touch { int f0, f1, x, y; };
static struct touch touches[32];
static int       n_touch = 0;
static int64_t   render_us_total = 0, render_n = 0;

int64_t vx_sim_clock_us(void) { return (int64_t)frame * 1000000 / fps; }

static bool vx_ready(void) { return vxe_is_open() || vxe_open(W, H); }

static int manifest_int(const char *json, const char *key, int def) {
    char pat[64];
    snprintf(pat, sizeof pat, "\"%s\"", key);
    const char *p = strstr(json, pat);
    if (!p) return def;
    p = strchr(p + strlen(pat), ':');
    return p ? atoi(p + 1) : def;
}

static uint8_t *read_file(const char *path, long *len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *b = (uint8_t *)malloc((size_t)n + 1);
    if (b && fread(b, 1, (size_t)n, f) != (size_t)n) { free(b); b = NULL; }
    fclose(f);
    if (b) { b[n] = 0; *len = n; }
    return b;
}

// ---- nv core -----------------------------------------------------------------------------------
void nv_print(const char *msg) { printf("print: %s\n", msg); }
void nv_log(int32_t level, const char *msg) { printf("log%d @%d: %s\n", (int)level, frame, msg); }   // @frame
void nv_toast(int32_t kind, const char *msg) { printf("toast%d: %s\n", (int)kind, msg); }
int32_t nv_millis(void) { return (int32_t)(vx_sim_clock_us() / 1000); }
int64_t nv_time_unix(void) { return 1790000000; }
int32_t nv_lang(char *buf, uint32_t len) {   // VX_LANG=en|it|... (default it)
    const char *l = getenv("VX_LANG");
    if (!l || strlen(l) != 2) l = "it";
    if (len >= 3) { memcpy(buf, l, 2); buf[2] = 0; return 2; }
    return 0;
}
int32_t nv_rand(void) { static uint32_t s = 12345; s = s * 1103515245u + 12345u; return (int32_t)(s >> 1); }
void nv_sleep_ms(int32_t ms) { (void)ms; }
int32_t nv_save(const char *name, const void *data, int32_t len) {
    char p[512]; snprintf(p, sizeof p, "%s/%s", out_dir, name);
    FILE *f = fopen(p, "wb"); if (!f) return 0;
    fwrite(data, 1, (size_t)len, f); fclose(f); return 1;
}
int32_t nv_load(const char *name, void *data, int32_t len) {
    char p[512]; snprintf(p, sizeof p, "%s/%s", out_dir, name);
    FILE *f = fopen(p, "rb"); if (!f) return 0;
    int32_t n = (int32_t)fread(data, 1, (size_t)len, f); fclose(f); return n;
}
// Sound calls are logged one per line, "snd @<frame> <op> ...", so a video can rebuild the mix
// offline from the game's own samples (play: voice name vol pitch flags; set: voice vol pitch;
// stop: voice fade_ms; master: vol; sound: name).
void nv_sound(const char *name) { printf("snd @%d sound %s\n", frame, name); }
// ABI v15 mixer: logged, voices numbered.
static int s_voice_n;
int32_t nv_snd_play(const char *name, int32_t vol, int32_t pitch, int32_t flags) {
    const int h = s_voice_n++ & 0x7FF;
    printf("snd @%d play %d %s %d %d %d\n", frame, h, name, (int)vol, (int)pitch, (int)flags);
    return h;
}
int32_t nv_snd_preload(const char *name) { (void)name; return 0; }
void nv_snd_set(int32_t v, int32_t vol, int32_t pitch) { printf("snd @%d set %d %d %d\n", frame, (int)v, (int)vol, (int)pitch); }
void nv_snd_stop(int32_t v, int32_t fade) { printf("snd @%d stop %d %d\n", frame, (int)v, (int)fade); }
void nv_snd_master(int32_t vol) { printf("snd @%d master %d\n", frame, (int)vol); }
void nv_speak(const char *text, const char *lang) { printf("speak[%s]: %s\n", lang, text); }
// ABI v10 raw audio: accepted at once (backlog 0) and written to $VX_AUDIO (raw s16 PCM) when set,
// to listen to or measure the mix.
static FILE *s_pcm;
int32_t nv_audio_open(int32_t rate, int32_t channels) {
    const char *p = getenv("VX_AUDIO");
    if (p && !s_pcm) s_pcm = fopen(p, "wb");
    printf("audio: open %d Hz x%d (frame %d)\n", rate, channels, frame);
    return 1;
}
int32_t nv_audio_write(const void *pcm, int32_t bytes) { if (s_pcm) fwrite(pcm, 1, (size_t)bytes, s_pcm); return bytes; }
int32_t nv_audio_backlog(void) { return 0; }
void nv_audio_close(void) { printf("audio: close (frame %d)\n", frame); if (s_pcm) fflush(s_pcm); }

// ---- 2D canvas ---------------------------------------------------------------------------------
static inline void px(int x, int y, uint16_t c) { if ((unsigned)x < (unsigned)W && (unsigned)y < (unsigned)H) fb[y * W + x] = c; }
int32_t nv_gfx_width(void) { return W; }
int32_t nv_gfx_height(void) { return H; }
void nv_gfx_clear(int32_t c) { for (int i = 0; i < W * H; i++) fb[i] = (uint16_t)c; }
void nv_gfx_rect(int32_t x, int32_t y, int32_t w, int32_t h, int32_t c) {
    for (int j = y; j < y + h; j++) for (int i = x; i < x + w; i++) px(i, j, (uint16_t)c);
}
void nv_gfx_circle(int32_t cx, int32_t cy, int32_t r, int32_t c) {
    for (int j = -r; j <= r; j++) for (int i = -r; i <= r; i++) if (i * i + j * j <= r * r) px(cx + i, cy + j, (uint16_t)c);
}
void nv_gfx_line(int32_t x0, int32_t y0, int32_t x1, int32_t y1, int32_t c) {
    int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1, dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1, e = dx + dy;
    for (;;) {
        px(x0, y0, (uint16_t)c); px(x0 + 1, y0, (uint16_t)c); px(x0, y0 + 1, (uint16_t)c);
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * e;
        if (e2 >= dy) { e += dy; x0 += sx; }
        if (e2 <= dx) { e += dx; y0 += sy; }
    }
}
void nv_gfx_tri(int32_t x0, int32_t y0, int32_t x1, int32_t y1, int32_t x2, int32_t y2, int32_t c) {
    int miny = y0 < y1 ? (y0 < y2 ? y0 : y2) : (y1 < y2 ? y1 : y2);
    int maxy = y0 > y1 ? (y0 > y2 ? y0 : y2) : (y1 > y2 ? y1 : y2);
    int xs[3] = {x0, x1, x2}, ys[3] = {y0, y1, y2};
    for (int y = miny; y <= maxy; y++) {
        int lo = 1 << 30, hi = -(1 << 30);
        for (int e = 0; e < 3; e++) {
            int ax = xs[e], ay = ys[e], bx = xs[(e + 1) % 3], by = ys[(e + 1) % 3];
            if ((y < ay && y < by) || (y > ay && y > by)) continue;
            int x = ay == by ? ax : ax + (bx - ax) * (y - ay) / (by - ay);
            if (ay == by) { if (bx < lo) lo = bx; if (bx > hi) hi = bx; }
            if (x < lo) lo = x;
            if (x > hi) hi = x;
        }
        for (int x = lo; x <= hi; x++) px(x, y, (uint16_t)c);
    }
}
void nv_gfx_blit_raw(const void *p, int32_t len, int32_t x, int32_t y, int32_t w, int32_t h) {
    if ((int64_t)w * h * 2 > len) return;
    const uint16_t *s = (const uint16_t *)p;
    for (int j = 0; j < h; j++) for (int i = 0; i < w; i++) px(x + i, y + j, s[j * w + i]);
}
static uint16_t *load_565(const char *name, int *w, int *h) {
    char p[512]; snprintf(p, sizeof p, "%s/img/%s.565", app_dir, name);
    long n = 0; uint8_t *b = read_file(p, &n);
    if (!b || n < 4) { free(b); return NULL; }
    *w = b[0] | b[1] << 8; *h = b[2] | b[3] << 8;
    if (n < 4 + (long)*w * *h * 2) { free(b); return NULL; }
    uint16_t *out = (uint16_t *)malloc((size_t)*w * *h * 2);
    memcpy(out, b + 4, (size_t)*w * *h * 2);
    free(b);
    return out;
}
// Asset cache (by name), like the device's image cache.
static struct { char name[32]; uint16_t *px; int w, h; } s_imgc[96];
static int s_imgn = 0;
static uint16_t *img_get(const char *name, int *w, int *h) {
    for (int i = 0; i < s_imgn; i++) if (!strcmp(s_imgc[i].name, name)) { *w = s_imgc[i].w; *h = s_imgc[i].h; return s_imgc[i].px; }
    uint16_t *im = load_565(name, w, h);
    if (!im) return NULL;
    if (s_imgn < 96) { snprintf(s_imgc[s_imgn].name, 32, "%s", name); s_imgc[s_imgn].px = im; s_imgc[s_imgn].w = *w; s_imgc[s_imgn].h = *h; s_imgn++; }
    return im;
}
void nv_gfx_image(const char *name, int32_t x, int32_t y, int32_t w, int32_t h) {
    int iw, ih; uint16_t *im = img_get(name, &iw, &ih);
    if (!im) return;
    for (int j = 0; j < h; j++) for (int i = 0; i < w; i++) {
        uint16_t c = im[(j * ih / h) * iw + i * iw / w];
        if (c != 0xF81F) px(x + i, y + j, c);
    }
}
void nv_gfx_sprite(const char *name, int32_t sx, int32_t sy, int32_t sw, int32_t sh, int32_t x, int32_t y, int32_t w, int32_t h, int32_t tint) {
    const uint32_t t = (uint32_t)tint & 0xFFFF, tr = (t >> 11) + 1, tg = ((t >> 5) & 63) + 1, tb = (t & 31) + 1;
    int iw, ih; uint16_t *im = img_get(name, &iw, &ih);
    if (!im || sw <= 0 || sh <= 0 || w <= 0 || h <= 0 || sx < 0 || sy < 0 || sx >= iw || sy >= ih) return;
    if (sw > iw - sx) sw = iw - sx;
    if (sh > ih - sy) sh = ih - sy;
    for (int j = 0; j < h; j++) for (int i = 0; i < w; i++) {
        uint16_t c = im[(sy + j * sh / h) * iw + sx + i * sw / w];
        if (c == 0xF81F) continue;
        if (t != 0xFFFF) c = (uint16_t)(((((c >> 11) * tr) >> 5) << 11) | (((((c >> 5) & 63) * tg) >> 6) << 5) | (((c & 31) * tb) >> 5));
        px(x + i, y + j, c);
    }
}
void nv_gfx_panel(int32_t x, int32_t y, int32_t w, int32_t h, int32_t r, int32_t ct, int32_t cb, int32_t alpha) {
    if (w <= 0 || h <= 0 || alpha <= 0) return;
    if (r > w / 2) r = w / 2;
    if (r > h / 2) r = h / 2;
    if (r < 0) r = 0;
    if (alpha > 255) alpha = 255;
    const uint32_t a5 = (uint32_t)(alpha + 4) >> 3;
    const uint32_t t32 = (((uint32_t)ct & 0xFFFF) | (((uint32_t)ct & 0xFFFF) << 16)) & 0x07E0F81Fu;
    const uint32_t b32 = (((uint32_t)cb & 0xFFFF) | (((uint32_t)cb & 0xFFFF) << 16)) & 0x07E0F81Fu;
    for (int j = 0; j < h; j++) {
        int inset = 0;
        if (r > 0 && (j < r || j >= h - r)) {
            const float dy = (j < r ? (float)(r - j) : (float)(j - (h - 1 - r))) - 0.5f, d = (float)r * r - dy * dy;
            inset = r - (int)(d > 0 ? __builtin_sqrtf(d) + 0.5f : 0);
        }
        const uint32_t k5 = h > 1 ? (uint32_t)(j * 32 / (h - 1)) : 0;
        const uint32_t c32 = ((t32 * (32 - k5) + b32 * k5) >> 5) & 0x07E0F81Fu;
        for (int i = inset; i < w - inset; i++) {
            const int xx = x + i, yy = y + j;
            if ((unsigned)xx >= (unsigned)W || (unsigned)yy >= (unsigned)H) continue;
            uint32_t o = c32;
            if (a5 < 32) { const uint16_t d0 = fb[yy * W + xx]; const uint32_t d = (d0 | ((uint32_t)d0 << 16)) & 0x07E0F81Fu; o = ((d * (32 - a5) + c32 * a5) >> 5) & 0x07E0F81Fu; }
            fb[yy * W + xx] = (uint16_t)(o | (o >> 16));
        }
    }
}
void nv_gfx_text(int32_t x, int32_t y, const char *s, int32_t color, int32_t scale) {
    if (scale < 1) scale = 1;
    for (int cx = x; *s; s++, cx += 6 * scale) {
        const uint8_t *g = FONT5x7[font_glyph(*s)];
        for (int row = 0; row < 7; row++) for (int col = 0; col < 5; col++)
            if (g[row] & (0x10 >> col))
                for (int dy = 0; dy < scale; dy++) for (int dx = 0; dx < scale; dx++)
                    px(cx + col * scale + dx, y + row * scale + dy, (uint16_t)color);
    }
}
int32_t nv_gfx_text_width(const char *s, int32_t scale) { return (int32_t)strlen(s) * 6 * (scale < 1 ? 1 : scale); }
void nv_backlight(int32_t level) { (void)level; }
void nv_gfx_terrain_raw(const void *tops, int32_t len, int32_t x0, int32_t ybot, int32_t cg, int32_t cd) {
    const int16_t *t = (const int16_t *)tops;
    for (int i = 0; i < len / 2; i++) for (int y = t[i]; y < ybot; y++) px(x0 + i, y, (uint16_t)(y < t[i] + 3 ? cg : cd));
}
void nv_gfx_tone(int32_t f, int32_t ms) { (void)f; (void)ms; }

static int fingers(int *xs, int *ys) {
    int n = 0;
    for (int i = 0; i < n_touch && n < 5; i++)
        if (frame >= touches[i].f0 && frame <= touches[i].f1) { xs[n] = touches[i].x; ys[n] = touches[i].y; n++; }
    return n;
}
int32_t nv_gfx_input_raw(void) {
    int xs[5], ys[5];
    static int lx, ly;
    int n = fingers(xs, ys);
    if (n) { lx = xs[0]; ly = ys[0]; }
    return ((n ? 1 : 0) << 24) | (ly << 12) | lx;
}
int32_t nv_gfx_touch_count(void) { int xs[5], ys[5]; return fingers(xs, ys); }
int32_t nv_gfx_touch_point_raw(int32_t idx) {
    int xs[5], ys[5], n = fingers(xs, ys);
    return idx < n ? (1 << 24) | (ys[idx] << 12) | xs[idx] : 0;
}
int32_t nv_gfx_back(void) { return 0; }
// VX_PAD="f0-f1:bits;..." scripted pad bits (as nv.gfx_pad returns them).
static struct touch pads[512];
static int n_pads = 0;
// VX_STICK="f0-f1:rps": one controller whose right stick turns in circles at rps turns a second
// between frames f0 and f1 (to test a crank-style reel); no controller when unset.
static int stick_f0 = -1, stick_f1 = -1;
static float stick_rps;
static void stick_init(void) {
    static int done;
    if (done) return;
    done = 1;
    const char *e = getenv("VX_STICK");
    if (e && sscanf(e, "%d-%d:%f", &stick_f0, &stick_f1, &stick_rps) != 3) stick_f0 = -1;
}
int32_t nv_pad_count(void) { stick_init(); return stick_f0 >= 0 ? 1 : 0; }
int32_t nv_pad_state(int32_t i, nv_pad_state_t *st, int32_t len) {
    stick_init();
    if (i != 0 || stick_f0 < 0 || len < (int32_t)sizeof *st) return 0;
    memset(st, 0, sizeof *st);
    st->rumble = 1;                                    // it has motors (the calls are logged)
    if (frame >= stick_f0 && frame <= stick_f1) {
        const float a = 6.2831853f * stick_rps * frame / fps;
        st->rx = (int16_t)(cosf(a) * 30000); st->ry = (int16_t)(sinf(a) * 30000);
    }
    return (int32_t)sizeof *st;
}
int32_t nv_pad_rumble(int32_t i, int32_t lo, int32_t hi, int32_t ms) {   // logged, to check a game's haptics
    printf("rumble @%d %d %d %d %d\n", frame, (int)i, (int)lo, (int)hi, (int)ms);
    return 0;
}
int32_t nv_gfx_pad(void) {
    int32_t v = 0;
    for (int i = 0; i < n_pads; i++) if (frame >= pads[i].f0 && frame <= pads[i].f1) v |= pads[i].x;
    return v;
}
void nv_gfx_persist(int32_t on) { (void)on; }
static uint16_t *bg_snap;                 // ABI v6 background snapshot (as the device: a copy of the canvas)
void nv_gfx_bg_save(void) {
    if (!bg_snap) bg_snap = (uint16_t *)malloc((size_t)W * H * 2);
    if (bg_snap) memcpy(bg_snap, fb, (size_t)W * H * 2);
}
void nv_gfx_bg_restore(int32_t x, int32_t y, int32_t w, int32_t h) {
    if (!bg_snap) return;
    int x1 = x + w, y1 = y + h;
    if (x < 0) x = 0; if (y < 0) y = 0; if (x1 > W) x1 = W; if (y1 > H) y1 = H;
    for (int yy = y; yy < y1; yy++) memcpy(&fb[yy * W + x], &bg_snap[yy * W + x], (size_t)(x1 - x) * 2);
}

static int dump_lo = -1, dump_hi = -1;   // VX_DUMP_RANGE="a-b": every frame in [a, b] (videos)
static bool want_dump(int f) {
    if (f == max_frames - 1) return true;
    if (dump_lo >= 0 && f >= dump_lo && f <= dump_hi) return true;
    for (int i = 0; i < n_dump; i++) if (dump_list[i] == f) return true;
    return !n_dump && dump_every > 0 && f % dump_every == 0;
}
int32_t nv_gfx_present(void) {
    if (want_dump(frame)) {
        char p[512]; snprintf(p, sizeof p, "%s/frame_%05d.ppm", out_dir, frame);
        FILE *f = fopen(p, "wb");
        if (f) {
            fprintf(f, "P6\n%d %d\n255\n", W, H);
            for (int i = 0; i < W * H; i++) {
                uint16_t c = fb[i];
                uint8_t rgb[3] = { (uint8_t)((c >> 11) * 255 / 31), (uint8_t)(((c >> 5) & 63) * 255 / 63),
                                   (uint8_t)((c & 31) * 255 / 31) };
                fwrite(rgb, 1, 3, f);
            }
            fclose(f);
        }
    }
    frame++;
    return frame < max_frames;
}

// ---- nv.open_* / net / http: not simulated -----------------------------------------------------
int32_t nv_open_path(char *buf, int32_t len) { if (len) buf[0] = 0; return 0; }
int32_t nv_open_size(void) { return -1; }
int32_t nv_open_read(int32_t off, void *buf, int32_t len) { (void)off; (void)buf; (void)len; return -1; }

// ---- ABI v9 Vertice ----------------------------------------------------------------------------
int32_t vx_texture_raw(const void *p, int32_t len, int32_t w, int32_t h, int32_t flags) {
    if (!vx_ready() || (int64_t)w * h * 2 > len) return -1;
    return vxe_texture((const uint16_t *)p, w, h, flags);
}
int32_t vx_texture_load(const char *name, int32_t flags) {
    int w, h; uint16_t *im = vx_ready() ? load_565(name, &w, &h) : NULL;
    if (!im) return -1;
    int t = vxe_texture(im, w, h, flags);
    free(im);
    return t;
}
int32_t vx_texture_new(int32_t w, int32_t h, int32_t c, int32_t f) { return vx_ready() ? vxe_texture_new(w, h, (uint32_t)c & 0xFFFF, f) : -1; }
void vx_texture_write_raw(int32_t t, int32_t x, int32_t y, int32_t w, int32_t h, const void *px, int32_t len) {
    if (vx_ready() && (int64_t)w * h * 2 <= len) vxe_texture_write(t, x, y, w, h, (const uint16_t *)px);
}
int32_t vx_material(int32_t c, int32_t s, int32_t a, int32_t t, int32_t sp) {
    return vx_ready() ? vxe_material((uint32_t)c & 0xFFFF, s, a, t, sp) : -1;
}
void vx_mat_color(int32_t m, int32_t c) { if (vx_ready()) vxe_mat_color(m, (uint32_t)c & 0xFFFF); }
int32_t vx_prim(int32_t k, int32_t a, int32_t b, int32_t c, int32_t m, int32_t m2) {
    return vx_ready() ? vxe_prim(k, a, b, c, m, m2) : -1;
}
int32_t vx_mesh_raw(const void *xyz, int32_t xl, const void *idx, int32_t il, const void *uv, int32_t ul,
                    const void *mats, int32_t ml, int32_t mat, int32_t flags) {
    if (!vx_ready()) return -1;
    int nv = xl / 12, nt = il / 6;
    if (ml && nt > ml) return -1;
    if (ul && ul < nv * 4) return -1;
    return vxe_mesh((const int32_t *)xyz, nv, (const uint16_t *)idx, nt, ul ? (const int16_t *)uv : NULL,
                    ml ? (const uint8_t *)mats : NULL, mat, flags);
}
int32_t vx_model(const char *name, int32_t flags) {
    char p[512]; snprintf(p, sizeof p, "%s/models/%s.vxm", app_dir, name);
    long n = 0; uint8_t *b = vx_ready() ? read_file(p, &n) : NULL;
    if (!b) return -1;
    int id = vxe_model(b, (size_t)n, flags);
    free(b);
    return id;
}
int32_t vx_clone(int32_t id) { return vx_ready() ? vxe_clone(id) : -1; }
void vx_obj_free(int32_t id) { if (vx_ready()) vxe_obj_free(id); }
void vx_obj_pos(int32_t id, int32_t x, int32_t y, int32_t z) { if (vx_ready()) vxe_obj_pos(id, x, y, z); }
void vx_obj_rot(int32_t id, int32_t x, int32_t y, int32_t z) { if (vx_ready()) vxe_obj_rot(id, x, y, z); }
void vx_obj_show(int32_t id, int32_t on) { if (vx_ready()) vxe_obj_show(id, on != 0); }
void vx_obj_depth(int32_t id, int32_t b, int32_t f) { if (vx_ready()) vxe_obj_depth(id, b, f); }
int32_t vx_obj_lod(int32_t id, int32_t lod, int32_t d) { return vx_ready() ? vxe_obj_lod(id, lod, d) : -1; }
void vx_obj_scale(int32_t id, int32_t p) { if (vx_ready()) vxe_obj_scale(id, p); }
void vx_camera(int32_t x, int32_t y, int32_t z, int32_t rx, int32_t ry, int32_t rz) { if (vx_ready()) vxe_camera(x, y, z, rx, ry, rz); }
void vx_look_at(int32_t x, int32_t y, int32_t z) { if (vx_ready()) vxe_look_at(x, y, z); }
void vx_lens(int32_t f, int32_t n, int32_t fa) { if (vx_ready()) vxe_lens(f, n, fa); }
void vx_sun(int32_t az, int32_t el, int32_t rgb, int32_t in) { if (vx_ready()) vxe_sun(az, el, (uint32_t)rgb, in); }
void vx_ambient(int32_t rgb) { if (vx_ready()) vxe_ambient((uint32_t)rgb); }
void vx_sky(int32_t t, int32_t b) { if (vx_ready()) vxe_sky((uint16_t)t, (uint16_t)b); }
void vx_fog(int32_t n, int32_t f) { if (vx_ready()) vxe_fog(n, f); }
void vx_depth(int32_t on) { if (vx_ready()) vxe_depth(on != 0); }
void vx_floor(int32_t y, int32_t t, int32_t r, int32_t c) { if (vx_ready()) vxe_floor(y, t, r, (uint32_t)c & 0xFFFF); }
void vx_water(int32_t k, int32_t w) { if (vx_ready()) vxe_water(k, w); }
void vx_caustics(int32_t k, int32_t s) { if (vx_ready()) vxe_caustics(k, s); }
void vx_shafts(int32_t k, int32_t s) { if (vx_ready()) vxe_shafts(k, s); }
void vx_ceiling(int32_t y, int32_t t, int32_t r) { if (vx_ready()) vxe_ceiling(y, t, r); }
void vx_panorama(int32_t t, int32_t h) { if (vx_ready()) vxe_panorama(t, h); }
int32_t vx_emitter(int32_t mx, int32_t c0, int32_t c1, int32_t s0, int32_t s1, int32_t life, int32_t gr, int32_t fl) {
    return vx_ready() ? vxe_emitter(mx, (uint32_t)c0 & 0xFFFF, (uint32_t)c1 & 0xFFFF, s0, s1, life, gr, fl) : -1;
}
void vx_emit(int32_t e, int32_t x, int32_t y, int32_t z, int32_t vx, int32_t vy, int32_t vz, int32_t sp, int32_t n) {
    if (vx_ready()) vxe_emit(e, x, y, z, vx, vy, vz, sp, n);
}
void vx_reset(void) { if (vx_ready()) vxe_reset(); }
int32_t vx_render(void) {
    if (!vx_ready()) return -1;
    int r = vxe_render(fb);
    render_us_total += vxe_stat(VX_STAT_US);
    render_n++;
    return r;
}
void vx_pick_at(int32_t x, int32_t y) { if (vx_ready()) vxe_pick_at(x, y); }
int32_t vx_picked(void) { return vx_ready() ? vxe_picked() : -1; }
int32_t vx_stat(int32_t w) { return vx_ready() ? vxe_stat(w) : -1; }

// nv_printf (normally sdk/src/nucleo_sdk.c, which also defines memcpy & co. — not for a host build)
#include <stdarg.h>
void nv_printf(const char *fmt, ...) {
    char b[256]; va_list ap; va_start(ap, fmt); vsnprintf(b, sizeof b, fmt, ap); va_end(ap); nv_print(b);
}

void run(void);   // the app's entry (NV_EXPORT("run"))

int main(int argc, char **argv) {
    if (argc < 3) { fprintf(stderr, "usage: vxsim <app dir> <out dir> [frames]\n"); return 2; }
    app_dir = argv[1]; out_dir = argv[2];
    if (argc > 3) max_frames = atoi(argv[3]);
    const char *e;
    if ((e = getenv("VX_FPS"))) fps = atoi(e) > 0 ? atoi(e) : 30;
    if ((e = getenv("VX_DUMP"))) {
        if (!strcmp(e, "none")) dump_every = 0;
        else for (const char *p = e; *p && n_dump < 256;) {
            dump_list[n_dump++] = atoi(p);
            p = strchr(p, ',');
            if (!p) break;
            p++;
        }
    }
    if ((e = getenv("VX_DUMP_RANGE")) && sscanf(e, "%d-%d", &dump_lo, &dump_hi) == 2) dump_every = 0;
    if ((e = getenv("VX_TOUCH"))) {
        for (const char *p = e; *p && n_touch < 32;) {
            struct touch t;
            if (sscanf(p, "%d-%d:%d,%d", &t.f0, &t.f1, &t.x, &t.y) == 4) touches[n_touch++] = t;
            p = strchr(p, ';');
            if (!p) break;
            p++;
        }
    }
    if ((e = getenv("VX_PAD"))) {
        for (const char *p = e; *p && n_pads < 512;) {
            struct touch t = {0, 0, 0, 0};
            if (sscanf(p, "%d-%d:%d", &t.f0, &t.f1, &t.x) == 3) pads[n_pads++] = t;
            p = strchr(p, ';');
            if (!p) break;
            p++;
        }
    }
    char mp[512]; snprintf(mp, sizeof mp, "%s/manifest.json", app_dir);
    long n = 0; char *m = (char *)read_file(mp, &n);
    if (m) { W = manifest_int(m, "canvas_w", W); H = manifest_int(m, "canvas_h", H); free(m); }
    fb = (uint16_t *)calloc((size_t)W * H, 2);
    printf("vxsim: %s canvas %dx%d, %d frames @%d fps\n", app_dir, W, H, max_frames, fps);
    run();
    printf("vxsim: done at frame %d; %lld renders\n", frame, (long long)render_n);
    vxe_close();
    return 0;
}
