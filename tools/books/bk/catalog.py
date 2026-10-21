"""Build stage: validate files, assign releases, write the §2 indexes, reports and credits.

Output tree (OUT, default tools/books/out, outside git):

  books/index-v1.json                 -> Pages books/index-v1.json
  books/<lang>/<k>.json               -> Pages books/<lang>/<k>.json
  releases/books-<lang>-<n>/<id>.epub -> GitHub Release assets (hard links into the cache)
  release-map.json                    -> id -> release tag; keeps URLs stable across rebuilds (F5 must keep it)
  reports/<lang>.md, reports/summary.md, reports/summary.json, reports/sample-<lang>.md
  CREDITS.md                          -> attribution for CC BY / CC BY-SA / GFDL items
"""
import datetime
import os
import random
import re
from collections import Counter, defaultdict
from pathlib import Path

from . import appcheck, categories, covers, curate, dedup, describe, epubcheck, quality, rank
from .common import (CACHE, MAX_ASSETS_PER_RELEASE, MAX_ROWS_PER_SHARD, OUT, PRIORITY_LANGS, RELEASE_BASE,
                     load_json, save_json, sha256_file)

ID_RX = re.compile(r"^[a-z0-9-]{1,40}$")
VALIDATE_CACHE = CACHE / "validate.json"
ROW_FIELDS = ("id", "t", "st", "a", "ad", "y", "c", "src", "lic", "s", "h", "u", "u2", "d", "cv")
PAGES_BUDGET = 120_000_000      # covers + indexes on Pages (Pages is ~790 MB of its 1 GB limit)
ROW_BYTES_ESTIMATE = 520        # one compact JSON row, measured


def _validate(path, vcache):
    st = os.stat(path)
    key = "%s|%d|%d" % (path, st.st_size, int(st.st_mtime))
    hit = vcache.get(key)
    if hit:
        return hit
    r = epubcheck.check(path)
    res = {"ok": r["ok"], "error": r["error"], "size": st.st_size}
    if r["ok"]:
        ok, why, stats = quality.text_quality(r["text"])
        res.update({"text_ok": ok, "text_why": why, "stats": stats, "spine": r["spine"]})
        res["sha256"] = sha256_file(path)
    vcache[key] = res
    return res


def assign_releases(rows_by_lang, rmap):
    """Stable release tags: known ids keep theirs, new ids fill the last release with room."""
    count = Counter(rmap.values())
    for lang, rows in rows_by_lang.items():
        for r in sorted(rows, key=lambda r: -(r.get("pop") or 0)):
            tag = rmap.get(r["id"])
            if tag and tag.startswith("books-%s-" % lang):
                continue
            n = 1
            while count["books-%s-%d" % (lang, n)] >= MAX_ASSETS_PER_RELEASE:
                n += 1
            tag = "books-%s-%d" % (lang, n)
            rmap[r["id"]] = tag
            count[tag] += 1
    return rmap


def build(candidates, excluded, log=print, link_assets=True, use_app_check=True):
    """candidates: records from all sources (with _file); excluded: {lang: Counter(reason)}."""
    excluded = defaultdict(Counter, {k: Counter(v) for k, v in excluded.items()})
    kept, dropped = dedup.dedupe(candidates)
    for r, w in dropped:
        excluded[r["lang"]]["duplicate"] += 1
    vcache = load_json(VALIDATE_CACHE, {}) or {}
    rows = []
    n = 0
    actx = None
    if use_app_check:
        files = [r["_file"] for r in kept if r.get("_file") and curate.keep(r)[0] and Path(r["_file"]).exists()]
        actx = appcheck.run(files, log=log)
    app_fail = []
    for r in kept:
        ok, why = curate.keep(r)
        if not ok:
            excluded[r["lang"]][why] += 1
            continue
        if not ID_RX.match(r["id"]):
            excluded[r["lang"]]["bad_id"] += 1
            continue
        if "gutenberg" in r["t"].lower():          # PG trademark never in our titles
            excluded[r["lang"]]["trademark_in_title"] += 1
            continue
        if "gutenberg" in (r.get("d") or "").lower():
            r["d"] = ""
        f = r.get("_file")
        if not f or not Path(f).exists():
            excluded[r["lang"]]["file:missing"] += 1
            continue
        v = _validate(str(f), vcache)
        n += 1
        if n % 500 == 0:
            save_json(VALIDATE_CACHE, vcache, compact=True)
            log("validated %d" % n)
        if not v["ok"]:
            excluded[r["lang"]]["file:invalid_epub"] += 1
            log("invalid %s: %s" % (r["id"], v["error"]))
            continue
        if not v["text_ok"] and not (r.get("c") and r["c"][0] == "comics"):
            excluded[r["lang"]]["file:" + v["text_why"]] += 1
            continue
        if actx is not None:
            a = appcheck.result(str(f), actx)
            if not a or a["status"] != "ok":
                why = a["status"] if a else "unchecked"
                excluded[r["lang"]]["file:app_" + why] += 1
                app_fail.append((r["lang"], r["id"], why, (a or {}).get("line", "")))
                continue
        r["s"], r["h"] = v["size"], v["sha256"]
        rows.append(r)
    save_json(VALIDATE_CACHE, vcache, compact=True)
    if app_fail:
        (OUT / "reports").mkdir(parents=True, exist_ok=True)
        (OUT / "reports" / "app-check-failures.md").write_text(
            "# Libri scartati dal motore dell'app (bk_check)\n\n| Lingua | id | Esito | Messaggio |\n|---|---|---|---|\n" +
            "".join("| %s | %s | %s | %s |\n" % (l, i, w, m.replace("|", "/")) for l, i, w, m in sorted(app_fail)),
            encoding="utf-8")
        log("app check: %d books dropped (see reports/app-check-failures.md)" % len(app_fail))

    for r in rows:
        # main title in "t", the rest (subtitle, series, volume note) in "st"; several authors joined by " & "
        r["t"], st = describe.split_title(r["t"])
        if st:
            r["st"] = st
        if r.get("a") and ", " in r["a"] and r["src"] in ("pg", "se", "ws", "mn"):
            r["a"] = r["a"].replace(", ", " & ")
    rank.apply(rows, log)

    by_lang = defaultdict(list)
    for r in rows:
        by_lang[r["lang"]].append(r)
    map_path = OUT / "release-map.json"
    rmap = assign_releases(by_lang, load_json(map_path, {}) or {})
    save_json(map_path, rmap)
    for r in rows:
        r["u"] = "%s/%s/%s.epub" % (RELEASE_BASE, rmap[r["id"]], r["id"])

    books_dir = OUT / "books"
    cover_stats = make_covers(rows, books_dir / "covers", PAGES_BUDGET - ROW_BYTES_ESTIMATE * len(rows) - 50_000, log)
    langs_meta = []
    order = sorted(by_lang, key=lambda l: (PRIORITY_LANGS.index(l) if l in PRIORITY_LANGS else 99, -len(by_lang[l]), l))
    for lang in order:
        lr = sorted(by_lang[lang], key=lambda r: (-(r.get("pop") or 0), r["t"]))
        shards = [lr[i:i + MAX_ROWS_PER_SHARD] for i in range(0, len(lr), MAX_ROWS_PER_SHARD)]
        ldir = books_dir / lang
        if ldir.exists():
            for old in ldir.glob("*.json"):
                old.unlink()
        for k, part in enumerate(shards, 1):
            save_json(ldir / ("%d.json" % k), {"format": 1, "lang": lang, "part": k, "parts": len(shards),
                                               "books": [row(r) for r in part]}, compact=True)
        langs_meta.append({"code": lang, "count": len(lr), "shards": len(shards)})
    save_json(books_dir / "index-v1.json", {
        "format": 1, "generated": datetime.date.today().isoformat(), "langs": langs_meta,
        "cats": categories.index_cats()}, compact=True)
    if link_assets:
        _link_assets(rows, rmap)
    idx_bytes = sum(f.stat().st_size for f in books_dir.rglob("*.json"))
    cover_stats["index_bytes"] = idx_bytes
    log("pages: indexes %.1f MB + covers %.1f MB (%d covers) = %.1f MB of %.0f MB budget" % (
        idx_bytes / 1e6, cover_stats["bytes"] / 1e6, cover_stats["count"],
        (idx_bytes + cover_stats["bytes"]) / 1e6, PAGES_BUDGET / 1e6))
    write_reports(by_lang, excluded, order, cover_stats)
    write_credits(rows)
    log("catalog: %d books in %d languages" % (len(rows), len(by_lang)))
    return rows, excluded


def make_covers(rows, cdir, budget, log=print):
    """Thumbnails for the most popular books first until the Pages budget is used; sets r["cv"] = 1."""
    cdir.mkdir(parents=True, exist_ok=True)
    used = n = skipped_budget = 0
    by_lang = Counter()
    keep = set()
    for i, r in enumerate(sorted(rows, key=lambda r: -(r.get("pop") or 0))):
        r.pop("cv", None)
        t = covers.cover_for(r)
        if i % 1000 == 0 and i:
            log("covers %d/%d (%d made)" % (i, len(rows), n))
        if not t:
            continue
        if used + len(t) > budget:
            skipped_budget += 1
            continue
        dst = cdir / (r["id"] + ".jpg")
        if not dst.exists() or dst.stat().st_size != len(t):
            dst.write_bytes(t)
        keep.add(dst.name)
        r["cv"] = 1
        used += len(t)
        n += 1
        by_lang[r["lang"]] += 1
    for f in cdir.glob("*.jpg"):
        if f.name not in keep:
            f.unlink()
    return {"count": n, "bytes": used, "by_lang": dict(by_lang), "skipped_budget": skipped_budget}


def row(r):
    out = {}
    for k in ROW_FIELDS:
        v = r.get(k)
        if v is None or v == "" or v == []:
            continue
        out[k] = v
    return out


def _link_assets(rows, rmap):
    rel = OUT / "releases"
    want = defaultdict(set)
    for r in rows:
        want[rmap[r["id"]]].add(r["id"])
    for r in rows:
        dst = rel / rmap[r["id"]] / (r["id"] + ".epub")
        dst.parent.mkdir(parents=True, exist_ok=True)
        src = Path(r["_file"])
        if dst.exists():
            if dst.stat().st_size == src.stat().st_size and os.path.samefile(dst, src):
                continue
            dst.unlink()
        try:
            os.link(src, dst)
        except OSError:
            import shutil
            shutil.copy2(src, dst)
    # drop assets no longer in the catalog
    if rel.exists():
        for d in rel.iterdir():
            if d.is_dir():
                for f in d.glob("*.epub"):
                    if f.stem not in want.get(d.name, ()):
                        f.unlink()


def _mb(n):
    return "%.1f MB" % (n / 1e6)


def write_reports(by_lang, excluded, order, cover_stats=None):
    rep = OUT / "reports"
    rep.mkdir(parents=True, exist_ok=True)
    summary = {"generated": datetime.date.today().isoformat(), "langs": {}}
    lines = ["# Catalogo libri: riepilogo", "", "| Lingua | Libri | Dimensione | Fonti | Esclusi |",
             "|---|---:|---:|---|---:|"]
    tot_n = tot_s = 0
    all_langs = order + sorted(l for l in excluded if l not in by_lang)
    for lang in all_langs:
        lr = by_lang.get(lang, [])
        size = sum(r["s"] for r in lr)
        tot_n += len(lr)
        tot_s += size
        cats = Counter(c for r in lr for c in (r.get("c") or ["(none)"]))
        srcs = Counter(r["src"] for r in lr)
        lics = Counter(r["lic"] for r in lr)
        exc = excluded.get(lang, Counter())
        summary["langs"][lang] = {"books": len(lr), "bytes": size, "categories": dict(cats),
                                  "sources": dict(srcs), "licenses": dict(lics), "excluded": dict(exc)}
        if lr:
            lines.append("| %s | %d | %s | %s | %d |" % (lang, len(lr), _mb(size),
                         ", ".join("%s %d" % kv for kv in srcs.most_common()), sum(exc.values())))
        md = ["# Libri in `%s`" % lang, "", "Libri: **%d**, dimensione totale %s" % (len(lr), _mb(size)), ""]
        for title, cnt in (("Categorie", cats), ("Fonti", srcs), ("Licenze", lics), ("Esclusi per motivo", exc)):
            md += ["## " + title, "", "| | n |", "|---|---:|"] + ["| %s | %d |" % kv for kv in cnt.most_common()] + [""]
        (rep / ("%s.md" % lang)).write_text("\n".join(md), encoding="utf-8")
        if lr:
            (rep / ("sample-%s.md" % lang)).write_text(sample_md(lang, lr), encoding="utf-8")
    lines += ["", "Totale: **%d libri**, %s" % (tot_n, _mb(tot_s))]
    summary["total_books"], summary["total_bytes"] = tot_n, tot_s
    if cover_stats:
        summary["covers"] = cover_stats
        lines += ["", "Copertine: %d (%s), indici %s; totale su Pages %s (budget %s)%s" % (
            cover_stats["count"], _mb(cover_stats["bytes"]), _mb(cover_stats.get("index_bytes", 0)),
            _mb(cover_stats["bytes"] + cover_stats.get("index_bytes", 0)), _mb(PAGES_BUDGET),
            ("; %d copertine lasciate fuori per il budget" % cover_stats["skipped_budget"])
            if cover_stats["skipped_budget"] else "")]
        lines += ["", "| Lingua | Copertine |", "|---|---:|"] + [
            "| %s | %d |" % kv for kv in sorted(cover_stats["by_lang"].items(), key=lambda kv: -kv[1])]
    (rep / "summary.md").write_text("\n".join(lines) + "\n", encoding="utf-8")
    save_json(rep / "summary.json", summary)


def sample_md(lang, lr, n=30):
    """30 titles for review: the 15 most popular + 15 random others (fixed seed, spread over categories)."""
    lr = sorted(lr, key=lambda r: -(r.get("pop") or 0))
    top = lr[:15]
    rest = lr[15:]
    rnd = random.Random("nucleo-" + lang)
    pick = rnd.sample(rest, min(n - len(top), len(rest)))
    out = ["# Campione `%s` (15 più scaricati + 15 a caso)" % lang, "",
           "| id | Titolo | Autore | Anno | Categorie | Fonte | Licenza | Dim. |", "|---|---|---|---|---|---|---|---:|"]
    for r in top + pick:
        out.append("| %s | %s | %s | %s | %s | %s | %s | %s |" % (
            r["id"], r["t"].replace("|", "/"), (r.get("a") or "").replace("|", "/"),
            r.get("y") or "", ", ".join(r.get("c") or []), r["src"], r["lic"], _mb(r["s"])))
    return "\n".join(out) + "\n"


def write_credits(rows):
    att = [r for r in rows if r["lic"] in ("cc-by", "cc-by-sa", "gfdl", "mit", "apache-2.0", "bsd", "gpl")]
    lines = ["# Crediti e attribuzioni", "",
             "Libri distribuiti con licenze che richiedono l'attribuzione. Ogni EPUB contiene anche la propria",
             "pagina di licenza. Le opere di pubblico dominio (Project Gutenberg, Standard Ebooks) non sono elencate:",
             "nelle schede compare la fonte con il link all'originale.", ""]
    for r in sorted(att, key=lambda r: (r["lang"], r["t"])):
        lines.append("- **%s** (%s) - %s. Licenza: %s. Fonte: %s%s" % (
            r["t"], r["lang"], r.get("a") or "", r["lic"].upper(), r.get("u2") or "",
            (". " + r["_attrib"]) if r.get("_attrib") else ""))
    (OUT / "CREDITS.md").write_text("\n".join(lines) + "\n", encoding="utf-8")
