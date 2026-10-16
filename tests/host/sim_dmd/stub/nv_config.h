#pragma once
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
int nv_config_get_int(const char *key, int def);
void nv_config_set_int(const char *key, int v);
void nv_config_get_str(const char *key, const char *def, char *out, size_t n);
void nv_config_set_str(const char *key, const char *value);
#ifdef __cplusplus
}
#endif
