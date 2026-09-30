// mathx.h — the little float math the game needs (the freestanding SDK has no libm).
#pragma once

#define PI_F 3.14159265f

static inline float fabsf_(float x) { return x < 0 ? -x : x; }
static inline float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }
static inline float sqrtf_(float x) { return __builtin_sqrtf(x); }   // wasm f32.sqrt

// sin/cos: range-reduced odd/even polynomials, |error| < 2e-4 — plenty for steering and cameras.
static inline float wrap_pi(float a) {
    while (a > PI_F) a -= 2 * PI_F;
    while (a < -PI_F) a += 2 * PI_F;
    return a;
}
static inline float sinf_(float x) {
    x = wrap_pi(x);
    float s = 1.0f;
    if (x < 0) { x = -x; s = -1.0f; }
    if (x > PI_F / 2) x = PI_F - x;               // sin(pi - x) = sin(x)
    const float x2 = x * x;
    return s * x * (1.0f - x2 / 6.0f * (1.0f - x2 / 20.0f * (1.0f - x2 / 42.0f)));
}
static inline float cosf_(float x) { return sinf_(x + PI_F / 2); }

// atan2 with ~0.005 rad error (rational approximation).
static inline float atan2f_(float y, float x) {
    const float ax = fabsf_(x), ay = fabsf_(y);
    if (ax < 1e-9f && ay < 1e-9f) return 0;
    const float a = (ax > ay ? ay / ax : ax / ay);
    const float s = a * a;
    float r = ((-0.0464964749f * s + 0.15931422f) * s - 0.327622764f) * s * a + a;
    if (ay > ax) r = PI_F / 2 - r;
    if (x < 0) r = PI_F - r;
    return y < 0 ? -r : r;
}

static inline int iroundf(float v) { return (int)(v < 0 ? v - 0.5f : v + 0.5f); }
static inline float deg(float rad) { return rad * (180.0f / PI_F); }
