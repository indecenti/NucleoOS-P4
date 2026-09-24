// USB transport: Windows IDD driver frames (nv_usb vendor endpoint) -> engine; touch -> HID.
#include "ss_internal.h"
#include "nv_ss_links.h"

#include "nv_usb.h"
#include "nv_config.h"
#include "nv_event_bus.h"
#include "nv_log.h"

#include "esp_timer.h"
#include <algorithm>

#include "esp_heap_caps.h"

#include <climits>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <strings.h>
#include <sys/stat.h>

extern const char nucleocast_py_start[] asm("_binary_nucleocast_py_start");
extern const char nucleocast_py_end[] asm("_binary_nucleocast_py_end");

namespace {

constexpr const char *TAG = "ss_usb";
// The driver renders 1024x576 (JPEG-MCU aligned 16:9); the panel is 600 tall: 12 px bands.
constexpr int kYOff = (NV_SS_PANEL_H - NV_USB_SCREEN_H) / 2;

bool s_device_mode = false;          // latched at boot (the personality can't change live)
volatile int64_t s_mount_us = 0;     // last mount edge, 0 while unplugged
volatile uint32_t s_frames = 0;
volatile int64_t s_last_frame_us = 0;
// "Disconnect" on a USB session: the PC keeps sending, so ignore its frames until the cable is
// re-plugged or the app page is reopened (otherwise the next frame would take the panel again).
volatile bool s_suppressed = false;

void on_touch(const nv_ss_touch_pt_t *p, int n, void *) {
    nv_usb_touch_pt_t out[NV_USB_TOUCH_MAX];
    if (n > NV_USB_TOUCH_MAX) n = NV_USB_TOUCH_MAX;
    for (int i = 0; i < n; i++) {
        int y = p[i].y - kYOff;   // panel 1024x600 -> desktop 1024x576
        if (y < 0) y = 0;
        if (y >= NV_USB_SCREEN_H) y = NV_USB_SCREEN_H - 1;
        int x = p[i].x < NV_USB_SCREEN_W ? p[i].x : NV_USB_SCREEN_W - 1;
        out[i] = {p[i].id, (uint16_t)x, (uint16_t)y, 30};
    }
    nv_usb_touch_report(n ? out : nullptr, (uint8_t)n);
}

// Only a real link drop ends the session: the IDD driver sends frames ONLY when the desktop
// changes, so a frame gap is normal (a static desktop keeps the last picture, like a monitor).
bool alive(void *) { return nv_usb_mounted(); }

void on_stop(bool by_user, void *) { if (by_user) s_suppressed = true; }

const nv_ss_source_ops_t kOps = {
    on_touch, nullptr, nullptr, on_stop, alive, nullptr,
};

// Runs on the nv_usb transfer task for every complete JPEG frame from the PC.
void frame_sink(const uint8_t *jpg, uint32_t len, uint16_t w, uint16_t h, void *) {
    s_frames = s_frames + 1;
    s_last_frame_us = esp_timer_get_time();
    if (w != NV_USB_SCREEN_W || h != NV_USB_SCREEN_H || s_suppressed) return;
    if (!nv_ss_owner(NV_SS_SRC_USB) && !nv_ss_begin(NV_SS_SRC_USB, &kOps, "PC (USB)")) return;
    // Pool buffers come from jpeg_alloc_decoder_mem(INPUT): decode in place, no copy.
    nv_ss_present_jpeg(NV_SS_SRC_USB, jpg, len, true, 0, kYOff, 0, 0);
}

// ---------------------------------------------------------------- setup drive
// What the host sees on the "NUCLEOOS" drive: instructions, the sender page (runs from the drive
// with the board address baked in: a file:// page is a secure context, so screen sharing works
// with no certificate warning), the Windows driver installer from the SD card, the helper.
const char kReadme[] =
    "NucleoOS - Secondo schermo / Second Screen\r\n"
    "==========================================\r\n\r\n"
    "ITALIANO\r\n"
    "- Windows, schermo esteso via cavo: apri \"Installa driver Windows.exe\" (solo la prima volta),\r\n"
    "  poi premi Win+P e scegli Estendi. Il touch della scheda comanda quello schermo.\r\n"
    "- Qualsiasi computer, senza installare nulla: apri \"Secondo schermo.html\" con Chrome, Edge o\r\n"
    "  Firefox e premi \"Condividi schermo\" (computer e scheda sulla stessa rete Wi-Fi o via cavo di rete).\r\n"
    "- Mac: Impostazioni di Sistema > Generali > Condivisione > Condivisione schermo, poi sulla scheda\r\n"
    "  apri Secondo schermo > Mac.\r\n"
    "- Android: app gratuita droidVNC-NG, poi sulla scheda Secondo schermo > Android.\r\n\r\n"
    "ENGLISH\r\n"
    "- Windows, extended screen over the cable: run \"Installa driver Windows.exe\" (first time only),\r\n"
    "  then press Win+P and choose Extend. The board's touch controls that screen.\r\n"
    "- Any computer, nothing to install: open \"Secondo schermo.html\" in Chrome, Edge or Firefox and\r\n"
    "  click \"Share screen\" (computer and board on the same Wi-Fi or wired network).\r\n"
    "- Mac: System Settings > General > Sharing > Screen Sharing, then on the board open\r\n"
    "  Second Screen > Mac.\r\n"
    "- Android: free app droidVNC-NG, then on the board Second Screen > Android.\r\n";

char *s_page_buf = nullptr;     // rendered sender page (fixed size, refreshed on mount)
size_t s_page_len = 0;
char s_drv_path[160] = "";
FILE *s_drv = nullptr;          // driver installer, opened lazily on first read

void render_drive_page(void) {
    if (!s_page_buf) return;
    ss_cast_render_page(s_page_buf, s_page_len, true);   // same length every time (fixed widths)
}

int read_page(void *, uint32_t off, void *buf, uint32_t len) {
    if (!s_page_buf || off >= s_page_len) return 0;
    const uint32_t n = std::min<uint32_t>(len, (uint32_t)(s_page_len - off));
    memcpy(buf, s_page_buf + off, n);
    return (int)n;
}

int read_driver(void *, uint32_t off, void *buf, uint32_t len) {
    // Runs on the TinyUSB task: plain stdio on the SD card (FATFS locks internally).
    if (!s_drv) s_drv = fopen(s_drv_path, "rb");
    if (!s_drv || fseek(s_drv, (long)off, SEEK_SET) != 0) return 0;
    return (int)fread(buf, 1, len, s_drv);
}

void publish_drive(void) {
    static nv_usb_file_t files[4];
    int n = 0;
    files[n] = {};
    files[n].name = "LEGGIMI - README.txt";
    memcpy(files[n].short83, "LEGGIMI TXT", 11);
    files[n].size = sizeof kReadme - 1;
    files[n].data = (const uint8_t *)kReadme;
    n++;

    const size_t plen = ss_cast_render_page(nullptr, 0, true);
    if (!s_page_buf && plen) s_page_buf = (char *)heap_caps_malloc(plen, MALLOC_CAP_SPIRAM);
    if (s_page_buf) {
        s_page_len = plen;
        render_drive_page();
        files[n] = {};
        files[n].name = "Secondo schermo.html";
        memcpy(files[n].short83, "SCHERMO HTM", 11);
        files[n].size = (uint32_t)plen;
        files[n].read = read_page;
        n++;
    }

    // First *.exe in the Windows driver folder on the SD card.
    if (DIR *d = opendir("/sdcard/nucleos/drivers/windows")) {
        while (struct dirent *e = readdir(d)) {
            const size_t l = strlen(e->d_name);
            if (l > 4 && strcasecmp(e->d_name + l - 4, ".exe") == 0) {
                snprintf(s_drv_path, sizeof s_drv_path, "/sdcard/nucleos/drivers/windows/%s", e->d_name);
                break;
            }
        }
        closedir(d);
    }
    struct stat st;
    if (s_drv_path[0] && stat(s_drv_path, &st) == 0 && st.st_size > 0) {
        files[n] = {};
        files[n].name = "Installa driver Windows.exe";
        memcpy(files[n].short83, "DRIVER  EXE", 11);
        files[n].size = (uint32_t)st.st_size;
        files[n].read = read_driver;
        n++;
    }

    size_t pyl = (size_t)(nucleocast_py_end - nucleocast_py_start);
    if (pyl && nucleocast_py_start[pyl - 1] == 0) pyl--;
    files[n] = {};
    files[n].name = "nucleocast.py";
    memcpy(files[n].short83, "NCAST   PY ", 11);
    files[n].size = (uint32_t)pyl;
    files[n].data = (const uint8_t *)nucleocast_py_start;
    n++;

    nv_usb_drive_publish(files, n);
}

void on_usb_event(nv_event_t, const void *d, void *) {
    auto *e = static_cast<const nv_usb_display_ev_t *>(d);
    if (e->streaming_unclaimed) return;   // our own auto-open nudges reuse this event
    s_mount_us = e->mounted ? esp_timer_get_time() : 0;
    if (e->mounted) s_suppressed = false;
    // A fresh plug-in: bake the current address into the drive's page (same size, new content).
    if (e->mounted) render_drive_page();
    else if (s_drv) { fclose(s_drv); s_drv = nullptr; }
}

}  // namespace

void ss_usb_init(void) {
    s_device_mode = !nv_config_get_bool("usbhost", true);
    nv_event_subscribe(NV_EV_USB_DISPLAY, on_usb_event, nullptr);
    if (s_device_mode) publish_drive();   // before nv_usb_init: the host enumerates right away
    if (nv_usb_mounted() && !s_mount_us) s_mount_us = esp_timer_get_time();
}

void ss_usb_attach(void) {
    s_suppressed = false;
    nv_usb_set_frame_sink(frame_sink, nullptr);
}

void ss_usb_detach(void) {
    // Non-synchronous: a sink already running finishes against the engine, whose calls all
    // re-check ownership under its lock (closed engine = frame dropped), so no drain is needed.
    nv_usb_set_frame_sink(nullptr, nullptr);
}

void nv_ss_usb_info(nv_ss_usb_info_t *out) {
    if (!out) return;
    memset(out, 0, sizeof *out);
    const int64_t now = esp_timer_get_time();
    out->device_mode = s_device_mode;
    out->device_mode_saved = !nv_config_get_bool("usbhost", true);
    out->mounted = nv_usb_mounted();
    const int64_t m = s_mount_us;
    if (out->mounted && !m) s_mount_us = now;   // mounted before we subscribed
    out->mounted_for_ms = (out->mounted && m) ? (uint32_t)((now - m) / 1000) : 0;
    out->frames = s_frames;
    const int64_t lf = s_last_frame_us;
    out->last_frame_ago_ms = lf ? (uint32_t)((now - lf) / 1000) : UINT32_MAX;
}

void nv_ss_usb_set_device_mode(bool device) {
    nv_config_set_bool("usbhost", !device);
    NV_LOGI(TAG, "USB personality for next boot: %s", device ? "device (PC screen)" : "host (accessories)");
}
