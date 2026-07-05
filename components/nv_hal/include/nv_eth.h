// nv_eth — wired Ethernet service for NucleoOS Anima.
//
// The board has an on-board 100 Mbps RJ45: ESP32-P4 internal EMAC (RMII) + IP101 PHY
// (MDC GPIO31, MDIO GPIO52, PHY power/reset GPIO51, external 50 MHz clock in on GPIO50,
// PHY addr 1 — same wiring as the vendor's JC-ESP32P4-M3 reference config). Plug & play:
// init once at boot, DHCP runs whenever a cable with link shows up, and the service nudges
// SNTP the moment an IP lands. Init is NON-FATAL — on any driver error the OS keeps running
// Wi-Fi-only and nv_eth_available() stays false.
//
// Threading: driver/netif events mutate a tiny state block under a spinlock; the UI polls
// nv_eth_generation() and copies via the getters — no cross-thread LVGL access.
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    NV_ETH_OFF = 0,    // driver not installed (init failed or not called)
    NV_ETH_DOWN,       // installed, no link (cable unplugged)
    NV_ETH_LINK,       // link up, waiting for DHCP
    NV_ETH_UP,         // link + IP: online
} nv_eth_state_t;

// Install EMAC+IP101+netif and start the driver. Safe to call once at boot after the
// default event loop exists (or it creates one). Returns false (and logs) on any failure.
bool nv_eth_init(void);

bool nv_eth_available(void);          // driver installed OK
nv_eth_state_t nv_eth_get_state(void);
uint32_t nv_eth_generation(void);     // bumps on every state/IP change (cheap UI poll)

// Copy current IP / MAC ("" when not applicable). Speed in Mbps (0 when no link).
void nv_eth_get_ip(char *out, size_t n);
void nv_eth_get_mac(char *out, size_t n);
int  nv_eth_speed_mbps(void);

#ifdef __cplusplus
}
#endif
