// harness.c — PC test of the chip8 app (native build, -DNV_SIM): stubs the nv_* host imports with
// a 1024x600 RGB565 canvas, then drives the real app loop (main.c is #included so the script can
// see its state): menu screenshot, a real tap on the first card, then every bundled game for 300
// frames with scripted gamepad + keypad input. Prints a per-game report, writes PPM screenshots
// to the output dir and exits non-zero if a game hit an unknown opcode or the tap test failed.
//
//   gcc -O2 -DNV_SIM -Isdk/include -Iports/chip8 -I<games.h dir> harness.c ../chip8.c -o c8h
//   ./c8h <outdir>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../main.c"

#define FRAMES_PER_GAME 300

static uint16_t cv[W * H];
static const char *outdir = ".";
static int sim_frame, sim_ms, sim_running = 1;
static int sim_touch_n, sim_tx[5], sim_ty[5];
static int sim_pad, sim_back_req, sim_pick_req = -1;
static int sim_phase, sim_gi, sim_gframes, sim_blits, sim_sound, sim_failures;
static long sim_audio_q;

// ---- canvas stubs --------------------------------------------------------------------------------
static void fill(int x, int y, int w, int h, uint16_t c) {
    for (int yy = y < 0 ? 0 : y; yy < y + h && yy < H; yy++)
        for (int xx = x < 0 ? 0 : x; xx < x + w && xx < W; xx++) cv[yy * W + xx] = c;
}
void nv_gfx_persist(int32_t on) { (void)on; }
void nv_gfx_clear(int32_t c) { fill(0, 0, W, H, (uint16_t)c); }
void nv_gfx_rect(int32_t x, int32_t y, int32_t w, int32_t h, int32_t c) { fill(x, y, w, h, (uint16_t)c); }
void nv_gfx_blit_raw(const void *px, int32_t len, int32_t x, int32_t y, int32_t w, int32_t h) {
    if ((long)w * h * 2 > len) { printf("  BAD BLIT len\n"); sim_failures++; return; }
    const uint16_t *s = (const uint16_t *)px;
    for (int r = 0; r < h; r++)
        for (int c = 0; c < w; c++)
            if (x + c >= 0 && x + c < W && y + r >= 0 && y + r < H) cv[(y + r) * W + x + c] = s[r * w + c];
    sim_blits++;
}
void nv_gfx_text(int32_t x, int32_t y, const char *s, int32_t col, int32_t scale) {
    // no font here: one block per glyph shows the layout
    for (int i = 0; s[i]; i++)
        if (s[i] != ' ') fill(x + i * 6 * scale, y + scale, 5 * scale, 5 * scale, (uint16_t)col);
}
void nv_gfx_tone(int32_t hz, int32_t ms) { (void)hz; (void)ms; sim_sound = 1; }
int32_t nv_save(const char *n, const void *d, int32_t len) { (void)n; (void)d; (void)len; return 1; }
int32_t nv_load(const char *n, void *d, int32_t len) { (void)n; (void)d; (void)len; return 0; }
int32_t nv_millis(void) { return sim_ms; }
int32_t nv_rand(void) { return rand(); }
int32_t nv_audio_open(int32_t rate, int32_t ch) { (void)rate; (void)ch; sim_audio_q = 0; return 1; }
int32_t nv_audio_write(const void *pcm, int32_t bytes) {
    const int16_t *p = (const int16_t *)pcm;
    for (int i = 0; i < bytes / 2; i++) if (p[i]) sim_sound = 1;
    sim_audio_q += bytes;
    return bytes;
}
int32_t nv_audio_backlog(void) { return (int32_t)sim_audio_q; }
void nv_audio_close(void) {}
int32_t nv_gfx_touch_count(void) { return sim_touch_n; }
int32_t nv_gfx_touch_point_raw(int32_t i) {
    if (i < 0 || i >= sim_touch_n) return 0;
    return 1 << 24 | sim_ty[i] << 12 | sim_tx[i];
}
int32_t nv_gfx_input_raw(void) { return sim_touch_n ? nv_gfx_touch_point_raw(0) : (sim_tx[0] | sim_ty[0] << 12); }
int32_t nv_gfx_pad(void) { return sim_pad; }
int32_t nv_pad_count(void) { return 0; }
int32_t nv_pad_state(int32_t i, nv_pad_state_t *st, int32_t len) { (void)i; (void)st; (void)len; return 0; }
int32_t nv_gfx_back(void) { int r = sim_back_req; sim_back_req = 0; return r; }
int sim_pick(void) { int r = sim_pick_req; sim_pick_req = -1; return r; }

static void dump(const char *name, int x0, int y0, int w, int h) {
    char path[512];
    snprintf(path, sizeof path, "%s/%s.ppm", outdir, name);
    FILE *f = fopen(path, "wb");
    if (!f) { perror(path); return; }
    fprintf(f, "P6\n%d %d\n255\n", w, h);
    for (int y = y0; y < y0 + h; y++)
        for (int x = x0; x < x0 + w; x++) {
            uint16_t c = cv[y * W + x];
            unsigned char px[3] = {(unsigned char)((c >> 11) << 3), (unsigned char)(((c >> 5) & 63) << 2),
                                   (unsigned char)((c & 31) << 3)};
            fwrite(px, 1, 3, f);
        }
    fclose(f);
}

static void touch(int n, int x, int y) { sim_touch_n = n; sim_tx[0] = x; sim_ty[0] = y; }

static void report_game(void) {
    const c8_game_t *g = &c8_games[g_game];
    int lit = 0;
    for (int y = 0; y < c8_h(&g_c8); y++)
        for (int x = 0; x < c8_w(&g_c8); x++) lit += c8_px(&g_c8, x, y) != 0;
    const char *st = g_c8.state == C8_RUN ? "run" : g_c8.state == C8_WAITKEY ? "waitkey" :
                     g_c8.state == C8_HALT ? "halt" : "ERROR";
    printf("%-22s %-7s %5u B  %-7s pc=%04X  %s  lit=%4d  blits=%5d  sound=%s", g->id,
           g->platform == 0 ? "chip8" : g->platform == 1 ? "schip" : "xochip", (unsigned)g->len, st,
           g_c8.pc, g_c8.hires ? "hires" : "lores", lit, sim_blits, sim_sound ? "yes" : "no");
    if (g_c8.state == C8_ERROR) { printf("  op=%04X at %04X", g_c8.err_op, g_c8.err_pc); sim_failures++; }
    printf("\n");
}

int32_t nv_gfx_present(void) {
    sim_frame++;
    sim_ms = (int)((long)sim_frame * 1000 / 60);
    sim_audio_q -= 16000 * 2 / 60;   // the speaker drains ~1 frame of audio
    if (sim_audio_q < 0) sim_audio_q = 0;
    switch (sim_phase) {
    case 0:   // menu, then a real tap on the first card
        if (sim_frame == 3) {
            dump("menu", 0, 0, W, H);
            int x, y;
            card_pos(0, &x, &y);
            touch(1, x + CARD_W / 2, y + CARD_H / 2);
        } else if (sim_frame == 4) {
            touch(0, sim_tx[0], sim_ty[0]);
        } else if (sim_frame == 6) {
            if (g_mode != MODE_GAME || g_game != 0) { printf("FAIL: tap on the first card did not start it\n"); sim_failures++; }
            else printf("ok: tap on the first card started %s\n", c8_games[0].id);
            sim_back_req = 1;
            sim_phase = 1;
        }
        break;
    case 1:   // every game in turn
        if (g_mode == MODE_MENU) {
            if (sim_gi >= C8_NGAMES) { sim_running = 0; break; }
            sim_pick_req = sim_gi;
            sim_gframes = 0; sim_blits = 0; sim_sound = 0; sim_pad = 0;
            touch(0, 0, 0);
            break;
        }
        sim_gframes++;
        {
            // gamepad: A, nothing, right, left, up, down, B, START, 15 frames each
            static const int seq[8] = {NV_PAD_A, 0, NV_PAD_RIGHT, NV_PAD_LEFT, NV_PAD_UP, NV_PAD_DOWN, NV_PAD_B, NV_PAD_START};
            sim_pad = seq[(sim_gframes / 15) % 8];
            // keypad: every 40 frames hold one hinted key (or cycle all keys) for 10 frames
            uint16_t h = c8_games[g_game].keys ? c8_games[g_game].keys : 0xFFFF;
            int slot = sim_gframes / 40, k = 0, seen = 0;
            for (int i = 0; i < 16; i++) if (h >> i & 1) { if (seen++ == slot % count_bits(h)) k = i; }
            if (sim_gframes % 40 < 10) { int x = 0, y = 0; key_at_pos(k, &x, &y); touch(1, x + KW / 2, y + KH / 2); }
            else touch(0, 0, 0);
        }
        if (sim_gframes == FRAMES_PER_GAME) {
            report_game();
            dump(c8_games[g_game].id, GX, GY, GW, GH);
            if (!strcmp(c8_games[g_game].id, "garlicscape")) dump("full_garlicscape", 0, 0, W, H);
            if (!strcmp(c8_games[g_game].id, "outlaw")) dump("full_outlaw", 0, 0, W, H);
            sim_gi++;
            sim_back_req = 1;
            touch(0, 0, 0);
            sim_pad = 0;
        }
        break;
    }
    return sim_running;
}

int main(int argc, char **argv) {
    if (argc > 1) outdir = argv[1];
    srand(1234);
    run();
    printf("%d games, %d frames, %d failure(s)\n", C8_NGAMES, sim_frame, sim_failures);
    return sim_failures ? 1 : 0;
}
