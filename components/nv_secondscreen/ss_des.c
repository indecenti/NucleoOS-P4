// DES encryption (ECB, one block) for RFB "VNC Authentication": the only place DES is needed,
// so it stays here instead of enabling MBEDTLS_DES_C for the whole firmware. Tables are the
// FIPS 46-3 ones (validated against a reference implementation + KAT; see ss_des_selftest()).
#include "ss_des.h"
#include <string.h>

static const uint8_t kIP[64] = {58,50,42,34,26,18,10,2,60,52,44,36,28,20,12,4,62,54,46,38,30,22,14,6,64,56,48,40,32,24,16,8,57,49,41,33,25,17,9,1,59,51,43,35,27,19,11,3,61,53,45,37,29,21,13,5,63,55,47,39,31,23,15,7};
static const uint8_t kFP[64] = {40,8,48,16,56,24,64,32,39,7,47,15,55,23,63,31,38,6,46,14,54,22,62,30,37,5,45,13,53,21,61,29,36,4,44,12,52,20,60,28,35,3,43,11,51,19,59,27,34,2,42,10,50,18,58,26,33,1,41,9,49,17,57,25};
static const uint8_t kE[48] = {32,1,2,3,4,5,4,5,6,7,8,9,8,9,10,11,12,13,12,13,14,15,16,17,16,17,18,19,20,21,20,21,22,23,24,25,24,25,26,27,28,29,28,29,30,31,32,1};
static const uint8_t kP[32] = {16,7,20,21,29,12,28,17,1,15,23,26,5,18,31,10,2,8,24,14,32,27,3,9,19,13,30,6,22,11,4,25};
static const uint8_t kPC1[56] = {57,49,41,33,25,17,9,1,58,50,42,34,26,18,10,2,59,51,43,35,27,19,11,3,60,52,44,36,63,55,47,39,31,23,15,7,62,54,46,38,30,22,14,6,61,53,45,37,29,21,13,5,28,20,12,4};
static const uint8_t kPC2[48] = {14,17,11,24,1,5,3,28,15,6,21,10,23,19,12,4,26,8,16,7,27,20,13,2,41,52,31,37,47,55,30,40,51,45,33,48,44,49,39,56,34,53,46,42,50,36,29,32};
static const uint8_t kSH[16] = {1,1,2,2,2,2,2,2,1,2,2,2,2,2,2,1};
static const uint8_t kS[8][64] = {
    {14,4,13,1,2,15,11,8,3,10,6,12,5,9,0,7,0,15,7,4,14,2,13,1,10,6,12,11,9,5,3,8,4,1,14,8,13,6,2,11,15,12,9,7,3,10,5,0,15,12,8,2,4,9,1,7,5,11,3,14,10,0,6,13},
    {15,1,8,14,6,11,3,4,9,7,2,13,12,0,5,10,3,13,4,7,15,2,8,14,12,0,1,10,6,9,11,5,0,14,7,11,10,4,13,1,5,8,12,6,9,3,2,15,13,8,10,1,3,15,4,2,11,6,7,12,0,5,14,9},
    {10,0,9,14,6,3,15,5,1,13,12,7,11,4,2,8,13,7,0,9,3,4,6,10,2,8,5,14,12,11,15,1,13,6,4,9,8,15,3,0,11,1,2,12,5,10,14,7,1,10,13,0,6,9,8,7,4,15,14,3,11,5,2,12},
    {7,13,14,3,0,6,9,10,1,2,8,5,11,12,4,15,13,8,11,5,6,15,0,3,4,7,2,12,1,10,14,9,10,6,9,0,12,11,7,13,15,1,3,14,5,2,8,4,3,15,0,6,10,1,13,8,9,4,5,11,12,7,2,14},
    {2,12,4,1,7,10,11,6,8,5,3,15,13,0,14,9,14,11,2,12,4,7,13,1,5,0,15,10,3,9,8,6,4,2,1,11,10,13,7,8,15,9,12,5,6,3,0,14,11,8,12,7,1,14,2,13,6,15,0,9,10,4,5,3},
    {12,1,10,15,9,2,6,8,0,13,3,4,14,7,5,11,10,15,4,2,7,12,9,5,6,1,13,14,0,11,3,8,9,14,15,5,2,8,12,3,7,0,4,10,1,13,11,6,4,3,2,12,9,5,15,10,11,14,1,7,6,0,8,13},
    {4,11,2,14,15,0,8,13,3,12,9,7,5,10,6,1,13,0,11,7,4,9,1,10,14,3,5,12,2,15,8,6,1,4,11,13,12,3,7,14,10,15,6,8,0,5,9,2,6,11,13,8,1,4,10,7,9,5,0,15,14,2,3,12},
    {13,2,8,4,6,15,11,1,10,9,3,14,5,0,12,7,1,15,13,8,10,3,7,4,12,5,6,11,0,14,9,2,7,11,4,1,9,12,14,2,0,6,10,13,15,3,5,8,2,1,14,7,4,10,8,13,15,12,9,0,3,5,6,11},
};

static uint64_t perm(uint64_t v, const uint8_t *t, int n, int nin) {
    uint64_t o = 0;
    for (int i = 0; i < n; i++) o = (o << 1) | ((v >> (nin - t[i])) & 1u);
    return o;
}

void ss_des_encrypt(const uint8_t key[8], const uint8_t in[8], uint8_t out[8]) {
    uint64_t k = 0, b = 0;
    for (int i = 0; i < 8; i++) { k = (k << 8) | key[i]; b = (b << 8) | in[i]; }
    const uint64_t cd = perm(k, kPC1, 56, 64);
    uint32_t c = (uint32_t)(cd >> 28), d = (uint32_t)(cd & 0xFFFFFFFu);
    uint64_t sub[16];
    for (int r = 0; r < 16; r++) {
        const int s = kSH[r];
        c = ((c << s) | (c >> (28 - s))) & 0xFFFFFFFu;
        d = ((d << s) | (d >> (28 - s))) & 0xFFFFFFFu;
        sub[r] = perm(((uint64_t)c << 28) | d, kPC2, 48, 56);
    }
    const uint64_t v = perm(b, kIP, 64, 64);
    uint32_t l = (uint32_t)(v >> 32), rr = (uint32_t)v;
    for (int r = 0; r < 16; r++) {
        const uint64_t x = perm(rr, kE, 48, 32) ^ sub[r];
        uint32_t f = 0;
        for (int i = 0; i < 8; i++) {
            const unsigned six = (unsigned)(x >> (42 - 6 * i)) & 0x3Fu;
            const unsigned row = ((six >> 4) & 2u) | (six & 1u);
            const unsigned col = (six >> 1) & 0xFu;
            f = (f << 4) | kS[i][row * 16 + col];
        }
        const uint32_t nl = rr;
        rr = l ^ (uint32_t)perm(f, kP, 32, 32);
        l = nl;
    }
    const uint64_t o = perm(((uint64_t)rr << 32) | l, kFP, 64, 64);
    for (int i = 0; i < 8; i++) out[i] = (uint8_t)(o >> (56 - 8 * i));
}

void ss_vnc_auth_response(const char *password, const uint8_t challenge[16], uint8_t response[16]) {
    // RFB quirk: the 8-byte key is the password (NUL-padded/truncated) with each byte bit-reversed.
    uint8_t key[8] = {0};
    for (int i = 0; i < 8 && password && password[i]; i++) {
        uint8_t v = (uint8_t)password[i], r = 0;
        for (int b = 0; b < 8; b++) if (v & (1u << b)) r |= (uint8_t)(0x80u >> b);
        key[i] = r;
    }
    ss_des_encrypt(key, challenge, response);
    ss_des_encrypt(key, challenge + 8, response + 8);
}

bool ss_des_selftest(void) {
    static const uint8_t k[8] = {0x13, 0x34, 0x57, 0x79, 0x9B, 0xBC, 0xDF, 0xF1};
    static const uint8_t p[8] = {0x01, 0x23, 0x45, 0x67, 0x89, 0xAB, 0xCD, 0xEF};
    static const uint8_t c[8] = {0x85, 0xE8, 0x13, 0x54, 0x0F, 0x0A, 0xB4, 0x05};
    uint8_t o[8];
    ss_des_encrypt(k, p, o);
    if (memcmp(o, c, 8) != 0) return false;
    // VNC auth answer for password "secret", challenge 00..0f (from the validated reference).
    static const uint8_t want[16] = {0xee, 0x22, 0x53, 0x9f, 0x33, 0xa5, 0x98, 0x3e,
                                     0xc1, 0x2f, 0x9c, 0x2e, 0xdb, 0xc9, 0x95, 0xdd};
    uint8_t ch[16], r[16];
    for (int i = 0; i < 16; i++) ch[i] = (uint8_t)i;
    ss_vnc_auth_response("secret", ch, r);
    return memcmp(r, want, 16) == 0;
}
