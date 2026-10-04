#!/usr/bin/env python3
"""Wikidata IDs for ANIMA's knowledge packs: one entity, the same ID in every language.

    python tools/kb/wikidata.py qids <wiki.zim>     title -> QID for every article of the ZIM (Wikipedia API)
    python tools/kb/wikidata.py sitelinks           QID -> its title in it/en/es/fr/de (Wikidata API)

Both write a TSV cache in tools/kb/.cache and RESUME from it: a run can stop anywhere. Requests are
sequential, 50 titles/IDs each, with maxlag and a User-Agent naming the project, as Wikimedia's API
etiquette asks. akb6.py build reads the caches: the QID goes into each record, the other languages' titles
become cross-language keys ("napoleon" finds "Napoleone Bonaparte"), and a QIDS section lets the device move
to the user's own language when that pack has the entity too.
"""
import json
import os
import sys
import time
import urllib.parse
import urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
CACHE = os.path.join(HERE, ".cache")
UA = "NucleoOS-P4-ANIMA-kb/0.1 (https://github.com/indecenti/NucleoOS-P4; offline knowledge packs)"
LANGS = ("it", "en", "es", "fr", "de")
BATCH = 50


def api(url, params):
    q = urllib.parse.urlencode(dict(params, format="json", formatversion="2", maxlag="5"))
    for attempt in range(40):                                           # ~30 min of patience: Wi-Fi/DNS drops
        try:
            req = urllib.request.Request(f"{url}?{q}", headers={"User-Agent": UA, "Accept-Encoding": "identity"})
            with urllib.request.urlopen(req, timeout=60) as r:
                d = json.loads(r.read().decode())
            if d.get("error", {}).get("code") == "maxlag":
                time.sleep(5 + attempt * 5); continue
            return d
        except Exception as e:                                          # network hiccup: back off and retry
            time.sleep(min(60, 2 + attempt * 3))
            last = e
    raise RuntimeError(f"API failed: {last}")


def zim_titles(zim_path):
    from libzim.reader import Archive
    import re
    secref = re.compile(r"http-equiv=\"refresh\"")
    z = Archive(zim_path)
    lang = {"ita": "it", "eng": "en", "spa": "es", "fra": "fr", "deu": "de"}[z.get_metadata("Language").decode()]
    out = []
    for i in range(z.all_entry_count):
        e = z._get_entry_by_id(i)
        if e.is_redirect or e.path.startswith(("_", "-")) or e.path in ("mainPage", "index"):
            continue
        it = e.get_item()
        if it.mimetype != "text/html":
            continue
        if it.size < 2048 and secref.search(bytes(it.content).decode("utf-8", "replace")):
            continue                                                    # a section page: not an entity
        out.append(e.title)
    return lang, out


def qids(zim_path):
    lang, titles = zim_titles(zim_path)
    path = os.path.join(CACHE, f"qid-{lang}.tsv")
    done = {}
    if os.path.exists(path):
        for line in open(path, encoding="utf-8"):
            t, _, q = line.rstrip("\n").partition("\t")
            done[t] = q
    todo = [t for t in titles if t not in done]
    print(f"{lang}: {len(titles)} articles, {len(done)} cached, {len(todo)} to ask")
    url = f"https://{lang}.wikipedia.org/w/api.php"
    with open(path, "a", encoding="utf-8", newline="\n") as f:
        for k in range(0, len(todo), BATCH):
            chunk = todo[k:k + BATCH]
            d = api(url, {"action": "query", "prop": "pageprops", "ppprop": "wikibase_item", "redirects": "1",
                          "titles": "|".join(chunk)})
            q = d.get("query", {})
            back = {}                                                   # normalized/redirected title -> asked one
            for n in q.get("normalized", []) + q.get("redirects", []):
                back[n["to"]] = back.get(n["from"], n["from"])
            got = {}
            for p in q.get("pages", []):
                qid = p.get("pageprops", {}).get("wikibase_item", "")
                got[back.get(p.get("title", ""), p.get("title", ""))] = qid
            for t in chunk:
                f.write(f"{t}\t{got.get(t, '')}\n")
            f.flush()
            if (k // BATCH) % 50 == 0:
                print(f"  {k + len(chunk)}/{len(todo)}", flush=True)
    print(f"  -> {path}")


def sitelinks():
    ids = set()
    for l in LANGS:
        p = os.path.join(CACHE, f"qid-{l}.tsv")
        if os.path.exists(p):
            ids |= {line.rstrip("\n").split("\t")[1] for line in open(p, encoding="utf-8") if "\tQ" in line}
    path = os.path.join(CACHE, "sitelinks.tsv")
    done = set()
    if os.path.exists(path):
        done = {line.split("\t", 1)[0] for line in open(path, encoding="utf-8")}
    todo = sorted(ids - done)
    print(f"{len(ids)} QIDs, {len(done)} cached, {len(todo)} to ask")
    with open(path, "a", encoding="utf-8", newline="\n") as f:
        for k in range(0, len(todo), BATCH):
            chunk = todo[k:k + BATCH]
            d = api("https://www.wikidata.org/w/api.php",
                    {"action": "wbgetentities", "ids": "|".join(chunk), "props": "sitelinks",
                     "sitefilter": "|".join(f"{l}wiki" for l in LANGS)})
            for q in chunk:
                sl = d.get("entities", {}).get(q, {}).get("sitelinks", {})
                f.write(q + "\t" + "\t".join(sl.get(f"{l}wiki", {}).get("title", "") for l in LANGS) + "\n")
            f.flush()
            if (k // BATCH) % 50 == 0:
                print(f"  {k + len(chunk)}/{len(todo)}", flush=True)
    print(f"  -> {path}")


if __name__ == "__main__":
    if len(sys.argv) >= 3 and sys.argv[1] == "qids":
        qids(sys.argv[2])
    elif len(sys.argv) == 2 and sys.argv[1] == "sitelinks":
        sitelinks()
    else:
        sys.exit(__doc__)
