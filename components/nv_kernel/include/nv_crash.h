// nv_crash — last-boot crash surfacing for NucleoOS Anima.
//
// The panic handler already writes an ELF core dump to the `coredump` flash partition
// (CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH). This module reads it back on the next boot so the OS
// can tell the user what died: a boot notification + a Diagnostics card with task/PC, and a
// clear action. Full decode (backtrace with symbols) happens on the PC over serial:
// tools\decode-coredump.ps1.
#pragma once
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    char     task[16];    // task that hit the panic
    uint32_t pc;          // program counter at the exception
    uint32_t size;        // core dump size in flash, bytes
} nv_crash_info_t;

// True when the coredump partition holds a valid dump from a previous boot; fills *out.
// Cheap after the first call (result cached).
bool nv_crash_get(nv_crash_info_t *out);

// Erase the stored dump (Diagnostics "clear" action). nv_crash_get returns false afterwards.
void nv_crash_erase(void);

#ifdef __cplusplus
}
#endif
