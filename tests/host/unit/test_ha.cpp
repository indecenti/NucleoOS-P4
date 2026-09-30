// Unit tests for nv_ha_proto (Home Assistant MQTT): command routing, the flat-JSON reader HA's
// light/notify payloads go through, payload text sanitizing and the discovery builder.
#include "check.h"
#include "nv_ha_proto.h"

#include <cstring>
#include <string>

static ha_entity_t route(const char *t) { return ha_route_cmd(t, strlen(t), "nucleo_a1b2c3"); }

static bool jget(const char *j, const char *k, std::string *out) {
    char buf[64];
    const bool ok = ha_json_get(j, strlen(j), k, buf, sizeof buf);
    if (ok) *out = buf;
    return ok;
}

int main() {
    // ---- routing: exact "nucleo/<node>/<obj>/set" for command entities only
    CHECK(route("nucleo/nucleo_a1b2c3/screen/set") == HA_E_SCREEN);
    CHECK(route("nucleo/nucleo_a1b2c3/notify/set") == HA_E_NOTIFY);
    CHECK(route("nucleo/nucleo_a1b2c3/reboot/set") == HA_E_REBOOT);
    CHECK(route("nucleo/nucleo_a1b2c3/temp/set") == HA_E_COUNT);       // sensor: no command
    CHECK(route("nucleo/nucleo_a1b2c3/screen/state") == HA_E_COUNT);
    CHECK(route("nucleo/nucleo_a1b2c3/screen/set/x") == HA_E_COUNT);
    CHECK(route("nucleo/nucleo_a1b2c4/screen/set") == HA_E_COUNT);     // other panel
    CHECK(route("nucleo/nucleo_a1b2c3x/screen/set") == HA_E_COUNT);
    CHECK(route("nucleo/nucleo_a1b2c3/") == HA_E_COUNT);
    CHECK(route("") == HA_E_COUNT);
    CHECK(ha_route_cmd("nucleo/nucleo_a1b2c3/mute/set", 22, "nucleo_a1b2c3") == HA_E_COUNT);  // cut

    // ---- flat JSON reader
    std::string v;
    CHECK(jget("{\"state\":\"ON\",\"brightness\":80}", "state", &v) && v == "ON");
    CHECK(jget("{\"state\":\"ON\",\"brightness\":80}", "brightness", &v) && v == "80");
    CHECK(jget(" { \"a\" : { \"b\" : \"}\" } , \"brightness\" : 7 } ", "brightness", &v) && v == "7");
    CHECK(!jget("{\"a\":{\"brightness\":5}}", "brightness", &v));        // nested: not top level
    CHECK(jget("{\"message\":\"ciao \\\"mondo\\\"\\n\\u00e8\"}", "message", &v) &&
          v == "ciao \"mondo\"\n\xc3\xa8");
    CHECK(jget("{\"m\":\"\\u20ac\"}", "m", &v) && v == "\xe2\x82\xac");
    CHECK(jget("{\"m\":\"\\ud83d\"}", "m", &v) && v == "?");             // lone surrogate
    CHECK(!jget("{\"state\":\"ON\"", "brightness", &v));                  // unterminated
    CHECK(!jget("{\"state\":\"O", "state", &v));
    CHECK(!jget("[\"state\"]", "state", &v));
    CHECK(!jget("{\"state\" \"ON\"}", "state", &v));
    CHECK(!jget("{\"s\":\"\\x\"}", "s", &v));
    CHECK(!jget("{\"s\":\"a\x01\"}", "s", &v));                           // raw control char
    CHECK(!jget("", "s", &v));
    CHECK(!jget("{\"s\":{\"x\":1}}", "s", &v));                           // object value refused
    {
        char small[4];
        CHECK(ha_json_get("{\"t\":\"abcdefgh\"}", 16, "t", small, sizeof small) &&
              std::string(small) == "abc");
        CHECK(ha_json_get("{\"t\":123456}", 12, "t", small, sizeof small) && std::string(small) == "123");
    }
    int n = 0;
    CHECK(ha_json_get_int("{\"brightness\":\"55\"}", 19, "brightness", &n) && n == 55);
    CHECK(!ha_json_get_int("{\"brightness\":\"x\"}", 18, "brightness", &n));

    // ---- scalar payloads
    bool on = false;
    CHECK(ha_parse_onoff("ON", 2, &on) && on);
    CHECK(ha_parse_onoff("off\n", 4, &on) && !on);
    CHECK(ha_parse_onoff("true", 4, &on) && on);
    CHECK(!ha_parse_onoff("onn", 3, &on));
    CHECK(!ha_parse_onoff("", 0, &on));
    CHECK(ha_parse_int("55.0", 4, &n) && n == 55);
    CHECK(ha_parse_int(" -3 ", 4, &n) && n == -3);
    CHECK(ha_parse_int("99999999999999", 14, &n) && n == 2147483647);
    CHECK(!ha_parse_int("5x", 2, &n));
    CHECK(!ha_parse_int("", 0, &n));
    CHECK(!ha_parse_int("-", 1, &n));

    // ---- display text
    char t[16];
    CHECK(ha_payload_text("hi\x01\x7f there \n\n", 14, t, sizeof t) == 8 && std::string(t) == "hi there");
    CHECK(ha_payload_text("\xff" "a", 2, t, sizeof t) == 2 && std::string(t) == "?a");
    CHECK(ha_payload_text("\xc3", 1, t, sizeof t) == 1 && std::string(t) == "?");          // cut UTF-8
    CHECK(ha_payload_text("\xc0\xaf", 2, t, sizeof t) == 2 && std::string(t) == "??");     // overlong
    CHECK(ha_payload_text("abc\0def", 7, t, sizeof t) == 3);
    CHECK(ha_payload_text("\xc3\xa8\xc3\xa8", 4, t, 4) == 2);                               // no split
    CHECK(ha_payload_text("x", 1, t, 1) == 0 && t[0] == '\0');

    // ---- escape + discovery
    char e[8];
    CHECK(ha_json_escape(e, sizeof e, "a\"b") == 4 && std::string(e) == "a\\\"b");
    CHECK(ha_json_escape(e, 4, "\xc3\xa8\xc3\xa8") == 2);    // truncation keeps whole characters
    char topic[128];
    CHECK(ha_config_topic(topic, sizeof topic, "homeassistant", "nucleo_a1b2c3", HA_E_SCREEN) &&
          std::string(topic) == "homeassistant/light/nucleo_a1b2c3/screen/config");
    CHECK(!ha_config_topic(topic, 10, "homeassistant", "nucleo_a1b2c3", HA_E_SCREEN));
    char cfg[2048];
    for (int i = 0; i < HA_E_COUNT; i++) {
        const size_t len = ha_build_config(cfg, sizeof cfg, (ha_entity_t)i, "nucleo_a1b2c3", i & 1,
                                           "1.1.128", "JC1060P470C", "[\"home\",\"calc\"]");
        CHECK(len > 0 && len == strlen(cfg) && len < 1024);    // fits the MQTT out buffer
        CHECK(cfg[0] == '{' && cfg[len - 1] == '}');
        std::string s(cfg);
        CHECK(s.find("\"uniq_id\":\"nucleo_a1b2c3_") != std::string::npos);
        CHECK(s.find("\"avty_t\":\"~/status\"") != std::string::npos);
        const ha_entity_def_t *d = ha_entity((ha_entity_t)i);
        CHECK((s.find("\"cmd_t\"") != std::string::npos) == d->has_cmd);
    }
    CHECK(ha_build_config(cfg, 64, HA_E_SCREEN, "nucleo_a1b2c3", false, "1", "m", nullptr) == 0);
    CHECK(ha_build_config(cfg, sizeof cfg, HA_E_COUNT, "n", false, "1", "m", nullptr) == 0);

    return TEST_DONE("ha");
}
