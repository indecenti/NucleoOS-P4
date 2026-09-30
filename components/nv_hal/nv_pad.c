// nv_pad — see nv_pad.h.
#include "nv_pad.h"

#include "nv_log.h"

#include "esp_timer.h"
#include "freertos/FreeRTOS.h"

#include <string.h>

static const char *TAG = "pad";

// ---------------------------------------------------------------- slots

// Drivers write from their own tasks (USB HID, XInput, NimBLE host), games read from theirs: every
// access copies under a spinlock (a few dozen bytes), callbacks run outside it.
typedef struct {
    uint32_t          seq;          // connection order; 0 = free
    nv_pad_info_t     info;
    nv_pad_input_t    in;
    nv_pad_rumble_fn  rumble;
    void             *rumble_ctx;
    esp_timer_handle_t stop;        // ends a timed rumble
} Slot;

static Slot s_slots[NV_PAD_MAX];
static uint32_t s_seq, s_gen;
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;

static void neutral(nv_pad_input_t *in) {
    memset(in, 0, sizeof *in);
}

int nv_pad_attach(const nv_pad_info_t *info) {
    int slot = -1;
    portENTER_CRITICAL(&s_mux);
    for (int i = 0; i < NV_PAD_MAX; i++) {
        if (s_slots[i].seq) continue;
        Slot *s = &s_slots[i];
        s->info = *info;
        s->info.name[sizeof s->info.name - 1] = 0;
        neutral(&s->in);
        s->rumble = NULL;
        s->rumble_ctx = NULL;
        s->seq = ++s_seq;
        s_gen++;
        slot = i;
        break;
    }
    portEXIT_CRITICAL(&s_mux);
    if (slot < 0) NV_LOGW(TAG, "%s ignored: %d controllers already connected", info->name, NV_PAD_MAX);
    else NV_LOGI(TAG, "player slot %d: %s (%04x:%04x, %s, %s)", slot, info->name, info->vid, info->pid,
                 info->source == NV_PAD_SRC_BLE ? "BLE" : info->source == NV_PAD_SRC_XINPUT ? "XInput" : "USB HID",
                 info->mapped ? "mapped" : "guessed layout");
    return slot;
}

static bool valid(int slot) { return slot >= 0 && slot < NV_PAD_MAX; }

void nv_pad_set_rumble(int slot, nv_pad_rumble_fn fn, void *ctx) {
    if (!valid(slot)) return;
    portENTER_CRITICAL(&s_mux);
    s_slots[slot].rumble = fn;
    s_slots[slot].rumble_ctx = ctx;
    s_slots[slot].info.rumble = fn != NULL;
    portEXIT_CRITICAL(&s_mux);
}

void nv_pad_set_battery(int slot, uint8_t percent) {
    if (!valid(slot)) return;
    portENTER_CRITICAL(&s_mux);
    s_slots[slot].info.battery = percent;
    portEXIT_CRITICAL(&s_mux);
}

void nv_pad_update(int slot, const nv_pad_input_t *in) {
    if (!valid(slot)) return;
    nv_pad_input_t v = *in;
    if (v.axis[NV_PADA_LT] > 16384) v.buttons |= NV_PADB_LT;
    if (v.axis[NV_PADA_RT] > 16384) v.buttons |= NV_PADB_RT;
    portENTER_CRITICAL(&s_mux);
    if (s_slots[slot].seq) s_slots[slot].in = v;
    portEXIT_CRITICAL(&s_mux);
}

void nv_pad_detach(int slot) {
    if (!valid(slot)) return;
    portENTER_CRITICAL(&s_mux);
    const bool was = s_slots[slot].seq != 0;
    s_slots[slot].seq = 0;
    s_slots[slot].rumble = NULL;
    neutral(&s_slots[slot].in);
    if (was) s_gen++;
    portEXIT_CRITICAL(&s_mux);
    if (s_slots[slot].stop) esp_timer_stop(s_slots[slot].stop);
    if (was) NV_LOGI(TAG, "player slot %d: %s disconnected", slot, s_slots[slot].info.name);
}

// index-th connected slot in connection order, -1 if none. Caller holds s_mux.
static int nth(int index) {
    uint32_t last = 0;
    int pick = -1;
    for (int k = 0; k <= index; k++) {
        int next = -1;
        for (int i = 0; i < NV_PAD_MAX; i++)
            if (s_slots[i].seq > last && (next < 0 || s_slots[i].seq < s_slots[next].seq)) next = i;
        if (next < 0) return -1;
        last = s_slots[next].seq;
        pick = next;
    }
    return pick;
}

int nv_pad_count(void) {
    int n = 0;
    portENTER_CRITICAL(&s_mux);
    for (int i = 0; i < NV_PAD_MAX; i++) if (s_slots[i].seq) n++;
    portEXIT_CRITICAL(&s_mux);
    return n;
}

bool nv_pad_get(int index, nv_pad_input_t *in, nv_pad_info_t *info) {
    if (index < 0) return false;
    portENTER_CRITICAL(&s_mux);
    const int s = nth(index);
    if (s >= 0) {
        if (in) *in = s_slots[s].in;
        if (info) *info = s_slots[s].info;
    }
    portEXIT_CRITICAL(&s_mux);
    return s >= 0;
}

uint32_t nv_pad_generation(void) { return s_gen; }

static void rumble_stop_cb(void *arg) {
    const int slot = (int)(intptr_t)arg;
    portENTER_CRITICAL(&s_mux);
    const nv_pad_rumble_fn fn = s_slots[slot].seq ? s_slots[slot].rumble : NULL;
    void *ctx = s_slots[slot].rumble_ctx;
    portEXIT_CRITICAL(&s_mux);
    if (fn) fn(ctx, 0, 0);
}

bool nv_pad_rumble(int index, uint16_t low, uint16_t high, uint32_t ms) {
    if (index < 0) return false;
    portENTER_CRITICAL(&s_mux);
    const int s = nth(index);
    const nv_pad_rumble_fn fn = s >= 0 ? s_slots[s].rumble : NULL;
    void *ctx = s >= 0 ? s_slots[s].rumble_ctx : NULL;
    portEXIT_CRITICAL(&s_mux);
    if (!fn) return false;
    if (!s_slots[s].stop) {
        const esp_timer_create_args_t a = { .callback = rumble_stop_cb, .arg = (void *)(intptr_t)s,
                                            .dispatch_method = ESP_TIMER_TASK, .name = "pad_rumble" };
        if (esp_timer_create(&a, &s_slots[s].stop) != ESP_OK) s_slots[s].stop = NULL;
    }
    if (s_slots[s].stop) esp_timer_stop(s_slots[s].stop);
    if (!fn(ctx, low, high)) return false;
    if (ms && (low || high) && s_slots[s].stop) esp_timer_start_once(s_slots[s].stop, (uint64_t)(ms > 10000 ? 10000 : ms) * 1000);
    return true;
}
