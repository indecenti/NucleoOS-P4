"""Fill missing dates from Wikidata (CC0): author birth/death (P569/P570), work publication (P577).

Lookups go by Wikipedia article URL (the sources already link authors and works to Wikipedia), 50
titles per wbgetentities request, cached in .cache/wikidata.json so reruns cost nothing.
"""
import re
from urllib.parse import unquote

from .common import CACHE, LIVING_LANGS, Http, load_json, save_json

CACHE_FILE = CACHE / "wikidata.json"
API = "https://www.wikidata.org/w/api.php"


def _split(url):
    m = re.match(r"https?://([a-z\-]+)\.wikipedia\.org/wiki/(.+)$", url or "")
    if not m:
        return None
    return m.group(1).replace("-", "_") + "wiki", unquote(m.group(2)).replace("_", " ")


def _year(claims, prop):
    ys = []
    for c in claims.get(prop, []):
        try:
            t = c["mainsnak"]["datavalue"]["value"]["time"]      # '+1817-07-18T00:00:00Z'
            prec = c["mainsnak"]["datavalue"]["value"].get("precision", 11)
        except (KeyError, TypeError):
            continue
        if prec < 9:            # decade/century precision: not a verifiable year
            continue
        m = re.match(r"([+-])(\d+)-", t)
        if m:
            y = int(m.group(2)) * (-1 if m.group(1) == "-" else 1)
            ys.append(y)
    return ys


class Wikidata:
    def __init__(self):
        self.cache = load_json(CACHE_FILE, {}) or {}
        self.http = None

    def lookup(self, urls):
        """urls -> {url: {"qid", "birth", "death", "pub"}} (None values when unknown)."""
        todo = {}
        for u in urls:
            hit = self.cache.get(u)
            if hit is not None and (not hit or "desc" in hit):
                continue
            s = _split(u)
            if s:
                todo.setdefault(s[0], {})[s[1]] = u
        for site, titles in todo.items():
            items = list(titles.items())
            for i in range(0, len(items), 50):
                chunk = dict(items[i:i + 50])
                self._fetch(site, chunk)
            save_json(CACHE_FILE, self.cache, compact=True)
        return {u: self.cache.get(u) for u in urls}

    def _fetch(self, site, chunk):
        if self.http is None:
            self.http = Http(delay=1.0)
        r = self.http.get(API, params={
            "action": "wbgetentities", "sites": site, "titles": "|".join(chunk), "props": "claims|sitelinks|descriptions",
            "sitefilter": site, "format": "json", "redirects": "yes"})
        r.raise_for_status()
        js = r.json()
        if "error" in js:
            raise RuntimeError("wikidata: %s" % js["error"].get("info"))
        found = {}
        for qid, ent in js.get("entities", {}).items():
            sl = ent.get("sitelinks", {}).get(site, {}).get("title")
            if not sl or qid.startswith("-"):
                continue
            cl = ent.get("claims", {})
            d = _year(cl, "P570")
            b = _year(cl, "P569")
            p = _year(cl, "P577") or _year(cl, "P571")
            found[sl] = {"qid": qid, "birth": min(b) if b else None, "death": max(d) if d else None,
                         "pub": min(p) if p else None,
                         "human": any(c.get("mainsnak", {}).get("datavalue", {}).get("value", {}).get("id") == "Q5"
                                      for c in cl.get("P31", [])),
                         "desc": _descs(ent)}
        norm = {n["to"]: n["from"] for n in js.get("normalized", [])} if js.get("normalized") else {}
        for title, url in chunk.items():
            hit = found.get(title)
            if hit is None:
                for k, v in found.items():
                    if norm.get(k) == title or k.replace("_", " ") == title:
                        hit = v
                        break
            self.cache[url] = hit or {}


def _descs(ent):
    """Short Wikidata descriptions (CC0) in the languages we ship, e.g. it: 'poema di Dante Alighieri'."""
    return {k: v["value"] for k, v in ent.get("descriptions", {}).items() if k in LIVING_LANGS}


def enrich_pg(meta, log=print):
    """Fill agent death/birth and book pub year (y_wd) from Wikidata where the RDF has none."""
    wd = Wikidata()
    agent_urls, book_urls = set(), {}
    for b in meta:
        for a in b["agents"]:
            if a["death"] is None and a.get("wiki"):
                agent_urls.add(a["wiki"][0])
        for d in b.get("wiki_desc", []):
            m = re.search(r"Wikipedia page about this book: (\S+)", d)
            if m:
                book_urls[b["id"]] = m.group(1)
    log("wikidata: %d authors, %d works to resolve" % (len(agent_urls), len(book_urls)))
    res = wd.lookup(sorted(agent_urls | set(book_urls.values())))
    na = nb = 0
    for b in meta:
        for a in b["agents"]:
            if a["death"] is None and a.get("wiki"):
                r = res.get(a["wiki"][0]) or {}
                if r.get("human") and r.get("death") is not None:
                    a["death"] = r["death"]
                    a["birth"] = a["birth"] if a["birth"] is not None else r.get("birth")
                    a["dates_from"] = "wikidata"
                    na += 1
        u = book_urls.get(b["id"])
        if u:
            r = res.get(u) or {}
            if r.get("human"):
                continue
            if r.get("pub") and b.get("y906") is None and b.get("y260") is None:
                b["y_wd"] = r["pub"]
                nb += 1
            if r.get("desc"):
                b["wd_desc"] = r["desc"]
            if r.get("qid"):
                b["wd_qid"] = r["qid"]
    log("wikidata: filled %d author deaths, %d publication years" % (na, nb))
    return meta


ENT_CACHE = CACHE / "wikidata-entities.json"


def _ids(claims, prop):
    out = []
    for c in claims.get(prop, []):
        try:
            out.append(c["mainsnak"]["datavalue"]["value"]["id"])
        except (KeyError, TypeError):
            pass
    return out


class Entities:
    """Small cached view of Wikidata items: authors/translators/illustrators, dates, edition-of, label."""

    def __init__(self):
        self.cache = load_json(ENT_CACHE, {}) or {}
        self.http = None

    def get(self, ids):
        todo = [i for i in dict.fromkeys(ids) if i and i not in self.cache]
        for k in range(0, len(todo), 50):
            if self.http is None:
                self.http = Http(delay=1.0)
            r = self.http.get(API, params={"action": "wbgetentities", "ids": "|".join(todo[k:k + 50]),
                                           "props": "claims|labels|descriptions", "format": "json"})
            js = r.json()
            if "error" in js:
                raise RuntimeError("wikidata: %s" % js["error"].get("info"))
            for qid, ent in js.get("entities", {}).items():
                cl = ent.get("claims", {})
                d, b = _year(cl, "P570"), _year(cl, "P569")
                p = _year(cl, "P577") or _year(cl, "P571")
                self.cache[qid] = {
                    "birth": min(b) if b else None, "death": max(d) if d else None, "pub": min(p) if p else None,
                    "authors": _ids(cl, "P50"), "translators": _ids(cl, "P655"), "illustrators": _ids(cl, "P110"),
                    "editors": _ids(cl, "P98"), "edition_of": _ids(cl, "P629"),
                    "human": "Q5" in _ids(cl, "P31"),
                    "labels": {k: v["value"] for k, v in ent.get("labels", {}).items()
                               if k in ("it", "en", "fr", "de", "es", "pt", "nl")},
                    "desc": _descs(ent),
                }
            save_json(ENT_CACHE, self.cache, compact=True)
        return {i: self.cache.get(i) for i in ids if i}
