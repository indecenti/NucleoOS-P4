// Vertice engine self-test scene for the PC simulator (not an installable app): checks axes
// (Y up, +X right, camera looking +Z by default), winding of a custom mesh, lighting, the grid
// checkerboard, a texture, fog, particles, picking and the HUD over the 3D frame.
#include "nucleo_sdk.h"

static const int32_t kWedgeXYZ[] = {   // a ramp: 6 verts, 8 tris, faceted
    -100, 0, -100,   100, 0, -100,   100, 0, 100,   -100, 0, 100,   -100, 120, 100,   100, 120, 100,
};
static const uint16_t kWedgeIdx[] = {
    0, 3, 2,  0, 2, 1,        // bottom (faces down)
    3, 4, 5,  3, 5, 2,        // back wall (+Z)
    0, 1, 5,  0, 5, 4,        // slope
    0, 4, 3,  1, 2, 5,        // sides
};
static uint16_t tex[32 * 32];

NV_EXPORT("run")
void run(void) {
    const int W = nv_gfx_width(), H = nv_gfx_height();
    vx_sky(NV_RGB(40, 90, 200), NV_RGB(200, 225, 255));
    vx_lens(70, 32, 6000);
    vx_fog(2500, 5500);
    int grass = vx_material(NV_RGB(60, 150, 60), VX_FLAT, 255, -1, 0);
    int grass2 = vx_material(NV_RGB(80, 175, 70), VX_FLAT, 255, -1, 0);
    vx_obj_pos(vx_prim(VX_GRID, 8000, 8000, 16, grass, grass2), 0, 0, 0);
    for (int y = 0; y < 32; y++) for (int x = 0; x < 32; x++)
        tex[y * 32 + x] = ((x / 4 + y / 4) & 1) ? NV_RGB(230, 60, 40) : NV_RGB(250, 240, 220);
    int t = vx_texture(tex, 32, 32, 0);
    int red = vx_material(NV_RGB(220, 50, 40), VX_GOURAUD, 255, -1, 0);
    int blue = vx_material(NV_RGB(40, 90, 230), VX_PHONG, 255, -1, 200);
    int texm = vx_material(0xFFFF, VX_GOURAUD, 255, t, 0);
    int yellow = vx_material(NV_RGB(240, 200, 30), VX_FLAT, 255, -1, 0);
    int cube = vx_prim(VX_CUBE, 200, 200, 200, texm, -1);
    vx_obj_pos(cube, -400, 100, 400);
    int sphere = vx_prim(VX_SPHERE, 130, 20, 0, blue, -1);
    vx_obj_pos(sphere, 0, 130, 600);
    int cyl = vx_prim(VX_CYLINDER, 80, 260, 16, red, -1);
    vx_obj_pos(cyl, 400, 130, 400);
    int wedge = vx_mesh(kWedgeXYZ, 6, kWedgeIdx, 8, 0, 0, yellow, 0);
    vx_obj_pos(wedge, 0, 0, 200);
    {   // winding probe: A is clockwise as the camera sees it (y up), B counter-clockwise
        static const int32_t q[] = { -60, 0, 0,  -60, 120, 0,  60, 120, 0,  60, 0, 0 };
        static const uint16_t cw[] = { 0, 1, 2,  0, 2, 3 }, ccw[] = { 0, 2, 1,  0, 3, 2 };
        int white = vx_material(NV_RGB(255, 255, 255), VX_UNLIT, 255, -1, 0);
        vx_obj_pos(vx_mesh(q, 4, cw, 2, 0, 0, white, 0), -250, 0, 900);
        vx_obj_pos(vx_mesh(q, 4, ccw, 2, 0, 0, yellow, 0), 250, 0, 900);
    }
    for (int i = 0; i < 6; i++) {   // a row of cones (pyramids) fading into the fog
        int p = vx_prim(VX_PYRAMID, 120, 240, 0, red, -1);
        vx_obj_pos(p, -600 + i * 60, 0, 1200 + i * 700);
    }
    int smoke = vx_emitter(256, NV_RGB(255, 255, 255), NV_RGB(120, 120, 130), 30, 160, 2500, -40, 0);
    int spark = vx_emitter(128, NV_RGB(255, 240, 120), NV_RGB(255, 60, 0), 20, 6, 800, 600, VX_PART_ADDITIVE);
    char buf[48];
    int picked = -1;
    for (int f = 0; nv_gfx_present(); f++) {
        int a = f * 2;
        vx_camera(0, 350, -700, 0, 0, 0);
        vx_look_at(0, 80, 500);
        vx_obj_rot(cube, 0, a, 0);
        vx_obj_rot(wedge, 0, a / 2, 0);
        vx_emit(smoke, 400, 270, 400, 0, 90, 0, 25, 2);
        if ((f % 20) == 0) vx_emit(spark, 0, 280, 600, 0, 300, 0, 200, 40);
        if (f == 40) vx_pick_at(W / 2, H / 2);
        int tris = vx_render();
        if (f == 40) picked = vx_picked();
        nv_gfx_rect(0, 0, W, 12, NV_RGB(0, 0, 0));
        // HUD
        buf[0] = 0;
        {
            int n = tris, i = 0; char tmp[12]; int k = 0;
            if (!n) tmp[k++] = '0';
            while (n) { tmp[k++] = (char)('0' + n % 10); n /= 10; }
            const char *pre = "TRIS ";
            while (*pre) buf[i++] = *pre++;
            while (k) buf[i++] = tmp[--k];
            buf[i] = 0;
        }
        nv_gfx_text(4, 3, buf, NV_RGB(255, 255, 255), 1);
        nv_gfx_text(W - 60, 3, picked == sphere ? "PICK OK" : "PICK --", NV_RGB(255, 255, 0), 1);
    }
}
