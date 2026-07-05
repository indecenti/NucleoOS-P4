#include "nv_service_mgr.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nv_event_bus.h"
#include "nv_log.h"

static const char *TAG = "svc";

// Locking model: s_mtx is held ACROSS the lifecycle callback (start/stop/suspend/resume) and
// the state mutation, so state and the callback are atomic — no drop-lock TOCTOU. Events are
// emitted AFTER releasing the lock (so subscribers may safely call back in).
// Constraint (documented): lifecycle callbacks and NV_EV_SERVICE_STATE subscribers must NOT
// call back into the service manager, and callbacks should be short (they run under the lock).

namespace {

constexpr int kMaxServices = 32;

struct Service {
    nv_service_def_t def;
    int refcount;
    nv_service_state_t state;
    bool suspended_by_broker;
};

Service s_svc[kMaxServices];
int s_count = 0;
SemaphoreHandle_t s_mtx = nullptr;

inline bool valid(nv_service_id_t id) {
    // s_count is only ever read/written under s_mtx by callers of this helper.
    return id >= 0 && id < s_count;
}

void emit(nv_service_id_t id, nv_service_state_t st, const char *name) {
    nv_service_event_t ev{id, st, name};
    nv_event_publish(NV_EV_SERVICE_STATE, &ev);
}

}  // namespace

void nv_service_mgr_init(void) {
    if (s_mtx) return;
    s_mtx = xSemaphoreCreateMutex();
    s_count = 0;
    NV_LOGI(TAG, "service manager init");
}

nv_service_id_t nv_service_register(const nv_service_def_t *def) {
    if (!def || !def->name || !s_mtx) return NV_SERVICE_INVALID;
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    if (s_count >= kMaxServices) {
        xSemaphoreGive(s_mtx);
        NV_LOGE(TAG, "service table full (%d)", kMaxServices);
        return NV_SERVICE_INVALID;
    }
    const nv_service_id_t id = s_count;
    s_svc[id] = {*def, 0, NV_SVC_STOPPED, false};
    s_count++;  // publish the slot only after it is fully written
    xSemaphoreGive(s_mtx);
    NV_LOGI(TAG, "registered '%s' (id %d%s)", def->name, id, def->essential ? ", essential" : "");
    return id;
}

bool nv_service_acquire(nv_service_id_t id) {
    if (!s_mtx) return false;
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    if (!valid(id)) { xSemaphoreGive(s_mtx); return false; }
    Service &s = s_svc[id];
    s.refcount++;
    bool started = false, failed = false;
    const char *name = s.def.name;
    if (s.refcount == 1 && s.state == NV_SVC_STOPPED) {
        const bool ok = s.def.start ? s.def.start() : true;  // under lock
        if (ok) {
            s.state = NV_SVC_RUNNING;
            started = true;
        } else {
            s.state = NV_SVC_STOPPED;
            s.refcount--;  // safe: still under the same lock, no other task interleaved
            failed = true;
        }
    }
    xSemaphoreGive(s_mtx);

    if (started) { NV_LOGI(TAG, "start '%s'", name); emit(id, NV_SVC_RUNNING, name); }
    else if (failed) { NV_LOGE(TAG, "start '%s' FAILED", name); }
    return !failed;
}

bool nv_service_release(nv_service_id_t id) {
    if (!s_mtx) return false;
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    if (!valid(id)) { xSemaphoreGive(s_mtx); return false; }
    Service &s = s_svc[id];
    if (s.refcount > 0) s.refcount--;
    bool stopped = false;
    const char *name = s.def.name;
    if (s.refcount == 0 && s.state != NV_SVC_STOPPED) {
        if (s.def.stop) s.def.stop();  // under lock
        s.state = NV_SVC_STOPPED;
        s.suspended_by_broker = false;
        stopped = true;
    }
    xSemaphoreGive(s_mtx);

    if (stopped) { NV_LOGI(TAG, "stop '%s'", name); emit(id, NV_SVC_STOPPED, name); }
    return true;
}

nv_service_state_t nv_service_state(nv_service_id_t id) {
    if (!s_mtx) return NV_SVC_STOPPED;
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    const nv_service_state_t st = valid(id) ? s_svc[id].state : NV_SVC_STOPPED;
    xSemaphoreGive(s_mtx);
    return st;
}

const char *nv_service_name(nv_service_id_t id) {
    if (!s_mtx) return "?";
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    const char *n = valid(id) ? s_svc[id].def.name : "?";
    xSemaphoreGive(s_mtx);
    return n;
}

bool nv_service_essential(nv_service_id_t id) {
    if (!s_mtx) return false;
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    const bool e = valid(id) ? s_svc[id].def.essential : false;
    xSemaphoreGive(s_mtx);
    return e;
}

int nv_service_count(void) {
    if (!s_mtx) return 0;
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    const int n = s_count;
    xSemaphoreGive(s_mtx);
    return n;
}

void nv_service_suspend_nonessential(const nv_service_id_t *keep, int keep_n) {
    if (!s_mtx) return;
    // Snapshot count once under the lock; ids are append-only so the range is stable.
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    const int n = s_count;
    xSemaphoreGive(s_mtx);

    for (nv_service_id_t id = 0; id < n; id++) {
        xSemaphoreTake(s_mtx, portMAX_DELAY);
        Service &s = s_svc[id];
        bool in_keep = false;
        for (int k = 0; keep && k < keep_n; k++)
            if (keep[k] == id) { in_keep = true; break; }
        bool suspended = false;
        const char *name = s.def.name;
        if (!s.def.essential && !in_keep && s.state == NV_SVC_RUNNING) {
            const bool ok = s.def.suspend ? s.def.suspend()
                                          : (s.def.stop ? s.def.stop() : true);  // under lock
            if (ok) {
                s.state = NV_SVC_SUSPENDED;
                s.suspended_by_broker = true;
                suspended = true;
            }
        }
        xSemaphoreGive(s_mtx);
        if (suspended) {
            NV_LOGI(TAG, "suspend '%s' (free RAM for app)", name);
            emit(id, NV_SVC_SUSPENDED, name);
        }
    }
}

void nv_service_resume_suspended(void) {
    if (!s_mtx) return;
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    const int n = s_count;
    xSemaphoreGive(s_mtx);

    for (nv_service_id_t id = 0; id < n; id++) {
        xSemaphoreTake(s_mtx, portMAX_DELAY);
        Service &s = s_svc[id];
        bool resumed = false, failed = false;
        const char *name = s.def.name;
        if (s.suspended_by_broker && s.state == NV_SVC_SUSPENDED) {
            const bool ok = s.def.resume ? s.def.resume()
                                         : (s.def.start ? s.def.start() : true);  // under lock
            s.state = ok ? NV_SVC_RUNNING : NV_SVC_STOPPED;
            s.suspended_by_broker = false;
            resumed = ok;
            failed = !ok;
        }
        xSemaphoreGive(s_mtx);
        if (resumed) { NV_LOGI(TAG, "resume '%s'", name); emit(id, NV_SVC_RUNNING, name); }
        else if (failed) { NV_LOGW(TAG, "resume '%s' failed", name); emit(id, NV_SVC_STOPPED, name); }
    }
}
