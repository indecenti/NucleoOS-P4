// LAN teacher discovery — nucleomind (the Android companion app) advertises "_anima._tcp"
// over mDNS and serves an OpenAI-compatible API on the phone (default :8080, /v1/chat/completions,
// /v1/distill, /v1/ground). When no cloud teacher key is configured, the online tier asks here
// whether a phone brain is reachable and uses it keylessly — the strongest available teacher wins
// without any user setup (docs/anima-knowledge-ollama.md: the "LAN teacher" leg of the cascade).
//
// Discovery is lazy and cached: at most one 2 s mDNS PTR query per 5-minute window, and only when
// the device actually has a station IP. Runs on the ANIMA worker/httpd task, never the UI thread.
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "mdns.h"

#include "nucleo_setup.h"   // nucleo_setup_ip(): "" when not associated

static const char *TAG = "anima_lan";

static char    s_base[96];      // "http://192.168.x.y:8080/v1"
static int64_t s_probe_us;      // time of the last probe (0 = never)
static bool    s_found;

// Probe (rate-limited) and report the phone teacher endpoint. Returns true and fills `base`
// ("http://ip:port/v1") when a nucleomind instance is reachable on this LAN.
bool nucleo_anima_lan_endpoint(char *base, size_t bcap)
{
    if (!nucleo_setup_ip()[0]) return false;             // no STA association -> no LAN
    int64_t now = esp_timer_get_time();
    if (!s_probe_us || (now - s_probe_us) > 300LL * 1000 * 1000) {
        s_probe_us = now;
        s_found = false;
        // Idempotent: nv_web/keydeck normally own mdns_init(); this returns INVALID_STATE then.
        mdns_init();
        mdns_result_t *r = NULL;
        if (mdns_query_ptr("_anima", "_tcp", 2000, 4, &r) == ESP_OK) {
            for (mdns_result_t *it = r; it && !s_found; it = it->next) {
                for (mdns_ip_addr_t *a = it->addr; a; a = a->next) {
                    if (a->addr.type != ESP_IPADDR_TYPE_V4) continue;
                    snprintf(s_base, sizeof s_base, "http://" IPSTR ":%u/v1",
                             IP2STR(&a->addr.u_addr.ip4), it->port ? it->port : 8080);
                    s_found = true;
                    ESP_LOGI(TAG, "nucleomind teacher found: %s", s_base);
                    break;
                }
            }
            mdns_query_results_free(r);
        }
        if (!s_found) ESP_LOGD(TAG, "no _anima._tcp on this LAN");
    }
    if (!s_found) return false;
    snprintf(base, bcap, "%s", s_base);
    return true;
}
