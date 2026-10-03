// nv_wasm_perms — permission names and the user's per-app revocations (see nv_wasm.h).
//
// Installing an app is consenting to its manifest permissions (the store lists the sensitive ones
// before install). What the user takes back later lives in /sdcard/nucleos/perms.json as
// {"<app id>": <revoked bitmask>}: outside every app folder, so no app can edit its own grants.
#include "nv_wasm.h"
#include "nv_log.h"
#include "nv_sd.h"
#include "nv_mem_attr.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_heap_caps.h"
#include "cJSON.h"

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <sys/stat.h>
#include <unistd.h>   // unlink

static const char *TAG = "wperm";

namespace {

struct Name { const char *name; uint32_t bit; };
const Name kNames[] = {
    {"log", NV_WPERM_LOG}, {"ui", NV_WPERM_UI}, {"net", NV_WPERM_NET}, {"fs", NV_WPERM_FS},
    {"gfx", NV_WPERM_GFX}, {"home", NV_WPERM_HOME}, {"lan", NV_WPERM_LAN}, {"ws", NV_WPERM_WS},
    {"mqtt", NV_WPERM_MQTT}, {"ha", NV_WPERM_HA}, {"camera", NV_WPERM_CAMERA}, {"mic", NV_WPERM_MIC},
};

constexpr char kDir[]  = "/sdcard/nucleos";
constexpr char kFile[] = "/sdcard/nucleos/perms.json";
constexpr int  kMax    = 64;
constexpr long kFileCap = 8 * 1024;

struct Entry { char id[32]; uint32_t mask; };
NV_PSRAM_BSS Entry s_tab[kMax];
int               s_n = 0;
bool              s_loaded = false;
SemaphoreHandle_t s_mx = nullptr;

void lock(void) {
    if (!s_mx) s_mx = xSemaphoreCreateMutex();   // first use is at boot (single-threaded scan)
    if (s_mx) xSemaphoreTake(s_mx, portMAX_DELAY);
}
void unlock(void) { if (s_mx) xSemaphoreGive(s_mx); }

void load_locked(void) {
    if (s_loaded || !nv_sd_is_mounted()) return;
    s_loaded = true;
    s_n = 0;
    FILE *f = nv_sd_fopen(kFile, "rb");
    if (!f) return;
    char *buf = (char *)heap_caps_malloc(kFileCap + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    const size_t n = buf ? fread(buf, 1, kFileCap, f) : 0;
    nv_sd_fclose(f);
    if (!buf) return;
    buf[n] = '\0';
    cJSON *root = cJSON_Parse(buf);
    free(buf);
    if (!cJSON_IsObject(root)) { cJSON_Delete(root); NV_LOGW(TAG, "perms.json unreadable, ignored"); return; }
    const cJSON *it = nullptr;
    cJSON_ArrayForEach(it, root) {
        if (s_n >= kMax || !it->string || strlen(it->string) >= sizeof s_tab[0].id || !cJSON_IsNumber(it))
            continue;
        const uint32_t m = (uint32_t)it->valuedouble & NV_WPERM_SENSITIVE;
        if (!m) continue;
        snprintf(s_tab[s_n].id, sizeof s_tab[0].id, "%s", it->string);
        s_tab[s_n++].mask = m;
    }
    cJSON_Delete(root);
}

void save_locked(void) {
    mkdir(kDir, 0777);
    char tmp[64];
    snprintf(tmp, sizeof tmp, "%s.tmp", kFile);
    FILE *f = nv_sd_fopen(tmp, "wb");
    if (!f) { NV_LOGE(TAG, "cannot write %s", tmp); return; }
    fputc('{', f);
    for (int i = 0; i < s_n; i++)
        fprintf(f, "%s\"%s\":%lu", i ? "," : "", s_tab[i].id, (unsigned long)s_tab[i].mask);
    fputs("}\n", f);
    const bool werr = ferror(f) != 0;
    if (nv_sd_fclose(f) != 0 || werr) { unlink(tmp); NV_LOGE(TAG, "write %s failed, old grants kept", tmp); return; }
    unlink(kFile);   // FATFS rename won't overwrite
    if (rename(tmp, kFile) != 0) NV_LOGE(TAG, "rename -> %s failed, grants left in %s", kFile, tmp);
}

bool id_ok(const char *id) {
    if (!id || !id[0] || strlen(id) > 31) return false;
    for (const char *p = id; *p; p++)
        if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') ||
              *p == '-' || *p == '_'))
            return false;
    return true;
}

}  // namespace

uint32_t nv_wasm_perm_bit(const char *name) {
    if (!name) return 0;
    for (const auto &n : kNames) if (!strcmp(n.name, name)) return n.bit;
    return 0;
}

const char *nv_wasm_perm_name(uint32_t bit) {
    for (const auto &n : kNames) if (n.bit == bit) return n.name;
    return nullptr;
}

uint32_t nv_wasm_perm_revoked(const char *app_id) {
    if (!id_ok(app_id)) return 0;
    lock();
    load_locked();
    uint32_t m = 0;
    for (int i = 0; i < s_n; i++) if (!strcmp(s_tab[i].id, app_id)) { m = s_tab[i].mask; break; }
    unlock();
    return m;
}

void nv_wasm_perm_set_revoked(const char *app_id, uint32_t mask) {
    if (!id_ok(app_id)) return;
    mask &= NV_WPERM_SENSITIVE;
    lock();
    load_locked();
    int at = -1;
    for (int i = 0; i < s_n; i++) if (!strcmp(s_tab[i].id, app_id)) { at = i; break; }
    if (at < 0 && mask) {
        if (s_n >= kMax) { unlock(); NV_LOGW(TAG, "revocation table full"); return; }
        at = s_n++;
        snprintf(s_tab[at].id, sizeof s_tab[0].id, "%s", app_id);
    }
    if (at >= 0) {
        if (mask) s_tab[at].mask = mask;
        else      s_tab[at] = s_tab[--s_n];          // nothing revoked any more: drop the row
        save_locked();
        NV_LOGI(TAG, "%s: revoked 0x%lx", app_id, (unsigned long)mask);
    }
    unlock();
}
