#!/usr/bin/env python3
"""HIL check: ABI v19 hardware JPEG decoder (nv.jpeg_info / nv.jpeg_decode) - results and speed.

Over Wi-Fi (firmware with ABI >= 19), with the test app tests/apps/jpgtest:
  1. takes comic pages from an EPUB (default: the Pepper&Carrot fixture of the Libri tests, CC BY) and
     makes variants of one with Pillow: grey, 4:4:4, progressive, CMYK, cut short
  2. uploads them to the app's data folder, runs the app: it decodes each to 496 px wide five times
     (timed) and writes /results.txt
  3. checks: baseline pages, grey and 4:4:4 decode (0); progressive and CMYK are refused as
     unsupported (-3) by both calls; the cut-short file is refused or decoded, never a crash (the board
     must still answer, same uptime)
  4. saves a screenshot of the last page shown (--shot) and goes back to Home

  python tools/hil/jpeg.py                  (new app: pushes it, then reboot and run again)
  python tools/hil/jpeg.py --push           upload manifest + app.wasm again (build first:
                                            .\\sdk\\build_app.ps1 -AppDir tests\\apps\\jpgtest -Wasi)

Exit code: 0 pass, 1 fail, 2 board unreachable / older firmware / app not registered yet.
Python 3.8+ and Pillow. Needs the board paired (tools/pair.py).
"""
import argparse
import io
import json
import os
import sys
import time
import urllib.request
import zipfile

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
import nsh  # noqa: E402  (host discovery)
from nvtoken import auth_headers  # noqa: E402

APP = "jpgtest"
APP_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "tests", "apps", APP)
DATA = "/apps/%s/data" % APP
EPUB = r"D:\bookstest\fixtures\mn-pepper-it-2.epub"


def req(host, path, method="GET", data=None, timeout=20, raw=False):
    r = urllib.request.Request("http://%s%s" % (host, path), data=data, method=method, headers=auth_headers())
    with urllib.request.urlopen(r, timeout=timeout) as resp:
        body = resp.read()
    return body if raw else body.decode("utf-8", "replace")


def pictures(epub):
    """name -> (bytes, expected decode result: 0, -3, or None = refused or decoded, never a crash)."""
    from PIL import Image
    z = zipfile.ZipFile(epub)
    jpgs = sorted(n for n in z.namelist() if n.lower().endswith(".jpg"))
    by_size = {}
    for n in jpgs:
        d = z.read(n)
        by_size.setdefault(Image.open(io.BytesIO(d)).size, d)
    page = by_size[(1000, 1383)]
    out = {"z_page_1000x1383.jpg": (page, 0)}
    if (1000, 1787) in by_size:
        out["tall_1000x1787.jpg"] = (by_size[(1000, 1787)], 0)
    if (1000, 239) in by_size:
        out["strip_1000x239.jpg"] = (by_size[(1000, 239)], 0)
    im = Image.open(io.BytesIO(page)).convert("RGB")

    def save(img, **kw):
        b = io.BytesIO()
        img.save(b, "JPEG", quality=85, **kw)
        return b.getvalue()

    out["grey.jpg"] = (save(im.convert("L")), 0)
    out["ycc444.jpg"] = (save(im, subsampling=0), 0)
    out["progressive.jpg"] = (save(im, progressive=True), -3)
    out["cmyk.jpg"] = (save(im.convert("CMYK")), -3)
    out["cut.jpg"] = (page[: len(page) * 6 // 10], None)
    out["z_page_1000x1383.jpg"] = out.pop("z_page_1000x1383.jpg")   # uploaded last: shown last (FAT order)
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--host", default="")
    ap.add_argument("--push", action="store_true", help="upload manifest.json + app.wasm again")
    ap.add_argument("--epub", default=EPUB)
    ap.add_argument("--shot", default="", help="save the final screenshot here")
    a = ap.parse_args()
    host = a.host or nsh.pick_host()
    try:
        info = json.loads(req(host, "/api/info"))
    except OSError as e:
        print("board unreachable: %s" % e)
        return 2
    print("board %s, firmware %s, ABI %s" % (host, info.get("version"), info.get("abi")))
    if int(info.get("abi", 0)) < 19:
        print("firmware ABI < 19: no nv.jpeg_decode")
        return 2
    apps = json.loads(req(host, "/api/ui/apps")).get("apps", [])
    registered = any(x.get("id") == APP for x in apps)
    if a.push or not registered:
        for f in ("manifest.json", "app.wasm"):
            with open(os.path.join(APP_DIR, f), "rb") as fh:
                req(host, "/api/fs/write?path=/apps/%s/%s" % (APP, f), method="POST", data=fh.read(), timeout=30)
        print("pushed %s" % APP)
        if not registered:
            print("new app: reboot the board (boot scan registers it), then run this again")
            return 2

    pics = pictures(a.epub)
    try:
        req(host, "/api/fs/mkdir?path=" + DATA, method="POST")
    except OSError:
        pass
    for name, (data, _) in pics.items():
        req(host, "/api/fs/write?path=%s/%s" % (DATA, name), method="POST", data=data, timeout=60)
    try:
        req(host, "/api/fs/delete?path=%s/results.txt" % DATA, method="POST")
    except OSError:
        pass
    up0 = json.loads(req(host, "/api/info")).get("uptime_s", 0)
    req(host, "/api/ui/open?id=" + APP, timeout=30)
    text, end = "", time.time() + 180
    while time.time() < end:
        try:
            text = req(host, "/api/fs/read?path=%s/results.txt" % DATA)
        except OSError:
            text = ""
        if "done" in text.split():
            break
        time.sleep(1.0)
    time.sleep(0.5)
    if a.shot:
        with open(a.shot, "wb") as f:
            f.write(req(host, "/api/screen", timeout=30, raw=True))
    req(host, "/api/ui/home")

    fails, rows = [], {}
    for line in text.splitlines():
        p = line.split()
        if len(p) >= 11 and p[1] == "info":
            rows[p[0]] = p
    print("\nfile                   info  size        decode  to        min ms  avg ms")
    for name, (_, want) in sorted(pics.items()):
        p = rows.get(name)
        if not p:
            fails.append("%s: no result" % name)
            continue
        ri, rd = int(p[2]), int(p[5])
        print("%-22s %4d  %-10s  %6d  %-8s  %6s  %6s" % (name, ri, p[3], rd, p[6], p[8], p[10]))
        if want == 0 and (ri != 0 or rd != 0):
            fails.append("%s: info %d decode %d, want 0" % (name, ri, rd))
        if want == -3 and (ri != -3 or rd != -3):
            fails.append("%s: info %d decode %d, want -3" % (name, ri, rd))
    if "done" not in text.split():
        fails.append("the app did not finish")
    info2 = json.loads(req(host, "/api/info"))
    if info2.get("uptime_s", 0) < up0:
        fails.append("the board restarted during the test")
    for f in fails:
        print("FAIL " + f)
    print("jpeg: %s" % ("FAIL" if fails else "PASS"))
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
