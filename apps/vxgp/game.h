// game.h — Vertice GP: shared declarations (world.c, cars.c, main.c).
#pragma once
#include "nucleo_sdk.h"
#include "mathx.h"

// ---- mesh builder (one scratch buffer, handed to vx_mesh and reused) -------------------------------
#define MB_MAXV 660
#define MB_MAXT 340
void mb_reset(void);
extern int mb_nv, mb_nt;
int  mb_v(float x, float y, float z, int u, int v);              // -> vertex index
// A face (tri or quad a,b,c[,d] around its edge) oriented to face AWAY from the inside point
// (ix,iy,iz): the engine draws triangles whose (b-a)x(c-a) points at the viewer.
void mb_tri(int a, int b, int c, int mat, float ix, float iy, float iz);
void mb_quad(int a, int b, int c, int d, int mat, float ix, float iy, float iz);
// Axis-aligned box [x0,x1]x[y0,y1]x[z0,z1] (optionally narrower on top: shrink per side).
void mb_box(float x0, float y0, float z0, float x1, float y1, float z1, float top_shrink_x,
            float top_shrink_z0, float top_shrink_z1, int mat);
int  mb_commit(int mat_default, int with_uv);                     // -> vx_mesh handle

// ---- track -----------------------------------------------------------------------------------------
#define TRACK_N   160          // centreline samples (closed loop)
#define ROAD_HW   170.0f       // road half width (world units)
typedef struct { float x, z, tx, tz, s; } TrackPt;   // position, unit tangent, arc length at sample
extern TrackPt g_trk[TRACK_N];
extern float   g_trk_len;
void world_build(void);                                  // ground, road, scenery (once)
// Nearest sample to (x,z), searching around `hint` (±range); returns index.
int  track_nearest(float x, float z, int hint, int range);
// Along-track position and signed lateral offset (+ = right) of (x,z) relative to sample i.
void track_frame(int i, float x, float z, float *s, float *lat);
void track_point(float s, float lat, float *x, float *z, float *heading);   // inverse (s wraps)

// ---- cars ------------------------------------------------------------------------------------------
#define NCARS 4
typedef struct {
    float x, z, heading, v, steer;   // heading: radians, 0 = +Z, pi/2 = +X
    float s, lat, total;             // along-track (wrapped), lateral offset, unwrapped progress
    int   seg, lap;                  // nearest sample, completed laps (-1 on the grid)
    int   obj, mat_body, mat_tail;
    int   ai, finished;
    float lane, skill, finish_ms, lap_start_ms, best_lap_ms, last_lap_ms;
    float bump_cool;
} Car;
extern Car g_car[NCARS];
void cars_build(void);
void cars_grid(void);                                    // line up on the grid
typedef struct { int left, right, gas, brake; } Input;
void cars_update(const Input *in, float dt, int racing, int now_ms);
int  car_position(int i);                                 // 1-based race position
float car_speed_kmh(int i);

// ---- effects ---------------------------------------------------------------------------------------
extern int g_fx_dust, g_fx_smoke, g_fx_spark, g_fx_confetti;

// ---- tiny text/format helpers (main.c) ---------------------------------------------------------------
int  fmt_int(char *out, int v);
void fmt_time(char *out, int ms);                         // m:ss.cc
