// w4run — run a WASM-4 cart on the PC through the firmware's own WASM-4 host code
// (components/nv_wasm/nv_wasm_w4.cpp, with FreeRTOS/IDF shimmed) on the same WAMR tree, built with
// the device's configuration (fast interpreter, software bounds checks, WASI, ref-types).
//
//   w4run cart.wasm [--frames N] [--stack KB] [--screen out.ppm] [--canvas out.ppm]
//                   [--icon out.argb] [--prep out.wasm] [--quiet]
//
// Default: load exactly like nv_wasm (nv_w4_prepare_module -> load -> instantiate with a single
// fixed page -> nv_w4_begin), run N frames (default 600 = 10 s) with an autoplay input pattern
// (X/Z taps, D-pad wander, screen clicks) and print one line:
//   RESULT <cart> <OK|TRAP|LOAD_FAIL|INST_FAIL|BEGIN_FAIL|NO_UPDATE> frames=.. changes=..
//          avg_ms=.. max_ms=.. [ex=..] | missing: <imports the OS doesn't provide>
// --screen   the 160x160 screen upscaled (480x480) at the last frame; --canvas the whole 1024x600
// --icon     an 80x80 launcher icon (ARGB8888, LVGL byte order B,G,R,A) of the last frame
// --prep     only write the bytes nv_wasm would load (for wamrc: AOT carts need the same renames)
#include "wasm_export.h"
#include "nv_wasm_w4.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

static const char *kKnown[] = { "blit", "blitSub", "line", "hline", "vline", "oval", "rect", "text",
                                "textUtf8", "textUtf16", "tone", "diskr", "diskw", "trace",
                                "traceUtf8", "traceUtf16", "tracef" };

// Same guard as nv_wasm.cpp w4_call(): results land in argv, never NULL, never overrun.
static bool w4_call(wasm_exec_env_t env, wasm_module_inst_t inst, wasm_function_inst_t f) {
    uint32_t av[4] = {};
    const uint32_t nres = wasm_func_get_result_count(f, inst);
    if (nres > 8) return false;
    wasm_valkind_t k[8];
    uint32_t need = 0;
    if (nres) wasm_func_get_result_types(f, inst, k);
    for (uint32_t i = 0; i < nres; i++) need += (k[i] == WASM_I64 || k[i] == WASM_F64) ? 2 : k[i] == WASM_V128 ? 4 : 1;
    if (need > 4) { wasm_runtime_set_exception(inst, "export returns more values than the host expects"); return false; }
    return wasm_runtime_call_wasm(env, f, 0, av);
}

static void rgb_of(uint16_t c, uint8_t out[3]) {
    out[0] = (uint8_t)((c >> 11) << 3);
    out[1] = (uint8_t)(((c >> 5) & 63) << 2);
    out[2] = (uint8_t)((c & 31) << 3);
}

static bool write_ppm(const char *path, const uint16_t *cv, int x0, int y0, int w, int h) {
    FILE *o = fopen(path, "wb");
    if (!o) return false;
    fprintf(o, "P6 %d %d 255\n", w, h);
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            uint8_t rgb[3];
            rgb_of(cv[(y0 + y) * kW4CanvasW + x0 + x], rgb);
            fwrite(rgb, 1, 3, o);
        }
    fclose(o);
    return true;
}

// 80x80 icon: the 480x480 screen box-filtered 6:1, with rounded corners (transparent).
static bool write_icon(const char *path, const uint16_t *cv) {
    FILE *o = fopen(path, "wb");
    if (!o) return false;
    const int S = 80, F = kW4Side / S, R = 14;
    for (int y = 0; y < S; y++)
        for (int x = 0; x < S; x++) {
            unsigned r = 0, g = 0, b = 0;
            for (int yy = 0; yy < F; yy++)
                for (int xx = 0; xx < F; xx++) {
                    uint8_t c[3];
                    rgb_of(cv[(kW4Y0 + y * F + yy) * kW4CanvasW + kW4X0 + x * F + xx], c);
                    r += c[0]; g += c[1]; b += c[2];
                }
            const int n = F * F;
            const int cx = x < R ? R - x : (x >= S - R ? x - (S - R - 1) : 0);
            const int cy = y < R ? R - y : (y >= S - R ? y - (S - R - 1) : 0);
            const uint8_t a = (cx * cx + cy * cy > R * R) ? 0 : 255;
            const uint8_t px[4] = { (uint8_t)(b / n), (uint8_t)(g / n), (uint8_t)(r / n), a };
            fwrite(px, 1, 4, o);
        }
    fclose(o);
    return true;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: w4run cart.wasm [--frames N] [--stack KB] [--screen f.ppm] [--canvas f.ppm] "
                        "[--icon f.argb] [--prep f.wasm] [--quiet]\n");
        return 2;
    }
    const char *cart = argv[1], *screen = nullptr, *canvas = nullptr, *icon = nullptr, *prep = nullptr;
    int frames = 600;
    uint32_t stack = 16 * 1024;
    bool quiet = false;
    for (int i = 2; i < argc; i++) {
        const std::string a = argv[i];
        const char *v = i + 1 < argc ? argv[i + 1] : "";
        if (a == "--frames") { frames = atoi(v); i++; }
        else if (a == "--stack") { stack = (uint32_t)atoi(v) * 1024; i++; }
        else if (a == "--screen") { screen = v; i++; }
        else if (a == "--canvas") { canvas = v; i++; }
        else if (a == "--icon") { icon = v; i++; }
        else if (a == "--prep") { prep = v; i++; }
        else if (a == "--quiet") quiet = true;
    }
    if (quiet) freopen("/dev/null", "w", stderr);
    const char *name = strrchr(cart, '/') ? strrchr(cart, '/') + 1 : cart;

    std::vector<uint8_t> b;
    if (FILE *f = fopen(cart, "rb")) {
        fseek(f, 0, SEEK_END); b.resize((size_t)ftell(f)); fseek(f, 0, SEEK_SET);
        if (fread(b.data(), 1, b.size(), f) != b.size()) b.clear();
        fclose(f);
    }
    if (b.empty()) { printf("RESULT %s READ_FAIL\n", name); return 1; }

    nv_w4_prepare_module(b.data(), (uint32_t)b.size());
    if (prep) {
        FILE *o = fopen(prep, "wb");
        const bool ok = o && fwrite(b.data(), 1, b.size(), o) == b.size();
        if (o) fclose(o);
        printf("%s %s -> %s\n", ok ? "PREP" : "PREP_FAIL", name, prep);
        return ok ? 0 : 1;
    }

    wasm_runtime_init();
    nv_w4_init();
    char err[200];
    wasm_module_t mod = wasm_runtime_load(b.data(), (uint32_t)b.size(), err, sizeof err);
    if (!mod) { printf("RESULT %s LOAD_FAIL %s\n", name, err); return 1; }

    std::string missing;
    const int32_t ni = wasm_runtime_get_import_count(mod);
    for (int32_t i = 0; i < ni; i++) {
        wasm_import_t im;
        memset(&im, 0, sizeof im);
        wasm_runtime_get_import_type(mod, i, &im);
        if (im.kind != WASM_IMPORT_EXPORT_KIND_FUNC) continue;
        bool known = false;
        if (!strcmp(im.module_name, "env"))
            for (const char *k : kKnown) if (!strcmp(k, im.name)) known = true;
        if (!known) { missing += im.module_name; missing += "."; missing += im.name; missing += " "; }
    }

    InstantiationArgs ia;
    memset(&ia, 0, sizeof ia);
    ia.default_stack_size = stack;
    ia.max_memory_pages = 1;
    wasm_module_inst_t inst = wasm_runtime_instantiate_ex(mod, &ia, err, sizeof err);
    if (!inst) { printf("RESULT %s INST_FAIL %s | missing: %s\n", name, err, missing.c_str()); return 1; }
    wasm_exec_env_t env = wasm_runtime_create_exec_env(inst, stack);
    wasm_function_inst_t update = wasm_runtime_lookup_function(inst, "update");
    wasm_function_inst_t start  = wasm_runtime_lookup_function(inst, "start");
    wasm_function_inst_t init   = wasm_runtime_lookup_function(inst, kW4Init);
    wasm_function_inst_t st     = wasm_runtime_lookup_function(inst, kW4Start);
    if (!update) { printf("RESULT %s NO_UPDATE | missing: %s\n", name, missing.c_str()); return 1; }
    if (!nv_w4_begin(inst, "w4run", err, sizeof err)) { printf("RESULT %s BEGIN_FAIL %s\n", name, err); return 1; }

    static uint16_t cv[kW4CanvasW * kW4CanvasH];
    int rc[4];
    nv_w4_render_overlay(cv, true, rc);
    // Autoplay: X taps every second, Z taps, D-pad wander, occasional screen clicks (menus/mouse carts).
    const int padx = kW4X0 / 2, pady = 360, bxx = kW4CanvasW - kW4X0 / 2 + 50, bxy = 320,
              bzx = kW4CanvasW - kW4X0 / 2 - 60, bzy = 440;
    const int dx[4] = { 80, -80, 0, 0 }, dy[4] = { 0, 0, -80, 80 };
    uint32_t rng = 12345;
    double max_ms = 0, sum_ms = 0;
    int ran = 0, changes = 0;
    const char *status = "OK";
    for (int fr = 0; fr < frames; fr++) {
        int xs[4], ys[4], n = 0;
        if ((fr % 60) >= 30 && (fr % 60) < 36) { xs[n] = bxx; ys[n] = bxy; n++; }
        if ((fr % 150) >= 100 && (fr % 150) < 104) { xs[n] = bzx; ys[n] = bzy; n++; }
        if ((fr / 20) % 3 != 0) {
            rng = rng * 1103515245u + 12345u;
            const int d = (rng >> 16) & 3;
            xs[n] = padx + dx[d]; ys[n] = pady + dy[d]; n++;
        }
        if ((fr % 200) >= 180 && (fr % 200) < 183) { xs[n] = kW4X0 + 240; ys[n] = kW4Y0 + 240; n++; }
        nv_w4_input(xs, ys, n);
        nv_w4_frame_begin(fr == 0);
        const auto t0 = std::chrono::steady_clock::now();
        bool ok = true;
        if (fr == 0) {
            if (st) ok = w4_call(env, inst, st);
            if (ok && init) ok = w4_call(env, inst, init);
            if (ok && start) ok = w4_call(env, inst, start);
        }
        if (ok) ok = w4_call(env, inst, update);
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        if (!ok) { status = "TRAP"; break; }
        nv_w4_frame_end();
        ran++;
        if (fr > 0) { sum_ms += ms; if (ms > max_ms) max_ms = ms; }
        if (nv_w4_render(cv, false, rc)) changes++;
        nv_w4_render_overlay(cv, false, rc);
    }
    const char *ex = wasm_runtime_get_exception(inst);
    if (screen) write_ppm(screen, cv, kW4X0, kW4Y0, kW4Side, kW4Side);
    if (canvas) write_ppm(canvas, cv, 0, 0, kW4CanvasW, kW4CanvasH);
    if (icon) write_icon(icon, cv);
    nv_w4_end();
    printf("RESULT %s %s frames=%d changes=%d avg_ms=%.3f max_ms=%.3f%s%s | missing: %s\n", name, status, ran,
           changes, ran > 1 ? sum_ms / (ran - 1) : 0.0, max_ms, ex ? " ex=" : "", ex ? ex : "", missing.c_str());
    return strcmp(status, "OK") ? 1 : 0;
}
