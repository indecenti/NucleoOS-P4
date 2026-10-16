#pragma once
#define NV_EV_LANG_CHANGED 0
#ifdef __cplusplus
extern "C" {
#endif
void nv_event_publish(int ev, const void *data);
#ifdef __cplusplus
}
#endif
