// nucleo_setup.h — NucleoV2 shim: the Anima online tier gates every network fetch on the
// station IP. Backed by nv_wifi (esp-hosted / C6) instead of the Cardputer setup module.
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

// Current STA IPv4 as a dotted string, or "" when not associated. Static storage.
const char *nucleo_setup_ip(void);

#ifdef __cplusplus
}
#endif
