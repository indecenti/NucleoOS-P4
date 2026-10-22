"""Rights filter (BOOKS_PLAN D3).

A book is free to distribute from GitHub (US) to boards in Italy/EU only if:

* public domain: every contributor (author, translator, illustrator, editor, ...) died by DEATH_LIMIT
  (EU: life + 70 years) AND the work was first published by PUB_LIMIT (US: 95 years);
* or it carries a free licence that allows redistribution outside personal use (FREE_LICENSES; no NC/ND).

Missing or unverifiable data means the book is excluded. Every decision returns a reason code so the
per-language report can count exclusions.
"""
import re

DEATH_LIMIT = 1955       # died in 1955 -> public domain in the EU from 1 Jan 2026
PUB_LIMIT = 1930         # published in 1930 -> public domain in the US from 1 Jan 2026

FREE_LICENSES = {"pd", "cc0", "cc-by", "cc-by-sa", "gfdl", "mit", "apache-2.0", "bsd", "gpl", "ofl"}

# Contributor names that carry no person and so no death date.
ANON_NAMES = {"anonymous", "unknown", "anonyme", "anonimo", "anónimo", "anonym", "anoniem", "nameless"}
VARIOUS_NAMES = {"various", "diversi", "varios", "divers", "verschiedene"}
# Roles that are not authorship of the text (a printer or publisher holds no copyright in it).
IGNORED_ROLES = {"pbl", "prt", "dub", "frg"}   # dub = dubious author (the real author is listed apart)


def normalise_license(s):
    """Map a licence string/URL to our short code, or None if it is not a free licence we accept."""
    if not s:
        return None
    t = s.strip().lower()
    if re.search(r"\b(nc|nd)\b|noncommercial|non-commercial|noderiv|no-deriv|non commerciale", t):
        return None
    if "publicdomain/zero" in t or t in ("cc0", "cc0-1.0", "cc0 1.0"):
        return "cc0"
    if "public domain" in t or t in ("pd", "publicdomain", "public-domain") or "publicdomain/mark" in t:
        return "pd"
    if re.search(r"by-sa|by_sa|attribution-sharealike|attribution share ?alike", t):
        return "cc-by-sa"
    if re.search(r"licenses/by/|\bcc[- ]by\b(?!-)|attribution \d|^cc-by-[\d.]+$", t):
        return "cc-by"
    if "gfdl" in t or "gnu free documentation" in t:
        return "gfdl"
    if t.startswith("mit") or "mit license" in t:
        return "mit"
    if "apache" in t:
        return "apache-2.0"
    if t.startswith("bsd") or "bsd license" in t:
        return "bsd"
    if re.match(r"gpl|gnu general public", t):
        return "gpl"
    if "ofl" in t or "open font" in t:
        return "ofl"
    return None


def name_kind(name):
    n = (name or "").strip().lower()
    if n in ANON_NAMES or n.startswith("anonymous"):
        return "anon"
    if n in VARIOUS_NAMES:
        return "various"
    return "person"


def check_public_domain(agents, pub_year, us_pd=True, death_limit=DEATH_LIMIT, pub_limit=PUB_LIMIT):
    """Decide whether a public-domain claim holds in both the US and the EU.

    agents: [{"name", "birth", "death", "role"}]; pub_year: first publication year or None;
    us_pd: the source asserts (and has cleared) public domain in the USA.
    Returns (ok, reason). reason is "ok" or an exclusion code.
    """
    if not us_pd:
        return False, "copyrighted"
    people = [a for a in agents if a.get("role", "aut") not in IGNORED_ROLES]
    if not people:
        return False, "no_author"
    deaths = []
    anon = False
    for a in people:
        kind = name_kind(a.get("name"))
        if kind == "various":
            return False, "various_authors"
        if kind == "anon":
            anon = True
            continue
        d = a.get("death")
        if d is None:
            if a.get("birth") is not None and a["birth"] > death_limit - 100:
                return False, "author_maybe_alive"
            return False, "no_death_date"
        if d > death_limit:
            return False, "died_after_%d" % death_limit
        deaths.append(d)
    if pub_year is not None:
        if pub_year > pub_limit:
            return False, "published_after_%d" % pub_limit
        return True, "ok"
    # No recorded publication year. A work cannot be published (by its author) after the last
    # contributor's death; posthumous first editions after the limit would be under US copyright and
    # the source would not mark them public domain in the USA, which us_pd already requires.
    if deaths and not anon and max(deaths) <= pub_limit:
        return True, "ok"
    return False, "no_pub_year"
