"""Catalog categories (ids + names in it/en/es/fr/de) and the mapping from source metadata.

Sources: Gutenberg "Category: ..." bookshelves first (curated by PG), then Library of Congress
Classification (LoCC) and LCSH subjects as fallback. A book gets at most MAX_CATS ids, most specific first.
"""
import re

MAX_CATS = 3

# id: (it, en, es, fr, de)
CATS = {
    "novel":      ("Romanzi", "Novels", "Novelas", "Romans", "Romane"),
    "short":      ("Racconti", "Short stories", "Cuentos", "Nouvelles", "Erzählungen"),
    "poetry":     ("Poesia", "Poetry", "Poesía", "Poésie", "Lyrik"),
    "drama":      ("Teatro", "Drama", "Teatro", "Théâtre", "Drama"),
    "classic":    ("Classici", "Classics", "Clásicos", "Classiques", "Klassiker"),
    "adventure":  ("Avventura", "Adventure", "Aventura", "Aventure", "Abenteuer"),
    "scifi":      ("Fantascienza e fantasy", "Science fiction & fantasy", "Ciencia ficción y fantasía",
                   "Science-fiction et fantasy", "Science-Fiction und Fantasy"),
    "crime":      ("Gialli e thriller", "Crime & mystery", "Policiaca y misterio", "Policier et mystère",
                   "Krimis und Thriller"),
    "historical": ("Romanzi storici", "Historical fiction", "Novela histórica", "Romans historiques",
                   "Historische Romane"),
    "romance":    ("Rosa", "Romance", "Romántica", "Romance", "Liebesromane"),
    "humour":     ("Umorismo", "Humour", "Humor", "Humour", "Humor"),
    "children":   ("Ragazzi", "Children & young adults", "Infantil y juvenil", "Jeunesse", "Kinder und Jugend"),
    "myth":       ("Miti, leggende e fiabe", "Myths, legends & folklore", "Mitos, leyendas y folclore",
                   "Mythes, légendes et contes", "Mythen, Sagen und Märchen"),
    "comics":     ("Fumetti", "Comics", "Cómics", "Bandes dessinées", "Comics"),
    "philosophy": ("Filosofia", "Philosophy", "Filosofía", "Philosophie", "Philosophie"),
    "religion":   ("Religione", "Religion", "Religión", "Religion", "Religion"),
    "history":    ("Storia", "History", "Historia", "Histoire", "Geschichte"),
    "biography":  ("Biografie", "Biographies", "Biografías", "Biographies", "Biografien"),
    "essays":     ("Saggi e lettere", "Essays & letters", "Ensayos y cartas", "Essais et lettres",
                   "Essays und Briefe"),
    "travel":     ("Viaggi", "Travel", "Viajes", "Voyages", "Reisen"),
    "science":    ("Scienze", "Science", "Ciencia", "Sciences", "Naturwissenschaften"),
    "maths":      ("Matematica", "Mathematics", "Matemáticas", "Mathématiques", "Mathematik"),
    "nature":     ("Natura, animali e giardino", "Nature, animals & gardening", "Naturaleza, animales y jardín",
                   "Nature, animaux et jardin", "Natur, Tiere und Garten"),
    "cooking":    ("Cucina", "Cooking", "Cocina", "Cuisine", "Kochen"),
    "health":     ("Salute", "Health", "Salud", "Santé", "Gesundheit"),
    "survival":   ("Sopravvivenza e primo soccorso", "Survival & first aid", "Supervivencia y primeros auxilios",
                   "Survie et premiers secours", "Überleben und Erste Hilfe"),
    "howto":      ("Manuali e fai da te", "How-to & DIY", "Manuales y bricolaje", "Guides pratiques et bricolage",
                   "Ratgeber und Heimwerken"),
    "tech":       ("Tecnica e ingegneria", "Engineering & technology", "Ingeniería y tecnología",
                   "Technique et ingénierie", "Technik"),
    "computing":  ("Informatica", "Computing", "Informática", "Informatique", "Informatik"),
    "language":   ("Lingue", "Languages", "Idiomas", "Langues", "Sprachen"),
    "art":        ("Arte e architettura", "Art & architecture", "Arte y arquitectura", "Art et architecture",
                   "Kunst und Architektur"),
    "music":      ("Musica", "Music", "Música", "Musique", "Musik"),
    "society":    ("Società, politica ed economia", "Society, politics & economics", "Sociedad, política y economía",
                   "Société, politique et économie", "Gesellschaft, Politik und Wirtschaft"),
    "education":  ("Scuola e studio", "Education", "Educación", "Éducation", "Bildung"),
    "reference":  ("Consultazione", "Reference", "Consulta", "Ouvrages de référence", "Nachschlagewerke"),
    "sports":     ("Sport e hobby", "Sports & hobbies", "Deportes y aficiones", "Sports et loisirs",
                   "Sport und Hobby"),
}
UI_LANGS = ("it", "en", "es", "fr", "de")

# Gutenberg "Category: X" bookshelves -> our ids (None = carries no category of its own).
PG_SHELF = {
    "Novels": "novel",
    "Short Stories": "short",
    "Poetry": "poetry",
    "Plays/Films/Dramas": "drama",
    "Classics of Literature": "classic",
    "Adventure": "adventure",
    "Science-Fiction & Fantasy": "scifi",
    "Crime, Thrillers and Mystery": "crime",
    "Historical Novels": "historical",
    "Romance": "romance",
    "Humour": "humour",
    "Children & Young Adult Reading": "children",
    "Mythology, Legends & Folklore": "myth",
    "Philosophy & Ethics": "philosophy",
    "Religion/Spirituality": "religion",
    "History - Religious": "religion",
    "History - Modern (1750+)": "history",
    "History - American": "history",
    "History - European": "history",
    "History - Other": "history",
    "History - Warfare": "history",
    "History - British": "history",
    "History - Early Modern (c. 1450-1750)": "history",
    "History - Ancient": "history",
    "History - Medieval/Middle Ages": "history",
    "History - Royalty": "history",
    "History - Schools & Universities": "education",
    "Archaeology & Anthropology": "history",
    "Biographies": "biography",
    "Essays, Letters & Speeches": "essays",
    "Journalism/Media/Writing": "essays",
    "Travel Writing": "travel",
    "Science - Biology": "science",
    "Science - Physics": "science",
    "Science - Chemistry/Biochemistry": "science",
    "Science - Earth/Agricultural/Farming": "nature",
    "Mathematics": "maths",
    "Research Methods/Statistics/Information Sys": "maths",
    "Nature/Gardening/Animals": "nature",
    "Environmental Issues": "nature",
    "Cooking & Drinking": "cooking",
    "Nutrition": "health",
    "Health & Medicine": "health",
    "Drugs/Alcohol/Pharmacology": "health",
    "Psychiatry/Psychology": "science",
    "How To ...": "howto",
    "Engineering & Technology": "tech",
    "Architecture": "art",
    "Art": "art",
    "Fashion": "art",
    "Music": "music",
    "Language & Communication": "language",
    "Teaching & Education": "education",
    "Encyclopedias/Dictionaries/Reference": "reference",
    "Politics": "society",
    "Economics": "society",
    "Business/Management": "society",
    "Sociology": "society",
    "Law & Criminology": "society",
    "Gender & Sexuality Studies": "society",
    "Parenthood & Family Relations": "society",
    "Old Age & the Elderly": "society",
    "Sports/Hobbies": "sports",
    # Literature-by-country shelves say nothing about the kind of book.
    "British Literature": None, "American Literature": None, "French Literature": None,
    "German Literature": None, "Russian Literature": None, "Literature - Other": None,
    # Never shipped (see quality.py): magazines, reports, erotica.
    "Journals": None, "Reports & Conference Proceedings": None, "Sexuality & Erotica": None,
}

# Specific categories outrank generic ones when trimming to MAX_CATS.
PRIORITY = ["comics", "survival", "computing", "cooking", "maths", "howto", "tech", "language", "music",
            "scifi", "crime", "historical", "adventure", "romance", "myth", "children", "humour",
            "poetry", "drama", "short", "novel", "health", "nature", "science", "philosophy", "religion",
            "biography", "travel", "essays", "history", "society", "art", "sports", "education",
            "reference", "classic"]

# LoCC class prefixes (longest match first) -> id
LOCC = [
    ("QA76", "computing"), ("QA", "maths"), ("QB", "science"), ("QC", "science"), ("QD", "science"),
    ("QE", "science"), ("QH", "nature"), ("QK", "nature"), ("QL", "nature"), ("QM", "science"),
    ("QP", "science"), ("QR", "science"), ("Q", "science"),
    ("TX", "cooking"), ("TT", "howto"), ("TH", "howto"), ("TK", "tech"), ("TJ", "tech"), ("T", "tech"),
    ("SB", "nature"), ("SF", "nature"), ("SK", "sports"), ("S", "nature"),
    ("RC86", "survival"), ("RC", "health"), ("RA", "health"), ("RJ", "health"), ("R", "health"),
    ("PZ", "children"), ("PN6700", "comics"), ("PN6", "poetry"),
    ("BL", "religion"), ("BM", "religion"), ("BP", "religion"), ("BQ", "religion"), ("BR", "religion"),
    ("BS", "religion"), ("BT", "religion"), ("BV", "religion"), ("BX", "religion"), ("BF", "science"),
    ("B", "philosophy"), ("CT", "biography"), ("C", "history"), ("D", "history"), ("E", "history"),
    ("F", "history"), ("GV", "sports"), ("GR", "myth"), ("G", "travel"), ("H", "society"), ("J", "society"),
    ("K", "society"), ("L", "education"), ("ML", "music"), ("MT", "music"), ("M", "music"),
    ("NA", "art"), ("N", "art"), ("PE", "language"), ("PC", "language"), ("PF", "language"),
    ("P1", "language"), ("PB", "language"), ("PG", None), ("PQ", None), ("PR", None), ("PS", None),
    ("PT", None), ("A", "reference"), ("Z", "reference"), ("U", "history"), ("V", "history"),
]

# LCSH subject patterns -> id (checked before LoCC; specific beats generic)
SUBJECT_RX = [
    (r"comic books, strips|graphic novels|bandes dessinées|cartoons and caricatures", "comics"),
    (r"wilderness survival|survival skills|first aid|emergency medicine|camping|woodcraft|outdoor life", "survival"),
    (r"cookery|cooking|recipes|baking|beverages|food", "cooking"),
    (r"computer|programming", "computing"),
    (r"mathematics|algebra|geometry|arithmetic|calculus|trigonometry", "maths"),
    (r"carpentry|woodwork|handicraft|do-it-yourself|building --|masonry|blacksmith|boatbuilding|"
     r"upholstery|sewing|knitting|needlework|plumbing|tools", "howto"),
    (r"electric|radio|telegraph|engines|machinery|mechanical|engineering", "tech"),
    (r"gardening|horticulture|vegetable gardening|fruit-culture|beekeeping|poultry|agriculture", "nature"),
    (r"grammar|language -- |dictionaries|phrase books|conversation and phrase|readers \(", "language"),
    (r"science fiction|fantasy fiction", "scifi"),
    (r"detective and mystery|mystery fiction|crime -- fiction", "crime"),
    (r"historical fiction", "historical"),
    (r"adventure stories|sea stories", "adventure"),
    (r"love stories", "romance"),
    (r"fairy tales|folklore|legends|mythology", "myth"),
    (r"children's stories|juvenile fiction|juvenile literature", "children"),
    (r"humorous|wit and humor", "humour"),
    (r"short stories", "short"),
    (r"\bpoetry\b|\bpoems\b|sonnets|\bverse\b", "poetry"),
    (r"drama|tragedies|comedies|plays\b", "drama"),
    (r"\bfiction\b", "novel"),
    (r"biography|autobiography", "biography"),
    (r"travel|description and travel|voyages", "travel"),
    (r"philosophy|ethics", "philosophy"),
    (r"music", "music"),
]


def names(cid):
    return dict(zip(UI_LANGS, CATS[cid]))


def index_cats():
    return [{"id": cid, "names": names(cid)} for cid in CATS]


def _locc_cat(code):
    code = code.replace(" ", "")
    for pref, cid in LOCC:
        if code.startswith(pref):
            return cid, True
    return None, False


def order(cats):
    seen = []
    for c in cats:
        if c and c not in seen:
            seen.append(c)
    seen.sort(key=lambda c: PRIORITY.index(c) if c in PRIORITY else len(PRIORITY))
    return seen


def pg_categories(shelves, locc, subjects):
    """Our category ids for a Gutenberg record (most specific first, at most MAX_CATS)."""
    cats = []
    for s in shelves or []:
        if s.startswith("Category:"):
            cid = PG_SHELF.get(s.split(":", 1)[1].strip())
            if cid:
                cats.append(cid)
    subj = " | ".join(subjects or []).lower()
    for rx, cid in SUBJECT_RX:
        if re.search(rx, subj):
            # Subjects refine: specific practical categories always count; fiction genres only if the
            # shelves gave nothing more specific than "novel".
            if cid in ("comics", "survival", "cooking", "computing", "maths", "howto", "tech", "language") \
                    or not cats or cats == ["novel"]:
                cats.append(cid)
    if not cats:
        for code in locc or []:
            cid, _ = _locc_cat(code)
            if cid:
                cats.append(cid)
    if not cats and any(re.match(r"P[A-Z]", c or "") for c in (locc or [])):
        cats.append("classic")   # literature class with no genre: shown under Classics
    cats = order(cats)
    # "classic" is a badge, keep it but never let it push a real category out
    if "classic" in cats and len(cats) > MAX_CATS:
        cats = [c for c in cats if c != "classic"][:MAX_CATS - 1] + ["classic"]
    return cats[:MAX_CATS]
