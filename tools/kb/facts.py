#!/usr/bin/env python3
"""Wikidata facts for ANIMA's knowledge packs (CC0): exact answers, the same in every language.

    python tools/kb/facts.py fetch      the facts of every entity in .cache/sitelinks.tsv (Wikidata SPARQL)
    python tools/kb/facts.py labels     the names of the values (places, currencies, people) in it/en/es/fr/de
    python tools/kb/facts.py extra      the properties added later (EXTRA: chemistry), same cache, resumable
    python tools/kb/facts.py show Q937  what a pack will carry for one entity

Both steps cache to tools/kb/.cache (facts-raw.tsv, facts-labels.tsv) and RESUME: a run can stop anywhere.
Queries are sequential, 400 entities each, with a User-Agent naming the project (WDQS etiquette).
akb6.py build reads the caches and stores each entity's facts in its record, values already named in the
pack's language (fact_line below); the device answers "quando è nato X", "quanti abitanti ha X", ...
"""
import collections
import json
import os
import sys
import time
import urllib.parse
import urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
CACHE = os.path.join(HERE, ".cache")
RAW = os.path.join(CACHE, "facts-raw.tsv")
LABELS = os.path.join(CACHE, "facts-labels.tsv")
UA = "NucleoOS-P4-ANIMA-kb/0.1 (https://github.com/indecenti/NucleoOS-P4; offline knowledge packs)"
SPARQL = "https://query.wikidata.org/sparql"
LANGS = ("it", "en", "es", "fr", "de")
BATCH = 400

# key on the device -> Wikidata property. Dates are read with their precision (psv), the rest as truthy values.
DATES = {"born": "P569", "died": "P570", "founded": "P571"}
VALUES = {"birthplace": "P19", "deathplace": "P20", "capital": "P36", "population": "P1082", "area": "P2046",
          "currency": "P38", "language": "P37", "continent": "P30", "country": "P17", "author": "P50",
          "director": "P57", "composer": "P86", "elevation": "P2044", "occupation": "P106",
          "gender": "P21"}                                # "è nato" / "è nata", "né" / "née"
# Added after the first fetch: asked by `extra` for every entity (most have none), written to the same cache.
EXTRA = {"formula": "P274", "symbol": "P246", "atomic_number": "P1086"}
MAXVAL = {"occupation": 3, "language": 3, "author": 3, "director": 2, "composer": 2, "currency": 2, "continent": 2}


def sparql(q):
    data = urllib.parse.urlencode({"query": q, "format": "json"}).encode()
    for attempt in range(40):
        try:
            req = urllib.request.Request(SPARQL, data=data, headers={
                "User-Agent": UA, "Accept": "application/sparql-results+json",
                "Content-Type": "application/x-www-form-urlencoded"})
            with urllib.request.urlopen(req, timeout=120) as r:
                return json.loads(r.read().decode())["results"]["bindings"]
        except urllib.error.HTTPError as e:
            if e.code in (429, 500, 502, 503, 504):
                time.sleep(min(120, 10 + attempt * 10)); continue
            raise
        except Exception:
            time.sleep(min(60, 3 + attempt * 3))
    raise RuntimeError("SPARQL kept failing")


def qid_of(uri):
    return uri.rsplit("/", 1)[-1]


def all_qids():
    p = os.path.join(CACHE, "sitelinks.tsv")
    return [line.split("\t", 1)[0] for line in open(p, encoding="utf-8") if line.startswith("Q")]


def fetch():
    done = set()
    if os.path.exists(RAW):
        for line in open(RAW, encoding="utf-8"):
            if line.startswith("#done\t"):
                done.update(line.rstrip("\n").split("\t")[1].split(","))
    todo = [q for q in all_qids() if q not in done]
    print(f"{len(done)} entities cached, {len(todo)} to ask")
    pv = " ".join(f"wdt:{p}" for p in VALUES.values())
    prop_key = {p: k for k, p in {**VALUES, **DATES}.items()}
    with open(RAW, "a", encoding="utf-8", newline="\n") as f:
        for k in range(0, len(todo), BATCH):
            chunk = todo[k:k + BATCH]
            ids = " ".join(f"wd:{q}" for q in chunk)
            rows = sparql(f"SELECT ?item ?p ?v WHERE {{ VALUES ?item {{ {ids} }} VALUES ?p {{ {pv} }} ?item ?p ?v }}")
            for b in rows:
                v = b["v"]["value"]
                v = qid_of(v) if b["v"]["type"] == "uri" else v
                f.write(f"{qid_of(b['item']['value'])}\t{prop_key[qid_of(b['p']['value'])]}\t{v}\n")
            dv = " ".join(f"(wd:{p} p:{p} psv:{p})" for p in DATES.values())
            rows = sparql(f"""SELECT ?item ?p ?t ?prec WHERE {{ VALUES ?item {{ {ids} }} VALUES (?p ?ps ?psv) {{ {dv} }}
                ?item ?ps ?st . ?st ?psv ?tv . ?tv wikibase:timeValue ?t ; wikibase:timePrecision ?prec .
                ?st wikibase:rank ?r FILTER(?r != wikibase:DeprecatedRank) }}""")
            for b in rows:
                f.write(f"{qid_of(b['item']['value'])}\t{prop_key[qid_of(b['p']['value'])]}\t{b['t']['value']}\t{b['prec']['value']}\n")
            f.write("#done\t" + ",".join(chunk) + "\n")
            f.flush()
            if (k // BATCH) % 25 == 0:
                print(f"  {k + len(chunk)}/{len(todo)}", flush=True)
    print(f"  -> {RAW}")


def extra():
    done = set()
    if os.path.exists(RAW):
        for line in open(RAW, encoding="utf-8"):
            if line.startswith("#extra\t"):
                done.update(line.rstrip("\n").split("\t")[1].split(","))
    todo = [q for q in all_qids() if q not in done]
    print(f"{len(done)} entities cached, {len(todo)} to ask for {', '.join(EXTRA)}")
    pv = " ".join(f"wdt:{p}" for p in EXTRA.values())
    prop_key = {p: k for k, p in EXTRA.items()}
    with open(RAW, "a", encoding="utf-8", newline="\n") as f:
        for k in range(0, len(todo), BATCH):
            chunk = todo[k:k + BATCH]
            ids = " ".join(f"wd:{q}" for q in chunk)
            rows = sparql(f"SELECT ?item ?p ?v WHERE {{ VALUES ?item {{ {ids} }} VALUES ?p {{ {pv} }} ?item ?p ?v }}")
            for b in rows:
                v = b["v"]["value"].replace("\t", " ")
                f.write(f"{qid_of(b['item']['value'])}\t{prop_key[qid_of(b['p']['value'])]}\t{v}\n")
            f.write("#extra\t" + ",".join(chunk) + "\n")
            f.flush()
            if (k // BATCH) % 25 == 0:
                print(f"  {k + len(chunk)}/{len(todo)}", flush=True)
    print(f"  -> {RAW}")


def load_raw():
    """QID -> {key: [values]}; dates keep the most precise value ("1769-08-15" over "1769")."""
    facts = collections.defaultdict(lambda: collections.defaultdict(list))
    best = {}
    for line in open(RAW, encoding="utf-8"):
        if line.startswith("#"):
            continue
        f = line.rstrip("\n").split("\t")
        if len(f) == 4:                                            # a date with its precision
            q, key, t, prec = f
            prec = int(prec)
            if prec < 9:                                           # decade/century: not an answer
                continue
            if prec > best.get((q, key), -1):
                best[(q, key)] = prec
                d = t.lstrip("+")
                neg = d.startswith("-")
                y, m, dd = d.lstrip("-").split("T")[0].split("-")
                val = ("-" if neg else "") + str(int(y)) + ("-" + m if prec >= 10 else "") + ("-" + dd if prec >= 11 else "")
                facts[q][key] = [val]
        elif len(f) == 3:
            q, key, v = f
            if v not in facts[q][key] and len(facts[q][key]) < MAXVAL.get(key, 1):
                facts[q][key].append(v)
    return facts


def ask_names(chunk):
    """QID -> {lang: name}. Wikidata keeps many names only as "mul" (the same in every language: "Victor
    Hugo"), so "mul" fills each language that has no label of its own."""
    ids = " ".join(f"wd:{q}" for q in chunk)
    rows = sparql(f"""SELECT ?x ?l WHERE {{ VALUES ?x {{ {ids} }} ?x rdfs:label ?l
        FILTER(LANG(?l) IN ("it","en","es","fr","de","mul")) }}""")
    names = collections.defaultdict(dict)
    for b in rows:
        names[qid_of(b["x"]["value"])][b["l"]["xml:lang"]] = b["l"]["value"].replace("\t", " ")
    for q, n in names.items():
        if "mul" in n:
            for l in LANGS:
                n.setdefault(l, n["mul"])
    return names


def fill():
    """Re-ask the cached names with a gap (the cache predates the "mul" fallback) and rewrite the cache."""
    rows = [line.rstrip("\n").split("\t") for line in open(LABELS, encoding="utf-8")]
    gaps = [r[0] for r in rows if len(r) == 6 and "" in r[1:]]
    print(f"{len(gaps)} names with a gap")
    got = {}
    for k in range(0, len(gaps), BATCH):
        got.update(ask_names(gaps[k:k + BATCH]))
    fixed = 0
    with open(LABELS + ".tmp", "w", encoding="utf-8", newline="\n") as f:
        for r in rows:
            if len(r) == 6 and r[0] in got:
                new = [r[1 + i] or got[r[0]].get(l, "") for i, l in enumerate(LANGS)]
                fixed += new != r[1:]
                r = [r[0]] + new
            f.write("\t".join(r) + "\n")
    os.replace(LABELS + ".tmp", LABELS)
    print(f"  {fixed} names completed -> {LABELS}")


def labels():
    facts = load_raw()
    want = {v for fs in facts.values() for vals in fs.values() for v in vals if v.startswith("Q") and v[1:].isdigit()}
    have = set()
    if os.path.exists(LABELS):
        have = {line.split("\t", 1)[0] for line in open(LABELS, encoding="utf-8")}
    todo = sorted(want - have)
    print(f"{len(want)} values to name, {len(have)} cached, {len(todo)} to ask")
    with open(LABELS, "a", encoding="utf-8", newline="\n") as f:
        for k in range(0, len(todo), BATCH):
            chunk = todo[k:k + BATCH]
            ids = " ".join(f"wd:{q}" for q in chunk)
            names = ask_names(chunk)
            for q in chunk:
                f.write(q + "\t" + "\t".join(names[q].get(l, "") for l in LANGS) + "\n")
            f.flush()
            if (k // BATCH) % 25 == 0:
                print(f"  {k + len(chunk)}/{len(todo)}", flush=True)
    print(f"  -> {LABELS}")


def load_labels():
    out = {}
    if os.path.exists(LABELS):
        for line in open(LABELS, encoding="utf-8"):
            f = line.rstrip("\n").split("\t")
            if len(f) == 6:
                out[f[0]] = dict(zip(LANGS, f[1:]))
    return out


def fact_line(fs, lang, names):
    """The record field: "born=1879-03-14|birthplace=Ulma|population=123802000" in the pack's language.
    A value without a name in that language falls back to English; with none at all it is left out."""
    parts = []
    for key, vals in fs.items():
        out = []
        if key == "gender":                                       # grammar only: m / f, else nothing
            g = {"Q6581097": "m", "Q2449503": "m", "Q6581072": "f", "Q1052281": "f"}.get(vals[0], "")
            if g:
                parts.append(f"gender={g}")
            continue
        for v in vals:
            if v.startswith("Q") and v[1:].isdigit():
                n = names.get(v, {})
                v = n.get(lang) or n.get("en") or ""
                v = v.replace("|", "/").replace("=", "-").replace(";", ",")
            if v:
                out.append(v)
        if out:
            parts.append(f"{key}={';'.join(out)}")
    return "|".join(parts)


if __name__ == "__main__":
    cmd = sys.argv[1] if len(sys.argv) > 1 else ""
    if cmd == "fetch":
        fetch()
    elif cmd == "labels":
        labels()
    elif cmd == "extra":
        extra()
    elif cmd == "fill":
        fill()
    elif cmd == "show" and len(sys.argv) == 3:
        fs, nm = load_raw(), load_labels()
        for l in LANGS:
            print(l, fact_line(fs.get(sys.argv[2], {}), l, nm))
    else:
        sys.exit(__doc__)
