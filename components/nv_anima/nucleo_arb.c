// nucleo_arb — the TLS gate (see nucleo_arb.h): one atomic holder id, try-only.
#include "nucleo_arb.h"
#include <stdatomic.h>
#include "esp_log.h"

static const char *TAG = "nucleo_arb";

static atomic_uint s_holder;           // token id of the current holder, 0 = free
static atomic_uint s_next = 1;         // token-id generator (0 is never handed out)

uint32_t nucleo_arb_acquire(const char *job)
{
    unsigned tk = atomic_fetch_add(&s_next, 1);
    if (tk == 0) tk = atomic_fetch_add(&s_next, 1);   // wrapped: skip the reserved 0
    unsigned free_id = 0;
    if (!atomic_compare_exchange_strong(&s_holder, &free_id, tk)) {
        ESP_LOGD(TAG, "busy: %s refused", job ? job : "?");
        return 0;
    }
    ESP_LOGD(TAG, "taken by %s", job ? job : "?");
    return tk;
}

void nucleo_arb_release(uint32_t token)
{
    unsigned held = token;
    if (token) atomic_compare_exchange_strong(&s_holder, &held, 0u);   // only the live holder frees it
}
