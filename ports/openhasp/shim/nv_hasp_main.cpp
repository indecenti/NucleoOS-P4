/* nv_hasp_main.cpp — the NucleoOS entry point of openHASP (replaces upstream's src/main_pc.cpp).
 *
 * The app is a WASI reactor exporting run() (manifest "entry": "run"). run() does what
 * main_pc.cpp does — setup() once, then loop() while running — with the SDL/fbdev pieces
 * swapped for the canvas: nv_gfx_present() paces each pass and tells us when the OS closes the
 * app, lv_tick follows nv_millis(), and the touch panel is an LVGL pointer indev.
 *
 * Files: permission "fs" makes /sdcard/apps/openhasp/data the program's "/", which is also the
 * LVGL "L:" drive (LV_FS_PC_PATH "/"). openHASP's usual files live there: pages.jsonl,
 * config.json, boot.cmd / online.cmd / offline.cmd, images and fonts.
 */
#include "hasplib.h"
#include "hasp_debug.h"
#include "hasp_gui.h"
#include "mqtt/hasp_mqtt.h"
#include "hasp/hasp_dispatch.h"
#include "hasp/hasp_page.h"

#include "nucleo_sdk.h"

#include <string.h>
#include <unistd.h>

// main.cpp (upstream)
extern void setup();
extern void loop();

// nv_hasp_tz.c
extern "C" void nv_hasp_tz_init(void);

static bool touch_read(lv_indev_drv_t* drv, lv_indev_data_t* data)
{
    (void)drv;
    int x = 0, y = 0;
    const int down = nv_touch(&x, &y);
    static lv_coord_t last_x, last_y;
    if(down) {
        last_x = (lv_coord_t)x;
        last_y = (lv_coord_t)y;
    }
    data->point.x = last_x; // LVGL wants the last point on release too
    data->point.y = last_y;
    data->state   = down ? LV_INDEV_STATE_PR : LV_INDEV_STATE_REL;
    return false;
}

static void touch_register(void)
{
    static lv_indev_drv_t indev_drv;
    lv_indev_drv_init(&indev_drv);
    indev_drv.type    = LV_INDEV_TYPE_POINTER;
    indev_drv.read_cb = touch_read;
    lv_indev_drv_register(&indev_drv);
}

/* The OS back gesture: step back through openHASP's page history ("page back"), and leave the
 * app from a page whose back target is itself (the home page by default). */
static bool handle_back(void)
{
    uint8_t cur  = haspPages.get();
    uint8_t back = haspPages.get_back(cur);
    if(back == 0 || back == cur) return false;
    dispatch_text_line("page back", TAG_MAIN);
    return true;
}

#if defined(NV_SIM)
extern "C" int nv_sim_step(void); // harness hook: inject MQTT/touch, take screenshots
#endif

NV_EXPORT("run")
extern "C" void run(void)
{
    nv_hasp_tz_init();
    setup();          // upstream: config.json, GUI, pages.jsonl, boot.cmd, MQTT
    touch_register(); // after guiSetup(): LVGL needs the display registered first
    lv_obj_invalidate(lv_scr_act());

    uint32_t last = (uint32_t)nv_millis();
    while(nv_gfx_present()) {
        uint32_t now = (uint32_t)nv_millis();
        lv_tick_inc(now - last);
        last = now;

        if(nv_gfx_back() && !handle_back()) break;
#if defined(NV_SIM)
        if(!nv_sim_step()) break;
#endif
        mqttLoop(); // empty the OS queue (32 KB) before the deferred commands run: a page push
                    // from HA can take a while to build and must not overflow it meanwhile
        loop();     // upstream main.cpp: deferred MQTT commands, lv_task_handler, mqttLoop, timers
    }

    mqttStop(); // LWT "offline" + offline.cmd
}
