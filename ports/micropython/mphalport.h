// MicroPython HAL for NucleoOS (WASI console program).
#include <unistd.h>
#define mp_hal_ticks_cpu() 0
static inline void mp_hal_set_interrupt_char(char c) {
    (void)c;
}
void mp_hal_get_random(size_t n, void *buf);

#include <errno.h>
// A POSIX call: retry on EINTR, else run `raise` with `err` set (as the unix port).
#define MP_HAL_RETRY_SYSCALL(ret, syscall, raise) { \
        for (;;) { \
            ret = syscall; \
            if (ret == -1) { \
                int err = errno; \
                if (err == EINTR) { \
                    continue; \
                } \
                raise; \
            } \
            break; \
        } \
}
