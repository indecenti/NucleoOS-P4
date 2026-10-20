"""Future Pinball scores read straight out of the running game's memory.

Why: on AMD cards FP's own backbox (second monitor) leaks ~6 MB per second and FP dies after a few minutes. With the
backbox off FP is stable, so the bridge draws the scores itself; this module tells it what they are.

What is read: FP's HUD display objects (HudReel and HudSegment elements share one class in Future Pinball.exe 1.9:
vtable 0x00586734, the value as int32 at +0x0C). Tables write the player's score there for desktop mode even when the
HUD is hidden. The backbox reels (EMReel, SegmentDisplay) keep no plain number and are not read. Found 20/10/2026 on
2001, ALASKA and Galaxy (see side-session notes, future-pinball-ottimizzazione.md).

    find(pid)        -> [{"addr", "kind": "hud", "style", "order"}, ...] in memory order; [] if not loaded yet.
                        Usually ONE element: the score of the player in play (no credits/ball/match, no "digits")
    read(pid, elems) -> [int, ...] in the same order; raises OSError when FP is gone
"""
import ctypes
import ctypes.wintypes as W
import struct

VTABLE = 0x00586734           # FP 1.9 HUD display class (Future Pinball.exe, image base 0x400000)
VALUE = 0x0C                  # int32 value offset in the object
_k32 = ctypes.WinDLL("kernel32", use_last_error=True)
_handles = {}


class _MBI(ctypes.Structure):
    _fields_ = [("BaseAddress", ctypes.c_void_p), ("AllocationBase", ctypes.c_void_p), ("AllocationProtect", W.DWORD),
                ("PartitionId", W.WORD), ("RegionSize", ctypes.c_size_t), ("State", W.DWORD), ("Protect", W.DWORD),
                ("Type", W.DWORD)]


def _handle(pid):
    h = _handles.get(pid)
    if not h:
        h = _k32.OpenProcess(0x0410, False, pid)          # PROCESS_QUERY_INFORMATION | PROCESS_VM_READ
        if not h:
            raise OSError("cannot open Future Pinball (pid %d)" % pid)
        for old in _handles.values():   # the previous game's FP: its handle closed
            _k32.CloseHandle(old)
        _handles.clear()
        _handles[pid] = h
    return h


def _read(h, addr, n):
    buf = ctypes.create_string_buffer(n)
    got = ctypes.c_size_t()
    if not _k32.ReadProcessMemory(h, ctypes.c_void_p(addr), buf, n, ctypes.byref(got)) or got.value != n:
        raise OSError("read failed at 0x%x" % addr)
    return buf.raw


def _is_fp19(h):
    """The class's first method must point into FP's code: a different Future Pinball.exe would read garbage."""
    try:
        first = struct.unpack("<I", _read(h, VTABLE, 4))[0]
    except OSError:
        return False
    return 0x00401000 <= first < 0x00580000


def find(pid):
    """One pass over FP's heap at the start of a game, at low thread priority (about 2 s on the cabinet PC)."""
    th = _k32.GetCurrentThread()
    old = _k32.GetThreadPriority(th)
    _k32.SetThreadPriority(th, -2)                        # THREAD_PRIORITY_LOWEST: the game comes first
    try:
        return _find(pid)
    finally:
        _k32.SetThreadPriority(th, old)


def _find(pid):
    h = _handle(pid)
    if not _is_fp19(h):
        return []
    pat = struct.pack("<I", VTABLE)
    found = []
    mbi = _MBI()
    addr = 0
    while addr < 0xFFFF0000 and _k32.VirtualQueryEx(h, ctypes.c_void_p(addr), ctypes.byref(mbi), ctypes.sizeof(mbi)):
        base = mbi.BaseAddress or 0
        size = mbi.RegionSize
        # committed, private, read-write heap only (the objects live in FP's heap)
        if mbi.State == 0x1000 and mbi.Type == 0x20000 and mbi.Protect == 0x04 and size < (256 << 20):
            try:
                data = _read(h, base, size)
            except OSError:
                data = b""
            i = data.find(pat)
            while i >= 0:
                if i % 4 == 0 and i + 16 <= len(data):
                    style, flag, value = struct.unpack_from("<iii", data, i + 4)
                    if 0 <= style <= 16 and 0 <= flag <= 16 and 0 <= value < 2000000000:
                        found.append({"addr": base + i, "kind": "hud", "style": style, "order": len(found)})
                i = data.find(pat, i + 1)
        addr = base + size
    return found


def read(pid, elems):
    h = _handle(pid)
    return [struct.unpack("<i", _read(h, e["addr"] + VALUE, 4))[0] for e in elems]


if __name__ == "__main__":
    import subprocess
    import sys
    import time
    out = subprocess.run(["tasklist", "/fi", "imagename eq Future Pinball.exe", "/fo", "csv", "/nh"],
                         capture_output=True, text=True).stdout
    if "," not in out:
        sys.exit("Future Pinball is not running")
    p = int(out.split('","')[1])
    t = time.time()
    e = find(p)
    print("find: %d display(s) in %.2f s: %s" % (len(e), time.time() - t, e))
    for _ in range(3):
        print("read:", read(p, e))
        time.sleep(1)
