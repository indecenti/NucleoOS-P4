"""Standard Ebooks source (CC0 production of US-public-domain texts, English).

Their OPDS/bulk feeds need a paid membership and their robots.txt blocks AI agents from the download
URLs, so nothing is downloaded from standardebooks.org. Instead each book is built from its public
CC0 source repository on GitHub (github.com/standardebooks/<repo>) with their own toolset
(`se build`, pip package `standardebooks`, run in WSL). The result is the same "compatible" EPUB they
publish.

Rights: SE clears US public domain; the EU side is checked here like every other source (every
author/translator/illustrator/cover artist dead by 1955, published by 1930), with dates from Wikidata
via the Wikipedia links in the OPF, falling back to the matching Gutenberg record (dc:source).
"""
import re
import subprocess
import zlib
import xml.etree.ElementTree as ET
from collections import Counter, defaultdict

from . import describe, rights
from .common import CACHE, Http, load_json, save_json
from .wikidata import Wikidata

SE_DIR = CACHE / "se"
OPF_DIR = SE_DIR / "opf"
EPUB_DIR = SE_DIR / "epub"
REPOS = SE_DIR / "repos.tsv"
CAND = CACHE / "candidates-se.json"
WSL_VENV_SE = "~/se-venv/bin/se"

OPF = "{http://www.idpf.org/2007/opf}"
DC = "{http://purl.org/dc/elements/1.1/}"
# Roles whose work is in the book text or pictures (producers, transcribers etc. are CC0 contributors)
RIGHTS_ROLES = {"aut", "trl", "ill", "edt", "art", "aui", "aft", "com", "cmm", "wpr", "ann"}

GENRE = {
    "Adventure": "adventure", "Autobiography": "biography", "Biography": "biography", "Childrens": "children",
    "Comedy": "humour", "Drama": "drama", "Fantasy": "scifi", "Fiction": "novel", "Horror": "scifi",
    "Memoir": "biography", "Mystery": "crime", "Nonfiction": "essays", "Philosophy": "philosophy",
    "Poetry": "poetry", "Satire": "humour", "Science Fiction": "scifi", "Shorts": "short",
    "Spirituality": "religion", "Travel": "travel", "History": "history", "Romance": "romance",
}


def list_repos():
    if not REPOS.exists():
        SE_DIR.mkdir(parents=True, exist_ok=True)
        out = subprocess.run(["gh", "api", "--paginate", "orgs/standardebooks/repos?per_page=100&type=public",
                              "--jq", '.[] | [.name, .default_branch, .archived, .size] | @tsv'],
                             capture_output=True, text=True, check=True).stdout
        REPOS.write_text(out, encoding="utf-8")
    repos = []
    for line in REPOS.read_text(encoding="utf-8").splitlines():
        name, branch, archived, _ = line.split("\t")
        if "_" in name and archived == "false":       # book repos are author_title
            repos.append((name, branch))
    return sorted(set(repos))


def fetch_opfs(repos):
    OPF_DIR.mkdir(parents=True, exist_ok=True)
    http = Http(delay=0.2)
    for name, branch in repos:
        p = OPF_DIR / (name + ".opf")
        if p.exists():
            continue
        r = http.get("https://raw.githubusercontent.com/standardebooks/%s/%s/src/epub/content.opf" % (name, branch))
        if r.status_code == 200:
            p.write_bytes(r.content)
        else:
            print("no opf", name, r.status_code)


def parse_opf(data):
    root = ET.fromstring(data)
    md = root.find(OPF + "metadata")
    refines = defaultdict(list)
    for m in md.findall(OPF + "meta"):
        if m.get("refines"):
            refines[m.get("refines")[1:]].append((m.get("property"), (m.text or "").strip()))
    links = defaultdict(list)
    for l in md.findall(OPF + "link"):
        links[(l.get("refines") or "")[1:]].append(l.get("href"))
    b = {
        "url": md.findtext(DC + "identifier", "").strip(),
        "title": md.findtext(DC + "title", "").strip(),
        "lang": md.findtext(DC + "language", "en").strip(),
        "sources": [s.text.strip() for s in md.findall(DC + "source") if s.text],
        "genres": [m.text for m in md.findall(OPF + "meta") if m.get("property") == "schema:genre" and m.text],
        "abstract": next((m.text.strip() for m in md.findall(OPF + "meta")
                          if m.get("property") == "schema:abstract" and m.text), ""),
        "subjects": [s.text for s in md.findall(DC + "subject") if s.text],
        "words": next((int(m.text) for m in md.findall(OPF + "meta")
                       if m.get("property") == "schema:wordCount" and m.text and m.text.isdigit()), 0),
        "work_wiki": [h for h in links[""] if "wikipedia.org" in (h or "")],
    }
    agents = []
    for el in md.findall(DC + "creator") + md.findall(DC + "contributor"):
        aid = el.get("id") or ""
        roles = [v for k, v in refines[aid] if k == "role"] or (["aut"] if el.tag == DC + "creator" else [])
        agents.append({
            "name": (el.text or "").strip(),
            "file_as": next((v for k, v in refines[aid] if k == "file-as"), ""),
            "roles": roles,
            "wiki": [h for h in links[aid] if "wikipedia.org" in (h or "")],
            "loc": [h for h in links[aid] if "id.loc.gov" in (h or "")],
        })
    b["agents"] = agents
    return b


_LOC_CACHE = CACHE / "loc-names.json"
_loc = None


def loc_dates(url, birth=None):
    """Library of Congress name authority heading 'Joyce, James, 1882-1941' -> (1882, 1941)."""
    global _loc
    if _loc is None:
        _loc = load_json(_LOC_CACHE, {}) or {}
    m = re.search(r"(n[a-z]?\d+)", url)
    if not m:
        return birth, None
    key = m.group(1)
    if key not in _loc:
        r = Http(delay=0.5).get("https://id.loc.gov/authorities/names/%s.json" % key)
        labels = re.findall(r'authoritativeLabel"\s*:\s*\[?\{?[^}]*?"@value"\s*:\s*"([^"]+)', r.text)             if r.status_code == 200 else []
        _loc[key] = labels[0] if labels else ""
        save_json(_LOC_CACHE, _loc, compact=True)
    d = re.search(r"(\d{3,4})\??-(\d{3,4})\b", _loc[key])
    if d:
        return int(d.group(1)), int(d.group(2))
    return birth, None


def _pg_ids(sources):
    out = []
    for s in sources:
        m = re.search(r"gutenberg\.org/(?:ebooks|files)/(\d+)", s)
        if m:
            out.append(int(m.group(1)))
    return out


def meta(log=print):
    repos = list_repos()
    log("se: %d book repos" % len(repos))
    fetch_opfs(repos)
    books = []
    for name, _ in repos:
        p = OPF_DIR / (name + ".opf")
        if p.exists():
            try:
                b = parse_opf(p.read_bytes())
            except ET.ParseError as e:
                log("bad opf %s: %s" % (name, e))
                continue
            b["repo"] = name
            books.append(b)
    pg_meta = {b["id"]: b for b in (load_json(CACHE / "pg" / "pg_meta.json") or [])}
    wd = Wikidata()
    urls = set()
    for b in books:
        for a in b["agents"]:
            if set(a["roles"]) & RIGHTS_ROLES and a["wiki"]:
                urls.add(a["wiki"][0])
        urls.update(b["work_wiki"][:1])
    res = wd.lookup(sorted(urls))
    recs, excl = [], defaultdict(Counter)
    for b in books:
        lang = "en"
        pgids = _pg_ids(b["sources"])
        pgrecs = [pg_meta[i] for i in pgids if i in pg_meta]
        pg_agents = [a for r in pgrecs for a in r["agents"]]
        agents = []
        for a in b["agents"]:
            if not set(a["roles"]) & RIGHTS_ROLES:
                continue
            birth = death = None
            if a["wiki"]:
                w = res.get(a["wiki"][0]) or {}
                birth, death = w.get("birth"), w.get("death")
            if death is None and a["loc"]:
                birth, death = loc_dates(a["loc"][0], birth)
            if death is None:      # fall back to the Gutenberg agent with the same surname
                sur = describe.norm_key(a["file_as"].split(",")[0] if a["file_as"] else a["name"].split(" ")[-1])
                for pa in pg_agents:
                    if pa.get("death") is not None and describe.norm_key(pa["name"].split(",")[0]) == sur:
                        birth, death = pa.get("birth"), pa["death"]
                        break
            agents.append({"name": a["name"], "role": a["roles"][0], "birth": birth, "death": death})
        years = [r.get("y906") or r.get("y260") or r.get("y_wd") for r in pgrecs]
        years = [y for y in years if y]
        if b["work_wiki"]:
            w = res.get(b["work_wiki"][0]) or {}
            if w.get("pub") and not w.get("human"):
                years.append(w["pub"])
        y = min(years) if years else None
        ok, why = rights.check_public_domain(agents, y, us_pd=True)
        if not ok:
            excl[lang]["rights:" + why] += 1
            continue
        cats = []
        for g in b["genres"]:
            if g in GENRE:
                cats.append(GENRE[g])
        if "novel" in cats and "short" in cats:
            cats.remove("novel")
        from .categories import order, MAX_CATS
        cats = order(cats + ["classic"])[:MAX_CATS]
        auts = [a for a in agents if a["role"] == "aut"] or agents[:1]
        author = " & ".join(a["name"] for a in auts[:3])
        ad = describe.dates(auts[0]["birth"], auts[0]["death"], lang) if auts else ""
        pop = max([r.get("downloads", 0) for r in pgrecs] or [0])
        sid = "se-" + re.sub(r"[^a-z0-9]+", "-", b["url"].rsplit("/ebooks/", 1)[-1].lower()).strip("-")
        if len(sid) > 40:
            sid = sid[:40].rstrip("-")
        recs.append({
            "id": sid, "lang": lang, "t": describe.clean_title(b["title"]), "a": author, "ad": ad, "y": y,
            "c": cats, "src": "se", "lic": "cc0", "pop": pop,
            "d": describe._cut(b["abstract"]) if b["abstract"] else describe.description(lang, author, ad, cats, y),
            "u2": b["url"], "dup_of": ["pg%d" % i for i in pgids],
            "_repo": b["repo"], "_file": str(EPUB_DIR / (b["repo"] + ".epub")),
            "_qid": (res.get(b["work_wiki"][0]) or {}).get("qid") if b["work_wiki"] else None,
        })
    ids = Counter(r["id"] for r in recs)
    for r in recs:
        if ids[r["id"]] > 1:
            r["id"] = (r["id"][:36] + "-" + str(zlib.crc32(r["_repo"].encode()) % 1000))[:40]
    save_json(CAND, {"records": recs, "excluded": excl})
    log("se candidates: %d, excluded %s" % (len(recs), dict(excl["en"])))
    return recs


def _build_one(r, dst):
    repo = r["_repo"]
    script = (
        "set -e; W=$(mktemp -d); cd $W; "
        "curl -sfL https://codeload.github.com/standardebooks/{repo}/tar.gz/HEAD | tar xz; "
        "D=$(ls -d */ | head -1); {se} build --output-dir=$W/out \"$D\" >/dev/null 2>&1; "
        "F=$(ls $W/out/*.epub | grep -v _advanced | head -1); "
        "cp \"$F\" {dst}/{repo}.epub.part && mv {dst}/{repo}.epub.part {dst}/{repo}.epub; "
        "rm -rf $W").format(repo=repo, se=WSL_VENV_SE, dst=dst)
    p = subprocess.run(["wsl", "-e", "bash", "-lc", script], capture_output=True, text=True)
    return repo, p.returncode, (p.stderr or "").strip()[-300:]


def fetch(log=print, workers=4):
    """Build every candidate's EPUB from its GitHub source (codeload tarball + `se build` in WSL)."""
    from concurrent.futures import ThreadPoolExecutor
    from .src_pg import _wsl_path
    js = load_json(CAND)
    EPUB_DIR.mkdir(parents=True, exist_ok=True)
    todo = [r for r in js["records"] if not (EPUB_DIR / (r["_repo"] + ".epub")).exists()]
    log("se-fetch: %d books to build" % len(todo))
    dst = _wsl_path(EPUB_DIR)
    with ThreadPoolExecutor(workers) as ex:
        for i, (repo, rc, err) in enumerate(ex.map(lambda r: _build_one(r, dst), todo)):
            if rc != 0:
                log("se build failed %s: %s" % (repo, err))
            if i % 50 == 0:
                log("  se %d/%d" % (i, len(todo)))
