"""Ranking inside a language: download counts alone are skewed (bots, school lists), so the canon leads.

score = downloads/pageviews (pop) + SITELINK_WEIGHT x Wikipedia/Wikisource editions that have a page
about the work (Wikidata sitelinks, a language-neutral measure of fame) + a fixed lift for the
hand-picked canon in data/canon.json. Curation thresholds keep using raw downloads.
"""
from .common import CACHE, DATA, Http, load_json, save_json
from .wikidata import API as API_WIKIDATA
from .describe import norm_key

SITELINK_WEIGHT = 250
CANON_LIFT = 100_000
CACHE_FILE = CACHE / "wikidata-sitelinks.json"


def sitelinks(qids, log=print):
    cache = load_json(CACHE_FILE, {}) or {}
    todo = [q for q in dict.fromkeys(qids) if q and q not in cache]
    http = Http(delay=1.0) if todo else None
    for i in range(0, len(todo), 50):
        r = http.get(API_WIKIDATA, params={"action": "wbgetentities", "ids": "|".join(todo[i:i + 50]),
                                           "props": "sitelinks", "format": "json"})
        js = r.json()
        for q, ent in js.get("entities", {}).items():
            cache[q] = len(ent.get("sitelinks", {}))
        for q in todo[i:i + 50]:
            cache.setdefault(q, 0)
        if i % 1000 == 0:
            save_json(CACHE_FILE, cache, compact=True)
            log("  sitelinks %d/%d" % (i, len(todo)))
    save_json(CACHE_FILE, cache, compact=True)
    return cache


def _canon():
    js = load_json(DATA / "canon.json", {}) or {}
    return {lang: [(norm_key(t).split(), norm_key(a)) for t, a in items]
            for lang, items in js.items() if not lang.startswith("_")}


def apply(rows, log=print):
    """Rewrite r["pop"] with the ranking score (raw count kept in r["_downloads"])."""
    links = sitelinks([r.get("_qid") for r in rows if r.get("_qid")], log)
    canon = _canon()
    lifted = 0
    for r in rows:
        r["_downloads"] = r.get("pop") or 0
        score = r["_downloads"] + SITELINK_WEIGHT * links.get(r.get("_qid"), 0)
        title = norm_key(r["t"]).split()
        author = norm_key(r.get("a") or "")
        for i, (words, surname) in enumerate(canon.get(r["lang"], [])):
            if all(w in title for w in words) and (not surname or surname in author):
                score += CANON_LIFT - 100 * i
                lifted += 1
                break
        r["pop"] = score
    log("rank: %d rows with Wikidata fame, %d canon matches" % (sum(1 for r in rows if r.get("_qid")), lifted))
    return rows
