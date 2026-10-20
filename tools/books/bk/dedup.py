"""Cross-source dedup (BOOKS_PLAN D4): one row per work and language.

Best edition wins: Standard Ebooks > Wikisource > Gutenberg > others. Inside one source the more
popular record wins. Records can also name the record they duplicate explicitly (`dup_of`, e.g. a
Standard Ebooks book built from a Gutenberg transcription), which always merges them.
"""
import re

from .describe import norm_key

SOURCE_RANK = {"se": 0, "ws": 1, "mn": 2, "pg": 3, "ia": 4}


def work_key(rec):
    title = rec.get("t", "")
    main = re.split(r"[:;(]| - | — ", title)[0]
    first_author = (rec.get("a") or "").split(",")[0]
    surname = norm_key(first_author).split(" ")[-1] if first_author else ""
    # volumes of one work are different books, never duplicates of each other
    vol = VOLUME_RX.search(norm_key(title))
    part = (vol.group(1) + " " + vol.group(2)) if vol else ""
    return "%s|%s|%s|%s" % (rec.get("lang", ""), norm_key(main), surname, part)


VOLUME_RX = re.compile(r"\b(vol|volume|volumen|tome|tomo|band|bd|book|libro|livre|buch|part|parte|partie|"
                       r"teil|deel|osa|kötet|series|serie|canto|cantica)\s+([ivxlc]+|\d+|one|two|three|"
                       r"first|second|third|primo|secondo|terzo|premier|second|erster|zweiter)\b")


def rank(rec):
    return (SOURCE_RANK.get(rec.get("src"), 9), -(rec.get("pop") or 0))


def dedupe(records):
    """Returns (kept, dropped) where dropped is [(rec, winner_id)]."""
    by_id = {r["id"]: r for r in records}
    alias = {}
    for r in records:
        for d in r.get("dup_of", []):
            if d in by_id:
                alias[d] = r["id"]
    groups = {}
    for r in records:
        groups.setdefault(work_key(r), []).append(r)
    kept, dropped = [], []
    for rs in groups.values():
        rs.sort(key=rank)
        win = rs[0]
        kept.append(win)
        for r in rs[1:]:
            dropped.append((r, win["id"]))
    # explicit duplicates that landed in different groups (different title spellings)
    kept_ids = {r["id"] for r in kept}
    final = []
    for r in kept:
        w = alias.get(r["id"])
        if w and w in kept_ids and rank(by_id[w]) <= rank(r):
            dropped.append((r, w))
        else:
            final.append(r)
    # popularity of the merged work = the best of its editions (keeps famous works on top)
    winners = {r["id"]: r for r in final}
    for r, w in dropped:
        if w in winners:
            winners[w]["pop"] = max(winners[w].get("pop") or 0, r.get("pop") or 0)
    return final, dropped
