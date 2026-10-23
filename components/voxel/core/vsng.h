/* VoxelSpaceNG core: heightfield terrain renderer, portable C99, integer-only frame loop.
 *
 * Output is COLUMN-MAJOR and bottom-up: column x of the screen is out[x * h .. x * h + h - 1], and
 * element j of a column is screen row (h - 1 - j). The march writes each column as one contiguous
 * run (good for caches and PSRAM), and on the ESP32-P4 the PPA turns that buffer upright for free.
 *
 * World: square map of 2^n x 2^n 16-bit cells, wraps, stored in 4x4-cell tiles (vs_tidx: one tile is
 * 32 bytes, so a ray in any direction reuses what it fetched), plus a mip pyramid (vs_build_mips) the
 * march switches to with distance. Cell = HHHHHHHHH IIIIIII: H = height in half units (0..511), I =
 * colour index into the world palette of 128 entries (lighting baked). The index also says what the
 * cell is: group = I >> 4 (8 groups of 16 shades), group VS_G_WATER is water; the group picks the
 * detail texture drawn up close. Half the bytes of an RGB565 cell: twice the world in the same RAM.
 *
 * Threads: vs_render_strip renders columns [x0, x1) into out, where out holds exactly those columns
 * (column x at out + (x - x0) * h). Columns are independent, so cores share a frame by taking strips
 * from a counter. Because the frame is column-major, a strip is ONE contiguous block of the frame:
 * render it into fast internal RAM and move it with a single linear (DMA) copy (see vsng_mt.h).
 */
#ifndef VSNG_H
#define VSNG_H

#include <stdint.h>

#define VS_G_WATER    7                  /* colour group of water cells */
#define VS_PAL_N      128                /* palette entries: 8 groups x 16 shades */

#define VS_ANG_BITS 12                   /* angles are 0..4095 for a full turn */
#define VS_ANG_FULL (1 << VS_ANG_BITS)
#define VS_ANG_MASK (VS_ANG_FULL - 1)

#define VS_MAX_STEPS 640
#define VS_MAX_W     2048
#define VS_MAX_H     2048
#define VS_MAX_MIPS  7

#define VS_DETAIL_N    64                /* detail tile side, texels */
#define VS_DETAIL_MATS 8                 /* one per colour group */

/* Index of cell (x, y) in a tiled map of side 1 << log2 (x, y already wrapped). */
static inline uint32_t vs_tidx(uint32_t x, uint32_t y, int log2)
{
    return ((y >> 2) << (log2 + 2)) | ((x >> 2) << 4) | ((y & 3) << 2) | (x & 3);
}

typedef struct {
    const uint16_t *mip[VS_MAX_MIPS];    /* mip[0] = the map, mip[l] is (size >> l) a side, tiled */
    const uint16_t *pal;                 /* VS_PAL_N RGB565 colours */
    int nmips;
    int log2size;                        /* map is (1 << log2size) cells a side */
    int water;                           /* water level, height units (0..255) */
    int maxh;                            /* highest cell, height units (early ray exit) */
    const int8_t *detail;                /* VS_DETAIL_MATS tiles of 64x64 signed luminance, or NULL */
    const uint8_t *maxb;                 /* max height (units) of each block, dilated; NULL = no skipping */
    int maxb_shift;                      /* block side = 1 << maxb_shift cells (vs_build_maxb) */
    const uint8_t *maxs;                 /* super-block maxima (4x4 blocks, 3x3-dilated); NULL = none */
    int vscale_q8;                       /* one height unit in cells, Q8 (the view takes it) */
    int zfar;                            /* suggested view distance, cells */
} vs_world;

typedef struct {
    const uint16_t *px;                  /* column-major: px[col * h + row], row 0 = horizon */
    int w, h;                            /* w columns = 360 degrees */
    uint16_t top;                        /* colour above the panorama */
    uint16_t fog;                        /* horizon haze colour (fog blends into it) */
    int focal;                           /* px per radian the band was baked for (rows scale to the view) */
} vs_sky;

typedef struct {
    int32_t x, y;                        /* position, Q16 cells */
    int32_t h;                           /* eye height, Q4 height units */
    int32_t yaw;                         /* 0..4095; 0 looks along +x, 1024 along +y */
    int32_t pitch;                       /* horizon shift in px, + = look down */
    int32_t roll;                        /* horizon tilt, Q16 px per px from the centre column */
    int32_t time_ms;                     /* animates the water */
} vs_cam;

typedef struct {
    int w, h;                            /* screen size, w <= VS_MAX_W, h <= VS_MAX_H */
    int focal;                           /* px per traced column; horizontal fov = 2 * atan(w / 2 / focal) */
    int focal_y;                         /* px vertically (0 = focal); with half-rate columns twice focal */
    int xscale;                          /* frame columns per traced column, 1, 2 or 4 (vsng_mt widens) */
    int zfar;                            /* cells */
    int vscale_q8;                       /* height unit vs cell size, Q8 (256 = 1 cell) */
    int fog_start;                       /* cells where haze starts */
    int near_z;                          /* cells: per-pixel texturing (bilinear + detail) nearer */
    int smooth_z;                        /* cells: bilinear samples (colour + height) nearer; flat beyond */
    int lod_q8;                          /* mip bias, Q8: 256 = level l once a step spans 2^l cells */
    int detail_q8;                       /* detail texture strength, Q8 (0 = off) */
    int quality;                         /* step density, Q8 (256 = default; lower = faster) */
    /* filled by vs_setup */
    int nsteps, nnear, nsmooth;
    int32_t dz_q8[VS_MAX_STEPS];         /* step length, Q8 cells */
    int32_t kz[VS_MAX_STEPS];            /* focal * vscale / z, Q8 */
    uint8_t fog[VS_MAX_STEPS];           /* 0..32 */
    uint8_t fres[VS_MAX_STEPS];          /* water's sky share by distance (Fresnel), 0..32 */
    uint8_t lod[VS_MAX_STEPS];           /* mip level wanted at this step */
    uint8_t lodf[VS_MAX_STEPS];          /* blend toward level + 1, 0..31 (second half of each octave) */
    int32_t col_dx_q16[VS_MAX_W];        /* (x - cx) / focal, Q16 */
    int16_t col_ang[VS_MAX_W];           /* atan((x - cx) / focal) in 4096ths of a turn */
    uint16_t recip[VS_MAX_H + 1];        /* 65535 / n */
    int nopin;                           /* 1 = steps follow the camera (no world pinning) */
    int32_t pdz[VS_MAX_STEPS];           /* this frame's step lengths, pinned to world planes (vs_frame) */
    int32_t pkz[VS_MAX_STEPS];           /* ...and their 1/z factors */
    int pinned;                          /* pdz/pkz are valid for the current frame */
    int32_t pz[VS_MAX_STEPS];            /* this frame's depth of each step, Q8 cells (vs_frame) */
} vs_view;

typedef struct {
    uint32_t samples;                    /* map reads (steps) */
    uint32_t pixels_near;                /* per-pixel textured */
    uint32_t pixels_sky;
    uint32_t cyc[7];                     /* -DVS_CYCLES: cycles in setup, sampling, water, near, flat, ridge AA, sky */
} vs_stats;

void vs_init_tables(void);
void vs_setup(vs_view *v);

/* Any screen: fills a view for a w x h frame with a horizontal fov (degrees) and sane defaults scaled to
 * the resolution, then vs_setup. xscale 2 (or 4) traces every other (every fourth) column: w / xscale
 * rays; set vs_mt.xscale the same, it interpolates the columns in between. Call again on a change. */
void vs_view_fit(vs_view *v, int w, int h, int fov_deg, int xscale);

/* Adaptive quality: feed the last frame time; it moves view quality/zfar/near_z (and suggests a
 * render scale in *scale_q8, 256 = native, the platform applies it) to hold target_ms. Returns 1 when
 * the view changed (vs_setup already called). Cheap: call once per frame. */
typedef struct {
    int target_ms;                       /* frame budget */
    int scale_q8, scale_min_q8;          /* render scale suggested / lowest allowed */
    int32_t avg_ms_q4;                   /* smoothed frame time, Q4 */
    int cooldown;                        /* frames before the next change */
    int q_min;                           /* lowest step density it may use (0 = 192) */
    int near0;                           /* the near zone it started from (set on the first call) */
} vs_governor;
void vs_governor_init(vs_governor *g, int target_ms, int scale_min_q8);
int vs_tune(vs_governor *g, vs_view *v, int frame_ms);

/* Mip pyramid: fills wd->mip[1..] from mip[0] into buf (vs_mip_cells(log2size) cells, caller owns). */
uint32_t vs_mip_cells(int log2size);
void vs_build_mips(vs_world *wd, uint16_t *buf);

/* Block maxima for empty-space skipping: the highest cell of every (1 << shift)^2 block, dilated by
 * two cells so a bilinear sample's neighbours are covered too. A sample whose block top could not
 * rise above what the column already shows is not read at all (measured: 40-50% of the map reads).
 * vs_maxb_shift picks ~128 blocks a side (16 KB, small enough for internal RAM). */
int vs_maxb_shift(int log2size);
uint32_t vs_maxb_bytes(int log2size, int shift);
void vs_build_maxb(vs_world *wd, uint8_t *buf, int shift);

/* Super-block maxima for early ray exit: 4x4 blocks of vs_build_maxb, each the max of itself and its
 * 8 neighbours. Each ray reads them along its path once (every super-block side) to know the tallest
 * terrain still ahead, and stops as soon as even that could not show (a ray that sees sky used to
 * march all the way to zfar). Call after vs_build_maxb. */
uint32_t vs_maxs_bytes(int log2size, int shift);
void vs_build_maxs(vs_world *wd, uint8_t *buf);

/* Once per frame, before the strips: pins the step depths to planes fixed in the world along the view
 * direction (a lattice of spacing ~dz per step), so moving forward slides the camera through the
 * sample planes instead of dragging them along: far terrain stops crawling (Structured Volume
 * Sampling, "forward pinning", SIGGRAPH 2015; measured -34% frame-to-frame flicker, no cost). */
void vs_frame(vs_view *v, const vs_cam *cam);

void vs_render_strip(const vs_view *v, const vs_world *wd, const vs_sky *sky, const vs_cam *cam,
                     uint16_t *out, int x0, int x1, vs_stats *st);
#define vs_render(v, wd, sky, cam, out, st) (vs_frame(v, cam), vs_render_strip(v, wd, sky, cam, out, 0, (v)->w, st))

/* Queries (gameplay / physics). */
int32_t vs_height_at(const vs_world *wd, int32_t x_q16, int32_t y_q16);   /* Q4, bilinear */
int32_t vs_sin(int32_t ang);             /* Q16 */
int32_t vs_cos(int32_t ang);

#endif
