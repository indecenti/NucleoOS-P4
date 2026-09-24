// DES block encryption for RFB VNC authentication (see ss_des.c).
#pragma once
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void ss_des_encrypt(const uint8_t key[8], const uint8_t in[8], uint8_t out[8]);
// 16-byte response to an RFB VNC-auth challenge.
void ss_vnc_auth_response(const char *password, const uint8_t challenge[16], uint8_t response[16]);
bool ss_des_selftest(void);   // FIPS KAT + a VNC-auth known answer

#ifdef __cplusplus
}
#endif
