/* VoxelSpaceNG multi-core frame: every core takes strips of columns from a shared counter, renders
 * each strip into its own fast buffer (internal SRAM on an ESP32) and hands it to an async linear
 * copy into the frame (a DMA on an ESP32) while it renders the next one. The frame is column-major,
 * so a strip is one contiguous block: no 2D copies, no transposes.
 *
 * Why: on the ESP32-P4 cached PSRAM writes run at ~33 MB/s per core, so writing a 1024x600 frame
 * pixel by pixel costs ~37 ms of stalls; an SRAM strip + DMA takes that off the cores.
 *
 * The platform supplies the threads and the copy engine (vs_mt_platform); the core supplies the
 * rest. Platforms: host (pthreads + memcpy), ESP-IDF (FreeRTOS task + esp_async_memcpy), NucleoOS.
 */
#ifndef VSNG_MT_H
#define VSNG_MT_H

#include <stddef.h>
#include "vsng.h"

#define VS_MT_MAX_CORES 4

typedef struct {
    int ncores;                          /* workers, the caller included */
    int strip_w;                         /* columns per strip (16 is a good start) */
    void *ctx;
    /* Run job(arg, core) on cores 0..ncores-1 concurrently (core 0 = the caller) and return when
     * all have finished. */
    void (*run_parallel)(void *ctx, void (*job)(void *arg, int core), void *arg);
    /* Fast per-core memory, 64-byte aligned, or NULL: then strips are rendered straight into the
     * frame and no copies are made. */
    void *(*alloc_fast)(void *ctx, size_t n, int core);
    void (*free_fast)(void *ctx, void *p);
    /* Async copy n bytes (64-byte multiples, aligned) for this core's buffer slot (0/1), and the
     * wait for it. copy_start may finish synchronously. */
    void (*copy_start)(void *ctx, void *dst, const void *src, size_t n, int core, int slot);
    void (*copy_wait)(void *ctx, int core, int slot);
} vs_mt_platform;

typedef struct {
    vs_mt_platform p;
    uint16_t *buf[VS_MT_MAX_CORES][2];
    size_t buf_bytes;
    int buf_h;
    /* per frame */
    const vs_view *v;
    const vs_world *wd;
    const vs_sky *sky;
    const vs_cam *cam;
    uint16_t *fb;
    int nstrips;
    volatile int next;                   /* strip counter (atomic add) */
    vs_stats st[VS_MT_MAX_CORES];
    uint32_t strips_done[VS_MT_MAX_CORES];
} vs_mt;

/* The vs_mt itself must live in internal RAM on an ESP32 (the strip counter is an atomic RMW shared
 * by the cores; that is only guaranteed on internal memory).
 * Buffers sized for views up to max_h rows; returns 0, or -1 if fast memory was refused (then the
 * frame is rendered straight into fb, still on all cores). */
int vs_mt_init(vs_mt *mt, const vs_mt_platform *p, int max_h);
void vs_mt_free(vs_mt *mt);
/* Render a whole frame into fb (column-major, v->w * v->h, 64-byte aligned, v->h even). */
void vs_mt_frame(vs_mt *mt, const vs_view *v, const vs_world *wd, const vs_sky *sky, const vs_cam *cam,
                 uint16_t *fb, vs_stats *total);

#endif
