// nv_service_mgr — the system's background services as the user sees them (System Monitor >
// Services, the terminal's `ps`): Wi-Fi, the web console, MQTT, Bluetooth, the DMD receiver, ...
// Each subsystem is registered once at boot with a probe that reports its LIVE state, asked on
// every read: nothing is cached, so the list can never disagree with the subsystem itself.
// The registry only observes. Starting and stopping stay with each subsystem's own API (Settings
// drives those); RAM for heavy apps is the memory broker's job (nv_memory_broker.h).
#pragma once
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef int nv_service_id_t;
#define NV_SERVICE_INVALID (-1)

typedef enum {
    NV_SVC_STOPPED = 0,   // off (disabled by the user, or not started in this mode)
    NV_SVC_RUNNING,       // doing its job (connected, serving, streaming)
    NV_SVC_WAITING,       // on, waiting for something (the network, a client, a broker)
    NV_SVC_ERROR,         // on, its last attempt failed (it may be retrying)
} nv_service_state_t;

// Probe: the live state; may fill `detail` (never NULL, n >= 1, starts as "") with a short
// English note ("MyWiFi", "2 devices", "192.168.0.10"). Called from any task (the System Monitor
// timer, the shell): it must be quick, non-blocking and lock-light.
typedef nv_service_state_t (*nv_service_probe_fn)(char *detail, size_t n);

typedef struct {
    const char *name;            // short, stable, lower case ("wifi", "web", "mqtt", ...)
    nv_service_probe_fn probe;
} nv_service_def_t;

void nv_service_mgr_init(void);

// Register a service. `def` must outlive the registration (a static). Registering the same name
// twice returns the first id.
nv_service_id_t nv_service_register(const nv_service_def_t *def);

int nv_service_count(void);
const char *nv_service_name(nv_service_id_t id);   // "?" when invalid
// Ask the probe now. `detail` may be NULL.
nv_service_state_t nv_service_state(nv_service_id_t id, char *detail, size_t n);

#ifdef __cplusplus
}
#endif
