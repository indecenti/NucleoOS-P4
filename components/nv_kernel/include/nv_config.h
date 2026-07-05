// nv_config — NucleoOS Anima config store (Phase 3: NVS-backed key/value + live-apply event).
// Every set() persists to NVS and publishes NV_EV_SETTINGS_CHANGED with the key, so the OS
// applies changes immediately. (Secrets get encrypted NVS later; this is plain prefs.)
#pragma once
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

void nv_config_init(void);

int  nv_config_get_int(const char *key, int def);
void nv_config_set_int(const char *key, int value);
bool nv_config_get_bool(const char *key, bool def);
void nv_config_set_bool(const char *key, bool value);

// String values (NVS-backed). get copies up to n-1 bytes into out (always NUL-terminated);
// when the key is absent it copies def ("" if def is NULL). set persists and publishes
// NV_EV_SETTINGS_CHANGED. Safe no-ops when the store is unavailable.
void nv_config_get_str(const char *key, const char *def, char *out, size_t n);
void nv_config_set_str(const char *key, const char *value);

#ifdef __cplusplus
}
#endif
