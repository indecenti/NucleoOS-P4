// nv_mqtt — Home Assistant integration over MQTT (docs/HOME_AUTOMATION_PLAN.md, phase F1).
//
// OFF by default. When nv_config "mqtt_en" is on and the network is up, one client connects to
// the broker in "mqtt_host"/"mqtt_port" (credentials "mqtt_user"/"mqtt_pass"), publishes HA MQTT
// discovery configs (retained) under "homeassistant/", and the panel shows up in Home Assistant
// as a device: screen light (sleep/wake + brightness), volume, mute, Do Not Disturb, notify,
// speak (TTS), foreground-app select, home/lock/restart buttons, lock + touch-activity binary
// sensors and diagnostic sensors. Availability via LWT on nucleo/<node>/status.
//
// Threading: esp-mqtt's task only parses and queues commands; one internal-stack service task
// ("nv_mqtt") owns connect/disconnect, executes commands (LVGL work under lvgl_port_lock) and
// publishes state. Plain TCP only (LAN broker); TLS waits for mbedTLS-in-PSRAM (plan F0.1).
// The password is never logged nor returned by any API.
#pragma once
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    NV_MQTT_OFF = 0,        // mqtt_en is false
    NV_MQTT_NO_BROKER,      // enabled but mqtt_host is empty
    NV_MQTT_WAIT_NET,       // waiting for Wi-Fi/Ethernet
    NV_MQTT_CONNECTING,     // client started, not (yet) accepted by the broker
    NV_MQTT_CONNECTED,
    NV_MQTT_ERROR,          // last attempt failed; retrying (detail in nv_mqtt_status)
} nv_mqtt_state_t;

// Subscribe to settings; start the service if "mqtt_en" is on. Call once after nv_config and
// the network stack are up (like nv_keydeck_init).
void nv_mqtt_init(void);

// Current state; if detail != NULL, a short English reason for NV_MQTT_ERROR / broker address
// for CONNECTED (e.g. "192.168.1.10:1883", "not authorized", "dns"). Any task.
nv_mqtt_state_t nv_mqtt_status(char *detail, size_t n);

// Node id used in topics and HA unique ids ("nucleo_a1b2c3"). Stable per board (MAC).
const char *nv_mqtt_node_id(void);

// Re-send discovery + every state (e.g. after renaming things in HA). No-op if not connected.
void nv_mqtt_republish(void);

#ifdef __cplusplus
}
#endif
