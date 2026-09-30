// libFuzzer target for nv_ha_proto: everything a hostile MQTT broker/client on the LAN can send
// (topic + payload) goes through route/json/text parsing without reading out of bounds, and every
// output stays NUL-terminated inside its buffer.
#include "nv_ha_proto.h"

#include <cstdint>
#include <cstdlib>
#include <cstring>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *d, size_t n) {
    if (n < 1) return 0;
    const size_t split = d[0] % (n);                 // first byte picks the topic/payload split
    const char *topic = reinterpret_cast<const char *>(d + 1);
    const size_t tl = split > n - 1 ? n - 1 : split;
    const char *p = topic + tl;
    const size_t pl = n - 1 - tl;

    const ha_entity_t e = ha_route_cmd(topic, tl, "nucleo_a1b2c3");
    if (e != HA_E_COUNT && !ha_entity(e)->has_cmd) abort();

    char out[32];
    static const char *const kKeys[] = {"state", "brightness", "title", "message", ""};
    for (const char *k : kKeys) {
        memset(out, 'Z', sizeof out);
        if (ha_json_get(p, pl, k, out, sizeof out) && !memchr(out, '\0', sizeof out)) abort();
    }
    int v;
    (void)ha_json_get_int(p, pl, "brightness", &v);
    bool on;
    (void)ha_parse_onoff(p, pl, &on);
    (void)ha_parse_int(p, pl, &v);

    char text[64];
    const size_t tn = ha_payload_text(p, pl, text, sizeof text);
    if (tn >= sizeof text || text[tn] != '\0' || strlen(text) != tn) abort();
    for (size_t i = 0; i < tn; i++)
        if ((unsigned char)text[i] < 0x20 && text[i] != '\n') abort();

    // Escaping arbitrary (NUL-terminated) text must stay inside the buffer and be valid JSON text.
    char src[64];
    const size_t sl = pl < sizeof src - 1 ? pl : sizeof src - 1;
    memcpy(src, p, sl);
    src[sl] = '\0';
    char esc[48];
    const size_t en = ha_json_escape(esc, sizeof esc, src);
    if (en >= sizeof esc || esc[en] != '\0') abort();
    return 0;
}
