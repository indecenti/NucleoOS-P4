// Vertice — the NucleoOS 3D engine (ESP32-P4, both cores). Rasteriser core derived from Jet by
// CubeCoders (MIT, core/LICENSE-Jet); scene engine, dual-core renderer, particles, models and the
// WASM-facing API are NucleoOS.
//
// One scene at a time, owned by the running game: nv_wasm binds it to the app's canvas on first use
// and closes it when the run ends; WASM apps reach it through the ABI v9 "nv.vx_*" imports. The
// scene renders into an RGB565 target of the size given to vx_open — normally the app's small
// canvas, which the OS then PPA-scales to the whole panel ("canvas_scale" in the manifest).
//
// Performance model:
//  - prepare (cull, transform, depth sort) runs once per frame on the calling thread;
//  - the frame is cut into two row bands; the caller rasterises the top one while a helper task
//    does the bottom one on the other core. Each band clears its own rows (sky gradient + depth),
//    rasterises, then draws the particles that touch it — all of it in parallel. The cut row moves
//    every frame toward equal time on both cores, from each band's measured per-row cost;
//  - every allocation (core included) comes from PSRAM under a byte budget: the engine never
//    touches the internal SRAM Wi-Fi and DMA depend on.
//
// Units: world coordinates are integers (Y up), angles integer degrees, colours RGB565 unless named
// rgb888, times milliseconds. All calls come from ONE thread (the app's worker). Every call
// validates its arguments (they come from untrusted WASM code) and returns -1 / does nothing on bad
// input. Handles are small integers, valid until vx_reset/vx_close (or vx_obj_free).
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Engine version, exposed to apps as the system component "vertice" (manifest "requires").
#define VX_VERSION "1.0.0"

// Hard caps: a frame's cost and memory stay bounded whatever the app asks for.
#define VX_MAX_OBJECTS    256
#define VX_MAX_MATERIALS  96
#define VX_MAX_TEXTURES   32
#define VX_MAX_TRIANGLES  24000   // whole scene
#define VX_MAX_VERTICES   32000   // whole scene
#define VX_MAX_TEX_SIDE   512     // textures: power of two, 8..512 per side (panoramas)
#define VX_MAX_EMITTERS   8
#define VX_MAX_PARTICLES  512     // per emitter
#define VX_MEM_BUDGET     (12u * 1024u * 1024u)   // PSRAM bytes the engine may hold

// Shading.
enum { VX_FLAT = 0, VX_GOURAUD = 1, VX_PHONG = 2, VX_WIRE = 3, VX_UNLIT = 4, VX_ADDITIVE = 5 };

// Primitive kinds for vx_prim(kind, a, b, c, mat, mat2):
enum {
    VX_CUBE      = 0,   // a,b,c = width, height, depth
    VX_SPHERE    = 1,   // a = radius, b = segments (3..48)
    VX_CYLINDER  = 2,   // a = radius, b = height, c = segments (3..48); closed caps
    VX_CAPSULE   = 3,   // a = radius, b = total height, c = segments (3..48)
    VX_PYRAMID   = 4,   // a = base size, b = height
    VX_PLANE     = 5,   // a = width (X), b = depth (Z)
    VX_GRID      = 6,   // a = width, b = depth, c = cells per side (1..64); mat / mat2 checkerboard
    VX_QUAD      = 7,   // a = width, b = height (XY plane)
    VX_BILLBOARD = 8,   // a = width, b = height, always faces the camera
};

#define VX_TEX_KEY       1   // texture: RGB565 0xF81F (magenta) is transparent
#define VX_TEX_CLAMP     2   // texture: clamp UVs instead of wrapping
#define VX_MESH_SMOOTH   1   // mesh/model: average face normals per vertex (else faceted)
#define VX_PART_ADDITIVE 1   // emitter: add light (sparks, fire) instead of alpha-blending (smoke)
#define VX_PART_NODEPTH  2   // emitter: ignore the depth buffer (always on top)
#define VX_DEPTH_NOTEST  1   // object: always passes the depth test (overlays)
#define VX_DEPTH_NOWRITE 2   // object: does not write depth (decals, glass)

// vx_stat(what)
enum { VX_STAT_US = 0,        // last render, µs (prepare + parallel bands)
       VX_STAT_TRIS = 1,      // triangles rasterised last frame
       VX_STAT_QUEUED = 2,    // triangles that survived culling
       VX_STAT_BAND0_US = 3, VX_STAT_BAND1_US = 4,
       VX_STAT_SPLIT = 5,     // current band cut row
       VX_STAT_SCENE_TRIS = 6,
       VX_STAT_MEM = 7,       // PSRAM bytes held
       VX_STAT_PREP_US = 8,   // cull + transform + sort
       VX_STAT_PARTICLES = 9, // live particles
       VX_STAT_OBJECTS = 10 };

// ---- lifecycle ---------------------------------------------------------------------------------
bool vx_open(int w, int h);   // bind a w×h target (w even): z-buffer, scene, camera, default lights
void vx_close(void);          // free everything
bool vx_is_open(void);
void vx_reset(void);          // drop objects/materials/textures/emitters; camera, lights, sky kept

// ---- resources ---------------------------------------------------------------------------------
int  vx_texture(const uint16_t *px, int w, int h, int flags);                 // copied
// A blank texture filled with color565, then written a rectangle at a time (large textures from
// small app buffers; dynamic textures). Writes outside the texture are ignored.
int  vx_texture_new(int w, int h, uint32_t color565, int flags);
void vx_texture_write(int tex, int x, int y, int w, int h, const uint16_t *px);
int  vx_material(uint32_t color565, int shading, int alpha, int tex, int specular);
void vx_mat_color(int mat, uint32_t color565);                               // e.g. brake lights

// ---- objects -----------------------------------------------------------------------------------
int  vx_prim(int kind, int a, int b, int c, int mat, int mat2);
// xyz: nverts×3 int32; idx: ntris×3 uint16 (< nverts); uv: nverts×2 int16 (1024 = one texture
// repeat) or NULL; tri_mat: ntris material handles or NULL (then `mat` everywhere).
int  vx_mesh(const int32_t *xyz, int nverts, const uint16_t *idx, int ntris, const int16_t *uv,
             const uint8_t *tri_mat, int mat, int flags);
// Parse a .vxm model (tools/vertice/obj2vxm.py; format in vertice.cpp). Creates its materials.
int  vx_model(const uint8_t *data, size_t len, int flags);
int  vx_clone(int id);                 // independent copy (own transform), same look
void vx_obj_free(int id);
void vx_obj_pos(int id, int x, int y, int z);
void vx_obj_rot(int id, int rx, int ry, int rz);
void vx_obj_show(int id, bool on);
// Depth behaviour: bias pulls the surface toward the camera by up to 127 world units (road
// markings over the road without z-fighting); flags VX_DEPTH_*.
void vx_obj_depth(int id, int bias, int flags);

// ---- camera, light, atmosphere ------------------------------------------------------------------
void vx_camera(int x, int y, int z, int rx, int ry, int rz);
void vx_look_at(int x, int y, int z);
void vx_lens(int fov_deg, int znear, int zfar);
void vx_sun(int azimuth, int elevation, uint32_t rgb888, int intensity);
void vx_ambient(uint32_t rgb888);
void vx_sky(uint16_t top, uint16_t bottom);   // sky gradient: zenith colour -> horizon haze (fog colour)
void vx_fog(int znear, int zfar);             // faces fade into the sky from znear to zfar; 0,0 = off
void vx_depth(bool on);                       // z-buffer (default) or painter's algorithm
// Mode-7 floor: an infinite horizontal plane at height y, drawn per screen row with no triangles
// (one division per row; lit by the sun/ambient, fogged, blended to its average colour far away).
// tex: texture handle (power of two) or -1 for a flat color565; repeat: world units per texture
// tile (0 = no floor). Exact for unrolled cameras (vx_look_at); a rolled one costs a divide/pixel.
void vx_floor(int y, int tex, int repeat, uint32_t color565);
// 360° panorama wrapped around the horizon (distant mountains, clouds): texture row horizon_row
// sits on the horizon; magenta (0xF81F) texels show the sky gradient. tex -1 = off.
void vx_panorama(int tex, int horizon_row);

// ---- particles ---------------------------------------------------------------------------------
// An emitter's particles live life_ms, fade color0 -> color1 and size0 -> size1 (world units),
// fall with `gravity` (world units/s²), and are drawn as depth-tested soft squares.
int  vx_emitter(int max, uint32_t color0, uint32_t color1, int size0, int size1, int life_ms,
                int gravity, int flags);
// Spawn `count` particles at (x,y,z) with velocity (vx,vy,vz) world units/s, each randomised by
// ±spread on every axis.
void vx_emit(int em, int x, int y, int z, int vx, int vy, int vz, int spread, int count);

// ---- frame -------------------------------------------------------------------------------------
// Render into target (w×h RGB565 from vx_open). Returns the triangles rasterised, -1 when closed.
int  vx_render(uint16_t *target);
// Picking: arm a query at canvas pixel (x,y) for the next vx_render; vx_picked() then returns the
// handle of the nearest object drawn there, or -1.
void vx_pick_at(int x, int y);
int  vx_picked(void);
int  vx_stat(int what);

size_t vx_mem_used(void);

#ifdef __cplusplus
}
#endif
