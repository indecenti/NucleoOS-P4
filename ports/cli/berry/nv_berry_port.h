// nv_berry_port.h — force-included into every Berry source for NucleoOS (WASI).
// NV_WASI switches be_exec.c/be_vm.c (ports/cli/berry.patch) from setjmp/longjmp to the host's
// nv_try_call/nv_throw. wasi-libc has no processes: os.system() answers -1.
#pragma once
#define NV_WASI 1
int system(const char *cmd);
