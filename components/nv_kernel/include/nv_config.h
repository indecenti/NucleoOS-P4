// nv_config — NucleoOS Anima config store (Phase 3: NVS-backed key/value + live-apply event).
// Every set() persists to NVS and publishes NV_EV_SETTINGS_CHANGED with the key, so the OS
// applies changes immediately. On firmware built with NVS encryption the whole store (Wi-Fi
// networks, tokens, PINs included) is encrypted with keys derived from an eFuse HMAC key.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void nv_config_init(void);

// true when the NVS store is encrypted (XTS-AES; keys derived from a chip-unique HMAC key burnt in
// eFuse on the first boot, which never leaves the chip).
bool nv_config_encrypted(void);
// A 32-byte device-unique key for `label`, derived from that same eFuse key. Other components use
// it to encrypt what they keep outside NVS (the SD settings mirror). false when not encrypted.
bool nv_config_device_key(const char *label, uint8_t out[32]);

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
