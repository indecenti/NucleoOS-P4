#!/usr/bin/env python3
"""NucleoOS hardware-in-the-loop smoke / soak test, driven over Wi-Fi.

Uses the nv_web automation API (/api/ui/*) -- no serial cable. For every registered app it opens
the app, lets it render, grabs the panel, returns home, and then checks the OS is still healthy:

  * the app really became foreground, and home really came back          (/api/ui/state)
  * the board did not reboot  (uptime monotonic; reset_reason + stored core dump if it did)
  * the board did not freeze  (HTTP keeps answering)
  * the step logged no new E-level lines                                  (/api/logs)
  * internal SRAM, PSRAM and FreeRTOS task count come back                (leak trend per cycle)

Examples:
  python tools/hil/smoke.py                          one pass over every app + screenshots
  python tools/hil/smoke.py --cycles 20              soak: 20 passes, leak trend
  python tools/hil/smoke.py --minutes 60             soak for an hour
  python tools/hil/smoke.py --kind native --no-shots quick native-only pass
  python tools/hil/smoke.py --apps calc,notes --direct   also switch app->app without home

Exit code: 0 pass, 1 failures (or warnings with --strict), 2 board unreachable at start.
Report: <out>/report.json and <out>/index.html (screenshot contact sheet + health chart).
Python 3.8+, standard library only.
"""
import argparse
import datetime as dt
import html
import http.client
import json
import os
import re
import sys
import time
import urllib.error
import urllib.parse
import urllib.request

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
from nvtoken import auth_headers  # noqa: E402  (session token from tools/pair.py)

DEFAULT_HOST = os.environ.get("NUCLEO_HOST", "nucleov2.local")

# Used only when the firmware predates /api/ui/apps (< 1.1.106).
NATIVE_FALLBACK = ["apps", "anima", "calc", "camera", "diag", "files", "gallery", "music", "notes",
                   "recorder", "secondscreen", "settings", "sysmon", "tasks", "terminal", "video"]

# E-level lines that are expected side effects of the test itself, not defects.
BENIGN_ERRORS = [
    r"^wasm: Exception: terminated by user",   # closing a WASM app aborts its run on purpose
]

CRASH_RESETS = {"panic", "int_wdt", "task_wdt", "wdt", "brownout", "cpu_lockup", "pwr_glitch"}

LOG_RE = re.compile(r"^([EWIDV])\s+(\d+)\s+(.*)$")


class BoardDown(Exception):
    """The board stopped answering HTTP (rebooting, frozen, or off the network)."""


class Board:
    def __init__(self, host, timeout):
        self.base = "http://" + host
        self.timeout = timeout

    def get(self, path, timeout=None, raw=False):
        req = urllib.request.Request(self.base + path, headers={"Connection": "close", **auth_headers()})
        try:
            with urllib.request.urlopen(req, timeout=timeout or self.timeout) as r:
                data = r.read()
        except urllib.error.HTTPError as e:
            if e.code == 401:
                sys.exit("the board refused the request (401): pair this PC first: python tools/pair.py")
            raise
        except (urllib.error.URLError, OSError, http.client.HTTPException) as e:
            raise BoardDown(f"{path}: {e}") from None
        return data if raw else json.loads(data.decode("utf-8", "replace"))

    def text(self, path, timeout=None):
        return self.get(path, timeout=timeout, raw=True).decode("utf-8", "replace")

    def state(self):
        return self.get("/api/ui/state").get("app", "")

    def wait_state(self, want, timeout):
        end = time.monotonic() + timeout
        cur = None
        while True:
            cur = self.state()
            if cur == want:
                return True, cur
            if time.monotonic() >= end:
                return False, cur
            time.sleep(0.15)

    def sample(self):
        info = self.get("/api/info")
        heap = self.get("/api/heap")
        cpu = self.get("/api/cpu")
        return {
            "t": time.time(),
            "version": info.get("version"),
            "uptime_s": info.get("uptime_s", 0),
            "reset_reason": info.get("reset_reason"),
            "crash": info.get("crash"),
            "sram_free": heap["internal"]["free_bytes"],
            "sram_largest": heap["internal"]["largest_free_block"],
            "sram_min": heap["internal"]["min_free_bytes"],
            "psram_free": heap["psram"]["free_bytes"],
            "psram_largest": heap["psram"]["largest_free_block"],
            "tasks": cpu.get("tasks", 0),
        }

    def heap_map(self):
        """PSRAM block layout (firmware >= 1.1.115), or None."""
        try:
            return self.get("/api/heap/map?min_kb=64", timeout=15)
        except (urllib.error.HTTPError, BoardDown, ValueError):
            return None

    def wait_back(self, timeout):
        """Poll until the board answers again; returns the first sample or None."""
        end = time.monotonic() + timeout
        while time.monotonic() < end:
            try:
                return self.sample()
            except BoardDown:
                time.sleep(2)
        return None


class LogCursor:
    """Returns only the /api/logs lines emitted since the previous call (ring snapshot, ms stamps)."""

    def __init__(self, board, ignore):
        self.board = board
        self.ignore = [re.compile(p) for p in ignore]
        self.last_ts = -1
        self.seen_at_last = set()

    def reset(self):
        self.last_ts = -1
        self.seen_at_last = set()

    def fresh(self):
        try:
            raw = self.board.text("/api/logs", timeout=15)
        except urllib.error.HTTPError:
            return []
        out = []
        max_ts, at_max = self.last_ts, set(self.seen_at_last)
        for line in raw.splitlines():
            m = LOG_RE.match(line.strip())
            if not m:
                continue
            lvl, ts, msg = m.group(1), int(m.group(2)), m.group(3)
            if ts < self.last_ts or (ts == self.last_ts and line in self.seen_at_last):
                continue
            out.append({"lvl": lvl, "ts": ts, "msg": msg})
            if ts > max_ts:
                max_ts, at_max = ts, {line}
            elif ts == max_ts:
                at_max.add(line)
        self.last_ts, self.seen_at_last = max_ts, at_max
        return out

    def errors(self, lines):
        return [l for l in lines if l["lvl"] == "E" and not any(p.search(l["msg"]) for p in self.ignore)]


def kb(b):
    return b / 1024.0


def slope(ys):
    """Least-squares slope of ys over their index (units per step)."""
    n = len(ys)
    if n < 2:
        return 0.0
    mx = (n - 1) / 2.0
    my = sum(ys) / n
    den = sum((i - mx) ** 2 for i in range(n))
    return sum((i - mx) * (y - my) for i, y in enumerate(ys)) / den


class Run:
    def __init__(self, board, args, out_dir):
        self.b = board
        self.a = args
        self.out = out_dir
        self.logs = LogCursor(board, BENIGN_ERRORS + (args.ignore or []))
        self.steps = []          # one record per app open/close
        self.transitions = []    # --direct app->app switches
        self.cycle_samples = []  # home sample at the end of every cycle
        self.timeline = []       # every health sample (chart)
        self.events = []         # reboots / freezes
        self.fatal = None
        self.prev = None

    # -- health bookkeeping -------------------------------------------------------------------
    def health(self, where):
        """Sample and detect a reboot since the previous sample. Returns (sample, reboot_note)."""
        s = self.b.sample()
        note = None
        if self.prev and s["uptime_s"] + 2 < self.prev["uptime_s"]:
            note = self.reboot_note(s, where)
        self.prev = s
        self.timeline.append({"t": s["t"], "where": where, "sram_kb": round(kb(s["sram_free"]), 1),
                              "psram_kb": round(kb(s["psram_free"])), "tasks": s["tasks"]})
        return s, note

    def reboot_note(self, s, where):
        rr = s.get("reset_reason") or "unknown (firmware < 1.1.106)"
        c = s.get("crash")
        crash = f" core dump: task '{c['task']}' @ {c['pc']}" if c and rr in CRASH_RESETS else ""
        note = f"board REBOOTED during {where}: reset_reason={rr}{crash}"
        self.events.append({"t": s["t"], "kind": "reboot", "where": where, "reset_reason": rr, "crash": c})
        self.logs.reset()
        return note

    def recover(self, where):
        """The board stopped answering: wait for it. Returns a problem string, sets fatal on freeze."""
        t0 = time.monotonic()
        s = self.b.wait_back(self.a.recover_s)
        if s is None:
            self.fatal = (f"board unresponsive for {self.a.recover_s}s during {where} "
                          "(frozen or off Wi-Fi) -- aborting")
            self.events.append({"t": time.time(), "kind": "freeze", "where": where})
            return self.fatal
        waited = time.monotonic() - t0
        if self.prev and s["uptime_s"] + 2 < self.prev["uptime_s"]:
            note = self.reboot_note(s, where)
        else:
            note = f"board stopped answering for {waited:.0f}s during {where} (no reboot)"
            self.events.append({"t": s["t"], "kind": "stall", "where": where, "seconds": round(waited)})
        self.prev = s
        time.sleep(self.a.settle)
        try:
            self.b.get("/api/ui/home")
            self.b.wait_state("", 10)
        except (BoardDown, urllib.error.HTTPError):
            pass
        return note

    def save_heap_map(self, tag):
        """Dump the PSRAM layout next to the report; returns (largest_kb, fit4m) or None."""
        m = self.b.heap_map()
        if not m:
            return None
        with open(os.path.join(self.out, f"heapmap-{tag}.json"), "w", encoding="utf-8") as f:
            json.dump(m, f)
        return m.get("largest_kb"), m.get("fit4m")

    # -- one app ------------------------------------------------------------------------------
    def step(self, cycle, app, shot):
        rec = {"cycle": cycle, "id": app["id"], "name": app.get("name", app["id"]),
               "kind": app.get("kind", "?"), "problems": [], "warnings": [], "errors": []}
        before = self.prev
        try:
            t0 = time.monotonic()
            self.b.get("/api/ui/open?id=" + urllib.parse.quote(app["id"]), timeout=20)
            ok, cur = self.b.wait_state(app["id"], self.a.switch_timeout)
            rec["open_ms"] = round((time.monotonic() - t0) * 1000)
            if not ok:
                rec["problems"].append(f"did not become foreground (ui state '{cur}')")
            elif rec["open_ms"] > self.a.slow_ms:
                rec["warnings"].append(f"slow open: {rec['open_ms']} ms")
            time.sleep(self.a.dwell)
            if shot and ok:
                jpg = self.b.get("/api/screen", timeout=25, raw=True)
                name = re.sub(r"[^A-Za-z0-9_.-]", "_", app["id"]) + ".jpg"
                with open(os.path.join(self.out, "shots", name), "wb") as f:
                    f.write(jpg)
                rec["shot"] = "shots/" + name
            t1 = time.monotonic()
            self.b.get("/api/ui/home")
            ok_home, cur = self.b.wait_state("", self.a.switch_timeout)
            rec["close_ms"] = round((time.monotonic() - t1) * 1000)
            if not ok_home:
                rec["problems"].append(f"did not return home (ui state '{cur}')")
                self.b.get("/api/ui/home")
                self.b.wait_state("", self.a.switch_timeout)
            elif rec["close_ms"] > self.a.slow_ms:
                rec["warnings"].append(f"slow close: {rec['close_ms']} ms")
            time.sleep(self.a.settle)
            after, note = self.health(f"{app['id']} (cycle {cycle})")
            if note:
                rec["problems"].append(note)
            elif before:
                rec["sram_delta_kb"] = round(kb(after["sram_free"] - before["sram_free"]), 1)
                rec["psram_delta_kb"] = round(kb(after["psram_free"] - before["psram_free"]), 1)
                rec["tasks_delta"] = after["tasks"] - before["tasks"]
            errs = self.logs.errors(self.logs.fresh())
            if errs:
                rec["errors"] = [f"E {e['ts']} {e['msg']}" for e in errs]
                rec["warnings"].append(f"{len(errs)} error log line(s)")
                self.save_heap_map(f"c{cycle}-after-{app['id']}")
        except BoardDown as e:
            rec["problems"].append(self.recover(f"{app['id']} (cycle {cycle})") + f" [{e}]")
        except urllib.error.HTTPError as e:
            rec["problems"].append(f"HTTP {e.code} on {e.url}")
        rec["status"] = "FAIL" if rec["problems"] else ("WARN" if rec["warnings"] else "PASS")
        self.steps.append(rec)
        return rec

    def direct(self, apps):
        """App -> app switches without going home (the path a real user takes from Recents)."""
        seq = apps + apps[:1]
        try:
            self.b.get("/api/ui/home")
            self.b.wait_state("", self.a.switch_timeout)
            self.b.get("/api/ui/open?id=" + urllib.parse.quote(seq[0]["id"]), timeout=20)
            self.b.wait_state(seq[0]["id"], self.a.switch_timeout)
        except (BoardDown, urllib.error.HTTPError):
            return
        for a, b in zip(seq, seq[1:]):
            rec = {"from": a["id"], "to": b["id"], "problems": []}
            try:
                self.b.get("/api/ui/open?id=" + urllib.parse.quote(b["id"]), timeout=20)
                ok, cur = self.b.wait_state(b["id"], self.a.switch_timeout)
                if not ok:
                    rec["problems"].append(f"switch landed on '{cur}'")
                    self.b.get("/api/ui/home")
                    self.b.wait_state("", self.a.switch_timeout)
                    self.b.get("/api/ui/open?id=" + urllib.parse.quote(b["id"]), timeout=20)
                    self.b.wait_state(b["id"], self.a.switch_timeout)
                time.sleep(self.a.dwell / 2)
                _, note = self.health(f"switch {a['id']}->{b['id']}")
                if note:
                    rec["problems"].append(note)
                errs = self.logs.errors(self.logs.fresh())
                if errs:
                    rec["errors"] = [f"E {e['ts']} {e['msg']}" for e in errs]
            except BoardDown as e:
                rec["problems"].append(self.recover(f"switch {a['id']}->{b['id']}") + f" [{e}]")
            except urllib.error.HTTPError as e:
                rec["problems"].append(f"HTTP {e.code} on {e.url}")
            rec["status"] = "FAIL" if rec["problems"] else ("WARN" if rec.get("errors") else "PASS")
            self.transitions.append(rec)
            print(f"  switch {a['id']:>14} -> {b['id']:<14} {rec['status']}"
                  + (f"  {rec['problems'][0]}" if rec["problems"] else ""), flush=True)
            if self.fatal:
                return
        try:
            self.b.get("/api/ui/home")
            self.b.wait_state("", self.a.switch_timeout)
        except (BoardDown, urllib.error.HTTPError):
            pass

    # -- leak verdict -------------------------------------------------------------------------
    def leak_verdict(self):
        """Trend of the end-of-cycle home samples, skipping cycle 1 (first-open caches warm up)."""
        cs = [c for c in self.cycle_samples if not c.get("rebooted")]
        v = {"cycles": len(cs), "status": "n/a", "notes": []}
        if len(cs) < 3:
            v["notes"].append("need >= 3 cycles for a leak trend (use --cycles or --minutes)")
            return v
        warm = cs[1:]
        sram = [kb(c["sram_free"]) for c in warm]
        psram = [kb(c["psram_free"]) for c in warm]
        tasks = [c["tasks"] for c in warm]
        v.update({
            "sram_kb_per_cycle": round(slope(sram), 2),
            "psram_kb_per_cycle": round(slope(psram), 1),
            "tasks_per_cycle": round(slope(tasks), 2),
            "sram_first_last_kb": [round(sram[0], 1), round(sram[-1], 1)],
            "tasks_first_last": [tasks[0], tasks[-1]],
        })
        status = "PASS"
        if len(warm) >= 2 and v["sram_kb_per_cycle"] < -self.a.leak_kb:
            status = "FAIL" if len(warm) >= 4 else "WARN"
            v["notes"].append(f"internal SRAM shrinking {-v['sram_kb_per_cycle']} KB/cycle")
        if tasks[-1] > tasks[0] and v["tasks_per_cycle"] > 0.25:
            status = "FAIL" if len(warm) >= 4 else "WARN"
            v["notes"].append(f"FreeRTOS tasks growing {tasks[0]} -> {tasks[-1]}")
        if v["psram_kb_per_cycle"] < -self.a.psram_leak_kb:
            status = status if status == "FAIL" else "WARN"
            v["notes"].append(f"PSRAM shrinking {-v['psram_kb_per_cycle']} KB/cycle")
        v["status"] = status
        return v


# ------------------------------------------------------------------------------ reporting

def spark_svg(timeline, key, color, w=920, h=120):
    ys = [p[key] for p in timeline]
    if len(ys) < 2:
        return ""
    lo, hi = min(ys), max(ys)
    span = (hi - lo) or 1
    pts = " ".join(f"{i * (w - 20) / (len(ys) - 1) + 10:.1f},{h - 20 - (y - lo) * (h - 40) / span:.1f}"
                   for i, y in enumerate(ys))
    return (f'<svg viewBox="0 0 {w} {h}" class="spark" role="img" aria-label="{key}">'
            f'<polyline fill="none" stroke="{color}" stroke-width="2" points="{pts}"/>'
            f'<text x="10" y="14">{hi:g}</text><text x="10" y="{h - 4}">{lo:g}</text></svg>')


def write_html(path, rep):
    esc = html.escape
    rows = []
    for s in rep["steps"]:
        msgs = "<br>".join(esc(m) for m in s["problems"] + s["warnings"] + s["errors"][:6])
        img = f'<a href="{esc(s["shot"])}"><img src="{esc(s["shot"])}" loading="lazy"></a>' if s.get("shot") else ""
        rows.append(
            f'<tr class="{s["status"].lower()}"><td>{s["cycle"]}</td><td><b>{esc(s["name"])}</b>'
            f'<br><code>{esc(s["id"])}</code> · {esc(s["kind"])}</td><td class="st">{s["status"]}</td>'
            f'<td>{s.get("open_ms", "")}</td><td>{s.get("close_ms", "")}</td>'
            f'<td>{s.get("sram_delta_kb", "")}</td><td>{s.get("tasks_delta", "")}</td>'
            f'<td class="msg">{msgs}</td><td>{img}</td></tr>')
    trans = "".join(
        f'<tr class="{t["status"].lower()}"><td><code>{esc(t["from"])}</code> → <code>{esc(t["to"])}</code></td>'
        f'<td class="st">{t["status"]}</td><td class="msg">'
        + "<br>".join(esc(m) for m in t["problems"] + t.get("errors", [])[:4]) + "</td></tr>"
        for t in rep["transitions"])
    ev = "".join(f"<li>{esc(json.dumps(e))}</li>" for e in rep["events"]) or "<li>none</li>"
    lk = rep["leak"]
    leak = esc(json.dumps({k: v for k, v in lk.items()}, ensure_ascii=False))
    tl = rep["timeline"]
    doc = f"""<!doctype html><html lang="en"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>HIL smoke report</title>
<style>
:root{{--bg:#fbfbfa;--fg:#1d1d1b;--mut:#6b6b66;--line:#e3e2de;--pass:#1f7a4d;--warn:#9a6700;--fail:#b42318;--card:#fff}}
@media (prefers-color-scheme:dark){{:root{{--bg:#151514;--fg:#ecebe7;--mut:#a3a29c;--line:#2e2d2a;--pass:#4cc38a;--warn:#e3b341;--fail:#ff7b72;--card:#1d1d1b}}}}
body{{margin:0;padding:24px 16px;background:var(--bg);color:var(--fg);font:14px/1.45 system-ui,sans-serif}}
main{{max-width:1180px;margin:0 auto}} h1{{font-size:22px;margin:0 0 4px}} .mut{{color:var(--mut)}}
.kpis{{display:flex;flex-wrap:wrap;gap:12px;margin:16px 0}} .kpi{{background:var(--card);border:1px solid var(--line);border-radius:8px;padding:10px 14px}}
.kpi b{{display:block;font-size:20px}} .verdict{{font-weight:700}} .PASS{{color:var(--pass)}} .WARN{{color:var(--warn)}} .FAIL{{color:var(--fail)}}
.wrap{{overflow-x:auto}} table{{border-collapse:collapse;width:100%;background:var(--card)}}
th,td{{border-bottom:1px solid var(--line);padding:6px 8px;text-align:left;vertical-align:top}}
th{{font-size:12px;color:var(--mut);font-weight:600}} td.msg{{font-size:12px;max-width:360px;word-break:break-word}}
tr.pass td.st{{color:var(--pass)}} tr.warn td.st{{color:var(--warn)}} tr.fail td.st{{color:var(--fail);font-weight:700}}
img{{width:170px;border-radius:4px;border:1px solid var(--line)}} code{{font-size:12px}}
.spark{{width:100%;height:120px;background:var(--card);border:1px solid var(--line);border-radius:8px}} .spark text{{font-size:11px;fill:var(--mut)}}
h2{{font-size:16px;margin:24px 0 8px}}
</style></head><body><main>
<h1>HIL smoke report · <span class="verdict {rep['verdict']}">{rep['verdict']}</span></h1>
<div class="mut">{esc(rep['host'])} · firmware {esc(str(rep['version']))} · {esc(rep['started'])} · {rep['duration_s']} s · {rep['cycles_done']} cycle(s)</div>
<div class="kpis">
<div class="kpi"><span class="mut">steps</span><b>{len(rep['steps'])}</b></div>
<div class="kpi"><span class="mut">fail</span><b class="FAIL">{rep['counts']['FAIL']}</b></div>
<div class="kpi"><span class="mut">warn</span><b class="WARN">{rep['counts']['WARN']}</b></div>
<div class="kpi"><span class="mut">reboots</span><b>{sum(1 for e in rep['events'] if e['kind'] == 'reboot')}</b></div>
<div class="kpi"><span class="mut">SRAM low-water</span><b>{rep['sram_min_kb']} KB</b></div>
<div class="kpi"><span class="mut">leak trend</span><b class="{lk['status'] if lk['status'] != 'n/a' else ''}">{lk['status']}</b></div>
</div>
{f'<p class="FAIL"><b>{esc(rep["fatal"])}</b></p>' if rep.get("fatal") else ''}
<h2>Internal SRAM free (KB) per sample</h2>{spark_svg(tl, 'sram_kb', 'var(--pass)')}
<h2>FreeRTOS tasks per sample</h2>{spark_svg(tl, 'tasks', 'var(--warn)')}
<h2>Leak trend</h2><code>{leak}</code>
<h2>Events</h2><ul>{ev}</ul>
<h2>Apps</h2><div class="wrap"><table><tr><th>cyc</th><th>app</th><th>status</th><th>open ms</th><th>close ms</th><th>ΔSRAM KB</th><th>Δtasks</th><th>notes</th><th>screen</th></tr>
{''.join(rows)}</table></div>
{f'<h2>Direct switches</h2><div class="wrap"><table><tr><th>switch</th><th>status</th><th>notes</th></tr>{trans}</table></div>' if trans else ''}
</main></body></html>"""
    with open(path, "w", encoding="utf-8") as f:
        f.write(doc)


# ------------------------------------------------------------------------------ main

def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--host", default=os.environ.get("NUCLEO_HOST", DEFAULT_HOST))
    ap.add_argument("--cycles", type=int, default=1, help="passes over the app list (default 1)")
    ap.add_argument("--minutes", type=float, default=0, help="soak: keep cycling for N minutes")
    ap.add_argument("--apps", help="comma list of app ids (default: all registered)")
    ap.add_argument("--skip", default="", help="comma list of app ids to leave out")
    ap.add_argument("--kind", choices=["all", "native", "wasm"], default="all")
    ap.add_argument("--direct", action="store_true", help="also test app->app switches without home")
    ap.add_argument("--no-shots", action="store_true", help="skip screenshots")
    ap.add_argument("--shots-every-cycle", action="store_true", help="screenshot every cycle, not only the first")
    ap.add_argument("--dwell", type=float, default=2.5, help="seconds an app stays open (default 2.5)")
    ap.add_argument("--settle", type=float, default=1.0, help="seconds at home before sampling (default 1)")
    ap.add_argument("--switch-timeout", type=float, default=8.0, help="max seconds for open/home to land")
    ap.add_argument("--slow-ms", type=int, default=3000, help="warn when open/close takes longer")
    ap.add_argument("--leak-kb", type=float, default=1.0, help="SRAM leak threshold, KB per cycle")
    ap.add_argument("--psram-leak-kb", type=float, default=256.0, help="PSRAM warn threshold, KB per cycle")
    ap.add_argument("--recover-s", type=int, default=120, help="wait this long for a vanished board")
    ap.add_argument("--timeout", type=float, default=10.0, help="HTTP timeout, seconds")
    ap.add_argument("--ignore", action="append", help="extra regex of benign E-log messages (repeatable)")
    ap.add_argument("--strict", action="store_true", help="warnings also fail the run")
    ap.add_argument("--out", help="report dir (default _scratch/hil/<timestamp>)")
    args = ap.parse_args()

    root = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
    stamp = dt.datetime.now().strftime("%Y%m%d-%H%M%S")
    out = os.path.abspath(args.out or os.path.join(root, "_scratch", "hil", stamp))
    os.makedirs(os.path.join(out, "shots"), exist_ok=True)

    b = Board(args.host, args.timeout)
    try:
        b.get("/api/ui/home")
        b.wait_state("", args.switch_timeout)
    except (BoardDown, urllib.error.HTTPError) as e:
        print(f"board {args.host} unreachable: {e}", file=sys.stderr)
        return 2

    run = Run(b, args, out)
    time.sleep(args.settle)
    first, _ = run.health("start")
    run.logs.fresh()   # everything before the run is history, not a finding

    try:
        apps = b.get("/api/ui/apps")["apps"]
    except urllib.error.HTTPError:
        apps = [{"id": i, "name": i, "kind": "native"} for i in NATIVE_FALLBACK]
        if not args.apps:
            print("note: firmware has no /api/ui/apps -- native fallback list, WASM apps not covered")
    if args.apps:
        want = [x.strip() for x in args.apps.split(",") if x.strip()]
        by_id = {a["id"]: a for a in apps}
        apps = [by_id.get(i, {"id": i, "name": i, "kind": "?"}) for i in want]
    skip = {x.strip() for x in args.skip.split(",") if x.strip()}
    apps = [a for a in apps if a["id"] not in skip and (args.kind == "all" or a.get("kind") == args.kind)]
    if not apps:
        print("no apps to test", file=sys.stderr)
        return 2

    print(f"HIL smoke on {args.host} fw {first['version']} reset_reason={first.get('reset_reason')} "
          f"uptime {first['uptime_s']}s  SRAM {kb(first['sram_free']):.0f} KB  tasks {first['tasks']}  "
          f"apps {len(apps)}  -> {out}", flush=True)

    started = time.time()
    deadline = started + args.minutes * 60 if args.minutes else None
    cycle = 0
    try:
        while not run.fatal:
            cycle += 1
            if deadline is None and cycle > args.cycles:
                break
            if deadline is not None and time.time() >= deadline and cycle > 1:
                break
            reboots_before = sum(1 for e in run.events if e["kind"] == "reboot")
            shots = not args.no_shots and (cycle == 1 or args.shots_every_cycle)
            for app in apps:
                r = run.step(cycle, app, shots)
                extra = r["problems"][:1] or r["warnings"][:1]
                print(f"[c{cycle}] {app['id']:<16} {r['status']:<4} open {r.get('open_ms', '-'):>5} ms  "
                      f"close {r.get('close_ms', '-'):>5} ms  dSRAM {r.get('sram_delta_kb', '-'):>6} KB  "
                      f"tasks {run.prev['tasks'] if run.prev else '-'}"
                      + (f"  | {extra[0]}" if extra else ""), flush=True)
                if run.fatal:
                    break
            if run.fatal:
                break
            end = dict(run.prev)
            end["cycle"] = cycle
            end["rebooted"] = sum(1 for e in run.events if e["kind"] == "reboot") > reboots_before
            hm = run.save_heap_map(f"c{cycle}-end")
            if hm:
                end["psram_largest_exact_kb"], end["fit4m"] = hm
            run.cycle_samples.append(end)
            print(f"-- cycle {cycle} done: SRAM {kb(end['sram_free']):.1f} KB  PSRAM {kb(end['psram_free']):.0f} KB  "
                  f"tasks {end['tasks']}  uptime {end['uptime_s']}s"
                  + (f"  PSRAM largest {hm[0]} KB, camera frames that fit {hm[1]}" if hm else ""), flush=True)
        if args.direct and not run.fatal:
            print("direct app->app switches:", flush=True)
            run.direct(apps)
    except KeyboardInterrupt:
        print("interrupted -- writing partial report", flush=True)

    counts = {"PASS": 0, "WARN": 0, "FAIL": 0}
    for s in run.steps + run.transitions:
        counts[s["status"]] += 1
    leak = run.leak_verdict()
    verdict = "FAIL" if (counts["FAIL"] or run.fatal or leak["status"] == "FAIL") else \
              ("WARN" if (counts["WARN"] or leak["status"] == "WARN") else "PASS")
    rep = {
        "host": args.host, "version": first["version"], "started": dt.datetime.fromtimestamp(started).isoformat(timespec="seconds"),
        "duration_s": round(time.time() - started), "cycles_done": len(run.cycle_samples),
        "verdict": verdict, "counts": counts, "fatal": run.fatal, "leak": leak, "events": run.events,
        "sram_min_kb": round(kb(min(s["sram_min"] for s in [first] + run.cycle_samples)), 1),
        "baseline": first, "cycle_samples": run.cycle_samples, "steps": run.steps,
        "transitions": run.transitions, "timeline": run.timeline,
    }
    with open(os.path.join(out, "report.json"), "w", encoding="utf-8") as f:
        json.dump(rep, f, indent=1)
    write_html(os.path.join(out, "index.html"), rep)

    print(f"\nVERDICT {verdict}  pass {counts['PASS']}  warn {counts['WARN']}  fail {counts['FAIL']}  "
          f"reboots {sum(1 for e in run.events if e['kind'] == 'reboot')}  leak {leak['status']}  "
          f"SRAM low-water {rep['sram_min_kb']} KB")
    for s in run.steps + run.transitions:
        if s["status"] != "PASS":
            who = s.get("id") or f"{s['from']}->{s['to']}"
            for m in s["problems"] + s.get("warnings", []) + s.get("errors", [])[:3]:
                print(f"  {s['status']} {who}: {m}")
    for n in leak["notes"]:
        print(f"  leak: {n}")
    if run.fatal:
        print(f"  FATAL: {run.fatal}")
    print(f"report: {os.path.join(out, 'index.html')}")
    if verdict == "FAIL" or (args.strict and verdict == "WARN"):
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
