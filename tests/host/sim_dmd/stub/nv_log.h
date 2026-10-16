#pragma once
#define NV_LOGI(...) ((void)0)
#define NV_LOGW(...) ((void)0)
#define NV_LOGE(...) ((void)0)
enum { NV_LOG_ERROR = 0, NV_LOG_WARN, NV_LOG_INFO, NV_LOG_DEBUG, NV_LOG_TRACE };
#define nv_log_write(...) ((void)0)
