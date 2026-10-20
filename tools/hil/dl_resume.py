#!/usr/bin/env python3
"""HIL check of the system downloader (nv_dl, docs/ZIM_READER_PLAN.md §4f), over Wi-Fi:

  1. `dl get URL.meta4 zim/` in the Terminal (the file must not be on the card yet)
  2. after --cut MB: the Wi-Fi goes off for 45 s (`wifi off; sleep 45; wifi on`); the download must
     go on by itself once the board is back, from a verified piece (no byte lost or doubled)
  3. after --reboot MB: the board restarts; the download must resume from its record
  4. it must end "done", and the file's SHA-256 (`sha256sum` on the board) must be the one the
     .meta4 publishes (fetched here, on the PC)
  5. the downloaded file is deleted again (--keep keeps it); the board is left at Home

  python tools/hil/dl_resume.py                                   Vikidia, 15 MB (a few minutes)
  python tools/hil/dl_resume.py --meta4 https://lb.download.kiwix.org/zim/wikipedia/wikipedia_it_top_nopic_2026-10.zim.meta4 \\
         --cut 60 --reboot 250                                    the F5 exit test: ~900 MB, about an hour

Progress is read from `dl ls` once a minute (never a tight loop on the Terminal), the steps from
/api/logs. Exit code: 0 pass, 1 fail, 2 board unreachable or the file already there.
Python 3.8+, standard library only. Needs the board paired (tools/pair.py).
"""
import argparse
import os
import re
import sys
import time
import urllib.request

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
import nsh  # noqa: E402
from nvtoken import auth_headers  # noqa: E402

UA = {"User-Agent": "NucleoOS-hil (+https://indecenti.github.io/nucleoos-p4-store)"}


def http(host, path, method="GET", timeout=15):
    req = urllib.request.Request("http://%s%s" % (host, path), method=method, headers=auth_headers())
    with urllib.request.urlopen(req, timeout=timeout) as r:
        return r.read().decode("utf-8", "replace")


def up(host):
    try:
        return "version" in http(host, "/api/info", timeout=4)
    except Exception:  # noqa: BLE001
        return False


def term(board, line, wait_s=30):
    r = board.run(line, 3000)
    out = [r.get("out", "")]
    end = time.time() + wait_s
    while not r.get("done") and not r.get("reading") and time.time() < end:
        r = board.out(r["seq"], max(100, min(nsh.POLL_MS, int((end - time.time()) * 1000))))
        out.append(r.get("out", ""))
    return "".join(out)


def row(board, did):
    """(state, done MB, size MB) of download `did` from `dl ls`."""
    out = term(board, "dl ls")
    m = re.search(r"^\s*%d\s+(\w+)\s+\d+%%\s+([\d.]+)([KMG]?)/([\d.?]+)([KMG]?)" % did, out, re.M)
    if not m:
        return None, 0.0, 0.0
    k = {"": 1 / 1048576, "K": 1 / 1024, "M": 1, "G": 1024}
    size = float(m.group(4)) * k[m.group(5)] if m.group(4) != "?" else 0.0
    return m.group(1), float(m.group(2)) * k[m.group(3)], size


def wait_up(host, min_uptime=0, limit=600):
    end = time.time() + limit
    while time.time() < end:
        if up(host):
            try:
                if int(re.search(r'"uptime_s":(\d+)', http(host, "/api/info")).group(1)) >= min_uptime:
                    return True
            except Exception:  # noqa: BLE001
                pass
        time.sleep(5)
    return False


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--host", default="")
    ap.add_argument("--meta4", default="https://lb.download.kiwix.org/zim/vikidia/vikidia_it_all_nopic_2026-09.zim.meta4")
    ap.add_argument("--cut", type=float, default=4, help="MB after which the Wi-Fi goes off for 45 s (0 = never)")
    ap.add_argument("--reboot", type=float, default=9, help="MB after which the board restarts (0 = never)")
    ap.add_argument("--hours", type=float, default=3, help="give up after this long")
    ap.add_argument("--keep", action="store_true", help="leave the downloaded file on the card")
    a = ap.parse_args()
    host = a.host or nsh.pick_host()
    fails = []

    def check(ok, what):
        print(("  ok    " if ok else "  FAIL  ") + what, flush=True)
        if not ok:
            fails.append(what)

    if not up(host):
        print("board unreachable at %s" % host)
        return 2
    name = a.meta4.rsplit("/", 1)[1][:-len(".meta4")]
    meta = urllib.request.urlopen(urllib.request.Request(a.meta4, headers=UA), timeout=30).read().decode()
    want = re.search(r'type="sha-256">([0-9a-f]{64})<', meta).group(1)
    board = nsh.Board(host)
    if name in term(board, "ls zim"):
        print("%s is on the card already: remove it first" % name)
        return 2
    out = term(board, "dl get %s zim/" % a.meta4)
    m = re.search(r"queued (\d+):", out)
    check(m is not None, "dl get queued it")
    if not m:
        return 1
    did = int(m.group(1))
    t0 = time.time()
    cut_done, reboot_done = a.cut <= 0, a.reboot <= 0
    state, done, size, last_report = None, 0.0, 0.0, 0.0
    while time.time() - t0 < a.hours * 3600:
        time.sleep(60)
        if not up(host):
            continue
        state, done, size = row(board, did)
        if time.time() - last_report > 300 or state in ("done", "failed"):
            print("  ..    %3.0f min: %s %.0f/%.0f MB" % ((time.time() - t0) / 60, state, done, size), flush=True)
            last_report = time.time()
        if state in ("done", "failed"):
            break
        if not cut_done and done >= a.cut:
            cut_done = True
            before = done
            print("  ..    Wi-Fi off for 45 s at %.0f MB" % done, flush=True)
            try:
                board.run("wifi off; sleep 45; wifi on", 1000)
            except Exception:  # noqa: BLE001  (the board drops off the network: expected)
                pass
            time.sleep(30)
            check(wait_up(host, limit=300), "the board is back on the network")
            time.sleep(120)
            s2, d2, _ = row(board, did)
            check(d2 > before, "the download went on after the Wi-Fi came back (%.0f -> %.0f MB)" % (before, d2))
        elif not reboot_done and done >= a.reboot:
            reboot_done = True
            before = done
            print("  ..    restart at %.0f MB" % done, flush=True)
            http(host, "/api/reboot", method="POST")
            time.sleep(20)
            check(wait_up(host, min_uptime=90, limit=600), "the board is back after the restart")
            time.sleep(120)
            s2, d2, _ = row(board, did)
            logs = http(host, "/api/logs")
            check(re.search(r"dl: %d: resumes at \d+/" % did, logs) is not None or s2 == "done",
                  "the downloader says where it resumed (dl: %d: resumes at ...)" % did)
            check(d2 >= before - 4.5 and (d2 > before or s2 == "done"),
                  "it resumed from the record (%.0f -> %.0f MB, at most one 4 MB piece again)" % (before, d2))
    check(state == "done", "the download ended done (%s after %.0f min)" % (state, (time.time() - t0) / 60))
    if state == "done":
        out = term(board, "sha256sum zim/%s" % name, wait_s=300)
        got = (re.search(r"([0-9a-f]{64})", out) or [None, ""])[1]
        check(got == want, "the file on the card hashes to the .meta4's SHA-256")
        if not a.keep:
            term(board, "rm zim/%s; dl cancel %d" % (name, did))
    try:
        http(host, "/api/ui/home")
    except Exception:  # noqa: BLE001
        pass
    print("PASS" if not fails else "FAIL: %d check(s)" % len(fails))
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
