// nv_wasm_dl — ABI v18: the system downloader (nv_dl) for WASM apps. See nv_wasm.h for the import table
// and docs/ZIM_READER_PLAN.md §4f.
//
//  - Permission "net"; the file goes where the app itself can see it: "home" -> the shared workspace
//    (/sdcard/home), else "fs" -> the app's data folder. The app passes the path it knows ("/zim/x.zim").
//  - An app sees and controls only its own downloads (owner = its id). They outlive the run: a download
//    started by an app goes on after it closes and after a restart.
//  - Answers are JSON (cJSON), paths given back in the app's own view.
#include "nv_wasm_net.h"
#include "nv_wasm.h"
#include "nv_dl.h"
#include "nv_log.h"

#include <cstdio>
#include <cstring>

#include "cJSON.h"
#include "esp_heap_caps.h"

namespace {

// Errors to the guest (mirrored in sdk/include/nucleo_sdk.h NV_DL_E_*).
enum {
    E_PERM = -1,      // no "net", or neither "home" nor "fs"
    E_ARG = -2,       // bad spec / id / path, or a download of another app
    E_FULL = -3,      // too many downloads listed
    E_DENIED = -4,    // a path the system keeps for itself
    E_EXISTS = -5,    // the file is there already, or is being downloaded
    E_SD = -6,        // no card
    E_SMALL = -7,     // the answer doesn't fit the buffer
};

constexpr int kSpecMax = 1536;

// The card folder the app's "/" is: its view of the files.
bool root_of(wasm_exec_env_t env, char *out, size_t n) {
    const uint32_t perms = nv_wasm_env_perms(env);
    const char *id = nv_wasm_env_app_id(env);
    if (!(perms & NV_WPERM_NET) || !id || !id[0]) return false;
    if (perms & NV_WPERM_HOME) snprintf(out, n, "/sdcard/home");
    else if (perms & NV_WPERM_FS) snprintf(out, n, "/sdcard/apps/%s/data", id);
    else return false;
    return true;
}

const char *kStates[] = {"queued", "paused", "done", "failed", "running"};

cJSON *to_json(const nv_dl_info_t &d, const char *root) {
    cJSON *o = cJSON_CreateObject();
    if (!o) return nullptr;
    const size_t rl = strlen(root);
    cJSON_AddNumberToObject(o, "id", d.id);
    cJSON_AddStringToObject(o, "state", kStates[d.state >= 0 && d.state < 5 ? d.state : 0]);
    cJSON_AddStringToObject(o, "title", d.title);
    cJSON_AddStringToObject(o, "dest", !strncmp(d.dest, root, rl) ? d.dest + rl : "");
    cJSON_AddNumberToObject(o, "size", (double)d.size);
    cJSON_AddNumberToObject(o, "done", (double)d.done);
    cJSON_AddNumberToObject(o, "verified", (double)d.verified);
    cJSON_AddNumberToObject(o, "bps", d.bps);
    cJSON_AddStringToObject(o, "phase", d.phase);
    cJSON_AddStringToObject(o, "error", d.error);
    return o;
}

int32_t emit(cJSON *j, char *buf, uint32_t cap) {
    if (!j) return E_SMALL;
    const bool ok = cap > 1 && cJSON_PrintPreallocated(j, buf, (int)cap, false);
    cJSON_Delete(j);
    return ok ? (int32_t)strlen(buf) : E_SMALL;
}

// The download `id` if it belongs to the calling app.
bool mine(wasm_exec_env_t env, int32_t id, nv_dl_info_t *d) {
    const char *app = nv_wasm_env_app_id(env);
    return id > 0 && nv_dl_get((uint32_t)id, d) && app && !strcmp(d->owner, app);
}

// nv.dl_start(spec) -> id
int32_t w_dl_start(wasm_exec_env_t env, const char *spec) {
    char root[64];
    if (!root_of(env, root, sizeof root)) return E_PERM;
    if (!spec || strnlen(spec, kSpecMax + 1) > kSpecMax) return E_ARG;
    cJSON *j = cJSON_Parse(spec);
    if (!j) return E_ARG;
    const cJSON *meta4 = cJSON_GetObjectItem(j, "meta4"), *url = cJSON_GetObjectItem(j, "url");
    const cJSON *dest = cJSON_GetObjectItem(j, "dest"), *title = cJSON_GetObjectItem(j, "title");
    const cJSON *size = cJSON_GetObjectItem(j, "size"), *sha = cJSON_GetObjectItem(j, "sha256");
    char path[NVDL_PATH_MAX];
    int32_t rc = E_ARG;
    if (cJSON_IsString(dest) && nvdl_guest_path(root, dest->valuestring, path, sizeof path) &&
        (cJSON_IsString(meta4) != cJSON_IsString(url))) {
        nv_dl_req_t q = {};
        q.owner = nv_wasm_env_app_id(env);
        q.title = cJSON_IsString(title) ? title->valuestring : nullptr;
        q.dest = path;
        if (cJSON_IsString(meta4)) q.meta4 = meta4->valuestring;
        else {
            q.url = url->valuestring;
            q.size = cJSON_IsNumber(size) && size->valuedouble > 0 ? (uint64_t)size->valuedouble : 0;
            q.sha256 = cJSON_IsString(sha) ? sha->valuestring : nullptr;
        }
        char err[96];
        rc = nv_dl_add(&q, err, sizeof err);
        if (rc < 0) {
            NV_LOGW("dl", "%s: %s", q.owner, err);
            rc = rc == NV_DL_EFULL ? E_FULL : rc == NV_DL_EEXISTS ? E_EXISTS : rc == NV_DL_ESD ? E_SD
               : rc == NV_DL_EDENIED ? E_DENIED : E_ARG;
        }
    }
    cJSON_Delete(j);
    return rc;
}

// nv.dl_info(id, buf, cap) -> bytes of JSON
int32_t w_dl_info(wasm_exec_env_t env, int32_t id, char *buf, uint32_t cap) {
    char root[64];
    if (!root_of(env, root, sizeof root)) return E_PERM;
    nv_dl_info_t d;
    if (!mine(env, id, &d)) return E_ARG;
    return emit(to_json(d, root), buf, cap);
}

// nv.dl_list(buf, cap) -> bytes of JSON (an array, oldest first)
int32_t w_dl_list(wasm_exec_env_t env, char *buf, uint32_t cap) {
    char root[64];
    if (!root_of(env, root, sizeof root)) return E_PERM;
    auto *l = (nv_dl_info_t *)heap_caps_malloc(sizeof(nv_dl_info_t) * NV_DL_MAX, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!l) return E_SMALL;
    const int n = nv_dl_list(nv_wasm_env_app_id(env), l, NV_DL_MAX);
    cJSON *a = cJSON_CreateArray();
    for (int i = 0; a && i < n; i++)
        if (cJSON *o = to_json(l[i], root)) cJSON_AddItemToArray(a, o);
    heap_caps_free(l);
    return emit(a, buf, cap);
}

// nv.dl_pause(id, on) -> 1 done, 0 not in a state for it
int32_t w_dl_pause(wasm_exec_env_t env, int32_t id, int32_t on) {
    char root[64];
    if (!root_of(env, root, sizeof root)) return E_PERM;
    nv_dl_info_t d;
    if (!mine(env, id, &d)) return E_ARG;
    return nv_dl_pause((uint32_t)id, on != 0) ? 1 : 0;
}

// nv.dl_cancel(id) -> 1: stops it and deletes what it wrote (a finished file stays, only forgotten)
int32_t w_dl_cancel(wasm_exec_env_t env, int32_t id) {
    char root[64];
    if (!root_of(env, root, sizeof root)) return E_PERM;
    nv_dl_info_t d;
    if (!mine(env, id, &d)) return E_ARG;
    return nv_dl_cancel((uint32_t)id) ? 1 : 0;
}

// nv.dl_free() -> MiB a new download may use (free space less the OTA reserve and what the listed
// downloads still need), or -6 without a card
int32_t w_dl_free(wasm_exec_env_t env) {
    char root[64];
    if (!root_of(env, root, sizeof root)) return E_PERM;
    const int64_t b = nv_dl_room();
    if (b < 0) return E_SD;
    const int64_t mb = b >> 20;
    return mb > 0x7fffffff ? 0x7fffffff : (int32_t)mb;
}

NativeSymbol s_natives[] = {
    {"dl_start",  (void *)w_dl_start,  "($)i",   nullptr},
    {"dl_info",   (void *)w_dl_info,   "(i*~)i", nullptr},
    {"dl_list",   (void *)w_dl_list,   "(*~)i",  nullptr},
    {"dl_pause",  (void *)w_dl_pause,  "(ii)i",  nullptr},
    {"dl_cancel", (void *)w_dl_cancel, "(i)i",   nullptr},
    {"dl_free",   (void *)w_dl_free,   "()i",    nullptr},
};

}  // namespace

void nv_wasm_dl_register(void) {
    if (!wasm_runtime_register_natives("nv", s_natives, sizeof s_natives / sizeof s_natives[0]))
        NV_LOGE("dl", "register ABI 18 natives failed");
}
