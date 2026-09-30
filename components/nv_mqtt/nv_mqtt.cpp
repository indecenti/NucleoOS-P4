// nv_mqtt — Home Assistant over MQTT. Contract in the header; payload parsing in nv_ha_proto.
#include "nv_mqtt.h"
#include "nv_ha_proto.h"
#include "nv_mqtt_topic.h"

#include <string.h>
#include <strings.h>   // strcasecmp
#include <stdio.h>
#include <atomic>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/idf_additions.h"
#include "freertos/ringbuf.h"
#include "esp_heap_caps.h"
#include "esp_mac.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "mqtt_client.h"
#include "mdns.h"
#include "lwip/ip4_addr.h"
#include "esp_lvgl_port.h"

#include "nv_log.h"
#include "nv_config.h"
#include "nv_event_bus.h"
#include "nv_mem_attr.h"
#include "nv_wifi.h"
#include "nv_eth.h"
#include "nv_hal.h"
#include "nv_audio.h"
#include "nv_ui.h"
#include "nv_notify.h"
#include "nv_app.h"
#include "nv_i18n.h"
#include "nv_tts.h"
#include "nv_sysmon.h"
#include "nv_ota.h"

static const char *TAG = "mqtt";

namespace {

constexpr const char *kModel  = "JC1060P470C";
constexpr const char *kPrefix = "homeassistant";   // HA's default discovery prefix
constexpr int kDiagPeriodMs   = 60 * 1000;
constexpr int kUiPollMs       = 1000;
constexpr int kCmdQueueLen    = 8;
constexpr int kAppOptsCap     = 1024;              // JSON array of app ids for the select
constexpr int kCfgBufCap      = 2048;              // one discovery payload

// Dirty bits: state topics to (re)publish from the service task.
enum : uint32_t {
    D_SCREEN = 1u << 0, D_VOLUME = 1u << 1, D_MUTE = 1u << 2, D_DND = 1u << 3,
    D_APP = 1u << 4, D_LOCKED = 1u << 5, D_TOUCH = 1u << 6, D_DIAG = 1u << 7,
    D_ALL = 0xFFu,
};

struct Cmd {
    ha_entity_t ent;
    uint16_t    len;
    char        payload[HA_TEXT_MAX + 56];   // notify JSON = text + title + keys
};

TaskHandle_t           s_task    = nullptr;
QueueHandle_t          s_q       = nullptr;
esp_mqtt_client_handle_t s_client = nullptr;
std::atomic<bool>      s_enabled{false};
std::atomic<bool>      s_connected{false};
std::atomic<bool>      s_need_disc{false};     // publish discovery + all states
std::atomic<uint32_t>  s_dirty{0};
std::atomic<uint32_t>  s_cfg_gen{0};           // bumps on broker settings -> reconnect
std::atomic<int>       s_state{NV_MQTT_OFF};
char                   s_node[HA_NODE_MAX] = "";
char                   s_detail[80] = "";      // guarded loosely: written by service/mqtt tasks
NV_PSRAM_BSS char      s_cfgbuf[kCfgBufCap];   // service task only
NV_PSRAM_BSS char      s_opts[kAppOptsCap];

// Last published UI state (service task only).
struct Pub { bool asleep, locked, touch; char app[32]; bool valid; } s_pub = {};

void set_state(nv_mqtt_state_t st, const char *detail)
{
    s_state.store(st);
    snprintf(s_detail, sizeof s_detail, "%s", detail ? detail : "");
}

bool net_up(void)
{
    return nv_wifi_get_state() == NV_WIFI_CONNECTED || nv_eth_get_state() == NV_ETH_UP;
}

bool italian(void) { return nv_i18n_get_lang() == NV_LANG_IT; }

// ---------------------------------------------------------------- publish helpers
void pub(const char *topic, const char *data, bool retain)
{
    if (!s_client || !s_connected.load()) return;
    esp_mqtt_client_publish(s_client, topic, data, 0, retain ? 1 : 0, retain ? 1 : 0);
}

void pub_state(const char *obj, const char *data)
{
    char t[96];
    if (ha_topic(t, sizeof t, s_node, obj, "state")) pub(t, data, true);
}

// Foreground app list for the select entity: ["home","calc",...] (registry read under the lock).
void build_app_options(void)
{
    size_t o = (size_t)snprintf(s_opts, sizeof s_opts, "[\"home\"");
    if (lvgl_port_lock(1000)) {
        for (int i = 0; i < nv_app_count(); i++) {
            const NvApp *a = nv_app_at(i);
            if (!a || !a->id || !a->id[0]) continue;
            char id[48];
            ha_json_escape(id, sizeof id, a->id);
            const size_t need = strlen(id) + 4;
            if (o + need + 2 >= sizeof s_opts) break;
            o += (size_t)snprintf(s_opts + o, sizeof s_opts - o, ",\"%s\"", id);
        }
        lvgl_port_unlock();
    }
    snprintf(s_opts + o, sizeof s_opts - o, "]");
}

void publish_discovery(void)
{
    build_app_options();
    const bool it = italian();
    const char *sw = nv_ota_running_version();
    char topic[128];
    for (int e = 0; e < HA_E_COUNT; e++) {
        if (!ha_config_topic(topic, sizeof topic, kPrefix, s_node, (ha_entity_t)e)) continue;
        if (!ha_build_config(s_cfgbuf, sizeof s_cfgbuf, (ha_entity_t)e, s_node, it, sw, kModel, s_opts)) {
            NV_LOGW(TAG, "discovery payload too big for entity %d", e);
            continue;
        }
        pub(topic, s_cfgbuf, true);
    }
    char st[64];
    snprintf(st, sizeof st, "nucleo/%s/status", s_node);
    pub(st, "online", true);
    NV_LOGI(TAG, "discovery published (%d entities)", (int)HA_E_COUNT);
}

// Mqtt switched off from Settings while connected: remove the device from HA instead of leaving
// a ghost of "unavailable" entities (empty retained config = delete).
void publish_forget(void)
{
    char topic[128];
    for (int e = 0; e < HA_E_COUNT; e++)
        if (ha_config_topic(topic, sizeof topic, kPrefix, s_node, (ha_entity_t)e)) pub(topic, "", true);
    char st[64];
    snprintf(st, sizeof st, "nucleo/%s/status", s_node);
    pub(st, "offline", true);
}

void publish_diag(void)
{
    nv_sys_perf_t perf;
    nv_sys_mem_t mem;
    nv_sysmon_perf(&perf);
    nv_sysmon_mem(&mem);
    int rssi = 0;
    char ip[16] = "";
    nv_wifi_link_t link;
    if (nv_wifi_get_state() == NV_WIFI_CONNECTED && nv_wifi_get_link(&link)) {
        rssi = link.rssi;
        snprintf(ip, sizeof ip, "%s", link.ip);
    } else if (nv_eth_get_state() == NV_ETH_UP) {
        nv_eth_get_ip(ip, sizeof ip);
    }
    char temp[16], rs[8];
    if (perf.temp_valid) snprintf(temp, sizeof temp, "%.1f", (double)perf.temp_c);
    else snprintf(temp, sizeof temp, "null");
    if (rssi) snprintf(rs, sizeof rs, "%d", rssi);
    else snprintf(rs, sizeof rs, "null");
    char b[192];
    snprintf(b, sizeof b,
             "{\"temp\":%s,\"rssi\":%s,\"uptime\":%llu,\"sram\":%u,\"psram\":%u,\"ip\":\"%s\"}",
             temp, rs, (unsigned long long)perf.uptime_s,
             (unsigned)(mem.internal.free_bytes / 1024), (unsigned)(mem.psram.free_bytes / 1024), ip);
    char t[64];
    snprintf(t, sizeof t, "nucleo/%s/diag", s_node);
    pub(t, b, false);
}

// Read panel state the UI owns. Short lock: skip this round rather than stall on a busy UI.
bool read_ui(Pub *p)
{
    if (!lvgl_port_lock(50)) return false;
    p->asleep = nv_ui_screen_is_asleep();
    p->locked = nv_ui_is_locked();
    p->touch  = lv_display_get_inactive_time(nullptr) < HA_TOUCH_WINDOW_S * 1000u;
    lvgl_port_unlock();
    const char *app = nv_ui_current_app_id();     // stable literal, lock-free read (see nv_web)
    snprintf(p->app, sizeof p->app, "%s", (app && app[0]) ? app : "home");
    p->valid = true;
    return true;
}

void publish_dirty(uint32_t d)
{
    char b[96];
    if (d & D_SCREEN) {
        snprintf(b, sizeof b, "{\"state\":\"%s\",\"brightness\":%d,\"color_mode\":\"brightness\"}",
                 s_pub.asleep ? "OFF" : "ON", nv_config_get_int("brightness", 90));
        pub_state("screen", b);
    }
    if (d & D_VOLUME) { snprintf(b, sizeof b, "%d", nv_config_get_int("volume", 60)); pub_state("volume", b); }
    if (d & D_MUTE)   pub_state("mute", nv_config_get_bool("mute", false) ? "ON" : "OFF");
    if (d & D_DND)    pub_state("dnd", nv_config_get_bool("qs_dnd", false) ? "ON" : "OFF");
    if (d & D_APP)    pub_state("app", s_pub.app);
    if (d & D_LOCKED) pub_state("locked", s_pub.locked ? "ON" : "OFF");
    if (d & D_TOUCH)  pub_state("touch", s_pub.touch ? "ON" : "OFF");
    if (d & D_DIAG)   publish_diag();
}

// ---------------------------------------------------------------- command execution
int64_t s_talk_tokens_at = 0;   // simple rate limit for notify/say: 1 per second, burst 5
int     s_talk_tokens    = 5;

bool talk_allowed(void)
{
    const int64_t now = esp_timer_get_time() / 1000;
    const int refill = (int)((now - s_talk_tokens_at) / 1000);
    if (refill > 0) {
        s_talk_tokens = s_talk_tokens + refill > 5 ? 5 : s_talk_tokens + refill;
        s_talk_tokens_at = now;
    }
    if (s_talk_tokens <= 0) return false;
    s_talk_tokens--;
    return true;
}

void screen_power(bool on)
{
    if (!lvgl_port_lock(500)) return;
    if (on) nv_ui_screen_wake();
    else    nv_ui_screen_sleep();
    lvgl_port_unlock();
}

void exec(const Cmd &c)
{
    const char *p = c.payload;
    const size_t n = c.len;
    bool on;
    int v;
    switch (c.ent) {
    case HA_E_SCREEN: {
        char st[8];
        int bri;
        const bool has_bri = ha_json_get_int(p, n, "brightness", &bri);
        const bool has_st  = ha_json_get(p, n, "state", st, sizeof st) && ha_parse_onoff(st, strlen(st), &on);
        if (has_bri) {
            bri = bri < 1 ? 1 : bri > 100 ? 100 : bri;
            nv_config_set_int("brightness", bri);           // publishes D_SCREEN via on_setting
        }
        if (has_st && !on) { screen_power(false); }
        else if (has_st || has_bri) {
            screen_power(true);
            if (has_bri) nv_hal_backlight_set(bri);         // wake restored the old level first
        }
        s_dirty.fetch_or(D_SCREEN);
        break;
    }
    case HA_E_VOLUME:
        if (ha_parse_int(p, n, &v)) {
            v = v < 0 ? 0 : v > 100 ? 100 : v;
            nv_audio_set_volume(v);
            nv_config_set_int("volume", v);
        }
        break;
    case HA_E_MUTE:
        if (ha_parse_onoff(p, n, &on)) { nv_audio_set_mute(on); nv_config_set_bool("mute", on); }
        break;
    case HA_E_DND:
        if (ha_parse_onoff(p, n, &on)) nv_config_set_bool("qs_dnd", on);
        break;
    case HA_E_NOTIFY: {
        if (!talk_allowed()) { NV_LOGW(TAG, "notify rate-limited"); break; }
        char title[40] = "Home Assistant", msg[HA_TEXT_MAX];
        char raw[HA_TEXT_MAX];
        if (n && p[0] == '{') {
            if (ha_json_get(p, n, "title", raw, sizeof raw) && raw[0])
                ha_payload_text(raw, strlen(raw), title, sizeof title);
            if (!ha_json_get(p, n, "message", raw, sizeof raw)) raw[0] = '\0';
            ha_payload_text(raw, strlen(raw), msg, sizeof msg);
        } else {
            ha_payload_text(p, n, msg, 96 + 1);             // the ring stores 96 chars anyway
        }
        if (!msg[0]) break;
        if (lvgl_port_lock(500)) {
            nv_notify_post(NV_NOTE_INFO, title, msg);
            lvgl_port_unlock();
        }
        break;
    }
    case HA_E_SAY: {
        if (!talk_allowed()) { NV_LOGW(TAG, "say rate-limited"); break; }
        char msg[HA_TEXT_MAX];
        if (ha_payload_text(p, n, msg, sizeof msg)) nv_tts_say(msg, nullptr);
        break;
    }
    case HA_E_APP: {
        char id[40];
        ha_payload_text(p, n, id, sizeof id);
        if (!id[0]) break;
        if (!strcmp(id, "home")) nv_ui_go_home_async();
        else if (!nv_ui_open_app_id_async(id)) NV_LOGW(TAG, "open '%s' refused", id);
        s_pub.valid = false;                                // re-read + republish soon
        break;
    }
    case HA_E_HOME:
        nv_ui_go_home_async();
        break;
    case HA_E_LOCK:
        if (lvgl_port_lock(500)) { nv_ui_lock(); lvgl_port_unlock(); }
        break;
    case HA_E_REBOOT: {
        NV_LOGW(TAG, "restart requested from Home Assistant");
        char st[64];
        snprintf(st, sizeof st, "nucleo/%s/status", s_node);
        pub(st, "offline", true);
        vTaskDelay(pdMS_TO_TICKS(800));
        esp_restart();
        break;
    }
    default:
        break;
    }
}

// ---------------------------------------------------------------- ABI v12 app bridge
// The app's thread never touches the esp-mqtt client: it edits the filter table (s_app_mx, never
// held across a client call, so the esp-mqtt task can take it from its event handler) and queues
// SUB/UNSUB/PUB items that the service task — the client's only owner — drains. Incoming
// messages matching a filter are reassembled (esp-mqtt hands >1 KB payloads over in fragments)
// and queued in s_in for nv_mqtt_app_recv.
enum : uint8_t { OUT_SUB = 1, OUT_UNSUB, OUT_PUB };
constexpr size_t kInRing  = 32 * 1024;
constexpr size_t kOutRing = 16 * 1024;
SemaphoreHandle_t s_app_mx = nullptr;
NV_PSRAM_BSS char s_app_filt[NV_MQTT_APP_FILTERS][128];   // any task; cold
std::atomic<int>  s_app_nf{0};
RingbufHandle_t   s_in = nullptr, s_out = nullptr;
NV_PSRAM_BSS char    s_frag_topic[128];
NV_PSRAM_BSS uint8_t s_frag[NV_MQTT_APP_MSG_MAX];
int                  s_frag_len = -1;                  // mqtt task only; -1 = not collecting
int                  s_frag_tl  = 0;

bool app_rings(void)
{
    if (!s_in)  s_in  = xRingbufferCreateWithCaps(kInRing, RINGBUF_TYPE_NOSPLIT, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_out) s_out = xRingbufferCreateWithCaps(kOutRing, RINGBUF_TYPE_NOSPLIT, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return s_in && s_out;
}

bool app_wanted(const char *topic, size_t tl)
{
    if (s_app_nf.load() == 0 || !s_app_mx) return false;
    bool hit = false;
    xSemaphoreTake(s_app_mx, portMAX_DELAY);
    for (int i = 0; i < s_app_nf.load() && !hit; i++) hit = np_mqtt_match(s_app_filt[i], topic, tl);
    xSemaphoreGive(s_app_mx);
    return hit;
}

// In-ring item: [u8 topic len][topic][payload]
void app_push_in(const char *topic, size_t tl, const uint8_t *p, size_t pl)
{
    if (!s_in || tl == 0 || tl > 127) return;
    void *slot = nullptr;
    if (xRingbufferSendAcquire(s_in, &slot, 1 + tl + pl, 0) != pdTRUE || !slot) return;   // full: drop
    uint8_t *b = static_cast<uint8_t *>(slot);
    b[0] = (uint8_t)tl;
    memcpy(b + 1, topic, tl);
    if (pl) memcpy(b + 1 + tl, p, pl);
    xRingbufferSendComplete(s_in, slot);
}

// Out-ring item: [type][retain][u8 topic len][topic][payload]
bool app_push_out(uint8_t type, const char *topic, const void *p, size_t pl, bool retain)
{
    const size_t tl = strnlen(topic, 128);
    void *slot = nullptr;
    if (!s_out || xRingbufferSendAcquire(s_out, &slot, 3 + tl + pl, 0) != pdTRUE || !slot) return false;
    uint8_t *b = static_cast<uint8_t *>(slot);
    b[0] = type;
    b[1] = retain ? 1 : 0;
    b[2] = (uint8_t)tl;
    memcpy(b + 3, topic, tl);
    if (pl) memcpy(b + 3 + tl, p, pl);
    xRingbufferSendComplete(s_out, slot);
    if (s_task) xTaskNotifyGive(s_task);
    return true;
}

// Service task: carry out what the app queued (dropped while offline except subscriptions,
// which the CONNECTED handler replays from the table anyway).
void app_drain_out(void)
{
    size_t n = 0;
    uint8_t *b;
    while (s_out && (b = static_cast<uint8_t *>(xRingbufferReceive(s_out, &n, 0))) != nullptr) {
        if (n >= 3 && (size_t)3 + b[2] <= n && s_client && s_connected.load()) {
            char topic[128];
            memcpy(topic, b + 3, b[2]);
            topic[b[2]] = '\0';
            const int pl = (int)(n - 3 - b[2]);
            if (b[0] == OUT_SUB)        esp_mqtt_client_subscribe(s_client, topic, 0);
            else if (b[0] == OUT_UNSUB) esp_mqtt_client_unsubscribe(s_client, topic);
            else if (b[0] == OUT_PUB)
                esp_mqtt_client_publish(s_client, topic, reinterpret_cast<const char *>(b + 3 + b[2]), pl, 0, b[1]);
        }
        vRingbufferReturnItem(s_out, b);
    }
}

// esp-mqtt task, MQTT_EVENT_DATA: deliver (or reassemble) a message an app filter wants.
void app_on_data(esp_mqtt_event_handle_t ev)
{
    if (s_app_nf.load() == 0) { s_frag_len = -1; return; }
    const bool first = ev->current_data_offset == 0;
    if (first) {
        s_frag_len = -1;
        if (ev->topic_len <= 0 || ev->topic_len > 127 || ev->total_data_len > NV_MQTT_APP_MSG_MAX ||
            !app_wanted(ev->topic, (size_t)ev->topic_len))
            return;
        if (ev->data_len == ev->total_data_len) {                      // whole message at once
            app_push_in(ev->topic, (size_t)ev->topic_len, (const uint8_t *)ev->data, (size_t)ev->data_len);
            return;
        }
        memcpy(s_frag_topic, ev->topic, (size_t)ev->topic_len);
        s_frag_tl = ev->topic_len;
        s_frag_len = 0;
    }
    if (s_frag_len < 0 || ev->current_data_offset != s_frag_len ||
        s_frag_len + ev->data_len > NV_MQTT_APP_MSG_MAX) { s_frag_len = -1; return; }
    memcpy(s_frag + s_frag_len, ev->data, (size_t)ev->data_len);
    s_frag_len += ev->data_len;
    if (s_frag_len == ev->total_data_len) {
        app_push_in(s_frag_topic, (size_t)s_frag_tl, s_frag, (size_t)s_frag_len);
        s_frag_len = -1;
    }
}

// ---------------------------------------------------------------- esp-mqtt glue
void mqtt_evt(void *, esp_event_base_t, int32_t id, void *data)
{
    auto *ev = static_cast<esp_mqtt_event_handle_t>(data);
    switch ((esp_mqtt_event_id_t)id) {
    case MQTT_EVENT_CONNECTED: {
        char t[64];
        snprintf(t, sizeof t, "nucleo/%s/+/set", s_node);
        esp_mqtt_client_subscribe(ev->client, t, 0);
        esp_mqtt_client_subscribe(ev->client, "homeassistant/status", 0);
        if (s_app_mx && s_app_nf.load()) {                  // the running app's filters survive a reconnect
            xSemaphoreTake(s_app_mx, portMAX_DELAY);
            for (int i = 0; i < s_app_nf.load(); i++) esp_mqtt_client_subscribe(ev->client, s_app_filt[i], 0);
            xSemaphoreGive(s_app_mx);
        }
        s_connected.store(true);
        s_need_disc.store(true);
        s_state.store(NV_MQTT_CONNECTED);                  // detail keeps "host:port"
        if (s_task) xTaskNotifyGive(s_task);
        NV_LOGI(TAG, "connected");
        break;
    }
    case MQTT_EVENT_DISCONNECTED:
        if (s_connected.exchange(false)) NV_LOGW(TAG, "disconnected");
        if (s_state.load() == NV_MQTT_CONNECTED) set_state(NV_MQTT_CONNECTING, "reconnecting");
        break;
    case MQTT_EVENT_ERROR:
        if (ev->error_handle) {
            const auto *eh = ev->error_handle;
            if (eh->error_type == MQTT_ERROR_TYPE_CONNECTION_REFUSED)
                set_state(NV_MQTT_ERROR, eh->connect_return_code == MQTT_CONNECTION_REFUSE_NOT_AUTHORIZED ||
                                         eh->connect_return_code == MQTT_CONNECTION_REFUSE_BAD_USERNAME
                                             ? "not authorized" : "refused");
            else if (eh->error_type == MQTT_ERROR_TYPE_TCP_TRANSPORT)
                set_state(NV_MQTT_ERROR, "unreachable");
        }
        break;
    case MQTT_EVENT_DATA: {
        app_on_data(ev);                                    // ABI v12: the foreground app's filters
        // Only whole, small messages: a fragmented (large) payload is never a command of ours.
        if (ev->total_data_len != ev->data_len || ev->current_data_offset != 0) break;
        if (ev->topic_len == 20 && !memcmp(ev->topic, "homeassistant/status", 20)) {
            // HA (re)started: re-send discovery + states (its birth message is "online").
            if (ev->data_len == 6 && !memcmp(ev->data, "online", 6)) {
                s_need_disc.store(true);
                if (s_task) xTaskNotifyGive(s_task);
            }
            break;
        }
        const ha_entity_t e = ha_route_cmd(ev->topic, (size_t)ev->topic_len, s_node);
        if (e == HA_E_COUNT || ev->data_len < 0) break;
        Cmd c;
        c.ent = e;
        c.len = (uint16_t)(ev->data_len < (int)sizeof c.payload - 1 ? ev->data_len : (int)sizeof c.payload - 1);
        memcpy(c.payload, ev->data, c.len);
        c.payload[c.len] = '\0';
        if (xQueueSend(s_q, &c, 0) != pdTRUE) NV_LOGW(TAG, "command queue full, dropped");
        else if (s_task) xTaskNotifyGive(s_task);
        break;
    }
    default:
        break;
    }
}

void stop_client(void)
{
    if (!s_client) return;
    esp_mqtt_client_stop(s_client);
    esp_mqtt_client_destroy(s_client);
    s_client = nullptr;
    s_connected.store(false);
}

bool start_client(void)
{
    char host[64], user[64], pass[64];
    nv_config_get_str("mqtt_host", "", host, sizeof host);
    if (!host[0]) { set_state(NV_MQTT_NO_BROKER, ""); return false; }
    const int port = nv_config_get_int("mqtt_port", 1883);

    // "*.local" through mDNS (lwIP's resolver doesn't do multicast DNS here).
    char addr[64];
    snprintf(addr, sizeof addr, "%s", host);
    const size_t hl = strlen(host);
    if (hl > 6 && !strcasecmp(host + hl - 6, ".local")) {
        const esp_err_t mi = mdns_init();
        (void)mi;                                         // ESP_ERR_INVALID_STATE = already up
        char name[64];
        snprintf(name, sizeof name, "%.*s", (int)(hl - 6), host);
        esp_ip4_addr_t ip = {};
        if (mdns_query_a(name, 3000, &ip) != ESP_OK) {
            set_state(NV_MQTT_ERROR, "mDNS lookup failed");
            return false;
        }
        snprintf(addr, sizeof addr, IPSTR, IP2STR(&ip));
    }

    char uri[96];
    snprintf(uri, sizeof uri, "mqtt://%s:%d", addr, port);
    char lwt[64];
    snprintf(lwt, sizeof lwt, "nucleo/%s/status", s_node);
    nv_config_get_str("mqtt_user", "", user, sizeof user);
    nv_config_get_str("mqtt_pass", "", pass, sizeof pass);

    esp_mqtt_client_config_t cfg = {};
    cfg.broker.address.uri = uri;
    cfg.credentials.client_id = s_node;
    if (user[0]) cfg.credentials.username = user;
    if (pass[0]) cfg.credentials.authentication.password = pass;
    cfg.session.keepalive = 30;
    cfg.session.last_will.topic = lwt;
    cfg.session.last_will.msg = "offline";
    cfg.session.last_will.qos = 1;
    cfg.session.last_will.retain = 1;
    cfg.network.reconnect_timeout_ms = 10000;
    cfg.network.timeout_ms = 8000;
    cfg.buffer.size = 1024;                               // commands are small; publishes fragment
    cfg.buffer.out_size = 1024;
    cfg.task.priority = 3;
    cfg.task.stack_size = 5120;
    cfg.outbox.limit = 16 * 1024;                         // don't grow forever while HA is away

    s_client = esp_mqtt_client_init(&cfg);                // copies every string it keeps
    memset(pass, 0, sizeof pass);
    if (!s_client) { set_state(NV_MQTT_ERROR, "no memory"); return false; }
    esp_mqtt_client_register_event(s_client, MQTT_EVENT_ANY, mqtt_evt, nullptr);
    if (esp_mqtt_client_start(s_client) != ESP_OK) {
        esp_mqtt_client_destroy(s_client);
        s_client = nullptr;
        set_state(NV_MQTT_ERROR, "start failed");
        return false;
    }
    char where[80];
    snprintf(where, sizeof where, "%s:%d", addr, port);
    set_state(NV_MQTT_CONNECTING, where);
    NV_LOGI(TAG, "connecting to %s as %s", where, s_node);
    return true;
}

void service_task(void *)
{
    uint32_t gen = s_cfg_gen.load();
    int64_t next_ui = 0, next_diag = 0, retry_at = 0;
    for (;;) {
        const bool want = s_enabled.load() && net_up();
        const uint32_t g = s_cfg_gen.load();
        const int64_t now = esp_timer_get_time() / 1000;

        if (s_client && (!want || g != gen)) {
            if (!s_enabled.load() && s_connected.load()) {
                publish_forget();
                vTaskDelay(pdMS_TO_TICKS(300));         // let the deletes leave before closing
            }
            stop_client();
            s_pub.valid = false;
        }
        if (!s_enabled.load())      set_state(NV_MQTT_OFF, "");
        else if (!net_up())         set_state(NV_MQTT_WAIT_NET, "");
        if (want && !s_client && (g != gen || now >= retry_at)) {
            gen = g;
            if (!start_client()) retry_at = now + 15000;   // bad host / mDNS miss: back off
        }

        Cmd c;
        while (s_q && xQueueReceive(s_q, &c, 0) == pdTRUE) exec(c);
        app_drain_out();

        if (s_connected.load()) {
            if (s_need_disc.exchange(false)) {
                publish_discovery();
                s_pub.valid = false;
                s_dirty.fetch_or(D_ALL);
            }
            if (now >= next_ui || !s_pub.valid) {
                next_ui = now + kUiPollMs;
                Pub old = s_pub;
                Pub cur = {};
                if (read_ui(&cur)) {
                    uint32_t d = 0;
                    if (!old.valid || old.asleep != cur.asleep) d |= D_SCREEN;
                    if (!old.valid || old.locked != cur.locked) d |= D_LOCKED;
                    if (!old.valid || old.touch != cur.touch)   d |= D_TOUCH;
                    if (!old.valid || strcmp(old.app, cur.app)) d |= D_APP;
                    s_pub = cur;
                    s_dirty.fetch_or(d);
                }
            }
            if (now >= next_diag) { next_diag = now + kDiagPeriodMs; s_dirty.fetch_or(D_DIAG); }
            const uint32_t d = s_dirty.exchange(0);
            if (d && s_pub.valid) publish_dirty(d);
            else if (d) s_dirty.fetch_or(d);                // UI busy: keep for next round
        }
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(s_enabled.load() ? 250 : 2000));
    }
}

void ensure_task(void)
{
    if (s_task) return;
    if (!s_q) {
        s_q = xQueueCreateWithCaps(kCmdQueueLen, sizeof(Cmd), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!s_q) { NV_LOGE(TAG, "queue alloc failed"); return; }
    }
    // INTERNAL stack: runs nv_config writes, LVGL work under the port lock and the esp-mqtt
    // client lifecycle (ENGINEERING_RULES §2). Lives forever once created; idles when disabled.
    if (xTaskCreate(service_task, "nv_mqtt", 6144, nullptr, 3, &s_task) != pdPASS) {
        s_task = nullptr;
        NV_LOGE(TAG, "service task create failed");
    }
}

void on_setting(nv_event_t, const void *data, void *)
{
    const char *key = static_cast<const char *>(data);
    if (!key) return;
    // Runs synchronously on whoever wrote the setting: flags only, no I/O here.
    if (!strcmp(key, "mqtt_en")) {
        s_enabled.store(nv_config_get_bool("mqtt_en", false));
        if (s_enabled.load()) ensure_task();
    } else if (!strncmp(key, "mqtt_", 5)) {
        s_cfg_gen.fetch_add(1);
    } else if (!strcmp(key, "brightness")) s_dirty.fetch_or(D_SCREEN);
    else if (!strcmp(key, "volume"))       s_dirty.fetch_or(D_VOLUME);
    else if (!strcmp(key, "mute"))         s_dirty.fetch_or(D_MUTE);
    else if (!strcmp(key, "qs_dnd"))       s_dirty.fetch_or(D_DND);
    else return;
    if (s_task) xTaskNotifyGive(s_task);
}

}  // namespace

// ---------------------------------------------------------------- public API
void nv_mqtt_init(void)
{
    static bool subscribed = false;
    if (!s_node[0]) {
        uint8_t mac[6] = {};
        esp_efuse_mac_get_default(mac);
        snprintf(s_node, sizeof s_node, "nucleo_%02x%02x%02x", mac[3], mac[4], mac[5]);
    }
    if (!s_app_mx) s_app_mx = xSemaphoreCreateMutex();
    if (!subscribed) {
        nv_event_subscribe(NV_EV_SETTINGS_CHANGED, on_setting, nullptr);
        subscribed = true;
    }
    s_enabled.store(nv_config_get_bool("mqtt_en", false));
    if (s_enabled.load()) ensure_task();
    else NV_LOGI(TAG, "off (Settings > Home to enable)");
}

nv_mqtt_state_t nv_mqtt_status(char *detail, size_t n)
{
    if (detail && n) snprintf(detail, n, "%s", s_detail);
    return (nv_mqtt_state_t)s_state.load();
}

const char *nv_mqtt_node_id(void) { return s_node; }

void nv_mqtt_republish(void)
{
    if (!s_connected.load()) return;
    s_need_disc.store(true);
    if (s_task) xTaskNotifyGive(s_task);
}

bool nv_mqtt_connected(void) { return s_connected.load(); }

int nv_mqtt_app_sub(const char *filter)
{
    if (!np_mqtt_filter_ok(filter)) return -1;
    if (!s_enabled.load() || !s_app_mx) return -2;
    if (!app_rings()) return -2;
    xSemaphoreTake(s_app_mx, portMAX_DELAY);
    int rc = 0;
    bool dup = false;
    for (int i = 0; i < s_app_nf.load(); i++) dup |= !strcmp(s_app_filt[i], filter);
    if (!dup) {
        if (s_app_nf.load() >= NV_MQTT_APP_FILTERS) rc = -3;
        else {
            snprintf(s_app_filt[s_app_nf.load()], sizeof s_app_filt[0], "%s", filter);
            s_app_nf.fetch_add(1);
        }
    }
    xSemaphoreGive(s_app_mx);
    if (rc == 0 && !dup) app_push_out(OUT_SUB, filter, nullptr, 0, false);
    return rc;
}

int nv_mqtt_app_pub(const char *topic, const void *data, int len, bool retain)
{
    if (!np_mqtt_pub_ok(topic) || len < 0 || len > NV_MQTT_APP_MSG_MAX || (len && !data)) return -1;
    if (!s_connected.load()) return -2;
    if (!app_rings()) return -3;
    return app_push_out(OUT_PUB, topic, data, (size_t)len, retain) ? 0 : -3;
}

int nv_mqtt_app_recv(char *topic, size_t tcap, void *payload, size_t pcap)
{
    if (!s_in || !topic || tcap == 0) return -1;
    size_t n = 0;
    uint8_t *b = static_cast<uint8_t *>(xRingbufferReceive(s_in, &n, 0));
    if (!b) return -1;
    int rc = -1;
    if (n >= 1 && (size_t)1 + b[0] <= n) {
        const size_t tl = b[0] < tcap - 1 ? b[0] : tcap - 1;
        memcpy(topic, b + 1, tl);
        topic[tl] = '\0';
        const size_t pl = n - 1 - b[0];
        if (payload && pcap) memcpy(payload, b + 1 + b[0], pl < pcap ? pl : pcap);
        rc = (int)pl;
    }
    vRingbufferReturnItem(s_in, b);
    return rc;
}

void nv_mqtt_app_reset(void)
{
    if (!s_app_mx) return;
    char drop[NV_MQTT_APP_FILTERS][128];
    xSemaphoreTake(s_app_mx, portMAX_DELAY);
    const int n = s_app_nf.load();
    memcpy(drop, s_app_filt, sizeof drop);
    s_app_nf.store(0);
    xSemaphoreGive(s_app_mx);
    for (int i = 0; i < n; i++) app_push_out(OUT_UNSUB, drop[i], nullptr, 0, false);
    size_t sz;
    void *it;
    while (s_in && (it = xRingbufferReceive(s_in, &sz, 0)) != nullptr) vRingbufferReturnItem(s_in, it);
}
