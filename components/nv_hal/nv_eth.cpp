// nv_eth — wired Ethernet (P4 EMAC + IP101). See nv_eth.h for the model.
#include "nv_eth.h"
#include "nv_log.h"
#include "nv_time.h"   // nv_time_notify_online: SNTP nudge when the wire comes up

#include "esp_eth.h"
#include "esp_eth_mac_esp.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_mac.h"
#include "freertos/FreeRTOS.h"

#include <cstring>
#include <cstdio>

static const char *TAG = "eth";

namespace {

// Board wiring (JC-ESP32P4-M3 module, vendor reference config).
constexpr int kMdcGpio   = 31;
constexpr int kMdioGpio  = 52;
constexpr int kPhyRstGpio = 51;  // PHY power/reset line — the driver's reset pulse power-cycles it
constexpr int kClkInGpio = 50;   // external 50 MHz RMII clock feeding the P4
constexpr int kPhyAddr   = 1;

esp_eth_handle_t s_eth = nullptr;
bool             s_available = false;

// Tiny state block, spinlock-guarded: written from the event task, read from the UI thread.
portMUX_TYPE     s_lock = portMUX_INITIALIZER_UNLOCKED;
nv_eth_state_t   s_state = NV_ETH_OFF;
uint32_t         s_gen = 0;
char             s_ip[16] = "";
int              s_speed = 0;

void set_state(nv_eth_state_t st, const char *ip, int speed) {
    portENTER_CRITICAL(&s_lock);
    s_state = st;
    if (ip) { strncpy(s_ip, ip, sizeof(s_ip) - 1); s_ip[sizeof(s_ip) - 1] = '\0'; }
    else s_ip[0] = '\0';
    s_speed = speed;
    s_gen++;
    portEXIT_CRITICAL(&s_lock);
}

void on_eth_event(void *, esp_event_base_t, int32_t id, void *) {
    switch (id) {
        case ETHERNET_EVENT_CONNECTED: {   // link up — DHCP starts now
            eth_speed_t sp = ETH_SPEED_10M;
            esp_eth_ioctl(s_eth, ETH_CMD_G_SPEED, &sp);
            set_state(NV_ETH_LINK, nullptr, sp == ETH_SPEED_100M ? 100 : 10);
            NV_LOGI(TAG, "link up (%d Mbps)", sp == ETH_SPEED_100M ? 100 : 10);
            break;
        }
        case ETHERNET_EVENT_DISCONNECTED:
            set_state(NV_ETH_DOWN, nullptr, 0);
            NV_LOGI(TAG, "link down");
            break;
        default: break;
    }
}

void on_got_ip(void *, esp_event_base_t, int32_t, void *data) {
    auto *e = static_cast<ip_event_got_ip_t *>(data);
    char ip[16];
    snprintf(ip, sizeof(ip), IPSTR, IP2STR(&e->ip_info.ip));
    int speed;
    portENTER_CRITICAL(&s_lock);
    speed = s_speed;
    portEXIT_CRITICAL(&s_lock);
    set_state(NV_ETH_UP, ip, speed);
    NV_LOGI(TAG, "got IP %s", ip);
    nv_time_notify_online();   // wired network is online — sync the clock now
}

// DHCP lease lost while the link stays up (renewal failed after a network reconfig):
// without this the state would stay NV_ETH_UP advertising a dead address forever.
void on_lost_ip(void *, esp_event_base_t, int32_t, void *) {
    int speed;
    portENTER_CRITICAL(&s_lock);
    speed = s_speed;
    portEXIT_CRITICAL(&s_lock);
    set_state(NV_ETH_LINK, nullptr, speed);
    NV_LOGI(TAG, "lost IP (link still up)");
}

}  // namespace

bool nv_eth_init(void) {
    // netif/event-loop are shared with Wi-Fi; both calls are idempotent-safe.
    esp_netif_init();
    esp_event_loop_create_default();   // ESP_ERR_INVALID_STATE when Wi-Fi made it first: fine

    eth_mac_config_t mac_cfg = ETH_MAC_DEFAULT_CONFIG();
    eth_esp32_emac_config_t emac_cfg = ETH_ESP32_EMAC_DEFAULT_CONFIG();
    emac_cfg.smi_gpio.mdc_num  = kMdcGpio;
    emac_cfg.smi_gpio.mdio_num = kMdioGpio;
    emac_cfg.clock_config.rmii.clock_mode = EMAC_CLK_EXT_IN;
    emac_cfg.clock_config.rmii.clock_gpio = (emac_rmii_clock_gpio_t)kClkInGpio;
    esp_eth_mac_t *mac = esp_eth_mac_new_esp32(&emac_cfg, &mac_cfg);

    eth_phy_config_t phy_cfg = ETH_PHY_DEFAULT_CONFIG();
    phy_cfg.phy_addr = kPhyAddr;
    phy_cfg.reset_gpio_num = kPhyRstGpio;
    esp_eth_phy_t *phy = esp_eth_phy_new_ip101(&phy_cfg);

    if (!mac || !phy) {   // free whichever half made it (MAC holds DMA + ISR + rx task)
        if (mac) mac->del(mac);
        if (phy) phy->del(phy);
        NV_LOGW(TAG, "MAC/PHY alloc failed — Ethernet disabled");
        return false;
    }

    esp_eth_config_t cfg = ETH_DEFAULT_CONFIG(mac, phy);
    if (esp_eth_driver_install(&cfg, &s_eth) != ESP_OK) {
        // Wrong PHY variant / no PHY response: keep the OS alive, Wi-Fi still works.
        NV_LOGW(TAG, "driver install failed — Ethernet disabled");
        mac->del(mac);
        phy->del(phy);
        s_eth = nullptr;
        return false;
    }

    // esp_netif_attach dereferences both args unconditionally — NULLs here (boot-time OOM)
    // must bail non-fatally, never panic.
    esp_netif_config_t netif_cfg = ESP_NETIF_DEFAULT_ETH();
    esp_netif_t *netif = esp_netif_new(&netif_cfg);
    esp_eth_netif_glue_handle_t glue = netif ? esp_eth_new_netif_glue(s_eth) : nullptr;
    if (!netif || !glue || esp_netif_attach(netif, glue) != ESP_OK) {
        NV_LOGW(TAG, "netif setup failed — Ethernet disabled");
        if (glue) esp_eth_del_netif_glue(glue);
        if (netif) esp_netif_destroy(netif);
        esp_eth_driver_uninstall(s_eth);
        s_eth = nullptr;
        mac->del(mac);
        phy->del(phy);
        return false;
    }

    esp_event_handler_register(ETH_EVENT, ESP_EVENT_ANY_ID, on_eth_event, nullptr);
    esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_GOT_IP, on_got_ip, nullptr);
    esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_LOST_IP, on_lost_ip, nullptr);

    // Seed DOWN *before* start: a link-up event that fires mid-start now always lands after
    // this write, so it can never be clobbered back to DOWN.
    set_state(NV_ETH_DOWN, nullptr, 0);
    s_available = true;

    if (esp_eth_start(s_eth) != ESP_OK) {
        // Symmetric teardown — this path is survivable by design, not a slow leak.
        NV_LOGW(TAG, "start failed — Ethernet disabled");
        s_available = false;
        esp_event_handler_unregister(ETH_EVENT, ESP_EVENT_ANY_ID, on_eth_event);
        esp_event_handler_unregister(IP_EVENT, IP_EVENT_ETH_GOT_IP, on_got_ip);
        esp_event_handler_unregister(IP_EVENT, IP_EVENT_ETH_LOST_IP, on_lost_ip);
        esp_eth_del_netif_glue(glue);
        esp_netif_destroy(netif);
        esp_eth_driver_uninstall(s_eth);
        s_eth = nullptr;
        mac->del(mac);
        phy->del(phy);
        set_state(NV_ETH_OFF, nullptr, 0);
        return false;
    }
    NV_LOGI(TAG, "ready (IP101 @%d, MDC%d/MDIO%d, ext 50MHz on GPIO%d) — waiting for cable",
            kPhyAddr, kMdcGpio, kMdioGpio, kClkInGpio);
    return true;
}

bool nv_eth_available(void) { return s_available; }

nv_eth_state_t nv_eth_get_state(void) {
    portENTER_CRITICAL(&s_lock);
    const nv_eth_state_t st = s_state;
    portEXIT_CRITICAL(&s_lock);
    return st;
}

uint32_t nv_eth_generation(void) {
    portENTER_CRITICAL(&s_lock);
    const uint32_t g = s_gen;
    portEXIT_CRITICAL(&s_lock);
    return g;
}

void nv_eth_get_ip(char *out, size_t n) {
    if (!out || n == 0) return;
    portENTER_CRITICAL(&s_lock);
    strncpy(out, s_ip, n - 1);
    portEXIT_CRITICAL(&s_lock);
    out[n - 1] = '\0';
}

void nv_eth_get_mac(char *out, size_t n) {
    if (!out || n == 0) return;
    out[0] = '\0';
    uint8_t m[6];
    if (s_available && esp_read_mac(m, ESP_MAC_ETH) == ESP_OK)
        snprintf(out, n, "%02X:%02X:%02X:%02X:%02X:%02X", m[0], m[1], m[2], m[3], m[4], m[5]);
}

int nv_eth_speed_mbps(void) {
    portENTER_CRITICAL(&s_lock);
    const int sp = s_speed;
    portEXIT_CRITICAL(&s_lock);
    return sp;
}
