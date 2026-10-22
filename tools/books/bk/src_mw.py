"""Wikisource + Wikibooks: EPUBs built from the MediaWiki API (action=parse), no scraping of HTML pages.

ws-export (the Wikisource EPUB service) sits behind an anti-bot wall, so books are assembled here:
the root page of a work is parsed, its subpage links (chapters) are followed in document order, each
chapter's rendered HTML is cleaned (everything Wikisource marks `ws-noexport`, navigation, quality
badges, page numbers, edit links) and packed with epubbuild. Images are left out in v1.

Wikisource: texts are public domain; the transcription is CC BY-SA 4.0 -> `lic` cc-by-sa with the
attribution page inside the file and in CREDITS. Rights are still checked with our dates (Wikidata of
the edition/work, author pages, "Testi del 1881"-style categories).
Wikibooks: original works under CC BY-SA 4.0 (contributors alive): licence-based, curated list in
data/wikibooks.json.
"""
import hashlib
import re
from collections import Counter, defaultdict
from urllib.parse import quote, unquote

from lxml import html as lhtml

from . import describe, epubbuild, rights
from .categories import MAX_CATS, order
from .common import CACHE, DATA, Http, load_json, save_json

MW_DIR = CACHE / "mw"
EPUB_DIR = MW_DIR / "epub"

WS = {
    "it": {"site": "it.wikisource.org", "cats": ["Categoria:Testi SAL 100%"], "author_ns": "Autore",
           "year": r"^Categoria:Testi del (\d{3,4})$", "author_cat": r"^Categoria:Testi di (.+)$"},
    "fr": {"site": "fr.wikisource.org", "cats": ["Catégorie:Bon pour export"], "author_ns": "Auteur",
           "year": r"(?:parus|parues|publiés|publiées|écrits|écrites) en (\d{3,4})$", "author_cat": None},
    "es": {"site": "es.wikisource.org", "cats": ["Categoría:Textos validados"], "author_ns": "Autor",
           "year": r"(?:publicad[oa]s|escrit[oa]s) en (\d{3,4})$", "author_cat": None},
    "de": {"site": "de.wikisource.org", "cats": ["Kategorie:Fertig"], "author_ns": None,
           "year": r"^Kategorie:(\d{4})$", "author_cat": None},
    "pt": {"site": "pt.wikisource.org", "cats": ["Categoria:Obras validadas"], "author_ns": "Autor",
           "year": r"(?:publicad[oa]s) em (\d{4})$", "author_cat": None},
    "en": {"site": "en.wikisource.org", "cats": ["Category:Featured texts", "Category:Index Validated"],
           "author_ns": "Author", "year": r"^Category:(\d{4}) works$", "author_cat": None},
}

CAT_RX = [
    (r"romanz|roman\b|romans\b|novel|novela|romane\b", "novel"),
    (r"raccont|novelle|nouvelles|cuentos|erzählung|short stor|contos", "short"),
    (r"poesi|poèmes|poésie|poems|poetry|poemas|gedichte|liriche|sonett|canzon", "poetry"),
    (r"teatr|théâtre|drama|comedia|commedi|tragedi|tragédie|komödie|pièces", "drama"),
    (r"fiab|favol|contes\b|fables|cuentos de hadas|märchen|leggend|légende|mitolog", "myth"),
    (r"ragazzi|infanzia|enfants|jeunesse|infantil|kinder|children", "children"),
    (r"filosof|philosoph", "philosophy"),
    (r"religio|teolog|théolog|bibbia|bible", "religion"),
    (r"storia|histoire|historia|geschichte|history", "history"),
    (r"biograf|autobiograf|memorie|mémoires|memorias", "biography"),
    (r"saggi|essais|ensayos|essays|lettere|lettres|cartas|briefe|epistolar", "essays"),
    (r"viagg|voyage|viaje|reise|travel", "travel"),
    (r"matemat|mathémat|mathemat", "maths"),
    (r"scienz|sciences?\b|ciencia|naturwiss|fisica|physique|chimica|astronom|botanic|zoolog", "science"),
    (r"cucina|cuisine|cocina|kochbuch|ricett|recettes|recetas|gastronom", "cooking"),
    (r"agricolt|agricult|giardin|jardin|orto\b", "nature"),
    (r"economia|économie|política|politica|politique|diritto|droit|derecho", "society"),
    (r"musica|musique|música|musik", "music"),
    (r"arte\b|art\b|architett|architect|pittura|peinture", "art"),
    (r"umoris|humour|humor|satir", "humour"),
    (r"canone|classici|classiques", "classic"),
    (r"grammatic|grammaire|gramática|dizionar|dictionnaire|lingua\b|linguistique", "language"),
]


class MW:
    def __init__(self, delay=0.25):
        self.http = Http(delay=delay)

    def api(self, site, **p):
        p.update(format="json", formatversion=2, maxlag=5)
        for _ in range(5):
            r = self.http.get("https://%s/w/api.php" % site, params=p)
            js = r.json()
            if js.get("error", {}).get("code") == "maxlag":
                import time
                time.sleep(5)
                continue
            return js
        return js

    def parse(self, site, title):
        key = hashlib.sha1(("%s|%s" % (site, title)).encode()).hexdigest()
        p = MW_DIR / "pages" / site / key[:2] / (key + ".json")
        js = load_json(p)
        if js is None:
            js = self.api(site, action="parse", page=title, prop="text|categories|properties|displaytitle",
                          disableeditsection=1, disablelimitreport=1, redirects=1)
            if "parse" in js:
                js = {"title": js["parse"]["title"], "text": js["parse"]["text"],
                      "cats": [c["category"] for c in js["parse"].get("categories", [])],
                      "props": {x["name"]: x["*"] if "*" in x else x.get("value") for x in js["parse"].get("properties", [])}
                      if isinstance(js["parse"].get("properties"), list) else js["parse"].get("properties", {})}
            else:
                js = {"missing": True}
            save_json(p, js, compact=True)
        return None if js.get("missing") else js

    def members(self, site, cat):
        out, cont = [], {}
        while True:
            js = self.api(site, action="query", list="categorymembers", cmtitle=cat, cmnamespace=0, cmlimit=500, **cont)
            out += [m["title"] for m in js.get("query", {}).get("categorymembers", [])]
            if "continue" not in js:
                return out
            cont = js["continue"]

    def page_info(self, site, titles):
        """{title: {"length", "qid"}} for up to 50 titles per call."""
        res = {}
        for i in range(0, len(titles), 50):
            js = self.api(site, action="query", prop="info|pageprops", ppprop="wikibase_item", titles="|".join(titles[i:i + 50]))
            for p in js.get("query", {}).get("pages", []):
                res[p["title"]] = {"length": p.get("length", 0), "qid": p.get("pageprops", {}).get("wikibase_item")}
        return res

    def subpage_counts(self, site, prefix_titles):
        """Number of subpages and their total wikitext size for each root title."""
        res = {}
        for t in prefix_titles:
            n = size = 0
            cont = {}
            while True:
                js = self.api(site, action="query", generator="allpages", gapprefix=t + "/", gapnamespace=0,
                              gaplimit=500, prop="info", **cont)
                for p in js.get("query", {}).get("pages", []):
                    n += 1
                    size += p.get("length", 0)
                if "continue" not in js:
                    break
                cont = js["continue"]
            res[t] = (n, size)
        return res


NOEXPORT = ("ws-noexport", "noprint", "mw-editsection", "pagenum", "ws-pagenum", "navbox", "metadata",
            "mw-empty-elt", "quality-ns0", "noexport", "ws-header", "headertemplate", "toccolours", "mw-kartographer")


def _mtext(el):
    """MathML element -> linear text (x^2, a_i, (a)/(b), √(x))."""
    tag = el.tag.split("}")[-1].lower() if isinstance(el.tag, str) else ""
    kids = [k for k in el if isinstance(k.tag, str)]
    if tag in ("annotation", "annotation-xml"):
        return ""
    if tag in ("mi", "mn", "mo", "mtext", "ms"):
        return (el.text_content() or "").strip()
    if tag == "mspace":
        return " "
    def grp(k):
        t = _mtext(k)
        return t if len(t) <= 1 or re.fullmatch(r"[\w.]+", t) else "(" + t + ")"
    if tag == "msup" and len(kids) == 2:
        return grp(kids[0]) + "^" + grp(kids[1])
    if tag == "msub" and len(kids) == 2:
        return grp(kids[0]) + "_" + grp(kids[1])
    if tag == "msubsup" and len(kids) == 3:
        return grp(kids[0]) + "_" + grp(kids[1]) + "^" + grp(kids[2])
    if tag == "mfrac" and len(kids) == 2:
        return grp(kids[0]) + "/" + grp(kids[1])
    if tag == "msqrt":
        return "√(" + "".join(_mtext(k) for k in kids) + ")"
    if tag == "mroot" and len(kids) == 2:
        return grp(kids[1]) + "√" + grp(kids[0])
    if tag in ("munder", "mover", "munderover") and kids:
        return " ".join(_mtext(k) for k in kids)
    if tag == "mtable":
        return "; ".join(_mtext(k) for k in kids)
    if tag == "mtr":
        return ", ".join(_mtext(k) for k in kids)
    if tag == "semantics" and kids:
        return _mtext(kids[0])
    return "".join(_mtext(k) for k in kids)


def _math_to_text(doc):
    for m in list(doc.iter()):
        if not isinstance(m.tag, str) or m.tag.split("}")[-1].lower() != "math" or m.getparent() is None:
            continue
        txt = re.sub(r"\s+", " ", _mtext(m)).strip()
        block = (m.get("display") == "block") or "mwe-math-element-block" in (m.get("class") or "")
        new = lhtml.Element("div" if block else "span")
        new.set("class", "center" if block else "math")
        new.text = txt
        new.tail = m.tail
        m.getparent().replace(m, new)
    for img in list(doc.iter("img")):        # old-style math fallback images: keep their TeX alt
        if "mwe-math-fallback" in (img.get("class") or "") and img.getparent() is not None:
            alt = re.sub(r"^\{\displaystyle\s*(.*)\}$", r"\1", (img.get("alt") or "").strip())
            new = lhtml.Element("span")
            new.set("class", "math")
            new.text = alt
            new.tail = img.tail
            img.getparent().replace(img, new)


def clean_html(text):
    doc = lhtml.fragment_fromstring(text, create_parent="div")
    _math_to_text(doc)
    for el in list(doc.iter()):
        if not isinstance(el.tag, str) or el.getparent() is None:
            continue
        cls = el.get("class") or ""
        eid = el.get("id") or ""
        if any(c in cls.split() for c in NOEXPORT) or eid in ("textquality", "toc", "headertemplate", "Intestazione") \
                or "intestazione" in cls.lower() or el.tag in ("style", "link"):
            el.drop_tree()
    return lhtml.tostring(doc, encoding="unicode")


def _wiki_target(href, site):
    if href.startswith("//%s/wiki/" % site):
        href = href[len("//" + site):]
    if href.startswith("https://%s/wiki/" % site):
        href = href[len("https://" + site):]
    if not href.startswith("/wiki/"):
        return None
    return unquote(href[6:].split("#")[0]).replace("_", " ")


def chapter_titles(page, site):
    """Subpages of `page` linked from it, in document order."""
    root = page["title"]
    doc = lhtml.fragment_fromstring(page["text"], create_parent="div")
    seen, out = set(), []
    for a in doc.iter("a"):
        t = _wiki_target(a.get("href") or "", site)
        if t and t.startswith(root + "/") and t not in seen:
            seen.add(t)
            out.append(t)
    return out


def _text_len(html_text):
    try:
        return len(lhtml.fragment_fromstring(html_text, create_parent="div").text_content().strip())
    except Exception:
        return 0


def collect(mw, site, title, depth=0, limit=400):
    """[(chapter_title, cleaned_html)] for a work, following subpages up to 3 levels."""
    page = mw.parse(site, title)
    if not page:
        return []
    subs = chapter_titles(page, site)
    body = clean_html(page["text"])
    if not subs or depth >= 3:
        return [(page["title"].split("/")[-1] if depth else "", body)]
    out = []
    if _text_len(body) - sum(len(s) for s in subs) > 2500:
        out.append(("", body))
    for s in subs[:limit]:
        out += collect(mw, site, s, depth + 1, limit)
        if len(out) >= limit:
            break
    return out


def build_epub(mw, site, title, lang, meta, dest, license_html):
    chapters = collect(mw, site, title)
    if not chapters:
        return None
    names = {}
    for i, (ct, _) in enumerate(chapters, 1):
        names.setdefault(ct, "c%03d.xhtml" % i)
    sub_to_file = {}
    for i, (ct, _) in enumerate(chapters, 1):
        sub_to_file[ct] = "c%03d.xhtml" % i

    def remap(href):
        if href.startswith("#"):
            return href
        t = _wiki_target(href, site)
        if t and t.startswith(title + "/"):
            return sub_to_file.get(t.split("/")[-1])
        return None
    clean = []
    for ct, body in chapters:
        clean.append((ct, epubbuild.sanitize(body, remap)))
    total = sum(_text_len("<div>%s</div>" % b) for _, b in clean if b)
    if total < 15000:
        return {"too_short": total}
    epubbuild.build(dest, meta["t"], [meta["a"]] if meta.get("a") else [], lang, clean, license_html,
                    identifier="https://%s/wiki/%s" % (site, quote(title.replace(" ", "_"))),
                    description=meta.get("d", ""), date=meta.get("y"),
                    source_url="https://%s/wiki/%s" % (site, quote(title.replace(" ", "_"))))
    return {"chars": total, "chapters": len(clean)}


def ws_license(lang, site, title, url):
    t = {
        "it": ("Testo di pubblico dominio trascritto e riletto dai volontari di Wikisource ({site}). "
               "La trascrizione è distribuita con licenza Creative Commons Attribuzione - Condividi allo stesso modo 4.0 "
               "(CC BY-SA 4.0, https://creativecommons.org/licenses/by-sa/4.0/). Fonte: {url} ; autori della trascrizione: "
               "vedi la cronologia delle pagine su Wikisource. Impaginato per NucleoOS Libri."),
        "fr": ("Texte du domaine public transcrit et relu par les bénévoles de Wikisource ({site}). La transcription "
               "est diffusée sous licence Creative Commons Attribution - Partage dans les mêmes conditions 4.0 "
               "(CC BY-SA 4.0, https://creativecommons.org/licenses/by-sa/4.0/). Source : {url} ; contributeurs : voir "
               "l'historique des pages sur Wikisource. Mis en page pour NucleoOS Libri."),
        "es": ("Texto de dominio público transcrito y revisado por los voluntarios de Wikisource ({site}). La "
               "transcripción se distribuye con licencia Creative Commons Atribución-CompartirIgual 4.0 "
               "(CC BY-SA 4.0, https://creativecommons.org/licenses/by-sa/4.0/). Fuente: {url} ; colaboradores: ver el "
               "historial de las páginas en Wikisource. Maquetado para NucleoOS Libri."),
        "de": ("Gemeinfreier Text, transkribiert und korrekturgelesen von den Freiwilligen von Wikisource ({site}). Die "
               "Transkription steht unter der Lizenz Creative Commons Namensnennung - Weitergabe unter gleichen "
               "Bedingungen 4.0 (CC BY-SA 4.0, https://creativecommons.org/licenses/by-sa/4.0/). Quelle: {url} ; "
               "Mitwirkende: siehe Versionsgeschichte der Seiten auf Wikisource. Gesetzt für NucleoOS Libri."),
    }.get(lang, "Public domain text transcribed and proofread by Wikisource volunteers ({site}). The transcription "
                "is licensed under Creative Commons Attribution-ShareAlike 4.0 (CC BY-SA 4.0, "
                "https://creativecommons.org/licenses/by-sa/4.0/). Source: {url} ; contributors: see the page "
                "histories on Wikisource. Typeset for NucleoOS Books.")
    return "<h2>%s</h2><p>%s</p>" % (epubbuild._esc(title), epubbuild._esc(t.format(site=site, url=url)))


def ws_categories(cats):
    out = []
    low = " | ".join(c.replace("_", " ").lower() for c in cats)
    for rx, cid in CAT_RX:
        if re.search(rx, low):
            out.append(cid)
    out = order(out or ["classic"])
    return out[:MAX_CATS]


def pageviews(http, project, title):
    url = ("https://wikimedia.org/api/rest_v1/metrics/pageviews/per-article/%s/all-access/user/%s/monthly/"
           "2025100100/2026093000" % (project, quote(title.replace(" ", "_"), safe="")))
    r = http.get(url)
    if r.status_code != 200:
        return 0
    return sum(i.get("views", 0) for i in r.json().get("items", []))
