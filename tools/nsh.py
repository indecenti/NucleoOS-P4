"""Run commands in the NucleoOS Terminal over Wi-Fi and get their output as plain text.

The board types the command into its Terminal (you see it on the screen), runs it through the
shell and hands back what it printed, ANSI colours stripped. Exit status = the command's status.

  python tools/nsh.py "ls -l"                 run one command line, print its output
  python tools/nsh.py "df -h; free -h"        several commands (; && || | work as in sh)
  python tools/nsh.py -i                      interactive: a remote shell prompt (^C interrupts)
  python tools/nsh.py --wait 300 "wget URL"   wait up to 300 s (default 60) for it to finish
  python tools/nsh.py --input "print(1+1)"    type a line into the running program (lua, bc...)
  python tools/nsh.py --follow                keep printing the output of what is running
  python tools/nsh.py --interrupt             ^C

Board address: --host, else NV_BOARD_IP / NUCLEO_HOST, else nucleov2.local. Token: the one
tools/pair.py saved (%USERPROFILE%\\.nucleo\\token, or NUCLEO_TOKEN). Needs firmware with
/api/term (the Terminal text API). Exit codes: the command's, 124 = still running when --wait
ran out, 255 = could not talk to the board.
"""
import argparse
import json
import os
import sys
import time
import urllib.error
import urllib.parse
import urllib.request

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from nvtoken import auth_headers  # noqa: E402

POLL_MS = 5000   # per request: the board's web server has one task, keep each wait short


class BoardError(Exception):
    pass


class Board:
    def __init__(self, host):
        self.base = "http://" + host
        self.headers = auth_headers()

    def call(self, method, path, body=None, **query):
        q = {k: v for k, v in query.items() if v is not None}
        url = self.base + path + ("?" + urllib.parse.urlencode(q) if q else "")
        data = body.encode("utf-8") if isinstance(body, str) else body
        if method == "POST" and data is None:
            data = b""
        hdr = dict(self.headers)
        hdr["Content-Type"] = "text/plain; charset=utf-8"
        req = urllib.request.Request(url, data=data, method=method, headers=hdr)
        wait = int(query.get("wait_ms") or 0) / 1000
        try:
            with urllib.request.urlopen(req, timeout=15 + wait) as r:
                raw = r.read()
        except urllib.error.HTTPError as e:
            msg = e.read().decode("utf-8", "replace")
            try:
                msg = json.loads(msg).get("error", msg)
            except ValueError:
                pass
            if e.code == 401:
                raise BoardError("not paired with the board (401): run  python tools/pair.py --host %s"
                                 "  (add --serial COM5 to read the code over USB)" % self.base[7:])
            if e.code == 404:
                raise BoardError("this firmware has no /api/term (update the board)")
            raise BoardError("HTTP %d: %s" % (e.code, msg))
        except (urllib.error.URLError, OSError) as e:
            raise BoardError("cannot reach %s: %s" % (self.base, getattr(e, "reason", e)))
        return json.loads(raw.decode("utf-8", "replace") or "{}")

    def run(self, line, wait_ms):
        return self.call("POST", "/api/term/run", line, wait_ms=wait_ms)

    def out(self, since, wait_ms):
        return self.call("GET", "/api/term/out", since=since, wait_ms=wait_ms)

    def input(self, text, eof=False, wait_ms=3000):
        return self.call("POST", "/api/term/input", text, eof=1 if eof else None, wait_ms=wait_ms)

    def interrupt(self):
        return self.call("POST", "/api/term/interrupt")


def emit(text):
    if text:
        sys.stdout.write(text)
        sys.stdout.flush()


def follow(board, r, wait_s):
    """Print r's output, then poll until the command is done or wait_s runs out. Returns r."""
    emit(r.get("out", ""))
    if r.get("trunc"):
        sys.stderr.write("[nsh: output was cut: the board keeps the last 64 KB]\n")
    deadline = time.time() + wait_s
    while not r.get("done") and not r.get("reading") and time.time() < deadline:
        left = int((deadline - time.time()) * 1000)
        r = board.out(r["seq"], max(100, min(POLL_MS, left)))
        emit(r.get("out", ""))
    return r


def status_of(r):
    if r.get("done"):
        return r.get("status") or 0
    return 124


def repl(board, wait_s):
    """A remote prompt: lines go to the shell, or to the running program when it reads input."""
    try:
        import readline  # noqa: F401  (history / editing where available)
    except ImportError:
        pass
    st = board.out("end", 0)   # where the output is now
    seq, reading, running = st["seq"], st.get("reading"), not st.get("done")
    sys.stderr.write("nsh: connected to %s  (^C interrupts, ^D / exit leaves)\n" % board.base[7:])
    while True:
        prompt = "" if reading else "nucleo$ "
        try:
            line = input(prompt)
        except EOFError:
            if reading:
                r = board.input("", eof=True)
                emit(r.get("out", ""))
                seq, reading, running = r["seq"], r.get("reading"), not r.get("done")
                continue
            print()
            return 0
        except KeyboardInterrupt:
            print("^C")
            if running or reading:
                board.interrupt()
                r = board.out(seq, 1000)
                emit(r.get("out", ""))
                seq, reading, running = r["seq"], r.get("reading"), not r.get("done")
            continue
        try:
            if reading:
                r = board.input(line)
            else:
                if line.strip() in ("exit", "logout"):
                    return 0
                r = board.run(line, min(POLL_MS, wait_s * 1000))
            r = follow(board, r, wait_s)
        except KeyboardInterrupt:
            print("^C")
            r = board.interrupt()
            r = board.out(seq, 1000)
            emit(r.get("out", ""))
        except BoardError as e:
            sys.stderr.write("nsh: %s\n" % e)
            continue
        seq, reading, running = r["seq"], r.get("reading"), not r.get("done")
        if running and not reading:
            sys.stderr.write("[nsh: still running - Enter polls again, ^C interrupts]\n")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("command", nargs="*", help="command line to run (quote it)")
    ap.add_argument("--host", default=os.environ.get("NV_BOARD_IP") or os.environ.get("NUCLEO_HOST")
                    or "nucleov2.local")
    ap.add_argument("--wait", type=float, default=60, metavar="SECONDS",
                    help="how long to wait for the command to finish (default 60)")
    ap.add_argument("-i", "--interactive", action="store_true", help="interactive remote shell")
    ap.add_argument("--input", metavar="TEXT", help="send a line to the running program's stdin")
    ap.add_argument("--eof", action="store_true", help="with --input: then close its stdin (^D)")
    ap.add_argument("--follow", action="store_true", help="print the running command's output until it ends")
    ap.add_argument("--since", type=int, help="with --follow: output cursor to start from")
    ap.add_argument("--interrupt", action="store_true", help="send ^C")
    a = ap.parse_args()
    for s in (sys.stdout, sys.stderr):
        try:
            s.reconfigure(encoding="utf-8", errors="replace")
        except (AttributeError, ValueError):
            pass
    board = Board(a.host)
    try:
        if a.interrupt:
            r = board.interrupt()
            print("interrupted" if r.get("was_running") else "nothing was running", file=sys.stderr)
            return 0
        if a.input is not None:
            r = follow(board, board.input(a.input, a.eof), a.wait)
            return status_of(r) if r.get("done") else 0
        if a.follow:
            since = a.since
            if since is None:
                since = board.out("end", 0)["seq"]
            r = follow(board, board.out(since, POLL_MS), a.wait)
            return status_of(r)
        if a.interactive or not a.command:
            return repl(board, a.wait)
        line = " ".join(a.command)
        r = follow(board, board.run(line, int(min(POLL_MS, a.wait * 1000))), a.wait)
        if r.get("reading") and not r.get("done"):
            sys.stderr.write("[nsh: the program waits for input - send it with --input TEXT]\n")
            return 0
        if not r.get("done"):
            sys.stderr.write("[nsh: still running after %gs - nsh.py --follow to keep reading, "
                             "--interrupt to stop]\n" % a.wait)
        return status_of(r)
    except BoardError as e:
        sys.stderr.write("nsh: %s\n" % e)
        return 255
    except KeyboardInterrupt:
        try:
            board.interrupt()
        except BoardError:
            pass
        return 130


if __name__ == "__main__":
    sys.exit(main())
