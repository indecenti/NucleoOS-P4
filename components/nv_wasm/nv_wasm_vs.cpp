// nv_wasm_vs — ABI v20: the VoxelSpaceNG terrain engine (components/voxel) for WASM apps. See
// nv_wasm.h for the import table.
//
// The app owns one world: vs_open loads /sdcard/apps/<id>/data/<world>.vsw and <sky>.vss into PSRAM
// (or "lib:name" from a declared package), vs_camera places the eye, vs_render draws a whole frame
// on both cores straight into the canvas. The frame is column-major, which is exactly the buffer of
// a quarter-turned canvas: the app calls nv_gfx_set_rotation(1) first and the PPA shows it upright
// (vs_render refuses an unturned canvas). Everything is freed when the run ends (nv_wasm.cpp).
//
// All calls run on the app's own worker thread, the only thread that touches the engine.
#include "nv_wasm_net.h"
#include "nv_wasm.h"
#include "nv_log.h"
#include "voxel_nv.h"

namespace {

constexpr const char *TAG = "vs";

bool gfx_ok(wasm_exec_env_t env) { return (nv_wasm_env_perms(env) & NV_WPERM_GFX) != 0; }

int32_t nvi_vs_open(wasm_exec_env_t env, const char *world, const char *sky) {
    char wp[160], sp[160];
    if (!gfx_ok(env)) return -1;
    if (!nv_wasm_app_asset(world, "data", "vsw", wp, sizeof wp) ||
        !nv_wasm_app_asset(sky, "data", "vss", sp, sizeof sp)) return -1;
    const int r = voxel_open(wp, sp);
    if (r < 0) NV_LOGW(TAG, "open %s / %s failed (%d)", wp, sp, r);
    return r;
}

void nvi_vs_close(wasm_exec_env_t env) {
    if (gfx_ok(env)) voxel_close();
}

void nvi_vs_camera(wasm_exec_env_t env, int32_t x, int32_t y, int32_t h, int32_t yaw, int32_t pitch,
                   int32_t roll, int32_t time_ms) {
    if (gfx_ok(env)) voxel_camera(x, y, h, yaw, pitch, roll, time_ms);
}

int32_t nvi_vs_render(wasm_exec_env_t env) {
    int w = 0, h = 0, rot = 0;
    if (!gfx_ok(env) || !voxel_is_open()) return -1;
    uint16_t *fb = nv_wasm_gfx_frame_target(&w, &h, &rot);
    if (!fb) return -1;
    if (!(rot & 1)) return -2;          // column-major needs the turned canvas (nv_gfx_set_rotation(1))
    return voxel_render(fb, h, w);      // upright w x h canvas = h columns of w rows
}

int32_t nvi_vs_height(wasm_exec_env_t env, int32_t x, int32_t y) {
    return gfx_ok(env) ? voxel_height(x, y) : 0;
}

int32_t nvi_vs_config(wasm_exec_env_t env, int32_t key, int32_t value) {
    return gfx_ok(env) ? voxel_config(key, value) : -1;
}

int32_t nvi_vs_info(wasm_exec_env_t env, int32_t key) {
    return gfx_ok(env) ? voxel_info(key) : -1;
}

NativeSymbol s_natives[] = {
    {"vs_open",   (void *)nvi_vs_open,   "($$)i",      nullptr},
    {"vs_close",  (void *)nvi_vs_close,  "()",         nullptr},
    {"vs_camera", (void *)nvi_vs_camera, "(iiiiiii)",  nullptr},
    {"vs_render", (void *)nvi_vs_render, "()i",        nullptr},
    {"vs_height", (void *)nvi_vs_height, "(ii)i",      nullptr},
    {"vs_config", (void *)nvi_vs_config, "(ii)i",      nullptr},
    {"vs_info",   (void *)nvi_vs_info,   "(i)i",       nullptr},
};

}  // namespace

void nv_wasm_vs_register(void) {
    if (!wasm_runtime_register_natives("nv", s_natives, sizeof s_natives / sizeof s_natives[0]))
        NV_LOGE(TAG, "register ABI 20 vs natives failed");
}
