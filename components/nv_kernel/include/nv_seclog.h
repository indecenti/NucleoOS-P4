// nv_seclog — security event log: what an owner (or a support technician) needs to see after the
// fact — failed and successful pairings, lockouts, revoked sessions, refused firmware and apps, the
// state of the settings encryption. A RAM ring of the last NV_SECLOG_MAX events (cleared by a
// reboot); shown in Settings → Security and served at GET /api/security/events (paired clients).
// Details are labels only (device names, app ids, versions): never put a secret in one.
#pragma once
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NV_SECLOG_MAX        32
#define NV_SECLOG_DETAIL_MAX 40   // incl. NUL

typedef enum {
    NV_SEC_PAIR_WRONG = 0,    // a wrong pairing code was entered
    NV_SEC_PAIR_LOCKED,       // too many wrong codes: pairing locked for a while
    NV_SEC_PAIR_OK,           // a new client was paired (detail: its name)
    NV_SEC_SESSION_REVOKED,   // a paired client was revoked from the device (detail: name or "all")
    NV_SEC_FW_REFUSED,        // a firmware image was refused (detail: why / version)
    NV_SEC_APP_REFUSED,       // a store package was refused (detail: app id)
    NV_SEC_NVS_ENCRYPTED,     // the settings store was encrypted this boot (first-boot migration)
    NV_SEC_NVS_PLAIN,         // settings encryption unavailable: the store stays plaintext
    NV_SEC_UNLOCK_LOCKED,     // lock screen: too many wrong PINs, unlocking paused
    NV_SEC_EVENT_COUNT
} nv_sec_event_t;

typedef struct {
    uint32_t unix_time;       // 0 when the clock was not set yet
    uint32_t uptime_s;
    uint8_t  code;            // nv_sec_event_t
    char     detail[NV_SECLOG_DETAIL_MAX];
} nv_sec_entry_t;

// Record an event. Any task, any time (also before nv_time is up); never touches flash.
void nv_seclog_add(nv_sec_event_t code, const char *detail);
int  nv_seclog_count(void);
// index 0 = newest. false when out of range.
bool nv_seclog_get(int index, nv_sec_entry_t *out);
// Bumped on every add: UIs poll it to refresh.
uint32_t nv_seclog_generation(void);
// Stable machine id for the API ("pair_wrong", ...).
const char *nv_seclog_code_id(nv_sec_event_t code);

#ifdef __cplusplus
}
#endif
