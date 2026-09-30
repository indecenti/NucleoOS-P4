// nv_sealed — secret files on the SD card, sealed to this chip.
//
// The SD card is removable plain FAT: whatever sits on it in clear is one card reader away. Files that
// hold secrets (today: /sdcard/data/anima/teacher.json, the paid LLM API keys) are stored as
// "NVX1" | iv[12] | tag[16] | AES-256-GCM(content), under a key derived from the eFuse HMAC key that
// also protects NVS (nv_config_device_key): only this chip can open them, a copied card yields
// nothing. Readers get the plaintext back transparently; a plaintext file found at a secret path is
// sealed in place on its first read (migration). Without settings encryption (no eFuse key) files
// stay plaintext, exactly as before.
#pragma once
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// true when `path` (a physical path, "/sdcard/...") is a secret file. Compared the way FatFs resolves
// names: case-insensitive, trailing dots/spaces of each component ignored.
bool nv_sealed_path(const char *path);

// Whole file as a NUL-terminated buffer (PSRAM; release with free()). `len` (optional) gets the
// plaintext length. NULL when absent, empty, over `max` bytes, or sealed by another chip / damaged.
char *nv_sealed_read(const char *path, size_t max, size_t *len);

// Write `data` atomically (tmp + rename), sealed when settings encryption is on, else plaintext.
bool nv_sealed_write(const char *path, const void *data, size_t len);

#ifdef __cplusplus
}
#endif
