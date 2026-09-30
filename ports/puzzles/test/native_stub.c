// native_stub.c — the nv_* host imports for a native (x86_64, ASan/UBSan) build of the Puzzles app:
// a 1024x600 canvas in memory, the scripted touch session of script.c, screenshots as PPM.
//   pz_native <out dir> [game index]
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "nucleo_sdk.h"
#include "script.h"

#define CW 1024
#define CH 600
static uint16_t canvas[CW * CH];
static int frame, cur_down, cur_x, cur_y, pend_back, blits, blit_rows;
static const char *outdir;
extern const int gamecount;
void run(void);

static void write_ppm(const char *name) {
    char path[512];
    snprintf(path, sizeof path, "%s/%s.ppm", outdir, name);
    FILE *f = fopen(path, "wb");
    if (!f) return;
    fprintf(f, "P6\n%d %d\n255\n", CW, CH);
    static unsigned char rgb[CW * CH * 3];   // one write: the output dir may be a slow /mnt share
    for (int i = 0; i < CW * CH; i++) {
        uint16_t p = canvas[i];
        rgb[3 * i] = (unsigned char)((p >> 11) << 3);
        rgb[3 * i + 1] = (unsigned char)(((p >> 5) & 63) << 2);
        rgb[3 * i + 2] = (unsigned char)((p & 31) << 3);
    }
    fwrite(rgb, 1, sizeof rgb, f);
    fclose(f);
}

int32_t nv_gfx_width(void) { return CW; }
int32_t nv_gfx_height(void) { return CH; }
void nv_gfx_persist(int32_t on) { (void)on; }
void nv_gfx_tone(int32_t f, int32_t ms) { (void)f; (void)ms; }
int32_t nv_gfx_pad(void) { return 0; }
int32_t nv_millis(void) { return frame * 16; }
int32_t nv_rand(void) { return rand(); }
int64_t nv_time_unix(void) { return 1759000000; }
int32_t nv_lang(char *buf, uint32_t len) { snprintf(buf, len, "%s", getenv("PZ_LANG") ? getenv("PZ_LANG") : "it"); return 2; }
void nv_gfx_blit_raw(const void *px, int32_t len, int32_t x, int32_t y, int32_t w, int32_t h) {
    if ((int64_t)w * h * 2 > len || x != 0 || w != CW || y < 0 || y + h > CH) {
        fprintf(stderr, "bad blit x=%d y=%d w=%d h=%d len=%d\n", x, y, w, h, len);
        abort();
    }
    memcpy(canvas + y * CW, px, (size_t)w * h * 2);
    blits++; blit_rows += h;
}
int32_t nv_gfx_input_raw(void) { return (cur_down << 24) | (cur_y << 12) | cur_x; }
int32_t nv_gfx_back(void) { int b = pend_back; pend_back = 0; return b; }
int32_t nv_gfx_present(void) {
    const script_frame *f = script_at(frame++);
    if (!f) return 0;
    cur_down = f->down; cur_x = f->x; cur_y = f->y;
    if (f->back) pend_back = 1;
    if (f->shot[0]) write_ppm(f->shot);
    return 1;
}

int main(int argc, char **argv) {
    outdir = argc > 1 ? argv[1] : ".";
    srand(1234);
    int n = script_build(gamecount, argc > 2 ? argv[2] : NULL);
    clock_t t0 = clock();
    run();
    printf("native: %d/%d frames, %d blits (%d rows), %.1f s CPU\n", frame, n, blits, blit_rows,
           (double)(clock() - t0) / CLOCKS_PER_SEC);
    return frame >= n - 25 ? 0 : 1;   // run() returns at the script's final back gesture
}
