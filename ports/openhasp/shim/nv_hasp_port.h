/* nv_hasp_port.h — force-included (-include) into every openHASP / LVGL / FreeType unit of the
 * NucleoOS build. openHASP is built as its own "PC" target (HASP_TARGET_PC + POSIX, like the
 * upstream linux_headless environment); this header only fills the few gaps between that target
 * and wasi-libc. The native test harness (-DNV_SIM, glibc) includes it too, where it is a no-op.
 */
#ifndef NV_HASP_PORT_H
#define NV_HASP_PORT_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

/* LV_MEM_CUSTOM=1: lv_conf_v7.h routes LVGL's allocator to hasp_malloc/hasp_free but includes only
 * <stdlib.h>; declare them for the C units too (openHASP's include/hasp_mem.h). */
#include "hasp_mem.h"

#ifdef __cplusplus
extern "C" {
#endif

#if defined(__wasi__)
/* wasi-libc declares no popen/pclose; openHASP's PC "shell" command uses them. They are stubs
 * that fail (nv_hasp_device.cpp), so "shell" logs "Couldn't execute system command". */
FILE* popen(const char* command, const char* mode);
int pclose(FILE* stream);
#endif

#ifdef __cplusplus
}
#endif

#endif /* NV_HASP_PORT_H */
