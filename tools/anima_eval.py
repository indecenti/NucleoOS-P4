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
     r"42[.,]1", [], None),            # the device's own exact converter (L0) is as good as units
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

# Harder tasks (--set hard): several steps, data the model must read instead of guess, tools it has to
# pick (jq, grep, eigenmath, datediff, curl), a picture of the screen. Seeds below.
HARD = [
    ("sum-file",
     "Il file ~/{d}/numeri.txt contiene un numero per riga. Sommali tutti e dimmi il totale esatto.",
     r"\b4821\b", ["sh"], None),
    ("log-grep",
     "Nel log ~/{d}/app.log c'e' una sola riga con ERROR: a che ora e' successo e qual e' il messaggio?",
     r"14:32:07.*(disk|disco)|(disk|disco).*14:32:07", ["sh"], None),
    ("json-jq",
     "Il file ~/{d}/utenti.json e' un array di utenti con nome ed eta. Quanti hanno piu' di 30 anni e chi e' il piu' vecchio?",
     r"(\b3\b|\btre\b).*Giulia|Giulia.*(\b3\b|\btre\b)", ["sh"], None),
    ("edit-json",
     "Nel file ~/{d}/config.json cambia il valore di volume da 30 a 55, lascia il resto com'e', e conferma.",
     r"55", [], ("{d}/config.json", r'(?s)"volume"\s*:\s*55.*"tema"\s*:\s*"scuro".*"lingua"')),
    ("python-primes",
     "Scrivi ~/{d}/primi.py che stampa i numeri primi minori di 100, uno per riga; eseguilo e dimmi quanti sono e qual e' il piu' grande.",
     r"(?s)(\b25\b|venticinque).*\b97\b|\b97\b.*(\b25\b|venticinque)", ["sh"], ("{d}/primi.py", r"print")),
    ("weekday",
     "Che giorno della settimana sara' il 25 dicembre 2026? Verificalo con gli strumenti del dispositivo.",
     r"venerd", ["sh"], None),
    ("derivative",
     "Calcola la derivata di x^3*sin(x) rispetto a x, in modo esatto.",
     r"cos", [], None),               # a textbook derivative needs no tool; the result is what counts
    ("days-left",
     "Quanti giorni ci sono dal 2026-10-05 al 2027-01-01? Usa gli strumenti del dispositivo.",
     r"\b88\b", ["sh"], None),
    ("web-title",
     "Scarica la pagina https://example.com e dimmi il suo titolo.",
     r"Example Domain", ["sh"], None),
    ("csv-avg",
     "Crea ~/{d}/voti.csv con intestazione nome,voto e queste righe: Anna 8, Bruno 6, Carla 9, Dario 7. "
     "Poi calcola la media dei voti con lua leggendo il file, e dimmela.",
     r"7[.,]5", ["sh"], ("{d}/voti.csv", r"(?s)nome,voto.*Carla,\s*9")),
    ("sqlite-sum",
     "Crea un database sqlite ~/{d}/spese.db con una tabella spese(voce, importo), inserisci affitto 700, "
     "cibo 250 e bollette 120, poi con una query dimmi il totale.",
     r"\b1070\b", ["sh"], None),
    ("python-test",
     "Scrivi ~/{d}/conv.py con una funzione celsius_to_f e dei test unittest per 0, 100 e -40 gradi; "
     "eseguilo e dimmi se i test passano.",
     r"(?i)\bOK\b|passa|superat|riuscit", ["sh"], ("{d}/conv.py", r"unittest")),
    ("lua-app",
     "Crea un'app Lua App chiamata contapassi{n} con un numero grande al centro, un pulsante +1 e uno Azzera; "
     "avviala e verifica che non ci siano errori.",
     r"(?i)pront|funzion|nessun errore|avviat|senza errori|gira", ["app run"], ("lua/contapassi{n}.lua", r"ui\.button")),
    ("screen-see",
     "Apri la calcolatrice, fai uno screenshot e dimmi cosa vedi sullo schermo.",
     r"calcol|tast|numer|\d", ["screenshot"], None),
]

# Other languages (--set ml): the same kind of work asked in English, French, Spanish, German. The reply must
# carry the result AND be in the user's language (a few function words of it, none of Italian's).
ML = [
    ("en-sum",  "en", "The file ~/{d}/numeri.txt has one number per line. Add them all up and tell me the exact total.",
     r"\b4821\b", ["sh"], None),
    ("fr-days", "en", "Combien de jours y a-t-il entre le 2026-01-01 et le 2026-12-25 ? Calcule-le avec les outils de l'appareil.",
     r"\b358\b", ["sh"], None),
    ("es-py",   "en", "Usa python en el dispositivo para calcular 3**40 y dime el numero exacto.",
     r"12157665459056928801", ["sh"], None),
    ("de-log",  "en", "In der Datei ~/{d}/app.log gibt es eine Zeile mit ERROR. Um wie viel Uhr ist das passiert?",
     r"14:32", ["sh"], None),
    ("en-lua",  "en", "Write a Lua program ~/{d}/fizz.lua that prints FizzBuzz from 1 to 15, run it and tell me the last line.",
     r"FizzBuzz", ["sh"], ("{d}/fizz.lua", r"(?i)fizz")),
    ("fr-json", "en", "Dans ~/{d}/utenti.json, qui est l'utilisateur le plus age ?",
     r"Giulia", ["sh"], None),
]
# Conversations (--set conv): later turns lean on earlier ones ("aggiungi kiwi", "e diviso 4?").
# The LAST turn's reply and trace are judged.
CONV = [
    ("conv-file", ["Crea il file ~/{d}/lista.txt con tre righe: mele, pere, banane.",
                   "Aggiungi kiwi in fondo allo stesso file.",
                   "Quante righe ha adesso quel file? Controllalo."],
     r"\b4\b|quattro", ["sh"], ("{d}/lista.txt", r"(?s)mele.*pere.*banane.*kiwi")),
    ("conv-calc", ["Quanto fa 12 per 13?", "E diviso 4?"], r"\b39\b", [], None),
    ("conv-code", ["Scrivi in lua ~/{d}/quad.lua che stampa i quadrati da 1 a 5, ed eseguilo.",
                   "Ora fallo arrivare fino a 8 e dimmi l'ultimo numero che stampa."],
     r"\b64\b", ["sh"], ("{d}/quad.lua", r"8")),
]

LANG_WORDS = {
    "en-": r"\b(the|is|are|and|of|line|total|it)\b",
    "fr-": r"\b(le|la|les|est|jours|de|il|y a)\b",
    "es-": r"\b(el|es|la|de|resultado|numero|número)\b",
    "de-": r"\b(der|die|das|ist|um|Uhr|Zeile)\b",
}
ITALIAN = r"\b(il risultato|ecco|sono|giorni|della|nel file)\b"

SEEDS = {
    "numeri.txt": "".join("%d\n" % n for n in (1200, 845, 17, 999, 60, 1700)),          # sum 4821
    "app.log": "2026-10-05 14:30:00 INFO avvio\n2026-10-05 14:31:12 WARN memoria bassa\n"
               "2026-10-05 14:32:07 ERROR disk full on /sdcard\n2026-10-05 14:33:40 INFO ripresa\n",
    "utenti.json": json.dumps([{"nome": "Marco", "eta": 25}, {"nome": "Giulia", "eta": 47},
                               {"nome": "Luca", "eta": 31}, {"nome": "Sara", "eta": 29},
                               {"nome": "Paolo", "eta": 38}]),                          # 3 over 30, Giulia
    "config.json": '{\n  "volume": 30,\n  "tema": "scuro",\n  "lingua": "it"\n}\n',
}

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

    def chat(self, q, conv="", lang="it"):
        r = json.loads(self.req("POST", "/api/anima/chat", json.dumps({"q": q, "conv": conv, "lang": lang})))
        end = time.time() + 900                          # firmware >= 1.2.8: long turns answer "pending"
        while r.get("pending") and r.get("job") and time.time() < end:
            r = json.loads(self.req("GET", "/api/anima/job", id=r["job"], wait_ms=1500))
        return r


def run_case(b, case, d):
    lang = "it"
    if len(case) == 6:                                   # ML: (id, lang, q, want, steps, check)
        cid, lang, q, want, steps, check = case
    else:
        cid, q, want, steps, check = case
    t0 = time.time()
    n = d.rsplit("r", 1)[-1][-5:]                      # unique per run: an app name never reused
    turns = q if isinstance(q, list) else [q]            # CONV: several turns of one conversation
    conv = ""
    for turn in turns:
        r = b.chat(turn.format(d=d, n=n), conv, lang=lang)
        conv = r.get("conv", conv)
        replies, traces = [r.get("reply", "")], [r.get("trace", "")]
        for _ in range(3):                               # confirmations: yes, and the task must go on
            if not r.get("awaiting"):
                break
            r = b.chat("sì" if lang == "it" else "yes", conv, lang)
            replies.append(r.get("reply", ""))
            traces.append(r.get("trace", ""))
    reply, trace = "\n".join(replies), " | ".join(t for t in traces if t)   # the last turn is judged
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
    for pre, words in LANG_WORDS.items():                # other languages: answered in the user's own
        if cid.startswith(pre):
            if len(re.findall(r"[^\W\d_]{2,}", reply)) >= 6 and not re.search(words, reply, re.I):   # prose, not just a number
                why.append("reply not in the user's language")
            if re.search(ITALIAN, reply, re.I):
                why.append("reply in Italian")
    if re.search(r"^\s*`*ACT ", reply, re.M):
        why.append("raw ACT line in the reply")
    if check:
        content = b.read(check[0].format(d=d, n=n))
        if content is None or not re.search(check[1], content):
            why.append("file check failed: ~/%s !~ /%s/" % (check[0].format(d=d, n=n), check[1]))
    return {"id": cid, "ok": not why, "why": why, "secs": round(time.time() - t0, 1),
            "trace": trace, "reply": reply, "conv": r.get("conv", ""), "action": r.get("action")}


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--host")
    ap.add_argument("-k", action="append", default=[], help="run only cases whose id contains this")
    ap.add_argument("--list", action="store_true")
    ap.add_argument("--json", help="save the full report here")
    ap.add_argument("--set", choices=("base", "hard", "ml", "conv", "all"), default="base",
                    help="base = 9 everyday tasks, hard = multi-step tasks on real data, ml = other languages, all")
    a = ap.parse_args()
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")   # replies carry any character
    pool = {"base": CASES, "hard": HARD, "ml": ML, "conv": CONV, "all": CASES + HARD + ML + CONV}[a.set]
    cases = [c for c in pool if not a.k or any(k in c[0] for k in a.k)]
    if a.list:
        for c in cases:
            q = c[2] if len(c) == 6 else c[1]
            print("%-14s %s" % (c[0], " / ".join(q) if isinstance(q, list) else q))
        return 0
    b = Board(board_host(a.host))
    d = "eval/r%d" % int(time.time())
    try:
        b.mkdir(d)
        b.write(d + "/bug.lua", BUG_LUA)
        for name, text in SEEDS.items():
            b.write(d + "/" + name, text)
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
