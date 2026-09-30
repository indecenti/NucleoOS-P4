"""Pair this PC with a NucleoOS board, so the tools in this repo can use its web API.

The board puts a 6-digit code on its screen (and on the USB serial console); the session token it
hands back is saved where every tool looks for it (tools/nvtoken.py: NUCLEO_TOKEN overrides it).

  python tools/pair.py                          type the code shown on the board
  python tools/pair.py --serial COM5            read the code from the serial console (board on USB)
  python tools/pair.py --host nucleov2.local --name "build PC"
"""
import argparse
import json
import os
import re
import socket
import sys
import time
import urllib.error
import urllib.request

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from nvtoken import token_path  # noqa: E402


def http_json(url, body=None, headers=None, timeout=10):
    req = urllib.request.Request(url, data=body, method="POST" if body is not None else "GET",
                                 headers=headers or {})
    with urllib.request.urlopen(req, timeout=timeout) as r:
        return json.loads(r.read() or b"{}")


def code_from_serial(port, base):
    import serial  # pyserial: in the ESP-IDF venv and most Python installs

    s = serial.Serial()
    s.port, s.baudrate, s.timeout = port, 115200, 0.2
    s.dtr = False  # set before open(): toggling DTR/RTS resets the board
    s.rts = False
    s.open()
    try:
        s.reset_input_buffer()
        http_json(base + "/api/auth/status")  # issues the code (or re-prints the one that is out)
        buf, deadline = b"", time.time() + 15
        while time.time() < deadline:
            buf += s.read(256)
            m = re.search(rb"\[nv_auth\] pairing code (\d{6})", buf)
            if m:
                return m.group(1).decode()
    finally:
        s.close()
    sys.exit("no pairing code on the serial console within 15 s "
             "(pairing locked after wrong codes or a Cancel on the board? wait and retry)")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--host", default=os.environ.get("NUCLEO_HOST", "nucleov2.local"))
    ap.add_argument("--name", default="tools " + socket.gethostname(), help="label shown in Settings > Security")
    ap.add_argument("--serial", metavar="PORT", help="read the code from this serial port (e.g. COM5)")
    a = ap.parse_args()
    base = "http://" + a.host

    if a.serial:
        code = code_from_serial(a.serial, base)
    else:
        st = http_json(base + "/api/auth/status")
        if not st.get("required", True):
            sys.exit("this board does not require pairing (firmware without web auth)")
        code = input("Code shown on the board: ").strip().replace(" ", "")

    body = json.dumps({"pin": code, "name": a.name, "token": True}).encode()
    try:
        r = http_json(base + "/api/pair", body, {"Content-Type": "application/json"})
    except urllib.error.HTTPError as e:
        why = {401: "wrong code", 409: "the code expired", 429: "too many attempts, pairing locked"}
        sys.exit("pairing refused: " + why.get(e.code, "HTTP %d" % e.code))
    tok = r["token"]
    path = token_path()
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w", encoding="utf-8") as f:
        f.write(tok + "\n")
    http_json(base + "/api/heap", headers={"Authorization": "Bearer " + tok})  # proves it works
    print("paired as %r; token saved to %s" % (a.name, path))


if __name__ == "__main__":
    main()
