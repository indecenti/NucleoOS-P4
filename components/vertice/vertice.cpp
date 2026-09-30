// vertice.cpp — Vertice, the NucleoOS 3D engine: scene handles, dual-core band renderer, particles,
// .vxm models. See include/vertice.h for the model. Builds for the P4 (FreeRTOS helper task on the
// other core) and for the PC harness (tools/vertice: std::thread helper); nothing else differs.
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
    bool      depth = true;
    uint16_t *target = nullptr;        // this frame's colour buffer
    // particles
    Emitter   em[VX_MAX_EMITTERS];
    Sprite   *spr = nullptr;
    int       nspr = 0, live = 0;
    int64_t   last_us = 0;
    uint32_t  rng = 0x9E3779B9u;
    // picking
    bool      pick_armed = false;
    PickQuery pick_q;
    int       picked = -1;
    // band split + stats
    int       split = 0;
    int64_t   us_total = 0, us_prep = 0, us_band[2] = {0, 0};
    int       rasterized = 0, queued = 0;
    uint8_t  *flags[2] = {nullptr, nullptr};
    int       flags_cap = 0;
    // tiled raster: per core one tile of colour + depth rows in internal SRAM (0 tiles = bands)
    int       tile_h = 0, ntiles = 0;
    uint16_t *tcol[2] = {nullptr, nullptr}, *tz[2] = {nullptr, nullptr};
    uint16_t *tile_block = nullptr;              // the one SRAM allocation they are carved from
    uint16_t *bin_list = nullptr;
    int       bin_cap = 0;
    uint32_t  bin_start[600 / 4 + 2] = {};   // tiles are >= 4 rows, the canvas <= 600
    // background: Mode-7 floor and 360° panorama (see clear_rows)
    bool      floor_on = false, floor_dirty = false;
    int       floor_y = 0, floor_repeat = 0, floor_w = 0, floor_h = 0, floor_wshift = 0;
    uint16_t  floor_color = 0, floor_avg = 0;
    const uint16_t *floor_src = nullptr;     // the texture's pixels (owned by its Texture)
    uint16_t *floor_lit = nullptr;           // lit copy (SRAM when available)
    const uint16_t *pano_px = nullptr;       // panorama pixels (owned by its Texture)
    int       pano_w = 0, pano_h = 0, pano_hrow = 0;
    int16_t  *pano_u = nullptr;              // per-column texel, this frame
    int       water_k = 0, water_wave = 0;   // vx_water: reflection strength (0..256), ripple px
    // level of detail: a master object may name up to VX_MAX_LODS simpler stand-ins, shown instead
    // of it beyond a camera distance (see apply_lods)
    struct Lod { int16_t id[VX_MAX_LODS]; int32_t dist[VX_MAX_LODS]; uint8_t n, cur; bool shown; };
    Lod       lod[VX_MAX_OBJECTS] = {};
    int16_t   lod_of[VX_MAX_OBJECTS] = {};    // a stand-in's master + 1 (0 = not a stand-in)
    bool      any_lod = false;
};
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
    const int64_t area_max = (int64_t)g.w * g.h / 2;
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
        const float k = g.pano_w / (2.0f * 3.14159265f);
        for (int x = 0; x < g.w; x++) {
            const float a = yaw + std::atan((x - g.w * 0.5f) / bgf.f);
            int u = (int)std::floor(a * k);
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
        g.floor_lit[i] = (uint16_t)((r << 11) | (gg << 5) | b);
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
static void water_row(uint16_t *row, int y) {
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
            for (int x = 0; x < g.w; x++) {
                int xs = x + off;
                xs = xs < 0 ? 0 : xs > w1 ? w1 : xs;
                const uint16_t c = prow[g.pano_u[xs]];
                row[x] = lerp565(row[x], c == 0xF81F ? sky : c, k);
            }
            return;
        }
    }
    for (int x = 0; x < g.w; x++) row[x] = lerp565(row[x], sky, k);
}

// Clear rows [y0,y1) of a virtual row base: panorama / sky gradient above the horizon, the Mode-7
// floor below it, depth to "far".
void clear_rows(int y0, int y1, uint16_t *col, uint16_t *zb) {
    const int pairs = g.w / 2;           // w is even (vx_open)
    for (int y = y0; y < y1; y++) {
        uint16_t *row = col + (size_t)y * g.w;
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
                    const uint32_t c2 = c | ((uint32_t)c << 16);
                    uint32_t *row32 = (uint32_t *)row;
                    for (int x = 0; x < pairs; x++) row32[x] = c2;
                    water_row(row, y);
                    continue;
                }
                // Texels in Q16; the texture is power-of-two so wrapping is a mask.
                const float tk = (float)g.floor_w / g.floor_repeat, tkv = (float)g.floor_h / g.floor_repeat;
                int32_t u = (int32_t)(px * tk * 65536.0f), v = (int32_t)(pz * tkv * 65536.0f);
                const int32_t du = (int32_t)(sx * tk * 65536.0f), dv = (int32_t)(sz * tkv * 65536.0f);
                const unsigned wm = g.floor_w - 1, hm = g.floor_h - 1, sh = g.floor_wshift;
                const uint16_t *tex = g.floor_lit;
                if (lodA == 0 && fogA == 0) {
                    for (int x = 0; x < g.w; x++, u += du, v += dv)
                        row[x] = tex[((((uint32_t)v >> 16) & hm) << sh) | (((uint32_t)u >> 16) & wm)];
                } else {
                    const uint16_t far = lerp565(g.floor_avg, sky, fogA);    // where the blend goes
                    const int a = 256 - (256 - lodA) * (256 - fogA) / 256;   // combined weight
                    for (int x = 0; x < g.w; x++, u += du, v += dv)
                        row[x] = lerp565(tex[((((uint32_t)v >> 16) & hm) << sh) | (((uint32_t)u >> 16) & wm)], far, a);
                }
                water_row(row, y);
                continue;
            }
        } else if (floor_row) {
            // Rolled camera: exact per-pixel ray/plane intersection (a division per pixel).
            const float qy = (g.h * 0.5f - y) / bgf.f;
            for (int x = 0; x < g.w; x++) {
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
            continue;
        }
        // Sky: the panorama (keyed texels show the gradient), or the plain gradient.
        if (g.pano_px) {
            const int v = (int)std::floor(bgf.pano_v0 + y * bgf.pano_dv);
            if (v >= 0 && v < g.pano_h) {
                const uint16_t *prow = g.pano_px + (size_t)v * g.pano_w;
                for (int x = 0; x < g.w; x++) {
                    const uint16_t c = prow[g.pano_u[x]];
                    row[x] = c == 0xF81F ? sky : c;
                }
                continue;
            }
        }
        const uint32_t c = sky | ((uint32_t)sky << 16);
        uint32_t *row32 = (uint32_t *)row;
        for (int x = 0; x < pairs; x++) row32[x] = c;
    }
    if (g.depth) memset(zb + (size_t)y0 * g.w, 0xFF, (size_t)(y1 - y0) * g.w * 2);
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
std::atomic<int> g_next_tile{0};

void tile_worker(int core) {
    const int64_t t0 = now_us();
    BandProf &pf = g_prof[core];
#if defined(ESP_PLATFORM) && CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS
    TaskStatus_t ts0;
    vTaskGetInfo(nullptr, &ts0, pdFALSE, eRunning);
#endif
    uint16_t *col = g.tcol[core], *zb = g.tz[core];
    uint8_t *flags = g.flags[core];
    for (;;) {
        const int t = g_next_tile.fetch_add(1, std::memory_order_relaxed);
        if (t >= g.ntiles) break;
        const int y0 = t * g.tile_h, y1 = std::min(y0 + g.tile_h, g.h);
        uint16_t *cb = col - (ptrdiff_t)y0 * g.w, *zbb = zb - (ptrdiff_t)y0 * g.w;   // virtual bases
        const int64_t a = now_us();
        clear_rows(y0, y1, cb, zbb);
        const int64_t b = now_us();
        const uint32_t s = g.bin_start[t], e = g.bin_start[t + 1];
        if (e > s) g.scene->vxRasterTile(y0, y1, g.bin_list + s, (int)(e - s), flags, cb, zbb, pf.stat);
        const int64_t c = now_us();
        if (g.nspr) particles_draw(y0, y1, cb, zbb);
        const int64_t d = now_us();
        const size_t bytes = (size_t)(y1 - y0) * g.w * 2;
        uint16_t *dst = g.target + (size_t)y0 * g.w;
#ifdef ESP_PLATFORM
        if (nv_2d_copy(dst, col, bytes, 50) != ESP_OK) memcpy(dst, col, bytes);   // unaligned canvas
#else
        memcpy(dst, col, bytes);
#endif
        pf.clear += b - a; pf.raster += c - b; pf.parts += d - c; pf.copy += now_us() - d; pf.tiles++;
    }
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
    }
    *us = now_us() - t0;
    pf.clear += t1 - t0; pf.raster += t2 - t1; pf.parts += now_us() - t2;
}

// What the two cores do for one frame: the helper runs job(1) while the caller runs job(0).
void job(int core) {
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

// The core's frontend hook: prepareFrame() has queued + sorted every visible triangle. Project the
// particles, bin the queue into row tiles (tile mode), rasterise on both cores, count what was
// drawn. Band mode also moves the cut toward equal time: each band's measured cost per row
// predicts where both sides cost the same; half a step per frame keeps it from ringing.
void exec_bands(Scene &sc) {
    particles_update();
    if (g.floor_dirty) floor_relight();
    bg_frame_setup();
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
        for (int tries = 0; tries < 3 && got < 0; tries++) {
            if (!g.bin_list || g.bin_cap < need * 2 + 64 || tries) {
                const int cap = std::max(need * 3 + 64, g.bin_cap * 2);
                psram_free(g.bin_list);
                g.bin_list = (uint16_t *)psram_calloc((size_t)cap * sizeof(uint16_t));
                g.bin_cap = g.bin_list ? cap : 0;
            }
            if (g.bin_list) got = sc.vxBin(g.tile_h, g.ntiles, g.bin_list, g.bin_cap, g.bin_start);
        }
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

void free_scene_content(void) {
    if (g.scene) g.scene->getObjects().clear();
    g.floor_on = false; g.floor_src = nullptr; g.pano_px = nullptr;   // their textures go below
    if (g.floor_lit) { sram_free(g.floor_lit); g.floor_lit = nullptr; }
    for (int i = 0; i < g.nobj; i++) { delete g.obj[i]; g.obj[i] = nullptr; g.obj_v[i] = g.obj_t[i] = 0; }
    for (auto &L : g.lod) L = Engine::Lod{};
    for (auto &m : g.lod_of) m = 0;
    g.any_lod = false;
    for (int i = 0; i < g.nmat; i++) { delete g.mat[i]; g.mat[i] = nullptr; }
    for (int i = 0; i < g.ntex; i++) { delete g.tex[i]; g.tex[i] = nullptr; psram_free(g.texpx[i]); g.texpx[i] = nullptr; }
    for (int e = 0; e < VX_MAX_EMITTERS; e++) { psram_free(g.em[e].p); g.em[e] = Emitter(); }
    g.nobj = g.nmat = g.ntex = 0;
    g.tris = g.verts = 0;
    g.nspr = g.live = 0;
    g.pick_armed = false;
    g.picked = -1;
}

inline bool pow2_side(int v) { return v >= 8 && v <= VX_MAX_TEX_SIDE && (v & (v - 1)) == 0; }

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
    for (int th : {12, 8, 4}) {
        const size_t one = (size_t)th * w * 2;
        if (one * 4 > 48 * 1024 && th > 4) continue;
        g.tile_block = (uint16_t *)sram_alloc(one * 4);
        if (!g.tile_block) continue;
        for (int c = 0; c < 2; c++) {
            g.tcol[c] = g.tile_block + (size_t)(c * 2) * (one / 2);
            g.tz[c]   = g.tile_block + (size_t)(c * 2 + 1) * (one / 2);
        }
        g.tile_h = th;
        g.ntiles = (h + th - 1) / th;
        break;
    }
    if (!g.zbuf || !g.sky || !g.spr) { vx_close(); return false; }
    g.scene = new Scene(nullptr, g.zbuf, w, h);
    g.scene->setClearBuffer(false);            // the bands clear their own rows, in parallel
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
    g.open = true;
    VX_LOGI("open %dx%d, %s (%u bytes held)", w, h, g.ntiles ? (g.tile_h == 8 ? "SRAM tiles x8" : "SRAM tiles x4")
                                                  : "PSRAM bands", (unsigned)vx_mem_used());
    return true;
}

void vx_close(void) {
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
    sram_free(g.tile_block); g.tile_block = nullptr;
    for (int c = 0; c < 2; c++) g.tcol[c] = g.tz[c] = nullptr;
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
    auto &v = g.scene->getObjects();
    v.erase(std::remove(v.begin(), v.end(), o), v.end());
    delete o;
    g.obj[id] = nullptr;
    g.verts -= g.obj_v[id];
    g.tris -= g.obj_t[id];
    g.obj_v[id] = g.obj_t[id] = 0;
    if (g.picked == id) g.picked = -1;
}

void vx_obj_pos(int id, int x, int y, int z) { if (g.open && valid_obj(id)) g.obj[id]->setPosition(x, y, z); }
void vx_obj_rot(int id, int rx, int ry, int rz) {
    if (g.open && valid_obj(id)) g.obj[id]->setRotation(wrap360(rx), wrap360(ry), wrap360(rz));
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
        g.floor_dirty = true;
    }
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
    g.pano_hrow = clampi(horizon_row, 0, t->height);
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

int vx_render(uint16_t *target) {
    if (!g.open || !target) return -1;
    const int64_t t0 = now_us();
    g.target = target;
    g.scene->setFramebuffer(target);
    if (g.pick_armed) g.scene->setPickQueries(&g.pick_q, 1);
    apply_lods();
    g.scene->render(exec_bands);
    restore_lods();
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
