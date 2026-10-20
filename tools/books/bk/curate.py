"""Per-language selection policy: curated, not "everything".

English Gutenberg alone has ~26k books that pass the rights filter; shipping all of them would be
~20 GB of mostly obscure titles. The user asked for ~9000 English books with the weight on useful
ones, so English keeps: every practical book (manuals, cooking, maths, computing, survival...), every
Standard Ebooks title, the canonical classics, non-fiction above a moderate popularity and fiction
only above a high one. Every other language keeps all books that passed rights + quality (they are few).
"""
PRACTICAL = {"comics", "survival", "computing", "cooking", "maths", "howto", "tech", "language"}
FICTION = {"novel", "short", "poetry", "drama", "adventure", "scifi", "crime", "historical", "romance",
           "humour", "children", "myth", "comics", "classic"}

# 30-day downloads at gutenberg.org (tuned to ~9000 English books in total)
EN_MIN_POP_FICTION = 1200
EN_MIN_POP_NONFICTION = 690
EN_MIN_POP_CLASSIC = 400


def keep(rec):
    """(keep, reason) for a candidate after rights/quality/dedup."""
    if rec["lang"] != "en" or rec["src"] != "pg":
        return True, "ok"
    c = set(rec.get("c") or [])
    pop = rec.get("pop") or 0
    if c & PRACTICAL:
        return True, "ok"
    if "classic" in c and pop >= EN_MIN_POP_CLASSIC:
        return True, "ok"
    fiction = bool(c & FICTION) and not (c - FICTION)
    if pop >= (EN_MIN_POP_FICTION if fiction else EN_MIN_POP_NONFICTION):
        return True, "ok"
    return False, "curation:not_popular_enough"
