// Host tests: a fake network for the ANIMA engine. fakenet_online(1) "associates" the device; each
// HTTP request is answered by the first registered fixture whose url_sub is in the URL (else fails).
#pragma once
#ifdef __cplusplus
extern "C" {
#endif
void fakenet_online(int on);
void fakenet_clear(void);
void fakenet_add(const char *url_sub, int status, const char *body);
void fakenet_add_once(const char *url_sub, int status, const char *body);   // answered once, in order
const char *fakenet_last_url(void);
const char *fakenet_last_post(void);
#ifdef __cplusplus
}
#endif
