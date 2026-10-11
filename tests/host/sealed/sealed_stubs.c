// Host stand-ins for what nv_sealed.cpp links against: settings encryption off (files stay plain),
// PSRAM = malloc, GCM always fails.
#include "nv_config.h"
#include "esp_random.h"
#include "mbedtls/gcm.h"
#include "esp_heap_caps.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

bool nv_config_encrypted(void) { return false; }
bool nv_config_device_key(const char *label, uint8_t out[32]) { (void)label; memset(out, 0, 32); return false; }
void esp_fill_random(void *buf, size_t len) { memset(buf, 0x5a, len); }
void *heap_caps_malloc(size_t n, uint32_t caps) { (void)caps; return malloc(n); }
void heap_caps_free(void *p) { free(p); }
void mbedtls_gcm_init(mbedtls_gcm_context *c) { (void)c; }
void mbedtls_gcm_free(mbedtls_gcm_context *c) { (void)c; }
int mbedtls_gcm_setkey(mbedtls_gcm_context *c, int ci, const unsigned char *k, unsigned int b) { (void)c; (void)ci; (void)k; (void)b; return -1; }
int mbedtls_gcm_crypt_and_tag(mbedtls_gcm_context *c, int m, size_t l, const unsigned char *iv, size_t ivl,
                              const unsigned char *a, size_t al, const unsigned char *in, unsigned char *out,
                              size_t tl, unsigned char *t) { (void)c; (void)m; (void)l; (void)iv; (void)ivl; (void)a; (void)al; (void)in; (void)out; (void)tl; (void)t; return -1; }
int mbedtls_gcm_auth_decrypt(mbedtls_gcm_context *c, size_t l, const unsigned char *iv, size_t ivl,
                             const unsigned char *a, size_t al, const unsigned char *t, size_t tl,
                             const unsigned char *in, unsigned char *out) { (void)c; (void)l; (void)iv; (void)ivl; (void)a; (void)al; (void)t; (void)tl; (void)in; (void)out; return -1; }
