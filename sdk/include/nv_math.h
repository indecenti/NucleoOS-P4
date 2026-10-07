// nv_math.h — small float math for NucleoOS apps and games (header-only).
//
// The freestanding WASM SDK has no libm: these are fast approximations good for games (cameras,
// steering, animation), plus a 3D vector, smoothing and a seeded random generator. Every game that
// used the Vertice engine had grown its own copy (Vertice Bass mathx.h, Vertice GP): this is the
// shared one. Names carry an nv_ prefix so they never clash with a game's own helpers.
//
//   #include "nv_math.h"
//   NvVec3 d = nv_v3_norm(nv_v3_sub(target, eye));
//   float yaw = nv_atan2f(d.x, d.z);
#pragma once
#include <stdint.h>

#define NV_PI 3.14159265f

// ---- scalars --------------------------------------------------------------------------------------
static inline float nv_absf(float x) { return x < 0 ? -x : x; }
static inline float nv_minf(float a, float b) { return a < b ? a : b; }
static inline float nv_maxf(float a, float b) { return a > b ? a : b; }
static inline float nv_clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }
static inline int   nv_clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }
static inline float nv_sqrtf(float x) { return x > 0 ? __builtin_sqrtf(x) : 0.0f; }   // wasm f32.sqrt
static inline int   nv_roundi(float v) { return (int)(v < 0 ? v - 0.5f : v + 0.5f); }
static inline float nv_lerpf(float a, float b, float t) { return a + (b - a) * t; }
static inline float nv_deg(float rad) { return rad * (180.0f / NV_PI); }
static inline float nv_rad(float deg) { return deg * (NV_PI / 180.0f); }

// Angle into (-pi, pi].
static inline float nv_wrap_pi(float a) {
    while (a > NV_PI) a -= 2 * NV_PI;
    while (a <= -NV_PI) a += 2 * NV_PI;
    return a;
}
// Shortest signed difference b - a between two angles.
static inline float nv_angle_diff(float a, float b) { return nv_wrap_pi(b - a); }

// sin/cos: range-reduced polynomial, |error| < 2e-4.
static inline float nv_sinf(float x) {
    x = nv_wrap_pi(x);
    float s = 1.0f;
    if (x < 0) { x = -x; s = -1.0f; }
    if (x > NV_PI / 2) x = NV_PI - x;
    const float x2 = x * x;
    return s * x * (1.0f - x2 / 6.0f * (1.0f - x2 / 20.0f * (1.0f - x2 / 42.0f)));
}
static inline float nv_cosf(float x) { return nv_sinf(x + NV_PI / 2); }

// atan2 with ~0.005 rad error.
static inline float nv_atan2f(float y, float x) {
    const float ax = nv_absf(x), ay = nv_absf(y);
    if (ax < 1e-9f && ay < 1e-9f) return 0;
    const float a = ax > ay ? ay / ax : ax / ay, s = a * a;
    float r = ((-0.0464964749f * s + 0.15931422f) * s - 0.327622764f) * s * a + a;
    if (ay > ax) r = NV_PI / 2 - r;
    if (x < 0) r = NV_PI - r;
    return y < 0 ? -r : r;
}

// Frame-rate independent smoothing: move `cur` toward `target`, covering `rate` of the way per
// second (exponential). nv_smooth(x, goal, 8, dt) settles in about half a second.
static inline float nv_smooth(float cur, float target, float rate, float dt) {
    float k = rate * dt;                       // 1 - e^-k, accurate for small k
    k = k > 4 ? 1.0f : k * (1.0f - k * 0.5f + k * k * (1.0f / 6.0f));
    return cur + (target - cur) * nv_clampf(k, 0, 1);
}
// Move toward target by at most `step`.
static inline float nv_approach(float cur, float target, float step) {
    return cur < target ? nv_minf(cur + step, target) : nv_maxf(cur - step, target);
}

// ---- 3D vectors ----------------------------------------------------------------------------------
typedef struct { float x, y, z; } NvVec3;
static inline NvVec3 nv_v3(float x, float y, float z) { NvVec3 v = { x, y, z }; return v; }
static inline NvVec3 nv_v3_add(NvVec3 a, NvVec3 b) { return nv_v3(a.x + b.x, a.y + b.y, a.z + b.z); }
static inline NvVec3 nv_v3_sub(NvVec3 a, NvVec3 b) { return nv_v3(a.x - b.x, a.y - b.y, a.z - b.z); }
static inline NvVec3 nv_v3_scale(NvVec3 a, float k) { return nv_v3(a.x * k, a.y * k, a.z * k); }
static inline float  nv_v3_dot(NvVec3 a, NvVec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
static inline NvVec3 nv_v3_cross(NvVec3 a, NvVec3 b) {
    return nv_v3(a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x);
}
static inline float  nv_v3_len(NvVec3 a) { return nv_sqrtf(nv_v3_dot(a, a)); }
static inline NvVec3 nv_v3_norm(NvVec3 a) { const float l = nv_v3_len(a); return l > 1e-9f ? nv_v3_scale(a, 1.0f / l) : a; }
static inline NvVec3 nv_v3_lerp(NvVec3 a, NvVec3 b, float t) {
    return nv_v3(nv_lerpf(a.x, b.x, t), nv_lerpf(a.y, b.y, t), nv_lerpf(a.z, b.z, t));
}
// Unit direction for a heading (yaw, around +Y, 0 = +Z) and pitch (up positive), radians.
static inline NvVec3 nv_v3_dir(float yaw, float pitch) {
    const float c = nv_cosf(pitch);
    return nv_v3(nv_sinf(yaw) * c, nv_sinf(pitch), nv_cosf(yaw) * c);
}
// Rotate a vector around +Y by yaw (radians), the way a vehicle or a character turns.
static inline NvVec3 nv_v3_rot_y(NvVec3 a, float yaw) {
    const float s = nv_sinf(yaw), c = nv_cosf(yaw);
    return nv_v3(a.x * c + a.z * s, a.y, -a.x * s + a.z * c);
}

// ---- random --------------------------------------------------------------------------------------
// xorshift32: deterministic for a given seed (same seed = same level), fast, not cryptographic.
typedef struct { uint32_t s; } NvRand;
static inline void nv_rand_seed(NvRand *r, uint32_t seed) { r->s = seed ? seed : 0x2545F491u; }
static inline uint32_t nv_rand_u32(NvRand *r) {
    uint32_t x = r->s; x ^= x << 13; x ^= x >> 17; x ^= x << 5; return r->s = x;
}
static inline int   nv_rand_int(NvRand *r, int n) { return n > 0 ? (int)(nv_rand_u32(r) % (uint32_t)n) : 0; }   // 0..n-1
static inline float nv_rand_float(NvRand *r) { return (nv_rand_u32(r) >> 8) * (1.0f / 16777216.0f); }         // 0..1
static inline float nv_rand_range(NvRand *r, float a, float b) { return a + (b - a) * nv_rand_float(r); }

// ---- colour --------------------------------------------------------------------------------------
// RGB565 helpers: mix two colours (t 0..256), and split/join channels.
static inline uint16_t nv_rgb565(int r, int g, int b) {
    return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | ((b & 0xF8) >> 3));
}
static inline uint16_t nv_mix565(uint16_t a, uint16_t b, int t) {
    const int ar = a >> 11, ag = (a >> 5) & 63, ab = a & 31, br = b >> 11, bg = (b >> 5) & 63, bb = b & 31;
    return (uint16_t)(((ar + ((br - ar) * t >> 8)) << 11) | ((ag + ((bg - ag) * t >> 8)) << 5) | (ab + ((bb - ab) * t >> 8)));
}
