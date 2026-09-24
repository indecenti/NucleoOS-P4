#!/usr/bin/env python3
"""End-to-end tests of the Second Screen transports on a real board (over Wi-Fi).

  python board_test.py --board 192.168.0.128 cast            # NucleoCast test picture + pixel check
  python board_test.py --board 192.168.0.128 vnc             # every VNC encoding via reverse connect
  python board_test.py --board 192.168.0.128 vnc --enc zrle --size 1920x1080
  python board_test.py --board 192.168.0.128 bench           # NucleoCast throughput (full frames)

Needs the Second Screen app open on the board (the script opens it through /api/ui/open).
Pixel checks compare the board's /api/screen JPEG with the picture that was sent, scaled the way
the board places it (fit + centre), and report the mean absolute error per channel.
"""
import argparse
import io
import json
import os
import subprocess
import sys
import threading
import time
import urllib.request

from PIL import Image, ImageChops, ImageStat

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
W, H = 1024, 600


def http(board, path, timeout=20, port=80):
    # The board shares one Wi-Fi link with the stream under test: retry instead of failing on a
    # request that queued behind a big update.
    for attempt in range(4):
        try:
            with urllib.request.urlopen(f"http://{board}:{port}{path}", timeout=timeout) as r:
                return r.read()
        except OSError:
            if attempt == 3:
                raise
            time.sleep(1.5)


def state(board):
    return json.loads(http(board, "/api/state", port=7070))


def screen(board):
    return Image.open(io.BytesIO(http(board, "/api/screen", timeout=20))).convert("RGB")


def placed(src):
    """Where the board puts a picture of this size (fit, never upscale, centred)."""
    w, h = src.size
    s = min(1.0, W / w, H / h)
    dw, dh = max(1, round(w * s)), max(1, round(h * s))
    canvas = Image.new("RGB", (W, H))
    canvas.paste(src.resize((dw, dh), Image.NEAREST), ((W - dw) // 2, (H - dh) // 2))
    return canvas


def compare(expected, got, label):
    diff = ImageChops.difference(expected, got)
    mae = [round(v, 1) for v in ImageStat.Stat(diff).mean]
    bad = max(mae) > 12
    print(f"  [{'FAIL' if bad else 'ok'}] {label}: mean abs error per channel {mae}")
    return not bad


def open_app(board):
    r = json.loads(http(board, "/api/ui/open?id=secondscreen"))
    time.sleep(1.5)
    return r


def wait_mode(board, want_src, secs=15):
    t0 = time.time()
    while time.time() - t0 < secs:
        try:
            st = state(board)
            if st.get("src") == want_src and st.get("mode") == 1:
                return st
        except OSError:
            pass
        time.sleep(0.5)
    return None


def wait_stable(board, quiet=1.5, secs=30):
    """Wait until the board stops presenting updates (the picture under test is complete)."""
    t0, last, since = time.time(), None, time.time()
    while time.time() - t0 < secs:
        try:
            u = state(board).get("updates")
        except OSError:
            u = None
        if u != last:
            last, since = u, time.time()
        elif time.time() - since >= quiet:
            return True
        time.sleep(0.3)
    return False


# ---------------------------------------------------------------- cast
def test_cast(a):
    from nucleocast import Cast, TestSource
    print("== NucleoCast (WebSocket, test picture)")
    open_app(a.board)
    src = TestSource()
    c = Cast(a.board, 7070, src, "85", None, verbose=a.verbose)
    th = threading.Thread(target=c.run, daemon=True)
    th.start()
    t0 = time.time()
    while c.state not in ("ready", "live") and time.time() - t0 < 30:
        if c.state == "pending":
            print("  board asks for approval: tap 'Always allow' on its screen (waiting 30 s)")
            time.sleep(3)
        time.sleep(0.3)
    st = wait_mode(a.board, "Cast")
    print("  state:", st)
    ok = st is not None
    time.sleep(2)
    c.alive = False
    time.sleep(0.5)
    got = screen(a.board)
    got.save(os.path.join(a.out, "cast_board.png"))
    exp, _ = src.frame()
    # the clock text changes every second: compare the static top 74 % only
    box = (0, 0, W, int(H * 0.74))
    ok &= compare(exp.crop(box), got.crop(box), "cast picture")
    c.ws.close()
    return ok


def bench_cast(a):
    from nucleocast import Cast, TestSource
    print("== NucleoCast throughput (full frames, 15 s)")
    open_app(a.board)
    c = Cast(a.board, 7070, TestSource(), "70", None, bench=15, verbose=True)
    c.run()
    r = sorted(c.stats["rtt"])
    print(f"  acks={c.stats['acks']} shown={c.stats['shown']} rtt p50={r[len(r) // 2] if r else 0:.0f} ms")
    return c.stats["acks"] > 0


# ---------------------------------------------------------------- vnc
ENCS = ["raw", "hextile", "zlib", "zrle", "tight-fill", "tight-basic", "tight-palette", "tight-gradient",
        "tight-jpeg", "copyrect"]


def wait_idle(board, secs=20):
    t0 = time.time()
    while time.time() - t0 < secs:
        try:
            if state(board).get("mode") == 0:
                return True
        except OSError:
            pass
        time.sleep(0.5)
    return False


def test_vnc_one(a, enc, size):
    exp_path = os.path.join(a.out, f"vnc_{enc}_{size}.expected.png")
    if os.path.exists(exp_path):
        os.remove(exp_path)
    if not wait_idle(a.board):
        print(f"  [FAIL] {enc} {size}: board still busy with a previous session")
        return False
    args = [sys.executable, os.path.join(HERE, "rfb_testserver.py"), "--reverse", a.board, "--enc", enc,
            "--size", size, "--expect", exp_path]
    if enc == "copyrect":   # scroll 12 times (CopyRect + strip each), then hold still for the check
        args += ["--animate", "--interval", "0.1", "--frames", "12"]
    if a.password:
        args += ["--password", a.password]
    p = subprocess.Popen(args, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    try:
        return run_case(a, p, enc, size, exp_path)
    finally:
        if p.poll() is None:
            p.kill()


def run_case(a, p, enc, size, exp_path):
    st = wait_mode(a.board, "VNC", 20)
    ok = st is not None
    if ok:
        # the server writes the expected picture once the update is on the wire; then let the
        # board finish drawing it
        t0 = time.time()
        while not os.path.exists(exp_path) and time.time() - t0 < 90:
            time.sleep(0.3)
        if not os.path.exists(exp_path):
            print(f"  [FAIL] {enc} {size}: the server never finished sending the picture")
            return False
        time.sleep(0.5)   # PNG fully written
        wait_stable(a.board)
    if ok:
        got = screen(a.board)
        got.save(os.path.join(a.out, f"vnc_{enc}_{size}.board.png"))
        exp = placed(Image.open(exp_path).convert("RGB"))
        tol = "jpeg" in enc
        good = compare(exp, got, f"{enc} {size}")
        ok = good or tol and max(ImageStat.Stat(ImageChops.difference(exp, got)).mean) < 20
    else:
        print(f"  [FAIL] {enc} {size}: board never went LIVE")
    p.terminate()
    try:
        out = p.communicate(timeout=5)[0]
    except subprocess.TimeoutExpired:
        p.kill()
        out = p.communicate()[0]
    if a.verbose or not ok:
        print("    server log:", " | ".join(out.strip().splitlines()[-6:]))
    time.sleep(2.5)   # session teardown on the board
    return ok


def test_vnc(a):
    print("== VNC viewer (reverse connections, every encoding)")
    open_app(a.board)
    encs = [a.enc] if a.enc else ENCS
    sizes = [a.size] if a.size else ["1024x600", "1920x1080"]
    results = {}
    for size in sizes:
        for enc in encs:
            results[(enc, size)] = test_vnc_one(a, enc, size)
    passed = sum(results.values())
    print(f"VNC: {passed}/{len(results)} passed")
    return passed == len(results)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("what", choices=["cast", "vnc", "bench", "all"])
    ap.add_argument("--board", default="192.168.0.128")
    ap.add_argument("--enc", default="")
    ap.add_argument("--size", default="")
    ap.add_argument("--password", default="")
    ap.add_argument("--out", default=".")
    ap.add_argument("-v", "--verbose", action="store_true")
    a = ap.parse_args()
    os.makedirs(a.out, exist_ok=True)
    print("board:", http(a.board, "/api/info").decode())
    ok = True
    if a.what in ("cast", "all"):
        ok &= test_cast(a)
    if a.what in ("vnc", "all"):
        ok &= test_vnc(a)
    if a.what == "bench":
        ok &= bench_cast(a)
    print("RESULT:", "PASS" if ok else "FAIL")
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
