"""Close ANIMA's learning loop from the dev PC: what the board did NOT understand, and what its user taught it.

  python tools/anima_misses.py                      report (board: NUCLEO_HOST or nucleov2.local)
  python tools/anima_misses.py --promote            also move the user-taught phrases into tools/anima_phrases.txt
  python tools/anima_misses.py --host 192.168.0.128

Reads over the paired web API (python tools/pair.py first):
  /data/anima/telemetry.ndjson     every turn; tier "none" = a miss, intent "suggest" = a suggestion was offered
  /data/anima/phrases.user.tsv     sentences the user confirmed with "sì" to a suggestion (key<TAB>canonical)
For each miss it shows what the offline suggester would offer (tools/train_anima_intent.py, if numpy and
scikit-learn are installed) so a new line for tools/anima_phrases.txt is one copy away. --promote appends the
user-taught phrases as templates under their canonical group: after gen_anima_phrases.py they ship to everyone.
"""
import argparse
import collections
import json
import os
import re
import sys
import urllib.parse
import urllib.request

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import gen_anima_phrases as gen  # noqa: E402
from nvtoken import token_path  # noqa: E402

PHRASES = os.path.join(gen.ROOT, "tools", "anima_phrases.txt")


def fetch(host, path, tok):
    url = "http://%s/api/fs/read?path=%s" % (host, urllib.parse.quote(path))
    req = urllib.request.Request(url, headers={"Authorization": "Bearer " + tok})
    try:
        with urllib.request.urlopen(req, timeout=20) as r:
            return r.read().decode("utf-8", "replace")
    except urllib.error.HTTPError as e:
        if e.code == 404:
            return ""
        raise


def suggester():
    try:
        import train_anima_intent as tr
        from sklearn.linear_model import LogisticRegression
    except ImportError:
        return None
    X, y, lang_of = tr.dataset()
    q = tr.Quant(LogisticRegression(C=8.0, max_iter=5000).fit(tr.matrix(X), y))
    return lambda s, en: q.suggest(s, en, lang_of)


def promote(learned):
    """Append each user-taught sentence under its canonical group in tools/anima_phrases.txt."""
    text = open(PHRASES, encoding="utf-8").read()
    added = 0
    for key, canon in learned:
        lang, words = key.split(":", 1)
        head = "= %s %s\n" % (lang, canon)
        if head not in text or re.search(r"(?m)^%s$" % re.escape(words), text):
            continue                                        # no such group here, or already there
        text = text.replace(head, head + words + "\n", 1)
        added += 1
    with open(PHRASES, "w", encoding="utf-8", newline="\n") as f:
        f.write(text)
    return added


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--host", default=os.environ.get("NUCLEO_HOST", "nucleov2.local"))
    ap.add_argument("--promote", action="store_true", help="add the user-taught phrases to tools/anima_phrases.txt")
    ap.add_argument("--top", type=int, default=40)
    a = ap.parse_args()
    tok = open(token_path(), encoding="utf-8").read().strip()

    misses, offered = collections.Counter(), collections.Counter()
    for line in fetch(a.host, "/data/anima/telemetry.ndjson", tok).splitlines():
        try:
            t = json.loads(line)
        except ValueError:
            continue
        q = (t.get("q") or "").strip()
        if not q:
            continue
        if t.get("tier") == "none":
            misses[q] += 1
        elif t.get("intent") == "suggest":
            offered[q] += 1
    learned = []
    for line in fetch(a.host, "/data/anima/phrases.user.tsv", tok).splitlines():
        if "\t" in line:
            k, c = line.split("\t", 1)
            learned.append((k, c))

    sug = suggester()
    print("== Not understood (%d distinct)" % len(misses))
    for q, n in misses.most_common(a.top):
        en = bool(re.search(r"\b(the|what|how|is|are|my|please|you)\b", q.lower()))
        hint = ""
        if sug:
            s, p = sug(q, en)
            hint = "  -> suggester: %s (%.2f)" % (s, p) if s else "  -> suggester: nothing (%.2f)" % p
        print("%3dx  %s%s" % (n, q, hint))
    print("\n== Suggestions offered (%d distinct)" % len(offered))
    for q, n in offered.most_common(a.top):
        print("%3dx  %s" % (n, q))
    print("\n== Taught by the user with a \"sì\" (%d)" % len(learned))
    for k, c in learned:
        print("      %-40s = %s" % (k, c))
    if a.promote and learned:
        n = promote(learned)
        print("\npromoted %d into %s; now run: python tools/gen_anima_phrases.py" % (n, os.path.relpath(PHRASES, gen.ROOT)))


if __name__ == "__main__":
    main()
