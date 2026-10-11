// Host stand-in for esp_random.h.
#pragma once
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
void esp_fill_random(void *buf, size_t len);
#ifdef __cplusplus
}
#endif
