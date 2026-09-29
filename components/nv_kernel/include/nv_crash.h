// nv_crash — last-boot crash surfacing for NucleoOS Anima.
//
// The panic handler already writes an ELF core dump to the `coredump` flash partition
// (CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH). This module reads it back on the next boot so the OS
// can tell the user what died: a boot notification + a Diagnostics card with task/PC, and a
// clear action. Full decode (backtrace with symbols) happens on the PC:
// tools\decode-coredump.ps1 -Url (over Wi-Fi, /api/crash/dump) or -Port (serial).
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    char     task[16];    // task that was running when the panic hit (the victim, for an ISR panic)
    uint32_t pc;          // program counter at the exception
    uint32_t size;        // core dump size in flash, bytes
    // Panic frame, as the panic handler stored it: exception code for faults (0..15), the panic
    // interrupt number for watchdog / cache / stack-guard panics (see nv_crash_reason).
    uint32_t mcause;
    uint32_t mtval;
    uint32_t ra;
    uint32_t sp;
    char     reason[24];  // mcause decoded to a stable token ("int_wdt_cpu0", "load_fault", ...)
    char     elf_sha[16]; // app ELF SHA-256 prefix recorded in the dump (hex)
    bool     this_build;  // elf_sha matches the running image: addr2line on this build's ELF is valid
} nv_crash_info_t;

// True when the coredump partition holds a valid dump from a previous boot; fills *out.
// Cheap after the first call (result cached).
bool nv_crash_get(nv_crash_info_t *out);

// Copy `len` bytes of the raw stored image (flash format: header + ELF + CRC, what
// `espcoredump.py --core-format raw` reads) starting at `off`. Returns the bytes copied, 0 past
// the end, -1 on error. Touches flash: call from an internal-stack task only.
int nv_crash_read(uint32_t off, void *buf, size_t len);

// Erase the stored dump (Diagnostics "clear" action). nv_crash_get returns false afterwards.
void nv_crash_erase(void);

#ifdef __cplusplus
}
#endif
