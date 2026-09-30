// nv_ha_proto — pure (no ESP-IDF) half of the Home Assistant MQTT integration: the entity table,
// discovery-payload builder, command-topic router and the bounded flat-JSON reader used on
// broker payloads. Everything that touches bytes from the LAN lives here so tests/host can
// unit-test and fuzz it (ENGINEERING_RULES §6).
#pragma once
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// Entities exposed to HA. Order = discovery publish order.
typedef enum {
    HA_E_SCREEN = 0,   // light (json schema): on/off = panel sleep/wake, brightness 1..100
    HA_E_VOLUME,       // number 0..100
    HA_E_MUTE,         // switch
    HA_E_DND,          // switch (Do Not Disturb)
    HA_E_NOTIFY,       // notify: text or {"title","message"} -> notification center
    HA_E_SAY,          // notify: text -> offline TTS
    HA_E_APP,          // select: foreground app id ("home" = launcher)
    HA_E_HOME,         // button
    HA_E_LOCK,         // button (lock only; unlocking stays on the panel)
    HA_E_REBOOT,       // button
    HA_E_LOCKED,       // binary_sensor
    HA_E_TOUCH,        // binary_sensor occupancy: touched in the last HA_TOUCH_WINDOW_S
    HA_E_TEMP,         // sensor (diag json)
    HA_E_RSSI,
    HA_E_UPTIME,
    HA_E_SRAM,
    HA_E_PSRAM,
    HA_E_IP,
    HA_E_COUNT
} ha_entity_t;

#define HA_TOUCH_WINDOW_S 60
#define HA_NODE_MAX       24     // "nucleo_a1b2c3"
#define HA_TEXT_MAX       200    // longest notify/say text accepted (bytes, UTF-8)

typedef struct {
    const char *obj;        // object id + topic segment ("screen")
    const char *component;  // HA platform ("light")
    const char *name_en;
    const char *name_it;
    bool        has_cmd;    // subscribes nucleo/<node>/<obj>/set
    bool        has_state;  // own state topic nucleo/<node>/<obj>/state (else diag json)
    const char *extra;      // extra discovery keys, already JSON ("\"ic\":\"mdi:x\"") or ""
} ha_entity_def_t;

const ha_entity_def_t *ha_entity(ha_entity_t e);

// "nucleo/<node>/<obj>/state" etc. Return false if it would not fit.
bool ha_topic(char *out, size_t cap, const char *node, const char *obj, const char *leaf);
// "homeassistant/<component>/<node>/<obj>/config" under `prefix`.
bool ha_config_topic(char *out, size_t cap, const char *prefix, const char *node, ha_entity_t e);

// Discovery config JSON for one entity. `italian` picks the entity names; `sw` = firmware
// version; `options_json` = JSON array for HA_E_APP (e.g. ["home","calc"]), ignored otherwise.
// Returns bytes written (without NUL) or 0 if `cap` is too small.
size_t ha_build_config(char *out, size_t cap, ha_entity_t e, const char *node, bool italian,
                       const char *sw, const char *model, const char *options_json);

// Map an incoming topic to the entity whose command it is: "nucleo/<node>/<obj>/set".
// Returns HA_E_COUNT for anything else (foreign node, unknown obj, state topics, garbage).
ha_entity_t ha_route_cmd(const char *topic, size_t topic_len, const char *node);

// Bounded reader for a FLAT JSON object (the only shape HA sends us). Finds `key` at the top
// level and copies its string value (unescaped, \uXXXX -> UTF-8, control chars dropped) into
// out[n]. Numbers/true/false/null are copied as their literal text. Nested objects/arrays as
// values are skipped, never descended into. Returns false if absent or malformed. `json` need
// not be NUL-terminated.
bool ha_json_get(const char *json, size_t len, const char *key, char *out, size_t n);
// Integer convenience over ha_json_get (also accepts "80" strings). False if absent/not a number.
bool ha_json_get_int(const char *json, size_t len, const char *key, int *out);

// Copy a raw payload into out[n] as display text: stops at NUL, drops control characters except
// \n, trims trailing whitespace. Invalid UTF-8 bytes become '?'. Returns bytes written.
size_t ha_payload_text(const char *p, size_t len, char *out, size_t n);

// Parse "ON"/"OFF" (case-insensitive, also 1/0/true/false). False if neither.
bool ha_parse_onoff(const char *p, size_t len, bool *on);
// Parse a decimal integer payload (optional sign, surrounding whitespace, a ".x" tail from HA
// number entities is truncated). False if not a number.
bool ha_parse_int(const char *p, size_t len, int *out);

// JSON string escape for building payloads (quotes, backslash, control chars). Always
// NUL-terminates; truncates on a UTF-8 boundary. Returns bytes written.
size_t ha_json_escape(char *out, size_t n, const char *s);

#ifdef __cplusplus
}
#endif
