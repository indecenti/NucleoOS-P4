// Host tests: a fake network for the ANIMA engine. fakenet_online(1) "associates" the device; each
// HTTP request is answered by the first registered fixture whose url_sub is in the URL (else fails).
#pragma once
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
void fakenet_online(int on);
void fakenet_clear(void);
void fakenet_add(const char *url_sub, int status, const char *body);
void fakenet_add_once(const char *url_sub, int status, const char *body);   // answered once, in order
const char *fakenet_last_url(void);
const char *fakenet_last_post(void);
const char *fakenet_chat_post(void);           // the body of the last /chat/completions request
int fakenet_chat_count(void);                   // /chat/completions requests since fakenet_clear()
void fakeclock_advance(int64_t us);             // esp_timer_get_time() jumps ahead (retry windows)
#ifdef __cplusplus
}
#endif
