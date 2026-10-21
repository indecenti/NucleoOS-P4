#!/usr/bin/env python3
"""NucleoOS free-books catalog pipeline (docs/BOOKS_PLAN.md, phase F4).

  python books.py pg-meta            # Gutenberg catalog -> .cache/candidates-pg.json
  python books.py pg-fetch [--langs it,en]   # EPUBs from the PG rsync mirror (WSL rsync)
  python books.py se-meta | se-fetch # Standard Ebooks (built from their CC0 GitHub sources)
  python books.py ws-scan --langs it,fr   # Wikisource: list complete texts, sizes
  python books.py ws-build --langs it,fr  # Wikisource: rights + EPUBs built from the MediaWiki API
  python books.py wb-build           # Wikibooks manuals (data/wikibooks.json)
  python books.py mn-fetch           # hand-curated manuals (data/manuals.json)
  python books.py comics [--langs ...] # Pepper&Carrot comics (CC BY 4.0)
  python books.py build              # validate, dedup, assign releases, write out/books + reports
  python books.py publish --langs it --store D:
v_books_store   # F5: Releases + books/ on Pages (user's OK!)
  python books.py all                # everything above, in order

Nothing here publishes: out/ is ready for F5 (GitHub Releases + Pages books/), which needs the user's OK.
"""
import argparse
import sys
from collections import Counter

from bk import catalog, curate, dedup, select_pg, src_pg, wikidata
from bk.common import CACHE, load_json, save_json

CAND = {"pg": CACHE / "candidates-pg.json", "se": CACHE / "candidates-se.json",
        "mn": CACHE / "candidates-mn.json", "ws": CACHE / "candidates-ws.json",
        "wb": CACHE / "candidates-wb.json", "comics": CACHE / "candidates-comics.json"}


def cmd_pg_meta(a):
    src_pg.fetch_meta(force=a.force)
    meta = src_pg.build_meta(force=a.force)
    wikidata.enrich_pg(meta)
    recs, excl = select_pg.candidates(meta)
    save_json(CAND["pg"], {"records": recs, "excluded": excl})
    print("pg candidates:", len(recs), Counter(r["lang"] for r in recs).most_common(10))


def _langs(a):
    return set(a.langs.split(",")) if getattr(a, "langs", None) else None


def cmd_pg_fetch(a):
    js = load_json(CAND["pg"])
    recs = js["records"]
    langs = _langs(a)
    kept, _ = dedup.dedupe([dict(r) for r in recs])
    want = [r for r in kept if curate.keep(r)[0] and (not langs or r["lang"] in langs)]
    want.sort(key=lambda r: (r["lang"] not in ("it", "en", "de", "fr", "es"), -(r.get("pop") or 0)))
    print("pg-fetch: %d books (%.1f GB estimated)" % (len(want), sum(r["_est_size"] or 0 for r in want) / 1e9))
    step = 100000
    for i in range(0, len(want), step):
        part = want[i:i + step]
        got = src_pg.rsync_epubs([(r["_pg"], r["_edition"]) for r in part])
        print("  batch %d-%d: %d new files" % (i, i + len(part), got), flush=True)


def load_candidates():
    """Every .cache/candidates-*.json (one per source, Wikisource one per language)."""
    recs, excl = [], {}
    for p in sorted(CACHE.glob("candidates-*.json")):
        js = load_json(p)
        if not js or "test" in p.name:
            continue
        for r in js["records"]:
            if r["src"] == "pg":
                r["_file"] = str(src_pg.epub_path(r["_pg"], r["_edition"]))
            recs.append(r)
        for lang, c in js.get("excluded", {}).items():
            excl.setdefault(lang, Counter()).update(c)
    return recs, excl


def cmd_build(a):
    recs, excl = load_candidates()
    langs = _langs(a)
    if langs:
        recs = [r for r in recs if r["lang"] in langs]
        excl = {k: v for k, v in excl.items() if k in langs}
    catalog.build(recs, excl)


def cmd_se_meta(a):
    from bk import src_se
    src_se.meta()


def cmd_se_fetch(a):
    from bk import src_se
    src_se.fetch()


def cmd_mn_fetch(a):
    from bk import src_manuals
    src_manuals.build()


def cmd_ws_scan(a):
    from bk import src_ws
    for lang in sorted(_langs(a) or {"it", "fr", "es"}):
        src_ws.scan(lang)


def cmd_ws_build(a):
    from bk import src_ws
    src_ws.build(sorted(_langs(a) or {"it", "fr", "es"}))


def cmd_wb_build(a):
    from bk import src_wb
    src_wb.build()


def cmd_comics(a):
    from bk import src_comics
    src_comics.build(_langs(a))


def cmd_publish(a):
    from bk import publish
    if not a.store:
        sys.exit("--store <sparse clone of indecenti/nucleoos-p4-store> is required")
    langs = a.langs.split(",") if a.langs else None
    if not langs:
        sys.exit("--langs is required (publishing is per language)")
    publish.publish(a.store, langs)


def cmd_all(a):
    cmd_pg_meta(a)
    cmd_se_meta(a)
    cmd_pg_fetch(a)
    cmd_se_fetch(a)
    cmd_ws_scan(a)
    cmd_ws_build(a)
    cmd_wb_build(a)
    cmd_mn_fetch(a)
    cmd_comics(a)
    cmd_build(a)


def main(argv=None):
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = p.add_subparsers(dest="cmd", required=True)
    for name, fn in (("pg-meta", cmd_pg_meta), ("pg-fetch", cmd_pg_fetch), ("se-meta", cmd_se_meta),
                     ("se-fetch", cmd_se_fetch), ("ws-scan", cmd_ws_scan), ("ws-build", cmd_ws_build),
                     ("wb-build", cmd_wb_build), ("mn-fetch", cmd_mn_fetch), ("comics", cmd_comics),
                     ("build", cmd_build), ("publish", cmd_publish), ("all", cmd_all)):
        sp = sub.add_parser(name)
        sp.add_argument("--force", action="store_true", help="refetch metadata")
        sp.add_argument("--langs", help="comma-separated language codes (default: all)")
        sp.add_argument("--store", help="publish: sparse clone of the store repo")
        sp.set_defaults(fn=fn)
    a = p.parse_args(argv)
    a.fn(a)


if __name__ == "__main__":
    sys.exit(main())
