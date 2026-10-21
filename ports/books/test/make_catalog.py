"""make_catalog.py — a small catalog of books in the store's format (docs/BOOKS_PLAN.md §2) for the PC
tests: the files the app asks GitHub Pages and GitHub Releases for, laid out by URL path under OUT/http
(test/native_host.c serves them). Two languages, a few books, one cover, one real EPUB to download.

    python ports/books/test/make_catalog.py FIXTURES OUT
"""
import hashlib
import io
import json
import os
import shutil
import sys

from PIL import Image, ImageDraw

PAGES = "nucleoos-p4-store/books"
REL = "indecenti/nucleoos-p4-store/releases/download"


def main():
    fixtures, out = sys.argv[1], sys.argv[2]
    http = os.path.join(out, "http")
    if os.path.isdir(http):
        shutil.rmtree(http)
    books = os.path.join(http, PAGES)
    os.makedirs(os.path.join(books, "it"))
    os.makedirs(os.path.join(books, "en"))
    os.makedirs(os.path.join(books, "covers"))
    rel = os.path.join(http, REL, "books-it-1")
    os.makedirs(rel)
    epub = os.path.join(fixtures, "pg1012.epub")
    shutil.copyfile(epub, os.path.join(rel, "pg1012.epub"))
    data = open(epub, "rb").read()
    # a cover, 132x192 like the real ones
    im = Image.new("RGB", (132, 192), (120, 40, 40))
    ImageDraw.Draw(im).rectangle((10, 10, 122, 182), outline=(240, 220, 160), width=3)
    b = io.BytesIO()
    im.save(b, "JPEG", quality=72)
    open(os.path.join(books, "covers", "pg1012.jpg"), "wb").write(b.getvalue())
    cats = [{"id": "poetry", "names": {"it": "Poesia", "en": "Poetry", "es": "Poesía", "fr": "Poésie", "de": "Lyrik"}},
            {"id": "novel", "names": {"it": "Romanzi", "en": "Novels", "es": "Novelas", "fr": "Romans", "de": "Romane"}},
            {"id": "howto", "names": {"it": "Manuali", "en": "How-to", "es": "Manuales", "fr": "Manuels", "de": "Ratgeber"}}]
    it = [{"id": "pg1012", "t": "La Divina Commedia", "a": "Dante Alighieri", "ad": "1265-1321", "y": 1320, "c": ["poetry"],
           "src": "pg", "lic": "pd", "s": len(data), "h": hashlib.sha256(data).hexdigest(),
           "u": "https://github.com/%s/books-it-1/pg1012.epub" % REL,
           "d": "Il poema di Dante: Inferno, Purgatorio, Paradiso.", "cv": 1}]
    for k in range(30):   # more rows to scroll and search
        it.append({"id": "pg9%03d" % k, "t": "I promessi sposi è un titolo %d" % k if k == 3 else "Libro di prova %d" % k,
                   "a": "Alessandro Manzoni" if k % 2 else "Autore %d" % k, "c": ["novel" if k % 3 else "howto"],
                   "src": "pg", "lic": "pd", "s": 100000 + k, "h": "0" * 64,
                   "u": "https://github.com/indecenti/nucleoos-p4-store/releases/download/books-it-1/pg9%03d.epub" % k})
    en = [{"id": "se-test", "t": "Pride and Prejudice", "a": "Jane Austen", "ad": "1775-1817", "y": 1813, "c": ["novel"],
           "src": "se", "lic": "cc0", "s": 123, "h": "1" * 64,
           "u": "https://github.com/indecenti/nucleoos-p4-store/releases/download/books-en-1/se-test.epub"}]
    json.dump({"format": 1, "generated": "2026-10-21",
               "langs": [{"code": "it", "count": len(it), "shards": 1}, {"code": "en", "count": len(en), "shards": 1}],
               "cats": cats}, open(os.path.join(books, "index-v1.json"), "w", encoding="utf-8"), ensure_ascii=True)
    json.dump({"format": 1, "lang": "it", "part": 1, "parts": 1, "books": it},
              open(os.path.join(books, "it", "1.json"), "w", encoding="utf-8"), ensure_ascii=False)
    json.dump({"format": 1, "lang": "en", "part": 1, "parts": 1, "books": en},
              open(os.path.join(books, "en", "1.json"), "w", encoding="utf-8"), ensure_ascii=False)
    print("catalog fixture in", http)


if __name__ == "__main__":
    main()
