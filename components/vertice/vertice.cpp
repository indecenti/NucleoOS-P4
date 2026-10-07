// vertice.cpp — Vertice, the NucleoOS 3D engine: scene handles, dual-core band renderer, particles,
// .vxm models. See include/vertice.h for the model. Builds for the P4 (FreeRTOS helper task on the
// other core) and for the PC harness (tools/vertice: std::thread helper); nothing else differs.
#ifdef ESP_PLATFORM
#include "esp_attr.h"
#endif
#include "vertice.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "Camera.hpp"
#include "Light.hpp"
#include "Material.hpp"
#include "Object.hpp"
#include "Picking.hpp"
#include "Primitives.hpp"
#include "Scene.hpp"
#include "Texture.hpp"
#include "TrigLUT.hpp"

#ifdef ESP_PLATFORM
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "freertos/idf_additions.h"
static const char *TAG = "vertice";
#include "nv_log.h"   // NV_LOG reaches /api/logs (plain ESP_LOG does not)
#include "nv_2d.h"    // nv_2d_copy: AXI-GDMA burst copy of finished tiles into the canvas
#define VX_LOGI(...) NV_LOGI(TAG, __VA_ARGS__)
#define VX_LOGW(...) NV_LOGW(TAG, __VA_ARGS__)
static inline int64_t now_us(void) { return esp_timer_get_time(); }
static void *psram_calloc(size_t n) { return heap_caps_aligned_calloc(64, 1, n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT); }
static void psram_free(void *p) { heap_caps_free(p); }
static size_t psram_largest(void) { return heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM); }
// Internal SRAM for the raster tiles (DMA-capable: the GDMA copy reads them). Only while a scene is
// open, and only if a healthy reserve stays free for Wi-Fi / DMA (docs/ENGINEERING_RULES.md).
static void *sram_alloc(size_t n) {
    constexpr size_t kReserve = 96 * 1024;
    const size_t caps = MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA | MALLOC_CAP_8BIT;
    if (heap_caps_get_free_size(caps) < n + kReserve || heap_caps_get_largest_free_block(caps) < n) return nullptr;
    return heap_caps_aligned_alloc(64, n, caps);
}
static void sram_free(void *p) { heap_caps_free(p); }
#else
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <thread>
#define VX_LOGI(...) (std::printf("vertice: " __VA_ARGS__), std::printf("\n"))
#define VX_LOGW(...) (std::printf("vertice: W " __VA_ARGS__), std::printf("\n"))
#ifdef VX_SIM_CLOCK   // the simulator drives a deterministic clock (particles advance per frame)
extern "C" int64_t vx_sim_clock_us(void);
static inline int64_t now_us(void) { return vx_sim_clock_us(); }
#else
static inline int64_t now_us(void) {
    return std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}
#endif
static void *psram_calloc(size_t n) { return std::calloc(1, n); }
static void psram_free(void *p) { std::free(p); }
static size_t psram_largest(void) { return (size_t)64 << 20; }
static void *sram_alloc(size_t n) { return std::calloc(1, n); }
static void sram_free(void *p) { std::free(p); }
extern "C" size_t vx_mem_used(void) { return 0; }   // the PC harness doesn't meter its heap
#endif

using namespace Renderer;
#include <atomic>

// Runtime fog (JetConfig.hpp maps the core's depthFogNear/Far/InvQ16 here). "Off" parks both past
// any far plane the depth buffer can express (65535).
int32_t vx_fog_near_z = 1 << 20, vx_fog_far_z = (1 << 20) + 1, vx_fog_inv_q16 = 255 << 16;
static void set_fog(int32_t znear, int32_t zfar) {
    vx_fog_near_z = znear;
    vx_fog_far_z = zfar;
    vx_fog_inv_q16 = (int32_t)(((int64_t)255 << 16) / (zfar - znear));
}

namespace {

// ---- particles ---------------------------------------------------------------------------------
struct Particle { float x, y, z, vx, vy, vz, age; };   // age 0 -> 1 (dies at 1)
struct Emitter {
    bool      used = false;
    int       max = 0, n = 0;
    uint16_t  c0 = 0, c1 = 0;
    float     s0 = 0, s1 = 0, rate = 0, gravity = 0;   // rate = 1 / life seconds
    int       flags = 0;
    Particle *p = nullptr;
};
struct Sprite { int16_t x0, y0, x1, y1; uint16_t z; uint16_t color; uint8_t alpha; uint8_t flags; };
constexpr int kMaxSprites = VX_MAX_EMITTERS * VX_MAX_PARTICLES;

// ---- engine state (one scene; every public call comes from the app's worker thread) -----------
// EVERY member must be zero-initialised: the struct then sits in .bss, which linker.lf sends to
// PSRAM. A single non-zero default moves all of it (~20 KB) into internal SRAM, leaving less than
// the reserve the SRAM floor texture needs - measured as a halved frame rate on the board.
struct Engine {
    bool      open = false;
    int       w = 0, h = 0;
    Scene    *scene = nullptr;
    Camera   *cam = nullptr;
    DirectionalLight *sun = nullptr;
    AmbientLight     *amb = nullptr;
    uint16_t *zbuf = nullptr;
    uint16_t *sky = nullptr;           // per-row clear colours (h entries), follow the horizon
    uint16_t  sky_top = 0, sky_bot = 0;
    Material *defmat = nullptr;
    Object   *obj[VX_MAX_OBJECTS] = {};
    int       obj_v[VX_MAX_OBJECTS] = {}, obj_t[VX_MAX_OBJECTS] = {};   // booked verts / tris
    Material *mat[VX_MAX_MATERIALS] = {};
    Texture  *tex[VX_MAX_TEXTURES] = {};
    uint16_t *texpx[VX_MAX_TEXTURES] = {};
    int       nobj = 0, nmat = 0, ntex = 0;   // high-water marks (object slots are reused)
    int       tris = 0, verts = 0;
    bool      depth = false;           // set by vx_open
    uint16_t *target = nullptr;        // this frame's colour buffer
    // particles
    Emitter   em[VX_MAX_EMITTERS];
    Sprite   *spr = nullptr;
    int       nspr = 0, live = 0;
    int64_t   last_us = 0;
    uint32_t  rng = 0;                 // seeded by vx_open
    // picking
    bool      pick_armed = false;
    PickQuery pick_q{0, 0};            // written before every arm
    int       picked = 0;              // -1 (none) from vx_open on
    // band split + stats
    int       split = 0;
    int64_t   us_total = 0, us_prep = 0, us_band[2] = {0, 0};
    int       rasterized = 0, queued = 0;
    uint8_t  *flags[2] = {nullptr, nullptr};
    int       flags_cap = 0;
    // tiled raster: per core one tile of colour + depth rows in internal SRAM (0 tiles = bands)
    int       tile_h = 0, ntiles = 0;
    uint16_t *tcol[2] = {nullptr, nullptr}, *tz[2] = {nullptr, nullptr};
    uint16_t *tcol2[2] = {nullptr, nullptr};     // the second colour buffer (async tile copy), if any
    // the camera of the last vx_render (vx_project). All zero-initialised ON PURPOSE: one non-zero
    // initialiser moves this whole struct from .bss (PSRAM, linker.lf) to .data in internal SRAM -
    // 20 KB that pushed free SRAM under the floor texture's 96 KB reserve and halved the frame rate
    // (Vertice 1.5 dev, 2026-10). Keep every member of Engine zero-initialised.
    // hierarchy and shadows (1.5): parent+1 (0 = none), the local transform, the shadow disc+1
    int16_t   parent1[VX_MAX_OBJECTS] = {};
    int32_t   lpos[VX_MAX_OBJECTS][3] = {};
    int16_t   lrot[VX_MAX_OBJECTS][3] = {};
    int16_t   shadow1[VX_MAX_OBJECTS] = {};
    int16_t   shadow_y[VX_MAX_OBJECTS] = {};
    bool      any_parent = false, any_shadow = false;
    int32_t   proj_pos[3] = {0, 0, 0};
    float     proj_f = 0.0f;
    int32_t   proj_near = 0;
    bool      proj_ok = false;
    void     *tdone[2][2] = {};                  // per core and buffer: "copy landed" semaphores
    uint16_t *tile_block = nullptr;              // the one SRAM allocation they are carved from
    size_t    tile_bytes = 0;                    // its size
    uint16_t *bin_list = nullptr;
    int       bin_cap = 0;
    uint32_t  bin_start[600 / 4 + 2] = {};   // tiles are >= 4 rows, the canvas <= 600
    // background: Mode-7 floor and 360° panorama (see clear_rows)
    bool      floor_on = false, floor_dirty = false;
    int       floor_y = 0, floor_repeat = 0, floor_w = 0, floor_h = 0, floor_wshift = 0;
    uint16_t  floor_color = 0, floor_avg = 0;
    const uint16_t *floor_src = nullptr;     // the texture's pixels (owned by its Texture)
    uint16_t *floor_lit = nullptr;           // lit copy (SRAM when available)
    uint16_t *floor_base = nullptr;          // lit copy without caustics (PSRAM, only with caustics)
    // under water (1.3): caustics shimmering on the floor, light shafts slanting down from the surface
    uint8_t  *caus = nullptr;                // two 128x128 caustic layers (PSRAM)
    int       caus_k = 0, caus_speed = 0;    // vx_caustics
    uint8_t  *shaft = nullptr;               // 512-entry ray profile across the screen
    int       shaft_k = 0, shaft_slope = 0;  // vx_shafts: strength 0..256, x shift per 64 rows
    const uint16_t *pano_px = nullptr;       // panorama pixels (owned by its Texture)
    int       pano_w = 0, pano_h = 0, pano_hrow = 0, pano_reps = 0;   // reps: times round the horizon (0 = once)
    int16_t  *pano_u = nullptr;              // per-column texel, this frame
    int       water_k = 0, water_wave = 0;   // vx_water: reflection strength (0..256), ripple px
    bool      ceil_on = false;               // vx_ceiling (1.3): a plane above, drawn like the floor
    int       ceil_y = 0, ceil_repeat = 0, ceil_w = 0, ceil_h = 0, ceil_wshift = 0;
    const uint16_t *ceil_px = nullptr;
    // level of detail: a master object may name up to VX_MAX_LODS simpler stand-ins, shown instead
    // of it beyond a camera distance (see apply_lods)
    struct Lod { int16_t id[VX_MAX_LODS]; int32_t dist[VX_MAX_LODS]; uint8_t n, cur; bool shown; };
    Lod       lod[VX_MAX_OBJECTS] = {};
    int16_t   lod_of[VX_MAX_OBJECTS] = {};    // a stand-in's master + 1 (0 = not a stand-in)
    bool      any_lod = false;
};
// Every member initialiser above is zero, so `g` is constant-initialised into .bss, which
// linker.lf maps to PSRAM. One non-zero default (rng, depth, picked, pick_q) used to drop all ~9.5 KB of it
// into internal .data instead.
Engine g;

inline int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }
inline int wrap360(int a) { return ((a % 360) + 360) % 360; }
inline bool valid_obj(int id) { return id >= 0 && id < g.nobj && g.obj[id]; }
inline Material *mat_or_default(int id) {
    return (id >= 0 && id < g.nmat && g.mat[id]) ? g.mat[id] : g.defmat;
}
inline uint32_t xrand(void) {   // xorshift32
    g.rng ^= g.rng << 13; g.rng ^= g.rng >> 17; g.rng ^= g.rng << 5;
    return g.rng;
}
inline float frand(void) { return (float)(int32_t)xrand() * (1.0f / 2147483648.0f); }   // [-1, 1)
inline uint16_t lerp565(uint16_t a, uint16_t b, int t /*0..256*/) {
    const int r = (a >> 11) + (((b >> 11) - (a >> 11)) * t >> 8);
    const int gg = ((a >> 5) & 63) + ((((b >> 5) & 63) - ((a >> 5) & 63)) * t >> 8);
    const int bl = (a & 31) + (((b & 31) - (a & 31)) * t >> 8);
    return (uint16_t)((r << 11) | (gg << 5) | bl);
}

// RGB565 blends with all three channels in one 32-bit word (green moved to the top half): one
// multiply-add pair per pixel instead of three channel lerps. k5 is the weight of b, 0..32.
inline uint32_t unpack565(uint16_t c) { return (c | ((uint32_t)c << 16)) & 0x07E0F81Fu; }
inline uint16_t pack565(uint32_t v) { v &= 0x07E0F81Fu; return (uint16_t)(v | (v >> 16)); }
inline uint16_t blend5(uint16_t a, uint32_t b32, uint32_t k5) {
    return pack565((unpack565(a) * (32 - k5) + b32 * k5) >> 5);
}

// Room for `verts` more vertices / `tris` more triangles: scene caps, the byte budget and what
// PSRAM can actually give (a core allocation failure is fatal — vertice_alloc.c — so refuse first).
bool room_for(int verts, int tris) {
    if (verts < 0 || tris < 0) return false;
    if (g.verts + verts > VX_MAX_VERTICES || g.tris + tris > VX_MAX_TRIANGLES) return false;
    // Mesh storage + the per-frame queues that grow with the scene (render queue, sort keys,
    // transformed vertices): a generous per-element estimate, doubled for vector growth.
    const size_t need = ((size_t)verts * 96 + (size_t)tris * 160) * 2;
    if (vx_mem_used() + need > VX_MEM_BUDGET) return false;
    return psram_largest() > need + (256u << 10);
}

// Level of detail, once per frame before culling: each master with stand-ins shows exactly one of
// {itself, lod 1, lod 2}, picked by its distance to the camera, and the chosen stand-in copies the
// master's transform. A 10% band around each switch distance stops a kart flickering between two
// levels at the boundary. Hidden masters (vx_obj_show 0) hide their stand-ins too.
void apply_lods(void) {
    if (!g.any_lod) return;
    const Vector3 &cp = g.cam->position;
    for (int i = 0; i < g.nobj; i++) {
        Engine::Lod &L = g.lod[i];
        if (!L.n || !g.obj[i]) continue;
        Object *m = g.obj[i];
        const float dx = (float)(m->position.x - cp.x), dy = (float)(m->position.y - cp.y),
                    dz = (float)(m->position.z - cp.z);
        const float d = std::sqrt(dx * dx + dy * dy + dz * dz);
        int lvl = 0;
        while (lvl < L.n) {
            const float edge = (float)L.dist[lvl] * (lvl + 1 <= L.cur ? 0.9f : 1.1f);   // hysteresis
            if (d < edge) break;
            lvl++;
        }
        L.cur = (uint8_t)lvl;
        // The master's own `enabled` is the app's show flag; render visibility goes through
        // lod_hide (restored after the frame by restore_lods).
        for (int k = 0; k < L.n; k++) {
            Object *o = valid_obj(L.id[k]) ? g.obj[L.id[k]] : nullptr;
            if (!o) continue;
            const bool on = L.shown && lvl == k + 1;
            o->enabled = on;
            if (on) { o->position.assign(m->position); o->rotation.assign(m->rotation); }
        }
        m->enabled = L.shown && lvl == 0;
    }
}
// After the frame: masters report the app's own show flag again (vx_obj_show / picking see it).
void restore_lods(void) {
    if (!g.any_lod) return;
    for (int i = 0; i < g.nobj; i++)
        if (g.lod[i].n && g.obj[i]) g.obj[i]->enabled = g.lod[i].shown;
}

// Detach object `id` from the LOD graph (it is being freed): as a master its stand-ins become
// ordinary (hidden) objects again; as a stand-in it leaves its master's list.
void lod_forget(int id) {
    Engine::Lod &L = g.lod[id];
    for (int k = 0; k < L.n; k++) if (L.id[k] >= 0 && L.id[k] < VX_MAX_OBJECTS) g.lod_of[L.id[k]] = 0;
    L = Engine::Lod{};
    if (g.lod_of[id]) {
        Engine::Lod &M = g.lod[g.lod_of[id] - 1];
        for (int k = 0; k < M.n; k++)
            if (M.id[k] == id) {
                for (int j = k; j + 1 < M.n; j++) { M.id[j] = M.id[j + 1]; M.dist[j] = M.dist[j + 1]; }
                M.n--;
                break;
            }
        g.lod_of[id] = 0;
    }
}

int free_slot(void) {
    for (int i = 0; i < g.nobj; i++) if (!g.obj[i]) return i;
    return g.nobj < VX_MAX_OBJECTS ? g.nobj : -1;
}

int add_object(Object *o) {
    if (!o) return -1;
    const int id = free_slot();
    if (id < 0) { delete o; return -1; }
    o->calculateBoundingBox();
    g.obj[id] = o;
    g.obj_v[id] = (int)o->vertices.size();
    g.obj_t[id] = (int)o->triangles.size();
    g.verts += g.obj_v[id];
    g.tris += g.obj_t[id];
    g.scene->addObject(o);
    if (id == g.nobj) g.nobj++;
    return id;
}

void build_sky(uint16_t top, uint16_t bottom) {
    g.sky_top = top; g.sky_bot = bottom;
    if (!g.sky) return;
    const int n = g.h > 1 ? g.h - 1 : 1;
    for (int y = 0; y < g.h; y++) g.sky[y] = lerp565(top, bottom, y * 256 / n);
}
// The gradient follows the horizon, not the screen: zenith colour high above it, the haze colour
// at and below it — so fog (which fades into g.sky[row]) always meets the sky's horizon haze,
// whatever the camera pitch.
void sky_follow_horizon(int horizon) {
    if (!g.sky) return;
    const int span = g.h * 3 / 4;
    for (int y = 0; y < g.h; y++) {
        const int d = horizon - y;                          // rows above the horizon
        g.sky[y] = d <= 0 ? g.sky_bot : lerp565(g.sky_bot, g.sky_top, d >= span ? 256 : d * 256 / span);
    }
}

// Shared by vx_mesh and vx_model. tri_mat holds material HANDLES (or null).
int build_mesh(const int32_t *xyz, int nverts, const uint16_t *idx, int ntris, const int16_t *uv,
               const uint8_t *tri_mat, int mat, int flags) {
    constexpr int32_t kMaxCoord = 1 << 16;   // keeps the core's int32 fixed-point math in range
    if (nverts < 3 || ntris < 1 || nverts > 65535) return -1;
    for (int i = 0; i < nverts * 3; i++) if (xyz[i] < -kMaxCoord || xyz[i] > kMaxCoord) return -1;
    for (int i = 0; i < ntris * 3; i++) if (idx[i] >= nverts) return -1;
    const bool smooth = (flags & VX_MESH_SMOOTH) != 0;
    // Faceted meshes get 3 private vertices per triangle so each face keeps its own normal.
    const int out_verts = smooth ? nverts : ntris * 3;
    if (out_verts > 65535 || !room_for(out_verts, ntris) || free_slot() < 0) return -1;
    Material *def = mat_or_default(mat);
    auto tri_material = [&](int t) -> Material * { return tri_mat ? mat_or_default(tri_mat[t]) : def; };
    auto vertex = [&](int i) {
        Object::Vertex v;
        v.position = Vector3{xyz[i * 3], xyz[i * 3 + 1], xyz[i * 3 + 2]};
        if (uv) v.uv = Vector2{uv[i * 2], uv[i * 2 + 1]};
        return v;
    };
    Object *o = new Object();
    o->vertices.reserve((size_t)out_verts);
    o->triangles.reserve((size_t)ntris);
    if (smooth) {
        for (int i = 0; i < nverts; i++) o->addVertex(vertex(i));
        // Area-weighted face normals accumulated per vertex (64-bit: design units can be large).
        int64_t *acc = (int64_t *)psram_calloc((size_t)nverts * 3 * sizeof(int64_t));
        if (!acc) { delete o; return -1; }
        for (int t = 0; t < ntris; t++) {
            const int i0 = idx[t * 3], i1 = idx[t * 3 + 1], i2 = idx[t * 3 + 2];
            const int64_t ux = (int64_t)xyz[i1 * 3] - xyz[i0 * 3], uy = (int64_t)xyz[i1 * 3 + 1] - xyz[i0 * 3 + 1],
                          uz = (int64_t)xyz[i1 * 3 + 2] - xyz[i0 * 3 + 2];
            const int64_t vx = (int64_t)xyz[i2 * 3] - xyz[i0 * 3], vy = (int64_t)xyz[i2 * 3 + 1] - xyz[i0 * 3 + 1],
                          vz = (int64_t)xyz[i2 * 3 + 2] - xyz[i0 * 3 + 2];
            const int64_t nx = uy * vz - uz * vy, ny = uz * vx - ux * vz, nz = ux * vy - uy * vx;
            for (int k = 0; k < 3; k++) {
                const int vi = idx[t * 3 + k];
                acc[vi * 3] += nx; acc[vi * 3 + 1] += ny; acc[vi * 3 + 2] += nz;
            }
            o->addTriangle((uint16_t)i0, (uint16_t)i1, (uint16_t)i2, tri_material(t));
        }
        for (int i = 0; i < nverts; i++) {
            const double x = (double)acc[i * 3], y = (double)acc[i * 3 + 1], z = (double)acc[i * 3 + 2];
            const double len = std::sqrt(x * x + y * y + z * z);
            if (len > 0) {
                const double s = FIXED_POINT_SCALE / len;
                o->vertices[i].normal = Vector3{(int32_t)(x * s), (int32_t)(y * s), (int32_t)(z * s)};
            }
        }
        psram_free(acc);
    } else {
        for (int t = 0; t < ntris; t++) {
            for (int k = 0; k < 3; k++) o->addVertex(vertex(idx[t * 3 + k]));
            o->addTriangle((uint16_t)(t * 3), (uint16_t)(t * 3 + 1), (uint16_t)(t * 3 + 2), tri_material(t));
        }
        o->computeFlatNormals();
    }
    return add_object(o);
}

// ---- particles: simulate + project once per frame, draw per band --------------------------------
void particles_update(void) {
    const int64_t now = now_us();
    float dt = g.last_us ? (float)(now - g.last_us) * 1e-6f : 0.0f;
    g.last_us = now;
    if (dt > 0.1f) dt = 0.1f;   // a stalled frame must not teleport everything
    g.nspr = 0;
    g.live = 0;
    if (!g.spr) return;
    const float *M = g.scene->getCameraMatrix();
    const float cx = (float)g.cam->position.x, cy = (float)g.cam->position.y, cz = (float)g.cam->position.z;
    const float f = g.cam->fovFactor, nearz = (float)g.cam->nearPlane, farz = (float)g.cam->farPlane;
    // Fill budget: soft particles are the one effect whose cost grows with how close they are. A
    // puff right in front of the lens covers the screen and costs more than the whole scene (and
    // looks like fog), so: particles closer than 6x the near plane are skipped, the radius is
    // capped at 1/32 of the canvas width, and once the sprites of a frame cover half a screen's
    // worth of pixels the rest are dropped.
    const float close = nearz * 6.0f;
    const int rmax = std::max(4, g.w / 32);
    int64_t area = 0;
    const int64_t area_max = (int64_t)g.w * g.h / 5;     // 1.3: a fifth of the screen at most
    for (int e = 0; e < VX_MAX_EMITTERS; e++) {
        Emitter &em = g.em[e];
        if (!em.used) continue;
        int j = 0;
        for (int i = 0; i < em.n; i++) {
            Particle p = em.p[i];
            p.age += dt * em.rate;
            if (p.age >= 1.0f) continue;            // dead: compacted away
            p.vy -= em.gravity * dt;
            p.x += p.vx * dt; p.y += p.vy * dt; p.z += p.vz * dt;
            em.p[j++] = p;
            // world -> camera -> screen, exactly as the mesh pipeline (Scene::renderObject)
            const float dx = p.x - cx, dy = p.y - cy, dz = p.z - cz;
            const float qz = M[6] * dx + M[7] * dy + M[8] * dz;
            if (qz < close || qz > farz || g.nspr >= kMaxSprites || area > area_max) continue;
            const float qx = M[0] * dx + M[1] * dy + M[2] * dz, qy = M[3] * dx + M[4] * dy + M[5] * dz;
            const float inv = f / qz;
            const int sx = (int)(qx * inv) + g.w / 2, sy = g.h / 2 - (int)(qy * inv);
            const float size = em.s0 + (em.s1 - em.s0) * p.age;
            int r = (int)(size * inv * 0.5f);
            if (r < 1) r = 1;
            if (r > rmax) r = rmax;
            if (sx + r < 0 || sx - r >= g.w || sy + r < 0 || sy - r >= g.h) continue;
            area += (int64_t)(2 * r + 1) * (2 * r + 1);
            Sprite &s = g.spr[g.nspr++];
            s.x0 = (int16_t)clampi(sx - r, 0, g.w - 1); s.x1 = (int16_t)clampi(sx + r, 0, g.w - 1);
            s.y0 = (int16_t)clampi(sy - r, 0, g.h - 1); s.y1 = (int16_t)clampi(sy + r, 0, g.h - 1);
            s.z = (uint16_t)std::min(qz, 65535.0f);
            s.color = lerp565(em.c0, em.c1, (int)(p.age * 256.0f));
            // alpha: solid for the first third of the life, then fades out
            s.alpha = (uint8_t)(p.age < 0.33f ? 220 : (int)(220.0f * (1.0f - p.age) / 0.67f));
            s.flags = (uint8_t)em.flags;
        }
        em.n = j;
        g.live += j;
    }
}

// Draw the projected particles overlapping rows [y0,y1): round, depth-tested against this frame's
// z-buffer, alpha-blended or additive. `col`/`zb` are virtual row bases (row y = col + y*w): the
// frame in PSRAM (band path) or an SRAM tile. Only rows [y0,y1) are written -> parallel-safe.
void particles_draw(int y0, int y1, uint16_t *col, const uint16_t *zb) {
    for (int i = 0; i < g.nspr; i++) {
        const Sprite &s = g.spr[i];
        if (s.y1 < y0 || s.y0 >= y1) continue;
        const int ya = std::max<int>(s.y0, y0), yb = std::min<int>(s.y1, y1 - 1);
        const int cxp = (s.x0 + s.x1) / 2, cyp = (s.y0 + s.y1) / 2;
        const int r = std::max(1, std::max(s.x1 - s.x0, s.y1 - s.y0) / 2), r2 = r * r;
        const bool ztest = g.depth && !(s.flags & VX_PART_NODEPTH);
        const bool add = (s.flags & VX_PART_ADDITIVE) != 0;
        const int sr = s.color >> 11, sg = (s.color >> 5) & 63, sb = s.color & 31;
        const int ka = (s.alpha << 16) / r2;   // soft edge a = alpha * (r2-d2)/r2, one divide per sprite
        for (int y = ya; y <= yb; y++) {
            uint16_t *row = col + (size_t)y * g.w;
            const uint16_t *zr = zb + (size_t)y * g.w;
            const int dy = y - cyp;
            for (int x = s.x0; x <= s.x1; x++) {
                const int dx = x - cxp, d2 = dx * dx + dy * dy;
                if (d2 > r2) continue;
                if (ztest && s.z >= zr[x]) continue;
                const int a = ((r2 - d2) * ka) >> 16;   // soft edge
                const uint16_t d = row[x];
                int rr = d >> 11, gg = (d >> 5) & 63, bb = d & 31;
                if (add) {
                    rr = std::min(31, rr + (sr * a >> 8)); gg = std::min(63, gg + (sg * a >> 8));
                    bb = std::min(31, bb + (sb * a >> 8));
                } else {
                    rr += (sr - rr) * a >> 8; gg += (sg - gg) * a >> 8; bb += (sb - bb) * a >> 8;
                }
                row[x] = (uint16_t)((rr << 11) | (gg << 5) | bb);
            }
        }
    }
}

// ---- background: sky gradient, 360° panorama, Mode-7 floor ------------------------------------
// Per-frame camera terms for the background (computed once, before the parallel section).
// World ray of screen pixel (x, y), in camera units with q.z = 1: q = ((x-w/2)/f, (h/2-y)/f, 1),
// d = Mᵀ·q. With no camera roll (lookAt cameras) d.y is constant along a row, so a flat floor has
// one depth per row and its texture coordinates are linear in x — the SNES Mode 7 insight: one
// division per ROW, then two additions per pixel.
struct BgFrame {
    float m[9];               // camera rotation (world -> camera)
    float cx, cy, cz, f;
    bool  roll;               // camera rolled: floor falls back to per-pixel division
    int   horizon;            // first floor row (rows above are sky)
    float pano_v0, pano_dv;   // panorama row at screen row 0, rows per screen row
    float hz;                 // the horizon row, unrounded (the mirror line of vx_water)
    float wt;                 // seconds, for the ripple phase
};
BgFrame bgf;

void bg_frame_setup(void) {
    const float *M = g.scene->getCameraMatrix();
    for (int i = 0; i < 9; i++) bgf.m[i] = M[i];
    bgf.cx = (float)g.cam->position.x; bgf.cy = (float)g.cam->position.y; bgf.cz = (float)g.cam->position.z;
    bgf.f = g.cam->fovFactor;
    bgf.roll = std::fabs(bgf.m[1]) > 1e-3f;
    // Horizon: the row where the ray turns level (d.y = 0) — m4·qy + m7 = 0 at the centre column.
    const float qy = std::fabs(bgf.m[4]) > 1e-6f ? -bgf.m[7] / bgf.m[4] : 0.0f;
    float hz = g.h * 0.5f - qy * bgf.f;
    hz = hz < -4.0f * g.h ? -4.0f * g.h : hz > 5.0f * g.h ? 5.0f * g.h : hz;   // steep pitch: keep it an int
    bgf.horizon = clampi((int)std::floor(hz), -1, g.h);
    bgf.hz = hz;
    bgf.wt = (float)((now_us() / 1000) % 3600000) * 0.001f;
    sky_follow_horizon((int)std::floor(hz));
    if (g.pano_px && g.pano_u) {
        // Yaw = heading of the camera's forward axis (Mᵀ·(0,0,1)); each column adds its own angle.
        const float yaw = std::atan2(bgf.m[6], bgf.m[8]);
        const float k = g.pano_w * (g.pano_reps > 1 ? g.pano_reps : 1) / (2.0f * 3.14159265f);   // texels per radian
        // Each column's angle off the view axis depends only on the lens: a table rebuilt when the
        // FOV or width changes (was one atan per column per frame).
        static float col_ang[1024];
        static float col_f = -1.0f;
        static int col_w = 0;
        if (col_f != bgf.f || col_w != g.w) {
            const int n = g.w < 1024 ? g.w : 1024;
            for (int x = 0; x < n; x++) col_ang[x] = std::atan((x - g.w * 0.5f) / bgf.f);
            col_f = bgf.f; col_w = g.w;
        }
        for (int x = 0; x < g.w && x < 1024; x++) {
            int u = (int)std::floor((yaw + col_ang[x]) * k);
            g.pano_u[x] = (int16_t)(u & (g.pano_w - 1));
        }
        // Texel rows per screen row keeps texels square in angle; row `pano_hrow` on the horizon.
        bgf.pano_dv = k / bgf.f;
        bgf.pano_v0 = g.pano_hrow - hz * bgf.pano_dv;
    }
}

// Floor texture lit by the scene's sun + ambient (a flat plane facing up has one brightness), so
// the per-pixel loop only samples. Rebuilt when the floor, sun or ambient change.
void floor_relight(void) {
    if (!g.floor_src || !g.floor_lit) return;
    uint16_t *out = g.floor_base ? g.floor_base : g.floor_lit;
    const float ly = g.sun->worldLightDir.y / (float)FIXED_POINT_SCALE;
    const float kd = (ly > 0 ? ly : 0) * g.sun->intensity / 255.0f;
    const float fr = std::min(1.3f, (g.amb->color.r + g.sun->color.r * kd) / 255.0f);
    const float fg = std::min(1.3f, (g.amb->color.g + g.sun->color.g * kd) / 255.0f);
    const float fb = std::min(1.3f, (g.amb->color.b + g.sun->color.b * kd) / 255.0f);
    const int n = g.floor_w * g.floor_h;
    int sr = 0, sg = 0, sb = 0;
    for (int i = 0; i < n; i++) {
        const uint16_t c = g.floor_src[i];
        const int r = std::min(31, (int)((c >> 11) * fr)), gg = std::min(63, (int)(((c >> 5) & 63) * fg)),
                  b = std::min(31, (int)((c & 31) * fb));
        out[i] = (uint16_t)((r << 11) | (gg << 5) | b);
        sr += r; sg += gg; sb += b;
    }
    g.floor_avg = (uint16_t)(((sr / n) << 11) | ((sg / n) << 5) | (sb / n));   // the far-distance "mip"
    g.floor_dirty = false;
}

// Water (vx_water): blend the floor row with the mirror image of what is above the horizon — the
// panorama row as far above the horizon as this row is below it (magenta texels: the sky gradient).
// Fresnel: the blend is strongest at the horizon and fades as quadratically toward the viewer, where
// the water shows its own colour. Each row ripples sideways a few pixels (two sines, time-shifted),
// more near the viewer, and the mirror row wobbles by a row or two: the reflection breaks up like
// real water. One extra texel fetch and one lerp per pixel, rows below the horizon only.
static void water_row(uint16_t *row, int y, int xa, int xb) {
    const int span = g.h - bgf.horizon;
    if (g.water_k <= 0 || span <= 1) return;
    const float fr = (float)(y - bgf.horizon) / (float)span;           // 0 at the horizon, 1 at the bottom
    const float fk = 1.0f - fr;
    const int k = (int)(g.water_k * fk * fk);
    if (k < 6) return;
    const float ph = bgf.wt;
    const int off = (int)(g.water_wave * (0.25f + fr) * (std::sin(y * 0.71f + ph * 2.3f) + 0.5f * std::sin(y * 0.23f - ph * 1.3f)));
    const int ym = (int)std::floor(2.0f * bgf.hz - y + 1.5f * std::sin(y * 0.41f + ph * 1.7f) * fr);
    const uint16_t sky = ym < 0 ? g.sky_top : ym >= g.h ? g.sky_bot : g.sky[ym];
    if (g.pano_px) {
        const int v = (int)std::floor(bgf.pano_v0 + ym * bgf.pano_dv);
        if (v >= 0 && v < g.pano_h) {
            const uint16_t *prow = g.pano_px + (size_t)v * g.pano_w;
            const int w1 = g.w - 1;
            const uint32_t k5 = (uint32_t)(k + 4) >> 3, sky32 = unpack565(sky);
            for (int x = xa & ~1; x < xb; x += 2) {         // the reflection is soft: one sample per pixel pair
                int xs = x + off;                           // (pairs on even columns, whatever the run)
                xs = xs < 0 ? 0 : xs > w1 ? w1 : xs;
                const uint16_t c = prow[g.pano_u[xs]];
                const uint32_t m32 = (c == 0xF81F ? sky32 : unpack565(c)) * k5, ik = 32 - k5;
                if (x >= xa) row[x] = pack565((unpack565(row[x]) * ik + m32) >> 5);
                if (x + 1 < xb) row[x + 1] = pack565((unpack565(row[x + 1]) * ik + m32) >> 5);
            }
            return;
        }
    }
    const uint32_t k5 = (uint32_t)(k + 4) >> 3, sky32 = unpack565(sky);
    for (int x = xa; x < xb; x++) row[x] = blend5(row[x], sky32, k5);
}

// The background of row y, pixels [xa, xb): panorama / sky gradient above the horizon, the Mode-7
// floor (with its water mirror) below it, the ceiling under water. `row` is the row's first pixel.
static void bg_row(int y, int xa, int xb, uint16_t *row) {
    {
        const uint16_t sky = g.sky[y];
        const bool floor_row = g.floor_on && y >= bgf.horizon;
        if (floor_row && !bgf.roll) {
            // One depth per row (see BgFrame). t = distance along the ray to the plane.
            const float qy = (g.h * 0.5f - y) / bgf.f, qx0 = (0 - g.w * 0.5f) / bgf.f;
            const float dy = bgf.m[4] * qy + bgf.m[7];                      // m1·qx ≈ 0 (no roll)
            if (dy < -1e-4f) {
                const float t = (g.floor_y - bgf.cy) / dy;                   // > 0: plane below the eye
                const float dx0 = bgf.m[0] * qx0 + bgf.m[3] * qy + bgf.m[6];
                const float dz0 = bgf.m[2] * qx0 + bgf.m[5] * qy + bgf.m[8];
                const float px = bgf.cx + t * dx0, pz = bgf.cz + t * dz0;
                const float sx = t * bgf.m[0] / bgf.f, sz = t * bgf.m[2] / bgf.f;   // world step per pixel
                // Fog and "mip" are row constants: blend weights for the whole row.
                const int fogA = t <= vx_fog_near_z ? 0 : t >= vx_fog_far_z ? 256
                               : (int)((t - vx_fog_near_z) * 256.0f / (vx_fog_far_z - vx_fog_near_z));
                const int lodA = g.floor_repeat > 0 ? clampi((int)((t / g.floor_repeat - 6.0f) * 24.0f), 0, 256) : 256;
                if (!g.floor_lit || lodA >= 256 || fogA >= 256) {
                    // Past the texture's useful distance: the average colour, fogged.
                    const uint16_t base = g.floor_lit ? g.floor_avg : g.floor_color;
                    const uint16_t c = lerp565(base, sky, fogA);
                    for (int x = xa; x < xb; x++) row[x] = c;
                    water_row(row, y, xa, xb);
                    return;
                }
                // Texels in Q16; the texture is power-of-two so wrapping is a mask.
                const float tk = (float)g.floor_w / g.floor_repeat, tkv = (float)g.floor_h / g.floor_repeat;
                int32_t u = (int32_t)(px * tk * 65536.0f), v = (int32_t)(pz * tkv * 65536.0f);
                const int32_t du = (int32_t)(sx * tk * 65536.0f), dv = (int32_t)(sz * tkv * 65536.0f);
                const unsigned wm = g.floor_w - 1, hm = g.floor_h - 1, sh = g.floor_wshift;
                const uint16_t *tex = g.floor_lit;
                u += (int32_t)((uint32_t)du * (uint32_t)xa); v += (int32_t)((uint32_t)dv * (uint32_t)xa);   // to xa
                if (lodA == 0 && fogA == 0) {
                    for (int x = xa; x < xb; x++, u += du, v += dv)
                        row[x] = tex[((((uint32_t)v >> 16) & hm) << sh) | (((uint32_t)u >> 16) & wm)];
                } else {
                    const uint16_t far = lerp565(g.floor_avg, sky, fogA);    // where the blend goes
                    const int a = 256 - (256 - lodA) * (256 - fogA) / 256;   // combined weight
                    const uint32_t far32 = unpack565(far), a5 = (uint32_t)(a + 4) >> 3;
                    for (int x = xa; x < xb; x++, u += du, v += dv)
                        row[x] = blend5(tex[((((uint32_t)v >> 16) & hm) << sh) | (((uint32_t)u >> 16) & wm)], far32, a5);
                }
                water_row(row, y, xa, xb);
                return;
            }
        } else if (floor_row) {
            // Rolled camera: exact per-pixel ray/plane intersection (a division per pixel).
            const float qy = (g.h * 0.5f - y) / bgf.f;
            for (int x = xa; x < xb; x++) {
                const float qx = (x - g.w * 0.5f) / bgf.f;
                const float dy = bgf.m[1] * qx + bgf.m[4] * qy + bgf.m[7];
                if (dy >= -1e-4f) { row[x] = sky; continue; }
                const float t = (g.floor_y - bgf.cy) / dy;
                const float wx = bgf.cx + t * (bgf.m[0] * qx + bgf.m[3] * qy + bgf.m[6]);
                const float wz = bgf.cz + t * (bgf.m[2] * qx + bgf.m[5] * qy + bgf.m[8]);
                const int fogA = t <= vx_fog_near_z ? 0 : t >= vx_fog_far_z ? 256
                               : (int)((t - vx_fog_near_z) * 256.0f / (vx_fog_far_z - vx_fog_near_z));
                uint16_t c = g.floor_color;
                if (g.floor_lit) {
                    const int tu = (int)std::floor(wx * g.floor_w / g.floor_repeat) & (g.floor_w - 1);
                    const int tv = (int)std::floor(wz * g.floor_h / g.floor_repeat) & (g.floor_h - 1);
                    c = g.floor_lit[(tv << g.floor_wshift) | tu];
                }
                row[x] = lerp565(c, sky, fogA);
            }
            return;
        }
        // Ceiling (1.3): a plane above the eye drawn like the floor, one division per row — the
        // underside of the water surface seen from below. Fogged toward the row's sky colour.
        if (g.ceil_on && !bgf.roll && g.ceil_px) {
            const float qy = (g.h * 0.5f - y) / bgf.f, qx0 = (0 - g.w * 0.5f) / bgf.f;
            const float dy = bgf.m[4] * qy + bgf.m[7];
            if (dy > 1e-4f && g.ceil_y > bgf.cy) {
                const float t = (g.ceil_y - bgf.cy) / dy;
                const float dx0 = bgf.m[0] * qx0 + bgf.m[3] * qy + bgf.m[6];
                const float dz0 = bgf.m[2] * qx0 + bgf.m[5] * qy + bgf.m[8];
                const float px = bgf.cx + t * dx0, pz = bgf.cz + t * dz0;
                const float sx = t * bgf.m[0] / bgf.f, sz = t * bgf.m[2] / bgf.f;
                const int fogA = t <= vx_fog_near_z ? 0 : t >= vx_fog_far_z ? 256
                               : (int)((t - vx_fog_near_z) * 256.0f / (vx_fog_far_z - vx_fog_near_z));
                if (fogA >= 250) {
                    for (int x = xa; x < xb; x++) row[x] = sky;
                    return;
                }
                const float tk = (float)g.ceil_w / g.ceil_repeat, tkv = (float)g.ceil_h / g.ceil_repeat;
                int32_t u = (int32_t)(px * tk * 65536.0f), v = (int32_t)(pz * tkv * 65536.0f);
                const int32_t du = (int32_t)(sx * tk * 65536.0f), dv = (int32_t)(sz * tkv * 65536.0f);
                const unsigned wm = g.ceil_w - 1, hm = g.ceil_h - 1, sh = g.ceil_wshift;
                const uint16_t *tex = g.ceil_px;
                const uint32_t sky32 = unpack565(sky), a5 = (uint32_t)(fogA + 4) >> 3;
                u += (int32_t)((uint32_t)du * (uint32_t)xa); v += (int32_t)((uint32_t)dv * (uint32_t)xa);
                for (int x = xa; x < xb; x++, u += du, v += dv)
                    row[x] = blend5(tex[((((uint32_t)v >> 16) & hm) << sh) | (((uint32_t)u >> 16) & wm)], sky32, a5);
                return;
            }
        }
        // Sky: the panorama (keyed texels show the gradient), or the plain gradient.
        if (g.pano_px) {
            const int v = (int)std::floor(bgf.pano_v0 + y * bgf.pano_dv);
            if (v >= 0 && v < g.pano_h) {
                const uint16_t *prow = g.pano_px + (size_t)v * g.pano_w;
                for (int x = xa; x < xb; x++) {
                    const uint16_t c = prow[g.pano_u[x]];
                    row[x] = c == 0xF81F ? sky : c;
                }
                return;
            }
        }
        if (xa == 0 && xb == g.w) {                      // whole row: two pixels a store
            const uint32_t c = sky | ((uint32_t)sky << 16);
            uint32_t *row32 = (uint32_t *)row;
            for (int x = 0; x < g.w / 2; x++) row32[x] = c;
        } else for (int x = xa; x < xb; x++) row[x] = sky;
    }
}

// Clear rows [y0,y1) of a virtual row base: the whole background, depth to "far".
void clear_rows(int y0, int y1, uint16_t *col, uint16_t *zb) {
    for (int y = y0; y < y1; y++) bg_row(y, 0, g.w, col + (size_t)y * g.w);
    if (g.depth) memset(zb + (size_t)y0 * g.w, 0xFF, (size_t)(y1 - y0) * g.w * 2);
}
// The background drawn AFTER the opaque geometry, only where the depth is still "far": the floor,
// the panorama and the mirror are no longer computed for pixels the scene covers. Runs per row.
void bg_fill_uncovered(int y0, int y1, uint16_t *col, const uint16_t *zb) {
    for (int y = y0; y < y1; y++) {
        const uint16_t *z = zb + (size_t)y * g.w;
        uint16_t *row = col + (size_t)y * g.w;
        int x = 0;
        while (x < g.w) {
            while (x < g.w && z[x] != 0xFFFF) x++;
            const int xa = x;
            while (x < g.w && z[x] == 0xFFFF) x++;
            if (x > xa) bg_row(y, xa, x, row);
        }
    }
}

// ---- under water (1.3) ---------------------------------------------------------------------------
// Caustics: sunlight focused by the waves draws a moving net of bright lines on the bed. Two tileable
// 128x128 layers of Voronoi cell edges, drifting against each other, are summed and thresholded —
// where both are bright the light concentrates. It is applied to the floor TEXTURE (lit copy) once a
// frame, ~16k texels however big the screen, so the per-pixel floor loop doesn't change at all.
constexpr int kCausN = 128;
void caustic_layer(uint8_t *out, uint32_t seed) {
    constexpr int C = 6;                                     // cells per side
    float px[C][C], py[C][C];
    for (int j = 0; j < C; j++)
        for (int i = 0; i < C; i++) {
            seed = seed * 1664525u + 1013904223u; px[j][i] = (i + 0.15f + 0.7f * (seed >> 8) / 16777216.0f) * kCausN / C;
            seed = seed * 1664525u + 1013904223u; py[j][i] = (j + 0.15f + 0.7f * (seed >> 8) / 16777216.0f) * kCausN / C;
        }
    for (int y = 0; y < kCausN; y++)
        for (int x = 0; x < kCausN; x++) {
            const int cx = x * C / kCausN, cy = y * C / kCausN;
            float d1 = 1e9f, d2 = 1e9f;
            for (int oy = -1; oy <= 1; oy++)
                for (int ox = -1; ox <= 1; ox++) {
                    const int ix = (cx + ox + C) % C, iy = (cy + oy + C) % C;
                    const float fx = px[iy][ix] + (cx + ox < 0 ? -kCausN : cx + ox >= C ? kCausN : 0);
                    const float fy = py[iy][ix] + (cy + oy < 0 ? -kCausN : cy + oy >= C ? kCausN : 0);
                    const float d = std::sqrt((fx - x) * (fx - x) + (fy - y) * (fy - y));
                    if (d < d1) { d2 = d1; d1 = d; } else if (d < d2) d2 = d;
                }
            const float e = d2 - d1;                             // 0 on a cell edge
            const float v = 1.0f - e / 5.0f;
            out[y * kCausN + x] = (uint8_t)(v <= 0 ? 0 : v >= 1 ? 255 : (int)(v * v * 255));
        }
}
void caustics_apply(void) {
    if (!g.caus || !g.floor_base || !g.floor_lit || g.caus_k <= 0) return;
    static uint32_t frame;
    if ((++frame & 1) && !g.floor_dirty) return;                 // slow drift: every other frame is plenty
    const float t = (float)((now_us() / 1000) % 1000000) * 0.001f * g.caus_speed / 64.0f;
    const int o1x = (int)(t * 5.0f), o1y = (int)(t * 3.0f), o2x = (int)(-t * 4.0f), o2y = (int)(t * 6.0f);
    const uint8_t *L1 = g.caus, *L2 = g.caus + kCausN * kCausN;
    const int lr = g.sun->color.r, lg = g.sun->color.g, lb = g.sun->color.b, k = g.caus_k;
    const int fw = g.floor_w, fh = g.floor_h, sh = g.floor_wshift;
    const int sx = (kCausN << 16) / fw, sy = (kCausN << 16) / fh;   // Q16 steps: a multiply per texel, not a divide
    for (int y = 0; y < fh; y++) {
        const int py = (y * sy) >> 16;
        const uint8_t *r1 = L1 + ((py + o1y) & (kCausN - 1)) * kCausN, *r2 = L2 + ((py + o2y) & (kCausN - 1)) * kCausN;
        const uint16_t *src = g.floor_base + (y << sh);
        uint16_t *dst = g.floor_lit + (y << sh);
        for (int x = 0; x < fw; x++) {
            const int pxi = (x * sx) >> 16;
            int v = r1[(pxi + o1x) & (kCausN - 1)] + r2[(pxi + o2x) & (kCausN - 1)] - 110;   // brightest where both are
            const uint16_t c = src[x];
            if (v <= 0) { dst[x] = c; continue; }
            v = v * k >> 7;                                       // 0..~400 at full strength
            int r = (c >> 11) + (v * lr >> 14), gg = ((c >> 5) & 63) + (v * lg >> 13), b = (c & 31) + (v * lb >> 14);
            dst[x] = (uint16_t)(((r > 31 ? 31 : r) << 11) | ((gg > 63 ? 63 : gg) << 5) | (b > 31 ? 31 : b));
        }
    }
}
// Light shafts: soft slanted bands of light from the surface, brightest at the top of the view and
// fading with depth (screen rows). A 512-entry profile across the slanted axis, swaying slowly; added
// on top of everything in the tile (the water itself glows, not the objects' surfaces).
void shafts_build(uint32_t seed) {
    int acc[512] = {};
    for (int b = 0; b < 9; b++) {
        seed = seed * 1664525u + 1013904223u;
        const int c = (int)(seed >> 23), w = 10 + (int)((seed >> 8) & 31), a = 90 + (int)((seed >> 16) & 127);
        for (int d = -w; d <= w; d++) acc[(c + d) & 511] += a * (w * w - d * d) / (w * w);
    }
    for (int i = 0; i < 512; i++) g.shaft[i] = (uint8_t)(acc[i] > 255 ? 255 : acc[i]);
}
void shafts_draw(int y0, int y1, uint16_t *col) {
    if (!g.shaft || g.shaft_k <= 0) return;
    const int lim = g.h / 2;                                      // fades out by half way down the view
    if (y0 >= lim) return;
    const float t = (float)((now_us() / 1000) % 1000000) * 0.001f;
    const int drift = (int)(std::sin(t * 0.35f) * 18.0f + t * 4.0f);
    const int lr = g.sun->color.r, lg = g.sun->color.g, lb = g.sun->color.b;
    for (int y = y0; y < y1 && y < lim; y++) {
        const int fade = g.shaft_k * (lim - y) / lim;             // 0..256
        uint16_t *row = col + (size_t)y * g.w;
        const int s0 = drift + y * g.shaft_slope / 64;
        const uint32_t step = (1024u << 16) / (uint32_t)g.w;       // ~2 profiles across the width, per 4 px
        uint32_t acc = (uint32_t)s0 << 16;
        for (int x = 0; x + 3 < g.w; x += 4, acc += step) {        // soft light: one value per 4 pixels
            const int v = g.shaft[(acc >> 16) & 511] * fade >> 8;
            if (v < 4) continue;
            const int ar = v * lr >> 14, ag = v * lg >> 13, ab = v * lb >> 14;
            for (int k = 0; k < 4; k++) {
                const uint16_t c = row[x + k];
                const int r = (c >> 11) + ar, gg = ((c >> 5) & 63) + ag, b = (c & 31) + ab;
                row[x + k] = (uint16_t)(((r > 31 ? 31 : r) << 11) | ((gg > 63 ? 63 : gg) << 5) | (b > 31 ? 31 : b));
            }
        }
    }
}

// Profiling (VX_PROFILE): per worker wall time of each phase, and the task's own CPU time over the
// frame's parallel section (FreeRTOS run-time stats) — wall >> cpu means it was preempted.
struct BandProf { int64_t clear, raster, parts, copy, cpu; int tiles; uint32_t stat[Rasterizer::VX_STAT_N]; };
BandProf g_prof[2];

// ---- tiled raster (the fast path) ---------------------------------------------------------------
// Cached PSRAM is latency-bound for CPU writes (~33 MB/s per core measured: every miss fills a
// 64-byte line first), so rasterising straight into the canvas spent most of its time waiting on
// memory. Instead each core renders one row tile at a time into internal SRAM (colour + depth,
// zero-wait), then streams the finished colour rows to the canvas with one AXI-GDMA burst copy.
// The depth buffer never touches PSRAM at all. Cores take tiles from a shared counter, so the load
// balances itself tile by tile whatever the scene looks like.
// In internal RAM on purpose: the linker fragment sends this archive's .bss to PSRAM, and atomic
// read-modify-write between the two cores is only guaranteed on internal memory.
#ifdef ESP_PLATFORM
DRAM_ATTR
#endif
std::atomic<int> g_next_tile{0};
bool vxEagerBg = false;                          // vx_config(VX_CFG_EAGER_BG): the background before the geometry
// VX_CFG_SPAN_EXP diagnostics: each frame one experiment (a part of the fast span off, or one core only),
// cycles per pixel and tile time accumulated per experiment, a summary in the log every 400 frames.
static int s_exp_on, s_exp_frame;
static bool s_exp_solo;                          // experiment: core 1 takes no tiles
static uint64_t s_exp_cyc[8], s_exp_px[8], s_exp_us[8], s_exp_n[8];
struct BgLate { int y0, y1; uint16_t *cb, *zb; };
static void bg_late(void *p) { const BgLate *q = (const BgLate *)p; bg_fill_uncovered(q->y0, q->y1, q->cb, q->zb); }

void tile_worker(int core) {
    const int64_t t0 = now_us();
    BandProf &pf = g_prof[core];
#if defined(ESP_PLATFORM) && CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS
    TaskStatus_t ts0;
    vTaskGetInfo(nullptr, &ts0, pdFALSE, eRunning);
#endif
    uint16_t *zb = g.tz[core];
    uint8_t *flags = g.flags[core];
    // Two colour buffers per core when there is SRAM for them: the DMA streams one tile out to the
    // canvas while the core renders the next into the other (the copy used to block the core).
    const int nbuf = g.tcol2[core] ? 2 : 1;
    uint16_t *const bufs[2] = { g.tcol[core], g.tcol2[core] };
    bool inflight[2] = { false, false };
    int cur = 0;
    for (;;) {
        if (s_exp_solo && core == 1) break;              // diagnostics: one core renders every tile
        const int t = g_next_tile.fetch_add(1, std::memory_order_relaxed);
        if (t >= g.ntiles) break;
        const int y0 = t * g.tile_h, y1 = std::min(y0 + g.tile_h, g.h);
        uint16_t *col = bufs[cur];
#ifdef ESP_PLATFORM
        if (inflight[cur]) {                             // its previous tile must have left first
            const int64_t w0 = now_us();
            if (nv_2d_copy_wait(g.tdone[core][cur], 50) != ESP_OK) g.tcol2[core] = nullptr;   // (DMA gone)
            inflight[cur] = false;
            pf.copy += now_us() - w0;
        }
#endif
        uint16_t *cb = col - (ptrdiff_t)y0 * g.w, *zbb = zb - (ptrdiff_t)y0 * g.w;   // virtual bases
        const int64_t a = now_us();
        const uint32_t s = g.bin_start[t], e = g.bin_start[t + 1];
        // Background late (1.5): depth to far, the opaque geometry, then the floor / panorama / mirror
        // only where nothing was drawn (vxRasterTile calls bg_late before anything that blends).
        const bool late = g.depth && !vxEagerBg && e > s;
        if (late) memset(zbb + (size_t)y0 * g.w, 0xFF, (size_t)(y1 - y0) * g.w * 2);
        else clear_rows(y0, y1, cb, zbb);
        const int64_t b = now_us();
        BgLate bl = { y0, y1, cb, zbb };
        if (e > s) g.scene->vxRasterTile(y0, y1, g.bin_list + s, (int)(e - s), flags, cb, zbb, pf.stat,
                                         late ? bg_late : nullptr, &bl);
        const int64_t c = now_us();
        if (g.nspr) particles_draw(y0, y1, cb, zbb);
        if (g.shaft_k) shafts_draw(y0, y1, cb);
        const int64_t d = now_us();
        const size_t bytes = (size_t)(y1 - y0) * g.w * 2;
        uint16_t *dst = g.target + (size_t)y0 * g.w;
#ifdef ESP_PLATFORM
        esp_err_t ae = ESP_FAIL;
        if (nbuf == 2 && g.tdone[core][cur]) {
            ae = nv_2d_copy_start(dst, col, bytes, g.tdone[core][cur]);
            static bool said;
            if (ae != ESP_OK && !said) { said = true; VX_LOGW("async tile copy refused (%d): blocking copies", (int)ae); }
        }
        if (ae == ESP_OK) {
            inflight[cur] = true;
            cur ^= 1;
        } else if (nv_2d_copy(dst, col, bytes, 50) != ESP_OK) {
            memcpy(dst, col, bytes);                     // unaligned canvas, or no DMA
        }
#else
        memcpy(dst, col, bytes);
#endif
        pf.clear += b - a; pf.raster += c - b; pf.parts += d - c; pf.copy += now_us() - d; pf.tiles++;
    }
#ifdef ESP_PLATFORM
    for (int k = 0; k < 2; k++)                          // the frame is done when its last tiles landed
        if (inflight[k]) { const int64_t w0 = now_us(); nv_2d_copy_wait(g.tdone[core][k], 50); pf.copy += now_us() - w0; }
#endif
    g.us_band[core] = now_us() - t0;
#if defined(ESP_PLATFORM) && CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS
    TaskStatus_t ts1;
    vTaskGetInfo(nullptr, &ts1, pdFALSE, eRunning);
    pf.cpu += (int64_t)(ts1.ulRunTimeCounter - ts0.ulRunTimeCounter);
#endif
}

// ---- band raster (fallback when there is no SRAM for tiles) ---------------------------------------
// Two row bands straight in the PSRAM frame, the cut moved every frame toward equal time.
void band(int y0, int y1, uint8_t *flags, int64_t *us, int core) {
    const int64_t t0 = now_us();
    BandProf &pf = g_prof[core];
    int64_t t1 = t0, t2 = t0;
    if (y1 > y0) {
        clear_rows(y0, y1, g.target, g.zbuf);
        t1 = now_us();
        g.scene->rasterizeBand(y0, y1, flags);
        t2 = now_us();
        if (g.nspr) particles_draw(y0, y1, g.target, g.zbuf);
        if (g.shaft_k) shafts_draw(y0, y1, g.target);
    }
    *us = now_us() - t0;
    pf.clear += t1 - t0; pf.raster += t2 - t1; pf.parts += now_us() - t2;
}

// What the two cores do for one frame: the helper runs job(1) while the caller runs job(0).
// A pair function (the scene's object transform, see run_pair) takes the helper first.
void (*g_pair_fn)(void *, int) = nullptr;
void *g_pair_arg = nullptr;
void job(int core) {
    if (g_pair_fn) { g_pair_fn(g_pair_arg, core); return; }
    if (g.ntiles) tile_worker(core);
    else if (core == 0) band(0, g.split, g.flags[0], &g.us_band[0], 0);
    else band(g.split, g.h, g.flags[1], &g.us_band[1], 1);
}

#ifdef ESP_PLATFORM
TaskHandle_t      s_helper = nullptr;
SemaphoreHandle_t s_go = nullptr, s_done = nullptr;
void helper_task(void *) {
    for (;;) {
        xSemaphoreTake(s_go, portMAX_DELAY);
        job(1);
        xSemaphoreGive(s_done);
    }
}

// One helper, created on first use and kept (a scene is set up/torn down per game run; a task per
// run would churn the heap). Unpinned at the worker's priority: the SMP scheduler puts it on
// whichever core the worker isn't using. PSRAM stack: it never writes flash (engineering rules).
// (Measured 2026-10: two pinned helpers with internal-SRAM stacks, the app thread waiting, were no
// faster per pixel and slower overall - the rasteriser's spills are not the cost.)
bool helper_start(void) {
    if (s_helper) return true;
    if (!s_go) s_go = xSemaphoreCreateBinary();
    if (!s_done) s_done = xSemaphoreCreateBinary();
    if (!s_go || !s_done) return false;
    const UBaseType_t prio = uxTaskPriorityGet(nullptr);
    if (xTaskCreatePinnedToCoreWithCaps(helper_task, "vx_band", 12 * 1024, nullptr, prio, &s_helper,
                                        tskNO_AFFINITY, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        s_helper = nullptr;
        return false;
    }
    return true;
}
void run_parallel(void) {
    xSemaphoreGive(s_go);
    job(0);
    xSemaphoreTake(s_done, portMAX_DELAY);   // bounded: finite work over capped scenes
}
#else
bool helper_start(void) { return true; }
void run_parallel(void) {
    std::thread t([] { job(1); });
    job(0);
    t.join();
}
#endif
// Scene::PairRunner: prepareFrame splits cull + transform of the objects across both cores.
[[maybe_unused]] void run_pair(void (*fn)(void *, int), void *arg) {
    g_pair_fn = fn; g_pair_arg = arg;
    run_parallel();                          // the semaphores order these stores for the helper
    g_pair_fn = nullptr; g_pair_arg = nullptr;
}

// The core's frontend hook: prepareFrame() has queued + sorted every visible triangle. Project the
// particles, bin the queue into row tiles (tile mode), rasterise on both cores, count what was
// drawn. Band mode also moves the cut toward equal time: each band's measured cost per row
// predicts where both sides cost the same; half a step per frame keeps it from ringing.
int64_t g_t_render0, g_pf_objs, g_pf_misc, g_pf_bin;     // prep breakdown (profile line)
void exec_bands(Scene &sc) {
    const int64_t tm0 = now_us();
    g_pf_objs += tm0 - g_t_render0;                       // cull + transform + sort of every object
    particles_update();
    if (g.floor_dirty) floor_relight();
    caustics_apply();
    bg_frame_setup();
    const int64_t tm1 = now_us();
    g_pf_misc += tm1 - tm0;
    g.queued = sc.lastFrameDrawnTriangles;
    const int need = g.queued > 0 ? g.queued : 1;
    if (need > g.flags_cap) {
        for (int i = 0; i < 2; i++) { psram_free(g.flags[i]); g.flags[i] = (uint8_t *)psram_calloc((size_t)need); }
        g.flags_cap = (g.flags[0] && g.flags[1]) ? need : 0;
    }
    if (!g.flags_cap) {   // no memory for the flags: one band, still correct
        const int nt = g.ntiles;
        g.ntiles = 0;
        band(0, g.h, nullptr, &g.us_band[0], 0);
        g.ntiles = nt;
        g.us_band[1] = 0;
        g.rasterized = sc.lastFrameRasterizedTriangles;
        return;
    }
    memset(g.flags[0], 0, (size_t)need);
    memset(g.flags[1], 0, (size_t)need);
    if (g.ntiles) {
        // Bin the queue per tile (grow the list if a busy frame overflows it).
        int got = -1;
        // The bin list holds 16-bit queue indices: past 65535 queued triangles no list size helps
        // (it was doubled twice a frame for nothing) - straight to the band fallback.
        const int max_tries = need > 65535 ? 0 : 3;
        for (int tries = 0; tries < max_tries && got < 0; tries++) {
            if (!g.bin_list || g.bin_cap < need * 2 + 64 || tries) {
                const int cap = std::max(need * 3 + 64, g.bin_cap * 2);
                psram_free(g.bin_list);
                g.bin_list = (uint16_t *)psram_calloc((size_t)cap * sizeof(uint16_t));
                g.bin_cap = g.bin_list ? cap : 0;
            }
            if (g.bin_list) got = sc.vxBin(g.tile_h, g.ntiles, g.bin_list, g.bin_cap, g.bin_start);
        }
        g_pf_bin += now_us() - tm1;
        if (got < 0) {   // could not bin: this frame goes through the PSRAM bands
            const int nt = g.ntiles;
            g.ntiles = 0;
            run_parallel();
            g.ntiles = nt;
        } else {
            g_next_tile.store(0, std::memory_order_relaxed);
            run_parallel();
        }
    } else {
        run_parallel();
    }
    int n = 0;
    for (int i = 0; i < g.queued; i++) n += (g.flags[0][i] | g.flags[1][i]);
    g.rasterized = n;
    sc.lastFrameRasterizedTriangles = n;

    const int64_t a = g.us_band[0], b = g.us_band[1];
    if (!g.ntiles && a > 0 && b > 0 && g.split > 0 && g.split < g.h) {
        const double ca = (double)a / g.split, cb = (double)b / (g.h - g.split);   // µs per row
        const int target = (int)(g.h * cb / (ca + cb));
        g.split = clampi(g.split + (target - g.split) / 2, g.h / 8, g.h - g.h / 8);
    }
}

void free_mips(Texture *t);   // below, with build_mips
void free_scene_content(void) {
    if (g.scene) g.scene->getObjects().clear();
    g.floor_on = false; g.floor_src = nullptr; g.pano_px = nullptr;   // their textures go below
    g.ceil_on = false; g.ceil_px = nullptr;
    if (g.floor_lit) { sram_free(g.floor_lit); g.floor_lit = nullptr; }
    psram_free(g.floor_base); g.floor_base = nullptr;
    for (int i = 0; i < g.nobj; i++) { delete g.obj[i]; g.obj[i] = nullptr; g.obj_v[i] = g.obj_t[i] = 0; }
    memset(g.parent1, 0, sizeof g.parent1); memset(g.shadow1, 0, sizeof g.shadow1);
    g.any_parent = g.any_shadow = false;
    for (auto &L : g.lod) L = Engine::Lod{};
    for (auto &m : g.lod_of) m = 0;
    g.any_lod = false;
    for (int i = 0; i < g.nmat; i++) { delete g.mat[i]; g.mat[i] = nullptr; }
    for (int i = 0; i < g.ntex; i++) { free_mips(g.tex[i]); delete g.tex[i]; g.tex[i] = nullptr; psram_free(g.texpx[i]); g.texpx[i] = nullptr; }
    for (int e = 0; e < VX_MAX_EMITTERS; e++) { psram_free(g.em[e].p); g.em[e] = Emitter(); }
    g.nobj = g.nmat = g.ntex = 0;
    g.tris = g.verts = 0;
    g.nspr = g.live = 0;
    g.pick_armed = false;
    g.picked = -1;
}

inline bool pow2_side(int v) { return v >= 8 && v <= VX_MAX_TEX_SIDE && (v & (v - 1)) == 0; }

// Mip chain (1.3): up to four half-size levels under a static texture, box-filtered; with a colour
// key a texel of the smaller level is transparent when most of its four parents are, else the mean
// of the opaque ones. The fast span picks the level where one pixel covers about one texel: far or
// small geometry then reads a small, cache-friendly level instead of scattering over a big texture
// in PSRAM (and stops shimmering).
void build_mips(Texture *t) {
    Texture *cur = t;
    for (int lv = 0; lv < 4 && cur->width >= 16 && cur->height >= 16; lv++) {
        const int w = cur->width / 2, h = cur->height / 2;
        if (vx_mem_used() + (size_t)w * h * 2 > VX_MEM_BUDGET) break;
        uint16_t *px = (uint16_t *)psram_calloc((size_t)w * h * 2);
        if (!px) break;
        const uint16_t *s = cur->data, key = t->alphaColor;
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++) {
                const uint16_t q[4] = { s[(2 * y) * cur->width + 2 * x], s[(2 * y) * cur->width + 2 * x + 1],
                                        s[(2 * y + 1) * cur->width + 2 * x], s[(2 * y + 1) * cur->width + 2 * x + 1] };
                int r = 0, gg = 0, b = 0, n = 0;
                for (int k = 0; k < 4; k++) {
                    if (t->hasAlpha && q[k] == key) continue;
                    r += q[k] >> 11; gg += (q[k] >> 5) & 63; b += q[k] & 31; n++;
                }
                uint16_t c = key;
                if (!t->hasAlpha || n >= 2) {
                    c = (uint16_t)(((r / n) << 11) | ((gg / n) << 5) | (b / n));
                    if (t->hasAlpha && c == key) c ^= 1;                // never turn into the key by accident
                }
                px[y * w + x] = c;
            }
        Texture *m = new Texture(w, h, px, t->hasAlpha, t->alphaColor, false, t->addressMode);
        cur->mip = m;
        cur = m;
    }
}
void free_mips(Texture *t) {
    Texture *m = t ? t->mip : nullptr;
    while (m) { Texture *n = m->mip; psram_free(m->data); delete m; m = n; }
    if (t) t->mip = nullptr;
}

// .vxm model, little-endian (tools/vertice/obj2vxm.py writes it):
//   "VXM1" | u16 nverts | u16 ntris | u8 nmats | u8 flags (1 smooth, 2 uv) | u16 0
//   mats[nmats]  { u16 color565; u8 shading; u8 alpha }
//   xyz[nverts]  { i32 x, y, z }
//   uv[nverts]   { i16 u, v }                      (flags & 2)
//   tris[ntris]  { u16 a, b, c; u8 mat; u8 0 }
template <class T> T rd(const uint8_t *p) { T v; memcpy(&v, p, sizeof v); return v; }

}  // namespace

// =================================================================================================
extern "C" {

bool vx_is_open(void) { return g.open; }

bool vx_open(int w, int h) {
    if (g.open) vx_close();
    if (w < 16 || h < 16 || w > 1024 || h > 600 || (w & 1)) return false;
    if (!helper_start()) { VX_LOGW("band helper unavailable"); return false; }
    g.w = w; g.h = h;
    g.zbuf = (uint16_t *)psram_calloc((size_t)w * h * 2);
    g.sky = (uint16_t *)psram_calloc((size_t)h * 2);
    g.spr = (Sprite *)psram_calloc(sizeof(Sprite) * kMaxSprites);
    g.pano_u = (int16_t *)psram_calloc(sizeof(int16_t) * (size_t)w);
    // SRAM tiles. Internal SRAM is read through the ONE 64 KB L1 data cache both cores share
    // (2-way, 32 KB ways), so the working set is kept to half of it: 8 rows of colour + depth per
    // core, 32 KB at 512 px wide, carved from ONE block so the four buffers map to distinct cache
    // sets. Fewer rows for wider canvases; none (PSRAM bands) if the heap can't spare it.
    g.ntiles = 0;
    g.tile_block = nullptr;
    // Per core: colour + depth, and a second colour buffer for the async copy when it fits (taller
    // tiles first: a triangle spanning several tiles is set up again in each one). Measured on the
    // board (2026-10, Vertice Bass): 12 rows without the async copy {12,2} was SLOWER than {8,3}
    // (tiles 38.9 vs 37.4 ms) - the overlapped copy is worth more than a third fewer setups.
    struct Plan { int th, bufs; };
    for (const Plan pl : { Plan{12, 3}, Plan{8, 3}, Plan{8, 2}, Plan{4, 3}, Plan{4, 2} }) {
        const size_t one = (size_t)pl.th * w * 2;
        if (one * 2 * pl.bufs > 48 * 1024 && pl.th > 4) continue;
        g.tile_block = (uint16_t *)sram_alloc(one * 2 * pl.bufs);
        if (!g.tile_block) continue;
        for (int c = 0; c < 2; c++) {
            uint16_t *base = g.tile_block + (size_t)(c * pl.bufs) * (one / 2);
            g.tcol[c] = base;
            g.tz[c] = base + one / 2;
            g.tcol2[c] = pl.bufs == 3 ? base + one : nullptr;
        }
        g.tile_h = pl.th;
        g.ntiles = (h + pl.th - 1) / pl.th;
        g.tile_bytes = one * 2 * pl.bufs;
        break;
    }
#ifdef ESP_PLATFORM
    for (int c = 0; c < 2; c++)
        for (int k = 0; k < 2; k++)
            if (!g.tdone[c][k]) g.tdone[c][k] = (void *)xSemaphoreCreateBinary();   // kept across scenes
#endif
    if (!g.zbuf || !g.sky || !g.spr) { vx_close(); return false; }
    g.scene = new Scene(nullptr, g.zbuf, w, h);
    g.scene->setClearBuffer(false);            // the bands clear their own rows, in parallel
    if (g.tile_block) {   // the idle tiles are the transform's scratch while the frame is prepared
        const size_t half = (g.tile_bytes / 2) & ~(size_t)15;
#ifdef ESP_PLATFORM
        // one lane only on the board (no PairRunner, below): it gets the whole block, twice the
        // vertices before a mesh falls back to the PSRAM scratch
        g.scene->setPrepScratch(g.tile_block, g.tile_bytes & ~(size_t)15, nullptr, 0);
#else
        if (getenv("VX_PARALLEL_PREP")) g.scene->setPrepScratch(g.tile_block, half, (uint8_t *)g.tile_block + half, half);
        else g.scene->setPrepScratch(g.tile_block, g.tile_bytes & ~(size_t)15, nullptr, 0);
#endif
    }
    // The object transform on both cores (Scene::PairRunner) is off: measured on the board (Vertice
    // Bass, 2026-10) it gained nothing — the two cores wait on the same PSRAM (mesh reads, the queue),
    // each op ~1.6x slower. Kept for the simulator A/B and for scenes with SRAM-resident meshes.
#ifndef ESP_PLATFORM
    if (getenv("VX_PARALLEL_PREP")) g.scene->setPairRunner(run_pair);
#endif
    g.scene->backgroundGradientColors = g.sky;  // fog fades into the sky of each row (fast spans)
    g.cam = new Camera();
    g.cam->setPosition(0, 150, -600);
    g.cam->setFOV((int32_t)70, (int32_t)w);
    g.cam->farPlane = 4000;
    g.scene->setCamera(g.cam);
    g.sun = new DirectionalLight(Vector3{240, 45, 0}, Color{255, 246, 228}, 230);   // front-left, high
    g.amb = new AmbientLight(Color{60, 66, 80});
    g.scene->setDirectionalLight(g.sun);
    g.scene->setAmbientLight(g.amb);
    g.defmat = new Material(0xFFFF);
    g.defmat->shadingMode = ShadingMode::GOURAUD;
    g.depth = true;
    g.scene->getRenderer()->setDepthTestingEnabled(true);
    set_fog(1 << 20, (1 << 20) + 1);
    build_sky(0x3A7F, 0xBEDF);                 // soft blue sky
    g.split = h / 2;
    g.us_total = g.us_prep = g.us_band[0] = g.us_band[1] = 0;
    g.rasterized = g.queued = 0;
    g.last_us = 0;
    if (!g.rng) g.rng = 0x9E3779B9u;
    g.pick_armed = false;
    g.picked = -1;
    g.open = true;
#ifdef ESP_PLATFORM
    {
        const size_t caps = MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA | MALLOC_CAP_8BIT;
        VX_LOGI("open %dx%d, %d-row SRAM tiles (%u bytes held; internal DMA SRAM free %u, largest %u)", w, h,
                g.ntiles ? g.tile_h : 0, (unsigned)vx_mem_used(), (unsigned)heap_caps_get_free_size(caps),
                (unsigned)heap_caps_get_largest_free_block(caps));
    }
#else
    VX_LOGI("open %dx%d, %d-row SRAM tiles (%u bytes held)", w, h, g.ntiles ? g.tile_h : 0, (unsigned)vx_mem_used());
#endif
    return true;
}

void vx_close(void) {
    vxMipBias = 0; vxNoTextures = false; vxEagerBg = false;   // vx_config settings end with the engine
    free_scene_content();
    delete g.scene; g.scene = nullptr;
    delete g.cam; g.cam = nullptr;
    delete g.sun; g.sun = nullptr;
    delete g.amb; g.amb = nullptr;
    delete g.defmat; g.defmat = nullptr;
    psram_free(g.zbuf); g.zbuf = nullptr;
    psram_free(g.sky); g.sky = nullptr;
    psram_free(g.spr); g.spr = nullptr;
    psram_free(g.pano_u); g.pano_u = nullptr;
    psram_free(g.caus); g.caus = nullptr;
    psram_free(g.shaft); g.shaft = nullptr;
    g.caus_k = g.shaft_k = 0;
    sram_free(g.tile_block); g.tile_block = nullptr;
    for (int c = 0; c < 2; c++) g.tcol[c] = g.tz[c] = g.tcol2[c] = nullptr;
    g.ntiles = 0;
    psram_free(g.bin_list); g.bin_list = nullptr; g.bin_cap = 0;
    for (int i = 0; i < 2; i++) { psram_free(g.flags[i]); g.flags[i] = nullptr; }
    g.flags_cap = 0;
    g.target = nullptr;
    if (g.open) VX_LOGI("closed (%u bytes still held)", (unsigned)vx_mem_used());
    g.open = false;
}

void vx_reset(void) {
    if (g.open) free_scene_content();
}

int vx_texture(const uint16_t *px, int w, int h, int flags) {
    if (!g.open || !px || !pow2_side(w) || !pow2_side(h) || g.ntex >= VX_MAX_TEXTURES) return -1;
    const size_t bytes = (size_t)w * h * 2;
    if (vx_mem_used() + bytes > VX_MEM_BUDGET) return -1;
    uint16_t *copy = (uint16_t *)psram_calloc(bytes);
    if (!copy) return -1;
    memcpy(copy, px, bytes);
    Texture *t = new Texture(w, h, copy, (flags & VX_TEX_KEY) != 0, 0xF81F, false,
                             (flags & VX_TEX_CLAMP) ? CLAMP : WRAP);
    build_mips(t);                                   // static textures only (vx_texture_new ones change)
    g.tex[g.ntex] = t;
    g.texpx[g.ntex] = copy;
    return g.ntex++;
}

int vx_texture_new(int w, int h, uint32_t color565, int flags) {
    if (!g.open || !pow2_side(w) || !pow2_side(h) || g.ntex >= VX_MAX_TEXTURES) return -1;
    const size_t bytes = (size_t)w * h * 2;
    if (vx_mem_used() + bytes > VX_MEM_BUDGET) return -1;
    uint16_t *px = (uint16_t *)psram_calloc(bytes);
    if (!px) return -1;
    for (int i = 0; i < w * h; i++) px[i] = (uint16_t)color565;
    Texture *t = new Texture(w, h, px, (flags & VX_TEX_KEY) != 0, 0xF81F, false,
                             (flags & VX_TEX_CLAMP) ? CLAMP : WRAP);
    g.tex[g.ntex] = t;
    g.texpx[g.ntex] = px;
    return g.ntex++;
}

void vx_texture_write(int tex, int x, int y, int w, int h, const uint16_t *px) {
    if (!g.open || !px || tex < 0 || tex >= g.ntex || !g.tex[tex] || w <= 0 || h <= 0) return;
    const Texture *t = g.tex[tex];
    if (x < 0 || y < 0 || x + w > t->width || y + h > t->height) return;
    for (int r = 0; r < h; r++) memcpy(g.texpx[tex] + (size_t)(y + r) * t->width + x, px + (size_t)r * w, (size_t)w * 2);
    if (t->data == g.floor_src && g.floor_lit) g.floor_dirty = true;   // the floor samples a lit copy
}

int vx_material(uint32_t color565, int shading, int alpha, int tex, int specular) {
    if (!g.open || g.nmat >= VX_MAX_MATERIALS) return -1;
    static const ShadingMode kModes[] = { ShadingMode::FLAT, ShadingMode::GOURAUD, ShadingMode::PHONG,
                                          ShadingMode::WIREFRAME, ShadingMode::UNLIT, ShadingMode::ADDITIVE };
    Texture *t = (tex >= 0 && tex < g.ntex) ? g.tex[tex] : nullptr;
    Material *m = new Material((uint16_t)color565, t, nullptr, false, (uint8_t)clampi(alpha, 0, 255),
                               255, (uint8_t)clampi(specular, 0, 255));
    m->shadingMode = kModes[clampi(shading, 0, (int)(sizeof kModes / sizeof kModes[0]) - 1)];
    if (m->shadingMode == ShadingMode::PHONG && specular > 0) m->specularExponent = 32;
    m->perspectiveCorrect = t != nullptr;   // subdivided perspective UVs (core/Renderer.cpp, vxPersp)
    g.mat[g.nmat] = m;
    return g.nmat++;
}

void vx_mat_color(int mat, uint32_t color565) {
    if (g.open && mat >= 0 && mat < g.nmat && g.mat[mat]) g.mat[mat]->color = (uint16_t)color565;
}

int vx_prim(int kind, int a, int b, int c, int mat, int mat2) {
    if (!g.open || free_slot() < 0) return -1;
    constexpr int kMaxDim = 1 << 16;   // world units; keeps the core's int32 fixed-point math in range
    if (a < 1 || a > kMaxDim || b < 0 || b > kMaxDim || c < 0 || c > kMaxDim) return -1;
    Material *m = mat_or_default(mat), *m2 = mat_or_default(mat2 >= 0 ? mat2 : mat);
    int seg = 0, verts = 0, tris = 0;
    switch (kind) {
    case VX_CUBE:      if (b < 1 || c < 1) return -1; verts = 24; tris = 12; break;
    case VX_SPHERE:    seg = clampi(b, 3, 48); verts = (seg + 1) * (seg + 1); tris = 2 * seg * seg; break;
    case VX_CYLINDER:
    case VX_CAPSULE:   if (b < 1) return -1; seg = clampi(c, 3, 48); verts = seg * seg + 4 * seg + 8;
                       tris = 2 * seg * seg + 4 * seg; break;
    case VX_PYRAMID:   if (b < 1) return -1; verts = 16; tris = 6; break;
    case VX_PLANE:     if (b < 1) return -1; verts = 4; tris = 2; break;
    case VX_GRID:      if (b < 1) return -1; seg = clampi(c, 1, 64); verts = 4 * seg * seg; tris = 2 * seg * seg; break;
    case VX_QUAD:
    case VX_BILLBOARD: if (b < 1) return -1; verts = 4; tris = 2; break;
    default: return -1;
    }
    if (!room_for(verts, tris)) return -1;
    Object *o = nullptr;
    switch (kind) {
    case VX_CUBE:      o = Primitives::createCube(a, b, c, m); break;
    case VX_SPHERE:    o = Primitives::createSphere(a, seg, m); break;
    case VX_CYLINDER:  o = Primitives::createCylinder(a, b, seg, true, m); break;
    case VX_CAPSULE:   o = Primitives::createCapsule(a, b, seg, m); break;
    case VX_PYRAMID:   o = Primitives::createPyramid(a, b, m); break;
    case VX_PLANE:     o = Primitives::createPlane(a, b, m); break;
    case VX_GRID:      o = Primitives::createGrid(a, b, seg, seg, m, m2, true); break;
    case VX_QUAD:      o = Primitives::createQuad(a, b, m); break;
    case VX_BILLBOARD: o = Primitives::createBillboard(a, b, m); break;
    }
    // Impostors are seen from every side: never cull them (the camera-facing turn may present
    // either winding).
    if (o && kind == VX_BILLBOARD) o->cullingMode = CullingMode::NO_CULLING;
    return add_object(o);   // books what was really built (the estimate above only gates it)
}

int vx_mesh(const int32_t *xyz, int nverts, const uint16_t *idx, int ntris, const int16_t *uv,
            const uint8_t *tri_mat, int mat, int flags) {
    if (!g.open || !xyz || !idx) return -1;
    return build_mesh(xyz, nverts, idx, ntris, uv, tri_mat, mat, flags);
}

int vx_model(const uint8_t *d, size_t len, int flags) {
    if (!g.open || !d || len < 12 || memcmp(d, "VXM1", 4)) return -1;
    const int nv = rd<uint16_t>(d + 4), nt = rd<uint16_t>(d + 6), nm = d[8], mf = d[9];
    const size_t o_mat = 12, o_xyz = o_mat + (size_t)nm * 4, o_uv = o_xyz + (size_t)nv * 12;
    const size_t o_tri = o_uv + ((mf & 2) ? (size_t)nv * 4 : 0), end = o_tri + (size_t)nt * 8;
    if (nv < 3 || nt < 1 || nm < 1 || end > len || g.nmat + nm > VX_MAX_MATERIALS) return -1;
    uint8_t handle[256];
    for (int i = 0; i < nm; i++) {
        const uint8_t *m = d + o_mat + i * 4;
        const int h = vx_material(rd<uint16_t>(m), m[2], m[3], -1, 0);
        if (h < 0) return -1;
        handle[i] = (uint8_t)h;
    }
    int32_t *xyz = (int32_t *)psram_calloc((size_t)nv * 12);
    uint16_t *idx = (uint16_t *)psram_calloc((size_t)nt * 6);
    uint8_t *tm = (uint8_t *)psram_calloc((size_t)nt);
    int16_t *uv = (mf & 2) ? (int16_t *)psram_calloc((size_t)nv * 4) : nullptr;
    int id = -1;
    if (xyz && idx && tm && (uv || !(mf & 2))) {
        memcpy(xyz, d + o_xyz, (size_t)nv * 12);
        if (uv) memcpy(uv, d + o_uv, (size_t)nv * 4);
        bool ok = true;
        for (int t = 0; t < nt && ok; t++) {
            const uint8_t *p = d + o_tri + (size_t)t * 8;
            for (int k = 0; k < 3; k++) idx[t * 3 + k] = rd<uint16_t>(p + k * 2);
            ok = p[6] < nm;
            tm[t] = ok ? handle[p[6]] : 0;
        }
        if (ok) id = build_mesh(xyz, nv, idx, nt, uv, tm, -1, flags | ((mf & 1) ? VX_MESH_SMOOTH : 0));
    }
    psram_free(xyz); psram_free(idx); psram_free(tm); psram_free(uv);
    return id;
}

int vx_clone(int id) {
    if (!g.open || !valid_obj(id) || free_slot() < 0 || !room_for(g.obj_v[id], g.obj_t[id])) return -1;
    return add_object(new Object(*g.obj[id]));
}

void vx_obj_free(int id) {
    if (!g.open || !valid_obj(id)) return;
    Object *o = g.obj[id];
    lod_forget(id);
    if (g.shadow1[id]) { const int s = g.shadow1[id] - 1; g.shadow1[id] = 0; if (s != id) vx_obj_free(s); }
    g.parent1[id] = 0;
    for (int i = 0; i < g.nobj; i++) {                  // nothing may point at a freed slot (it is reused)
        if (g.parent1[i] == id + 1) g.parent1[i] = 0;
        if (g.shadow1[i] == id + 1) g.shadow1[i] = 0;
    }
    auto &v = g.scene->getObjects();
    v.erase(std::remove(v.begin(), v.end(), o), v.end());
    delete o;
    g.obj[id] = nullptr;
    g.verts -= g.obj_v[id];
    g.tris -= g.obj_t[id];
    g.obj_v[id] = g.obj_t[id] = 0;
    if (g.picked == id) g.picked = -1;
}

void vx_obj_pos(int id, int x, int y, int z) {
    if (!g.open || !valid_obj(id)) return;
    if (g.parent1[id]) { g.lpos[id][0] = x; g.lpos[id][1] = y; g.lpos[id][2] = z; return; }   // relative
    g.obj[id]->setPosition(x, y, z);
}
void vx_obj_rot(int id, int rx, int ry, int rz) {
    if (!g.open || !valid_obj(id)) return;
    if (g.parent1[id]) { g.lrot[id][0] = (int16_t)wrap360(rx); g.lrot[id][1] = (int16_t)wrap360(ry); g.lrot[id][2] = (int16_t)wrap360(rz); return; }
    g.obj[id]->setRotation(wrap360(rx), wrap360(ry), wrap360(rz));
}
void vx_obj_scale(int id, int percent) {
    if (g.open && valid_obj(id)) g.obj[id]->vxScale = (float)clampi(percent, 5, 1000) / 100.0f;
}
void vx_obj_show(int id, bool on) {
    if (!g.open || !valid_obj(id) || g.lod_of[id]) return;   // stand-ins follow their master
    g.lod[id].shown = on;
    g.obj[id]->enabled = on;
}

int vx_obj_lod(int id, int lod, int dist) {
    if (!g.open || !valid_obj(id) || !valid_obj(lod) || id == lod) return -1;
    Engine::Lod &L = g.lod[id];
    if (g.lod_of[id] || g.lod_of[lod] || g.lod[lod].n || L.n >= VX_MAX_LODS) return -1;
    dist = clampi(dist, 1, 1 << 20);
    if (L.n && dist <= L.dist[L.n - 1]) return -1;                 // levels go farther out
    if (!L.n) L.shown = g.obj[id]->enabled;
    L.id[L.n] = (int16_t)lod;
    L.dist[L.n] = dist;
    L.n++;
    g.lod_of[lod] = (int16_t)(id + 1);
    g.obj[lod]->enabled = false;
    g.any_lod = true;
    return 0;
}
void vx_obj_depth(int id, int bias, int flags) {
    if (!g.open || !valid_obj(id)) return;
    Object *o = g.obj[id];
    o->zBias = (int8_t)clampi(bias, -127, 127);
    o->ignoreZBuffer = (flags & VX_DEPTH_NOTEST) != 0;
    o->noWriteZBuffer = (flags & VX_DEPTH_NOWRITE) != 0;
}

void vx_camera(int x, int y, int z, int rx, int ry, int rz) {
    if (!g.open) return;
    g.cam->setPosition(x, y, z);
    g.cam->setRotation(wrap360(rx), wrap360(ry), wrap360(rz));
}
void vx_look_at(int x, int y, int z) { if (g.open) g.cam->lookAt(Vector3{x, y, z}); }
void vx_lens(int fov_deg, int znear, int zfar) {
    if (!g.open) return;
    g.cam->setFOV((int32_t)clampi(fov_deg, 20, 150), (int32_t)g.w);
    znear = clampi(znear, 16, 16384);
    g.cam->nearPlane = znear;
    g.cam->farPlane = clampi(zfar, znear + 64, 65535);   // the depth buffer holds 16-bit camera Z
}
void vx_sun(int azimuth, int elevation, uint32_t rgb888, int intensity) {
    if (!g.open) return;
    g.sun->color = Color{(uint8_t)(rgb888 >> 16), (uint8_t)(rgb888 >> 8), (uint8_t)rgb888};
    g.sun->intensity = (uint16_t)clampi(intensity, 0, 255);
    g.sun->updateDirection(Vector3{wrap360(azimuth), wrap360(elevation), 0});
    g.floor_dirty = g.floor_lit != nullptr;
}
void vx_ambient(uint32_t rgb888) {
    if (!g.open) return;
    g.amb->color = Color{(uint8_t)(rgb888 >> 16), (uint8_t)(rgb888 >> 8), (uint8_t)rgb888};
    g.floor_dirty = g.floor_lit != nullptr;
}

void vx_floor(int y, int tex, int repeat, uint32_t color565) {
    if (!g.open) return;
    if (g.floor_lit) { sram_free(g.floor_lit); g.floor_lit = nullptr; }
    g.floor_src = nullptr;
    g.floor_on = repeat > 0;   // repeat 0 = no floor
    if (!g.floor_on) return;
    g.floor_y = clampi(y, -(1 << 16), 1 << 16);
    g.floor_repeat = clampi(repeat, 16, 1 << 16);
    g.floor_color = (uint16_t)color565;
    if (tex >= 0 && tex < g.ntex && g.tex[tex]) {
        const Texture *t = g.tex[tex];
        g.floor_w = t->width; g.floor_h = t->height;
        g.floor_wshift = 0;
        while ((1 << g.floor_wshift) < g.floor_w) g.floor_wshift++;
        g.floor_src = t->data;
        g.floor_lit = (uint16_t *)sram_alloc((size_t)g.floor_w * g.floor_h * 2);   // hot: sampled every pixel
        if (!g.floor_lit) g.floor_lit = (uint16_t *)psram_calloc((size_t)g.floor_w * g.floor_h * 2);
        if (g.caus_k > 0 && g.floor_lit) g.floor_base = (uint16_t *)psram_calloc((size_t)g.floor_w * g.floor_h * 2);
        g.floor_dirty = true;
    }
}

// Under water (1.3). Caustics: a shimmering net of focused sunlight on the floor texture, strength
// 0..256 (0 = off), speed 64 = normal drift. Shafts: slanted rays of light from the surface down the
// upper three quarters of the view, strength 0..256, slope = x pixels per 64 rows.
void vx_caustics(int strength, int speed) {
    if (!g.open) return;
    g.caus_k = clampi(strength, 0, 256);
    g.caus_speed = clampi(speed, 0, 512);
    if (g.caus_k && !g.caus) {
        g.caus = (uint8_t *)psram_calloc((size_t)kCausN * kCausN * 2);
        if (!g.caus) { g.caus_k = 0; return; }
        caustic_layer(g.caus, 0x1234567u);
        caustic_layer(g.caus + kCausN * kCausN, 0x89ABCDEu);
    }
    if (g.caus_k && g.floor_lit && !g.floor_base)
        g.floor_base = (uint16_t *)psram_calloc((size_t)g.floor_w * g.floor_h * 2);
    if (!g.caus_k && g.floor_base) { psram_free(g.floor_base); g.floor_base = nullptr; }
    g.floor_dirty = g.floor_lit != nullptr;
}
void vx_shafts(int strength, int slope) {
    if (!g.open) return;
    g.shaft_k = clampi(strength, 0, 256);
    g.shaft_slope = clampi(slope, -256, 256);
    if (g.shaft_k && !g.shaft) {
        g.shaft = (uint8_t *)psram_calloc(512);
        if (!g.shaft) { g.shaft_k = 0; return; }
        shafts_build(0xC0FFEEu);
    }
}

void vx_ceiling(int y, int tex, int repeat) {
    if (!g.open) return;
    g.ceil_on = false;
    g.ceil_px = nullptr;
    if (repeat <= 0 || tex < 0 || tex >= g.ntex || !g.tex[tex]) return;
    const Texture *t = g.tex[tex];
    g.ceil_y = clampi(y, -(1 << 16), 1 << 16);
    g.ceil_repeat = clampi(repeat, 16, 1 << 16);
    g.ceil_w = t->width; g.ceil_h = t->height; g.ceil_wshift = 0;
    while ((1 << g.ceil_wshift) < g.ceil_w) g.ceil_wshift++;
    g.ceil_px = t->data;
    g.ceil_on = true;
}

void vx_water(int strength, int wave) {
    if (!g.open) return;
    g.water_k = clampi(strength, 0, 256);
    g.water_wave = clampi(wave, 0, 16);
}

void vx_panorama(int tex, int horizon_row) {
    if (!g.open) return;
    g.pano_px = nullptr;
    if (tex < 0 || tex >= g.ntex || !g.tex[tex]) return;
    const Texture *t = g.tex[tex];
    g.pano_px = t->data; g.pano_w = t->width; g.pano_h = t->height;
    // horizon_row bits 12-15: how many times the texture goes round (0 = once). A 1024-wide texture
    // round 360 degrees is ~3 screen pixels a texel; twice round, half that (a sky can repeat unseen).
    const int reps = (horizon_row >> 12) & 15;
    g.pano_reps = reps > 0 ? reps : 1;
    g.pano_hrow = clampi(horizon_row & 0xFFF, 0, t->height);
}
void vx_sky(uint16_t top, uint16_t bottom) { if (g.open) build_sky(top, bottom); }
void vx_fog(int znear, int zfar) {
    if (!g.open) return;
    if (zfar <= 0 || zfar <= znear) { set_fog(1 << 20, (1 << 20) + 1); return; }
    znear = clampi(znear, 0, 65535);
    set_fog(znear, clampi(zfar, znear + 1, 1 << 20));
}
void vx_depth(bool on) {
    if (!g.open) return;
    g.depth = on;
    g.scene->getRenderer()->setDepthTestingEnabled(on);
}

int vx_emitter(int max, uint32_t color0, uint32_t color1, int size0, int size1, int life_ms, int gravity,
               int flags) {
    if (!g.open) return -1;
    int e = 0;
    while (e < VX_MAX_EMITTERS && g.em[e].used) e++;
    if (e >= VX_MAX_EMITTERS) return -1;
    max = clampi(max, 1, VX_MAX_PARTICLES);
    Particle *p = (Particle *)psram_calloc(sizeof(Particle) * (size_t)max);
    if (!p) return -1;
    Emitter &em = g.em[e];
    em.used = true; em.max = max; em.n = 0; em.p = p;
    em.c0 = (uint16_t)color0; em.c1 = (uint16_t)color1;
    em.s0 = (float)clampi(size0, 1, 4096); em.s1 = (float)clampi(size1, 1, 4096);
    em.rate = 1000.0f / (float)clampi(life_ms, 16, 60000);
    em.gravity = (float)clampi(gravity, -100000, 100000);
    em.flags = flags & (VX_PART_ADDITIVE | VX_PART_NODEPTH);
    return e;
}

void vx_emit(int e, int x, int y, int z, int vx, int vy, int vz, int spread, int count) {
    if (!g.open || e < 0 || e >= VX_MAX_EMITTERS || !g.em[e].used) return;
    Emitter &em = g.em[e];
    const float sp = (float)clampi(spread, 0, 100000);
    count = clampi(count, 0, em.max);
    for (int i = 0; i < count; i++) {
        // Full pool: overwrite a random live particle, so a burst never stalls.
        Particle &p = em.p[em.n < em.max ? em.n++ : (int)(xrand() % (uint32_t)em.max)];
        p.x = (float)x; p.y = (float)y; p.z = (float)z;
        p.vx = (float)vx + frand() * sp; p.vy = (float)vy + frand() * sp; p.vz = (float)vz + frand() * sp;
        p.age = 0.0f;
    }
}

static void apply_hierarchy(void);   // 1.5, defined with the hierarchy calls below
int vx_render(uint16_t *target) {
    if (!g.open || !target) return -1;
    const int64_t t0 = now_us();
    g.target = target;
    g.scene->setFramebuffer(target);
    if (g.pick_armed) g.scene->setPickQueries(&g.pick_q, 1);
    apply_hierarchy();
    apply_lods();
    int exp_mode = -1;
    uint32_t exp_px0 = 0, exp_cyc0 = 0;
    if (s_exp_on) {                                       // diagnostics: this frame's experiment
        static const int bits[8] = { 0, 1, 2, 4, 8, 0, 0, 15 };
        exp_mode = s_exp_frame++ % 8;
        vxSpanExp = bits[exp_mode];
        vxNoTextures = exp_mode == 5;
        s_exp_solo = exp_mode == 6;
        for (int c = 0; c < 2; c++) {
            exp_px0 += g_prof[c].stat[Rasterizer::VX_STAT_PX];
            exp_cyc0 += g_prof[c].stat[Rasterizer::VX_STAT_SPAN_CYC];
        }
    }
    g_t_render0 = now_us();
    g.scene->render(exec_bands);
    restore_lods();
    if (exp_mode >= 0) {
        uint32_t px1 = 0, cyc1 = 0;
        for (int c = 0; c < 2; c++) {
            px1 += g_prof[c].stat[Rasterizer::VX_STAT_PX];
            cyc1 += g_prof[c].stat[Rasterizer::VX_STAT_SPAN_CYC];
        }
        s_exp_px[exp_mode] += px1 - exp_px0;
        s_exp_cyc[exp_mode] += (uint64_t)(cyc1 - exp_cyc0) * 16;
        s_exp_us[exp_mode] += (uint64_t)(now_us() - g_t_render0);
        s_exp_n[exp_mode]++;
        vxSpanExp = 0; vxNoTextures = false; s_exp_solo = false;
        if (s_exp_frame % 400 == 0) {
            static const char *const nm[8] = { "base", "nodepth", "nostore", "nofog", "nomod", "notex", "1core", "bare" };
            for (int h = 0; h < 2; h++) {                  // two short lines (the log keeps ~200 chars)
                char line[160]; int k = 0;
                for (int m = h * 4; m < h * 4 + 4; m++) {
                    const unsigned cpp = s_exp_px[m] ? (unsigned)(s_exp_cyc[m] / s_exp_px[m]) : 0;
                    const unsigned ms10 = s_exp_n[m] ? (unsigned)(s_exp_us[m] / s_exp_n[m] / 100) : 0;
                    k += snprintf(line + k, sizeof line - (size_t)k, "%s %u/%u.%u ", nm[m], cpp, ms10 / 10, ms10 % 10);
                }
                VX_LOGI("spanexp%d cyc/px/ms: %s", h, line);
            }
            memset(s_exp_cyc, 0, sizeof s_exp_cyc); memset(s_exp_px, 0, sizeof s_exp_px);
            memset(s_exp_us, 0, sizeof s_exp_us); memset(s_exp_n, 0, sizeof s_exp_n);
        }
    }
    g.proj_pos[0] = g.cam->position.x; g.proj_pos[1] = g.cam->position.y; g.proj_pos[2] = g.cam->position.z;
    g.proj_f = g.cam->fovFactor; g.proj_near = g.cam->nearPlane; g.proj_ok = true;   // vx_project's camera
    if (g.pick_armed) {
        const PickResult &r = g.scene->getPickResults()[0];
        g.picked = -1;
        if (r.hit)
            for (int i = 0; i < g.nobj; i++)
                if (g.obj[i] == r.object) { g.picked = g.lod_of[i] ? g.lod_of[i] - 1 : i; break; }
        g.scene->setPickQueries(nullptr, 0);
        g.pick_armed = false;
    }
    g.us_total = now_us() - t0;
    // Wall time minus the parallel section (the slower band): cull + transform + sort.
    g.us_prep = g.us_total - (g.us_band[0] > g.us_band[1] ? g.us_band[0] : g.us_band[1]);
    static int s_prof_frames = 0;
    static int64_t s_prof_total = 0, s_prof_prep = 0;
    s_prof_total += g.us_total; s_prof_prep += g.us_prep;
    if (++s_prof_frames >= 60) {
        const int n = s_prof_frames;
        VX_LOGI("prof prep: objects %lld, particles+floor+bg %lld, binning %lld", (long long)(g_pf_objs / n),
                (long long)(g_pf_misc / n), (long long)(g_pf_bin / n));
        {
            uint32_t *ps = g.scene->prepStat;
            VX_LOGI("prof objs: drawn %u verts %u tris %u | kcyc setup %u xform %u tris %u", (unsigned)(ps[0] / n),
                    (unsigned)(ps[1] / n), (unsigned)(ps[2] / n), (unsigned)(ps[3] * 16 / 1000 / n),
                    (unsigned)(ps[4] * 16 / 1000 / n), (unsigned)(ps[5] * 16 / 1000 / n));
            memset(ps, 0, sizeof g.scene->prepStat);
        }
        g_pf_objs = g_pf_misc = g_pf_bin = 0;
        VX_LOGI("prof %s: tot %lld prep %lld | c0 %lld/%lld/%lld/%lld cpu %lld t%d | c1 %lld/%lld/%lld/%lld cpu %lld t%d",
                g.ntiles ? "tiles" : "bands", (long long)(s_prof_total / n), (long long)(s_prof_prep / n),
                (long long)(g_prof[0].clear / n), (long long)(g_prof[0].raster / n), (long long)(g_prof[0].parts / n),
                (long long)(g_prof[0].copy / n), (long long)(g_prof[0].cpu / n), g_prof[0].tiles / n,
                (long long)(g_prof[1].clear / n), (long long)(g_prof[1].raster / n), (long long)(g_prof[1].parts / n),
                (long long)(g_prof[1].copy / n), (long long)(g_prof[1].cpu / n), g_prof[1].tiles / n);
        uint32_t st[Rasterizer::VX_STAT_N];
        for (int k = 0; k < Rasterizer::VX_STAT_N; k++) st[k] = (g_prof[0].stat[k] + g_prof[1].stat[k]) / n;
        VX_LOGI("prof tris fast %u slow %u (a%u t%u l%u o%u) px %u slowpx %u rows %u | kcyc setup %u rows %u span %u slow %u",
                (unsigned)st[0], (unsigned)st[1], (unsigned)st[10], (unsigned)st[11], (unsigned)st[12],
                (unsigned)st[13], (unsigned)st[2], (unsigned)st[8], (unsigned)st[3],
                (unsigned)(st[4] * 16 / 1000), (unsigned)(st[5] * 16 / 1000), (unsigned)(st[6] * 16 / 1000),
                (unsigned)(st[9] * 16 / 1000));
        memset(g_prof, 0, sizeof g_prof);
        s_prof_frames = 0; s_prof_total = s_prof_prep = 0;
    }
    return g.rasterized;
}

void vx_pick_at(int x, int y) {
    if (!g.open || x < 0 || y < 0 || x >= g.w || y >= g.h) return;
    g.pick_q.x = (int16_t)x;
    g.pick_q.y = (int16_t)y;
    g.pick_armed = true;
}
int vx_picked(void) { return g.open ? g.picked : -1; }

// ---- 1.5 ------------------------------------------------------------------------------------------
// Children follow their parents, shadows their owners (each frame, before the LODs and the render).
// World = parent position + R(parent) * (local offset * parent scale); angles add. R = Rz*Ry*Rx in
// degrees, the same matrix the scene applies to the parent's vertices (Scene.cpp renderObject).
static void apply_hierarchy(void) {
    if (g.any_parent)
        for (int pass = 0; pass < 4; pass++)                  // parents first: chains settle in <= 4 passes
            for (int id = 0; id < g.nobj; id++) {
                const int p = g.parent1[id] - 1;
                if (p < 0 || !g.obj[id] || !g.obj[p]) continue;
                const Object *P = g.obj[p];
                const float d2r = 3.14159265f / 180.0f;
                const float ax = P->rotation.x * d2r, ay = P->rotation.y * d2r, az = P->rotation.z * d2r;
                const float cx = std::cos(ax), sx = std::sin(ax), cy = std::cos(ay), sy = std::sin(ay), cz = std::cos(az), sz = std::sin(az);
                const float k00 = cy, k01 = sy * sx, k02 = sy * cx, k11 = cx, k12 = -sx, k20 = -sy, k21 = cy * sx, k22 = cy * cx;
                const float m00 = cz * k00, m01 = cz * k01 - sz * k11, m02 = cz * k02 - sz * k12;
                const float m10 = sz * k00, m11 = sz * k01 + cz * k11, m12 = sz * k02 + cz * k12;
                const float sc = P->vxScale > 0 ? P->vxScale : 1.0f;
                const float lx = g.lpos[id][0] * sc, ly = g.lpos[id][1] * sc, lz = g.lpos[id][2] * sc;
                g.obj[id]->setPosition(P->position.x + (int)(m00 * lx + m01 * ly + m02 * lz),
                                       P->position.y + (int)(m10 * lx + m11 * ly + m12 * lz),
                                       P->position.z + (int)(k20 * lx + k21 * ly + k22 * lz));
                g.obj[id]->setRotation(wrap360(P->rotation.x + g.lrot[id][0]), wrap360(P->rotation.y + g.lrot[id][1]),
                                       wrap360(P->rotation.z + g.lrot[id][2]));
            }
    if (g.any_shadow)
        for (int id = 0; id < g.nobj; id++) {
            const int s = g.shadow1[id] - 1;
            if (s < 0 || !g.obj[id] || !g.obj[s]) continue;
            g.obj[s]->setPosition(g.obj[id]->position.x, g.shadow_y[id], g.obj[id]->position.z);
            g.obj[s]->enabled = g.obj[id]->enabled;
        }
}
void vx_obj_material(int id, int mat) {
    if (!g.open || !valid_obj(id) || mat < 0 || mat >= g.nmat || !g.mat[mat]) return;
    Object *o = g.obj[id];
    for (auto &t : o->triangles) t.material = g.mat[mat];
    o->vxAllUnlit = o->vxAllNonSpecular = -1;           // the material scan's cache (Scene.cpp)
}
void vx_obj_alpha(int id, int alpha) {
    if (g.open && valid_obj(id)) g.obj[id]->vxAlpha = (uint8_t)clampi(alpha, 0, 255);
}
void vx_mat_set(int mat, int key, int value) {
    if (!g.open || mat < 0 || mat >= g.nmat || !g.mat[mat]) return;
    Material *m = g.mat[mat];
    switch (key) {
    case VX_MAT_COLOR:    vx_mat_color(mat, (uint32_t)value & 0xFFFF); return;
    case VX_MAT_ALPHA:    m->alpha = (uint8_t)clampi(value, 0, 255); return;
    case VX_MAT_TEXTURE:  m->diffuseMap = (value >= 0 && value < g.ntex) ? g.tex[value] : nullptr; return;
    case VX_MAT_SPECULAR: m->specular = (uint8_t)clampi(value, 0, 255); break;
    case VX_MAT_SHADING: {
        static const ShadingMode kModes[] = { ShadingMode::FLAT, ShadingMode::GOURAUD, ShadingMode::PHONG,
                                              ShadingMode::WIREFRAME, ShadingMode::UNLIT, ShadingMode::ADDITIVE };
        m->shadingMode = kModes[clampi(value, 0, (int)(sizeof kModes / sizeof kModes[0]) - 1)];
        break;
    }
    default: return;
    }
    for (int i = 0; i < g.nobj; i++)                       // shading / specular feed the per-mesh cache
        if (g.obj[i]) g.obj[i]->vxAllUnlit = g.obj[i]->vxAllNonSpecular = -1;
}
int vx_obj_parent(int child, int parent) {
    if (!g.open || !valid_obj(child)) return -1;
    if (parent < 0) {                                      // detach where it stands
        g.parent1[child] = 0;
        return 0;
    }
    if (!valid_obj(parent) || parent == child) return -1;
    for (int p = parent, n = 0; p >= 0 && n < 8; p = g.parent1[p] - 1, n++)
        if (p == child) return -1;                         // no loops
    g.parent1[child] = (int16_t)(parent + 1);
    g.lpos[child][0] = g.lpos[child][1] = g.lpos[child][2] = 0;
    g.lrot[child][0] = g.lrot[child][1] = g.lrot[child][2] = 0;
    g.any_parent = true;
    return 0;
}
int vx_obj_shadow(int id, int radius, int y, int alpha) {
    if (!g.open || !valid_obj(id)) return -1;
    const int old = g.shadow1[id] - 1;
    if (old >= 0) { vx_obj_free(old); g.shadow1[id] = 0; }
    if (radius <= 0) return -1;
    enum { N = 12 };
    int32_t xyz[(N + 1) * 3];
    uint16_t idx[N * 3];
    xyz[0] = xyz[1] = xyz[2] = 0;
    for (int i = 0; i < N; i++) {
        const float a = i * (6.2831853f / N);
        xyz[(i + 1) * 3] = (int32_t)(std::sin(a) * radius); xyz[(i + 1) * 3 + 1] = 0; xyz[(i + 1) * 3 + 2] = (int32_t)(std::cos(a) * radius);
        idx[i * 3] = 0; idx[i * 3 + 1] = (uint16_t)(1 + i); idx[i * 3 + 2] = (uint16_t)(1 + (i + 1) % N);   // facing up
    }
    const int mat = vx_material(0, VX_UNLIT, clampi(alpha, 1, 255), -1, 0);
    if (mat < 0) return -1;
    const int disc = vx_mesh(xyz, N + 1, idx, N, nullptr, nullptr, mat, 0);
    if (disc < 0) return -1;
    vx_obj_depth(disc, 2, VX_DEPTH_NOWRITE);               // on the ground, never hiding what is on it
    g.shadow1[id] = (int16_t)(disc + 1);
    g.shadow_y[id] = (int16_t)clampi(y, -32000, 32000);
    g.any_shadow = true;
    return disc;
}
int vx_project(int x, int y, int z, int out[3]) {
    if (!g.open || !g.scene || !out) return 0;
    if (!g.proj_ok) return 0;
    const float *M = g.scene->getCameraMatrix();           // world -> camera, as the last frame used
    const float dx = (float)(x - g.proj_pos[0]), dy = (float)(y - g.proj_pos[1]), dz = (float)(z - g.proj_pos[2]);
    const float cx = M[0] * dx + M[1] * dy + M[2] * dz;
    const float cy = M[3] * dx + M[4] * dy + M[5] * dz;
    const float cz = M[6] * dx + M[7] * dy + M[8] * dz;
    if (cz < (float)g.proj_near) return 0;
    const float k = g.proj_f / cz;                          // the engine's own projection (Scene.cpp)
    out[0] = (int)(cx * k) + g.w / 2;
    out[1] = g.h / 2 - (int)(cy * k);
    out[2] = (int)cz;
    return 1;
}
int vx_texture_size(int tex) {
    if (!g.open || tex < 0 || tex >= g.ntex || !g.tex[tex]) return -1;
    return (g.tex[tex]->width << 16) | (g.tex[tex]->height & 0xFFFF);
}
int vx_obj_get_pos(int id, int out[3]) {
    if (!g.open || !valid_obj(id) || !out) return 0;
    const Object *o = g.obj[id];
    out[0] = o->position.x; out[1] = o->position.y; out[2] = o->position.z;
    return 1;
}
void vx_obj_fade(int id, int near, int far) {
    if (!g.open || !valid_obj(id)) return;
    near = clampi(near, 0, 1 << 20); far = clampi(far, 0, 1 << 20);
    g.obj[id]->fadeNear = far > near ? near : 0;
    g.obj[id]->fadeFar = far > near ? far : 0;
}
void vx_obj_appear(int id, int near, int far) {
    if (!g.open || !valid_obj(id)) return;
    near = clampi(near, 0, 1 << 20); far = clampi(far, 0, 1 << 20);
    g.obj[id]->appearNear = far > near ? near : 0;
    g.obj[id]->appearFar = far > near ? far : 0;
}
void vx_emitter_clear(int e) {
    if (!g.open || e < 0 || e >= VX_MAX_EMITTERS || !g.em[e].used) return;
    g.em[e].n = 0;
}
int vx_config(int key, int value) {
    int old;
    switch (key) {
    case VX_CFG_MIP_BIAS:    old = vxMipBias; vxMipBias = clampi(value, 0, 3); return old;
    case VX_CFG_NO_TEXTURES: old = vxNoTextures; vxNoTextures = value != 0; return old;
    case VX_CFG_EAGER_BG:    old = vxEagerBg; vxEagerBg = value != 0; return old;
    case VX_CFG_SPAN_EXP:    old = s_exp_on; s_exp_on = value != 0; if (!s_exp_on) { vxSpanExp = 0; s_exp_solo = false; } return old;
    default:                 return -1;
    }
}

int vx_stat(int what) {
    switch (what) {
    case VX_STAT_US:         return (int)g.us_total;
    case VX_STAT_TRIS:       return g.rasterized;
    case VX_STAT_QUEUED:     return g.queued;
    case VX_STAT_BAND0_US:   return (int)g.us_band[0];
    case VX_STAT_BAND1_US:   return (int)g.us_band[1];
    case VX_STAT_SPLIT:      return g.split;
    case VX_STAT_SCENE_TRIS: return g.tris;
    case VX_STAT_MEM:        return (int)vx_mem_used();
    case VX_STAT_PREP_US:    return (int)g.us_prep;
    case VX_STAT_PARTICLES:  return g.live;
    case VX_STAT_OBJECTS: {
        int n = 0;
        for (int i = 0; i < g.nobj; i++) n += g.obj[i] != nullptr;
        return n;
    }
    default:                 return -1;
    }
}

}  // extern "C"
