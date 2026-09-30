#!/usr/bin/env python3
"""nvtele-stats: daily summaries of the NucleoOS P4 telemetry beats (opt-in, anonymous).

Destination: /usr/local/bin/nvtele-stats (run hourly by nvstore-stats.timer, after nvstore-stats).

nginx answers GET /t/1?<fields> with 204 and appends "time status args" to
/var/log/nginx/nvstore/tele (log format "nvtele": no IP address, no user agent). logrotate keeps
90 days of that raw log (/etc/logrotate.d/nvtele) and deletes older ones. This script turns every
complete or current day into /var/lib/nvtele/days/YYYY-MM-DD.json — counts only, nothing that
tells two devices apart — which is what the private report (tools/telemetry_report.py) reads.

A beat carries (see components/nv_telemetry/nv_telemetry.cpp, docs/privacy):
  v  firmware version          l  UI language          r  region setting
  m  1 = first beat this month (monthly active devices without any device id)
  n  1 = first beat ever after consent (new devices)
  rb reset reasons since the last beat "reason.count,..."   cr crashes since the last beat
  ota last OTA outcome since the last beat (ok|rollback|fail)
  a  app use "id.launches.minutes,..." (catalog / system apps only)
  si si su sx  store installs / updates / uninstalls since the last beat
  hw hardware flags "psram.sd.pad.kbd" (MB, 0/1, 0/1, 0/1)     hm lowest free internal heap, KB bucket
"""
import glob
import gzip
import json
import os
import re
import sys
import time
from collections import Counter, defaultdict
from urllib.parse import parse_qs

LOG_DIR = "/var/log/nginx/nvstore"
OUT_DIR = "/var/lib/nvtele/days"
ID_RE = re.compile(r"^[A-Za-z0-9_-]{1,31}$")
VER_RE = re.compile(r"^[0-9]{1,3}\.[0-9]{1,3}\.[0-9]{1,4}$")
TOKEN_RE = re.compile(r"^[a-z0-9_]{1,16}$")
MAX_APPS = 24


def lines():
    """Every beat line still on disk: the live log and the rotated ones (plain or gzipped)."""
    files = sorted(glob.glob(os.path.join(LOG_DIR, "tele*")))
    for path in files:
        opener = gzip.open if path.endswith(".gz") else open
        try:
            with opener(path, "rt", encoding="ascii", errors="replace") as f:
                yield from f
        except OSError:
            continue


def num(q, k, lo=0, hi=10 ** 6):
    try:
        v = int(q.get(k, ["0"])[0])
    except ValueError:
        return 0
    return v if lo <= v <= hi else 0


def tok(q, k):
    v = q.get(k, [""])[0]
    return v if TOKEN_RE.match(v) else ""


def main():
    days = defaultdict(lambda: {
        "beats": 0, "monthly_first": 0, "new": 0,
        "version": Counter(), "lang": Counter(), "region": Counter(),
        "reset": Counter(), "crashes": 0, "ota": Counter(),
        "app_launches": Counter(), "app_minutes": Counter(), "app_devices": Counter(),
        "store": Counter(), "psram_mb": Counter(), "sd": 0, "pad": 0, "kbd": 0, "heap_min_kb": Counter(),
    })
    for ln in lines():
        parts = ln.split(" ", 2)
        if len(parts) != 3 or parts[1] != "204":
            continue
        day = parts[0][:10]
        if not re.match(r"^\d{4}-\d{2}-\d{2}$", day):
            continue
        q = parse_qs(parts[2].strip()[:2048], max_num_fields=32)
        d = days[day]
        d["beats"] += 1
        d["monthly_first"] += num(q, "m", 0, 1)
        d["new"] += num(q, "n", 0, 1)
        v = q.get("v", [""])[0]
        d["version"][v if VER_RE.match(v) else "?"] += 1
        d["lang"][tok(q, "l") or "?"] += 1
        d["region"][(q.get("r", [""])[0].upper() if re.match(r"^[A-Za-z*]{1,4}$", q.get("r", [""])[0]) else "?")] += 1
        for item in q.get("rb", [""])[0].split(",")[:12]:
            k, _, c = item.partition(".")
            if TOKEN_RE.match(k) and c.isdigit():
                d["reset"][k] += min(int(c), 1000)
        d["crashes"] += num(q, "cr", 0, 1000)
        o = tok(q, "ota")
        if o in ("ok", "rollback", "fail"):
            d["ota"][o] += 1
        for item in q.get("a", [""])[0].split(",")[:MAX_APPS]:
            f = item.split(".")
            if len(f) == 3 and ID_RE.match(f[0]) and f[1].isdigit() and f[2].isdigit():
                d["app_launches"][f[0]] += min(int(f[1]), 10000)
                d["app_minutes"][f[0]] += min(int(f[2]), 1440)
                d["app_devices"][f[0]] += 1
        for k, name in (("si", "installs"), ("su", "updates"), ("sx", "uninstalls")):
            d["store"][name] += num(q, k, 0, 1000)
        hw = q.get("hw", [""])[0].split(".")
        if len(hw) == 4 and all(x.isdigit() for x in hw):
            d["psram_mb"][hw[0]] += 1
            d["sd"] += hw[1] == "1"
            d["pad"] += hw[2] == "1"
            d["kbd"] += hw[3] == "1"
        hm = num(q, "hm", 0, 1024)
        if hm:
            d["heap_min_kb"][str(hm // 16 * 16)] += 1

    os.makedirs(OUT_DIR, exist_ok=True)
    for day, d in days.items():
        body = {"day": day, "generated": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime())}
        for k, v in d.items():
            body[k] = dict(v.most_common()) if isinstance(v, Counter) else v
        tmp = os.path.join(OUT_DIR, f".{day}.tmp")
        with open(tmp, "w", encoding="utf-8") as f:
            json.dump(body, f, sort_keys=True)
        os.replace(tmp, os.path.join(OUT_DIR, f"{day}.json"))
    print(f"nvtele-stats: {len(days)} day(s), {sum(d['beats'] for d in days.values())} beat(s)")


if __name__ == "__main__":
    sys.exit(main())
