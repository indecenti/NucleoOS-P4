#!/bin/bash
# bc_generate.sh <bc source dir> — WSL side of the bc build (run by ports/cli/build.sh).
# bc's configure.sh (bc only, no history/NLS/man pages/generated tests) writes the Makefile;
# its host tool strgen then turns the math library (lib.bc, lib2.bc) and the help text into C
# (gen/lib.c, gen/lib2.c, gen/bc_help.c). The compiler flags configure chose (feature -Ds and
# the bc.* defaults) are saved to nv_cflags.txt for the wasm compile in build.sh.
set -e
cd "$1"
CC=gcc HOSTCC=gcc ./configure.sh -b -G -H -N -M -O2 >/dev/null
make gen/lib.c gen/lib2.c gen/bc_help.c >/dev/null
printf 'nv_cflags:\n\t@echo $(CFLAGS)\n' | make -s -f Makefile -f - nv_cflags > nv_cflags.txt
