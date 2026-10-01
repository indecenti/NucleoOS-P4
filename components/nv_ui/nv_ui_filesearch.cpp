// nv_ui_filesearch — the file half of the Start menu search: an index of the file names on the SD
// card, built on the background worker and searched on the LVGL thread.
//
// The index is one PSRAM block of NUL-separated full paths. A rebuild writes a new block off the
// UI thread and swaps it in under the LVGL lock, so a search never sees a half-built index. System
// and app-private folders are skipped: the user's own files only.
#include "nv_ui_internal.h"

#include "nv_bgwork.h"
#include "nv_log.h"
#include "nv_sd.h"
#include "nv_mem_attr.h"
#include "nv_ui_kit.h"   // nv_kit_find_ci

#include "esp_heap_caps.h"
#include "esp_lvgl_port.h"

#include <dirent.h>
#include <stdlib.h>
#include <string.h>

namespace {

const char *TAG = "fsearch";

constexpr size_t   kPoolMax  = 384 * 1024;   // paths, NUL-separated
constexpr int      kMaxFiles = 8000;
constexpr int      kMaxDepth = 8;
constexpr uint32_t kStaleMs  = 5 * 60 * 1000;

struct Index { char *pool; size_t used; int n; };

NV_PSRAM_BSS Index s_idx;             // live index (LVGL thread reads, swap under the LVGL lock)
volatile bool s_building = false;
NV_PSRAM_BSS uint32_t s_built_ms;

// Folders that hold the system, app packages, caches or web assets — not the user's files.
bool skip_dir(const char *name) {
    if (name[0] == '.') return true;
    static const char *const kSkip[] = {"System Volume Information", "system", "web", "nucleos",
                                        "NUCLEOS", "APPS", "apps", "data", "tmp", "sdcard"};
    for (const char *k : kSkip) if (!strcmp(name, k)) return true;
    return false;
}

void walk(Index &ix, char *path, size_t len, int depth) {
    if (depth > kMaxDepth || ix.n >= kMaxFiles) return;
    DIR *d = opendir(path);
    if (!d) return;
    while (struct dirent *e = readdir(d)) {
        if (ix.n >= kMaxFiles) break;
        const size_t nl = strlen(e->d_name);
        if (!nl || len + 1 + nl >= 255) continue;
        path[len] = '/';
        memcpy(path + len + 1, e->d_name, nl + 1);
        if (e->d_type == DT_DIR) {
            if (!skip_dir(e->d_name)) walk(ix, path, len + 1 + nl, depth + 1);
        } else if (ix.used + len + nl + 2 <= kPoolMax) {
            memcpy(ix.pool + ix.used, path, len + 1 + nl + 1);
            ix.used += len + 1 + nl + 1;
            ix.n++;
        }
        path[len] = 0;
    }
    closedir(d);
}

void build_job(void *) {
    Index ix = {};
    ix.pool = (char *)heap_caps_malloc(kPoolMax, MALLOC_CAP_SPIRAM);
    if (ix.pool && nv_sd_is_mounted()) {
        char path[256] = "/sdcard";
        walk(ix, path, strlen(path), 0);
    }
    char *old = nullptr;
    if (lvgl_port_lock(2000)) {
        old = s_idx.pool;
        s_idx = ix;
        s_built_ms = lv_tick_get();
        lvgl_port_unlock();
    } else {
        old = ix.pool;                            // UI busy for 2 s: keep the old index
    }
    free(old);
    NV_LOGI(TAG, "indexed %d files (%u KB)", ix.n, (unsigned)(ix.used / 1024));
    s_building = false;
}

}  // namespace

namespace nvsearch {

void refresh(void) {
    if (s_building) return;
    if (s_idx.pool && lv_tick_elaps(s_built_ms) < kStaleMs) return;
    s_building = true;
    if (!nv_bgwork_submit(build_job, nullptr)) s_building = false;
}

bool ready(void) { return s_idx.pool != nullptr; }

int find(const char *q, const char **out, int max) {
    if (!q || !q[0] || !s_idx.pool) return 0;
    // Two passes: names that start with the query first, then the ones that contain it.
    int n = 0;
    for (int pass = 0; pass < 2 && n < max; pass++) {
        const char *p = s_idx.pool;
        for (int i = 0; i < s_idx.n && n < max; i++) {
            const char *base = strrchr(p, '/');
            base = base ? base + 1 : p;
            const int at = nv_kit_find_ci(base, q);
            if ((pass == 0 && at == 0) || (pass == 1 && at > 0)) out[n++] = p;
            p += strlen(p) + 1;
        }
    }
    return n;
}

}  // namespace nvsearch
