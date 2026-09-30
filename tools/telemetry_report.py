#!/usr/bin/env python3
"""telemetry_report.py — the private report of the opt-in NucleoOS P4 statistics.

    python tools/telemetry_report.py [--days 30] [--out <file.html>]

Reads the daily summaries the server keeps in /var/lib/nvtele/days (server/stats/nvtele-stats.py;
counts only, never served on the web) over SSH, and writes one self-contained HTML page on this
PC, outside every repository (default %USERPROFILE%\\.nucleo\\telemetry\\report.html). Nothing is
uploaded anywhere.

SSH: NV_STATS_SSH (user@host, default opc@nucleoos.indexhub.it) and NV_STATS_KEY (the private key
file). The key never goes in this repository.
"""
import argparse
import datetime
import html
import json
import os
import subprocess
import sys
from collections import Counter

REMOTE_DIR = "/var/lib/nvtele/days"


def fetch(days):
    target = os.environ.get("NV_STATS_SSH", "opc@nucleoos.indexhub.it")
    key = os.environ.get("NV_STATS_KEY")
    if not key:
        sys.exit("set NV_STATS_KEY to the SSH private key of the stats server")
    since = (datetime.date.today() - datetime.timedelta(days=days)).isoformat()
    # One small command: print the summaries of the window as JSON lines (the files are tiny).
    cmd = (f"for f in {REMOTE_DIR}/*.json; do b=$(basename $f .json); "
           f"[ \"$b\" \\> \"{since}\" ] && cat $f && echo; done")
    out = subprocess.run(["ssh", "-o", "BatchMode=yes", "-i", key, target, cmd],
                         capture_output=True, text=True, timeout=60)
    if out.returncode != 0:
        sys.exit(out.stderr.strip() or "ssh failed")
    return [json.loads(ln) for ln in out.stdout.splitlines() if ln.strip()]


def merge(days, key):
    c = Counter()
    for d in days:
        c.update(d.get(key, {}))
    return c


def bar_table(title, counter, unit="", limit=15):
    if not counter:
        return f"<h2>{html.escape(title)}</h2><p class=dim>no data</p>"
    top = counter.most_common(limit)
    peak = max(v for _, v in top) or 1
    rows = "".join(
        f"<tr><td>{html.escape(str(k))}</td><td class=n>{v}{unit}</td>"
        f"<td class=b><span style='width:{100 * v // peak}%'></span></td></tr>" for k, v in top)
    return f"<h2>{html.escape(title)}</h2><table>{rows}</table>"


def page(days):
    days = sorted(days, key=lambda d: d["day"])
    last = days[-1] if days else {}
    months = Counter()
    for d in days:
        months[d["day"][:7]] += d.get("monthly_first", 0)
    daily = "".join(f"<tr><td>{d['day']}</td><td class=n>{d.get('beats', 0)}</td>"
                    f"<td class=n>{d.get('new', 0)}</td><td class=n>{d.get('crashes', 0)}</td></tr>"
                    for d in reversed(days))
    beats = sum(d.get("beats", 0) for d in days)
    crashes = sum(d.get("crashes", 0) for d in days)
    body = (
        f"<h1>NucleoOS P4 — statistics</h1>"
        f"<p class=dim>{len(days)} day(s), {beats} report(s). Generated {datetime.datetime.now():%Y-%m-%d %H:%M}. "
        f"Opt-in, anonymous: one report per device per day, no device id.</p>"
        f"<div class=k><div><b>{last.get('beats', 0)}</b>active yesterday</div>"
        f"<div><b>{months.get(datetime.date.today().strftime('%Y-%m'), 0)}</b>active this month</div>"
        f"<div><b>{sum(d.get('new', 0) for d in days)}</b>new devices</div>"
        f"<div><b>{crashes}</b>crashes</div></div>"
        + bar_table("Monthly active devices", Counter(dict(sorted(months.items()))), limit=24)
        + bar_table("Firmware (reports)", merge(days, "version"))
        + bar_table("App minutes", merge(days, "app_minutes"), " min")
        + bar_table("App launches", merge(days, "app_launches"))
        + bar_table("Devices using the app (device-days)", merge(days, "app_devices"))
        + bar_table("Reset reasons", merge(days, "reset"))
        + bar_table("Updates", merge(days, "ota"))
        + bar_table("Store", merge(days, "store"))
        + bar_table("Language", merge(days, "lang"))
        + bar_table("Region", merge(days, "region"))
        + bar_table("PSRAM (MB)", merge(days, "psram_mb"))
        + bar_table("Lowest free SRAM (KB)", merge(days, "heap_min_kb"))
        + "<h2>Per day</h2><table><tr><th>day</th><th>reports</th><th>new</th><th>crashes</th></tr>"
        + daily + "</table>")
    css = ("body{font:15px/1.5 system-ui,sans-serif;max-width:980px;margin:32px auto;padding:0 16px;"
           "background:#fff;color:#1d1d1f}h2{margin-top:28px;font-size:18px}.dim{color:#666}"
           "table{border-collapse:collapse;width:100%}td,th{padding:4px 8px;border-bottom:1px solid #eee;text-align:left}"
           ".n{text-align:right;width:90px}.b span{display:block;height:10px;background:#2563eb;border-radius:3px}"
           ".k{display:flex;gap:16px;flex-wrap:wrap}.k div{flex:1 1 180px;background:#f4f5f8;border-radius:10px;padding:12px}"
           ".k b{display:block;font-size:28px}"
           "@media (prefers-color-scheme:dark){body{background:#111;color:#eee}.dim{color:#aaa}"
           "td,th{border-color:#2a2a2a}.k div{background:#1c1c1e}.b span{background:#8ab4ff}}")
    return f"<!doctype html><meta charset=utf-8><title>NucleoOS statistics</title><style>{css}</style>{body}"


def main():
    ap = argparse.ArgumentParser(description="private report of the opt-in statistics")
    ap.add_argument("--days", type=int, default=30)
    ap.add_argument("--out", default=os.path.join(os.path.expanduser("~"), ".nucleo", "telemetry", "report.html"))
    a = ap.parse_args()
    days = fetch(a.days)
    os.makedirs(os.path.dirname(a.out), exist_ok=True)
    with open(a.out, "w", encoding="utf-8") as f:
        f.write(page(days))
    print(f"REPORT {a.out}  ({len(days)} day(s))")


if __name__ == "__main__":
    main()
