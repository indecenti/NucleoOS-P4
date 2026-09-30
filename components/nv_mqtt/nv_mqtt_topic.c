// nv_mqtt_topic — see header. Pure C, bounded loops.
#include "nv_mqtt_topic.h"

#include <string.h>

static bool topic_chars_ok(const char *t, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        const unsigned char c = (unsigned char)t[i];
        if (c == 0 || c < 0x20 || c == 0x7f) return false;
    }
    return true;
}

bool np_mqtt_pub_ok(const char *topic)
{
    if (!topic) return false;
    const size_t n = strnlen(topic, 128);
    if (n == 0 || n > 127 || !topic_chars_ok(topic, n)) return false;
    if (memchr(topic, '+', n) || memchr(topic, '#', n) || topic[0] == '$') return false;
    if (strncmp(topic, "homeassistant/", 14) == 0 || strncmp(topic, "nucleo/", 7) == 0) return false;
    return true;
}

bool np_mqtt_filter_ok(const char *f)
{
    if (!f) return false;
    const size_t n = strnlen(f, 128);
    if (n == 0 || n > 127 || !topic_chars_ok(f, n) || f[0] == '$') return false;
    if (n == 1 && f[0] == '#') return false;                  // everything: too broad for an app
    for (size_t i = 0; i < n; i++) {
        if (f[i] == '+') {
            if ((i > 0 && f[i - 1] != '/') || (i + 1 < n && f[i + 1] != '/')) return false;
        } else if (f[i] == '#') {
            if (i + 1 != n || (i > 0 && f[i - 1] != '/')) return false;
        }
    }
    return true;
}

bool np_mqtt_match(const char *f, const char *t, size_t tl)
{
    if (!f || !t) return false;
    if (tl && t[0] == '$') return false;                      // never match system topics
    size_t fi = 0, ti = 0;
    const size_t fl = strnlen(f, 128);
    while (fi < fl) {
        if (f[fi] == '#') return true;                        // rest (incl. parent level)
        if (f[fi] == '+') {
            while (ti < tl && t[ti] != '/') ti++;
            fi++;
        } else {
            while (fi < fl && ti < tl && f[fi] != '/' && f[fi] == t[ti]) { fi++; ti++; }
            if (fi < fl && f[fi] != '/') return false;
            if (ti < tl && t[ti] != '/') return false;
        }
        // both at a level end
        if (fi == fl) return ti == tl;
        if (f[fi] != '/') return false;
        if (ti == tl) return fl - fi == 2 && f[fi + 1] == '#';   // "a/#" matches "a"
        if (t[ti] != '/') return false;
        fi++;
        ti++;
    }
    return ti == tl;
}
