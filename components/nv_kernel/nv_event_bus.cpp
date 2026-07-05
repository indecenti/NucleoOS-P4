#include "nv_event_bus.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nv_log.h"

namespace {

constexpr int kMaxSubs = 8;  // per event

struct Sub {
    nv_event_cb_t cb;
    void *user;
};

Sub s_subs[NV_EV__COUNT][kMaxSubs];
SemaphoreHandle_t s_mtx = nullptr;

inline bool valid(nv_event_t ev) { return ev >= 0 && ev < NV_EV__COUNT; }

}  // namespace

void nv_event_init(void) {
    if (s_mtx) return;
    s_mtx = xSemaphoreCreateMutex();
    for (int e = 0; e < NV_EV__COUNT; e++)
        for (int i = 0; i < kMaxSubs; i++) s_subs[e][i] = {nullptr, nullptr};
    NV_LOGI("event", "event bus init (%d topics)", (int)NV_EV__COUNT);
}

bool nv_event_subscribe(nv_event_t ev, nv_event_cb_t cb, void *user) {
    if (!valid(ev) || !cb || !s_mtx) return false;
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    bool ok = false;
    for (int i = 0; i < kMaxSubs; i++) {
        if (s_subs[ev][i].cb == nullptr) {
            s_subs[ev][i] = {cb, user};
            ok = true;
            break;
        }
    }
    xSemaphoreGive(s_mtx);
    if (!ok) NV_LOGW("event", "subscriber table full for topic %d", (int)ev);
    return ok;
}

void nv_event_unsubscribe(nv_event_t ev, nv_event_cb_t cb, void *user) {
    if (!valid(ev) || !s_mtx) return;
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    for (int i = 0; i < kMaxSubs; i++) {
        if (s_subs[ev][i].cb == cb && s_subs[ev][i].user == user) {
            s_subs[ev][i] = {nullptr, nullptr};
        }
    }
    xSemaphoreGive(s_mtx);
}

void nv_event_publish(nv_event_t ev, const void *data) {
    if (!valid(ev) || !s_mtx) return;
    // Snapshot under lock, dispatch outside it (so callbacks may (un)subscribe safely).
    Sub snapshot[kMaxSubs];
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    for (int i = 0; i < kMaxSubs; i++) snapshot[i] = s_subs[ev][i];
    xSemaphoreGive(s_mtx);

    for (int i = 0; i < kMaxSubs; i++) {
        if (snapshot[i].cb) snapshot[i].cb(ev, data, snapshot[i].user);
    }
}
