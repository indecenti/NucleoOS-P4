// The system's background services in the service registry (nv_service_mgr): what System Monitor >
// Services and the terminal's `ps` list. Each probe reads the subsystem's own live state; nothing is
// cached or guessed. Registered from app_main once each subsystem has been started, so a service
// left off in safe mode (or by the USB mode) is not listed as if it ran.
#include "system_services.h"

#include <cstdio>

#include "nv_service_mgr.h"
#include "nv_config.h"
#include "nv_wifi.h"
#include "nv_eth.h"
#include "nv_web.h"
#include "nv_mqtt.h"
#include "nv_bt.h"
#include "nv_keydeck.h"
#include "nv_dmd.h"
#include "nv_usb.h"
#include "nv_usb_storage.h"
#include "nv_telemetry.h"

namespace {

nv_service_state_t wifi_probe(char *d, size_t n) {
    switch (nv_wifi_get_state()) {
        case NV_WIFI_DISABLED:
            return NV_SVC_STOPPED;
        case NV_WIFI_CONNECTED:
            nv_wifi_get_connected(d, n, nullptr, 0, nullptr);
            return NV_SVC_RUNNING;
        case NV_WIFI_FAILED:
            snprintf(d, n, "%s", nv_wifi_last_fail_auth() ? "wrong password" : "network not found");
            return NV_SVC_ERROR;
        case NV_WIFI_CONNECTING:
            snprintf(d, n, "connecting");
            return NV_SVC_WAITING;
        default:   // on, not connected (idle or scanning)
            snprintf(d, n, "not connected");
            return NV_SVC_WAITING;
    }
}

nv_service_state_t eth_probe(char *d, size_t n) {
    switch (nv_eth_get_state()) {
        case NV_ETH_UP:   snprintf(d, n, "%d Mbit/s", nv_eth_speed_mbps()); return NV_SVC_RUNNING;
        case NV_ETH_LINK: snprintf(d, n, "waiting for DHCP"); return NV_SVC_WAITING;
        case NV_ETH_DOWN: snprintf(d, n, "no cable"); return NV_SVC_WAITING;
        default:          return NV_SVC_STOPPED;
    }
}

nv_service_state_t web_probe(char *d, size_t n) {
    if (nv_web_running()) return NV_SVC_RUNNING;
    snprintf(d, n, "waiting for Wi-Fi");
    return NV_SVC_WAITING;
}

nv_service_state_t mqtt_probe(char *d, size_t n) {
    switch (nv_mqtt_status(d, n)) {
        case NV_MQTT_OFF:        d[0] = '\0'; return NV_SVC_STOPPED;
        case NV_MQTT_CONNECTED:  return NV_SVC_RUNNING;
        case NV_MQTT_ERROR:      return NV_SVC_ERROR;
        case NV_MQTT_NO_BROKER:  snprintf(d, n, "no broker set"); return NV_SVC_WAITING;
        case NV_MQTT_WAIT_NET:   snprintf(d, n, "waiting for network"); return NV_SVC_WAITING;
        default:                 return NV_SVC_WAITING;   // connecting
    }
}

nv_service_state_t bt_probe(char *d, size_t n) {
    nv_bt_status_t st;
    nv_bt_status(&st);
    switch (st.state) {
        case NV_BT_OFF:   return NV_SVC_STOPPED;
        case NV_BT_ERROR: snprintf(d, n, "%s", st.error); return NV_SVC_ERROR;
        default: break;
    }
    if (st.n_connected) {
        snprintf(d, n, "%u connected", (unsigned)st.n_connected);
        return NV_SVC_RUNNING;
    }
    snprintf(d, n, "%u paired", (unsigned)st.n_paired);
    return NV_SVC_WAITING;
}

nv_service_state_t keydeck_probe(char *d, size_t n) {
    if (!nv_config_get_bool("keydeck_en", false)) return NV_SVC_STOPPED;
    if (nv_keydeck_client_connected()) return NV_SVC_RUNNING;
    snprintf(d, n, "no client");
    return NV_SVC_WAITING;
}

// The DMD receiver runs while the DMD app is open (it is the cabinet's display).
nv_service_state_t dmd_probe(char *d, size_t n) {
    nv_dmd_status_t st;
    nv_dmd_get_status(&st);
    if (!st.listening && !st.usb_ready) return NV_SVC_STOPPED;
    if (st.usb || st.client) {
        if (st.game[0]) snprintf(d, n, "%s (%s)", st.game, st.usb ? "USB" : st.peer);
        else            snprintf(d, n, "%s", st.usb ? "USB" : st.peer);
        return NV_SVC_RUNNING;
    }
    if (st.bridge) { snprintf(d, n, "Popper bridge"); return NV_SVC_RUNNING; }
    snprintf(d, n, "port %d", st.port);
    return NV_SVC_WAITING;
}

nv_service_state_t usb_host_probe(char *d, size_t n) {
    const int devs = nv_usb_bus_list(nullptr, 0);
    if (devs <= 0) return NV_SVC_WAITING;
    snprintf(d, n, "%d device%s", devs, devs == 1 ? "" : "s");
    return NV_SVC_RUNNING;
}

nv_service_state_t usb_screen_probe(char *d, size_t n) {
    if (nv_usb_mounted()) { snprintf(d, n, "PC connected"); return NV_SVC_RUNNING; }
    snprintf(d, n, "no PC");
    return NV_SVC_WAITING;
}

nv_service_state_t stats_probe(char *, size_t) {
    return nv_telemetry_consent() == NV_TELEMETRY_YES ? NV_SVC_RUNNING : NV_SVC_STOPPED;
}

const nv_service_def_t kWifi       = {"wifi", wifi_probe};
const nv_service_def_t kEth        = {"ethernet", eth_probe};
const nv_service_def_t kWeb        = {"web", web_probe};
const nv_service_def_t kMqtt       = {"mqtt", mqtt_probe};
const nv_service_def_t kBt         = {"bluetooth", bt_probe};
const nv_service_def_t kKeydeck    = {"keydeck", keydeck_probe};
const nv_service_def_t kDmd        = {"dmd", dmd_probe};
const nv_service_def_t kUsbHost    = {"usb-host", usb_host_probe};
const nv_service_def_t kUsbScreen  = {"usb-screen", usb_screen_probe};
const nv_service_def_t kStats      = {"statistics", stats_probe};

}  // namespace

void system_services_register(bool safe_mode, bool usb_host) {
    nv_service_register(&kWifi);
    nv_service_register(&kEth);
    nv_service_register(&kWeb);
    if (!safe_mode) {
        nv_service_register(&kMqtt);
        nv_service_register(&kBt);
        nv_service_register(&kKeydeck);
        nv_service_register(&kDmd);
        nv_service_register(usb_host ? &kUsbHost : &kUsbScreen);
    }
    nv_service_register(&kStats);
}
