// bass.h — Vertice Bass: arcade lake fishing on the Vertice 3D engine. Shared declarations.
#pragma once
#include "nucleo_sdk.h"
#include "mathx.h"

// Compile-time RGB565 (NV_RGB is a function: not usable in static tables).
#define C565(r, g, b) ((uint16_t)((((r) & 0xF8) << 8) | (((g) & 0xFC) << 3) | ((b) >> 3)))

#define W 512
#define H 300

// ---- mesh builder + helpers (mesh.c) -----------------------------------------------------------------
#define MB_MAXV 640
#define MB_MAXT 420
extern int mb_nv, mb_nt;
void mb_reset(void);
int  mb_v(float x, float y, float z, int u, int v);
void mb_tri(int a, int b, int c, int mat, float ix, float iy, float iz);
void mb_quad(int a, int b, int c, int d, int mat, float ix, float iy, float iz);
void mb_box(float x0, float y0, float z0, float x1, float y1, float z1, int mat);
void mb_box_uv(float x0, float y0, float z0, float x1, float y1, float z1, int mat, float tile);
void mb_rock(float cx, float cz, float r, float h, int mat, float tile);
void mb_cyl(float ax, float ay, float az, float bx, float by, float bz, float r, int mat, float tile);
void mb_limb(float ax, float ay, float az, float ra, float bx, float by, float bz, float rb, int n,
             int caps, int mat, float tile);
int  mb_commit(int mat_default, int with_uv);
int  mb_commit_ex(int mat_default, int with_uv, int flags);   // flags: VX_MESH_*
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
void lake_view(int under);
extern int g_cam_far;                     // the far plane for this view: past the fog nothing is drawn
float lake_shore(float angle);
void  lake_birds(int now_ms);             // animate the gulls (above water)            // radius of the waterline at that angle (atan2f_(x, z))                // 0: above the water, 1: under it (swaps groups + atmosphere)
extern int g_fx_splash, g_fx_bubble, g_fx_dust, g_fx_spark, g_fx_glint, g_boat, g_boat_trim;
#define ANGLER_SIZE 210                     // the angler billboards (world units, square)
extern int g_angler[4];                     // poses: idle, wind-up, cast, reel (-1 if the art is missing)
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
int  fish_watch(float lx, float ly, float lz, float *x, float *y, float *z);   // the fish to frame in the bite
extern int g_fish_peck;
extern float g_lure_half;                  // half the lure's length: a mouthing fish keeps its nose there                    // a mouthing fish just pecked (main clears it)
int  fish_mark(int i, float *x, float *y, float *z);   // 0 none, 1 noticed ("?"), 2 chasing ("!")
int  fish_slots(void);
extern float g_depth_bias;                 // the lake's conditions: + shallower, - deeper (units)
int  fish_any_interest(void);             // a fish has noticed the lure
float fish_mark_x(int i);                 // where fish i is now (x, z)
void  fish_mouth(int i, float *x, float *y, float *z);   // the hooked fish's mouth (where the line ties)
float fish_mark_z(int i);                 // a fish mouthing the lure (before the bite), or -1
void fish_spook(int i);                   // hooked too early: it bolts
int  fish_species(int i);
float fish_kg(int i);
void fish_release_others(int keep);
void fish_pose_test(int i, float x, float y, float z, float yaw);   // simulator views

// ---- the record wall (main.c): the ten biggest fish ever landed, saved on the device ---------------
#define NRECORDS 10
typedef struct { uint16_t kg100; uint8_t species, stage; } Record;
typedef struct {
    int   fish;           // index
    float dist, tension, stamina, run, run_dir, run_t, slack_t, over_t, jump_t;
    float fx, fy, fz;     // fish position (underwater coords)
    int   jumping, jump_ok;
    float surge;          // > 0 right after a sudden hard run (the camera shakes)
    int   drag;           // the drag is slipping: line going out (not cranking into a hard pull)
    float strain;         // 0..1: builds in the red, drains out of it; 1 = the line snaps
    int   bolts;          // boat-side bolts left: a "beaten" fish near the boat finds its legs again
    int   bolt_now;       // set for one frame: 1 it bolts by the boat, 2 it comes back from tired
    int   winds, tired;   // second winds left; it has been worn out (stamina bottomed) since the last
} Fight;
// A jump lasts JUMP_T s: the fish races up (JUMP_T..JUMP_AIR), is in the air (JUMP_AIR..JUMP_IN),
// and falls back in. fight_update counts jump_t down.
#define JUMP_T   1.6f
#define JUMP_AIR 1.2f
#define JUMP_IN  0.25f
extern int g_rod_lift;    // fight: 1 rod held high (pressure), -1 rod dropped (gives line), 0 level
void fight_start(Fight *f, int fish, float lx, float ly, float lz);
// rod: -1 left, 0 centre, 1 right; reel: held. Returns 0 fighting, 1 landed, -1 line snapped,
// -2 hook thrown.
int  fight_update(Fight *f, int rod, float reel, int tap, float dt);   // reel 0..1 (analog trigger)
