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
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_timer.h"
#include "esp_rom_sys.h"
#include "nv_bgwork.h"

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

// ---- driver on demand ----------------------------------------------------------------------------
// The EMAC driver costs ~20 KB of internal SRAM (DMA rings + rx task) and almost nobody has a cable
// in. So it only runs while a cable is in: a 2 s esp_timer reads the PHY link bit by bit-banging
// MDIO (no driver, no RAM), starts the driver when the link appears and stops it again after the
// cable has been out for ~10 s. Start/stop run on nv_bgwork (they allocate and can take a while).

constexpr int kPollMs      = 2000;
constexpr int kDownPolls   = 5;      // cable out this many polls in a row -> driver off
constexpr uint16_t kIp101Id1 = 0x0243;

esp_eth_mac_t               *s_mac = nullptr;
esp_eth_phy_t               *s_phy = nullptr;
esp_netif_t                 *s_netif = nullptr;   // kept for life: mDNS holds the ETH netif
esp_eth_netif_glue_handle_t  s_glue = nullptr;
esp_timer_handle_t           s_poll = nullptr;
volatile bool                s_running = false;   // driver installed + started
volatile bool                s_busy = false;      // a start/stop job is queued or running
int                          s_down_polls = 0;

// --- MDIO bit-bang (IEEE 802.3 clause 22), used only while the driver is off ---
void mdio_pins_gpio(void) {
    gpio_set_direction((gpio_num_t)kMdcGpio, GPIO_MODE_OUTPUT);
    gpio_set_level((gpio_num_t)kMdcGpio, 0);
    gpio_set_direction((gpio_num_t)kMdioGpio, GPIO_MODE_INPUT);
    gpio_set_pull_mode((gpio_num_t)kMdioGpio, GPIO_PULLUP_ONLY);
    gpio_set_direction((gpio_num_t)kPhyRstGpio, GPIO_MODE_OUTPUT);
    gpio_set_level((gpio_num_t)kPhyRstGpio, 1);   // PHY out of reset/powered
}
inline void mdc_pulse(void) {
    gpio_set_level((gpio_num_t)kMdcGpio, 1); esp_rom_delay_us(1);
    gpio_set_level((gpio_num_t)kMdcGpio, 0); esp_rom_delay_us(1);
}
void mdio_out_bits(uint32_t v, int n) {
    for (int i = n - 1; i >= 0; i--) {
        gpio_set_level((gpio_num_t)kMdioGpio, (v >> i) & 1);
        esp_rom_delay_us(1);
        mdc_pulse();
    }
}
uint16_t mdio_read(int reg) {
    gpio_set_direction((gpio_num_t)kMdioGpio, GPIO_MODE_OUTPUT);
    mdio_out_bits(0xFFFFFFFFu, 32);                                       // preamble
    mdio_out_bits((0x1u << 12) | (0x2u << 10) | ((kPhyAddr & 31) << 5) | (reg & 31), 14);   // ST OP PHY REG
    gpio_set_direction((gpio_num_t)kMdioGpio, GPIO_MODE_INPUT);
    mdc_pulse(); mdc_pulse();                                             // turnaround
    uint16_t v = 0;
    for (int i = 0; i < 16; i++) {
        v = (uint16_t)((v << 1) | (gpio_get_level((gpio_num_t)kMdioGpio) & 1));
        mdc_pulse();
    }
    return v;
}
bool phy_link_up(void) {
    mdio_read(1);                       // BMSR link bit latches low: first read clears it
    return (mdio_read(1) & 0x0004) != 0;
}

bool driver_start(void) {
    eth_mac_config_t mac_cfg = ETH_MAC_DEFAULT_CONFIG();
    eth_esp32_emac_config_t emac_cfg = ETH_ESP32_EMAC_DEFAULT_CONFIG();
    emac_cfg.smi_gpio.mdc_num  = kMdcGpio;
    emac_cfg.smi_gpio.mdio_num = kMdioGpio;
    emac_cfg.clock_config.rmii.clock_mode = EMAC_CLK_EXT_IN;
    emac_cfg.clock_config.rmii.clock_gpio = (emac_rmii_clock_gpio_t)kClkInGpio;
    s_mac = esp_eth_mac_new_esp32(&emac_cfg, &mac_cfg);

    eth_phy_config_t phy_cfg = ETH_PHY_DEFAULT_CONFIG();
    phy_cfg.phy_addr = kPhyAddr;
    phy_cfg.reset_gpio_num = kPhyRstGpio;
    s_phy = esp_eth_phy_new_ip101(&phy_cfg);

    esp_eth_config_t cfg = ETH_DEFAULT_CONFIG(s_mac, s_phy);
    if (!s_mac || !s_phy || esp_eth_driver_install(&cfg, &s_eth) != ESP_OK) {
        NV_LOGW(TAG, "driver install failed");
        goto fail;
    }
    s_glue = esp_eth_new_netif_glue(s_eth);
    if (!s_glue || esp_netif_attach(s_netif, s_glue) != ESP_OK) {
        NV_LOGW(TAG, "netif attach failed");
        goto fail;
    }
    set_state(NV_ETH_DOWN, nullptr, 0);   // seed before start: link-up mid-start lands after
    if (esp_eth_start(s_eth) != ESP_OK) {
        NV_LOGW(TAG, "start failed");
        goto fail;
    }
    s_running = true;
    NV_LOGI(TAG, "cable in -> driver up");
    return true;
fail:
    if (s_glue) { esp_eth_del_netif_glue(s_glue); s_glue = nullptr; }
    if (s_eth) { esp_eth_driver_uninstall(s_eth); s_eth = nullptr; }
    if (s_mac) { s_mac->del(s_mac); s_mac = nullptr; }
    if (s_phy) { s_phy->del(s_phy); s_phy = nullptr; }
    set_state(NV_ETH_OFF, nullptr, 0);
    mdio_pins_gpio();
    return false;
}

void driver_stop(void) {
    s_running = false;
    esp_eth_stop(s_eth);
    esp_eth_del_netif_glue(s_glue); s_glue = nullptr;
    esp_eth_driver_uninstall(s_eth); s_eth = nullptr;
    s_mac->del(s_mac); s_mac = nullptr;
    s_phy->del(s_phy); s_phy = nullptr;
    set_state(NV_ETH_OFF, nullptr, 0);
    mdio_pins_gpio();
    NV_LOGI(TAG, "cable out -> driver off (RAM released)");
}

void job_start(void *) { driver_start(); s_down_polls = 0; s_busy = false; }
void job_stop(void *) { driver_stop(); s_busy = false; }

void poll_cb(void *) {
    if (s_busy) return;
    if (!s_running) {
        if (!phy_link_up()) return;
        s_busy = true;
        if (!nv_bgwork_submit(job_start, nullptr)) s_busy = false;
        return;
    }
    if (nv_eth_get_state() != NV_ETH_DOWN) { s_down_polls = 0; return; }
    if (++s_down_polls < kDownPolls) return;
    s_busy = true;
    if (!nv_bgwork_submit(job_stop, nullptr)) s_busy = false;
}

}  // namespace

bool nv_eth_init(void) {
    // netif/event-loop are shared with Wi-Fi; both calls are idempotent-safe.
    esp_netif_init();
    esp_event_loop_create_default();   // ESP_ERR_INVALID_STATE when Wi-Fi made it first: fine

    // Is the PHY there at all? (no driver needed: MDIO by hand)
    mdio_pins_gpio();
    vTaskDelay(pdMS_TO_TICKS(10));
    const uint16_t id1 = mdio_read(2);
    if (id1 != kIp101Id1) {
        NV_LOGW(TAG, "no IP101 PHY (id 0x%04x) - Ethernet disabled", id1);
        return false;
    }
    esp_netif_config_t netif_cfg = ESP_NETIF_DEFAULT_ETH();
    s_netif = esp_netif_new(&netif_cfg);
    if (!s_netif) { NV_LOGW(TAG, "netif alloc failed - Ethernet disabled"); return false; }

    esp_event_handler_register(ETH_EVENT, ESP_EVENT_ANY_ID, on_eth_event, nullptr);
    esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_GOT_IP, on_got_ip, nullptr);
    esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_LOST_IP, on_lost_ip, nullptr);

    const esp_timer_create_args_t ta = { .callback = poll_cb, .arg = nullptr,
                                         .dispatch_method = ESP_TIMER_TASK, .name = "eth_poll",
                                         .skip_unhandled_events = true };
    if (esp_timer_create(&ta, &s_poll) != ESP_OK) return false;
    esp_timer_start_periodic(s_poll, kPollMs * 1000ULL);
    s_available = true;
    NV_LOGI(TAG, "IP101 found - driver starts when a cable is plugged in");
    poll_cb(nullptr);   // cable already in at boot: start now
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
