// LAN teacher discovery — nucleomind (the Android companion app) advertises "_anima._tcp"
// over mDNS and serves an OpenAI-compatible API on the phone (default :8080, /v1/chat/completions,
// /v1/distill, /v1/ground). When no cloud teacher key is configured, the online tier asks here
// whether a phone brain is reachable and uses it keylessly — the strongest available teacher wins
// without any user setup (docs/anima-knowledge-ollama.md: the "LAN teacher" leg of the cascade).
//
// Discovery is lazy and cached: at most one 2 s mDNS PTR query per 5-minute window, and only when
// the device actually has a station IP. Runs on the ANIMA workers and the httpd task (the native app
// asks once from the UI thread only when /model, /status or its settings open before the first turn).
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "mdns.h"
#include "freertos/FreeRTOS.h"   // portMUX critical sections

#include "nucleo_setup.h"   // nucleo_setup_ip(): "" when not associated

static const char *TAG = "anima_lan";

// Shared by the httpd (/api/anima/caps), the native and web ANIMA workers: the probe runs on a
// local copy and the result is published / read under a short critical section, so a reader never
// sees a half-written base or the "not found yet" of another task's probe in progress. One task
// probes at a time; the others answer from the previous result meanwhile.
static char    s_base[96];      // "http://192.168.x.y:8080/v1"
static int64_t s_probe_us;      // time of the last probe (0 = never)
static bool    s_found;
static bool    s_probing;
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;

// Probe (rate-limited) and report the phone teacher endpoint. Returns true and fills `base`
// ("http://ip:port/v1") when a nucleomind instance is reachable on this LAN.
bool nucleo_anima_lan_endpoint(char *base, size_t bcap)
{
    if (!nucleo_setup_ip()[0]) return false;             // no STA association -> no LAN
    int64_t now = esp_timer_get_time();
    bool probe = false;
    portENTER_CRITICAL(&s_mux);
    if (!s_probing && (!s_probe_us || (now - s_probe_us) > 300LL * 1000 * 1000)) {
        s_probing = true;
        s_probe_us = now;
        probe = true;
    }
    portEXIT_CRITICAL(&s_mux);
    if (probe) {
        char found_base[sizeof s_base] = "";
        bool found = false;
        // Idempotent: nv_web/keydeck normally own mdns_init(); this returns INVALID_STATE then.
        mdns_init();
        mdns_result_t *r = NULL;
        if (mdns_query_ptr("_anima", "_tcp", 2000, 4, &r) == ESP_OK) {
            for (mdns_result_t *it = r; it && !found; it = it->next) {
                for (mdns_ip_addr_t *a = it->addr; a; a = a->next) {
                    if (a->addr.type != ESP_IPADDR_TYPE_V4) continue;
                    snprintf(found_base, sizeof found_base, "http://" IPSTR ":%u/v1",
                             IP2STR(&a->addr.u_addr.ip4), it->port ? it->port : 8080);
                    found = true;
                    ESP_LOGI(TAG, "nucleomind teacher found: %s", found_base);
                    break;
                }
            }
            mdns_query_results_free(r);
        }
        if (!found) ESP_LOGD(TAG, "no _anima._tcp on this LAN");
        portENTER_CRITICAL(&s_mux);
        memcpy(s_base, found_base, sizeof s_base);
        s_found = found;
        s_probing = false;
        portEXIT_CRITICAL(&s_mux);
    }
    char out[sizeof s_base];
    portENTER_CRITICAL(&s_mux);
    const bool found = s_found;
    memcpy(out, s_base, sizeof out);
    portEXIT_CRITICAL(&s_mux);
    if (!found) return false;
    snprintf(base, bcap, "%s", out);
    return true;
}
