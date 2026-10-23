/* VoxelSpace NG - the engine demo on NucleoOS: a free camera over the world (ABI 20).
 *
 * The firmware renders the world on both cores straight into the canvas (vs_render); this app flies
 * the camera, reads every input device (input.h), draws a small HUD and logs performance. The canvas
 * is quarter-turned (nv_gfx_set_rotation(1)): the engine's column-major frame is exactly that buffer
 * and the PPA shows it upright on the panel, scaled to fit any canvas size the manifest asks for.
 *
 * Files in the app's data folder: alps.vsw (world) and day.vss (sky), from tools/bake.py.
 * Controls: move = left stick / WASD-arrows / touch stick (left half); look = right stick / mouse /
 * drag on the right half / Q-E PgUp-PgDn; climb = LB-RB / R-F / wheel / two fingers; boost = RT /
 * Shift / left button; confirm (Enter, A, tap) = demo flight on/off; G = adaptive quality on/off;
 * pause = Esc / Start / B (again = quit). A perf line goes to the OS log every 2 s (app:vsng).
 */
#include "hud.h"
#include "input.h"

static char *put_int(char *p, int v)
{
    char t[12];
    int n = 0;
    if (v < 0) { *p++ = '-'; v = -v; }
    do { t[n++] = (char)('0' + v % 10); v /= 10; } while (v);
    while (n) *p++ = t[--n];
    return p;
}

static void log_kv(const char *tag, const char **k, const int *v, int n)
{
    char buf[220], *p = buf;
    int i;
    while (*tag) *p++ = *tag++;
    for (i = 0; i < n; i++) {
        const char *s = k[i];
        *p++ = ' ';
        while (*s) *p++ = *s++;
        *p++ = '=';
        p = put_int(p, v[i]);
    }
    *p = 0;
    nv_log(1, buf);
}

typedef struct { int x, y, h, yaw, pitch, roll; } cam_t;

/* demo flight: a lazy figure-of-eight over the map, banking into the turns (the host harness's) */
static void demo_cam(cam_t *c, int fr, int size, int water, int *eye)
{
    int ang = (fr * 18) & 4095, r = size << 14, ground, target;
    int dx = -(int)(((long long)isin(ang) * r) >> 16);
    int dy = (int)(((long long)icos(2 * ang) * r) >> 16);
    c->x = (size << 15) + (int)(((long long)icos(ang) * r) >> 16);
    c->y = (size << 15) + (int)(((long long)isin(2 * ang) * (r / 2)) >> 16);
    c->yaw = iatan2(dy >> 8, dx >> 8);
    c->roll = (icos(2 * ang) >> 3) / 4 * 512 / g_w;
    c->pitch = g_h / 6;
    ground = vs_height(c->x, c->y);
    target = ground + (40 << 4);
    if (target < ((water + 30) << 4)) target = (water + 30) << 4;
    *eye = fr == 0 ? target : *eye + (target - *eye) / 8;
    if (*eye < ground + (12 << 4)) *eye = ground + (12 << 4);
    c->h = *eye;
}

NV_EXPORT("run")
void run(void)
{
    in_state in;
    cam_t cam;
    int demo = 1, paused = 0, fr = 0, eye = 0, speed = 0, gov = 0, size, water, maxh, last;
    int t_last, n_perf = 0, sum_render = 0, sum_frame = 0, fps10 = 0;

    sin_init();
    if (nv_gfx_set_rotation(1) != 1 || vs_open("alps", "day") < 0) {
        nv_log(3, "vsng: needs ABI 20 firmware, a scaled canvas and data/alps.vsw + data/day.vss");
        while (nv_gfx_present()) { nv_gfx_clear(C565(60, 0, 0)); if (nv_gfx_back()) break; }
        return;
    }
    g_w = nv_gfx_height();               /* turned: the upright canvas is h x w */
    g_h = nv_gfx_width();
    size = 1 << vs_info(VS_INFO_LOG2SIZE);
    water = vs_info(VS_INFO_WATER);
    maxh = vs_info(VS_INFO_MAXH);
    {
        const char *k[] = { "w", "h", "size", "water", "maxh" };
        int v[] = { g_w, g_h, size, water, maxh };
        log_kv("vsng: open", k, v, 5);
    }
    __builtin_memset(&in, 0, sizeof in);
    __builtin_memset(&cam, 0, sizeof cam);
    demo_cam(&cam, 0, size, water, &eye);
    t_last = last = nv_millis();

    for (;;) {
        int t0 = nv_millis(), dt = t0 - last, us = 0;
        if (dt > 50) dt = 50;
        last = t0;
        if (nv_gfx_back()) { if (paused) break; paused = 1; }
        in_poll(&in, g_w, g_h, dt);
        if (in_key_edge(&in, 0x0A)) {                                    /* G: adaptive quality */
            gov = !gov;
            vs_config(VS_CFG_TARGET_MS, gov ? 33 : 0);
            if (!gov) vs_config(VS_CFG_QUALITY, 256);
        }
        if (paused) {
            if (in.back_edge) break;
            if (in.confirm_edge) paused = 0;
        } else if (in.pause_edge) {
            paused = 1;
        } else {
            if (in.confirm_edge) demo = !demo;
            if (in.move_x || in.move_y || in.look_x || in.look_y || in.climb) demo = 0;
            if (demo) {
                demo_cam(&cam, fr, size, water, &eye);
            } else {
                int fwd = -in.move_y, turn = in.move_x * dt / 256 + in.look_x * 2, ground;
                int vmax = (in.boost ? 6 : 3) << 16;                     /* cells per 50 ms frame, Q16 */
                speed += (fwd * vmax / 256 - speed) * dt / 200;
                cam.yaw = (cam.yaw + turn) & 4095;
                cam.roll += (-turn * 2400 / (dt + 1) * 1024 / g_w - cam.roll) / 6;   /* bank into turns */
                cam.pitch += in.look_y;
                cam.x += (int)(((long long)icos(cam.yaw) * (speed / 50 * dt)) >> 16);
                cam.y += (int)(((long long)isin(cam.yaw) * (speed / 50 * dt)) >> 16);
                cam.h += in.climb * dt / 64;
                if (cam.pitch < -g_h / 2) cam.pitch = -g_h / 2;
                if (cam.pitch > g_h / 2) cam.pitch = g_h / 2;
                ground = vs_height(cam.x, cam.y);
                if (cam.h < ground + (6 << 4)) cam.h = ground + (6 << 4);
                if (cam.h > (maxh + 200) << 4) cam.h = (maxh + 200) << 4;
                eye = cam.h;
            }
            fr++;
        }

#ifdef VSNG_BENCH
        /* profile build: a fixed view, one engine setting changed at a time, 30 frames each, logged */
        {
            static const struct { const char *name; int cores, smooth, nearz, quality, detail, direct, zfar; } B[] = {
                { "warm", 2, -1, -1, 256, 256, 0, 0 }, { "strips", 2, -1, -1, 256, 256, 0, 0 },
                { "direct", 2, -1, -1, 256, 256, 1, 0 }, { "strips1c", 1, -1, -1, 256, 256, 0, 0 },
                { "direct1c", 1, -1, -1, 256, 256, 1, 0 }, { "strips2", 2, -1, -1, 256, 256, 0, 0 },
            };
            static int stage = -1, frames = 0, sum = 0, smooth0 = 0, near0 = 0;
            static int zfar0 = 0;
            if (stage < 0) {
                demo_cam(&cam, 40, size, water, &eye);
                vs_camera(cam.x, cam.y, cam.h, cam.yaw, cam.pitch, cam.roll, t0);
                vs_render();                                           /* fits the view: defaults exist */
                smooth0 = vs_config(VS_CFG_SMOOTH, -1);
                near0 = vs_config(VS_CFG_NEAR, -1);
                zfar0 = vs_config(VS_CFG_ZFAR, -1);
                stage = 0;
            }
            demo = 0;
            if (frames == 0 && stage < (int)(sizeof B / sizeof B[0])) {
                vs_config(VS_CFG_CORES, B[stage].cores);
                vs_config(VS_CFG_SMOOTH, B[stage].smooth < 0 ? smooth0 : B[stage].smooth);
                vs_config(VS_CFG_NEAR, B[stage].nearz < 0 ? near0 : B[stage].nearz);
                vs_config(VS_CFG_QUALITY, B[stage].quality);
                vs_config(VS_CFG_DETAIL, B[stage].detail);
                vs_config(VS_CFG_DIRECT, B[stage].direct);
                vs_config(VS_CFG_ZFAR, B[stage].zfar ? B[stage].zfar : zfar0);
            }
            vs_camera(cam.x, cam.y, cam.h, cam.yaw, cam.pitch, cam.roll, t0);
            us = vs_render();
            if (frames >= 5) sum += us;                                    /* skip the warm-up frames */
            if (++frames == 65 && stage < (int)(sizeof B / sizeof B[0])) {
                const char *k[] = { "stage", "render_us", "samples", "near_px" };
                int v[4];
                v[0] = stage; v[1] = sum / 60; v[2] = vs_info(VS_INFO_SAMPLES); v[3] = vs_info(VS_INFO_NEAR_PX);
                log_kv(B[stage].name, k, v, 4);
                stage++; frames = 0; sum = 0;
            }
        }
#else
        vs_camera(cam.x, cam.y, cam.h, cam.yaw, cam.pitch, cam.roll, t0);
        us = vs_render();
#endif
        hud_int(12, 12, fps10 / 10, 3, gov ? C565(120, 230, 140) : 0xFFFF);
        if (in.last == IN_TOUCH) hud_stick(in.stick_on, in.stick_x0, in.stick_y0, in.stick_x, in.stick_y);
        if (paused) hud_pause();
        if (!nv_gfx_present()) break;

        sum_render += us;
        sum_frame += nv_millis() - t0;
        n_perf++;
        if (nv_millis() - t_last >= 2000) {
            int now = nv_millis();
            const char *k[] = { "fps10", "render_us", "frame_ms", "samples", "near_px", "quality", "fast", "gov", "input", "xs" };
            int v[10];
            fps10 = n_perf * 10000 / (now - t_last);
            v[0] = fps10; v[1] = sum_render / n_perf; v[2] = sum_frame / n_perf;
            v[3] = vs_info(VS_INFO_SAMPLES); v[4] = vs_info(VS_INFO_NEAR_PX); v[5] = vs_info(VS_INFO_QUALITY);
            v[6] = vs_info(VS_INFO_FAST); v[7] = gov; v[8] = in.last; v[9] = vs_info(VS_INFO_XSCALE);
            log_kv("vsng: perf", k, v, 10);
            n_perf = sum_render = sum_frame = 0;
            t_last = now;
        }
    }
    vs_close();
}
