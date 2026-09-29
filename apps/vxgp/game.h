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
// Axis-aligned box [x0,x1]x[y0,y1]x[z0,z1] (optionally narrower on top: shrink per side). The
// bottom face is left out when the box stands on the ground (y0 == 0).
void mb_box(float x0, float y0, float z0, float x1, float y1, float z1, float top_shrink_x,
            float top_shrink_z0, float top_shrink_z1, int mat);
extern int mb_no_bottom;   // 1: mb_box never emits bottom faces (models only ever seen from above)
int  mb_commit(int mat_default, int with_uv);                     // -> vx_mesh handle
int  rnd(int n);

// ---- track -----------------------------------------------------------------------------------------
#define TRACK_N   160          // centreline samples (closed loop)
#define ROAD_HW   215.0f       // road half width (world units)
#define LIMIT_HW  (ROAD_HW + 520.0f)   // soft wall: how far off the road a kart may go
typedef struct { float x, z, tx, tz, s; } TrackPt;   // position, unit tangent, arc length at sample
extern TrackPt g_trk[TRACK_N];
extern float   g_trk_len;
void world_build(void);                                  // floor, sky, road, scenery (once)
// Nearest sample to (x,z), searching around `hint` (±range); returns index.
int  track_nearest(float x, float z, int hint, int range);
// Along-track position and signed lateral offset (+ = right) of (x,z) relative to sample i.
void track_frame(int i, float x, float z, float *s, float *lat);
void track_point(float s, float lat, float *x, float *z, float *heading);   // inverse (s wraps)
float track_curvature(float s);                          // rad per unit length around s
// How much the track bends in the next ~5 samples after sample i (|heading change|, radians):
// precomputed once, what the AI brakes for.
extern float g_trk_bend[TRACK_N];

// ---- pickups: boost pads (background decals) and coins (spinning impostors) --------------------------
#define NPADS  4
#define NCOINS 15
typedef struct { float x, z, s, lat; int obj; int respawn_ms; } Pickup;
extern Pickup g_pad[NPADS];
extern Pickup g_coin[NCOINS];

// ---- solid scenery: karts bounce off tree trunks, gantry pillars and the grandstand ------------------
#define MAX_SOLIDS 96
typedef struct { float x0, z0, x1, z1, r; } Solid;       // r > 0: circle at (x0,z0); else box x0..x1, z0..z1
extern Solid g_solid[MAX_SOLIDS];
extern int   g_nsolid;

// ---- karts -----------------------------------------------------------------------------------------
#define NCARS 4
#define LAPS  3
typedef struct {
    float x, z, heading, v, steer;   // heading: radians, 0 = +Z, pi/2 = +X; v < 0 = reversing
    float s, lat, total;             // along-track (wrapped), lateral offset, unwrapped progress
    int   seg, lap;                  // nearest sample, completed laps (-1 on the grid)
    int   obj, shadow, mat_body, mat_tail;
    int   ai, finished, coins;
    float lane, skill, finish_ms, lap_start_ms, best_lap_ms, last_lap_ms;
    float bump_cool, boost_t, drift_t, stuck_t, wrong_t;
    float fx, fz;                    // forward unit vector (sin/cos of heading), this frame
    float draft_t;                   // time spent in another kart's slipstream
    float bvx, bvz;                  // shove velocity from hits (decays), added to the driven one
    float spin;                      // yaw rate from a hit (rad/s, decays)
    int   drift_dir;
} Car;
extern Car g_car[NCARS];
void cars_build(void);
void cars_grid(void);                                    // line up on the grid
typedef struct { int left, right, gas, brake; } Input;
// Returns event bits for the HUD/sounds: 1 lap done, 2 coin, 4 boost, 8 bump, 16 respawn,
// 32 slipstream boost.
int  cars_update(const Input *in, float dt, int racing, int now_ms);
// The lights go green. gas_ms = how long the player has been holding the throttle (0 = not):
// a short hold is a rocket start, a long one floods the engine. Returns 1 rocket, -1 flooded, 0.
int  cars_launch(int gas_ms);
int  car_position(int i);                                 // 1-based race position
float car_speed_kmh(int i);
extern const char *g_msg;                                 // transient HUD message ("" = none)
extern int g_msg_until;

// ---- effects ---------------------------------------------------------------------------------------
extern int g_fx_dust, g_fx_smoke, g_fx_spark, g_fx_confetti, g_fx_boost, g_fx_drift;

// ---- tiny text/format helpers (main.c) ---------------------------------------------------------------
int  fmt_int(char *out, int v);
void fmt_time(char *out, int ms);                         // m:ss.cc
