#!/usr/bin/env python3
"""gen_paddb.py - compile SDL_GameControllerDB into components/nv_hal/nv_paddb.c.

Downloads (or reads) gamecontrollerdb.txt from https://github.com/mdqinc/SDL_GameControllerDB
(zlib license, see tools/paddb/LICENSE) and turns every usable line into the compact
nv_pad_map_t encoding of nv_pad.h, deduplicated, in two VID/PID-sorted tables:

  USB  - Windows (DirectInput) lines first, then Mac, then Linux; Linux lines for pads that have a
         dedicated kernel driver (Microsoft, Sony, Nintendo) are skipped because their button
         order is the driver's, not the raw HID one.
  BT   - Linux, then Mac lines with bus 0x05 (Bluetooth). At run time a Bluetooth pad that isn't
         here falls back to the USB table (DirectInput doesn't care about the bus).

Usage:  python tools/gen_paddb.py [--db path/to/gamecontrollerdb.txt]
"""
import argparse
import os
import sys
import urllib.request

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
URL = "https://raw.githubusercontent.com/mdqinc/SDL_GameControllerDB/master/gamecontrollerdb.txt"
OUT = os.path.join(ROOT, "components", "nv_hal", "nv_paddb.c")
CACHE = os.path.join(ROOT, "tools", "paddb", "gamecontrollerdb.txt")

BUTTONS = ["a", "b", "x", "y", "back", "guide", "start", "leftstick", "rightstick",
           "leftshoulder", "rightshoulder", "dpup", "dpdown", "dpleft", "dpright",
           "misc1", "paddle1", "paddle2", "paddle3", "paddle4", "touchpad"]
AXES = ["leftx", "lefty", "rightx", "righty", "lefttrigger", "righttrigger"]
KERNEL_DRIVER_VIDS = {0x045E, 0x054C, 0x057E}   # xpad / hid-playstation / hid-nintendo remap


def encode_source(src):
    """SDL source ("b3", "a2", "+a2", "-a1", "a5~", "h0.4") -> one nv_pad_map_t byte, None if unusable."""
    half = 0
    if src[:1] in "+-":
        half = 1 if src[0] == "+" else 2
        src = src[1:]
    inv = src.endswith("~")
    if inv:
        src = src[:-1]
    try:
        if src.startswith("b"):
            n = int(src[1:])
            return n if n < 64 else None
        if src.startswith("a"):
            n = int(src[1:])
            if n > 15:
                return None
            mode = half if half else (3 if inv else 0)
            return 0x40 | (mode << 4) | n
        if src.startswith("h"):
            h, m = src[1:].split(".")
            h, m = int(h), int(m)
            if h > 3 or m not in (1, 2, 4, 8):
                return None
            return 0x80 | (h << 4) | m
    except ValueError:
        return None
    return None


def parse_mapping(fields):
    btn = [0xFF] * len(BUTTONS)
    axis = [0xFF] * len(AXES)
    for f in fields:
        if ":" not in f:
            continue
        k, v = f.split(":", 1)
        if not v or k[:1] in "+-":          # half-axis outputs: not in our model
            continue
        e = encode_source(v)
        if e is None:
            continue
        if k in BUTTONS:
            btn[BUTTONS.index(k)] = e
        elif k in AXES:
            axis[AXES.index(k)] = e
    if all(b == 0xFF for b in btn[:4]):     # no face buttons: not a gamepad mapping
        return None
    return bytes(btn + axis)


def better(old, rank, m):
    """Lower platform rank wins; within a rank the line mapping the most targets (newer firmware)."""
    if old is None or rank < old[0]:
        return True
    return rank == old[0] and sum(x != 0xFF for x in m) > sum(x != 0xFF for x in old[1])


def guid_ids(guid):
    """-> (bus, vid, pid) or None."""
    if len(guid) != 32:
        return None
    try:
        b = bytes.fromhex(guid)
    except ValueError:
        return None
    if b[10:16] == b"PIDVID":                           # old DirectInput product GUID
        return 0x03, b[0] | b[1] << 8, b[2] | b[3] << 8
    if b[6:8] == b"\0\0" and b[10:12] == b"\0\0":       # SDL >= 2.0.5
        vid, pid = b[4] | b[5] << 8, b[8] | b[9] << 8
        if vid == 0 and pid == 0:
            return None
        return b[0] | b[1] << 8, vid, pid
    return None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--db", help="gamecontrollerdb.txt (default: download, cached in tools/paddb)")
    a = ap.parse_args()
    if a.db:
        text = open(a.db, encoding="utf-8").read()
    else:
        print("downloading", URL)
        text = urllib.request.urlopen(URL, timeout=60).read().decode("utf-8")
        os.makedirs(os.path.dirname(CACHE), exist_ok=True)
        with open(CACHE, "w", encoding="utf-8", newline="\n") as f:
            f.write(text)

    rank_usb = {"Windows": 0, "Mac OS X": 1, "Linux": 2}
    rank_bt = {"Linux": 0, "Mac OS X": 1}
    usb, bt = {}, {}                                    # (vid, pid) -> (rank, map, name)
    lines = 0
    for line in text.splitlines():
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        parts = [p for p in line.split(",") if p != ""]
        if len(parts) < 3:
            continue
        plat = next((p.split(":", 1)[1] for p in parts if p.startswith("platform:")), None)
        ids = guid_ids(parts[0])
        if not plat or not ids:
            continue
        bus, vid, pid = ids
        m = parse_mapping(parts[2:])
        if m is None:
            continue
        lines += 1
        key = (vid, pid)
        if bus == 0x05 and plat in rank_bt:
            r = rank_bt[plat]
            if better(bt.get(key), r, m):
                bt[key] = (r, m, parts[1])
        elif bus == 0x03 and plat in rank_usb:
            if plat == "Linux" and vid in KERNEL_DRIVER_VIDS:
                continue
            r = rank_usb[plat]
            if better(usb.get(key), r, m):
                usb[key] = (r, m, parts[1])

    maps = []
    index = {}
    def map_id(m):
        if m not in index:
            index[m] = len(maps)
            maps.append(m)
        return index[m]

    usb_rows = sorted((k[0], k[1], map_id(v[1]), v[2]) for k, v in usb.items())
    bt_rows = sorted((k[0], k[1], map_id(v[1]), v[2]) for k, v in bt.items())
    width = len(BUTTONS) + len(AXES)

    o = []
    o.append("// nv_paddb - GENERATED by tools/gen_paddb.py from SDL_GameControllerDB, do not edit.")
    o.append("// https://github.com/mdqinc/SDL_GameControllerDB - zlib license:")
    o.append("//   This software is provided 'as-is', without any express or implied warranty. Permission is")
    o.append("//   granted to anyone to use it for any purpose, including commercial applications, and to alter")
    o.append("//   it and redistribute it freely, subject to: the origin must not be misrepresented; altered")
    o.append("//   versions must be plainly marked as such; this notice may not be removed (tools/paddb/LICENSE).")
    o.append(f"// {lines} usable lines -> {len(usb_rows)} USB + {len(bt_rows)} Bluetooth devices, {len(maps)} distinct mappings.")
    o.append('#include "nv_paddb.h"')
    o.append("")
    o.append(f"static const uint8_t kMaps[{len(maps)}][{width}] = {{")
    for m in maps:
        o.append("    {" + ",".join(f"0x{x:02x}" for x in m) + "},")
    o.append("};")
    for tag, rows in (("Usb", usb_rows), ("Bt", bt_rows)):
        o.append("")
        o.append(f"static const nv_paddb_row_t k{tag}[{len(rows)}] = {{")
        for vid, pid, mi, name in rows:
            safe = name.replace("*/", "").replace("\\", "/")[:40]
            o.append(f"    {{0x{vid:04x}, 0x{pid:04x}, {mi}}},   // {safe}")
        o.append("};")
    o.append("")
    o.append("const nv_paddb_t nv_paddb = {")
    o.append(f"    kMaps[0], {width}, kUsb, {len(usb_rows)}, kBt, {len(bt_rows)},")
    o.append("};")
    with open(OUT, "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(o) + "\n")
    print(f"{OUT}: {len(usb_rows)} USB + {len(bt_rows)} BT, {len(maps)} maps "
          f"(~{(len(maps) * width + (len(usb_rows) + len(bt_rows)) * 6) // 1024} KB)")


if __name__ == "__main__":
    sys.exit(main())
