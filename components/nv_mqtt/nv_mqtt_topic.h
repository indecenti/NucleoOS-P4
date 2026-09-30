// nv_mqtt_topic — pure MQTT topic rules for the ABI v12 app bridge (nv_mqtt_app_*): what an app
// may publish or subscribe to, and wildcard matching. No ESP-IDF; tested in tests/host "netpol".
#pragma once
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// MQTT: may an app publish to `topic`? Not empty, no wildcards/NUL, <= 127 bytes, and not in the
// system namespaces ("homeassistant/", "nucleo/", "$...").
bool np_mqtt_pub_ok(const char *topic);
// Valid subscription filter (MQTT 3.1.1 wildcard rules), <= 127 bytes, not "$..." and not "#".
bool np_mqtt_filter_ok(const char *filter);
// Does `topic` (len bytes, not NUL-terminated) match the filter?
bool np_mqtt_match(const char *filter, const char *topic, size_t len);

#ifdef __cplusplus
}
#endif
