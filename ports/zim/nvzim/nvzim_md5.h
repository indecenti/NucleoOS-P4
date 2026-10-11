// nvzim_md5.h — MD5 (RFC 1321) for nvzim_verify: the checksum at the end of every ZIM file.
#pragma once
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t h[4];
    uint64_t len;
    uint8_t buf[64];
} nvzim_md5_t;

void nvzim_md5_init(nvzim_md5_t *c);
void nvzim_md5_update(nvzim_md5_t *c, const void *data, size_t n);
void nvzim_md5_final(nvzim_md5_t *c, uint8_t out[16]);

#ifdef __cplusplus
}
#endif
