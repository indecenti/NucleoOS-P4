#!/usr/bin/env python3
"""Drive the Second Screen wizard on the board like a user: Mac (or Linux) -> VNC method ->
Connect step, type host / Mac user / password through KeyDeck (TCP 5588) with the keyboard's
Next/Go keys, and check that the board reaches the RFB test server and goes LIVE.

  python rfb_testserver.py --ard nicola:segreto --enc zrle --size 1440x900 --once &
  python ui_flow_test.py --host 192.168.0.216 --user nicola --password segreto
  python rfb_testserver.py --password secret --enc tight-jpeg --once &
  python ui_flow_test.py --host 192.168.0.216 --password secret --os linux
"""
import argparse
import json
import socket
import time
import urllib.request


def get(board, path, port=80, timeout=10):
    with urllib.request.urlopen(f"http://{board}:{port}{path}", timeout=timeout) as r:
        return r.read()


def tap(board, x, y, wait=1.2):
    get(board, f"/api/ui/tap?x={x}&y={y}")
    time.sleep(wait)


class KeyDeck:
    def __init__(self, board):
        self.s = socket.create_connection((board, 5588), timeout=5)
        self.s.sendall(b"HELLO v1 ui-test\n")
        time.sleep(0.3)

    def text(self, t):
        self.s.sendall(f"TXT {t}\n".encode())
        time.sleep(0.5)

    def key(self, k):
        self.s.sendall(f"KEY {k}\n".encode())
        time.sleep(0.6)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--board", default="192.168.0.128")
    ap.add_argument("--host", required=True)
    ap.add_argument("--user", default="")
    ap.add_argument("--password", default="")
    ap.add_argument("--os", choices=["mac", "linux"], default="mac")
    ap.add_argument("--shot", default="")
    ap.add_argument("--form-shot", default="")
    a = ap.parse_args()
    b = a.board
    get(b, "/api/ui/home"); time.sleep(1)
    get(b, "/api/ui/open?id=secondscreen"); time.sleep(1.5)
    if a.os == "mac":
        tap(b, 308, 300)          # Mac card
        tap(b, 300, 202, 1.8)     # Screen Sharing (1st method)
    else:
        tap(b, 510, 300)          # Linux card
        tap(b, 300, 295, 1.8)     # VNC (2nd method)
    tap(b, 180, 300, 1.5)         # step 3: Connect
    kd = KeyDeck(b)
    tap(b, 520, 293)              # host field (first in the form column)
    if a.form_shot:
        open(a.form_shot, "wb").write(get(b, "/api/screen", timeout=20))
    kd.text(a.host)
    kd.key("ENTER")               # Next
    if a.os == "mac":
        if a.user:
            kd.text(a.user)
        kd.key("ENTER")           # Next -> password
    if a.password:
        kd.text(a.password)
    kd.key("ENTER")               # Go -> connect
    t0 = time.time()
    st = {}
    while time.time() - t0 < 25:
        try:
            st = json.loads(get(b, "/api/state", port=7070))
            if st.get("src") == "VNC" and st.get("mode") == 1:
                break
        except OSError:
            pass
        time.sleep(0.5)
    ok = st.get("src") == "VNC" and st.get("mode") == 1
    print("state:", st)
    if a.shot:
        time.sleep(4)
        open(a.shot, "wb").write(get(b, "/api/screen", timeout=20))
    print("RESULT:", "PASS" if ok else "FAIL")


if __name__ == "__main__":
    main()
