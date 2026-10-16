#!/usr/bin/env python3
"""dmd_sender — a stand-in for dmdext, to test the DMD app without a cabinet, over either link.

Wi-Fi: it connects like dmdext's "networkstream" output does (websocket-sharp: masked binary frames,
every message longer than 1016 bytes split into continuation frames) and sends the same messages:
gameName, dimensions, color, palette, then frames (gray2Planes, gray4Planes, coloredGray2, coloredGray4,
rgb24).

USB (--usb COMx): it drives the board like dmdext's [zedmd] output does through libzedmd 0.11.0: the
64-byte handshake, then per frame the changed 8x4 zones as zlib-compressed RGB565 packets and a render
command, cut into chunks of the package size the board announced, each one waiting up to 16 ms for the
"ZeDMDA" ack (a miss forces a full frame next time, as libzedmd does). Needs pyserial (the ESP-IDF
Python environment has it).

The test picture: a border, a solid block in the TOP-LEFT corner (orientation check: it must appear
top-left on the board with rotation 0), a gradient that scrolls left to right and a bouncing box.

    python tools/dmd_sender.py 192.168.0.50                  # every format in turn, 128x32, 60 fps
    python tools/dmd_sender.py 192.168.0.50 --fmt gray4 --size 256x64 --fps 30 --seconds 20
    python tools/dmd_sender.py --usb COM6 --seconds 20       # as a ZeDMD over USB (128x32 RGB565)

Stdlib only for Wi-Fi; pyserial for USB.
"""
import argparse
import base64
import os
import socket
import struct
import sys
import time

FRAG = 1016  # websocket-sharp's FragmentLength


def ws_connect(host, port, path="/dmd"):
    s = socket.create_connection((host, port), timeout=5)
    key = base64.b64encode(os.urandom(16)).decode()
    s.sendall((f"GET {path} HTTP/1.1\r\nUser-Agent: dmd_sender\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
               f"Host: {host}:{port}\r\nSec-WebSocket-Key: {key}\r\nSec-WebSocket-Version: 13\r\n\r\n").encode())
    resp = b""
    while b"\r\n\r\n" not in resp:
        chunk = s.recv(1024)
        if not chunk:
            raise ConnectionError("closed during the handshake")
        resp += chunk
    if b" 101 " not in resp.split(b"\r\n")[0]:
        raise ConnectionError(resp.split(b"\r\n")[0].decode(errors="replace"))
    return s


def ws_frame(op, payload, fin):
    head = bytes([(0x80 if fin else 0) | op])
    n = len(payload)
    if n < 126:
        head += bytes([0x80 | n])
    elif n <= 0xFFFF:
        head += bytes([0x80 | 126]) + struct.pack(">H", n)
    else:
        head += bytes([0x80 | 127]) + struct.pack(">Q", n)
    mask = os.urandom(4)
    body = bytes(b ^ mask[i & 3] for i, b in enumerate(payload))
    return head + mask + body


def ws_send(s, msg):
    if len(msg) <= FRAG:
        s.sendall(ws_frame(0x2, msg, True))
        return
    out = b""
    for o in range(0, len(msg), FRAG):
        out += ws_frame(0x2 if o == 0 else 0x0, msg[o:o + FRAG], o + FRAG >= len(msg))
    s.sendall(out)


def m(name, payload=b""):
    return name.encode() + b"\0" + payload


def planes(levels, w, h, bits):
    """dmdext FrameUtil.Split: plane k = bit k, bit 0 of each byte = leftmost pixel."""
    size = w * h // 8
    out = [bytearray(size) for _ in range(bits)]
    for p, lv in enumerate(levels):
        for k in range(bits):
            if lv >> k & 1:
                out[k][p >> 3] |= 1 << (p & 7)
    return b"".join(out)


def picture(w, h, t, maxlv):
    """Levels 0..maxlv: border, top-left block, scrolling gradient, bouncing box."""
    lv = [0] * (w * h)
    bw = max(4, w // 8)
    bx = int((w - bw) * (0.5 + 0.5 * __import__("math").sin(t * 2.0)))
    for y in range(h):
        for x in range(w):
            v = ((x + int(t * 40)) % w) * maxlv // max(1, w - 1) // 2
            if x < h // 3 and y < h // 3:
                v = maxlv                                   # top-left marker
            if x == 0 or y == 0 or x == w - 1 or y == h - 1:
                v = maxlv
            if bx <= x < bx + bw and h // 3 <= y < h - h // 4:
                v = maxlv
            lv[y * w + x] = v
    return lv


def rainbow(w, h, t):
    out = bytearray()
    for y in range(h):
        for x in range(w):
            hue = ((x + y + t * 60) % w) / w * 6
            i, f = int(hue), hue - int(hue)
            r, g, b = [(1, f, 0), (1 - f, 1, 0), (0, 1, f), (0, 1 - f, 1), (f, 0, 1), (1, 0, 1 - f)][i % 6]
            if x < h // 3 and y < h // 3:
                r = g = b = 1
            out += bytes((int(r * 255), int(g * 255), int(b * 255)))
    return bytes(out)


def frame_msg(fmt, w, h, t, ts):
    tsb = struct.pack("<I", ts)
    if fmt == "gray2":
        return m("gray2Planes", tsb + planes(picture(w, h, t, 3), w, h, 2))
    if fmt == "gray4":
        return m("gray4Planes", tsb + planes(picture(w, h, t, 15), w, h, 4))
    if fmt == "cgray2":
        pal = [0x000000, 0x0000FF, 0x00FF00, 0xFF0000]
        return m("coloredGray2", tsb + struct.pack("<i", 4) + b"".join(struct.pack("<i", c) for c in pal)
                 + planes(picture(w, h, t, 3), w, h, 2))
    if fmt == "cgray4":
        pal = [(i * 17) << 16 | (255 - i * 17) << 8 | 0x40 for i in range(16)]
        pal[0] = 0
        return m("coloredGray4", tsb + struct.pack("<i", 16) + b"".join(struct.pack("<i", c) for c in pal)
                 + planes(picture(w, h, t, 15), w, h, 4))
    return m("rgb24", tsb + rainbow(w, h, t))


# 5x7 capitals, digits and a few signs (one string of 7 rows of 5 bits per glyph) for --text.
FONT = {
    "A": "01110 10001 10001 11111 10001 10001 10001", "B": "11110 10001 10001 11110 10001 10001 11110",
    "C": "01110 10001 10000 10000 10000 10001 01110", "D": "11100 10010 10001 10001 10001 10010 11100",
    "E": "11111 10000 10000 11110 10000 10000 11111", "F": "11111 10000 10000 11110 10000 10000 10000",
    "G": "01110 10001 10000 10111 10001 10001 01111", "H": "10001 10001 10001 11111 10001 10001 10001",
    "I": "01110 00100 00100 00100 00100 00100 01110", "J": "00111 00010 00010 00010 00010 10010 01100",
    "K": "10001 10010 10100 11000 10100 10010 10001", "L": "10000 10000 10000 10000 10000 10000 11111",
    "M": "10001 11011 10101 10101 10001 10001 10001", "N": "10001 10001 11001 10101 10011 10001 10001",
    "O": "01110 10001 10001 10001 10001 10001 01110", "P": "11110 10001 10001 11110 10000 10000 10000",
    "Q": "01110 10001 10001 10001 10101 10010 01101", "R": "11110 10001 10001 11110 10100 10010 10001",
    "S": "01111 10000 10000 01110 00001 00001 11110", "T": "11111 00100 00100 00100 00100 00100 00100",
    "U": "10001 10001 10001 10001 10001 10001 01110", "V": "10001 10001 10001 10001 10001 01010 00100",
    "W": "10001 10001 10001 10101 10101 10101 01010", "X": "10001 10001 01010 00100 01010 10001 10001",
    "Y": "10001 10001 01010 00100 00100 00100 00100", "Z": "11111 00001 00010 00100 01000 10000 11111",
    "0": "01110 10001 10011 10101 11001 10001 01110", "1": "00100 01100 00100 00100 00100 00100 01110",
    "2": "01110 10001 00001 00010 00100 01000 11111", "3": "11111 00010 00100 00010 00001 10001 01110",
    "4": "00010 00110 01010 10010 11111 00010 00010", "5": "11111 10000 11110 00001 00001 10001 01110",
    "6": "00110 01000 10000 11110 10001 10001 01110", "7": "11111 00001 00010 00100 01000 01000 01000",
    "8": "01110 10001 10001 01110 10001 10001 01110", "9": "01110 10001 10001 01111 00001 00010 01100",
    "!": "00100 00100 00100 00100 00100 00000 00100", ".": "00000 00000 00000 00000 00000 01100 01100",
    "-": "00000 00000 00000 11111 00000 00000 00000", " ": "00000 00000 00000 00000 00000 00000 00000",
}


def text_frame(w, h, text, color=(255, 69, 0)):
    """The text as RGB565, one line per '|' (or one line per word when it does not fit), centred, as big
    as fits (5x7 glyphs, integer scale)."""
    lines = [l.strip().upper() for l in text.split("|")]
    if len(lines) == 1 and len(lines[0]) * 6 - 1 > w:
        lines = lines[0].split()
    width = max(len(l) * 6 - 1 for l in lines)
    scale = max(1, min(w // max(1, width), h // (len(lines) * 8 - 1)))
    lit = set()
    y0 = (h - (len(lines) * 8 - 1) * scale) // 2
    for li, line in enumerate(lines):
        x0 = (w - (len(line) * 6 - 1) * scale) // 2
        for ci, ch in enumerate(line):
            rows = FONT.get(ch, FONT[" "]).split()
            for ry, bits in enumerate(rows):
                for rx, bit in enumerate(bits):
                    if bit == "1":
                        for sy in range(scale):
                            for sx in range(scale):
                                lit.add((x0 + (ci * 6 + rx) * scale + sx, y0 + (li * 8 + ry) * scale + sy))
    r, g, b = color
    on = (r >> 3) << 11 | (g >> 2) << 5 | b >> 3
    out = bytearray()
    for y in range(h):
        for x in range(w):
            c = on if (x, y) in lit else 0
            out += bytes((c & 0xFF, c >> 8))
    return bytes(out)


def rgb565_frame(w, h, t, fmt):
    """The test picture as little-endian RGB565, as libzedmd sends it."""
    if fmt == "rgb24":
        rgb = rainbow(w, h, t)
        px = [(rgb[3 * i], rgb[3 * i + 1], rgb[3 * i + 2]) for i in range(w * h)]
    else:
        lv = picture(w, h, t, 15)
        px = [(255 * v // 15, 69 * v // 15, 0) for v in lv]   # OrangeRed, dmdext's default colour
    out = bytearray()
    for r, g, b in px:
        c = (r >> 3) << 11 | (g >> 2) << 5 | b >> 3
        out += bytes((c & 0xFF, c >> 8))
    return bytes(out)


def run_usb(a):
    import zlib
    try:
        import serial
    except ImportError:
        sys.exit("pyserial is needed for --usb (python -m pip install pyserial)")
    # Open with DTR and RTS low: on the ESP32-P4's USB-Serial-JTAG some DTR/RTS transitions reset the chip
    # (that is how esptool resets it), and pyserial raises both by default.
    sp = serial.Serial()
    sp.port, sp.baudrate, sp.timeout, sp.write_timeout = a.usb, 115200, 0.5, 1
    sp.dtr = False
    sp.rts = False
    sp.open()
    sp.reset_input_buffer()
    hs = bytearray(64)
    hs[0:11] = b"FRAMEZeDMD\x0c"
    sp.write(bytes(hs))
    for _ in range(29):
        sp.write(bytes(64))
        time.sleep(0.01)
    time.sleep(0.2)
    r = sp.read(64)
    if len(r) != 64 or r[:4] != b"ZeDM" or r[57] != ord("R") or r[8] == 0:
        sys.exit(f"no ZeDMD answer on {a.usb}: {r[:16]!r} ({len(r)} bytes) - is the DMD app open?")
    w, h = r[4] | r[5] << 8, r[6] | r[7] << 8
    chunk = r[11] | r[12] << 8
    zw, zh = w // 16, h // 8
    print(f"ZeDMD {r[8]}.{r[9]}.{r[10]} on {a.usb}: {w}x{h}, package {chunk} bytes")
    sp.timeout = 0.016                                     # ZEDMD_COMM_SERIAL_READ_TIMEOUT
    hashes = [None] * 128
    fmts = ["gray4", "rgb24"] if a.fmt == "all" else ["rgb24" if a.fmt == "rgb24" else "gray4"]
    # One second of animation per format, computed up front: Python is too slow to draw 60 frames a
    # second itself, and the point is to measure the link and the board.
    print("preparing frames...")
    if a.text:
        fmts = ["text"]
        cycle = {"text": [text_frame(w, h, a.text)] * 60}
    else:
        cycle = {f: [rgb565_frame(w, h, i / 60.0, f) for i in range(60)] for f in fmts}
    t0 = time.monotonic()
    n = sent_ok = forced = 0
    last_fmt = None
    try:
        while True:
            t = time.monotonic() - t0
            if a.seconds and t >= a.seconds:
                break
            fmt = fmts[int(t / a.each) % len(fmts)]
            if fmt != last_fmt:
                print(f"{t:6.1f}s  {fmt}")
                last_fmt = fmt
            data = cycle[fmt][n % 60]
            packets, buf = [], bytearray()
            for idx in range(128):
                x0, y0 = (idx % 16) * zw, (idx // 16) * zh
                zone = b"".join(data[((y0 + y) * w + x0) * 2:((y0 + y) * w + x0 + zw) * 2] for y in range(zh))
                black = not any(zone)
                key = 1 if black else hash(zone)
                if key == hashes[idx]:
                    continue
                hashes[idx] = key
                if len(buf) + 1 + len(zone) > 128 * 4 * 2 + 16:   # ZEDMD_ZONES_BYTE_LIMIT_RGB565
                    packets.append(bytes(buf))
                    buf = bytearray()
                buf += bytes([idx + 128]) if black else bytes([idx]) + zone
            if buf:
                packets.append(bytes(buf))
            payload = bytearray(b"FRAME")
            for pk in packets:
                c = zlib.compress(pk)
                comp = len(c) < len(pk)
                body = c if comp else pk
                payload += b"ZeDMD" + bytes([0x05, len(body) >> 8, len(body) & 0xFF, 1 if comp else 0]) + body
            payload += b"ZeDMD\x06\x00\x00\x00"
            ok = True
            sp.reset_input_buffer()
            for o in range(0, len(payload), chunk):
                part = payload[o:o + chunk]
                sp.write(bytes(part) + bytes(chunk - len(part)))
                ack = sp.read(6)
                if ack != b"ZeDMDA":
                    ok = False
                    break
            if ok:
                sent_ok += 1
            else:
                forced += 1
                hashes = [None] * 128                      # full frame next time
            n += 1
            d = t0 + n / a.fps - time.monotonic()
            if d > 0:
                time.sleep(d)
    except KeyboardInterrupt:
        pass
    el = time.monotonic() - t0
    print(f"{n} frames in {el:.1f}s = {n / el:.1f} fps, acked {sent_ok}, full frames forced {forced}")
    rs = b"FRAMEZeDMD\x1f\x00\x00\x00"                 # libzedmd's Close(): a soft reset
    sp.write(rs + bytes(chunk - len(rs)))
    sp.read(6)
    sp.close()


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("host", nargs="?", help="the board's IP (Wi-Fi)")
    ap.add_argument("--usb", metavar="COMx", help="drive the board as a ZeDMD on this serial port instead")
    ap.add_argument("--port", type=int, default=9090)
    ap.add_argument("--fmt", default="all", choices=["all", "gray2", "gray4", "cgray2", "cgray4", "rgb24"])
    ap.add_argument("--size", default="128x32")
    ap.add_argument("--fps", type=float, default=60)
    ap.add_argument("--seconds", type=float, default=0, help="0 = until Ctrl+C")
    ap.add_argument("--each", type=float, default=6, help="seconds per format with --fmt all")
    ap.add_argument("--text", help="USB: show this text instead of the test picture ('|' = new line)")
    a = ap.parse_args()
    if a.usb:
        run_usb(a)
        return
    if not a.host:
        ap.error("the board's IP, or --usb COMx")
    w, h = (int(v) for v in a.size.lower().split("x"))
    fmts = ["gray2", "gray4", "cgray2", "cgray4", "rgb24"] if a.fmt == "all" else [a.fmt]

    s = ws_connect(a.host, a.port)
    ws_send(s, m("gameName", b"dmd_sender"))
    ws_send(s, m("dimensions", struct.pack("<ii", w, h)))
    ws_send(s, m("color", struct.pack("<i", 0xFF4500)))
    print(f"connected to {a.host}:{a.port}, {w}x{h}, formats {fmts}")
    t0 = time.monotonic()
    n = 0
    last_fmt = None
    try:
        while True:
            t = time.monotonic() - t0
            if a.seconds and t >= a.seconds:
                break
            fmt = fmts[int(t / a.each) % len(fmts)]
            if fmt != last_fmt:
                print(f"{t:6.1f}s  {fmt}")
                last_fmt = fmt
            ws_send(s, frame_msg(fmt, w, h, t, int(t * 1000)))
            n += 1
            nxt = t0 + n / a.fps
            d = nxt - time.monotonic()
            if d > 0:
                time.sleep(d)
    except KeyboardInterrupt:
        pass
    el = time.monotonic() - t0
    print(f"{n} frames in {el:.1f}s = {n / el:.1f} fps")
    try:
        s.sendall(ws_frame(0x8, b"", True))
    except OSError:
        pass
    s.close()


if __name__ == "__main__":
    main()
