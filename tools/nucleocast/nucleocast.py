#!/usr/bin/env python3
"""NucleoCast helper — stream a computer screen to a NucleoOS board (Second Screen, Wi-Fi/LAN).

Works on Windows, macOS and Linux (X11). Only needs Pillow (pip install pillow); numpy makes the
change detection faster when present. The board's touch is applied back to the computer as mouse
clicks/drags (Windows: SendInput, macOS: Quartz, Linux: xdotool).

  python nucleocast.py                      # find the board (nucleov2.local) and share monitor 1
  python nucleocast.py --host 192.168.0.128 --monitor 2
  python nucleocast.py --host 192.168.0.128 --test        # animated test picture, no capture
  python nucleocast.py --host 192.168.0.128 --bench 20    # throughput test for 20 s

Protocol: WebSocket /cast on port 7070 (see components/nv_secondscreen/ss_cast.cpp).
"""
import argparse
import base64
import io
import json
import os
import platform
import socket
import struct
import sys
import threading
import time

try:
    from PIL import Image, ImageDraw, ImageGrab, ImageFont
except ImportError:
    sys.exit("Pillow is required: pip install pillow")
try:
    import numpy as np
except ImportError:
    np = None

W, H, CELL = 1024, 600, 32


# ---------------------------------------------------------------- minimal WebSocket client
class WS:
    def __init__(self, host, port, path="/cast", timeout=10):
        self.s = socket.create_connection((host, port), timeout=timeout)
        self.s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        key = base64.b64encode(os.urandom(16)).decode()
        req = (f"GET {path} HTTP/1.1\r\nHost: {host}:{port}\r\nUpgrade: websocket\r\n"
               f"Connection: Upgrade\r\nSec-WebSocket-Key: {key}\r\nSec-WebSocket-Version: 13\r\n"
               f"User-Agent: nucleocast-python/{platform.system()}\r\n\r\n")
        self.s.sendall(req.encode())
        head = b""
        while b"\r\n\r\n" not in head:
            c = self.s.recv(1)
            if not c:
                raise ConnectionError("closed during handshake")
            head += c
        status = head.split(b"\r\n", 1)[0]
        if b" 101 " not in status:
            raise ConnectionError(status.decode(errors="replace"))
        self.lock = threading.Lock()
        self.s.settimeout(None)

    def send(self, data, op):
        mask = os.urandom(4)
        n = len(data)
        if n < 126:
            hdr = struct.pack("!BB", 0x80 | op, 0x80 | n)
        elif n < 65536:
            hdr = struct.pack("!BBH", 0x80 | op, 0x80 | 126, n)
        else:
            hdr = struct.pack("!BBQ", 0x80 | op, 0x80 | 127, n)
        if np is not None and n > 4096:
            a = np.frombuffer(data, dtype=np.uint8)
            m = np.frombuffer((mask * (n // 4 + 1))[:n], dtype=np.uint8)
            payload = (a ^ m).tobytes()
        else:
            payload = bytes(b ^ mask[i & 3] for i, b in enumerate(data))
        with self.lock:
            self.s.sendall(hdr + mask + payload)

    def send_text(self, obj):
        self.send(json.dumps(obj).encode(), 1)

    def _exact(self, n):
        b = b""
        while len(b) < n:
            c = self.s.recv(n - len(b))
            if not c:
                raise ConnectionError("closed")
            b += c
        return b

    def recv(self):
        """Returns (opcode, payload). Answers pings itself."""
        while True:
            b0, b1 = self._exact(2)
            op, n = b0 & 0x0F, b1 & 0x7F
            if n == 126:
                n = struct.unpack("!H", self._exact(2))[0]
            elif n == 127:
                n = struct.unpack("!Q", self._exact(8))[0]
            if b1 & 0x80:
                mk = self._exact(4)
                data = bytes(x ^ mk[i & 3] for i, x in enumerate(self._exact(n)))
            else:
                data = self._exact(n)
            if op == 9:
                self.send(data, 10)
                continue
            return op, data

    def close(self):
        try:
            self.send(b"\x03\xe8", 8)
        except OSError:
            pass
        self.s.close()


# ---------------------------------------------------------------- input injection
class Injector:
    """Maps board touches (panel coords) to mouse events on the captured monitor."""

    def __init__(self, box, place):
        self.box = box          # monitor rect on the desktop: (left, top, right, bottom)
        self.place = place      # where the picture sits on the panel: (x, y, w, h)
        self.down = False
        self.sys = platform.system()
        self.ok = True
        if self.sys == "Windows":
            import ctypes
            self.u32 = ctypes.windll.user32
        elif self.sys == "Darwin":
            import ctypes
            import ctypes.util
            self.q = ctypes.cdll.LoadLibrary(ctypes.util.find_library("ApplicationServices"))
            self.q.CGEventCreateMouseEvent.restype = ctypes.c_void_p
            self.q.CGEventCreateMouseEvent.argtypes = [ctypes.c_void_p, ctypes.c_uint32,
                                                       CGPoint, ctypes.c_uint32]
            self.q.CGEventPost.argtypes = [ctypes.c_uint32, ctypes.c_void_p]
            self.q.CFRelease.argtypes = [ctypes.c_void_p]
        else:
            import shutil
            self.ok = shutil.which("xdotool") is not None
            if not self.ok:
                print("touch -> mouse needs xdotool on Linux (sudo apt install xdotool)")

    def to_desktop(self, x, y):
        px, py, pw, ph = self.place
        l, t, r, b = self.box
        fx = min(max((x - px) / max(pw, 1), 0.0), 0.9999)
        fy = min(max((y - py) / max(ph, 1), 0.0), 0.9999)
        return int(l + fx * (r - l)), int(t + fy * (b - t))

    def touch(self, pts):
        if not self.ok:
            return
        if pts:
            x, y = self.to_desktop(pts[0][1], pts[0][2])
            self._move(x, y)
            if not self.down:
                self._button(True, x, y)
                self.down = True
        elif self.down:
            self._button(False, None, None)
            self.down = False

    def _move(self, x, y):
        self.last = (x, y)
        if self.sys == "Windows":
            self.u32.SetCursorPos(x, y)
        elif self.sys == "Darwin":
            ev = self.q.CGEventCreateMouseEvent(None, 6 if self.down else 5, CGPoint(x, y), 0)
            self.q.CGEventPost(0, ev)
            self.q.CFRelease(ev)
        else:
            os.system(f"xdotool mousemove {x} {y}")

    def _button(self, press, x, y):
        if self.sys == "Windows":
            self.u32.mouse_event(0x0002 if press else 0x0004, 0, 0, 0, 0)
        elif self.sys == "Darwin":
            x, y = self.last
            ev = self.q.CGEventCreateMouseEvent(None, 1 if press else 2, CGPoint(x, y), 0)
            self.q.CGEventPost(0, ev)
            self.q.CFRelease(ev)
        else:
            os.system(f"xdotool {'mousedown' if press else 'mouseup'} 1")


if platform.system() == "Darwin":
    import ctypes

    class CGPoint(ctypes.Structure):
        _fields_ = [("x", ctypes.c_double), ("y", ctypes.c_double)]
else:
    CGPoint = None


# ---------------------------------------------------------------- sources
def monitors():
    """List of desktop rects (l, t, r, b). Windows/macOS/X11 via Pillow; index 0 = all screens."""
    boxes = []
    if platform.system() == "Windows":
        import ctypes
        from ctypes import wintypes
        user32 = ctypes.windll.user32
        try:
            ctypes.windll.shcore.SetProcessDpiAwareness(2)
        except Exception:
            user32.SetProcessDPIAware()
        cb_t = ctypes.WINFUNCTYPE(ctypes.c_int, wintypes.HMONITOR, wintypes.HDC,
                                  ctypes.POINTER(wintypes.RECT), wintypes.LPARAM)

        def cb(h, dc, rc, lp):
            r = rc.contents
            boxes.append((r.left, r.top, r.right, r.bottom))
            return 1
        user32.EnumDisplayMonitors(None, None, cb_t(cb), 0)
    if not boxes:
        im = ImageGrab.grab(all_screens=True) if platform.system() == "Windows" else ImageGrab.grab()
        boxes.append((0, 0, im.width, im.height))
    return boxes


class ScreenSource:
    def __init__(self, box, fit):
        self.box, self.fit = box, fit

    def frame(self):
        im = ImageGrab.grab(bbox=self.box, all_screens=True) if platform.system() == "Windows" \
            else ImageGrab.grab(bbox=self.box)
        return compose(im.convert("RGB"), self.fit)


class TestSource:
    def __init__(self):
        self.t0 = time.time()
        try:
            self.font = ImageFont.truetype("arial.ttf", 40)
        except OSError:
            self.font = ImageFont.load_default()

    def frame(self):
        im = Image.new("RGB", (W, H), (20, 26, 40))
        d = ImageDraw.Draw(im)
        bars = [(255, 255, 255), (255, 255, 0), (0, 255, 255), (0, 255, 0),
                (255, 0, 255), (255, 0, 0), (0, 0, 255), (0, 0, 0)]
        for i, c in enumerate(bars):
            d.rectangle([i * W // 8, 0, (i + 1) * W // 8 - 1, int(H * 0.55)], fill=c)
        for x in range(W):
            v = x * 255 // (W - 1)
            d.line([(x, int(H * 0.55)), (x, int(H * 0.66))], fill=(v, v, v))
        t = time.time() - self.t0
        bx = int((W - 120) * (0.5 + 0.5 * __import__("math").sin(t * 1.3)))
        d.rectangle([bx, int(H * 0.68), bx + 120, int(H * 0.8)], fill=(255, 140, 0))
        d.text((24, int(H * 0.84)), f"NucleoCast test  {time.strftime('%H:%M:%S')}  t={t:5.1f}s",
               fill=(255, 255, 255), font=self.font)
        return im, (0, 0, W, H)


def compose(src, fit):
    """Scale a captured picture into the 1024x600 panel. Returns (image, placement rect)."""
    sw, sh = src.size
    s = max(W / sw, H / sh) if fit == "cover" else min(W / sw, H / sh)
    dw, dh = max(1, round(sw * s)), max(1, round(sh * s))
    im = Image.new("RGB", (W, H))
    im.paste(src.resize((dw, dh), Image.BILINEAR), ((W - dw) // 2, (H - dh) // 2))
    return im, ((W - dw) // 2, (H - dh) // 2, dw, dh)


# ---------------------------------------------------------------- change detection
def dirty_rects(cur, prev):
    if prev is None:
        return [(0, 0, W, H)]
    cols, rows = W // CELL, (H + CELL - 1) // CELL
    if np is not None:
        a = np.asarray(cur, dtype=np.uint8)
        b = np.asarray(prev, dtype=np.uint8)
        diff = np.any(a != b, axis=2)
        pad = np.zeros((rows * CELL, W), dtype=bool)
        pad[:H] = diff
        cellmap = pad.reshape(rows, CELL, cols, CELL).any(axis=(1, 3))
    else:
        from PIL import ImageChops
        cellmap = [[False] * cols for _ in range(rows)]
        d = ImageChops.difference(cur, prev)
        for ry in range(rows):
            for rx in range(cols):
                box = (rx * CELL, ry * CELL, rx * CELL + CELL, min(H, ry * CELL + CELL))
                cellmap[ry][rx] = d.crop(box).getbbox() is not None
    n = sum(1 for ry in range(rows) for rx in range(cols) if cellmap[ry][rx])
    if n == 0:
        return []
    if n > cols * rows * 0.45:
        return [(0, 0, W, H)]
    open_rects = []
    for ry in range(rows):
        rx = 0
        while rx < cols:
            if cellmap[ry][rx]:
                e = rx
                while e + 1 < cols and cellmap[ry][e + 1]:
                    e += 1
                for o in open_rects:
                    if o[0] == rx and o[1] == e and o[3] == ry:
                        o[3] = ry + 1
                        break
                else:
                    open_rects.append([rx, e, ry, ry + 1])
                rx = e + 1
            else:
                rx += 1
    rects = [(a * CELL, y0 * CELL, (b - a + 1) * CELL, min(H, y1 * CELL) - y0 * CELL)
             for a, b, y0, y1 in open_rects]
    return rects if len(rects) <= 40 else [(0, 0, W, H)]


def jpeg(im, q):
    buf = io.BytesIO()
    im.save(buf, "JPEG", quality=q, subsampling=2, optimize=False)   # baseline 4:2:0
    return buf.getvalue()


# ---------------------------------------------------------------- session
class Cast:
    def __init__(self, host, port, source, quality, injector=None, bench=0, verbose=True):
        self.ws = WS(host, port)
        self.source, self.q_mode, self.inj = source, quality, injector
        self.q = 70 if quality == "auto" else int(quality)
        self.ready = threading.Event()
        self.slots = threading.Semaphore(2)   # frames in flight (hides the Wi-Fi round trip)
        self.need_full = True
        self.state = "?"
        self.alive = True
        self.bench = bench
        self.verbose = verbose
        self.stats = {"frames": 0, "bytes": 0, "tiles": 0, "acks": 0, "rtt": [], "shown": 0}
        self.sent_at = {}
        self.token_file = os.path.join(os.path.expanduser("~"), ".nucleocast_token")
        threading.Thread(target=self._rx, daemon=True).start()
        token = ""
        try:
            token = open(self.token_file).read().strip()
        except OSError:
            pass
        self.ws.send_text({"t": "hi", "agent": f"NucleoCast {platform.system()}", "token": token,
                           "touch": injector is not None})

    def log(self, *a):
        if self.verbose:
            print(*a, flush=True)

    def _rx(self):
        try:
            while self.alive:
                op, data = self.ws.recv()
                if op == 8:
                    break
                if op != 1:
                    continue
                m = json.loads(data)
                t = m.get("t")
                if t == "hello":
                    self.log(f"board: {m.get('name')} fw {m.get('fw')} {m.get('w')}x{m.get('h')}")
                elif t == "state":
                    self.state = m["s"]
                    self.log("state:", m["s"])
                    if m["s"] in ("ready", "live"):
                        self.ready.set()
                        if m["s"] == "ready":
                            self.need_full = True
                    elif m["s"] in ("waiting", "paused", "pending"):
                        self.ready.clear()
                    elif m["s"] == "occupied":   # another source has the display: retry every 3 s
                        self.ready.clear()
                        threading.Timer(3.0, self._retry_occupied).start()
                    elif m["s"] in ("denied", "busy"):
                        self.alive = False
                elif t == "trust":
                    with open(self.token_file, "w") as f:
                        f.write(m["token"])
                elif t == "ack":
                    t0 = self.sent_at.pop(m["seq"], None)
                    if t0:
                        rtt = (time.time() - t0) * 1000
                        self.stats["rtt"].append(rtt)
                        if self.q_mode == "auto":
                            self.q = max(35, self.q - 8) if rtt > 300 else min(85, self.q + 3) if rtt < 90 else self.q
                    self.stats["acks"] += 1
                    self.stats["shown"] += 1 if m.get("shown") else 0
                    if not m.get("shown") and self.state == "occupied":
                        self.ready.clear()
                        threading.Timer(3.0, self._retry_occupied).start()
                    self.slots.release()
                elif t == "full":
                    self.need_full = True
                elif t == "bye":
                    self.log("the board ended the connection")
                    self.alive = False
                    break
                elif t == "touch" and self.inj:
                    self.inj.touch(m.get("p", []))
        except (ConnectionError, OSError) as e:
            self.log("link closed:", e)
        self.alive = False
        self.ready.set()
        for _ in range(2):
            self.slots.release()

    def _retry_occupied(self):
        if self.alive and self.state == "occupied":
            self.need_full = True
            self.ready.set()

    def run(self):
        prev, seq, t_end = None, 1, (time.time() + self.bench if self.bench else None)
        last_report = time.time()
        while self.alive:
            if t_end and time.time() > t_end:
                break
            if not self.ready.wait(0.5) or not self.slots.acquire(timeout=2.0):
                continue
            if not self.alive:
                break
            im, place = self.source.frame()
            if self.inj:
                self.inj.place = place
            rects = [(0, 0, W, H)] if self.need_full or self.bench else dirty_rects(im, prev)
            if not rects:
                self.slots.release()   # nothing sent: give the in-flight slot back
                time.sleep(0.03)
                continue
            self.need_full = False
            prev = im
            self.sent_at[seq] = time.time()
            for i, (x, y, w, h) in enumerate(rects):
                data = jpeg(im.crop((x, y, x + w, y + h)), self.q)
                pkt = struct.pack("<BBBBHHHHI", 78, 67, 1, 1 if i == len(rects) - 1 else 0,
                                  x, y, 0, 0, seq) + data
                self.ws.send(pkt, 2)
                self.stats["bytes"] += len(pkt)
                self.stats["tiles"] += 1
            self.stats["frames"] += 1
            seq += 1
            if time.time() - last_report > 2 and self.verbose:
                dt = time.time() - last_report
                r = self.stats["rtt"][-20:]
                self.log(f"{self.stats['frames'] / dt:5.1f} fps  {self.stats['bytes'] / dt / 1024:6.0f} kB/s  "
                         f"q={self.q}  rtt={sum(r) / max(len(r), 1):5.0f} ms  state={self.state}")
                self.stats["frames"] = self.stats["bytes"] = 0
                last_report = time.time()
        self.ws.close()


def resolve(host):
    try:
        return socket.gethostbyname(host)
    except OSError:
        return host


def main():
    ap = argparse.ArgumentParser(description="Stream this computer's screen to a NucleoOS board.")
    ap.add_argument("--host", default="nucleov2.local", help="board address (default nucleov2.local)")
    ap.add_argument("--port", type=int, default=7070)
    ap.add_argument("--monitor", type=int, default=1, help="monitor number (1 = primary)")
    ap.add_argument("--fit", choices=["contain", "cover"], default="contain")
    ap.add_argument("--quality", default="auto", help="auto or 30..95")
    ap.add_argument("--test", action="store_true", help="send an animated test picture")
    ap.add_argument("--no-input", action="store_true", help="don't apply the board's touch")
    ap.add_argument("--bench", type=float, default=0, help="send full frames for N seconds and report")
    a = ap.parse_args()

    host = resolve(a.host)
    if a.test or a.bench:
        src, inj = TestSource(), None
    else:
        boxes = monitors()
        box = boxes[min(max(a.monitor, 1), len(boxes)) - 1]
        print(f"sharing monitor {a.monitor}: {box}")
        src = ScreenSource(box, a.fit)
        inj = None if a.no_input else Injector(box, (0, 0, W, H))
    print(f"connecting to {host}:{a.port} ...")
    c = Cast(host, a.port, src, a.quality, inj, bench=a.bench)
    try:
        c.run()
    except KeyboardInterrupt:
        pass
    if a.bench:
        r = sorted(c.stats["rtt"])
        print(f"bench: acks={c.stats['acks']} shown={c.stats['shown']} "
              f"rtt p50={r[len(r) // 2] if r else 0:.0f} ms p90={r[int(len(r) * .9)] if r else 0:.0f} ms")


if __name__ == "__main__":
    main()
