// nv_service_mgr — NucleoOS Anima service lifecycle (ref-counted).
// Services are start/stop/suspend/resume-able units (wifi, audio, anima, ...). A service
// starts on first acquire and stops at refcount 0. The Memory Broker suspends non-essential
// services before launching a RAM-heavy app and resumes them afterwards.
#pragma once
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef int nv_service_id_t;
#define NV_SERVICE_INVALID (-1)

typedef enum {
    NV_SVC_STOPPED = 0,
    NV_SVC_RUNNING,
    NV_SVC_SUSPENDED,
} nv_service_state_t;

// Emitted on NV_EV_SERVICE_STATE.
typedef struct {
    nv_service_id_t id;
    nv_service_state_t state;
    const char *name;
} nv_service_event_t;

// Lifecycle callbacks return true on success. suspend/resume are optional (NULL => the
// broker falls back to stop/start for that service).
typedef struct {
    const char *name;
    bool (*start)(void);
    bool (*stop)(void);
    bool (*suspend)(void);
    bool (*resume)(void);
    bool essential;  // never auto-suspended by the Memory Broker (e.g. display)
} nv_service_def_t;

void nv_service_mgr_init(void);

// Register a service definition. `def` must outlive the registration (store a static).
nv_service_id_t nv_service_register(const nv_service_def_t *def);

// Ref-counted use. acquire starts the service on 0->1; release stops it on 1->0.
bool nv_service_acquire(nv_service_id_t id);
bool nv_service_release(nv_service_id_t id);

nv_service_state_t nv_service_state(nv_service_id_t id);
const char *nv_service_name(nv_service_id_t id);
bool nv_service_essential(nv_service_id_t id);  // true = broker never auto-suspends it
int nv_service_count(void);

// Broker hooks: suspend every RUNNING non-essential service not in `keep`, remembering which
// were suspended; nv_service_resume_suspended() restores exactly those. Nesting is not
// supported (one suspend/resume pair at a time).
void nv_service_suspend_nonessential(const nv_service_id_t *keep, int keep_n);
void nv_service_resume_suspended(void);

#ifdef __cplusplus
}
#endif
