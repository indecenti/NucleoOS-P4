// nv_fwup — firmware update core shared by NucleoOS (nv_ota) and the recovery app (recovery/).
//
// Layout v2 (partitions.csv): `recovery` (ota_1, 1 MB) + `system` (ota_0, 10 MB). A new image is staged on the SD
// card and installed by recovery, which also rolls back from the saved copy of the previous system.
// Everything that decides whether bytes may reach the system slot lives here, so the two apps can't
// disagree on it:
//   * signed manifests: ECDSA P-256 by the release key (tools/ota_sign.py) over
//     "nucleoos-ota-v1\n<version>\n<sha256>\n<size>\n" (nv_ota_manifest.h);
//   * the image file on SD must hash to the signed sha256/size BEFORE the slot is touched;
//   * after writing, the slot is read back and hashed again, and its app descriptor must carry the
//     signed version.
//
// SD staging area (8.3 names, FAT): <mount>/nvupd/
//   next.bin + next.jsn  image to install + its signed manifest (written last: the "go" signal)
//   prev.bin + prev.jsn  copy of the system that was running before, for rollback
//   cur.jsn              signed manifest of the installed system (makes the next backup possible)
//   tries.txt            install attempts for next.jsn (recovery gives up after kMaxTries)
//   result.jsn           what recovery did last time ({"op":..,"ok":..,"from":..,"to":..,"why":..})
//
// No dependency on the rest of NucleoOS: only ESP-IDF (app_update, esp_partition, mbedtls, json).
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_partition.h"

#ifdef __cplusplus
extern "C" {
#endif

#define NV_FWUP_DIR        "nvupd"
#define NV_FWUP_NEXT_BIN   "next.bin"
#define NV_FWUP_NEXT_MAN   "next.jsn"
#define NV_FWUP_PREV_BIN   "prev.bin"
#define NV_FWUP_PREV_MAN   "prev.jsn"
#define NV_FWUP_CUR_MAN    "cur.jsn"
#define NV_FWUP_TRIES      "tries.txt"
#define NV_FWUP_RESULT     "result.jsn"
#define NV_FWUP_MAX_TRIES  3

#define NV_FWUP_LABEL_RECOVERY "recovery"
#define NV_FWUP_LABEL_SYSTEM   "system"

typedef struct {
    char     version[32];
    char     sha256_hex[65];
    uint8_t  sha256[32];
    uint32_t size;
    char     sig_hex[161];   // DER ECDSA signature, hex (<= 80 bytes)
} nv_fwup_manifest_t;

typedef enum {
    NV_FWUP_OK = 0,
    NV_FWUP_E_FIELDS,      // manifest fields missing or malformed
    NV_FWUP_E_SIGNATURE,   // not signed by the release key
    NV_FWUP_E_IO,          // file / flash I/O error
    NV_FWUP_E_SIZE,        // file size differs from the manifest, or bigger than the slot
    NV_FWUP_E_HASH,        // bytes do not hash to the signed sha256
    NV_FWUP_E_IMAGE,       // ESP image validation failed (magic/segments/checksum)
    NV_FWUP_E_VERSION,     // image app descriptor version != signed version
    NV_FWUP_E_NOMEM,
} nv_fwup_err_t;

const char *nv_fwup_err_str(nv_fwup_err_t e);   // short English reason

// Progress 0..100. Called from the worker doing the job.
typedef void (*nv_fwup_progress_cb)(int pct, void *user);

// ---- layout ----
const esp_partition_t *nv_fwup_recovery_part(void);   // NULL on a layout-v1 device
const esp_partition_t *nv_fwup_system_part(void);
bool nv_fwup_layout_v2(void);                        // both partitions present

// "<mount>/nvupd/<name>" into out. Returns out.
char *nv_fwup_path(char *out, size_t n, const char *mount, const char *name);
// mkdir <mount>/nvupd (ok if it exists).
bool nv_fwup_ensure_dir(const char *mount);

// ---- manifests ----
// Validate + verify a manifest JSON text ({"version","sha256","size","sig",...}). max_size caps the
// image size (pass the system slot size). Extra fields (url, notes) are ignored.
nv_fwup_err_t nv_fwup_manifest_parse(const char *json, uint32_t max_size, nv_fwup_manifest_t *out);
nv_fwup_err_t nv_fwup_manifest_load(const char *path, uint32_t max_size, nv_fwup_manifest_t *out);
// Write {"version","sha256","size","sig"} atomically (tmp file + rename).
bool nv_fwup_manifest_save(const char *path, const nv_fwup_manifest_t *m);

// ---- hashing / flashing ----
nv_fwup_err_t nv_fwup_hash_file(const char *path, const nv_fwup_manifest_t *m,
                                nv_fwup_progress_cb cb, void *user);   // size + sha256 vs manifest
nv_fwup_err_t nv_fwup_hash_partition(const esp_partition_t *p, const nv_fwup_manifest_t *m,
                                     nv_fwup_progress_cb cb, void *user);
// Verify the file, write it into `dst` (esp_ota_begin/write/end: erases, validates the image), read it
// back and hash it, and check the descriptor version. Does NOT move the boot pointer.
nv_fwup_err_t nv_fwup_install(const char *path, const nv_fwup_manifest_t *m, const esp_partition_t *dst,
                              nv_fwup_progress_cb cb, void *user);
// Copy the first m->size bytes of `src` to `path` (tmp + rename) after checking they hash to `m`.
nv_fwup_err_t nv_fwup_backup(const esp_partition_t *src, const nv_fwup_manifest_t *m, const char *path,
                             nv_fwup_progress_cb cb, void *user);

// ---- small state files ----
int  nv_fwup_tries_get(const char *mount);
void nv_fwup_tries_set(const char *mount, int n);   // n <= 0 removes the file
// result.jsn: op = "install" | "rollback" | "restore"; why may be NULL.
void nv_fwup_result_write(const char *mount, const char *op, bool ok, const char *from, const char *to,
                          const char *why);

// "A.B.C" strictly newer than cur.
bool nv_fwup_version_newer(const char *cand, const char *cur);

#ifdef __cplusplus
}
#endif
