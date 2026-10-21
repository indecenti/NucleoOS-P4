#!/usr/bin/env python3
"""HIL check: ABI v19 turned canvas (nv.gfx_set_rotation) - picture, touch mapping and cost.

Drives the test app tests/apps/rotest over Wi-Fi (firmware with ABI >= 19). For each quarter turn
(1, 0, 2, 3, then 1 again):
  1. taps the app's R<q> button, found through the turn in effect (the tap itself checks the
     mapping), and waits for its log line "rot q -> q size W H" (W x H swapped on an odd turn)
  2. screenshot (/api/screen): the four coloured corners of the upright test card must sit on the
     panel where the turn puts them (upright (x, y) -> q=1 (y, W-1-x), q=2 (W-1-x, H-1-y),
     q=3 (H-1-y, x): the system portrait mode's direction)
  3. taps six panel points; the app must log the canvas pixel the turn maps each one to
  4. BENCH: 120 full-canvas repaints; the app logs ms per frame, /api/display the firmware's
     cost per blit (blit_us_avg: the PPA scaling, and turning, the canvas into the panel)
Then back to Home. Remote taps reach the app through the LVGL pointer (the game view maps it with
the same nv_blit_unmap as the raw finger points), so finger touch is checked by the same code.

  python tools/hil/rotation.py              push the app if missing (then: reboot, run again)
  python tools/hil/rotation.py --push       push manifest + app.wasm again (build it first:
                                            .\\sdk\\build_app.ps1 -AppDir tests\\apps\\rotest)

Exit code: 0 pass, 1 fail, 2 board unreachable / older firmware / app not registered yet.
Python 3.8+ and Pillow (the screenshot check). Needs the board paired (tools/pair.py).
"""
import argparse
import io
import json
import os
import re
import sys
import time
import urllib.request

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
import nsh  # noqa: E402  (host discovery)
from nvtoken import auth_headers  # noqa: E402

APP = "rotest"
APP_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "tests", "apps", APP)
PW, PH = 1024, 600
CANVAS = (1024, 600)   # the manifest's canvas: upright size at turn 0
# Panel points that miss the app's button row under every turn (a tap there would turn the canvas).
TAPS = ((100, 100), (900, 80), (300, 450), (700, 200), (40, 560), (1000, 590))
CORNERS = {            # test card squares (60 px): canvas point inside each -> colour
    "red": ((30, 30), (230, 40, 40)),
    "green": ((-31, 30), (40, 200, 60)),
    "blue": ((30, -31), (40, 90, 230)),
    "yellow": ((-31, -31), (240, 220, 40)),
}


def req(host, path, method="GET", data=None, timeout=15, raw=False):
    r = urllib.request.Request("http://%s%s" % (host, path), data=data, method=method, headers=auth_headers())
    with urllib.request.urlopen(r, timeout=timeout) as resp:
        body = resp.read()
    return body if raw else body.decode("utf-8", "replace")


def get_json(host, path, timeout=15):
    return json.loads(req(host, path, timeout=timeout) or "{}")


def size_for(q):
    w, h = CANVAS
    return (h, w) if q & 1 else (w, h)


def to_panel(q, x, y):
    """Upright canvas pixel -> panel pixel (1:1: both shapes fill the 1024x600 panel)."""
    w, h = size_for(q)
    return [(x, y), (y, w - 1 - x), (w - 1 - x, h - 1 - y), (h - 1 - y, x)][q]


def to_canvas(q, px, py):
    w, h = size_for(q)
    return [(px, py), (w - 1 - py, px), (w - 1 - px, h - 1 - py), (py, h - 1 - px)][q]


def button(q, i):
    """Panel point at the centre of the app's button i (R0..R3, BENCH) under turn q."""
    w, h = size_for(q)
    x = (w - (5 * 100 + 4 * 10)) // 2 + i * 110 + 50
    y = h // 2
    return to_panel(q, x, y)


class Log:
    """New lines of the app's log since the previous call (GET /api/logs is the whole ring)."""

    def __init__(self, host):
        self.host = host
        self.seen = set(self.lines())

    def lines(self):
        return [l for l in req(self.host, "/api/logs", timeout=20).splitlines() if "app:" + APP in l]

    def wait(self, pattern, timeout):
        rx = re.compile(pattern)
        end = time.time() + timeout
        while time.time() < end:
            for l in self.lines():
                if l in self.seen:
                    continue
                self.seen.add(l)
                m = rx.search(l)
                if m:
                    return m
            time.sleep(0.4)
        return None


def tap(host, p):
    req(host, "/api/ui/tap?x=%d&y=%d" % p)


def nearest(rgb):
    best, bd = None, 1 << 30
    for name, (_, c) in CORNERS.items():
        d = sum((a - b) ** 2 for a, b in zip(rgb, c))
        if d < bd:
            best, bd = name, d
    return best


def check_picture(host, q, fails):
    from PIL import Image  # noqa: WPS433 (only this check needs Pillow)
    img = Image.open(io.BytesIO(req(host, "/api/screen", timeout=30, raw=True))).convert("RGB")
    if img.size != (PW, PH):
        fails.append("q%d: screenshot %dx%d" % (q, img.size[0], img.size[1]))
        return
    w, h = size_for(q)
    for name, ((cx, cy), _) in CORNERS.items():
        cx, cy = cx % w, cy % h
        px, py = to_panel(q, cx, cy)
        got = nearest(img.getpixel((px, py)))
        if got != name:
            fails.append("q%d: %s corner (canvas %d,%d) shows %s at panel %d,%d" % (q, name, cx, cy, got, px, py))


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--host", default="")
    ap.add_argument("--push", action="store_true", help="upload manifest.json + app.wasm again")
    a = ap.parse_args()
    host = a.host or nsh.pick_host()
    try:
        info = get_json(host, "/api/info")
    except OSError as e:
        print("board unreachable: %s" % e)
        return 2
    print("board %s, firmware %s, ABI %s" % (host, info.get("version"), info.get("abi")))
    if int(info.get("abi", 0)) < 19:
        print("firmware ABI < 19: no nv.gfx_set_rotation")
        return 2
    apps = get_json(host, "/api/ui/apps").get("apps", [])
    registered = any(x.get("id") == APP for x in apps)
    if a.push or not registered:
        for f in ("manifest.json", "app.wasm"):
            with open(os.path.join(APP_DIR, f), "rb") as fh:
                req(host, "/api/fs/write?path=/apps/%s/%s" % (APP, f), method="POST", data=fh.read(), timeout=30)
        print("pushed %s" % APP)
        if not registered:
            print("new app: reboot the board (boot scan registers it), then run this again")
            return 2

    fails, bench = [], []
    log = Log(host)
    req(host, "/api/ui/open?id=" + APP, timeout=30)
    if not log.wait(r"rot 1 -> 1 size 600 1024", 15):   # the app starts turned once
        fails.append("start: no 'rot 1 -> 1 size 600 1024'")
    time.sleep(1.0)
    cur = 1
    for q in (1, 0, 2, 3, 1):
        if q != cur:
            tap(host, button(cur, q))
            w, h = size_for(q)
            if not log.wait(r"rot %d -> %d size %d %d" % (q, q, w, h), 8):
                fails.append("q%d: button R%d (panel %s under q%d) did not turn the canvas" % (q, q, button(cur, q), cur))
                continue
            cur = q
        time.sleep(0.8)
        check_picture(host, q, fails)
        for p in TAPS:
            tap(host, p)
            want = to_canvas(q, *p)
            m = log.wait(r"touch (\d+) (\d+) rot %d" % q, 6)
            if not m:
                fails.append("q%d: tap %s not seen" % (q, p))
            elif (int(m.group(1)), int(m.group(2))) != want:
                fails.append("q%d: tap %s -> canvas %s,%s, want %s" % (q, p, m.group(1), m.group(2), want))
            time.sleep(0.3)
        get_json(host, "/api/display")
        tap(host, button(q, 4))
        m = log.wait(r"bench rot %d frames (\d+) total_ms (\d+) us_per_frame (\d+)" % q, 60)
        d = get_json(host, "/api/display")
        if not m:
            fails.append("q%d: no bench result" % q)
        else:
            bench.append((q, int(m.group(3)) / 1000.0, d.get("blit_us_avg", 0) / 1000.0, d.get("blit_us_last", 0) / 1000.0))
        time.sleep(0.5)
    req(host, "/api/ui/home")

    print("\nturn  app ms/frame  blit ms avg  blit ms last")
    for q, ms, avg, last in bench:
        print("q%d    %8.1f    %9.1f    %10.1f" % (q, ms, avg, last))
    for f in fails:
        print("FAIL " + f)
    print("rotation: %s" % ("FAIL" if fails else "PASS"))
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
