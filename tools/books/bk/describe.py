"""Display fields: author names, dates, titles and the short description (`d`).

The description is built only from source metadata, never invented: the source's own summary when it
is in the book's language (Gutenberg summaries are English), otherwise a factual line in the book's
language made of author, dates, categories and first-publication year.
"""
import re
import unicodedata

from .categories import CATS, UI_LANGS

D_MAX = 240

FIRST_PUB = {
    "it": "Prima edizione", "en": "First published", "es": "Primera edición", "fr": "Première édition",
    "de": "Erstausgabe", "pt": "Primeira edição", "nl": "Eerste uitgave", "ca": "Primera edició",
    "sv": "Första utgåva", "da": "Første udgave", "no": "Første utgave", "fi": "Ensimmäinen painos",
    "hu": "Első kiadás", "pl": "Pierwsze wydanie", "cs": "První vydání", "eo": "Unua eldono",
    "el": "Πρώτη έκδοση", "ru": "Первое издание", "ro": "Prima ediție", "gl": "Primeira edición",
    "is": "Fyrsta útgáfa", "cy": "Cyhoeddwyd gyntaf", "af": "Eerste uitgawe", "fy": "Earste útjefte",
}
BCE = {"it": "a.C.", "es": "a. C.", "fr": "av. J.-C.", "de": "v. Chr.", "pt": "a.C.", "ca": "aC",
       "nl": "v.Chr.", "en": "BC"}


TITLES = r"(Sir|Lord|Lady|Baron|Baroness|Count|Countess|Graf|Gräfin|Freiherr|Freifrau|Jr\.|Sr\.|Dame|Saint|St\.|"          r"Mrs\.|Mr\.|Miss|Duke|Duchess|Marquis|Marquise|Marchesa|Marchese|Conte|Contessa|Viscount|Earl|Prince|"          r"Princess|Vicomte|Comte|Comtesse|pseud\.|Rev\.)"


def person_name(pg_name):
    """'Austen, Jane' -> 'Jane Austen'; 'Kennedy, John F. (John Fitzgerald)' -> 'John F. Kennedy';
    'Orczy, Emmuska Orczy, Baroness' -> 'Emmuska Orczy'."""
    n = re.sub(r"\s*\([^)]*\)", "", pg_name or "").strip()
    while True:
        m = re.sub(r",\s*%s$" % TITLES, "", n)
        if m == n:
            break
        n = m
    parts = [p.strip() for p in n.split(",")]
    if len(parts) >= 2 and parts[1] and not re.match(r"^\d", parts[1]):
        given, surname = parts[1], parts[0]
        n = given if given.split()[-1] == surname else given + " " + surname
    else:
        n = parts[0]
    return re.sub(r"\s+", " ", n).strip()


def year_str(y, lang):
    if y is None:
        return "?"
    if y < 0:
        return "%d %s" % (-y, BCE.get(lang, "BC"))
    return str(y)


def dates(birth, death, lang):
    if birth is None and death is None:
        return ""
    if birth is None:
        return "†" + year_str(death, lang)
    if death is None:
        return year_str(birth, lang) + "-"
    return "%s-%s" % (year_str(birth, lang), year_str(death, lang))


def clean_title(t):
    t = (t or "").replace("$b", "").replace("\r", "")
    lines = [l.strip(" :;") for l in t.split("\n") if l.strip()]
    t = lines[0] if lines else ""
    if len(lines) > 1 and len(t) < 30:
        t = t + ": " + lines[1]
    t = re.sub(r"\s+", " ", t).strip(" :;,")
    if len(t) > 140:
        cut = t[:140]
        t = cut[:cut.rfind(" ")] + "…" if " " in cut else cut
    return t


def _cut(s, n=D_MAX):
    s = re.sub(r"\s+", " ", s).strip()
    if len(s) <= n:
        return s
    sentences = re.split(r"(?<=[.!?])\s+", s)
    out = ""
    for sen in sentences:
        if len(out) + len(sen) + 1 > n:
            break
        out = (out + " " + sen).strip()
    if len(out) >= 60:
        return out
    cut = s[:n - 1]
    return cut[:cut.rfind(" ")] + "…"


def clean_summary(s):
    s = re.sub(r"\(This is an automatically generated summary\.\)", "", s or "")
    s = s.replace("�", "'")
    # the Gutenberg trademark never appears in our descriptions (PG licence): drop sentences naming it
    s = " ".join(x for x in re.split(r"(?<=[.!?])\s+", s) if "gutenberg" not in x.lower())
    return s.strip()


def description(lang, author, adates, cats, year, summary=None, summary_lang=None, wd_desc=None):
    if summary and summary_lang == lang:
        d = _cut(clean_summary(summary))
        if len(d) >= 40:
            return d
    bits = []
    if wd_desc:
        w = wd_desc.strip().rstrip(".")
        bits.append(w[:1].upper() + w[1:])
    surname = (author or "").split(",")[0].split(" ")[-1]
    if author and not (wd_desc and surname and surname.lower() in wd_desc.lower()):
        bits.append(author + (" (%s)" % adates if adates else ""))
    elif author and adates:
        bits[-1] += " (%s)" % adates if surname.lower() in bits[-1].lower() else ""
    if cats and lang in UI_LANGS:
        i = UI_LANGS.index(lang)
        bits.append(" · ".join(CATS[c][i] for c in cats))
    if year and lang in FIRST_PUB:
        bits.append("%s: %s" % (FIRST_PUB[lang], year_str(year, lang)))
    return _cut(". ".join(bits) + ".") if bits else ""


def norm_key(s):
    """Accent/case/punctuation-insensitive key for dedup."""
    s = unicodedata.normalize("NFKD", s or "")
    s = "".join(c for c in s if not unicodedata.combining(c)).lower()
    s = re.sub(r"[^a-z0-9α-ωа-я ]+", " ", s)
    s = re.sub(r"^(the|a|an|il|lo|la|i|gli|le|l|el|los|las|der|die|das|les|un|une|o|os|as) ", "", s.strip())
    return re.sub(r"\s+", " ", s).strip()
