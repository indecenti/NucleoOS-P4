// Host stand-in for mbedtls/gcm.h: the path tests run with settings encryption off, so nothing is
// sealed; these only have to link (sealed_stubs.c fails them).
#pragma once
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct { int unused; } mbedtls_gcm_context;
#define MBEDTLS_CIPHER_ID_AES 2
#define MBEDTLS_GCM_ENCRYPT 1
void mbedtls_gcm_init(mbedtls_gcm_context *ctx);
void mbedtls_gcm_free(mbedtls_gcm_context *ctx);
int mbedtls_gcm_setkey(mbedtls_gcm_context *ctx, int cipher, const unsigned char *key, unsigned int keybits);
int mbedtls_gcm_crypt_and_tag(mbedtls_gcm_context *ctx, int mode, size_t length, const unsigned char *iv,
                              size_t iv_len, const unsigned char *add, size_t add_len, const unsigned char *input,
                              unsigned char *output, size_t tag_len, unsigned char *tag);
int mbedtls_gcm_auth_decrypt(mbedtls_gcm_context *ctx, size_t length, const unsigned char *iv, size_t iv_len,
                             const unsigned char *add, size_t add_len, const unsigned char *tag, size_t tag_len,
                             const unsigned char *input, unsigned char *output);
#ifdef __cplusplus
}
#endif
