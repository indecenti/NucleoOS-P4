// nv_auth — who may use the device's network API. Browsers and tools pair once with a 6-digit code
// shown on the device screen (and printed on the USB serial console, for a host that is physically
// attached); pairing hands out a 128-bit session token. Only SHA-256 hashes of tokens are stored
// (nv_config "web_sess"), at most 12, revocable from Settings → Security.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NV_AUTH_TOKEN_HEX 32   // token length (lowercase hex)
#define NV_AUTH_NAME_MAX  32   // session label incl. NUL
#define NV_AUTH_WHO_MAX   48   // pairing requester label incl. NUL

typedef enum {
    NV_AUTH_PAIR_OK = 0,
    NV_AUTH_PAIR_WRONG,        // wrong code; tries left
    NV_AUTH_PAIR_LOCKED,       // too many wrong codes (or the owner cancelled): wait
    NV_AUTH_PAIR_NO_CODE,      // no code is out (expired, or never requested)
} nv_auth_pair_result_t;

typedef struct {
    char     name[NV_AUTH_NAME_MAX];
    uint32_t created;          // unix time, 0 when the clock wasn't set at pairing time
} nv_auth_session_t;

void nv_auth_init(void);

// true when `token` (NUL-terminated) belongs to a paired session.
bool nv_auth_check_token(const char *token);
// Same, taking the raw Authorization and Cookie header values (either may be NULL).
bool nv_auth_check_headers(const char *authorization, const char *cookie);

// An unpaired client asked to pair: put out a code for the device owner to read (no-op while one is
// already out, or while pairing is locked). `who` labels the requester on the prompt.
void nv_auth_pair_request(const char *who);
// Try a code. On success a new session named `name` is stored and its token copied to `token_out`
// (NV_AUTH_TOKEN_HEX + 1 bytes).
nv_auth_pair_result_t nv_auth_pair_finish(const char *code, const char *name, char *token_out);
// For SystemUI: the code to show right now, if any (code: 7 bytes, who: NV_AUTH_WHO_MAX).
bool nv_auth_pair_pending(char *code, char *who, uint32_t *secs_left);
// The owner dismissed the prompt: burn the code and suppress new prompts for a few minutes.
void nv_auth_pair_deny(void);

int  nv_auth_session_count(void);
bool nv_auth_session_get(int index, nv_auth_session_t *out);
void nv_auth_session_revoke(int index);
void nv_auth_session_revoke_all(void);

#ifdef __cplusplus
}
#endif
