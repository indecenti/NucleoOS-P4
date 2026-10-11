#!/usr/bin/env python3
"""HIL check: a store install reaches the Home screen at once, an uninstall leaves it at once.

Over Wi-Fi, no restart in between:
  1. the test app is not installed (the tool refuses to touch an app that is: it never deletes
     something the user installed)
  2. `store install <app>` in the Terminal; when it returns, the app must already be registered
     (/api/ui/apps): the store's app hook registers it before the job turns READY
  3. within a few seconds the launcher shows its tile (/api/ui/apps "home" >= 0)
  4. `store remove <app>`: registry entry and tile gone
  5. back to Home; the board did not reboot (uptime keeps growing)

  python tools/hil/store_tile.py                 default test app (chip8)
  python tools/hil/store_tile.py --app zork      another one (must not be installed)

Exit code: 0 pass, 1 fail, 2 board unreachable or the test app already installed.
Python 3.8+, standard library only. Needs the board paired (tools/pair.py).
"""
import argparse
import json
import os
import sys
import time
import urllib.request

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
import nsh  # noqa: E402  (Terminal text API + host discovery)
from nvtoken import auth_headers  # noqa: E402


def get(host, path, timeout=10):
    req = urllib.request.Request("http://%s%s" % (host, path), headers=auth_headers())
    with urllib.request.urlopen(req, timeout=timeout) as r:
        return json.loads(r.read().decode("utf-8", "replace") or "{}")


def entry(host, app):
    """The app's /api/ui/apps row, or None."""
    for a in get(host, "/api/ui/apps").get("apps", []):
        if a.get("id") == app:
            return a
    return None


def term(board, line, wait_s):
    r = nsh.follow(board, board.run(line, 3000), wait_s)
    return nsh.status_of(r)


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--host", default="")
    ap.add_argument("--app", default="chip8", help="store app to install and remove (default chip8)")
    ap.add_argument("--wait", type=int, default=300, help="max seconds for the install")
    a = ap.parse_args()

    host = a.host or nsh.pick_host()
    fails = []

    def check(ok, what):
        print(("  ok    " if ok else "  FAIL  ") + what)
        if not ok:
            fails.append(what)

    try:
        info0 = get(host, "/api/info")
        if entry(host, a.app):
            print("%s is installed on the board: pick an app that is not (--app ID)" % a.app)
            return 2
    except Exception as e:  # noqa: BLE001  (anything here means: no board)
        print("board unreachable at %s: %s" % (host, e))
        return 2
    print("board %s, firmware %s, test app %s" % (host, info0.get("version"), a.app))
    board = nsh.Board(host)

    try:
        rc = term(board, "store install " + a.app, a.wait)
        check(rc == 0, "store install %s exits 0 (got %s)" % (a.app, rc))
        e = entry(host, a.app)
        check(e is not None and e.get("kind") == "wasm", "registered as soon as the install returns")
        page, t0 = -1, time.time()
        while e is not None and time.time() - t0 < 5:
            page = entry(host, a.app).get("home", -1)
            if page >= 0:
                break
            time.sleep(0.25)
        check(page >= 0, "Home tile on page %d within %.1f s" % (page, time.time() - t0))

        rc = term(board, "store remove " + a.app, 60)
        check(rc == 0, "store remove %s exits 0 (got %s)" % (a.app, rc))
        check(entry(host, a.app) is None, "registry entry and tile gone after the remove")
    finally:
        try:
            get(host, "/api/ui/home")
        except Exception:  # noqa: BLE001
            pass

    info1 = get(host, "/api/info")
    check(info1.get("uptime_s", 0) >= info0.get("uptime_s", 0),
          "no reboot (uptime %s -> %s s)" % (info0.get("uptime_s"), info1.get("uptime_s")))
    print("PASS" if not fails else "FAIL: %d check(s)" % len(fails))
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
