/* nv_hasp_device.cpp — the NucleoOS "hardware" behind openHASP's PC target:
 *
 *   dev::PosixDevice  haspDevice   node name, hardware id, backlight (nv_backlight), uptime
 *   dev::TftNullDrv   haspTft      LVGL flush -> nv_gfx_blit_raw (the app canvas, RGB565 LE)
 *   dev::BaseTouch    haspTouch    unused: the touch indev is registered in nv_hasp_main.cpp
 *   PosixMillis / msleep           nv_millis / nv_sleep_ms (hasp_conf.h maps millis/delay here)
 *
 * The class declarations are upstream's (src/dev/posix/hasp_posix.h, src/drv/tft/
 * tft_driver_null.h); only these definitions replace upstream's Linux ones (uname, sysinfo,
 * pthreads, /sys/class/backlight), which do not exist on the board.
 */
#include "hasplib.h"
#include "hasp_debug.h"
#include "dev/device.h"
#include "drv/tft/tft_driver.h"
#include "drv/touch/touch_driver.h"

#include "nucleo_sdk.h"

#include <string.h>

dev::PosixDevice haspDevice;
dev::TftNullDrv haspTft;
dev::BaseTouch haspTouch;

unsigned long PosixMillis()
{
    return (unsigned long)(uint32_t)nv_millis();
}

void msleep(unsigned long ms)
{
    if(ms > 0) nv_sleep_ms((int32_t)(ms > 1000 ? 1000 : ms));
}

bool hasp_use_psram()
{
    return true; // PSRAM-sized caches (LV_IMG_CACHE_DEF_SIZE_PSRAM, FreeType glyph cache)
}

#if defined(__wasi__)
extern "C" FILE* popen(const char*, const char*)
{
    return NULL;
}
extern "C" int pclose(FILE*)
{
    return -1;
}
#endif

namespace dev {

PosixDevice::PosixDevice()
{
    _hostname         = MQTT_NODENAME; // "plate"; config.json {"mqtt":{"name":...}} overrides it
    _core_version     = "NucleoOS";
    _chip_model       = "ESP32-P4";
    _backlight_pin    = 0;
    _backlight_power  = 1;
    _backlight_invert = 0;
    _backlight_level  = 255;
}

void PosixDevice::set_config(const JsonObject& settings)
{
    (void)settings;
}

void PosixDevice::reboot()
{} // like upstream's PC build: nothing to restart inside an app

void PosixDevice::show_info()
{
    LOG_VERBOSE(0, "Processor  : %s", get_chip_model());
    LOG_VERBOSE(0, "OS Version : %s", get_core_version());
}

const char* PosixDevice::get_hostname()
{
    return _hostname.c_str();
}

void PosixDevice::set_hostname(const char* hostname)
{
    if(hostname && *hostname) _hostname = hostname;
}

const char* PosixDevice::get_core_version()
{
    return _core_version.c_str();
}

const char* PosixDevice::get_chip_model()
{
    return _chip_model.c_str();
}

/* The HA integration keys the device on this id (discovery "hwid"), so it must be stable across
 * runs and unique per board: 12 hex digits drawn once from the hardware RNG and kept in the app's
 * own SD folder (nv_save). */
const char* PosixDevice::get_hardware_id()
{
    static char id[16];
    if(id[0]) return id;
    char buf[16] = {0};
    int32_t n = nv_load("hwid", buf, 12);
    bool ok   = n == 12;
    for(int i = 0; ok && i < 12; i++) ok = (buf[i] >= '0' && buf[i] <= '9') || (buf[i] >= 'a' && buf[i] <= 'f');
    if(!ok) {
        static const char hex[] = "0123456789abcdef";
        for(int i = 0; i < 12; i += 4) {
            uint32_t r = (uint32_t)nv_rand();
            for(int k = 0; k < 4; k++) buf[i + k] = hex[(r >> (4 * k)) & 15];
        }
        nv_save("hwid", buf, 12);
    }
    memcpy(id, buf, 12);
    id[12] = '\0';
    return id;
}

void PosixDevice::set_backlight_pin(uint8_t pin)
{
    _backlight_pin = pin;
}

void PosixDevice::set_backlight_invert(bool invert)
{
    _backlight_invert = invert;
}

bool PosixDevice::get_backlight_invert()
{
    return _backlight_invert;
}

void PosixDevice::set_backlight_level(uint8_t level)
{
    _backlight_level = level;
    update_backlight();
}

uint8_t PosixDevice::get_backlight_level()
{
    return _backlight_level;
}

void PosixDevice::set_backlight_power(bool power)
{
    _backlight_power = power;
    update_backlight();
}

bool PosixDevice::get_backlight_power()
{
    return _backlight_power != 0;
}

/* openHASP levels are 0..255 (plus on/off); the panel takes 0..100 %. The OS puts the user's own
 * brightness back when the app exits. */
void PosixDevice::update_backlight()
{
    int pct = 0;
    if(_backlight_power) {
        pct = (_backlight_level * 100 + 127) / 255;
        if(pct < 1 && _backlight_level > 0) pct = 1;
    }
    nv_backlight(pct);
}

size_t PosixDevice::get_free_max_block()
{
    return get_free_heap();
}

size_t PosixDevice::get_free_heap()
{
#if defined(__wasm__)
    /* ram_budget (manifest) minus the linear memory grown so far: what malloc can still get. */
    const size_t budget = 8u * 1024 * 1024;
    const size_t used   = __builtin_wasm_memory_size(0) * 65536u;
    return used < budget ? budget - used : 0;
#else
    return 0;
#endif
}

uint8_t PosixDevice::get_heap_fragmentation()
{
    return 0;
}

uint16_t PosixDevice::get_cpu_frequency()
{
    return 360;
}

long PosixDevice::get_uptime()
{
    return (long)((uint32_t)nv_millis() / 1000u);
}

bool PosixDevice::is_system_pin(uint8_t pin)
{
    (void)pin;
    return false;
}

void PosixDevice::run_thread(void (*func)(void*), void* arg)
{
    func(arg); // single-threaded; only the "shell" command uses it, and popen always fails here
}

/* ---- display ------------------------------------------------------------------------------- */

void TftNullDrv::init(int32_t w, int h)
{
    _width  = w;
    _height = h;
    nv_gfx_persist(1); // keep the last frame: LVGL only redraws the areas that changed
}

void TftNullDrv::show_info()
{
    LOG_VERBOSE(TAG_TFT, F("Driver     : NucleoOS canvas %dx%d"), _width, _height);
}

void TftNullDrv::splashscreen()
{}

void TftNullDrv::set_rotation(uint8_t rotation)
{
    (void)rotation;
}

void TftNullDrv::set_invert(bool invert)
{
    (void)invert;
}

/* LV_COLOR_DEPTH 16, LV_COLOR_16_SWAP 0: lv_color_t is RGB565 in native (little-endian) order,
 * exactly the canvas format, and the area's pixels are contiguous with stride w. */
void TftNullDrv::flush_pixels(lv_disp_drv_t* disp, const lv_area_t* area, lv_color_t* color_p)
{
    const int32_t w = area->x2 - area->x1 + 1;
    const int32_t h = area->y2 - area->y1 + 1;
    if(w > 0 && h > 0) nv_gfx_blit_raw(color_p, w * h * (int32_t)sizeof(lv_color_t), area->x1, area->y1, w, h);
    lv_disp_flush_ready(disp);
}

bool TftNullDrv::is_driver_pin(uint8_t pin)
{
    (void)pin;
    return false;
}

const char* TftNullDrv::get_tft_model()
{
    return "NucleoOS canvas";
}

int32_t TftNullDrv::width()
{
    return _width;
}

int32_t TftNullDrv::height()
{
    return _height;
}

} // namespace dev
