// MicroPython for NucleoOS: a WASI console program (Terminal app, host ABI 8), see README.md.
#include <stdint.h>
#include <time.h>
#include <stdlib.h>

#define MICROPY_CONFIG_ROM_LEVEL                (MICROPY_CONFIG_ROM_LEVEL_EXTRA_FEATURES)

// No setjmp under WAMR: non-local returns go through the host's protected call (nlr_nv.c).
#define MICROPY_NLR_NV                          (1)
// Python-to-Python calls do not nest C frames (nor host try_calls): frames live on the pystack.
#define MICROPY_STACKLESS                       (1)
#define MICROPY_ENABLE_PYSTACK                  (1)
#define MICROPY_STACK_CHECK                     (1)

#define MICROPY_ENABLE_GC                       (1)
#define MICROPY_GC_SPLIT_HEAP                   (1)
#define MICROPY_GC_SPLIT_HEAP_AUTO              (1)
#define MICROPY_ENABLE_FINALISER                (1)
#define MICROPY_ENABLE_EMERGENCY_EXCEPTION_BUF  (1)
#define MICROPY_EMERGENCY_EXCEPTION_BUF_SIZE    (256)
#define MICROPY_LONGINT_IMPL                    (MICROPY_LONGINT_IMPL_MPZ)
#define MICROPY_FLOAT_IMPL                      (MICROPY_FLOAT_IMPL_DOUBLE)
#define MICROPY_ALLOC_PATH_MAX                  (256)
#define MICROPY_ENABLE_DOC_STRING               (0)
#define MICROPY_WARNINGS                        (1)
#define MICROPY_ERROR_PRINTER                   (&mp_stderr_print)
#define MICROPY_USE_INTERNAL_ERRNO              (0)
#define MICROPY_USE_INTERNAL_PRINTF             (0)
#define MICROPY_KBD_EXCEPTION                   (0)
#define MICROPY_HELPER_REPL                     (1)
#define MICROPY_REPL_AUTO_INDENT                (1)
#define MICROPY_OPT_COMPUTED_GOTO               (0)
#define MICROPY_PERSISTENT_CODE_LOAD            (1)
#define MICROPY_MODULE_OVERRIDE_MAIN_IMPORT     (1)

// Files: the SD home through POSIX calls (wasi-libc preopens /sdcard/home as /).
#define MICROPY_VFS                             (1)
#define MICROPY_VFS_POSIX                       (1)
#define MICROPY_READER_VFS                      (1)
#define MICROPY_EPOCH_IS_1970                   (1)
#define MICROPY_TIMESTAMP_IMPL                  (MICROPY_TIMESTAMP_IMPL_TIME_T)

#define MICROPY_PY_SYS_PLATFORM                 "nucleoos"
#define MICROPY_PY_SYS_PATH_DEFAULT             ".frozen:/lib:/py"
#define MICROPY_PY_SYS_EXIT                     (1)
#define MICROPY_PY_SYS_ARGV                     (1)
#define MICROPY_PY_SYS_STDFILES                 (1)
#define MICROPY_PY_SYS_STDIO_BUFFER             (1)
#define MICROPY_PY_OS_INCLUDEFILE               "modos_nv.c"
#define MICROPY_PY_OS_GETENV_PUTENV_UNSETENV    (0)
#define MICROPY_PY_OS_SYSTEM                    (0)
#define MICROPY_PY_OS_URANDOM                   (1)
#define MICROPY_PY_OS_DUPTERM                   (0)
#define MICROPY_PY_TIME_GMTIME_LOCALTIME_MKTIME (1)
#define MICROPY_PY_TIME_TIME_TIME_NS            (1)
#define MICROPY_PY_TIME_INCLUDEFILE             "modtime_nv.c"
#define MICROPY_PY_RANDOM_SEED_INIT_FUNC        (mp_nv_random_seed())

// Nothing of this exists in a WASI console program.
#define MICROPY_PY_SELECT                       (0)
#define MICROPY_PY_ASYNCIO                      (0)
#define MICROPY_PY_SOCKET                       (0)
#define MICROPY_PY_NETWORK                      (0)
#define MICROPY_PY_SSL                          (0)
#define MICROPY_PY_MACHINE                      (0)
#define MICROPY_PY_THREAD                       (0)
#define MICROPY_PY_BLUETOOTH                    (0)
#define MICROPY_PY_WEBSOCKET                    (0)
#define MICROPY_PY_WEBREPL                      (0)
#define MICROPY_PY_FRAMEBUF                     (0)
#define MICROPY_PY_VFS                          (1)
#define MICROPY_PY_DEFLATE                      (1)
#define MICROPY_PY_DEFLATE_COMPRESS             (1)
#define MICROPY_PY_OPENAMP                      (0)
#define MICROPY_PY_LWIP                         (0)

#define MP_SSIZE_MAX                            (0x7fffffff)
typedef intptr_t mp_int_t;
typedef uintptr_t mp_uint_t;
typedef long mp_off_t;

#define MICROPY_HW_BOARD_NAME                   "NucleoOS"
#define MICROPY_HW_MCU_NAME                     "ESP32-P4 (WASI)"
#define MP_STATE_PORT                           MP_STATE_VM

#include <alloca.h>
uint32_t mp_nv_random_seed(void);
extern const struct _mp_print_t mp_stderr_print;
