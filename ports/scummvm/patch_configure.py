#!/usr/bin/env python3
"""Teach ScummVM's configure a wasm32-wasi host and the "nucleo" backend (idempotent)."""
import sys, pathlib

p = pathlib.Path(sys.argv[1]) / "configure"
s = p.read_text()
if "NUCLEO_BACKEND" in s:
    sys.exit(0)

def sub(old, new):
    global s
    assert s.count(old) == 1, f"anchor not unique/missing: {old!r}"
    s = s.replace(old, new)

# Host: wasm32-wasi* is a plain WASI target, not emscripten (the generic wasm32-* case).
sub("wasm32-*)\n\t_endian=little",
    "wasm32-wasi*)\n\t_endian=little\n\t_host_os=wasi\n\t_host_cpu=wasm32\n\t;;\n"
    "wasm32-*)\n\t_endian=little")
# Backend: NucleoOS (WAMR guest, nv_* host imports).
sub("\tnull)\n\t\tappend_var DEFINES \"-DUSE_NULL_DRIVER\"",
    "\tnucleo)\n\t\tappend_var DEFINES \"-DNUCLEO_BACKEND\"\n"
    "\t\t_seq_midi=no\n\t\t_timidity=no\n\t\t;;\n"
    "\tnull)\n\t\tappend_var DEFINES \"-DUSE_NULL_DRIVER\"")
sub("maemo | null | opendingux", "maemo | nucleo | null | opendingux")
# wasi-libc has the POSIX file APIs the POSIX fs/saves code needs.
sub("\temscripten)\n\t\t_posix=yes", "\temscripten | wasi)\n\t\t_posix=yes")
p.write_text(s)
