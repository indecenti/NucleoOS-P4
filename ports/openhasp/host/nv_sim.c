/* nv_sim.c — native PC stand-in for the NucleoOS host ABI, to run the openHASP app (the same
 * sources as app.wasm, built with -DNV_SIM) on Linux/WSL without the board:
 *
 *   canvas   nv_gfx_blit_raw draws into a 1024x600 RGB565 frame buffer; "shot" dumps it as PPM
 *   clock    simulated: 16 ms per nv_gfx_present plus every nv_sleep_ms (runs as fast as it can)
 *   MQTT     nv_mqtt_sub records the filters, nv_mqtt_pub logs to <out>/mqtt.log (and stdout),
 *            nv_mqtt_recv hands out the messages the script injects (matched against the filters)
 *   touch    the script's "tap"/"press"/"release" drive nv_gfx_input_raw
 *   storage  nv_save/nv_load: files ".nv_<name>" in the data folder (the program's cwd)
 *
 *   openhasp_sim <data dir> <script> <out dir>
 *
 * Script (one command per line, '#' comments):
 *   wait N                 let N frames run
 *   mqtt TOPIC PAYLOAD     inject a message (payload = rest of the line)
 *   mqttfile TOPIC FILE    inject a message whose payload is FILE's content (read from the data dir)
 *   mqttlines TOPIC FILE   inject one message per non-empty line of FILE
 *   mqttjsonl TOPIC FILE   inject FILE the way the HA integration's load_pages does: whole lines
 *                          (with their newlines) packed into payloads of about 1000 characters
 *   tap X Y                press 6 frames, release
 *   press X Y / release
 *   back                   OS back gesture
 *   shot NAME              write <out>/NAME.ppm
 *   quit
 */
#define _GNU_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "nucleo_sdk.h"

#define W 1024
#define H 600

static uint16_t g_fb[W * H];
static uint32_t g_ms = 1000;
static FILE* g_script;
static const char* g_out;
static FILE* g_mqttlog;
static int g_wait, g_running = 1, g_back;
static int g_touch, g_tx, g_ty, g_tap_frames;
static int g_blits, g_blit_px;

/* ---- MQTT ------------------------------------------------------------------------------------ */
#define MAXF 16
static char g_filters[MAXF][128];
static int g_nf;

typedef struct Msg {
    char topic[256];
    char* payload;
    int len;
    struct Msg* next;
} Msg;
static Msg *g_q_head, *g_q_tail;

static int topic_match(const char* f, const char* t)
{
    while(*f) {
        if(*f == '#') return 1;
        if(f[0] == '/' && f[1] == '#' && !f[2] && !*t) return 1; // "a/#" also matches "a"
        if(*f == '+') {
            while(*t && *t != '/') t++;
            f++;
            continue;
        }
        if(*f != *t) return 0;
        f++;
        t++;
    }
    return *t == 0;
}

static void inject(const char* topic, const char* payload, int len)
{
    int ok = 0;
    for(int i = 0; i < g_nf; i++) ok |= topic_match(g_filters[i], topic);
    if(!ok) {
        printf("[sim] not subscribed, dropped: %s\n", topic);
        return;
    }
    Msg* m = calloc(1, sizeof *m);
    snprintf(m->topic, sizeof m->topic, "%s", topic);
    m->payload = malloc(len + 1);
    memcpy(m->payload, payload, len);
    m->payload[len] = 0;
    m->len          = len;
    if(g_q_tail) g_q_tail->next = m;
    else g_q_head = m;
    g_q_tail = m;
}

int32_t nv_mqtt_sub(const char* filter)
{
    if(g_nf >= 8) return NV_NET_E_BUSY; // the OS allows 8 filters per app
    snprintf(g_filters[g_nf++], 128, "%s", filter);
    fprintf(g_mqttlog, "SUB %s\n", filter);
    return 0;
}

int32_t nv_mqtt_pub(const char* topic, const void* buf, uint32_t len, int32_t retain)
{
    if(!strncmp(topic, "homeassistant/", 14) || !strncmp(topic, "nucleo/", 7) || topic[0] == '$') {
        fprintf(g_mqttlog, "REFUSED %s\n", topic); // the OS rule
        return NV_NET_E_ARG;
    }
    if(len > 8192) return NV_NET_E_ARG;
    fprintf(g_mqttlog, "PUB%s %s %.*s\n", retain ? "(retained)" : "", topic, (int)len, (const char*)buf);
    fflush(g_mqttlog);
    return 0;
}

int32_t nv_mqtt_recv(char* topic, uint32_t tcap, void* buf, uint32_t cap)
{
    Msg* m = g_q_head;
    if(!m) return -1;
    g_q_head = m->next;
    if(!g_q_head) g_q_tail = NULL;
    snprintf(topic, tcap, "%s", m->topic);
    memcpy(buf, m->payload, (uint32_t)m->len < cap ? (uint32_t)m->len : cap);
    int n = m->len;
    free(m->payload);
    free(m);
    return n;
}

/* ---- gfx / input / misc ---------------------------------------------------------------------- */
int32_t nv_millis(void)
{
    return (int32_t)g_ms;
}
void nv_sleep_ms(int32_t ms)
{
    if(ms > 0) g_ms += (uint32_t)ms;
}
int32_t nv_rand(void)
{
    return rand();
}
int32_t nv_lang(char* buf, uint32_t len)
{
    return snprintf(buf, len, "it");
}
void nv_backlight(int32_t level)
{
    fprintf(g_mqttlog, "BACKLIGHT %d\n", (int)level);
}
void nv_gfx_persist(int32_t on)
{
    (void)on;
}
int32_t nv_gfx_back(void)
{
    int b  = g_back;
    g_back = 0;
    return b;
}
int32_t nv_gfx_input_raw(void)
{
    return (g_touch ? 1 << 24 : 0) | (g_ty << 12) | g_tx;
}

void nv_gfx_blit_raw(const void* px, int32_t len, int32_t x, int32_t y, int32_t w, int32_t h)
{
    if(len < w * h * 2 || x < 0 || y < 0 || x + w > W || y + h > H) {
        printf("[sim] bad blit %d,%d %dx%d len %d\n", x, y, w, h, len);
        abort();
    }
    const uint16_t* s = px;
    for(int r = 0; r < h; r++) memcpy(&g_fb[(y + r) * W + x], s + r * w, (size_t)w * 2);
    g_blits++;
    g_blit_px += w * h;
}

int32_t nv_gfx_present(void)
{
    g_ms += 16;
    if(g_tap_frames > 0 && --g_tap_frames == 0) g_touch = 0;
    return g_running;
}

static char* read_file(const char* path, int* len)
{
    FILE* f = fopen(path, "rb");
    if(!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char* b = malloc(n + 1);
    *len    = (int)fread(b, 1, n, f);
    b[*len] = 0;
    fclose(f);
    return b;
}

int32_t nv_save(const char* name, const void* data, int32_t len)
{
    char p[128];
    snprintf(p, sizeof p, ".nv_%s", name);
    FILE* f = fopen(p, "wb");
    if(!f) return 0;
    fwrite(data, 1, len, f);
    fclose(f);
    return 1;
}

int32_t nv_load(const char* name, void* data, int32_t len)
{
    char p[128];
    int n = 0;
    snprintf(p, sizeof p, ".nv_%s", name);
    char* b = read_file(p, &n);
    if(!b) return 0;
    if(n > len) n = len;
    memcpy(data, b, n);
    free(b);
    return n;
}

static void shot(const char* name)
{
    char p[512];
    snprintf(p, sizeof p, "%s/%s.ppm", g_out, name);
    FILE* f = fopen(p, "wb");
    fprintf(f, "P6\n%d %d\n255\n", W, H);
    for(int i = 0; i < W * H; i++) {
        uint16_t c = g_fb[i];
        unsigned char rgb[3] = {(unsigned char)(((c >> 11) & 31) * 255 / 31), (unsigned char)(((c >> 5) & 63) * 255 / 63),
                                (unsigned char)((c & 31) * 255 / 31)};
        fwrite(rgb, 1, 3, f);
    }
    fclose(f);
    printf("[sim] t=%u ms shot %s (blits so far %d, %d px)\n", g_ms, name, g_blits, g_blit_px);
}

/* Called by run() once per frame before loop(). Returns 0 to stop. */
int nv_sim_step(void)
{
    if(g_wait > 0) {
        g_wait--;
        return 1;
    }
    char line[4096];
    while(fgets(line, sizeof line, g_script)) {
        line[strcspn(line, "\r\n")] = 0;
        char* p = line;
        while(*p == ' ') p++;
        if(!*p || *p == '#') continue;
        char cmd[32] = {0}, a[512] = {0};
        int x, y, n = 0;
        sscanf(p, "%31s %n", cmd, &n);
        char* rest = p + n;
        printf("[sim] t=%u ms > %s\n", g_ms, p);
        if(!strcmp(cmd, "wait")) {
            g_wait = atoi(rest);
            return 1;
        } else if(!strcmp(cmd, "mqtt")) {
            int k = 0;
            sscanf(rest, "%511s %n", a, &k);
            inject(a, rest + k, (int)strlen(rest + k));
        } else if(!strcmp(cmd, "mqttfile") || !strcmp(cmd, "mqttlines") || !strcmp(cmd, "mqttjsonl")) {
            char file[256];
            sscanf(rest, "%511s %255s", a, file);
            int len = 0;
            char* b = read_file(file, &len);
            if(!b) {
                printf("[sim] missing %s\n", file);
                continue;
            }
            if(cmd[4] == 'f') inject(a, b, len);
            else if(cmd[4] == 'j') {
                /* openHASP-custom-component __init__.py send_lines(): a line joins the buffer
                 * unless buffer + line > 1000 characters, then the buffer is sent first */
                char* buf = calloc(1, len + 1);
                int bl    = 0, msgs = 0;
                for(char* s = b; *s;) {
                    char* e = strchr(s, '\n');
                    int ll  = e ? (int)(e - s) + 1 : (int)strlen(s);
                    if(bl + ll > 1000) {
                        inject(a, buf, bl);
                        msgs++;
                        bl = 0;
                    }
                    memcpy(buf + bl, s, ll);
                    bl += ll;
                    s += ll;
                }
                inject(a, buf, bl);
                printf("[sim] %s pushed in %d messages\n", file, msgs + 1);
                free(buf);
            } else {
                for(char* s = strtok(b, "\n"); s; s = strtok(NULL, "\n")) {
                    s[strcspn(s, "\r")] = 0;
                    if(*s) inject(a, s, (int)strlen(s));
                }
            }
            free(b);
        } else if(!strcmp(cmd, "tap") && sscanf(rest, "%d %d", &x, &y) == 2) {
            g_tx = x, g_ty = y, g_touch = 1, g_tap_frames = 6;
            g_wait = 8;
            return 1;
        } else if(!strcmp(cmd, "press") && sscanf(rest, "%d %d", &x, &y) == 2) {
            g_tx = x, g_ty = y, g_touch = 1, g_tap_frames = 0;
        } else if(!strcmp(cmd, "release")) {
            g_touch = 0;
        } else if(!strcmp(cmd, "back")) {
            g_back = 1;
            return 1;
        } else if(!strcmp(cmd, "shot")) {
            shot(rest);
        } else if(!strcmp(cmd, "quit")) {
            g_running = 0;
            return 1;
        } else {
            printf("[sim] unknown command: %s\n", p);
        }
    }
    g_running = 0; // end of script
    return 1;
}

#ifndef NV_SIM_NO_MAIN
extern void run(void);

int main(int argc, char** argv)
{
    if(argc < 4) {
        fprintf(stderr, "usage: %s <data dir> <script> <out dir>\n", argv[0]);
        return 2;
    }
    g_script = fopen(argv[2], "r");
    if(!g_script) {
        perror(argv[2]);
        return 2;
    }
    g_out = realpath(argv[3], NULL);
    char log[512];
    snprintf(log, sizeof log, "%s/mqtt.log", g_out);
    g_mqttlog = fopen(log, "w");
    if(chdir(argv[1])) {
        perror(argv[1]);
        return 2;
    }
    run();
    fprintf(g_mqttlog, "EXIT\n");
    fclose(g_mqttlog);
    printf("[sim] app exited at t=%u ms\n", g_ms);
    return 0;
}
#endif /* NV_SIM_NO_MAIN */
