// Host stand-in for nv_config.h: only what nv_sealed.cpp uses (tests/host/sealed/sealed_stubs.c).
#pragma once
#include <stdbool.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
bool nv_config_encrypted(void);
bool nv_config_device_key(const char *label, uint8_t out[32]);
#ifdef __cplusplus
}
#endif
