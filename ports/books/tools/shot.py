"""shot.py — a screenshot of the Libri app on the board, taken by the app itself (app.c shot_poll):
asks for it through /appdata/shot.req, waits for /appdata/shot.565, saves a PNG.

    python ports/books/tools/shot.py OUT.png

The board's address is in %USERPROFILE%\\.nucleo\\host and the pairing token in %USERPROFILE%\\.nucleo\\token.
The app's data folder on the SD card is /apps/libri/data (permission "fs").
"""
import os
import struct
import sys
import time
import urllib.request

from PIL import Image

home = os.path.expanduser("~")
HOST = open(os.path.join(home, ".nucleo", "host")).read().strip()
TOKEN = open(os.path.join(home, ".nucleo", "token")).read().strip()
DATA = "/apps/libri/data"


def call(method, path, body=None):
    req = urllib.request.Request("http://%s%s" % (HOST, path), data=body, method=method,
                                 headers={"Authorization": "Bearer " + TOKEN})
    with urllib.request.urlopen(req, timeout=60) as r:
        return r.read()


def main():
    out = sys.argv[1]
    try:   # an old shot must not be taken for the new one
        call("POST", "/api/fs/delete?path=%s/shot.565" % DATA)
    except Exception:   # noqa: BLE001  none there
        pass
    call("POST", "/api/fs/write?path=%s/shot.req" % DATA, b"1")
    for _ in range(60):
        time.sleep(1)
        try:
            listing = call("GET", "/api/fs/list?path=%s" % DATA).decode()
        except Exception:   # noqa: BLE001  the board's Wi-Fi drops now and then
            continue
        if '"shot.req"' not in listing and '"shot.565"' in listing:
            break
    for _ in range(3):   # a long read over a weak Wi-Fi can come back short: again
        data = call("GET", "/api/fs/read?path=%s/shot.565" % DATA)
        if data[:4] != b"BKS1":
            sys.exit("not a shot: %r" % data[:16])
        w, h = struct.unpack("<HH", data[4:8])
        if len(data) >= 8 + w * h * 2:
            break
    else:
        sys.exit("shot truncated: %d bytes" % len(data))
    px = data[8:8 + w * h * 2]
    img = Image.new("RGB", (w, h))
    pix = img.load()
    for y in range(h):
        for x in range(w):
            v = px[(y * w + x) * 2] | px[(y * w + x) * 2 + 1] << 8
            pix[x, y] = ((v >> 11) << 3, ((v >> 5) & 63) << 2, (v & 31) << 3)
    img.save(out)
    print("%s: %dx%d" % (out, w, h))


if __name__ == "__main__":
    main()
