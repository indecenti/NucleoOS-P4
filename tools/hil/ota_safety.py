#!/usr/bin/env python3
"""Hardware-in-the-loop regression test of the update safety net (docs/OTA.md), driven over Wi-Fi.

Every scenario ends with the board running a known version and is checked against /api/info,
`update status` and the boot log (/api/logs) - no serial cable, no screenshots:

  install   the candidate image (--bin, signed with the release key: it must embed it) is installed
            from the card; recovery saves a safety copy; the new version passes its probation
  retry     `update drill rearm` puts it back on probation, then a restart DURING probation: recovery
            retries it (not its fault) and it confirms itself again
  rescue    `update rescue`: recovery restores the safety copy from flash
  boot      `update drill boot`: dies 8 s into probation -> rolled back
  ui        `update drill ui`:   UI stops answering on probation -> rolled back after 60 s
  net       `update drill net`:  update server never reached on probation -> rolled back after 10 min
  late      `update drill late`: crashes 120 s after every boot -> safe mode at 3, rescue at 5
After each fault scenario the candidate is installed again, so the next one starts from it.

  python tools/hil/ota_safety.py --bin build/nucleos-anima.bin               every scenario (~1 h)
  python tools/hil/ota_safety.py --bin build/nucleos-anima.bin install rescue boot

Before a release: run it on a board whose safety copy holds the previous release. Exit 0 = all
passed. Python 3.8+, standard library + cryptography (tools/ota_sign.py).
"""
import argparse
import json
import os
import sys
import time
import urllib.error
import urllib.parse
import urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, ".."))
import ota_sign  # noqa: E402
from nvtoken import auth_headers  # noqa: E402

ALL = ["install", "retry", "rescue", "boot", "ui", "net", "late"]


class Board:
    def __init__(self, host):
        self.base = "http://" + host
        self.hdr = auth_headers()

    def req(self, method, endpoint, body=None, timeout=15, **q):
        url = self.base + endpoint + ("?" + urllib.parse.urlencode(q) if q else "")
        data = body if isinstance(body, (bytes, type(None))) else body.encode()
        if method == "POST" and data is None:
            data = b""
        r = urllib.request.Request(url, data=data, method=method, headers=self.hdr)
        with urllib.request.urlopen(r, timeout=timeout) as resp:
            return resp.read()

    def info(self):
        try:
            return json.loads(self.req("GET", "/api/info", timeout=4))
        except (urllib.error.URLError, OSError, ValueError):
            return None

    def logs(self):
        return self.req("GET", "/api/logs", timeout=10).decode("utf-8", "replace")

    def sh(self, line, wait_ms=60000):
        """One shell command (POST /api/term/run, then /api/term/out in short waits: the board's web
        server has one task); returns its output once done, or what came within wait_ms."""
        poll = 5000
        r = json.loads(self.req("POST", "/api/term/run", line, timeout=poll / 1000 + 15, wait_ms=poll))
        out = r.get("out", "")
        deadline = time.time() + wait_ms / 1000
        while not r.get("done") and time.time() < deadline:
            r = json.loads(self.req("GET", "/api/term/out", timeout=poll / 1000 + 15, since=r["seq"], wait_ms=poll))
            out += r.get("out", "")
        return out

    def put(self, dest, data):
        self.req("POST", "/api/fs/write", data, timeout=300, path=dest)

    def reboot(self):
        try:
            self.req("POST", "/api/reboot", timeout=5)
        except (urllib.error.URLError, OSError):
            pass

    def home(self):
        try:
            self.req("GET", "/api/ui/home", timeout=5)
        except (urllib.error.URLError, OSError):
            pass


def log(msg):
    print("%s %s" % (time.strftime("%H:%M:%S"), msg), flush=True)


class Fail(Exception):
    pass


def wait_until(what, check, timeout_s, step=5):
    t0 = time.time()
    while time.time() - t0 < timeout_s:
        v = check()
        if v:
            return v
        time.sleep(step)
    raise Fail("timed out after %d s waiting for %s" % (timeout_s, what))


def wait_down_up(b, timeout_s=600):
    """The board went away (restart) and answers again; returns its /api/info."""
    wait_until("the board to restart", lambda: b.info() is None, 120, 2)
    return wait_until("the board to come back", b.info, timeout_s, 5)


def wait_verdict(b, timeout_s=900):
    """Probation verdict of the running image (from the boot log)."""
    def v():
        lg = b.logs()
        if "confirmed after" in lg:
            return "confirmed"
        if "FAILED its probation" in lg:
            return "failed"
        return None
    return wait_until("the probation verdict", v, timeout_s, 5)


def safety(b):
    out = b.sh("update status")
    lines = {k.strip(): v.strip() for k, v in (l.split(":", 1) for l in out.splitlines() if ":" in l)}
    return out, lines


def lkg_version(b):
    out, _ = safety(b)
    for part in out.replace("\n", ",").split(","):
        part = part.strip()
        if part.startswith("safety: safety copy ") or part.startswith("safety copy "):
            return part.split("safety copy ", 1)[1].split(" ")[0]
    return None


def recovery_result(b):
    for line in b.logs().splitlines():
        if "ota: recovery:" in line:
            return line.split("ota: recovery:", 1)[1].strip()
    return ""


class Run:
    def __init__(self, b, bin_path):
        self.b = b
        self.bin = bin_path
        self.data = open(bin_path, "rb").read() if bin_path else None
        self.ver = ota_sign.image_version(self.data) if self.data else None

    # -- helpers ---------------------------------------------------------------------------------
    def stage_candidate(self):
        if not self.data:
            raise Fail("--bin is needed to install the candidate")
        m = {"version": self.ver, "url": "nucleos-anima.bin", "notes": "HIL " + self.ver}
        m.update(ota_sign.sign_fields(self.ver, self.data))   # refuses an image without the release key
        self.b.put("/nucleos-anima.bin", self.data)
        self.b.put("/nucleos-anima.json", (json.dumps(m, separators=(",", ":")) + "\n").encode())
        out = self.b.sh("update sd", 300000)
        if "ready" not in out:
            raise Fail("update sd: " + out.strip())

    def ensure_candidate(self):
        info = self.b.info()
        if info and info.get("version") == self.ver:
            return
        self.scenario_install()

    def expect_back_to(self, prev, op, timeout_s):
        """After a fault: the board ends up on `prev`, and recovery reports `op ok`."""
        info = wait_until("the previous version %s" % prev,
                          lambda: (lambda i: i if i and i.get("version") == prev else None)(self.b.info()),
                          timeout_s, 5)
        res = recovery_result(self.b)
        if not res.startswith(op + " ok"):
            raise Fail("recovery reported %r, expected '%s ok ...'" % (res, op))
        # The restored version goes through probation too (and refuses updates until it is through).
        if wait_verdict(self.b) != "confirmed":
            raise Fail("the restored version %s failed its probation" % prev)
        log("  back on %s (%s), confirmed" % (info["version"], res))

    # -- scenarios -------------------------------------------------------------------------------
    def scenario_install(self):
        self.stage_candidate()
        self.b.reboot()
        info = wait_down_up(self.b)
        if info.get("version") != self.ver:
            raise Fail("expected %s after the install, running %s" % (self.ver, info.get("version")))
        if wait_verdict(self.b) != "confirmed":
            raise Fail("candidate failed its probation")
        lkg = lkg_version(self.b)
        if not lkg or lkg == self.ver:
            raise Fail("no safety copy of another version after the install (got %r)" % lkg)
        log("  %s confirmed, safety copy %s" % (self.ver, lkg))

    def scenario_retry(self):
        self.ensure_candidate()
        out = self.b.sh("update drill rearm", 10000)   # back on probation, no fault
        if "armed" not in out:
            raise Fail("drill refused: " + out.strip())
        info = wait_down_up(self.b)
        if info.get("version") != self.ver or info.get("uptime_s", 99) > 40:
            raise Fail("could not catch the image on probation: %s" % info)
        self.b.reboot()   # a deliberate restart during probation: not the image's fault
        info = wait_down_up(self.b)
        if info.get("version") != self.ver:
            raise Fail("a restart during probation rolled back to %s (should retry)" % info.get("version"))
        if wait_verdict(self.b) != "confirmed":
            raise Fail("the retried image failed its probation")
        log("  retried and confirmed")

    def drill(self, kind, op, timeout_s):
        self.ensure_candidate()
        prev = lkg_version(self.b)
        if not prev or prev == self.ver:
            raise Fail("no safety copy of another version to fall back to")
        out = self.b.sh("update drill " + kind, 10000) if kind != "rescue" else self.b.sh("update rescue", 10000)
        if "armed" not in out and "asking recovery" not in out:
            raise Fail("drill refused: " + out.strip())
        self.expect_back_to(prev, op, timeout_s)

    def scenario_rescue(self):
        self.drill("rescue", "rescue", 600)

    def scenario_boot(self):
        self.drill("boot", "rollback", 600)

    def scenario_ui(self):
        self.drill("ui", "rollback", 900)

    def scenario_net(self):
        self.drill("net", "rollback", 1800)

    def scenario_late(self):
        self.drill("late", "rescue", 2400)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("scenarios", nargs="*", default=ALL, help="subset of: " + " ".join(ALL))
    ap.add_argument("--bin", default="", help="candidate image (release build)")
    ap.add_argument("--host", default=os.environ.get("NUCLEO_HOST", ""))
    a = ap.parse_args()
    host = a.host
    if not host:
        try:
            host = open(os.path.join(os.path.expanduser("~"), ".nucleo", "host"), encoding="utf-8").read().strip()
        except OSError:
            host = "nucleov2.local"
    b = Board(host)
    if not b.info():
        print("board %s unreachable" % host)
        return 2
    run = Run(b, a.bin)
    results = []
    for sc in a.scenarios:
        if sc not in ALL:
            print("unknown scenario %s" % sc)
            return 2
        log("== %s" % sc)
        t0 = time.time()
        try:
            getattr(run, "scenario_" + sc)()
            results.append((sc, "PASS", time.time() - t0, ""))
        except Exception as e:   # one broken scenario must not hide the others
            log("  FAIL: %s: %s" % (type(e).__name__, e))
            results.append((sc, "FAIL", time.time() - t0, str(e)))
        # every fault leaves the board on the previous version: put the candidate back
        if sc in ("rescue", "boot", "ui", "net", "late") and a.bin:
            try:
                run.ensure_candidate()
            except Exception as e:
                log("  could not reinstall the candidate: %s" % e)
                results.append((sc + "/reinstall", "FAIL", 0, str(e)))
                break
    b.home()
    print()
    for sc, st, dt, why in results:
        print("%-18s %s  %4.0f s  %s" % (sc, st, dt, why))
    failed = [r for r in results if r[1] != "PASS"]
    print("\n%s: %d/%d passed" % ("FAIL" if failed else "PASS", len(results) - len(failed), len(results)))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
