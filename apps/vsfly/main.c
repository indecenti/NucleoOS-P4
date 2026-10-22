/* Sky Ranger - an arcade flight demo on the VoxelSpaceNG engine (NucleoOS ABI 20).
 *
 * Fly through 12 rings in order over the valley as fast as you can; touch the ground and you restart
 * from the last ring. The world comes from the engine demo app (manifest "requires": vsng), the
 * terrain is rendered natively on both cores, this app only flies, draws the rings and the HUD.
 *
 * Controls (input.h): bank and climb = left stick / arrows-WASD / mouse / touch stick (left half);
 * boost = RT / Shift / left button / hold on the right half; altitude trim = LB-RB / R-F / wheel;
 * pause = Esc / Start / B (again = quit), resume or restart = Enter / A / tap. Gamepads rumble on a
 * crash.
 */
#include "hud.h"
#include "input.h"

#define NRINGS   12
#define RING_R   14                      /* cells */

static int g_focal;

typedef struct { int x, y, h; } pt;      /* x, y Q16 cells; h Q4 height units */
static pt g_ring[NRINGS];
static int g_size, g_water, g_maxh;

static int iabs(int v) { return v < 0 ? -v : v; }

static void make_course(void)
{
    int i, k;
    for (i = 0; i < NRINGS; i++) {
        int a = i * 4096 / NRINGS;
        int r = g_size / 4 + (isin(a * 3) >> 11);              /* a wobbly loop round the map centre, cells */
        int hmax = 0;
        g_ring[i].x = (g_size << 15) + icos(a) * r;
        g_ring[i].y = (g_size << 15) + isin(a) * r;
        for (k = -2; k <= 2; k++) {                            /* clear the ground round it */
            int hx = vs_height(g_ring[i].x + (k << 19), g_ring[i].y);
            int hy = vs_height(g_ring[i].x, g_ring[i].y + (k << 19));
            if (hx > hmax) hmax = hx;
            if (hy > hmax) hmax = hy;
        }
        if (hmax < g_water << 4) hmax = g_water << 4;
        g_ring[i].h = hmax + ((RING_R + 10 + (i % 3) * 8) << 4);
    }
}

/* ---- camera projection, the engine's: fov 80, vscale 205/256 ---- */
typedef struct { int x, y, h, yaw, pitch, roll; } cam_t;
static int project(const cam_t *c, const pt *p, int *sx, int *sy, int *z_q8)
{
    int dx = (p->x - c->x) >> 8, dy = (p->y - c->y) >> 8;      /* Q8 cells */
    int cy = icos(c->yaw), sy_ = isin(c->yaw);
    int z = (int)(((long long)dx * cy + (long long)dy * sy_) >> 16);
    int r = (int)(((long long)-dx * sy_ + (long long)dy * cy) >> 16);
    int dh = (c->h - p->h) * 205 / 16;                        /* Q8 cells */
    if (z < 512) return 0;
    *sx = g_w / 2 + (int)((long long)r * g_focal / z);
    *sy = g_h / 2 - c->pitch + (int)((long long)dh * g_focal / z);
    *sy += (int)(((long long)c->roll * (*sx - g_w / 2)) >> 16);
    *z_q8 = z;
    return 1;
}

/* is the straight line from the eye to p clear of the ground? */
static int visible(const cam_t *c, const pt *p)
{
    int k;
    for (k = 1; k < 8; k++) {
        int x = c->x + (int)(((long long)(p->x - c->x) * k) >> 3);
        int y = c->y + (int)(((long long)(p->y - c->y) * k) >> 3);
        int h = c->h + (int)(((long long)(p->h - c->h) * k) >> 3);
        if (vs_height(x, y) > h) return 0;
    }
    return 1;
}

static void draw_ring(const cam_t *c, const pt *p, int col, int col2)
{
    int sx, sy, z, r;
    if (!project(c, p, &sx, &sy, &z)) return;
    r = (int)((long long)(RING_R << 8) * g_focal / z);
    if (r < 3 || r > g_w) return;
    ring_s(sx, sy, r, 28, col);
    ring_s(sx, sy, r - 2, 28, col);
    ring_s(sx, sy, r - 4, 28, col2);
}

/* arrow at the screen edge toward a target that is behind or off screen */
static void draw_pointer(const cam_t *c, const pt *p)
{
    int a = (iatan2((p->y - c->y) >> 8, (p->x - c->x) >> 8) - c->yaw) & 4095;   /* 0 = ahead, 1024 = right */
    int cx = g_w / 2, cy = g_h / 2, rx = g_w / 2 - 40, ry = g_h / 2 - 40;
    int ex = cx + ((isin(a) * rx) >> 16), ey = cy - ((icos(a) * ry) >> 16);
    int dx = isin(a) >> 11, dy = -icos(a) >> 11;            /* direction, ~32 px */
    tri_s(ex + dx, ey + dy, ex - dy / 2, ey + dx / 2, ex + dy / 2, ey - dx / 2, C565(255, 210, 40));
}

static void rumble(int ms)
{
    int i, n = nv_pad_count();
    for (i = 0; i < n; i++) nv_pad_rumble(i, 40000, 60000, ms);
}

NV_EXPORT("run")
void run(void)
{
    in_state in;
    cam_t cam;
    int px, py, ph, yaw, bank = 0, vs = 0;        /* plane: Q16 cells, Q4 units, angle; vertical speed Q4/s */
    int ring = 0, t_start = 0, t_now, best = 0, crashed_until = 0, finished_ms = 0, countdown_until = 0;
    int paused = 0, pause_t = 0, last;

    sin_init();
    if (nv_gfx_set_rotation(1) != 1 || vs_open("vsng:alps", "vsng:day") < 0) {
        nv_log(3, "vsfly: needs ABI 20 firmware and the VoxelSpace NG app (world files)");
        while (nv_gfx_present()) { nv_gfx_clear(C565(60, 0, 0)); if (nv_gfx_back()) break; }
        return;
    }
    g_w = nv_gfx_height();
    g_h = nv_gfx_width();
    g_focal = g_w * 596 / 1000;                  /* w/2 / tan(40 deg): the engine's fov 80 */
    g_size = 1 << vs_info(VS_INFO_LOG2SIZE);
    g_water = vs_info(VS_INFO_WATER);
    g_maxh = vs_info(VS_INFO_MAXH);
    vs_config(VS_CFG_TARGET_MS, 33);             /* adaptive quality: hold ~30 fps */
    if (nv_load("best", &best, sizeof best) != sizeof best) best = 0;
    __builtin_memset(&in, 0, sizeof in);
    make_course();

respawn:
    {
        const pt *from = &g_ring[(ring + NRINGS - 1) % NRINGS], *to = &g_ring[ring];
        px = from->x; py = from->y; ph = from->h;
        if (ring == 0) {                           /* start: behind ring 0, facing it */
            int a = iatan2((to->y - g_ring[NRINGS - 1].y) >> 8, (to->x - g_ring[NRINGS - 1].x) >> 8);
            px = to->x - icos(a) * 120;
            py = to->y - isin(a) * 120;
            ph = to->h;
        }
        {   /* clear of the ground along the first leg: the plane starts above whatever lies before the ring */
            int k;
            for (k = 0; k <= 8; k++) {
                int x = px + (int)(((long long)(to->x - px) * k) >> 3), y = py + (int)(((long long)(to->y - py) * k) >> 3);
                int g = vs_height(x, y) + (25 << 4);
                if (g > ph) ph = g;
            }
        }
        yaw = iatan2((to->y - py) >> 8, (to->x - px) >> 8);
        bank = 0; vs = 0;
        last = nv_millis();
        countdown_until = ring == 0 ? last + 3000 : last + 1000;
        if (ring == 0) { t_start = countdown_until; finished_ms = 0; }
    }

    for (;;) {
        int dt, ground, speed, flying;
        t_now = nv_millis();
        dt = t_now - last;
        if (dt > 50) dt = 50;
        last = t_now;
        if (nv_gfx_back()) { if (paused) break; paused = 1; pause_t = t_now; }
        in_poll(&in, g_w, g_h, dt);

        if (paused) {
            if (in.back_edge) break;
            if (in.confirm_edge) {                 /* resume: the clocks skip the pause */
                int d = t_now - pause_t;
                paused = 0; t_start += d; countdown_until += d;
                if (crashed_until) crashed_until += d;
            }
        } else if (in.pause_edge) {
            paused = 1; pause_t = t_now;
        } else if (finished_ms) {
            if (in.confirm_edge && t_now - finished_ms > 1000) { ring = 0; goto respawn; }
        }

        flying = !paused && !finished_ms && t_now >= countdown_until && !crashed_until;
        if (flying) {
            /* flight model: bank turns, stick climbs (pull back = climb), speed with boost */
            int stx = in.move_x + in.look_x * 12, sty = -in.move_y - in.look_y * 12;
            if (stx > 256) stx = 256;
            if (stx < -256) stx = -256;
            if (sty > 256) sty = 256;
            if (sty < -256) sty = -256;
            bank += (stx * 380 / 256 - bank) * dt / 160;
            yaw = (yaw + bank * dt / 500) & 4095;
            vs += ((sty * 72 * 16 / 256) + in.climb * 4 - vs) * dt / 300;          /* to +-72 units/s */
            ph += vs * dt / 1000;
            speed = 70 + in.boost * 50 / 256;                                      /* cells/s */
            px += (int)(((long long)icos(yaw) * speed * dt) / 1000);
            py += (int)(((long long)isin(yaw) * speed * dt) / 1000);
            if (ph > (g_maxh + 160) << 4) ph = (g_maxh + 160) << 4;

            ground = vs_height(px, py);
            if (ground < g_water << 4) ground = g_water << 4;
            if (ph < ground + (2 << 4)) {                                           /* crash */
                crashed_until = t_now + 1200;
                nv_gfx_tone(110, 300);
                rumble(400);
            }
            {
                const pt *r = &g_ring[ring];
                int dx = iabs((r->x - px) >> 16), dy = iabs((r->y - py) >> 16), dh = iabs(r->h - ph) >> 4;
                if (dx * dx + dy * dy + dh * dh < RING_R * RING_R) {
                    nv_gfx_tone(880 + ring * 40, 80);
                    if (++ring == NRINGS) {
                        int t = t_now - t_start;
                        finished_ms = t_now;
                        if (!best || t < best) { best = t; nv_save("best", &best, sizeof best); }
                        ring = 0;
                    }
                }
            }
        } else if (crashed_until && !paused && t_now >= crashed_until) {
            crashed_until = 0;
            goto respawn;
        }

        cam.x = px; cam.y = py; cam.h = ph; cam.yaw = yaw;
        cam.pitch = g_h / 10 - vs / 12;          /* nose follows the climb */
        cam.roll = -bank * 100 * 1024 / g_w;     /* horizon tilts against the bank */
        vs_camera(cam.x, cam.y, cam.h, cam.yaw, cam.pitch, cam.roll, t_now);
        vs_render();

        /* rings: the next one bright, the one after dim; pointer when it is not in view */
        if (!finished_ms) {
            const pt *r = &g_ring[ring], *r2 = &g_ring[(ring + 1) % NRINGS];
            int sx, sy, z;
            if (visible(&cam, r2)) draw_ring(&cam, r2, C565(150, 120, 40), C565(90, 70, 20));
            if (visible(&cam, r)) draw_ring(&cam, r, C565(255, 220, 40), C565(255, 140, 0));
            if (!project(&cam, r, &sx, &sy, &z) || sx < 0 || sx >= g_w || sy < 0 || sy >= g_h) draw_pointer(&cam, r);
        }
        /* crosshair */
        rect_s(g_w / 2 - 14, g_h / 2 - 1, 9, 3, 0xFFFF);
        rect_s(g_w / 2 + 5, g_h / 2 - 1, 9, 3, 0xFFFF);
        rect_s(g_w / 2 - 1, g_h / 2 - 1, 3, 3, C565(255, 80, 60));

        /* HUD: ring, time, best, countdown, crash / finish bars, touch stick */
        hud_int(16, 16, finished_ms ? NRINGS : ring, 4, C565(255, 220, 40));
        hud_time(g_w / 2 - 60, 16, finished_ms ? finished_ms - t_start : t_now - t_start, 4, 0xFFFF);
        if (best) hud_time(g_w - 150, 16, best, 3, C565(120, 230, 140));
        if (t_now < countdown_until && !paused) hud_int(g_w / 2 - 20, g_h / 2 - 90, (countdown_until - t_now) / 1000 + 1, 12, 0xFFFF);
        if (crashed_until) rect_s(0, g_h - 14, g_w, 14, C565(220, 30, 30));
        if (finished_ms) rect_s(0, g_h - 14, g_w, 14, C565(60, 200, 90));
        if (in.last == IN_TOUCH) hud_stick(in.stick_on, in.stick_x0, in.stick_y0, in.stick_x, in.stick_y);
        if (paused) hud_pause();

        if (!nv_gfx_present()) break;
    }
    vs_close();
}
