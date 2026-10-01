// nucleo_arb — the TLS gate: at most one outbound TLS session at a time.
//
// Every cloud call (Wikipedia GET, teacher POST, Whisper upload) holds an mbedTLS context plus its
// record buffers in INTERNAL SRAM. The pre-TLS heap check (online_tls_heap_too_low) is a point check:
// two callers — the native ANIMA worker and the web handler — could both pass it and then both
// allocate a session. Holding this token across the allocate -> use -> free window serializes them.
//
// Non-blocking by design (the httpd task must never wait): a caller that finds it busy degrades
// exactly as it does when the heap check fails.
#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Take the token. `job` is a short label for the debug log. Returns a non-zero token, or 0 when
// another TLS session holds it (the caller must bail / fall back).
uint32_t nucleo_arb_acquire(const char *job);

// Release a token from nucleo_arb_acquire(). 0, or a token that no longer holds it, is a no-op.
void nucleo_arb_release(uint32_t token);

#ifdef __cplusplus
}
#endif
