// Unit tests for nv_net_policy: the checks that keep ABI v12 network imports inside what an app's
// permissions allow (public vs home network, headers, Home Assistant paths, MQTT topics).
#include "check.h"
#include "nv_net_policy.h"
#include "nv_mqtt_topic.h"

#include <cstring>
#include <string>

static bool url(const char *u, np_url_t *o = nullptr) {
    np_url_t t;
    return np_url_parse(u, o ? o : &t);
}
static bool ip(const char *s, uint32_t *v = nullptr) { return np_parse_ipv4(s, v); }
static bool priv(const char *s) { uint32_t v = 0; return ip(s, &v) && np_ip_is_private(v); }
static bool m(const char *f, const char *t) { return np_mqtt_match(f, t, strlen(t)); }

int main() {
    np_url_t u;
    CHECK(url("http://Example.COM/a?b=1#frag", &u) && u.scheme == NP_HTTP && u.port == 80 &&
          !strcmp(u.host, "example.com") && !strcmp(u.path, "/a?b=1"));
    CHECK(url("https://h:8443", &u) && u.port == 8443 && !strcmp(u.path, "/") && u.scheme == NP_HTTPS);
    CHECK(url("wss://ha.local/api/websocket", &u) && u.scheme == NP_WSS && u.port == 443);
    CHECK(url("http://h?x=1", &u) && !strcmp(u.path, "/?x=1"));
    CHECK(!url("ftp://h/"));
    CHECK(!url("http://user@h/"));
    CHECK(!url("http://[::1]/"));
    CHECK(!url("http://h:0/") && !url("http://h:65536/") && !url("http://h:/") && !url("http://h:123456/"));
    CHECK(!url("http://h/a b") && !url("http://h/a\r\nX: y") && !url("http://h\t/"));
    CHECK(!url("http:///x") && !url("http://.h/") && !url("http://a..b/") && !url("http://h_x/"));
    CHECK(!url(("http://" + std::string(64, 'a') + "/").c_str()));
    CHECK(url(("http://" + std::string(63, 'a') + "/").c_str()));
    CHECK(!url(("http://h/" + std::string(191, 'a')).c_str()));
    CHECK(url(("http://h/" + std::string(190, 'a')).c_str()));
    CHECK(!url("") && !url(nullptr));

    uint32_t v = 0;
    CHECK(ip("192.168.1.10", &v) && v == 0xC0A8010A);
    CHECK(!ip("192.168.1") && !ip("1.2.3.4.5") && !ip("256.1.1.1") && !ip("01.2.3.4") && !ip("1.2.3.4 "));
    CHECK(!ip("") && !ip("a.b.c.d") && !ip("1..2.3") && !ip("1234.1.1.1"));
    CHECK(ip("0.0.0.0") && ip("255.255.255.255"));
    CHECK(priv("10.0.0.1") && priv("127.0.0.1") && priv("172.16.0.1") && priv("172.31.255.255"));
    CHECK(priv("192.168.77.216") && priv("169.254.1.1") && priv("100.64.0.1") && priv("0.1.2.3"));
    CHECK(priv("224.0.0.1") && priv("255.255.255.255") && priv("192.0.0.8"));
    CHECK(!priv("8.8.8.8") && !priv("172.32.0.1") && !priv("100.128.0.1") && !priv("192.169.0.1"));

    CHECK(np_header_ok("Content-Type", "application/json"));
    CHECK(np_header_ok("Authorization", "Bearer abc.def"));
    CHECK(!np_header_ok("Host", "evil") && !np_header_ok("content-length", "1"));
    CHECK(!np_header_ok("Sec-WebSocket-Key", "x") && !np_header_ok("sec-websocket-protocol", "x"));
    CHECK(!np_header_ok("X", "a\r\nInjected: 1") && !np_header_ok("X Y", "1") && !np_header_ok("", "1"));
    CHECK(!np_header_ok("X:", "1") && np_header_ok("X", "") && np_header_ok("X", "tab\there"));

    CHECK(np_ha_path_ok("/api/states/light.kitchen"));
    CHECK(np_ha_path_ok("/api/services/light/turn_on"));
    CHECK(np_ha_path_ok("/api/history/period?filter_entity_id=sensor.t"));
    CHECK(np_ha_path_ok("/api/states/sensor.a..b"));                 // dots inside a segment
    CHECK(!np_ha_path_ok("/api/../auth/token") && !np_ha_path_ok("/api/./x") && !np_ha_path_ok("/api//x"));
    CHECK(!np_ha_path_ok("/api/%2e%2e/auth") && !np_ha_path_ok("/api/a%2Fb") && !np_ha_path_ok("/api/a%5cb"));
    CHECK(!np_ha_path_ok("/auth/token") && !np_ha_path_ok("/api") && !np_ha_path_ok("api/x"));
    CHECK(!np_ha_path_ok("/api/x y") && !np_ha_path_ok("/api/x\\y") && !np_ha_path_ok("/api/x#y"));
    CHECK(!np_ha_path_ok("/api/x/..") && !np_ha_path_ok("/api/x%"));

    CHECK(np_mqtt_pub_ok("zigbee2mqtt/lamp/set") && np_mqtt_pub_ok("a"));
    CHECK(!np_mqtt_pub_ok("homeassistant/light/x/config") && !np_mqtt_pub_ok("nucleo/n/screen/set"));
    CHECK(!np_mqtt_pub_ok("$SYS/x") && !np_mqtt_pub_ok("a/+") && !np_mqtt_pub_ok("a/#") && !np_mqtt_pub_ok(""));
    CHECK(!np_mqtt_pub_ok(std::string(128, 'a').c_str()) && np_mqtt_pub_ok(std::string(127, 'a').c_str()));
    CHECK(np_mqtt_filter_ok("zigbee2mqtt/+/state") && np_mqtt_filter_ok("a/#") && np_mqtt_filter_ok("+"));
    CHECK(np_mqtt_filter_ok("homeassistant/+/+/+/config"));             // reading is allowed
    CHECK(!np_mqtt_filter_ok("#") && !np_mqtt_filter_ok("$SYS/#") && !np_mqtt_filter_ok("a+/b"));
    CHECK(!np_mqtt_filter_ok("a/#/b") && !np_mqtt_filter_ok("a#") && !np_mqtt_filter_ok(""));

    CHECK(m("a/b", "a/b") && !m("a/b", "a/bc") && !m("a/b", "a") && !m("ab", "a"));
    CHECK(m("a/+", "a/x") && m("a/+", "a/") && !m("a/+", "a/x/y") && !m("a/+", "a"));
    CHECK(m("a/+/c", "a/b/c") && !m("a/+/c", "a/b/d"));
    CHECK(m("a/#", "a") && m("a/#", "a/b/c") && !m("a/#", "ab"));
    CHECK(m("+/+", "a/b") && !m("+/+", "a") && m("+", "a") && !m("+", "a/b"));
    CHECK(!m("+/x", "$SYS/x") && !m("a/b", "$a/b"));

    return TEST_DONE("netpol");
}
