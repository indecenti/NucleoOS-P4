"""Wikibooks manuals (CC BY-SA 4.0), hand-picked in data/wikibooks.json, built with src_mw.

Book = root page with chapter subpages, or a print-version page ("Druckversion", "Texto completo",
"Version imprimable"...) that transcludes the whole book; single long pages are split at <h2>.
Only licence-based (no public-domain dates involved): Wikibooks text is CC BY-SA 4.0 since 2009
(older text dual GFDL/CC BY-SA), authors = the page histories.
"""
import hashlib
import re
from collections import Counter, defaultdict
from urllib.parse import quote

from lxml import html as lhtml

from . import describe, epubbuild, mwimages, src_mw
from .categories import MAX_CATS, order
from .common import CACHE, DATA, Http, load_json, save_json

CAND = CACHE / "candidates-wb.json"
AUTHOR = {"it": "Autori di Wikibooks", "de": "Wikibooks-Autoren", "fr": "Contributeurs de Wikilivres",
          "es": "Colaboradores de Wikilibros", "en": "Wikibooks contributors"}
PRINT_SUFFIX = re.compile(r"(/\s*|:\s*)(Druckversion|Texto completo|Texto Completo|Versión para imprimir|"
                          r"Versión Para Imprimir|Version imprimable|Print version|Versione stampabile)$")
LICENSE = {
    "it": "Testo tratto da Wikibooks ({site}), scritto dai volontari di Wikibooks e distribuito con licenza Creative "
          "Commons Attribuzione - Condividi allo stesso modo 4.0 (CC BY-SA 4.0, "
          "https://creativecommons.org/licenses/by-sa/4.0/). Fonte: {url} ; autori: vedi la cronologia delle pagine. "
          "Le immagini incluse hanno ciascuna la propria licenza libera, elencata sotto. Impaginato per NucleoOS Libri.",
    "de": "Text aus Wikibooks ({site}), geschrieben von den Freiwilligen von Wikibooks, Lizenz Creative Commons "
          "Namensnennung - Weitergabe unter gleichen Bedingungen 4.0 (CC BY-SA 4.0, "
          "https://creativecommons.org/licenses/by-sa/4.0/). Quelle: {url} ; Autoren: siehe Versionsgeschichte. "
          "Die enthaltenen Bilder stehen jeweils unter der unten genannten freien Lizenz. Gesetzt für NucleoOS Libri.",
    "fr": "Texte issu de Wikilivres ({site}), écrit par les bénévoles de Wikilivres et diffusé sous licence Creative "
          "Commons Attribution - Partage dans les mêmes conditions 4.0 (CC BY-SA 4.0, "
          "https://creativecommons.org/licenses/by-sa/4.0/). Source : {url} ; auteurs : voir l'historique des pages. "
          "Les images incluses ont chacune leur licence libre, indiquée ci-dessous. Mis en page pour NucleoOS Libri.",
    "es": "Texto de Wikilibros ({site}), escrito por los voluntarios de Wikilibros y distribuido con licencia Creative "
          "Commons Atribución-CompartirIgual 4.0 (CC BY-SA 4.0, https://creativecommons.org/licenses/by-sa/4.0/). "
          "Fuente: {url} ; autores: ver el historial de las páginas. Las imágenes incluidas tienen cada una su licencia libre, indicada abajo. "
          "Maquetado para NucleoOS Libri.",
    "en": "Text from Wikibooks ({site}), written by Wikibooks volunteers and licensed under Creative Commons "
          "Attribution-ShareAlike 4.0 (CC BY-SA 4.0, https://creativecommons.org/licenses/by-sa/4.0/). "
          "Source: {url} ; authors: see the page histories. Included images each carry their own free licence, listed below. Typeset for NucleoOS Books.",
}

IMAGES_HEADING = {"it": "Immagini (file, autore, licenza, fonte)", "de": "Bilder (Datei, Urheber, Lizenz, Quelle)",
                  "fr": "Images (fichier, auteur, licence, source)", "es": "Imágenes (archivo, autor, licencia, fuente)",
                  "en": "Images (file, author, licence, source)"}
H2 = re.compile(r'(?=<div class="mw-heading mw-heading2")|(?=<h2[ >])')


def split_h2(body):
    """One long page -> chapters at each level-2 heading."""
    parts = [p for p in H2.split(body) if p.strip()]
    if len(parts) < 3:
        return None
    out = []
    for p in parts:
        try:
            frag = lhtml.fragment_fromstring(p, create_parent="div")
            h = frag.find(".//h2")
            title = h.text_content().strip() if h is not None else ""
        except Exception:
            title = ""
        out.append((title, p))
    return out


def first_paragraph(body):
    try:
        doc = lhtml.fragment_fromstring(body, create_parent="div")
    except Exception:
        return ""
    for p in doc.iter("p"):
        t = re.sub(r"\s+", " ", p.text_content()).strip()
        if len(t) >= 80:
            return t
    return ""


def wb_id(site, title):
    return "wb%s-%s" % (site.split(".")[0], hashlib.sha1(("%s|%s" % (site, title)).encode()).hexdigest()[:10])


def build(log=print):
    picks = load_json(DATA / "wikibooks.json")
    mw = src_mw.MW()
    http = Http(delay=0.2)
    recs, excl = [], defaultdict(Counter)
    for lang, items in picks.items():
        if lang.startswith("_"):
            continue
        site = "%s.wikibooks.org" % lang
        for title, cats in items:
            rid = wb_id(site, title)
            dest = src_mw.EPUB_DIR / (rid + ".epub")
            book_title = PRINT_SUFFIX.sub("", title).strip()
            url = "https://%s/wiki/%s" % (site, quote(title.replace(" ", "_")))
            root = mw.parse(site, book_title) or mw.parse(site, title)
            desc = first_paragraph(src_mw.clean_html(root["text"])) if root else ""
            meta = {"t": book_title, "a": AUTHOR[lang], "d": describe._cut(desc) if desc else ""}
            if not dest.exists():
                chapters = src_mw.collect(mw, site, title)
                if len(chapters) == 1:
                    chapters = split_h2(chapters[0][1]) or chapters
                if not chapters:
                    excl[lang]["wb:missing"] += 1
                    log("wb missing %s %s" % (lang, title))
                    continue

                def remap(href):
                    return href if href.startswith("#") else None
                chapters, images, credits = mwimages.embed(mw, site, chapters)
                clean = [(ct, epubbuild.sanitize(b, remap, keep_images=True)) for ct, b in chapters]
                total = sum(src_mw._text_len("<div>%s</div>" % b) for _, b in clean if b)
                if total < 15000:
                    excl[lang]["quality:too_short"] += 1
                    log("wb too short %s %s (%d)" % (lang, title, total))
                    continue
                lic = "<h2>%s</h2><p>%s</p>" % (epubbuild._esc(book_title),
                                                epubbuild._esc(LICENSE[lang].format(site=site, url=url)))
                lic += mwimages.credits_html(credits, IMAGES_HEADING[lang])
                epubbuild.build(dest, book_title, [AUTHOR[lang]], lang, clean, lic, identifier=url,
                                description=meta["d"], source_url=url, images=images)
            c = order(cats)[:MAX_CATS]
            pop = src_mw.pageviews(http, site.replace(".org", ""), book_title) // 12
            recs.append({"id": rid, "lang": lang, "t": describe.clean_title(book_title), "a": AUTHOR[lang],
                         "c": c, "src": "wb", "lic": "cc-by-sa", "pop": pop,
                         "d": meta["d"] or describe.description(lang, "", "", c, None), "u2": url,
                         "_file": str(dest), "_attrib": "Wikibooks (%s), CC BY-SA 4.0" % site})
            log("wb %s %s ok" % (lang, book_title))
    save_json(CAND, {"records": recs, "excluded": excl})
    log("wb candidates: %d" % len(recs))
    return recs
