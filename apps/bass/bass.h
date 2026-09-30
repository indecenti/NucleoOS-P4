// bass.h — Vertice Bass: arcade lake fishing on the Vertice 3D engine. Shared declarations.
#pragma once
#include "nucleo_sdk.h"
#include "mathx.h"

// Compile-time RGB565 (NV_RGB is a function: not usable in static tables).
#define C565(r, g, b) ((uint16_t)((((r) & 0xF8) << 8) | (((g) & 0xFC) << 3) | ((b) >> 3)))

#define W 512
#define H 300

// ---- mesh builder + helpers (mesh.c) -----------------------------------------------------------------
#define MB_MAXV 400
#define MB_MAXT 260
extern int mb_nv, mb_nt;
void mb_reset(void);
int  mb_v(float x, float y, float z, int u, int v);
void mb_tri(int a, int b, int c, int mat, float ix, float iy, float iz);
void mb_quad(int a, int b, int c, int d, int mat, float ix, float iy, float iz);
void mb_box(float x0, float y0, float z0, float x1, float y1, float z1, int mat);
void mb_box_uv(float x0, float y0, float z0, float x1, float y1, float z1, int mat, float tile);
int  mb_commit(int mat_default, int with_uv);
int  rnd(int n);
void rnd_seed(uint32_t s);
uint16_t rgb(int r, int g, int b);
extern uint16_t tex_buf[4096];
#define KEY 0xF81F

// ---- the lake (lake.c) ---------------------------------------------------------------------------------
// One coordinate system for both views: the boat sits at the origin looking along +Z; above the
// water the surface is y = 0, below it the lake bed is y = 0 and the surface y = SURF.
#define SURF     420.0f
#define NSPOTS   6
enum { SPOT_WEEDS, SPOT_LOG, SPOT_ROCKS, SPOT_PADS };
typedef struct { float x, z, r; int kind; } Spot;
extern Spot g_spot[NSPOTS];

typedef struct {
    const char *name_it, *name_en;
    int   time_s;          // stage clock
    float quota_kg;        // weigh-in target
    uint16_t sky_top, sky_bot, water, deep;   // palettes
    uint32_t sun_rgb, amb_rgb;
    int   sun_el;
    uint8_t mix[8];        // species weights (percent, by SP_* order)
    uint16_t forest, rock; // shoreline forest and mountain tint
} Stage;
#define NSTAGES 6
extern const Stage g_stage[NSTAGES];

void lake_build(int stage, int loop);      // vx_reset + everything for this stage
void lake_view(int under);                // 0: above the water, 1: under it (swaps groups + atmosphere)
extern int g_fx_splash, g_fx_bubble, g_fx_dust, g_fx_spark, g_fx_glint, g_boat;
int  lake_spot_near(float x, float z);    // spot index within its radius (+ margin) or -1
void lake_clear_near(float x, float z, float r);
int  lake_collide(float *x, float *y, float *z, float r);   // push a point out of rocks/logs; 1 if it hit   // hide weeds within r of (x,z) (the camera), show the rest

// ---- fish (fish.c) -----------------------------------------------------------------------------------
enum { SP_BASS, SP_TROUT, SP_PIKE, SP_CATFISH, SP_CARP, SP_PERCH, SP_ZANDER, SP_GOLD, NSPECIES };
typedef struct {
    const char *name_it, *name_en;
    float kg_min, kg_max, depth, speed, power;
    float like[3];        // interest per lure action: steady, stop, twitch
} Species;
extern const Species g_species[NSPECIES];

enum { LURE_CRANK, LURE_POPPER, LURE_WORM, LURE_JIG, NLURES };
extern const char *const g_lure_it[NLURES], *const g_lure_en[NLURES];

void fish_build(void);                    // models (after lake_build)
void fish_spawn(float x, float z, int stage);   // populate around the cast
void fish_hide(void);
typedef struct { int action; float lx, ly, lz; int lure; } LureState;   // action 0 steady 1 stop 2 twitch
// Advance the fish; returns the index of a fish striking the lure this frame, or -1.
int  fish_update(const LureState *l, float dt, int now_ms);
void fish_pose(int i, float x, float y, float z, float yaw, float wiggle, float pitch);
int  fish_nibbling(void);
int  fish_mark(int i, float *x, float *y, float *z);   // 0 none, 1 noticed ("?"), 2 chasing ("!")
int  fish_slots(void);                 // a fish mouthing the lure (before the bite), or -1
void fish_spook(int i);                   // hooked too early: it bolts
int  fish_species(int i);
float fish_kg(int i);
void fish_release_others(int keep);

// ---- the record wall (main.c): the ten biggest fish ever landed, saved on the device ---------------
#define NRECORDS 10
typedef struct { uint16_t kg100; uint8_t species, stage; } Record;
typedef struct {
    int   fish;           // index
    float dist, tension, stamina, run, run_dir, run_t, slack_t, over_t, jump_t;
    float fx, fy, fz;     // fish position (underwater coords)
    int   jumping, jump_ok;
    float surge;          // > 0 right after a sudden hard run (the camera shakes)
} Fight;
extern int g_rod_lift;    // fight: 1 rod held high (pressure), -1 rod dropped (gives line), 0 level
void fight_start(Fight *f, int fish, float lx, float ly, float lz);
// rod: -1 left, 0 centre, 1 right; reel: held. Returns 0 fighting, 1 landed, -1 line snapped,
// -2 hook thrown.
int  fight_update(Fight *f, int rod, int reel, int tap, float dt);
