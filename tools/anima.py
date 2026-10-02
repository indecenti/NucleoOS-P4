"""Talk to ANIMA on the board from a PC shell: plain text in, compact text out - no screenshots.

Every answer is one short block: what ANIMA understood and did, then its reply.

  python tools/anima.py "che ore sono"            one question
  python tools/anima.py -i                        interactive (/mode /models /caps /chat /json /quit)
  python tools/anima.py --chat "e domani?"        a conversation turn (memory + context, /api/anima/chat)
  python tools/anima.py --test tools/anima/smoke.txt   run a regression file on the real device
  python tools/anima.py --mode local              network mode: offline | local | hybrid | llm
  python tools/anima.py --models                  the models the configured server offers (Ollama...)
  python tools/anima.py --caps                    engine status: mode, provider, model, L1
  python tools/anima.py --json "apri musica"      the raw JSON of the answer

Output of one turn:
  > apri musica
  [open_app music] tier=command conf=95
  Apro Musica.
    └ L0 apps | 95%

Test file: one case per line, "question => intent" with an optional "| text the reply contains";
"#" starts a comment, "@mode local" switches the network mode for the lines below, "en:" in front of
a question asks it in English. Exit code: the number of failed cases (0 = all passed).

  quanto fa 6 per 7 => calc | 42
  apri la calcolatrice => open_app | Calcolatrice

Board address: --host, else NV_BOARD_IP / NUCLEO_HOST, else nucleov2.local. Token: the one
tools/pair.py saved (or NUCLEO_TOKEN). Exit 255 = could not talk to the board.
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

MODES = ("offline", "local", "hybrid", "llm")


class BoardError(Exception):
    pass


class Anima:
    def __init__(self, host, lang):
        self.base = "http://" + host
        self.lang = lang
        self.headers = auth_headers()
        self.conv = ""

    def call(self, method, path, body=None, timeout=140, **query):
        q = {k: v for k, v in query.items() if v is not None}
        url = self.base + path + ("?" + urllib.parse.urlencode(q) if q else "")
        data = json.dumps(body).encode("utf-8") if body is not None else (b"" if method == "POST" else None)
        hdr = dict(self.headers)
        hdr["Content-Type"] = "application/json"
        req = urllib.request.Request(url, data=data, method=method, headers=hdr)
        try:
            with urllib.request.urlopen(req, timeout=timeout) as r:
                raw = r.read()
        except urllib.error.HTTPError as e:
            if e.code == 401:
                raise BoardError("not paired with the board (401): run  python tools/pair.py --host %s" % self.base[7:])
            raise BoardError("HTTP %d: %s" % (e.code, e.read().decode("utf-8", "replace")[:200]))
        except (urllib.error.URLError, OSError) as e:
            raise BoardError("cannot reach %s: %s" % (self.base, getattr(e, "reason", e)))
        return json.loads(raw.decode("utf-8", "replace") or "{}")

    # A turn that outlasts the board's wait answers {"pending":true,"job":N}: collect it.
    def await_job(self, r, limit_s=900):
        end = time.time() + limit_s
        while r.get("pending") and r.get("job") and time.time() < end:
            r = self.call("GET", "/api/anima/job", id=r["job"], wait_ms=1500)
        return r

    # A question through the full cascade (actions really run on the device).
    def ask(self, q, lang=None):
        r = self.call("GET", "/api/anima", q=q, lang=lang or self.lang)
        if r.get("busy"):   # the on-device app holds the engine for a moment: one retry
            time.sleep(1.5)
            r = self.call("GET", "/api/anima", q=q, lang=lang or self.lang)
        return self.await_job(r)

    # A conversation turn: memory + rolling summary + recent turns, stored on the device.
    def chat(self, q, lang=None):
        r = self.await_job(self.call("POST", "/api/anima/chat", {"q": q, "conv": self.conv, "lang": lang or self.lang}))
        self.conv = r.get("conv") or self.conv
        return r

    def mode(self, m=None):
        return self.call("POST", "/api/anima/net", {"mode": m}) if m else self.call("GET", "/api/anima/net")

    def models(self):
        return self.call("GET", "/api/anima/models", lang=self.lang)

    def caps(self):
        return self.call("GET", "/api/anima/caps")


def render(q, r, ms):
    """The compact block for one answer."""
    if r.get("busy"):
        return "> %s\n(busy: the device is answering another request)" % q
    head = "[%s%s]" % (r.get("intent") or r.get("action") or "-", (" " + r["arg"]) if r.get("arg") else "")
    bits = [head]
    if r.get("tier"):
        bits.append("tier=%s" % r["tier"])
    if r.get("conf") is not None:
        bits.append("conf=%s" % r["conf"])
    bits.append("%.1fs" % (ms / 1000.0))
    out = ["> " + q, " ".join(bits), (r.get("reply") or "(no reply)").strip()]
    if "done" in r:
        out.append("  └ %s %s" % ("done:" if r["done"] else "NOT done:", r.get("note", "")))
    if r.get("why"):
        out.append("  └ why: " + r["why"])
    if r.get("trace"):
        out.append("  └ " + r["trace"])
    return "\n".join(out)


def timed(fn, *a):
    t0 = time.time()
    r = fn(*a)
    return r, (time.time() - t0) * 1000


def split_lang(q):
    return (q[3:].strip(), "en") if q.lower().startswith("en:") else (q, None)


def run_test(an, path, verbose):
    """Run a regression file; returns the number of failed cases."""
    fails = total = 0
    with open(path, encoding="utf-8") as f:
        lines = [ln.rstrip("\n") for ln in f]
    for n, line in enumerate(lines, 1):
        s = line.strip()
        if not s or s.startswith("#"):
            continue
        if s.startswith("@mode"):
            an.mode(s.split()[1])
            continue
        if "=>" not in s:
            print("%s:%d: no '=>' in: %s" % (path, n, s))
            fails += 1
            continue
        q, exp = [p.strip() for p in s.split("=>", 1)]
        want_intent, want_text = (exp.split("|", 1) + [""])[:2]
        want_intent, want_text = want_intent.strip(), want_text.strip()
        q, lang = split_lang(q)
        total += 1
        r, ms = timed(an.ask, q, lang)
        ok = (not want_intent or r.get("intent") == want_intent) and \
             (not want_text or want_text.lower() in (r.get("reply") or "").lower())
        if not ok:
            fails += 1
            print("FAIL %s:%d  want %s%s" % (path, n, want_intent, (" | " + want_text) if want_text else ""))
            print(render(q, r, ms))
        elif verbose:
            print("ok   %-40s %s %.1fs" % (q[:40], r.get("intent"), ms / 1000.0))
    print("%d/%d passed" % (total - fails, total))
    return fails


def repl(an, as_json):
    print("ANIMA on %s - questions, or /mode [m] /models /caps /chat /json /quit" % an.base[7:])
    chat = False
    while True:
        try:
            q = input("> ").strip()
        except (EOFError, KeyboardInterrupt):
            print()
            return 0
        if not q:
            continue
        if q in ("/quit", "/exit"):
            return 0
        try:
            if q.startswith("/mode"):
                parts = q.split()
                print(json.dumps(an.mode(parts[1] if len(parts) > 1 else None)))
            elif q == "/models":
                print(json.dumps(an.models(), ensure_ascii=False))
            elif q == "/caps":
                print(json.dumps(an.caps(), ensure_ascii=False))
            elif q == "/chat":
                chat = not chat
                print("conversation mode " + ("on" if chat else "off"))
            elif q == "/json":
                as_json = not as_json
                print("json " + ("on" if as_json else "off"))
            else:
                qq, lang = split_lang(q)
                r, ms = timed(an.chat if chat else an.ask, qq, lang)
                print(json.dumps(r, ensure_ascii=False, indent=1) if as_json else render(qq, r, ms).split("\n", 1)[1])
        except BoardError as e:
            print("error: %s" % e)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("question", nargs="*")
    ap.add_argument("--host", default=os.environ.get("NV_BOARD_IP") or os.environ.get("NUCLEO_HOST") or "nucleov2.local")
    ap.add_argument("--lang", default="it", choices=("it", "en"))
    ap.add_argument("-i", "--interactive", action="store_true")
    ap.add_argument("--chat", action="store_true", help="conversation turn (memory + context)")
    ap.add_argument("--json", action="store_true", help="print the raw JSON")
    ap.add_argument("--test", metavar="FILE", help="run a regression file")
    ap.add_argument("-v", "--verbose", action="store_true", help="with --test: list passing cases too")
    ap.add_argument("--mode", choices=MODES, help="set the network mode")
    ap.add_argument("--models", action="store_true")
    ap.add_argument("--caps", action="store_true")
    a = ap.parse_args()
    an = Anima(a.host, a.lang)
    try:
        if a.mode:
            print(json.dumps(an.mode(a.mode)))
        if a.models:
            print(json.dumps(an.models(), ensure_ascii=False))
        if a.caps:
            print(json.dumps(an.caps(), ensure_ascii=False))
        if a.test:
            return min(run_test(an, a.test, a.verbose), 254)
        if a.interactive:
            return repl(an, a.json)
        if a.question:
            q, lang = split_lang(" ".join(a.question))
            r, ms = timed(an.chat if a.chat else an.ask, q, lang)
            print(json.dumps(r, ensure_ascii=False, indent=1) if a.json else render(q, r, ms))
        elif not (a.mode or a.models or a.caps):
            ap.print_help()
    except BoardError as e:
        print("anima: %s" % e, file=sys.stderr)
        return 255
    return 0


if __name__ == "__main__":
    sys.exit(main())
