"""Wikisource selection + build (see src_mw for how pages become EPUBs).

  scan(lang)  -> .cache/mw/ws-<lang>-scan.json  (roots in the quality category, sizes, metadata, rights)
  build(lang) -> EPUBs in .cache/mw/epub/ + .cache/candidates-ws.json (merged over languages)
"""
import re
from collections import Counter, defaultdict
from urllib.parse import quote

from . import describe, rights, src_mw
from .common import CACHE, Http, load_json, save_json
from .wikidata import Entities

MIN_WIKITEXT = 30_000      # a root page this long is a work by itself
MIN_SUBPAGES = 3
MIN_TRANSCLUDED = 10
SCAN = lambda lang: src_mw.MW_DIR / ("ws-%s-scan.json" % lang)


def scan(lang, log=print):
    conf = src_mw.WS[lang]
    site = conf["site"]
    mw = src_mw.MW()
    state = load_json(SCAN(lang), {}) or {}
    if "roots" not in state:
        roots = []
        for cat in conf["cats"]:
            roots += [t for t in mw.members(site, cat) if "/" not in t]
        state["roots"] = sorted(set(roots))
        save_json(SCAN(lang), state, compact=True)
    roots = state["roots"]
    log("ws %s: %d roots" % (lang, len(roots)))
    if "info" not in state:
        state["info"] = mw.page_info(site, roots)
        save_json(SCAN(lang), state, compact=True)
    info = state["info"]
    sizes = state.setdefault("sizes", {})
    todo = [t for t in roots if t not in sizes and info.get(t, {}).get("length", 0) < MIN_WIKITEXT]
    for i, t in enumerate(todo):
        n, s = mw.subpage_counts(site, [t])[t]
        sizes[t] = [n, s]
        if i % 200 == 0:
            save_json(SCAN(lang), state, compact=True)
            log("  sizes %d/%d" % (i, len(todo)))
    save_json(SCAN(lang), state, compact=True)
    # Wikitext size says little: proofread texts are <pages index=.../> calls of a few bytes. A work is a
    # candidate when it has chapters (>= MIN_SUBPAGES subpages), a long page of its own, or transcludes
    # >= MIN_TRANSCLUDED proofread pages. The real text length is checked after the EPUB is built.
    if "transcluded" not in state:
        state["transcluded"] = transcluded_pages(mw, site, [t for t in roots if (sizes.get(t) or [0, 0])[0] < MIN_SUBPAGES
                                                             and info.get(t, {}).get("length", 0) < MIN_WIKITEXT], log)
        save_json(SCAN(lang), state, compact=True)
    tr = state["transcluded"]
    big = [t for t in roots if (sizes.get(t) or [0, 0])[0] >= MIN_SUBPAGES or info.get(t, {}).get("length", 0) >= MIN_WIKITEXT
           or tr.get(t, 0) >= MIN_TRANSCLUDED]
    log("ws %s: %d candidate works" % (lang, len(big)))
    state["big"] = big
    save_json(SCAN(lang), state, compact=True)
    return state


def transcluded_pages(mw, site, titles, log=print):
    """{title: number of ProofreadPage pages it transcludes} (prop=templates in the Page namespace)."""
    js = mw.api(site, action="query", meta="proofreadinfo", piprop="namespaces")
    ns = js.get("query", {}).get("proofreadnamespaces", {}).get("page", {}).get("id")
    out = {}
    if ns is None:
        return out
    for i in range(0, len(titles), 50):
        cont = {}
        while True:
            js = mw.api(site, action="query", prop="templates", tlnamespace=ns, tllimit="max",
                        titles="|".join(titles[i:i + 50]), **cont)
            for p in js.get("query", {}).get("pages", []):
                out[p["title"]] = out.get(p["title"], 0) + len(p.get("templates", []))
            if "continue" not in js:
                break
            cont = js["continue"]
        if i % 1000 == 0:
            log("  transcluded %d/%d" % (i, len(titles)))
    return out


def _author_from_categories(mw, site, conf, cats):
    """it.wikisource: 'Testi di Giovanni Verga' -> page 'Autore:Giovanni Verga' -> its Wikidata item."""
    if not conf.get("author_cat") or not conf.get("author_ns"):
        return []
    out = []
    for c in cats:
        m = re.match(conf["author_cat"], "Categoria:" + c.replace("_", " ")) or re.match(conf["author_cat"], c.replace("_", " "))
        if m:
            name = m.group(1)
            inf = mw.page_info(site, ["%s:%s" % (conf["author_ns"], name)])
            q = next(iter(inf.values()), {}).get("qid") if inf else None
            out.append((name, q))
    return out


def metadata(lang, log=print):
    """Rights + display metadata for every big work. Returns (records, excluded)."""
    conf = src_mw.WS[lang]
    site = conf["site"]
    mw = src_mw.MW()
    ents = Entities()
    state = load_json(SCAN(lang))
    recs, excl = [], defaultdict(Counter)
    for i, t in enumerate(state["big"]):
        if i % 200 == 0:
            log("  ws %s meta %d/%d" % (lang, i, len(state["big"])))
        page = mw.parse(site, t)
        if not page:
            excl[lang]["ws:missing"] += 1
            continue
        cats = [c.replace("_", " ") for c in page["cats"]]
        qid = state["info"].get(t, {}).get("qid") or (page.get("props") or {}).get("wikibase_item")
        agents, years, names = [], [], []
        work_qid = qid
        if qid:
            e = ents.get([qid]).get(qid) or {}
            work_qid = (e.get("edition_of") or [qid])[0]
            work = ents.get(e.get("edition_of", [])[:1])
            people = [(q, "aut") for q in e.get("authors", [])] + [(q, "trl") for q in e.get("translators", [])] + \
                     [(q, "ill") for q in e.get("illustrators", [])] + [(q, "edt") for q in e.get("editors", [])]
            for w in work.values():
                if w:
                    if not e.get("authors"):
                        people += [(q, "aut") for q in w.get("authors", [])]
                    if w.get("pub"):
                        years.append(w["pub"])
            if e.get("pub"):
                years = [e["pub"]]          # the edition's own date wins (translations!)
            pe = ents.get([q for q, _ in people])
            for q, role in people:
                p = pe.get(q) or {}
                agents.append({"name": (p.get("labels") or {}).get(lang) or (p.get("labels") or {}).get("en") or q,
                               "role": role, "birth": p.get("birth"), "death": p.get("death")})
                if role == "aut":
                    names.append(agents[-1]["name"])
        if not agents:
            for name, aq in _author_from_categories(mw, site, conf, page["cats"]):
                p = ents.get([aq]).get(aq) or {} if aq else {}
                agents.append({"name": name, "role": "aut", "birth": p.get("birth"), "death": p.get("death")})
                names.append(name)
        if not years:
            for c in cats:
                m = re.search(conf["year"], c) or re.search(conf["year"], "Categoria:" + c)
                if m:
                    years.append(int(m.group(1)))
        y = min(years) if years else None
        ok, why = rights.check_public_domain(agents, y, us_pd=True)
        if not ok:
            excl[lang]["rights:" + why] += 1
            continue
        a0 = next((a for a in agents if a["role"] == "aut"), agents[0])
        author = " & ".join(names[:3])
        ad = describe.dates(a0.get("birth"), a0.get("death"), lang) if rights.name_kind(a0["name"]) == "person" else ""
        c = src_mw.ws_categories(cats)
        title = re.sub(r"<[^>]+>", "", page["title"])
        pageid = state["info"].get(t, {}).get("pageid")
        url = "https://%s/wiki/%s" % (site, quote(t.replace(" ", "_")))
        rid = "ws%s-%s" % (lang, hashlib_id(site, t))
        recs.append({"id": rid, "lang": lang, "t": describe.clean_title(title), "a": author, "ad": ad, "y": y,
                     "c": c, "src": "ws", "lic": "cc-by-sa", "pop": 0,
                     "d": describe.description(lang, author, ad, c, y), "u2": url,
                     "_site": site, "_title": t, "_file": str(src_mw.EPUB_DIR / (rid + ".epub")), "_qid": work_qid,
                     "_attrib": "Wikisource (%s), trascrizione CC BY-SA 4.0" % site})
    return recs, excl


def hashlib_id(site, title):
    import hashlib
    return hashlib.sha1(("%s|%s" % (site, title)).encode("utf-8")).hexdigest()[:10]


def build(langs, log=print):
    mw = src_mw.MW()
    http = Http(delay=0.2)
    total = 0
    for lang in langs:
        recs, excl = metadata(lang, log)
        all_recs, all_excl = [], defaultdict(Counter)
        for k, v in excl.items():
            all_excl[k].update(v)
        for i, r in enumerate(recs):
            dest = src_mw.EPUB_DIR / (r["id"] + ".epub")
            if not dest.exists():
                try:
                    res = src_mw.build_epub(mw, r["_site"], r["_title"], lang, r, dest,
                                            src_mw.ws_license(lang, r["_site"], r["t"], r["u2"]))
                except Exception as e:     # one broken page must not stop the run
                    log("ws build failed %s: %s" % (r["_title"], e))
                    res = None
                if not res or res.get("too_short"):
                    all_excl[lang]["quality:too_short" if res else "ws:build_failed"] += 1
                    continue
            r["pop"] = src_mw.pageviews(http, r["_site"].replace(".org", ""), r["_title"]) // 12
            all_recs.append(r)
            if i % 100 == 0:
                log("  ws %s build %d/%d" % (lang, i, len(recs)))
        # one candidates file per language: several languages can build at the same time
        save_json(CACHE / ("candidates-ws-%s.json" % lang), {"records": all_recs, "excluded": all_excl})
        log("ws %s candidates: %d" % (lang, len(all_recs)))
        total += len(all_recs)
    log("ws candidates: %d" % total)
