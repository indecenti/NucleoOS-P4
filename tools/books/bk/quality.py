"""Usefulness and quality filters (BOOKS_PLAN D4).

Metadata stage (before any download): drop magazines, catalogues, reports, government papers,
encyclopaedia slices, indexes, tiny files, outdated medical advice, erotica. Content stage (after the
EPUB is downloaded): text-quality heuristic on the extracted text, so OCR garbage never ships.
"""
import re
import unicodedata

MIN_TEXT_BYTES = 20_000          # plain-text size below this is a pamphlet / single article
MIN_TEXT_BYTES_POETRY = 8_000    # poetry and drama collections can be short and still whole works
MAX_EPUB_BYTES = 40_000_000      # bigger is image-heavy; we pick the no-images edition instead
MAX_EPUB_BYTES_VISUAL = 80_000_000   # comics / art books need their pictures

BAD_SHELVES = {
    "Category: Journals": "periodical",
    "Category: Reports & Conference Proceedings": "report",
    "Category: Sexuality & Erotica": "erotica",
}
TITLE_RX = [
    (r"\bslice \d|\bvolume \d+, slice\b", "encyclopedia_slice"),
    (r"^(the )?project gutenberg|\bgutenberg (works|index|catalog)|index of the project", "index"),
    (r"\b(catalogue|catalog)\b(?! of (the )?(stars|birds|plants))|price[- ]list|\bdirectory of\b", "catalog"),
    (r"\bannual report|\breport of the\b|\bproceedings of\b|\btransactions of\b|\bbulletin\b|"
     r"\bhearings? before\b|\bcongressional\b|\bminutes of\b", "report"),
    (r"\b(magazine|journal|gazette|review|weekly|monthly|quarterly|almanac|almanack|annual)\b.*"
     r"\b(vol\.|volume|no\.|number|issue|\d{4})|\bpunch,? or the london charivari|"
     r"\b(vol|no)\. \d+,? (no|issue)\.? \d+", "periodical"),
    (r"\bcopyright renewals?\b|\bcatalog of copyright entries\b", "catalog"),
]
SUBJECT_RX = [
    (r"-- periodicals$|^periodicals|newspapers", "periodical"),
    (r"catalogs?$|-- catalogs|price lists|trade catalogs", "catalog"),
    (r"^law -- |laws, statutes|-- statutes|legislation|treaties", "government"),
    (r"united states\. congress|parliament -- |-- appropriations", "government"),
    (r"erotic|pornograph", "erotica"),
    (r"^registers|^directories|-- directories|telephone", "catalog"),
]
# Old medical / health advice is dangerous today: only modern curated manuals cover health.
OUTDATED_MEDICAL_RX = (r"medicine, popular|therapeutics|materia medica|hygiene|homeopathy|"
                       r"first aid in illness|diseases -- treatment|nursing|pharmacy|patent medicines")


def pg_metadata_reject(b, text_bytes, cats):
    """Reason code if a Gutenberg record is not worth shipping, else None."""
    if b.get("type") != "Text":
        return "not_text"
    title = (b.get("title") or "").lower()
    for s in b.get("shelves", []):
        if s in BAD_SHELVES:
            return BAD_SHELVES[s]
    for rx, why in TITLE_RX:
        if why and re.search(rx, title):
            return why
    for s in b.get("subjects", []):
        sl = s.lower()
        for rx, why in SUBJECT_RX:
            if re.search(rx, sl):
                return why
    if cats and cats[0] == "health" and any(re.search(OUTDATED_MEDICAL_RX, s.lower()) for s in b.get("subjects", [])):
        return "outdated_medical"
    if not cats and not b.get("subjects"):
        return "no_metadata"
    if text_bytes is None:
        return "no_text_file"
    short_ok = cats and cats[0] in ("poetry", "drama", "children", "comics")
    if text_bytes < (MIN_TEXT_BYTES_POETRY if short_ok else MIN_TEXT_BYTES):
        return "too_short"
    return None


_WORD = re.compile(r"[^\W\d_]+", re.UNICODE)


def text_quality(text):
    """Score extracted book text. Returns (ok, reason, stats).

    Proofread text: almost all characters are letters/spaces/punctuation, words have normal lengths,
    few one-letter fragments and few non-letter symbols. OCR garbage fails on one of these.
    """
    t = unicodedata.normalize("NFC", text or "")
    n = len(t)
    if n < 5000:
        return False, "too_little_text", {"chars": n}
    sample = t if n <= 400_000 else t[:200_000] + t[n // 2:n // 2 + 200_000]
    nonspace = [c for c in sample if not c.isspace()]
    letters = sum(1 for c in nonspace if c.isalpha())
    letter_ratio = letters / max(1, len(nonspace))
    words = _WORD.findall(sample)
    if not words:
        return False, "no_words", {"chars": n}
    avg = sum(len(w) for w in words) / len(words)
    single = sum(1 for w in words if len(w) == 1 and w.lower() not in "aeiouyàèéìòùåäöøæ") / len(words)
    odd = sum(1 for c in nonspace if unicodedata.category(c)[0] in "SC" and c not in "€$£%&+=<>|~^`") / max(1, len(nonspace))
    caps_mid = sum(1 for w in words if len(w) > 3 and any(ch.isupper() for ch in w[1:]) and not w.isupper()) / len(words)
    st = {"chars": n, "letters": round(letter_ratio, 3), "avg_word": round(avg, 2), "single": round(single, 3),
          "odd": round(odd, 4), "mixed_case": round(caps_mid, 3)}
    if letter_ratio < 0.72:
        return False, "low_letter_ratio", st
    if not 3.0 <= avg <= 9.5:
        return False, "odd_word_length", st
    if single > 0.08:
        return False, "fragmented_words", st
    if odd > 0.01:
        return False, "symbol_noise", st
    if caps_mid > 0.03:
        return False, "mixed_case_noise", st
    return True, "ok", st
