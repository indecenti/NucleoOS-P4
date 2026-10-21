// jpgtest — board check for ABI v19 nv_jpeg_decode (not a store app: tools/hil/jpeg.py pushes it with
// its pictures, runs it and reads /results.txt from its data folder).
//
// For every .jpg in its data folder ("/" with "fs"): nv_jpeg_info, then nv_jpeg_decode to 496 px wide
// (height by aspect) five times, timed; the picture is shown upright on the turned canvas (ABI v19
// nv_gfx_set_rotation(1)). One line per file in /results.txt:
//   <file> info <ret> <w>x<h> decode <ret> <tw>x<th> min_ms <a> avg_ms <b>
// then "done". Stays on screen until Back.
#include "nucleo_sdk.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned char *read_all(const char *path, long *n) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    *n = ftell(f);
    fseek(f, 0, SEEK_SET);
    unsigned char *p = *n > 0 ? malloc((size_t)*n) : NULL;
    if (p && fread(p, 1, (size_t)*n, f) != (size_t)*n) { free(p); p = NULL; }
    fclose(f);
    return p;
}

int main(void) {
    nv_gfx_set_rotation(1);
    const int W = nv_gfx_width(), H = nv_gfx_height();
    nv_gfx_persist(1);
    nv_gfx_clear(0);
    nv_gfx_present();
    FILE *res = fopen("/results.txt", "w");
    DIR *dir = opendir("/");
    struct dirent *e;
    char names[32][64];
    int nn = 0;
    while (dir && (e = readdir(dir)) && nn < 32) {
        const size_t l = strlen(e->d_name);
        if (l > 4 && l < 64 && !strcasecmp(e->d_name + l - 4, ".jpg")) strcpy(names[nn++], e->d_name);
    }
    if (dir) closedir(dir);
    for (int i = 0; i < nn && res; i++) {
        char path[80];
        snprintf(path, sizeof path, "/%s", names[i]);
        long n = 0;
        unsigned char *jpg = read_all(path, &n);
        if (!jpg) { fprintf(res, "%s read failed\n", names[i]); continue; }
        int32_t iw = 0, ih = 0;
        const int ri = nv_jpeg_info(jpg, (uint32_t)n, &iw, &ih);
        int tw = 496, th = iw > 0 ? (int)((long)ih * tw / iw) : 685;
        if (th > H) th = H;
        if (th < 1) th = 1;
        uint16_t *out = malloc((size_t)tw * th * 2);
        int rd = -99, mn = 1 << 30, sum = 0, runs = 0;
        for (int k = 0; k < 5 && out; k++) {
            const int t0 = nv_millis();
            rd = nv_jpeg_decode(jpg, (uint32_t)n, out, (uint32_t)tw * th * 2, tw, th);
            const int ms = nv_millis() - t0;
            if (rd != 0) break;
            if (ms < mn) mn = ms;
            sum += ms;
            runs++;
        }
        fprintf(res, "%s info %d %dx%d decode %d %dx%d min_ms %d avg_ms %d\n", names[i], ri, (int)iw, (int)ih, rd,
                tw, th, runs ? mn : -1, runs ? sum / runs : -1);
        fflush(res);
        if (rd == 0) {
            nv_gfx_clear(0);
            nv_gfx_blit(out, tw, th, (W - tw) / 2, (H - th) / 2);
            nv_gfx_present();
        }
        free(out);
        free(jpg);
    }
    if (res) { fprintf(res, "done\n"); fclose(res); }
    nv_log(NV_LOG_INFO, "jpgtest done");
    while (nv_gfx_present())
        if (nv_gfx_back()) break;
    return 0;
}
