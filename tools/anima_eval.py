"""Measure what ANIMA really gets done on the board: real tasks through POST /api/anima/chat.

Each case is one fresh conversation. A case passes when the reply contains what it must, the steps
it had to run show up in the trace, no raw "ACT ..." line leaks into the reply, and an optional file
check on the board succeeds. When ANIMA asks "procedo? (si/no)" the tool answers yes (up to 3
times), which also tests that the task goes on after a confirmation.

Only headless APIs are used (/api/anima/chat, /api/fs/*): nothing pops up on the tablet's screen
besides what ANIMA itself opens (the open-app case), and the tool goes back home at the end.
Each run works in its own folder ~/eval/r<time>.

  python tools/anima_eval.py                    all cases
  python tools/anima_eval.py -k lua -k python   cases whose id contains one of these words
  python tools/anima_eval.py --list
  python tools/anima_eval.py --json out.json    also save the full report

Board: --host, else NV_BOARD_IP / NUCLEO_HOST, else %USERPROFILE%/.nucleo/host, else nucleov2.local.
Token: tools/pair.py. Exit status: 0 all passed, 1 some failed, 255 board unreachable.
"""
import argparse
import json
import os
import re
import sys
import time
import urllib.parse
import urllib.request

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from nvtoken import auth_headers  # noqa: E402

# id, request ({d} = the run's folder under ~), reply must match (regex), trace must contain,
# file check (path under ~ with {d}, regex its content must match) or None
CASES = [
    ("lua-hello",
     "Scrivi un programma Lua in ~/{d}/hello.lua che stampa esattamente Ciao NucleoOS, eseguilo e dimmi l'output.",
     r"Ciao NucleoOS", ["sh"], ("{d}/hello.lua", r"Ciao NucleoOS")),
    ("lua-fix-bug",
     "Il file ~/{d}/bug.lua ha un errore: correggilo, eseguilo e dimmi cosa stampa.",
     r"\b55\b", ["sh"], ("{d}/bug.lua", r"\bend\b")),
    ("python-bigint",
     "Usa python sul dispositivo per calcolare 2**100 e dimmi il numero esatto.",
     r"1267650600228229401496703205376", ["sh"], None),
    ("js-run",
     "Esegui con js sul dispositivo: console.log([3,1,2].sort().join('-')) e dimmi il risultato.",
     r"1-2-3", ["sh"], None),
    ("dates",
     "Quanti giorni ci sono tra il 2026-01-01 e il 2026-12-25? Calcolalo con gli strumenti del dispositivo.",
     r"\b358\b", ["sh"], None),
    ("units",
     "Quanti chilometri sono 26.2 miglia? Usa units sul dispositivo.",
     r"42[.,]1", ["sh"], None),
    ("files-count",
     "Quanti file .lua ci sono nella cartella ~/{d}? Rispondi con il numero.",
     r"\b2\b|\bdue\b", ["sh"], None),
    ("board-memory",
     "Quanta PSRAM libera ha la scheda adesso?",
     r"\d", ["sh"], None),
    ("open-app",
     "Apri la calcolatrice.",
     r".", [], None),
]

# a missing 'end' for the for loop; fixed, it prints the sum 1..10 = 55
BUG_LUA = "s = 0\nfor i = 1, 10 do\n  s = s + i\nprint(s)\n"


def board_host(arg):
    if arg:
        return arg
    for k in ("NV_BOARD_IP", "NUCLEO_HOST"):
        if os.environ.get(k):
            return os.environ[k]
    try:
        with open(os.path.join(os.path.expanduser("~"), ".nucleo", "host"), encoding="utf-8") as f:
            h = f.read().strip()
            if h:
                return h
    except OSError:
        pass
    return "nucleov2.local"


class Board:
    def __init__(self, host):
        self.base = "http://" + host
        self.hdr = auth_headers()

    def req(self, method, route, body=None, timeout=240, **query):
        url = self.base + route + ("?" + urllib.parse.urlencode(query) if query else "")
        data = body.encode("utf-8") if isinstance(body, str) else body
        if method == "POST" and data is None:
            data = b""
        h = dict(self.hdr)
        if data:
            h["Content-Type"] = "application/json" if route == "/api/anima/chat" else "application/octet-stream"
        r = urllib.request.Request(url, data=data, headers=h, method=method)
        with urllib.request.urlopen(r, timeout=timeout) as resp:
            return resp.read().decode("utf-8", "replace")

    def mkdir(self, home_rel):
        return self.req("POST", "/api/fs/mkdir", path="/home/" + home_rel)

    def write(self, home_rel, text):
        return self.req("POST", "/api/fs/write", text, path="/home/" + home_rel)

    def read(self, home_rel):
        try:
            return self.req("GET", "/api/fs/read", path="/home/" + home_rel)
        except OSError:
            return None

    def chat(self, q, conv=""):
        r = json.loads(self.req("POST", "/api/anima/chat", json.dumps({"q": q, "conv": conv, "lang": "it"})))
        end = time.time() + 900                          # firmware >= 1.2.8: long turns answer "pending"
        while r.get("pending") and r.get("job") and time.time() < end:
            r = json.loads(self.req("GET", "/api/anima/job", id=r["job"], wait_ms=1500))
        return r


def run_case(b, case, d):
    cid, q, want, steps, check = case
    t0 = time.time()
    r = b.chat(q.format(d=d))
    replies, traces = [r.get("reply", "")], [r.get("trace", "")]
    for _ in range(3):                                   # confirmations: yes, and the task must go on
        if not r.get("awaiting"):
            break
        r = b.chat("sì", r.get("conv", ""))
        replies.append(r.get("reply", ""))
        traces.append(r.get("trace", ""))
    reply, trace = "\n".join(replies), " | ".join(t for t in traces if t)
    why = []
    if not r.get("ok", False):
        why.append("no answer (%s)" % (r.get("why") or "offline?"))
    joined = re.sub(r"(?<=\d)[\s   '’]+(?=\d)", "", reply)   # 1 267 650 -> 1267650
    joined = re.sub(r"(?<=\d)[.,](?=\d{3}(?!\d))", "", joined)              # 1.267.650 -> 1267650
    if not (re.search(want, reply, re.I | re.S) or re.search(want, joined, re.I | re.S)):
        why.append("reply lacks /%s/" % want)
    for s in steps:
        if s not in trace:
            why.append("no '%s' step in the trace" % s)
    if re.search(r"^\s*`*ACT ", reply, re.M):
        why.append("raw ACT line in the reply")
    if check:
        content = b.read(check[0].format(d=d))
        if content is None or not re.search(check[1], content):
            why.append("file check failed: ~/%s !~ /%s/" % (check[0].format(d=d), check[1]))
    return {"id": cid, "ok": not why, "why": why, "secs": round(time.time() - t0, 1),
            "trace": trace, "reply": reply, "conv": r.get("conv", ""), "action": r.get("action")}


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--host")
    ap.add_argument("-k", action="append", default=[], help="run only cases whose id contains this")
    ap.add_argument("--list", action="store_true")
    ap.add_argument("--json", help="save the full report here")
    a = ap.parse_args()
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")   # replies carry any character
    cases =[c for c in CASES if not a.k or any(k in c[0] for k in a.k)]
    if a.list:
        for c in cases:
            print("%-14s %s" % (c[0], c[1]))
        return 0
    b = Board(board_host(a.host))
    d = "eval/r%d" % int(time.time())
    try:
        b.mkdir(d)
        b.write(d + "/bug.lua", BUG_LUA)
    except OSError as e:
        print("cannot talk to the board: %s" % e, file=sys.stderr)
        return 255
    print("board %s, folder ~/%s" % (b.base, d))
    results = []
    for c in cases:
        try:
            res = run_case(b, c, d)
        except OSError as e:
            res = {"id": c[0], "ok": False, "why": ["request failed: %s" % e], "secs": 0, "trace": "", "reply": ""}
        results.append(res)
        print("%s %-14s %5.1fs  %s" % ("PASS" if res["ok"] else "FAIL", res["id"], res["secs"],
                                        "; ".join(res["why"]) or res["trace"][:90]))
        if not res["ok"]:
            print("      trace: %s" % res["trace"][:200])
            print("      reply: %s" % res["reply"].replace("\n", " / ")[:300])
    try:
        b.req("GET", "/api/ui/home")
    except OSError:
        pass
    n = sum(r["ok"] for r in results)
    print("\n%d/%d passed" % (n, len(results)))
    if a.json:
        with open(a.json, "w", encoding="utf-8") as f:
            json.dump(results, f, ensure_ascii=False, indent=1)
    return 0 if n == len(results) else 1


if __name__ == "__main__":
    sys.exit(main())
