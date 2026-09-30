// nv_net_policy — pure checks behind the ABI v12 network imports (nv_wasm_net.cpp): URL split,
// destination class (public vs private address), request header sanity, Home Assistant API
// paths. (MQTT topic rules: nv_mqtt_topic.h.) No ESP-IDF; unit-tested and fuzzed in tests/host (target "netpol").
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum { NP_HTTP = 0, NP_HTTPS, NP_WS, NP_WSS } np_scheme_t;

typedef struct {
    np_scheme_t scheme;
    char        host[64];    // lowercase name or dotted IPv4 (no IPv6 literals)
    uint16_t    port;        // explicit or the scheme default
    char        path[192];   // "/..." (query included), "/" when absent
} np_url_t;

// Parse an absolute http/https/ws/wss URL. Refuses userinfo ("user@host"), IPv6 literals,
// control characters or spaces anywhere, ports outside 1..65535, over-long parts.
bool np_url_parse(const char *url, np_url_t *out);

// True for RFC1918, loopback, link-local, CGNAT (100.64/10), "this network" (0/8), multicast,
// broadcast and reserved space: anything that is not the public Internet. `ip` is host order.
bool np_ip_is_private(uint32_t ip_host_order);

// Dotted IPv4 literal -> host-order address. False if `s` is not exactly a.b.c.d.
bool np_parse_ipv4(const char *s, uint32_t *out);

// Request header an app may set: token name, printable value, not one the host owns
// (Host, Content-Length, Transfer-Encoding, Connection, Upgrade, Sec-WebSocket-*).
bool np_header_ok(const char *name, const char *value);

// Home Assistant REST path for the proxy: "/api/..." with no "..", "//", "\", "%" escapes of
// '.', '/', or control characters; <= 191 chars. The host adds the token.
bool np_ha_path_ok(const char *path);

#ifdef __cplusplus
}
#endif
