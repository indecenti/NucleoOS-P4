// Net 12 — smoke test of the ABI v12 network imports (http_req, ws_*, mqtt_*, ha_*).
//
// Point it at a test server on the LAN (tools/net12_server.py) by writing its address, e.g.
// "192.168.1.20:8766", into /sdcard/apps/net12/server (the web console's file manager or
// /api/fs/write), then run it from the web console (/api/app/run?id=net12) and read the log.
// The manifest grants "lan" but NOT "net": the public-address request must be refused (-4).
#include "nucleo_sdk.h"

static char g_srv[64];
static char g_buf[2048];

static int wait_http(int h, int ms)
{
    int st = 0;
    for (int t = 0; t < ms; t += 20) {
        st = nv_http_state(h);
        if (st != 0) break;
        nv_sleep_ms(20);
    }
    return st;
}

static int check(const char *what, int ok)
{
    nv_printf("%s %s", ok ? "PASS" : "FAIL", what);
    return ok ? 0 : 1;
}

static int str_has(const char *hay, int n, const char *needle)
{
    int nl = 0;
    while (needle[nl]) nl++;
    for (int i = 0; i + nl <= n; i++) {
        int k = 0;
        while (k < nl && hay[i + k] == needle[k]) k++;
        if (k == nl) return 1;
    }
    return 0;
}

NV_EXPORT("run")
void run(void)
{
    int fails = 0;
    int n = nv_load("server", g_srv, sizeof g_srv - 1);
    while (n > 0 && (g_srv[n - 1] == '\n' || g_srv[n - 1] == '\r' || g_srv[n - 1] == ' ')) n--;
    g_srv[n > 0 ? n : 0] = 0;
    if (n <= 0) { nv_print("no /sdcard/apps/net12/server file: LAN tests skipped"); }

    char spec[256];
    if (n > 0) {
        // 1. GET on the LAN ("lan" permission)
        nv_snprintf(spec, sizeof spec, "{\"url\":\"http://%s/ping\"}", g_srv);
        int h = nv_http_req(spec, 0, 0);
        int st = h >= 0 ? wait_http(h, 12000) : h;
        int r = st == 1 ? nv_http_read(h, g_buf, sizeof g_buf - 1) : 0;
        g_buf[r > 0 ? r : 0] = 0;
        fails += check("http GET lan", st == 1 && nv_http_status(h) == 200 && str_has(g_buf, r, "pong"));
        if (h >= 0) nv_http_close(h);

        // 2. POST with a body and a header, echoed back
        nv_snprintf(spec, sizeof spec,
                    "{\"url\":\"http://%s/echo\",\"method\":\"POST\",\"headers\":{\"X-Test\":\"v12\"}}", g_srv);
        const char body[] = "{\"hello\":\"nucleo\"}";
        h = nv_http_req(spec, body, sizeof body - 1);
        st = h >= 0 ? wait_http(h, 12000) : h;
        r = st == 1 ? nv_http_read(h, g_buf, sizeof g_buf - 1) : 0;
        g_buf[r > 0 ? r : 0] = 0;
        fails += check("http POST echo", st == 1 && str_has(g_buf, r, "nucleo") && str_has(g_buf, r, "v12"));
        if (h >= 0) nv_http_close(h);

        // 3. a redirect is returned, not followed
        nv_snprintf(spec, sizeof spec, "{\"url\":\"http://%s/redirect\"}", g_srv);
        h = nv_http_req(spec, 0, 0);
        st = h >= 0 ? wait_http(h, 12000) : h;
        fails += check("redirect not followed", st == 1 && nv_http_status(h) == 302);
        if (h >= 0) nv_http_close(h);

        // 4. header injection refused up front
        nv_snprintf(spec, sizeof spec, "{\"url\":\"http://%s/ping\",\"headers\":{\"Host\":\"x\"}}", g_srv);
        fails += check("Host header refused", nv_http_req(spec, 0, 0) == NV_NET_E_ARG);

        // 5. WebSocket echo
        nv_snprintf(spec, sizeof spec, "ws://%s/ws", g_srv);
        int w = nv_ws_open(spec, "");
        int ws = 0;
        for (int t = 0; t < 8000 && w >= 0; t += 20) { ws = nv_ws_state(w); if (ws != 0) break; nv_sleep_ms(20); }
        int got = 0;
        if (ws == 1) {
            nv_ws_send(w, "ciao-ws", 7, 0);
            for (int t = 0; t < 5000 && got <= 0; t += 20) { got = nv_ws_recv(w, g_buf, sizeof g_buf - 1); nv_sleep_ms(20); }
        }
        g_buf[got > 0 ? got : 0] = 0;
        fails += check("websocket echo", ws == 1 && got > 0 && str_has(g_buf, got, "ciao-ws"));
        if (w >= 0) nv_ws_close(w);
    }

    // 6. public address without "net": refused by destination policy
    int h = nv_http_req("{\"url\":\"http://1.1.1.1/\"}", 0, 0);
    int st = h >= 0 ? wait_http(h, 12000) : h;
    fails += check("public denied without net", st == NV_NET_E_DEST);
    if (h >= 0) nv_http_close(h);

    // 7. Home Assistant / MQTT: report configuration (they pass either way)
    nv_printf("ha_available=%d mqtt_sub=%d", nv_ha_available(), nv_mqtt_sub("nucleo_test/#"));
    if (nv_ha_available()) {
        h = nv_ha_req("GET", "/api/", 0, 0);
        st = h >= 0 ? wait_http(h, 12000) : h;
        int r = st == 1 ? nv_http_read(h, g_buf, sizeof g_buf - 1) : 0;
        g_buf[r > 0 ? r : 0] = 0;
        fails += check("ha GET /api/", st == 1 && nv_http_status(h) == 200 && str_has(g_buf, r, "API running"));
        if (h >= 0) nv_http_close(h);
    }
    fails += check("ha path traversal refused", nv_ha_req("GET", "/api/../auth", 0, 0) == NV_NET_E_ARG ||
                                              nv_ha_req("GET", "/api/../auth", 0, 0) == NV_NET_E_PERM);

    nv_printf("net12 done: %d failure(s)", fails);
}
