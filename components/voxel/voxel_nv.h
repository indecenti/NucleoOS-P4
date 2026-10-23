/* voxel_nv - the VoxelSpaceNG engine inside NucleoOS (ESP32-P4), rendering on both cores.
 *
 * One world at a time, owned by the running WASM app (nv_wasm_vs.cpp binds it to ABI 20 and closes
 * it when the app ends). Frames are column-major (see vsng.h): on a quarter-turned canvas
 * (nv_gfx_set_rotation(1)) the canvas buffer IS that layout, and the PPA shows it upright.
 */
#ifndef VOXEL_NV_H
#define VOXEL_NV_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {                                   /* voxel_config keys */
    VOXEL_CFG_FOV = 0,                   /* degrees, horizontal */
    VOXEL_CFG_ZFAR = 1,                  /* cells */
    VOXEL_CFG_QUALITY = 2,               /* step density, Q8 (256 default) */
    VOXEL_CFG_NEAR = 3,                  /* textured near zone, cells */
    VOXEL_CFG_SMOOTH = 4,                /* bilinear zone, cells */
    VOXEL_CFG_DETAIL = 5,                /* detail strength, Q8 (0 off) */
    VOXEL_CFG_TARGET_MS = 6,             /* adaptive quality frame budget (0 = off) */
    VOXEL_CFG_CORES = 7,                 /* 1 or 2 */
    VOXEL_CFG_FOG = 8,                   /* fog start, cells */
    VOXEL_CFG_DIRECT = 9,                /* 1 = render straight into the canvas (no SRAM strips + DMA) */
    VOXEL_CFG_XSCALE = 10,               /* traced columns: 0 auto, 1 every column, 2 every other (interpolated) */
};

enum {                                   /* voxel_info keys */
    VOXEL_INFO_LOG2SIZE = 0,
    VOXEL_INFO_WATER = 1,
    VOXEL_INFO_MAXH = 2,
    VOXEL_INFO_SAMPLES = 3,              /* last frame */
    VOXEL_INFO_NEAR_PX = 4,              /* last frame */
    VOXEL_INFO_RENDER_US = 5,            /* last frame */
    VOXEL_INFO_QUALITY = 6,              /* now (the governor moves it) */
    VOXEL_INFO_FAST = 7,                 /* 1 = SRAM strips + DMA, 0 = straight into the canvas */
    VOXEL_INFO_XSCALE = 8,               /* traced columns in use, 1 or 2 */
    VOXEL_INFO_SRAM_LEVELS = 9,          /* mip levels copied into internal RAM */
    VOXEL_INFO_VSCALE = 10,              /* one height unit in cells, Q8 (for projecting your own 3D) */
    VOXEL_INFO_FOCAL = 11,               /* vertical focal length in px of the last frame */
};

/* Load a world (.vsw, VSW3: 16-bit palette cells) and a sky (.vss) from the SD card into PSRAM. 0, or <0: -1 busy/args,
 * -2 file, -3 format, -4 memory. Must run on the thread that renders. */
int voxel_open(const char *world_path, const char *sky_path);
void voxel_close(void);
bool voxel_is_open(void);

void voxel_camera(int32_t x, int32_t y, int32_t h, int32_t yaw, int32_t pitch, int32_t roll, int32_t time_ms);
/* Render into fb: cols x rows, column-major bottom-up, 64-byte aligned, rows even. Returns the render
 * time in microseconds, or <0. */
int32_t voxel_render(uint16_t *fb, int cols, int rows);
int32_t voxel_height(int32_t x, int32_t y);
int32_t voxel_config(int32_t key, int32_t value);   /* returns the value now in effect, <0 bad key */
int32_t voxel_info(int32_t key);

#ifdef __cplusplus
}
#endif

#endif
