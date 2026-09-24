#!/usr/bin/env python3
"""RFB (VNC) test server for the NucleoOS Second Screen viewer — exercises every encoding the
board supports with a deterministic picture, so the result can be checked pixel by pixel through
the board's /api/screen.

  python rfb_testserver.py --enc zrle --size 1920x1080            # board connects to PC:5900
  python rfb_testserver.py --enc tight-jpeg --password secret
  python rfb_testserver.py --enc raw --reverse 192.168.0.128       # server dials the board (5500)
  python rfb_testserver.py --enc zrle --screen                     # live capture of this PC

--enc: raw, copyrect, hextile, zlib, zrle, tight-fill, tight-basic, tight-palette, tight-gradient,
       tight-jpeg, auto (first one the client lists).
Writes the expected picture to --expect (PNG) so a test can compare it with the board screenshot.
"""
import argparse
import io
import os
import socket
import struct
import sys
import threading
import time
import zlib

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from PIL import Image, ImageDraw, ImageFont, ImageGrab

ENC = {"raw": 0, "copyrect": 1, "hextile": 5, "zlib": 6, "tight": 7, "zrle": 16}


def test_picture(w, h, frame=0):
    im = Image.new("RGB", (w, h), (18, 24, 38))
    d = ImageDraw.Draw(im)
    bars = [(255, 255, 255), (255, 255, 0), (0, 255, 255), (0, 255, 0),
            (255, 0, 255), (255, 0, 0), (0, 0, 255), (0, 0, 0)]
    for i, c in enumerate(bars):
        d.rectangle([i * w // 8, 0, (i + 1) * w // 8 - 1, h * 45 // 100], fill=c)
    for x in range(w):   # grey ramp
        v = x * 255 // max(w - 1, 1)
        d.line([(x, h * 45 // 100), (x, h * 55 // 100)], fill=(v, v, v))
    for i in range(0, w, max(w // 16, 8)):   # checker / fine detail row
        for j in range(h * 55 // 100, h * 70 // 100, 16):
            if ((i // max(w // 16, 8)) + (j // 16)) & 1:
                d.rectangle([i, j, i + max(w // 16, 8) - 1, j + 15], fill=(230, 120, 30))
    try:
        f = ImageFont.truetype("arial.ttf", max(18, h // 14))
    except OSError:
        f = ImageFont.load_default()
    d.text((w // 30, h * 74 // 100), f"RFB test {w}x{h}  frame {frame}", fill=(255, 255, 255), font=f)
    d.rectangle([w - w // 6 - frame * 7 % (w // 2), h * 88 // 100, w - frame * 7 % (w // 2) - 1, h - 4],
                fill=(40, 200, 120))
    return im


class Client:
    def __init__(self, sock, args):
        self.s, self.a = sock, args
        self.pf = dict(bpp=32, depth=24, be=0, tc=1, rmax=255, gmax=255, bmax=255, rs=16, gs=8, bs=0)
        self.encs = []
        self.zlib = zlib.compressobj(6)
        self.zrle = zlib.compressobj(6)
        self.tight = [zlib.compressobj(6) for _ in range(4)]
        self.w, self.h = args.w, args.h
        self.frame = 0
        self.img = None
        self.sent_once = False
        self.ext = False
        self.pointer_log = []

    # ---- io
    def rx(self, n):
        b = b""
        while len(b) < n:
            c = self.s.recv(n - len(b))
            if not c:
                raise ConnectionError("closed")
            b += c
        return b

    def tx(self, b):
        self.s.sendall(b)

    # ---- pixels
    def pix(self, r, g, b):
        p = self.pf
        v = ((r * p["rmax"] // 255) << p["rs"]) | ((g * p["gmax"] // 255) << p["gs"]) | ((b * p["bmax"] // 255) << p["bs"])
        n = p["bpp"] // 8
        return v.to_bytes(n, "big" if p["be"] else "little")

    def raw_rect(self, im, x, y, w, h):
        px = im.crop((x, y, x + w, y + h)).tobytes()
        if self.pf["bpp"] == 32 and self.pf["rs"] == 16 and not self.pf["be"]:
            out = bytearray(w * h * 4)
            out[0::4], out[1::4], out[2::4] = px[2::3], px[1::3], px[0::3]
            return bytes(out)
        return b"".join(self.pix(px[i], px[i + 1], px[i + 2]) for i in range(0, len(px), 3))

    def cpixel(self, r, g, b):   # ZRLE CPIXEL (3 bytes for 32bpp depth 24)
        return self.pix(r, g, b)[:3] if not self.pf["be"] else self.pix(r, g, b)[1:]

    # ---- encodings
    def enc_raw(self, im, x, y, w, h):
        return struct.pack(">HHHHi", x, y, w, h, 0) + self.raw_rect(im, x, y, w, h)

    def enc_hextile(self, im, x, y, w, h):
        out = [struct.pack(">HHHHi", x, y, w, h, 5)]
        first = True
        for ty in range(y, y + h, 16):
            for tx in range(x, x + w, 16):
                tw, th = min(16, x + w - tx), min(16, y + h - ty)
                t = im.crop((tx, ty, tx + tw, ty + th))
                colors = t.getcolors(tw * th)
                if colors and len(colors) == 1:
                    out.append(bytes([2]) + self.pix(*colors[0][1]))   # background only
                elif colors and len(colors) <= 4 and not first:
                    # background + coloured 1x1..16x1 runs per row
                    bg = max(colors)[1]
                    subs = []
                    px = t.load()
                    for ry in range(th):
                        rx = 0
                        while rx < tw:
                            c = px[rx, ry]
                            if c == bg:
                                rx += 1
                                continue
                            e = rx
                            while e + 1 < tw and px[e + 1, ry] == c:
                                e += 1
                            subs.append(self.pix(*c) + bytes([(rx << 4) | ry, ((e - rx) << 4) | 0]))
                            rx = e + 1
                    if len(subs) < 255:
                        out.append(bytes([2 | 8 | 16]) + self.pix(*bg) + bytes([len(subs)]) + b"".join(subs))
                    else:
                        out.append(bytes([1]) + self.raw_rect(t, 0, 0, tw, th))
                else:
                    out.append(bytes([1]) + self.raw_rect(t, 0, 0, tw, th))
                first = False
        return b"".join(out)

    def enc_zlib(self, im, x, y, w, h):
        data = self.zlib.compress(self.raw_rect(im, x, y, w, h)) + self.zlib.flush(zlib.Z_SYNC_FLUSH)
        return struct.pack(">HHHHiI", x, y, w, h, 6, len(data)) + data

    def enc_zrle(self, im, x, y, w, h):
        body = bytearray()
        for ty in range(y, y + h, 64):
            for tx in range(x, x + w, 64):
                tw, th = min(64, x + w - tx), min(64, y + h - ty)
                t = im.crop((tx, ty, tx + tw, ty + th))
                colors = t.getcolors(tw * th)
                px = list(t.getdata())
                if colors and len(colors) == 1:
                    body += bytes([1]) + self.cpixel(*colors[0][1])
                elif colors and len(colors) <= 16:
                    pal = [c for _, c in colors]
                    idx = {c: i for i, c in enumerate(pal)}
                    n = len(pal)
                    bpp = 1 if n == 2 else 2 if n <= 4 else 4
                    body += bytes([n]) + b"".join(self.cpixel(*c) for c in pal)
                    for r in range(th):
                        acc, bits = 0, 0
                        row = bytearray()
                        for q in range(tw):
                            acc = (acc << bpp) | idx[px[r * tw + q]]
                            bits += bpp
                            if bits == 8:
                                row.append(acc); acc = bits = 0
                        if bits:
                            row.append(acc << (8 - bits))
                        body += row
                elif colors and len(colors) <= 127:
                    pal = [c for _, c in colors]
                    idx = {c: i for i, c in enumerate(pal)}
                    body += bytes([128 + len(pal)]) + b"".join(self.cpixel(*c) for c in pal)
                    i = 0
                    while i < len(px):
                        j = i
                        while j + 1 < len(px) and px[j + 1] == px[i]:
                            j += 1
                        run = j - i + 1
                        if run == 1:
                            body.append(idx[px[i]])
                        else:
                            body.append(idx[px[i]] | 128)
                            run -= 1
                            while run >= 255:
                                body.append(255); run -= 255
                            body.append(run)
                        i = j + 1
                else:
                    # plain RLE
                    body.append(128)
                    i = 0
                    while i < len(px):
                        j = i
                        while j + 1 < len(px) and px[j + 1] == px[i]:
                            j += 1
                        body += self.cpixel(*px[i])
                        run = j - i
                        while run >= 255:
                            body.append(255); run -= 255
                        body.append(run)
                        i = j + 1
        data = self.zrle.compress(bytes(body)) + self.zrle.flush(zlib.Z_SYNC_FLUSH)
        return struct.pack(">HHHHiI", x, y, w, h, 16, len(data)) + data

    @staticmethod
    def compact(n):
        b = bytearray([n & 0x7F])
        if n > 0x7F:
            b[0] |= 0x80
            b.append((n >> 7) & 0x7F)
            if n > 0x3FFF:
                b[1] |= 0x80
                b.append((n >> 14) & 0xFF)
        return bytes(b)

    def tight_zdata(self, sid, raw):
        if len(raw) < 12:
            return raw
        z = self.tight[sid].compress(raw) + self.tight[sid].flush(zlib.Z_SYNC_FLUSH)
        return self.compact(len(z)) + z

    def enc_tight(self, im, x, y, w, h, mode):
        hdr = struct.pack(">HHHHi", x, y, w, h, 7)
        t = im.crop((x, y, x + w, y + h))
        if mode == "tight-fill":
            c = t.getcolors(w * h)
            if c and len(c) == 1:
                r, g, b = c[0][1]
                return hdr + bytes([0x80, r, g, b])
            mode = "tight-basic"
        if mode == "tight-jpeg":
            buf = io.BytesIO()
            t.save(buf, "JPEG", quality=80)
            j = buf.getvalue()
            return hdr + bytes([0x90]) + self.compact(len(j)) + j
        if mode == "tight-palette":
            c = t.getcolors(256)
            if c:
                pal = [col for _, col in c]
                idx = {col: i for i, col in enumerate(pal)}
                px = list(t.getdata())
                if len(pal) == 2:
                    rows = bytearray()
                    for r in range(h):
                        acc, bits = 0, 0
                        for q in range(w):
                            acc = (acc << 1) | idx[px[r * w + q]]
                            bits += 1
                            if bits == 8:
                                rows.append(acc); acc = bits = 0
                        if bits:
                            rows.append(acc << (8 - bits))
                    data = bytes(rows)
                else:
                    data = bytes(idx[p] for p in px)
                return hdr + bytes([0x40 | (1 << 4), 1, len(pal) - 1]) + b"".join(bytes(p) for p in pal) + self.tight_zdata(1, data)
            mode = "tight-basic"
        if mode == "tight-gradient":
            try:
                import numpy as np
                a = np.asarray(t, dtype=np.int16)
                left = np.zeros_like(a); left[:, 1:] = a[:, :-1]
                up = np.zeros_like(a); up[1:, :] = a[:-1, :]
                ul = np.zeros_like(a); ul[1:, 1:] = a[:-1, :-1]
                pred = np.clip(left + up - ul, 0, 255)
                out = ((a - pred) & 0xFF).astype(np.uint8).tobytes()
                return hdr + bytes([0x40 | (2 << 4), 2]) + self.tight_zdata(2, out)
            except ImportError:
                pass
            px = t.tobytes()
            out = bytearray(len(px))
            prev = bytearray(w * 3)
            for r in range(h):
                left = [0, 0, 0]
                upleft = [0, 0, 0]
                for q in range(w):
                    for ch in range(3):
                        up = prev[q * 3 + ch]
                        pr = max(0, min(255, left[ch] + up - upleft[ch]))
                        v = px[(r * w + q) * 3 + ch]
                        out[(r * w + q) * 3 + ch] = (v - pr) & 0xFF
                        upleft[ch] = up
                        left[ch] = v
                        prev[q * 3 + ch] = v
            return hdr + bytes([0x40 | (2 << 4), 2]) + self.tight_zdata(2, bytes(out))
        # basic copy filter, stream 0: TPIXEL = R,G,B
        return hdr + bytes([0x00]) + self.tight_zdata(0, t.tobytes())

    # ---- updates
    def rects(self, full):
        """Split the frame into rects (Tight servers cap rects at 65536 px)."""
        out = []
        step_h = max(1, 65536 // self.w) if self.a.enc.startswith("tight") else 128
        for y in range(0, self.h, step_h):
            out.append((0, y, self.w, min(step_h, self.h - y)))
        return out

    def picture(self):
        if self.a.screen:
            im = ImageGrab.grab(all_screens=False).convert("RGB")
            if im.size != (self.w, self.h):
                im = im.resize((self.w, self.h))
            return im
        return test_picture(self.w, self.h, self.frame)

    def send_update(self, incremental):
        animating = self.a.animate and (not self.a.frames or self.frame < self.a.frames)
        if incremental and self.sent_once and not animating and not self.a.screen and not self.a.evil:
            return False
        if self.a.evil and self.sent_once:
            # hostile server: CopyRect whose SOURCE lies far outside the framebuffer
            self.tx(struct.pack(">BBH", 0, 0, 1) + struct.pack(">HHHHiHH", 0, 0, 64, 64, 1, 60000, 60000))
            print("sent hostile CopyRect (src 60000,60000)")
            self.a.evil = False
            return True
        im = self.picture()
        self.frame += 1
        enc = self.a.enc
        rects = []
        if enc == "copyrect" and self.img is not None and 1 not in self.encs:
            enc = "raw"   # the client didn't ask for CopyRect (e.g. a scaling viewer): plain updates
        if enc == "copyrect" and self.img is not None:
            # scroll the previous picture up by 40 px with one CopyRect + a raw strip
            rects.append(struct.pack(">HHHHiHH", 0, 0, self.w, self.h - 40, 1, 0, 40))
            strip = im.crop((0, self.h - 40, self.w, self.h))
            shifted = self.img.crop((0, 40, self.w, self.h))
            im = Image.new("RGB", (self.w, self.h))
            im.paste(shifted, (0, 0))
            im.paste(strip, (0, self.h - 40))
            rects.append(self.enc_raw(im, 0, self.h - 40, self.w, 40))
        else:
            for (x, y, w, h) in self.rects(not incremental):
                if enc in ("raw", "copyrect"):
                    rects.append(self.enc_raw(im, x, y, w, h))
                elif enc == "hextile":
                    rects.append(self.enc_hextile(im, x, y, w, h))
                elif enc == "zlib":
                    rects.append(self.enc_zlib(im, x, y, w, h))
                elif enc == "zrle":
                    rects.append(self.enc_zrle(im, x, y, w, h))
                else:
                    rects.append(self.enc_tight(im, x, y, w, h, enc))
        self.img = im
        self.tx(struct.pack(">BBH", 0, 0, len(rects)) + b"".join(rects))
        self.sent_once = True
        if self.a.expect:
            im.save(self.a.expect)
        return True

    def run(self):
        self.tx(b"RFB 003.008\n")
        ver = self.rx(12)
        print("client version", ver)
        if self.a.ard:
            # Apple Remote Desktop auth (type 30), as macOS Screen Sharing offers it
            import hashlib
            from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes
            self.tx(bytes([1, 30]))
            assert self.rx(1)[0] == 30
            p = int("FFFFFFFFFFFFFFFFC90FDAA22168C234C4C6628B80DC1CD129024E088A67CC74020BBEA63B139B22514A08798E3404DD"
                    "EF9519B3CD3A431B302B0A6DF25F14374FE1356D6D51C245E485B576625E7EC6F44C42E9A637ED6B0BFF5CB6F406B7ED"
                    "EE386BFB5A899FA5AE9F24117C4B1FE649286651ECE65381FFFFFFFFFFFFFFFF", 16)   # RFC 2409 group 2
            kl, g = 128, 2
            x = int.from_bytes(os.urandom(kl - 1), "big")
            spub = pow(g, x, p)
            self.tx(struct.pack(">HH", g, kl) + p.to_bytes(kl, "big") + spub.to_bytes(kl, "big"))
            ct = self.rx(128)
            cpub = int.from_bytes(self.rx(kl), "big")
            key = hashlib.md5(pow(cpub, x, p).to_bytes(kl, "big")).digest()
            plain = Cipher(algorithms.AES(key), modes.ECB()).decryptor().update(ct)
            user = plain[:64].split(b"\0")[0].decode(errors="replace")
            pw = plain[64:].split(b"\0")[0].decode(errors="replace")
            want_user, want_pw = self.a.ard.split(":", 1)
            ok = user == want_user and pw == want_pw
            print(f"ARD auth user={user!r} -> {'OK' if ok else 'FAILED'}")
            if not ok:
                msg = b"Authentication failed"
                self.tx(struct.pack(">II", 1, len(msg)) + msg)
                return
            self.tx(struct.pack(">I", 0))
        elif self.a.password:
            self.tx(bytes([1, 2]))
            assert self.rx(1)[0] == 2
            ch = os.urandom(16)
            self.tx(ch)
            resp = self.rx(16)
            from des_ref_vnc import vnc_response
            ok = resp == vnc_response(self.a.password, ch)
            print("auth", "OK" if ok else "FAILED")
            if not ok:
                msg = b"password check failed"
                self.tx(struct.pack(">II", 1, len(msg)) + msg)
                return
            self.tx(struct.pack(">I", 0))
        else:
            self.tx(bytes([1, 1]))
            assert self.rx(1)[0] == 1
            self.tx(struct.pack(">I", 0))
        shared = self.rx(1)
        name = self.a.name.encode()
        pf = struct.pack(">BBBBHHHBBB3x", 32, 24, 0, 1, 255, 255, 255, 16, 8, 0)
        self.tx(struct.pack(">HH", self.w, self.h) + pf + struct.pack(">I", len(name)) + name)
        pending = False
        self.s.settimeout(0.2)
        last_anim = time.time()
        while True:
            try:
                t = self.rx(1)[0]
            except socket.timeout:
                if pending and (self.a.animate or self.a.screen) and time.time() - last_anim > self.a.interval:
                    last_anim = time.time()
                    if self.send_update(True):
                        pending = False
                continue
            self.s.settimeout(None)
            if t == 0:
                d = self.rx(19)
                bpp, depth, be, tc, rmax, gmax, bmax, rs, gs, bs = struct.unpack(">3xBBBBHHHBBB3x", d)
                self.pf = dict(bpp=bpp, depth=depth, be=be, tc=tc, rmax=rmax, gmax=gmax, bmax=bmax, rs=rs, gs=gs, bs=bs)
                print("SetPixelFormat", self.pf)
            elif t == 2:
                _, n = struct.unpack(">BH", self.rx(3))
                self.encs = list(struct.unpack(f">{n}i", self.rx(4 * n)))
                print("SetEncodings", self.encs)
                if self.a.enc == "auto":
                    names = {v: k for k, v in ENC.items()}
                    self.a.enc = next((names[e] for e in self.encs if e in names), "raw")
                    if self.a.enc == "tight":
                        self.a.enc = "tight-jpeg"
                    print("auto ->", self.a.enc)
                self.ext = -308 in self.encs
                if self.ext and self.a.resizable:
                    scr = struct.pack(">IHHHHI", 1, 0, 0, self.w, self.h, 0)
                    self.tx(struct.pack(">BBH", 0, 0, 1) + struct.pack(">HHHHi", 0, 0, self.w, self.h, -308) +
                            struct.pack(">B3x", 1) + scr)
            elif t == 3:
                inc, x, y, w, h = struct.unpack(">BHHHH", self.rx(9))
                if self.send_update(bool(inc)):
                    pending = False
                else:
                    pending = True
            elif t == 4:
                self.rx(7)
            elif t == 5:
                mask, x, y = struct.unpack(">BHH", self.rx(5))
                self.pointer_log.append((time.time(), mask, x, y))
                print(f"pointer mask={mask} x={x} y={y}")
            elif t == 6:
                self.rx(3)
                n = struct.unpack(">I", self.rx(4))[0]
                self.rx(n)
            elif t == 251:
                _, w, h, ns, _ = struct.unpack(">BHHBB", self.rx(7))
                self.rx(16 * ns)
                print(f"SetDesktopSize {w}x{h}")
                if self.a.resizable:
                    self.w, self.h = w, h
                    scr = struct.pack(">IHHHHI", 1, 0, 0, w, h, 0)
                    self.tx(struct.pack(">BBH", 0, 0, 1) + struct.pack(">HHHHi", 1, 0, w, h, -308) +
                            struct.pack(">B3x", 1) + scr)
                    self.sent_once = False
            else:
                print("unknown client message", t)
                return
            self.s.settimeout(0.2)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=5900)
    ap.add_argument("--enc", default="auto")
    ap.add_argument("--size", default="1024x600")
    ap.add_argument("--password", default="")
    ap.add_argument("--ard", default="", help="user:password -> offer only Apple auth (type 30)")
    ap.add_argument("--name", default="RFB test server")
    ap.add_argument("--reverse", default="", help="board IP: connect to it on port 5500 instead of listening")
    ap.add_argument("--animate", action="store_true", help="send a changing picture on each request")
    ap.add_argument("--interval", type=float, default=0.5)
    ap.add_argument("--frames", type=int, default=0, help="with --animate: stop changing after N updates")
    ap.add_argument("--screen", action="store_true", help="live capture of this PC's primary screen")
    ap.add_argument("--resizable", action="store_true", help="accept SetDesktopSize (like Xvnc/GNOME extend)")
    ap.add_argument("--expect", default="", help="save the last sent picture here (PNG)")
    ap.add_argument("--once", action="store_true", help="exit after one client")
    ap.add_argument("--evil", action="store_true", help="after the first picture, send an out-of-bounds CopyRect")
    a = ap.parse_args()
    a.w, a.h = (int(v) for v in a.size.lower().split("x"))
    if a.reverse:
        s = socket.create_connection((a.reverse, 5500), timeout=10)
        s.settimeout(None)
        print("reverse-connected to", a.reverse)
        Client(s, a).run()
        return
    ls = socket.socket()
    ls.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    ls.bind(("0.0.0.0", a.port))
    ls.listen(1)
    print(f"RFB test server on :{a.port} enc={a.enc} size={a.w}x{a.h}", flush=True)
    while True:
        s, addr = ls.accept()
        print("client", addr, flush=True)
        try:
            Client(s, a).run()
        except (ConnectionError, OSError) as e:
            print("client gone:", e, flush=True)
        s.close()
        if a.once:
            break


if __name__ == "__main__":
    main()
