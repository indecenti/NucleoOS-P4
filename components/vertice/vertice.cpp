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
#define VX_LOGI(...) ESP_LOGI(TAG, __VA_ARGS__)
#define VX_LOGW(...) ESP_LOGW(TAG, __VA_ARGS__)
static inline int64_t now_us(void) { return esp_timer_get_time(); }
static void *psram_calloc(size_t n) { return heap_caps_aligned_calloc(64, 1, n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT); }
static void psram_free(void *p) { heap_caps_free(p); }
static size_t psram_largest(void) { return heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM); }
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
extern "C" size_t vx_mem_used(void) { return 0; }   // the PC harness doesn't meter its heap
#endif

using namespace Renderer;

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
    uint16_t *sky = nullptr;           // per-row clear colours (h entries)
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
    if (!g.sky) return;
    const int n = g.h > 1 ? g.h - 1 : 1;
    for (int y = 0; y < g.h; y++) g.sky[y] = lerp565(top, bottom, y * 256 / n);
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
            if (qz < nearz || qz > farz || g.nspr >= kMaxSprites) continue;
            const float qx = M[0] * dx + M[1] * dy + M[2] * dz, qy = M[3] * dx + M[4] * dy + M[5] * dz;
            const float inv = f / qz;
            const int sx = (int)(qx * inv) + g.w / 2, sy = g.h / 2 - (int)(qy * inv);
            const float size = em.s0 + (em.s1 - em.s0) * p.age;
            int r = (int)(size * inv * 0.5f);
            if (r < 1) r = 1;
            if (r > 48) r = 48;
            if (sx + r < 0 || sx - r >= g.w || sy + r < 0 || sy - r >= g.h) continue;
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
// z-buffer, alpha-blended or additive. Each band only writes its own rows -> parallel-safe. The
// disc centre and radius come from the unclamped projection, kept in the sprite's own rect.
void particles_draw(int y0, int y1) {
    for (int i = 0; i < g.nspr; i++) {
        const Sprite &s = g.spr[i];
        if (s.y1 < y0 || s.y0 >= y1) continue;
        const int ya = std::max<int>(s.y0, y0), yb = std::min<int>(s.y1, y1 - 1);
        const int cxp = (s.x0 + s.x1) / 2, cyp = (s.y0 + s.y1) / 2;
        const int r = std::max(1, std::max(s.x1 - s.x0, s.y1 - s.y0) / 2), r2 = r * r;
        const bool ztest = g.depth && !(s.flags & VX_PART_NODEPTH);
        const bool add = (s.flags & VX_PART_ADDITIVE) != 0;
        const int sr = s.color >> 11, sg = (s.color >> 5) & 63, sb = s.color & 31;
        for (int y = ya; y <= yb; y++) {
            uint16_t *row = g.target + (size_t)y * g.w;
            const uint16_t *zr = g.zbuf + (size_t)y * g.w;
            const int dy = y - cyp;
            for (int x = s.x0; x <= s.x1; x++) {
                const int dx = x - cxp, d2 = dx * dx + dy * dy;
                if (d2 > r2) continue;
                if (ztest && s.z >= zr[x]) continue;
                const int a = s.alpha * (r2 - d2) / r2;   // soft edge
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

// ---- band worker -------------------------------------------------------------------------------
// Clear rows [y0,y1) (sky colour per row, depth to "far"), rasterise them, then draw the particles
// touching them. Runs on both cores at once for disjoint bands: Scene::rasterizeBand works on a
// private Rasterizer copy and only writes rows inside its band; `flags` records which queued
// triangles it drew (for the stats).
void band(int y0, int y1, uint8_t *flags, int64_t *us) {
    const int64_t t0 = now_us();
    if (y1 > y0) {
        uint32_t *row32 = (uint32_t *)(g.target + (size_t)y0 * g.w);
        const int pairs = g.w / 2;           // w is even (vx_open)
        for (int y = y0; y < y1; y++, row32 += pairs) {
            const uint32_t c = g.sky[y] | ((uint32_t)g.sky[y] << 16);
            for (int x = 0; x < pairs; x++) row32[x] = c;
        }
        if (g.depth) memset(g.zbuf + (size_t)y0 * g.w, 0xFF, (size_t)(y1 - y0) * g.w * 2);
        g.scene->rasterizeBand(y0, y1, flags);
        if (g.nspr) particles_draw(y0, y1);
    }
    *us = now_us() - t0;
}

#ifdef ESP_PLATFORM
TaskHandle_t      s_helper = nullptr;
SemaphoreHandle_t s_go = nullptr, s_done = nullptr;
volatile int      s_job_y0 = 0, s_job_y1 = 0;

void helper_task(void *) {
    for (;;) {
        xSemaphoreTake(s_go, portMAX_DELAY);
        band(s_job_y0, s_job_y1, g.flags[1], &g.us_band[1]);
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
void run_bands(int split) {
    s_job_y0 = split; s_job_y1 = g.h;
    xSemaphoreGive(s_go);
    band(0, split, g.flags[0], &g.us_band[0]);
    xSemaphoreTake(s_done, portMAX_DELAY);   // bounded: a band is finite work over capped scenes
}
#else
bool helper_start(void) { return true; }
void run_bands(int split) {
    std::thread t([split] { band(split, g.h, g.flags[1], &g.us_band[1]); });
    band(0, split, g.flags[0], &g.us_band[0]);
    t.join();
}
#endif

// The core's frontend hook: prepareFrame() has queued + sorted every visible triangle. Project the
// particles, rasterise the two bands in parallel, then move the cut toward equal time: each band's
// measured cost per row predicts where both sides cost the same; half a step per frame keeps it
// from ringing.
void exec_bands(Scene &sc) {
    particles_update();
    g.queued = sc.lastFrameDrawnTriangles;
    const int need = g.queued > 0 ? g.queued : 1;
    if (need > g.flags_cap) {
        for (int i = 0; i < 2; i++) { psram_free(g.flags[i]); g.flags[i] = (uint8_t *)psram_calloc((size_t)need); }
        g.flags_cap = (g.flags[0] && g.flags[1]) ? need : 0;
    }
    if (!g.flags_cap) {   // no memory for the flags: one band, still correct
        band(0, g.h, nullptr, &g.us_band[0]);
        g.us_band[1] = 0;
        g.rasterized = sc.lastFrameRasterizedTriangles;
        return;
    }
    memset(g.flags[0], 0, (size_t)need);
    memset(g.flags[1], 0, (size_t)need);
    const int split = g.split;
    run_bands(split);
    int n = 0;
    for (int i = 0; i < g.queued; i++) n += (g.flags[0][i] | g.flags[1][i]);
    g.rasterized = n;
    sc.lastFrameRasterizedTriangles = n;

    const int64_t a = g.us_band[0], b = g.us_band[1];
    if (a > 0 && b > 0 && split > 0 && split < g.h) {
        const double ca = (double)a / split, cb = (double)b / (g.h - split);   // µs per row
        const int target = (int)(g.h * cb / (ca + cb));
        g.split = clampi(split + (target - split) / 2, g.h / 8, g.h - g.h / 8);
    }
}

void free_scene_content(void) {
    if (g.scene) g.scene->getObjects().clear();
    for (int i = 0; i < g.nobj; i++) { delete g.obj[i]; g.obj[i] = nullptr; g.obj_v[i] = g.obj_t[i] = 0; }
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
    VX_LOGI("open %dx%d (%u bytes held)", w, h, (unsigned)vx_mem_used());
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
void vx_obj_show(int id, bool on) { if (g.open && valid_obj(id)) g.obj[id]->enabled = on; }
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
}
void vx_ambient(uint32_t rgb888) {
    if (g.open) g.amb->color = Color{(uint8_t)(rgb888 >> 16), (uint8_t)(rgb888 >> 8), (uint8_t)rgb888};
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
    g.scene->render(exec_bands);
    if (g.pick_armed) {
        const PickResult &r = g.scene->getPickResults()[0];
        g.picked = -1;
        if (r.hit)
            for (int i = 0; i < g.nobj; i++) if (g.obj[i] == r.object) { g.picked = i; break; }
        g.scene->setPickQueries(nullptr, 0);
        g.pick_armed = false;
    }
    g.us_total = now_us() - t0;
    // Wall time minus the parallel section (the slower band): cull + transform + sort.
    g.us_prep = g.us_total - (g.us_band[0] > g.us_band[1] ? g.us_band[0] : g.us_band[1]);
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
