"""Filter tests: python -m pytest tools/books/tests (or python -m unittest discover tools/books/tests)."""
import os
import sys
import unittest
import zipfile

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))

from bk import categories, curate, dedup, describe, epubcheck, quality, rights  # noqa: E402
from bk.select_pg import pick_lang  # noqa: E402


def A(name, death, birth=None, role="aut"):
    return {"name": name, "death": death, "birth": birth, "role": role}


class Rights(unittest.TestCase):
    def ok(self, agents, y, us=True):
        return rights.check_public_domain(agents, y, us)

    def test_old_author_known_year(self):
        self.assertEqual(self.ok([A("Austen, Jane", 1817, 1775)], 1813), (True, "ok"))

    def test_death_limit_inclusive(self):
        self.assertTrue(self.ok([A("X", 1955)], 1920)[0])
        self.assertEqual(self.ok([A("X", 1956)], 1920), (False, "died_after_1955"))

    def test_publication_limit_inclusive(self):
        self.assertTrue(self.ok([A("X", 1940)], 1930)[0])
        self.assertEqual(self.ok([A("X", 1940)], 1931), (False, "published_after_1930"))

    def test_every_contributor_counts(self):
        # Translator still in copyright in the EU
        self.assertEqual(self.ok([A("Tolstoy", 1910), A("Trans", 1970, role="trl")], 1925),
                         (False, "died_after_1955"))
        # Illustrator without a death date: unverifiable
        self.assertEqual(self.ok([A("Author", 1900), A("Ill", None, role="ill")], 1890),
                         (False, "no_death_date"))

    def test_printer_and_publisher_ignored(self):
        self.assertTrue(self.ok([A("Author", 1900), A("Printer", None, role="prt")], 1890)[0])

    def test_missing_data_excluded(self):
        self.assertEqual(self.ok([A("X", None)], 1900), (False, "no_death_date"))
        self.assertEqual(self.ok([A("X", None, birth=1920)], None), (False, "author_maybe_alive"))
        self.assertEqual(self.ok([], 1900), (False, "no_author"))

    def test_no_pub_year(self):
        # all dead by 1930: the work cannot postdate them -> ok (source also asserts US PD)
        self.assertTrue(self.ok([A("X", 1925)], None)[0])
        # died 1931-1955 and no year: cannot prove publication by 1930
        self.assertEqual(self.ok([A("X", 1945)], None), (False, "no_pub_year"))

    def test_anonymous_needs_year(self):
        self.assertTrue(self.ok([A("Anonymous", None)], 1850)[0])
        self.assertEqual(self.ok([A("Anonymous", None)], None), (False, "no_pub_year"))
        self.assertEqual(self.ok([A("Various", None)], 1900), (False, "various_authors"))

    def test_copyrighted_source(self):
        self.assertEqual(self.ok([A("X", 1800)], 1790, us=False), (False, "copyrighted"))

    def test_bce(self):
        self.assertTrue(self.ok([A("Homer", -700, -750)], -700)[0])

    def test_licenses(self):
        n = rights.normalise_license
        self.assertEqual(n("https://creativecommons.org/publicdomain/zero/1.0/"), "cc0")
        self.assertEqual(n("CC BY-SA 4.0"), "cc-by-sa")
        self.assertEqual(n("https://creativecommons.org/licenses/by/4.0/"), "cc-by")
        self.assertEqual(n("GNU Free Documentation License 1.3"), "gfdl")
        self.assertIsNone(n("CC BY-NC-SA 4.0"))
        self.assertIsNone(n("https://creativecommons.org/licenses/by-nd/4.0/"))
        self.assertIsNone(n("All rights reserved"))
        self.assertEqual(n("MIT"), "mit")


class Dedup(unittest.TestCase):
    def rec(self, id, src, t, a, pop=0, lang="en", **kw):
        r = {"id": id, "src": src, "t": t, "a": a, "pop": pop, "lang": lang}
        r.update(kw)
        return r

    def test_source_priority(self):
        kept, dropped = dedup.dedupe([
            self.rec("pg1342", "pg", "Pride and Prejudice", "Jane Austen", 190000),
            self.rec("se-austen-pride", "se", "Pride and Prejudice", "Jane Austen", 0),
            self.rec("ws-x", "ws", "Pride and prejudice", "Jane Austen", 0),
        ])
        self.assertEqual([r["id"] for r in kept], ["se-austen-pride"])
        self.assertEqual(kept[0]["pop"], 190000)   # popularity carried over to the winner
        self.assertEqual(len(dropped), 2)

    def test_popularity_inside_source(self):
        kept, _ = dedup.dedupe([self.rec("pg1", "pg", "Emma", "Jane Austen", 10),
                                self.rec("pg2", "pg", "Emma", "Jane Austen", 500)])
        self.assertEqual(kept[0]["id"], "pg2")

    def test_volumes_and_languages_stay_apart(self):
        kept, _ = dedup.dedupe([
            self.rec("pg1", "pg", "Les Misérables, Tome I", "Victor Hugo", lang="fr"),
            self.rec("pg2", "pg", "Les Misérables, Tome II", "Victor Hugo", lang="fr"),
            self.rec("pg3", "pg", "Guerra e pace: Volume 1", "Lev Tolstoj", lang="it"),
            self.rec("pg4", "pg", "Guerra e pace: Volume 2", "Lev Tolstoj", lang="it"),
            self.rec("pg5", "pg", "Emma", "Jane Austen", lang="en"),
            self.rec("pg6", "pg", "Emma", "Jane Austen", lang="fr"),
        ])
        self.assertEqual(len(kept), 6)

    def test_same_title_other_author(self):
        kept, _ = dedup.dedupe([self.rec("pg1", "pg", "Poems", "John Keats"),
                                self.rec("pg2", "pg", "Poems", "Emily Dickinson")])
        self.assertEqual(len(kept), 2)

    def test_explicit_dup(self):
        kept, _ = dedup.dedupe([self.rec("pg2814", "pg", "Dubliners", "James Joyce", 5000),
                                self.rec("se-joyce-dub", "se", "Dubliners (Collection)", "J. Joyce", 0,
                                         dup_of=["pg2814"])])
        self.assertEqual([r["id"] for r in kept], ["se-joyce-dub"])


class Categories(unittest.TestCase):
    def test_shelves(self):
        c = categories.pg_categories(["Category: Novels", "Category: Romance", "Category: Classics of Literature",
                                      "Category: British Literature"], ["PR"], ["Love stories"])
        self.assertEqual(c, ["romance", "novel", "classic"])

    def test_practical_subjects_win(self):
        c = categories.pg_categories(["Category: How To ..."], ["TX"], ["Cookery, Italian"])
        self.assertEqual(c[0], "cooking")
        c = categories.pg_categories([], ["TT"], ["Woodwork -- Handbooks, manuals, etc."])
        self.assertIn("howto", c)

    def test_locc_fallback(self):
        self.assertEqual(categories.pg_categories([], ["QA"], []), ["maths"])
        self.assertEqual(categories.pg_categories([], ["QA76"], []), ["computing"])
        self.assertEqual(categories.pg_categories([], ["PQ"], []), ["classic"])

    def test_max_and_known_ids(self):
        c = categories.pg_categories(["Category: %s" % s for s in categories.PG_SHELF], ["PR"], [])
        self.assertLessEqual(len(c), categories.MAX_CATS)
        self.assertTrue(all(x in categories.CATS for x in c))

    def test_names_in_five_languages(self):
        for cid, names in categories.CATS.items():
            self.assertEqual(len(names), 5, cid)
        idx = categories.index_cats()
        self.assertEqual(set(idx[0]["names"]), {"it", "en", "es", "fr", "de"})


class Quality(unittest.TestCase):
    def test_reject_periodicals_and_slices(self):
        b = {"type": "Text", "title": "Punch, or the London Charivari, Vol. 98, May 24, 1890", "shelves": [],
             "subjects": []}
        self.assertEqual(quality.pg_metadata_reject(b, 50000, ["humour"]), "periodical")
        b = {"type": "Text", "title": "Encyclopaedia Britannica, 11th Edition, Volume 2, Slice 3", "shelves": [],
             "subjects": []}
        self.assertEqual(quality.pg_metadata_reject(b, 900000, ["reference"]), "encyclopedia_slice")
        b = {"type": "Text", "title": "A Book", "shelves": ["Category: Journals"], "subjects": []}
        self.assertEqual(quality.pg_metadata_reject(b, 900000, []), "periodical")

    def test_tiny_and_ok(self):
        b = {"type": "Text", "title": "A Story", "shelves": [], "subjects": ["Fiction"]}
        self.assertEqual(quality.pg_metadata_reject(b, 5000, ["novel"]), "too_short")
        self.assertIsNone(quality.pg_metadata_reject(b, 300000, ["novel"]))
        self.assertIsNone(quality.pg_metadata_reject(b, 12000, ["poetry"]))

    def test_dead_language_courses(self):
        def rej(subject):
            return quality.pg_metadata_reject({"type": "Text", "title": "X", "shelves": [], "subjects": [subject]},
                                              50000, ["language"])
        self.assertEqual(rej("Latin language -- Grammar"), "dead_language_course")
        self.assertEqual(rej("English language -- Old English, ca. 450-1100 -- Grammar"), "dead_language_course")
        self.assertIsNone(rej("English language -- Grammar"))
        self.assertIsNone(rej("Greek language, Modern -- Grammar"))
        self.assertIsNone(rej("Italian language -- Grammar"))

    def test_outdated_medical(self):
        b = {"type": "Text", "title": "Household Medicine", "shelves": [], "subjects": ["Medicine, Popular"]}
        self.assertEqual(quality.pg_metadata_reject(b, 300000, ["health"]), "outdated_medical")

    def test_text_heuristic(self):
        good = ("It was the best of times, it was the worst of times, it was the age of wisdom. " * 200)
        self.assertTrue(quality.text_quality(good)[0])
        ocr = ("Tlie qnick b r o w n f o x jumpcd ovcr tlie 1azy d o g , ; ' ' . " * 300)
        self.assertFalse(quality.text_quality(ocr)[0])
        noise = ("~~## @@ %% ¤¤ §§ " * 1000)
        self.assertFalse(quality.text_quality(noise)[0])


class Curation(unittest.TestCase):
    def test_english_policy(self):
        k = curate.keep
        self.assertTrue(k({"lang": "en", "src": "pg", "c": ["cooking"], "pop": 1})[0])
        self.assertTrue(k({"lang": "en", "src": "se", "c": ["novel"], "pop": 0})[0])
        self.assertFalse(k({"lang": "en", "src": "pg", "c": ["novel"], "pop": 900})[0])
        self.assertTrue(k({"lang": "en", "src": "pg", "c": ["history"], "pop": 900})[0])
        self.assertTrue(k({"lang": "it", "src": "pg", "c": ["novel"], "pop": 1})[0])


class Misc(unittest.TestCase):
    def test_person_name(self):
        self.assertEqual(describe.person_name("Austen, Jane"), "Jane Austen")
        self.assertEqual(describe.person_name("Kennedy, John F. (John Fitzgerald)"), "John F. Kennedy")
        self.assertEqual(describe.person_name("Dante Alighieri"), "Dante Alighieri")
        self.assertEqual(describe.person_name("Orczy, Emmuska Orczy, Baroness"), "Emmuska Orczy")
        self.assertEqual(describe.person_name("Doyle, Arthur Conan, Sir"), "Arthur Conan Doyle")

    def test_no_trademark_in_descriptions(self):
        s = describe.clean_summary('"X" by Y is a novel. This Project Gutenberg edition has notes. It tells of Z. '
                                   '(This is an automatically generated summary.)')
        self.assertEqual(s, '"X" by Y is a novel. It tells of Z.')

    def test_split_title(self):
        s = describe.split_title
        self.assertEqual(s("I promessi sposi.: Opere di Alessando Manzoni, vol. 1"),
                         ("I promessi sposi", "Opere di Alessando Manzoni, vol. 1"))
        self.assertEqual(s("Le avventure di Pinocchio: Storia di un burattino"),
                         ("Le avventure di Pinocchio", "Storia di un burattino"))
        self.assertEqual(s("Pride and Prejudice [Illustrated]"), ("Pride and Prejudice", ""))
        self.assertEqual(s("Eh! la vita....: Novelle"), ("Eh! la vita....", "Novelle"))
        self.assertEqual(describe.person_name("Azeglio, Massimo d'"), "Massimo d'Azeglio")

    def test_canon_rank(self):
        from bk import rank
        rank.sitelinks = lambda qids, log=print: {}
        rows = [{"id": "pg1", "lang": "it", "t": "Ricordi d'infanzia e di scuola", "a": "Edmondo De Amicis", "pop": 9700},
                {"id": "pg2", "lang": "it", "t": "Le avventure di Pinocchio", "a": "Carlo Collodi", "pop": 3000}]
        rank.apply(rows, log=lambda *a: None)
        self.assertGreater(rows[1]["pop"], rows[0]["pop"])
        self.assertEqual(rows[0]["_downloads"], 9700)

    def test_lang_pick(self):
        self.assertEqual(pick_lang(["it"]), "it")
        self.assertEqual(pick_lang(["de", "en"]), "en")
        self.assertEqual(pick_lang(["la", "fr"]), "fr")

    def test_description_cut(self):
        # no real description -> empty, never a line rebuilt from author/dates/categories
        self.assertEqual(describe.description("it", "Giovanni Verga", "1840-1922", ["novel", "classic"], 1881), "")
        self.assertEqual(describe.description("it", "", "", [], None, None, None, "romanzo verista ambientato ad Aci Trezza"),
                         "Romanzo verista ambientato ad Aci Trezza.")
        rec = {"lang": "it", "a": "Edmondo De Amicis", "ad": "1846-1908", "c": ["biography"], "y": None}
        self.assertTrue(describe.is_filler("Edmondo De Amicis (1846-1908). Biografie.", rec))
        self.assertFalse(describe.is_filler("Ricordi della scuola torinese e dei maestri dell'autore.", rec))
        long = "A sentence that is long enough. " * 30
        self.assertLessEqual(len(describe.description("en", "X", "", [], None, long, "en")), describe.D_MAX)

    def test_epub_validator(self):
        import tempfile
        d = tempfile.mkdtemp()
        good = os.path.join(d, "g.epub")
        with zipfile.ZipFile(good, "w") as z:
            z.writestr("mimetype", "application/epub+zip")
            z.writestr("META-INF/container.xml",
                       '<?xml version="1.0"?><container version="1.0" xmlns="urn:oasis:names:tc:opendocument:'
                       'xmlns:container"><rootfiles><rootfile full-path="OEBPS/c.opf" media-type="application/'
                       'oebps-package+xml"/></rootfiles></container>')
            z.writestr("OEBPS/c.opf",
                       '<package xmlns="http://www.idpf.org/2007/opf" version="3.0"><metadata xmlns:dc="http://'
                       'purl.org/dc/elements/1.1/"><dc:title>T</dc:title><dc:language>en</dc:language></metadata>'
                       '<manifest><item id="a" href="a.xhtml" media-type="application/xhtml+xml"/></manifest>'
                       '<spine><itemref idref="a"/></spine></package>')
            z.writestr("OEBPS/a.xhtml", "<html><body><p>Hello world</p></body></html>")
        r = epubcheck.check(good)
        self.assertTrue(r["ok"], r["error"])
        self.assertIn("Hello world", r["text"])
        bad = os.path.join(d, "b.epub")
        with zipfile.ZipFile(bad, "w") as z:
            z.writestr("mimetype", "application/epub+zip")
            z.writestr("META-INF/container.xml", "<container/>")
        self.assertFalse(epubcheck.check(bad)["ok"])
        notzip = os.path.join(d, "n.epub")
        with open(notzip, "wb") as f:
            f.write(b"not a zip")
        self.assertFalse(epubcheck.check(notzip)["ok"])


if __name__ == "__main__":
    unittest.main()
