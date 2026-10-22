/* voxel_nv - VoxelSpaceNG on NucleoOS: loading, both cores, SRAM strips + AXI-GDMA. See voxel_nv.h.
 *
 * Threads: the caller (the app's WASM worker) renders strips as core 0 while one helper task, created
 * on first use and kept, renders as core 1. The helper is not pinned (the SMP scheduler puts it on the
 * other core), runs at the caller's priority and has a PSRAM stack: it never touches flash. Same
 * scheme as Vertice (components/vertice), which measured it at 1.7x one core.
 *
 * Memory: world, mips, sky and view tables in PSRAM (a 1024^2 world is 4 MB + 1.4 MB of mips); per
 * core two strip buffers of 8 columns in internal DMA RAM (8 x 600 x 2 B = 9.6 KB each, 38 KB in all),
 * only while plenty of internal RAM stays free (the Wi-Fi driver dies of SRAM famine). Strips are
 * moved into the PSRAM canvas by nv_2d_copy_start: cached CPU writes to PSRAM run at ~33 MB/s a core,
 * the DMA does it in long bursts while the core renders the next strip.
 */
#include "voxel_nv.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nv_2d.h"
#include "nv_log.h"
#include "vsng.h"
#include "vsng_mt.h"

static const char *TAG = "voxel";

#define STRIP_W        8
#define SRAM_RESERVE   (96 * 1024)

typedef struct {
    uint8_t *world_file;                 /* whole .vsw (header + tiled cells + detail) */
    uint16_t *mips;
    uint16_t *sram_mip[VS_MAX_MIPS];     /* internal-RAM copies of the small mips (sram_tier) */
    uint16_t *sram_pal;
    uint8_t *maxb;                       /* block maxima for empty-space skipping (internal RAM if it fits) */
    int sram_levels;
    uint8_t *sky_file;
    vs_world wd;
    vs_sky sky;
    vs_view *view;                       /* PSRAM: ~25 KB of tables */
    vs_cam cam;
    vs_governor gov;
    int fov, cores, target_ms, direct;
    int xscale, xscale_cfg;              /* traced columns: in use / forced (0 = auto) */
    int view_cols, view_rows;
    int fast;
    vs_stats last;
    int32_t last_us;
    uint32_t frames;
    int64_t prof_us;
    int64_t wait_us[VS_MT_MAX_CORES];    /* time the cores spent waiting for their DMA copies */
    bool open;
} voxel_state;

/* The state lives in PSRAM (allocated on the first open, kept): internal RAM is budgeted for drivers
 * (tools/ci/check_budgets.py). Every entry point but voxel_open returns early while it is NULL. */
static voxel_state *s_vp;
#define s_v (*s_vp)
static vs_mt s_mt;                       /* internal RAM: its strip counter is an atomic shared by the cores */

/* ---- helper task (core 1) ---- */
static TaskHandle_t s_helper;
static SemaphoreHandle_t s_go, s_done;
static void (*s_job)(void *, int);
static void *s_job_arg;

static void helper_task(void *arg)
{
    (void)arg;
    for (;;) {
        xSemaphoreTake(s_go, portMAX_DELAY);
        s_job(s_job_arg, 1);
        xSemaphoreGive(s_done);
    }
}

static bool helper_start(void)
{
    if (s_helper) return true;
    if (!s_go) s_go = xSemaphoreCreateBinary();
    if (!s_done) s_done = xSemaphoreCreateBinary();
    if (!s_go || !s_done) return false;
    return xTaskCreatePinnedToCoreWithCaps(helper_task, "vs_strip", 8 * 1024, NULL,
                                           uxTaskPriorityGet(NULL), &s_helper, tskNO_AFFINITY,
                                           MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) == pdPASS;
}

static void nv_run_parallel(void *ctx, void (*job)(void *, int), void *arg)
{
    (void)ctx;
    if (!helper_start()) { job(arg, 0); job(arg, 1); return; }
    vTaskPrioritySet(s_helper, uxTaskPriorityGet(NULL));
    s_job = job;
    s_job_arg = arg;
    xSemaphoreGive(s_go);
    job(arg, 0);
    xSemaphoreTake(s_done, portMAX_DELAY);
}

/* ---- fast buffers + DMA ---- */
static SemaphoreHandle_t s_copy_done[VS_MT_MAX_CORES][2];
static bool s_copy_dma[VS_MT_MAX_CORES][2];

static void *nv_alloc_fast(void *ctx, size_t n, int core)
{
    (void)ctx; (void)core;
    if (heap_caps_get_free_size(MALLOC_CAP_INTERNAL) < n + SRAM_RESERVE) return NULL;
    if (heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA) < n) return NULL;
    return heap_caps_aligned_alloc(64, n, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA | MALLOC_CAP_8BIT);
}

static void nv_free_fast(void *ctx, void *p) { (void)ctx; heap_caps_free(p); }

static void nv_copy_start(void *ctx, void *dst, const void *src, size_t n, int core, int slot)
{
    (void)ctx;
    if (!s_copy_done[core][slot]) s_copy_done[core][slot] = xSemaphoreCreateBinary();
    s_copy_dma[core][slot] = s_copy_done[core][slot] &&
                             nv_2d_copy_start(dst, src, n, s_copy_done[core][slot]) == ESP_OK;
    if (!s_copy_dma[core][slot]) memcpy(dst, src, n);   /* DMA refused or retired: copy it ourselves */
}

static void nv_copy_wait(void *ctx, int core, int slot)
{
    (void)ctx;
    if (s_copy_dma[core][slot]) {
        const int64_t t0 = esp_timer_get_time();
        nv_2d_copy_wait(s_copy_done[core][slot], 100);
        s_v.wait_us[core] += esp_timer_get_time() - t0;   /* s_vp is set while rendering */
    }
    s_copy_dma[core][slot] = false;
}

/* ---- files ---- */
static uint8_t *load_psram(const char *path, size_t *len)
{
    /* read() in 32 KB pieces straight into a cache-line aligned PSRAM buffer, like the shell's cp:
     * every piece starts on an aligned address and a sector boundary, so the SD driver does multi-block
     * DMA. (A single unbuffered fread of the whole file was ~65 KB/s, buffered stdio ~0.6 MB/s.) */
    const size_t kPiece = 32 * 1024;
    struct stat st;
    uint8_t *p = NULL;
    size_t n, off = 0;
    int fd = open(path, O_RDONLY);
    if (fd < 0) return NULL;
    if (fstat(fd, &st) == 0 && st.st_size > 16) {
        n = (size_t)st.st_size;
        p = (uint8_t *)heap_caps_aligned_alloc(64, (n + kPiece - 1) & ~(kPiece - 1), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        while (p && off < n) {
            const ssize_t r = read(fd, p + off, kPiece);
            if (r <= 0) { heap_caps_free(p); p = NULL; break; }
            off += (size_t)r;
        }
        if (p) *len = n;
    }
    close(fd);
    return p;
}

void voxel_close(void)
{
    int l;
    if (!s_vp) return;
    vs_mt_free(&s_mt);
    for (l = 0; l < VS_MAX_MIPS; l++) if (s_v.sram_mip[l]) heap_caps_free(s_v.sram_mip[l]);
    if (s_v.sram_pal) heap_caps_free(s_v.sram_pal);
    if (s_v.maxb) heap_caps_free(s_v.maxb);
    if (s_v.world_file) heap_caps_free(s_v.world_file);
    if (s_v.mips) heap_caps_free(s_v.mips);
    if (s_v.sky_file) heap_caps_free(s_v.sky_file);
    if (s_v.view) heap_caps_free(s_v.view);
    memset(&s_v, 0, sizeof s_v);
}

bool voxel_is_open(void) { return s_vp && s_v.open; }

/* SRAM tier: the small mips (what the far half of every ray reads) and the palette are copied into
 * internal RAM, smallest first, while enough stays free for the strip buffers, the Wi-Fi driver and
 * the rest of the OS. Internal RAM answers in a few cycles, PSRAM after a cache miss of ~100 ns.
 * Freed on close; the PSRAM copies stay, so nothing breaks if a level did not fit. */
static void sram_tier(void)
{
    /* left free: the OS reserve, the strip buffers (2 per core, 8 columns x 1024 rows each), a margin */
    const size_t keep = SRAM_RESERVE + (size_t)STRIP_W * 1024 * 2 * 2 * 2 + 16 * 1024;
    int l, n = 0;
    size_t used = 0;
    uint16_t *p;
    /* first the block maxima: every step of every ray reads them */
    const int sh = vs_maxb_shift(s_v.wd.log2size);
    const size_t mb = vs_maxb_bytes(s_v.wd.log2size, sh);
    const bool mb_sram = heap_caps_get_free_size(MALLOC_CAP_INTERNAL) >= mb + keep;
    s_v.maxb = (uint8_t *)heap_caps_malloc(mb, (mb_sram ? MALLOC_CAP_INTERNAL : MALLOC_CAP_SPIRAM) | MALLOC_CAP_8BIT);
    if (s_v.maxb) vs_build_maxb(&s_v.wd, s_v.maxb, sh);
    p = (uint16_t *)heap_caps_malloc(VS_PAL_N * 2, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (p) { memcpy(p, s_v.wd.pal, VS_PAL_N * 2); s_v.sram_pal = p; s_v.wd.pal = p; }
    for (l = s_v.wd.nmips - 1; l >= 1; l--) {
        const size_t bytes = ((size_t)2 << (2 * (s_v.wd.log2size - l)));
        if (heap_caps_get_free_size(MALLOC_CAP_INTERNAL) < bytes + keep) break;
        if (heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL) < bytes) break;
        p = (uint16_t *)heap_caps_aligned_alloc(32, bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        if (!p) break;
        memcpy(p, s_v.wd.mip[l], bytes);
        s_v.sram_mip[l] = p;
        s_v.wd.mip[l] = p;
        used += bytes;
        n++;
    }
    s_v.sram_levels = n;
    NV_LOGI(TAG, "sram tier: block maxima %u KB in %s, %d mip level(s) %u KB in internal RAM, %u KB left",
            (unsigned)(mb >> 10), s_v.maxb ? (mb_sram ? "internal RAM" : "PSRAM") : "nowhere (no skipping)", n,
            (unsigned)(used >> 10), (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) >> 10));
}

int voxel_open(const char *world_path, const char *sky_path)
{
    size_t wl = 0, sl = 0, cells;
    uint32_t hd[7];
    uint16_t sh[6];
    int64_t t0 = esp_timer_get_time(), t1;
    if (!world_path || !sky_path) return -1;
    if (!s_vp && !(s_vp = (voxel_state *)heap_caps_calloc(1, sizeof *s_vp, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)))
        return -4;
    voxel_close();
    s_v.world_file = load_psram(world_path, &wl);
    s_v.sky_file = load_psram(sky_path, &sl);
    if (!s_v.world_file || !s_v.sky_file) { voxel_close(); return -2; }
    t1 = esp_timer_get_time();
    if (wl < 32 + VS_PAL_N * 2 || memcmp(s_v.world_file, "VSW3", 4) || sl < 16 || memcmp(s_v.sky_file, "VSS2", 4)) {
        voxel_close();
        return -3;
    }
    memcpy(hd, s_v.world_file + 4, 28);
    cells = hd[0] >= 4 && hd[0] <= 12 ? (size_t)1 << (2 * hd[0]) : 0;
    if (!cells || wl < 32 + VS_PAL_N * 2 + cells * 2 + (size_t)hd[5] * 4096) { voxel_close(); return -3; }
    s_v.wd.log2size = (int)hd[0];
    s_v.wd.water = (int)hd[1];
    s_v.wd.maxh = (int)hd[2];
    s_v.wd.vscale_q8 = (int)hd[3];
    s_v.wd.zfar = (int)hd[4];
    s_v.wd.pal = (const uint16_t *)(s_v.world_file + 32);
    s_v.wd.mip[0] = (const uint16_t *)(s_v.world_file + 32 + VS_PAL_N * 2);
    s_v.wd.detail = hd[5] >= VS_DETAIL_MATS ? (const int8_t *)(s_v.world_file + 32 + VS_PAL_N * 2 + cells * 2) : NULL;
    memcpy(sh, s_v.sky_file + 4, 12);
    if (sl < 16 + (size_t)sh[0] * sh[1] * 2 || !sh[0] || !sh[1]) { voxel_close(); return -3; }
    s_v.sky.w = sh[0]; s_v.sky.h = sh[1]; s_v.sky.top = sh[2]; s_v.sky.fog = sh[3]; s_v.sky.focal = sh[4];
    s_v.sky.px = (const uint16_t *)(s_v.sky_file + 16);

    s_v.mips = (uint16_t *)heap_caps_aligned_alloc(64, (size_t)vs_mip_cells(s_v.wd.log2size) * 2,
                                                   MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_v.view = (vs_view *)heap_caps_calloc(1, sizeof(vs_view), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_v.mips || !s_v.view) { voxel_close(); return -4; }
    vs_build_mips(&s_v.wd, s_v.mips);
    sram_tier();
    /* the world sets the view's scale: distance, height unit, near zone (in its own cells) */
    s_v.view->zfar = s_v.wd.zfar > 0 ? s_v.wd.zfar : 1100;
    s_v.view->vscale_q8 = s_v.wd.vscale_q8 > 0 ? s_v.wd.vscale_q8 : 205;
    s_v.view->near_z = 48 << s_v.wd.log2size >> 10;
    s_v.fov = 80;
    s_v.cores = 2;
    s_v.open = true;
    NV_LOGI(TAG, "open: world %d^2, water %d, maxh %d, %u KB (read %d ms) + mips %u KB, %d ms in all",
            1 << s_v.wd.log2size, s_v.wd.water, s_v.wd.maxh, (unsigned)(wl >> 10), (int)((t1 - t0) / 1000),
            (unsigned)(vs_mip_cells(s_v.wd.log2size) >> 9), (int)((esp_timer_get_time() - t0) / 1000));
    return 0;
}

static void mt_setup(int rows)
{
    vs_mt_platform p;
    memset(&p, 0, sizeof p);
    p.ncores = s_v.cores;
    p.strip_w = STRIP_W;
    p.run_parallel = nv_run_parallel;
    if (!s_v.direct) {                       /* VOXEL_CFG_DIRECT: strips straight into the canvas */
        p.alloc_fast = nv_alloc_fast;
        p.free_fast = nv_free_fast;
        p.copy_start = nv_copy_start;
        p.copy_wait = nv_copy_wait;
    }
    vs_mt_free(&s_mt);
    s_v.fast = vs_mt_init(&s_mt, &p, rows) == 0 && s_mt.buf[0][0];
}

void voxel_camera(int32_t x, int32_t y, int32_t h, int32_t yaw, int32_t pitch, int32_t roll, int32_t time_ms)
{
    if (!s_vp) return;
    s_v.cam.x = x; s_v.cam.y = y; s_v.cam.h = h;
    s_v.cam.yaw = yaw & VS_ANG_MASK; s_v.cam.pitch = pitch; s_v.cam.roll = roll; s_v.cam.time_ms = time_ms;
}

int32_t voxel_render(uint16_t *fb, int cols, int rows)
{
    int64_t t0;
    if (!s_vp || !s_v.open || !fb || cols < 16 || rows < 16 || cols > VS_MAX_W || rows > VS_MAX_H || (rows & 1))
        return -1;
    {
        /* traced columns: forced by VOXEL_CFG_XSCALE, else the governor's render scale, else half rate on
         * wide screens (the in-between columns are interpolated: ~1.8x faster, hard to tell apart) */
        int xs = s_v.xscale_cfg ? s_v.xscale_cfg : s_v.target_ms ? (s_v.gov.scale_q8 <= 192 ? 2 : 1)
                                                 : cols >= 800 ? 2 : 1;
        if (cols != s_v.view_cols || rows != s_v.view_rows || xs != s_v.xscale) {
            const int resized = cols != s_v.view_cols || rows != s_v.view_rows;
            vs_view_fit(s_v.view, cols, rows, s_v.fov, xs);
            s_v.view_cols = cols;
            s_v.view_rows = rows;
            s_v.xscale = xs;
            if (resized) mt_setup(rows);
            if (resized && s_v.target_ms) {
                vs_governor_init(&s_v.gov, s_v.target_ms, 128);
                if (cols >= 800) s_v.gov.scale_q8 = 128;   /* wide screens start interpolated: near lossless */
            }
        }
        s_mt.xscale = xs;
    }
    t0 = esp_timer_get_time();
    vs_mt_frame(&s_mt, s_v.view, &s_v.wd, &s_v.sky, &s_v.cam, fb, &s_v.last);
    s_v.last_us = (int32_t)(esp_timer_get_time() - t0);
    if (s_v.target_ms) vs_tune(&s_v.gov, s_v.view, s_v.last_us / 1000);
    s_v.prof_us += s_v.last_us;
    if (++s_v.frames % 60 == 0) {
        NV_LOGI(TAG, "prof %dx%d cores %d fast %d xs %d: render %d us avg, dma wait %d/%d us, samples %u, near px %u, "
                "q %d, strips %u/%u", cols, rows, s_v.cores, s_v.fast, s_v.xscale, (int)(s_v.prof_us / 60),
                (int)(s_v.wait_us[0] / 60), (int)(s_v.wait_us[1] / 60), (unsigned)s_v.last.samples,
                (unsigned)s_v.last.pixels_near, s_v.view->quality, (unsigned)s_mt.strips_done[0],
                (unsigned)s_mt.strips_done[1]);
        s_v.prof_us = 0;
        s_v.wait_us[0] = s_v.wait_us[1] = 0;
    }
    return s_v.last_us;
}

int32_t voxel_height(int32_t x, int32_t y)
{
    return voxel_is_open() ? vs_height_at(&s_v.wd, x, y) : 0;
}

int32_t voxel_config(int32_t key, int32_t value)
{
    vs_view *v;
    if (!voxel_is_open() || !(v = s_v.view)) return -1;
    switch (key) {
    case VOXEL_CFG_FOV:
        if (value >= 30 && value <= 140) { s_v.fov = value; s_v.view_cols = 0; }
        return s_v.fov;
    case VOXEL_CFG_ZFAR:
        if (value >= 64 && value <= 4096) { v->zfar = value; v->fog_start = 0; s_v.view_cols = 0; }
        return v->zfar;
    case VOXEL_CFG_QUALITY:
        if (value >= 64 && value <= 512) { v->quality = value; vs_setup(v); }
        return v->quality;
    case VOXEL_CFG_NEAR:
        if (value >= 0 && value <= 512) { v->near_z = value; vs_setup(v); }
        return v->near_z;
    case VOXEL_CFG_SMOOTH:
        if (value >= 0 && value <= 4096) { v->smooth_z = value; vs_setup(v); }
        return v->smooth_z;
    case VOXEL_CFG_DETAIL:
        if (value >= 0 && value <= 1024) v->detail_q8 = value;
        return v->detail_q8;
    case VOXEL_CFG_TARGET_MS:
        if (value >= 0 && value <= 1000) {
            s_v.target_ms = value;
            if (value) {
                vs_governor_init(&s_v.gov, value, 128);
                if (s_v.view_cols >= 800) s_v.gov.scale_q8 = 128;
            }
        }
        return s_v.target_ms;
    case VOXEL_CFG_CORES:
        if ((value == 1 || value == 2) && value != s_v.cores) {
            s_v.cores = value;                       /* only the scheduler changes, the view stays */
            if (s_v.view_rows) mt_setup(s_v.view_rows);
        }
        return s_v.cores;
    case VOXEL_CFG_DIRECT:
        if ((value == 0 || value == 1) && value != s_v.direct) {
            s_v.direct = value;
            if (s_v.view_rows) mt_setup(s_v.view_rows);
        }
        return s_v.direct;
    case VOXEL_CFG_XSCALE:
        if (value >= 0 && value <= 2) s_v.xscale_cfg = value;
        return s_v.xscale_cfg;
    case VOXEL_CFG_FOG:
        if (value >= 0 && value < v->zfar) { v->fog_start = value; vs_setup(v); }
        return v->fog_start;
    default:
        return -1;
    }
}

int32_t voxel_info(int32_t key)
{
    if (!s_vp) return -1;
    switch (key) {
    case VOXEL_INFO_LOG2SIZE: return s_v.wd.log2size;
    case VOXEL_INFO_WATER: return s_v.wd.water;
    case VOXEL_INFO_MAXH: return s_v.wd.maxh;
    case VOXEL_INFO_SAMPLES: return (int32_t)s_v.last.samples;
    case VOXEL_INFO_NEAR_PX: return (int32_t)s_v.last.pixels_near;
    case VOXEL_INFO_RENDER_US: return s_v.last_us;
    case VOXEL_INFO_QUALITY: return s_v.view ? s_v.view->quality : 0;
    case VOXEL_INFO_FAST: return s_v.fast;
    case VOXEL_INFO_XSCALE: return s_v.xscale;
    case VOXEL_INFO_SRAM_LEVELS: return s_v.sram_levels;
    default: return -1;
    }
}
