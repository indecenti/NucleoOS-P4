"""Turn Gutenberg metadata into catalog candidates (rights + usefulness + categories + text fields)."""
from collections import Counter, defaultdict

from . import categories, describe, quality, rights, src_pg
from .common import DEAD_LANGS, LIVING_LANGS

IMAGES_EDITION_MAX = 5_000_000    # above this the no-images EPUB is shipped instead (visual books excepted)


def pick_lang(langs):
    langs = [l.split("-")[0].lower() for l in langs]
    if len(langs) == 1:
        return langs[0]
    if "en" in langs:
        return "en"
    for l in langs:
        if l in LIVING_LANGS:
            return l
    return langs[0] if langs else ""


def pub_year(b):
    ys = [y for y in (b.get("y906"), b.get("y260"), b.get("y_wd")) if y and -3000 < y < 2100]
    return min(ys) if ys else None


def _file_sizes(b):
    f = b["files"]
    pid = b["id"]
    base = "https://www.gutenberg.org/ebooks/%d" % pid
    return {
        "img3": f.get(base + ".epub3.images"),
        "noimg": f.get(base + ".epub.noimages"),
        "txt": f.get(base + ".txt.utf-8") or max([s for u, s in f.items() if u.endswith(".txt")] or [0]) or None,
    }


def candidates(meta):
    """Returns (records, excluded) where excluded[lang][reason] = count."""
    out = []
    excl = defaultdict(Counter)
    for b in meta:
        lang = pick_lang(b.get("langs", []))
        if lang in DEAD_LANGS:
            excl[lang]["dead_language"] += 1
            continue
        if lang not in LIVING_LANGS:
            excl[lang or "?"]["script_or_language_unsupported"] += 1
            continue
        y = pub_year(b)
        ok, why = rights.check_public_domain(b["agents"], y, us_pd=b["rights"].startswith("Public domain in the USA"))
        if not ok:
            excl[lang]["rights:" + why] += 1
            continue
        auts = [a for a in b["agents"] if a["role"] == "aut"] or b["agents"][:1]
        if lang == "el" and all(a.get("death") is not None and a["death"] < 1453 for a in auts):
            excl[lang]["dead_language"] += 1          # Ancient/Byzantine Greek tagged as "el"
            continue
        cats = categories.pg_categories(b["shelves"], b["locc"], b["subjects"])
        sizes = _file_sizes(b)
        why = quality.pg_metadata_reject(b, sizes["txt"], cats)
        if why:
            excl[lang]["quality:" + why] += 1
            continue
        visual = bool(cats) and cats[0] in ("comics", "art", "children")
        limit = quality.MAX_EPUB_BYTES_VISUAL if visual else IMAGES_EDITION_MAX
        if sizes["img3"] and sizes["img3"] <= limit:
            edition = "img3"
        elif sizes["noimg"]:
            edition = "noimg"
        else:
            excl[lang]["quality:no_epub"] += 1
            continue
        names = [describe.person_name(a["name"]) for a in auts if rights.name_kind(a["name"]) == "person"]
        a0 = auts[0]
        author = ", ".join(names[:3]) + (" et al." if len(names) > 3 else "")
        ad = describe.dates(a0.get("birth"), a0.get("death"), lang) if rights.name_kind(a0["name"]) == "person" else ""
        t = describe.clean_title(b["title"])
        rec = {
            "id": "pg%d" % b["id"], "lang": lang, "t": t, "a": author, "ad": ad, "y": y,
            "c": cats, "src": "pg", "lic": "pd", "pop": b["downloads"],
            "d": describe.description(lang, author, ad, cats, y, b.get("summary"), "en",
                                      (b.get("wd_desc") or {}).get(lang)),
            "u2": src_pg.epub_url(b["id"], edition),
            "_pg": b["id"], "_edition": edition, "_est_size": sizes[edition],
            "_contributors": [(a["name"], a["role"], a.get("birth"), a.get("death")) for a in b["agents"]],
        }
        out.append(rec)
    return out, excl
