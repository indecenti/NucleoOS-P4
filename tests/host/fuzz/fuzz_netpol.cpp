// libFuzzer target for nv_net_policy: arbitrary app-supplied URLs, headers, HA paths and MQTT
// topics never read out of bounds, and whatever is accepted keeps the invariants the network
// imports rely on (no CR/LF/space in URL parts, a /api/ path without traversal, and so on).
#include "nv_net_policy.h"
#include "nv_mqtt_topic.h"

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *d, size_t n) {
    const std::string s(reinterpret_cast<const char *>(d), n);
    const char *c = s.c_str();                       // stops at the first NUL like the imports do

    np_url_t u;
    if (np_url_parse(c, &u)) {
        if (!u.host[0] || u.port == 0 || u.path[0] != '/') abort();
        for (const char *p = u.host; *p; p++) if (*p <= ' ' || *p == '@' || *p == ':' || *p == '/') abort();
        for (const char *p = u.path; *p; p++) if ((unsigned char)*p <= ' ' || *p == '#') abort();
        uint32_t ip;
        if (np_parse_ipv4(u.host, &ip)) (void)np_ip_is_private(ip);
    }
    const size_t half = s.find('\0') == std::string::npos ? s.size() / 2 : s.find('\0');
    const std::string name = s.substr(0, half), value = half < s.size() ? s.substr(half + 1) : "";
    if (np_header_ok(name.c_str(), value.c_str())) {
        if (strpbrk(value.c_str(), "\r\n") || strpbrk(name.c_str(), ": \r\n")) abort();
    }
    if (np_ha_path_ok(c)) {
        const std::string path(c, strcspn(c, "?"));                 // queries may hold anything
        if (strncmp(c, "/api/", 5) || path.find("/../") != std::string::npos ||
            path.find("//") != std::string::npos || strchr(c, '\\')) abort();
        for (const char *p = c; *p; p++) if ((unsigned char)*p <= ' ' || (unsigned char)*p >= 0x7f) abort();
    }
    if (np_mqtt_pub_ok(c) && (strchr(c, '+') || strchr(c, '#') || c[0] == '$')) abort();
    if (np_mqtt_filter_ok(name.c_str())) (void)np_mqtt_match(name.c_str(), value.data(), value.size());
    return 0;
}
