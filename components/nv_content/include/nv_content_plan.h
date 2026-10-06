// nv_content_plan — the decisions of the content service (nv_content.h), pure code: host unit tests in
// tests/host (target "content"). What a pack's state is, whether it is recommended for the owner's
// language, and the one case where the device installs without being asked.
#pragma once
#include <stdint.h>

namespace nv_content_plan {

struct Pack {
    const char *id;
    const char *avail;          // version the store offers ("" = not in the index / index not loaded)
    const char *installed;      // installed version ("" = not installed)
    const char *langs;          // UI languages it is recommended for: "*" = all, "it,es", "" = none
    const char *required_min;   // version THIS firmware needs ("" = not required)
};

enum State { MISSING, OK, UPDATE, UNAVAILABLE };

// a > b, comparing up to four dot-separated numbers ("2026.10.2" > "2026.10.1", "2026.11" > "2026.10.9").
bool newer(const char *a, const char *b);

// Installed and current = OK; installed, the store has newer = UPDATE; not installed and offered =
// MISSING; not installed and not offered = UNAVAILABLE.
State state(const Pack &p);

// Recommended for UI language `lang` ("it"): listed for every language ("*") or for that one.
bool recommended(const Pack &p, const char *lang);

// The device updates on its own ONLY a pack that is installed and older than what this firmware
// requires, and only when the store offers a version that satisfies the requirement. A web companion
// changed on the device itself (web_local) is never overwritten. Everything else waits for the owner.
bool auto_update(const Pack &p, bool web_local);

// This firmware needs a version the store doesn't offer yet (published out of order): keep working,
// retry later, never loop.
bool requirement_unmet(const Pack &p);

// Retry delay after `failures` failed attempts in a row: 1, 5, 15, then 60 minutes (seconds).
uint32_t backoff_s(int failures);

}  // namespace nv_content_plan
