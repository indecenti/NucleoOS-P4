"""Project Gutenberg source.

Metadata: the offline catalog files (public domain) from https://www.gutenberg.org/cache/epub/feeds/
(pg_catalog.csv + rdf-files.tar.bz2). The RDF has everything the CSV has plus download counts,
author birth/death years, roles (translator, illustrator, editor...), original publication year
(marc906 / marc260) and the summary (marc520).

EPUB files: ONLY through the mirror route the PG robot policy and mirroring page allow, i.e. rsync
from the generated-files collection (gutenberg.pglaf.org::gutenberg-epub, ibiblio as fallback), one
`<id>/pg<id>-images-3.epub` per book. The website itself is never scraped. rsync runs in WSL.
"""
import re
import subprocess
import tarfile
import xml.etree.ElementTree as ET
from pathlib import Path

from .common import CACHE, Http, load_json, save_json

FEEDS = "https://www.gutenberg.org/cache/epub/feeds/"
PG_DIR = CACHE / "pg"
META_JSON = PG_DIR / "pg_meta.json"
EPUB_DIR = PG_DIR / "epub"
RSYNC_HOSTS = ["rsync://gutenberg.pglaf.org/gutenberg-epub/", "rsync://rsync.ibiblio.org/gutenberg-epub/"]

NS = {
    "rdf": "http://www.w3.org/1999/02/22-rdf-syntax-ns#",
    "dcterms": "http://purl.org/dc/terms/",
    "pgterms": "http://www.gutenberg.org/2009/pgterms/",
    "marcrel": "http://id.loc.gov/vocabulary/relators/",
    "dcam": "http://purl.org/dc/dcam/",
}
MARCREL = "{%s}" % NS["marcrel"]


def fetch_meta(force=False):
    PG_DIR.mkdir(parents=True, exist_ok=True)
    http = Http(delay=1.0)
    for name in ("pg_catalog.csv", "rdf-files.tar.bz2"):
        dest = PG_DIR / name
        if force or not dest.exists():
            print("fetch", FEEDS + name)
            http.download(FEEDS + name, dest)


def _int(s):
    try:
        return int(str(s).strip())
    except (TypeError, ValueError):
        return None


def _year_from_260(s):
    """'  $aLondon :$bHurst and Blackett, $c1906.' -> 1906 (the $c date subfield only)."""
    if not s:
        return None
    m = re.search(r"\$c\s*\[?c?(\d{4})", s)
    return int(m.group(1)) if m else None


def parse_rdf(xml_bytes):
    root = ET.fromstring(xml_bytes)
    eb = root.find("pgterms:ebook", NS)
    if eb is None:
        return None
    about = eb.get("{%s}about" % NS["rdf"], "")
    m = re.search(r"ebooks/(\d+)", about)
    if not m:
        return None
    b = {"id": int(m.group(1))}
    t = eb.find("dcterms:title", NS)
    b["title"] = (t.text or "").strip() if t is not None else ""
    alt = eb.find("dcterms:alternative", NS)
    if alt is not None and alt.text:
        b["alt"] = alt.text.strip()
    b["rights"] = (eb.findtext("dcterms:rights", "", NS) or "").strip()
    b["downloads"] = _int(eb.findtext("pgterms:downloads", "0", NS)) or 0
    b["issued"] = eb.findtext("dcterms:issued", "", NS)
    typ = eb.find("dcterms:type/rdf:Description/rdf:value", NS)
    b["type"] = typ.text if typ is not None else ""
    b["langs"] = [v.text for v in eb.findall("dcterms:language/rdf:Description/rdf:value", NS) if v.text]
    agents = []
    for child in eb:
        tag = child.tag
        if tag == "{%s}creator" % NS["dcterms"]:
            role = "aut"
        elif tag.startswith(MARCREL):
            role = tag[len(MARCREL):]
        else:
            continue
        for ag in child.findall("pgterms:agent", NS):
            agents.append({
                "role": role,
                "name": (ag.findtext("pgterms:name", "", NS) or "").strip(),
                "birth": _int(ag.findtext("pgterms:birthdate", None, NS)),
                "death": _int(ag.findtext("pgterms:deathdate", None, NS)),
                "aid": (ag.get("{%s}about" % NS["rdf"]) or "").rsplit("/", 1)[-1],
                "wiki": [w.get("{%s}resource" % NS["rdf"]) for w in ag.findall("pgterms:webpage", NS)
                         if "wikipedia" in (w.get("{%s}resource" % NS["rdf"]) or "")],
            })
    b["agents"] = agents
    subj, locc = [], []
    for s in eb.findall("dcterms:subject/rdf:Description", NS):
        mem = s.find("dcam:memberOf", NS)
        v = s.findtext("rdf:value", "", NS)
        kind = (mem.get("{%s}resource" % NS["rdf"]) if mem is not None else "") or ""
        (locc if kind.endswith("LCC") else subj).append(v)
    b["subjects"], b["locc"] = subj, locc
    b["shelves"] = [v.text for v in eb.findall("pgterms:bookshelf/rdf:Description/rdf:value", NS) if v.text]
    b["summary"] = (eb.findtext("pgterms:marc520", "", NS) or "").strip()
    b["wiki_desc"] = [d.text for d in eb.findall("dcterms:description", NS) if d.text]
    b["y906"] = _int(eb.findtext("pgterms:marc906", None, NS))
    b["y260"] = _year_from_260(eb.findtext("pgterms:marc260", None, NS))
    b["note250"] = eb.findtext("pgterms:marc250", "", NS) or ""
    b["reading_ease"] = eb.findtext("pgterms:marc908", "", NS) or ""
    files = {}
    for f in eb.findall("dcterms:hasFormat/pgterms:file", NS):
        url = f.get("{%s}about" % NS["rdf"]) or ""
        files[url] = _int(f.findtext("dcterms:extent", "0", NS)) or 0
    b["files"] = files
    return b


def build_meta(force=False):
    if META_JSON.exists() and not force:
        return load_json(META_JSON)
    tar = PG_DIR / "rdf-files.tar.bz2"
    out = []
    with tarfile.open(tar, "r:bz2") as t:
        for m in t:
            if not m.isfile() or not m.name.endswith(".rdf"):
                continue
            try:
                b = parse_rdf(t.extractfile(m).read())
            except ET.ParseError as e:
                print("rdf parse error", m.name, e)
                continue
            if b:
                out.append(b)
    out.sort(key=lambda b: b["id"])
    save_json(META_JSON, out, compact=True)
    print("pg meta:", len(out), "records")
    return out


EDITIONS = {"img3": "pg%d-images-3.epub", "noimg": "pg%d.epub"}
EDITION_URL = {"img3": "https://www.gutenberg.org/ebooks/%d.epub3.images",
               "noimg": "https://www.gutenberg.org/ebooks/%d.epub.noimages"}


def epub_path(pgid, edition="img3"):
    return EPUB_DIR / str(pgid) / (EDITIONS[edition] % pgid)


def epub_url(pgid, edition="img3"):
    """Original source URL (u2): the public per-book download, used by the app only as a mirror."""
    return EDITION_URL[edition] % pgid


def rsync_epubs(items, log=print, workers=8):
    """Fetch the EPUB of each (pgid, edition) from the PG mirrors with rsync (in WSL).

    Mirror bandwidth per connection is low and long multi-file sessions stall, so each of `workers`
    WSL shells walks its own share of the list one file at a time: ibiblio first, pglaf as fallback.
    rsync writes to a temp name and renames, so an interrupted run never leaves half files.
    """
    want = [(i, e) for i, e in items if not epub_path(i, e).exists()]
    if not want:
        return 0
    EPUB_DIR.mkdir(parents=True, exist_ok=True)
    dst = _wsl_path(EPUB_DIR)
    script = ('while read f; do d=$(dirname "$f"); mkdir -p "%s/$d"; '
              'rsync -t --timeout=120 "%s$f" "%s/$f" 2>/dev/null || '
              'rsync -t --timeout=120 "%s$f" "%s/$f" 2>/dev/null || echo "MISS $f"; done < "$1"'
              % (dst, RSYNC_HOSTS[1], dst, RSYNC_HOSTS[0], dst))
    procs = []
    for w in range(min(workers, len(want))):
        lst = PG_DIR / ("rsync-files-%d.txt" % w)
        with open(lst, "w", newline="\n") as f:
            for i, e in want[w::workers]:
                f.write("%d/%s\n" % (i, EDITIONS[e] % i))
        procs.append(subprocess.Popen(["wsl", "-e", "bash", "-c", script, "fetch", _wsl_path(lst)],
                                      stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True))
    log("rsync %d files, %d workers" % (len(want), len(procs)))
    import time
    while any(p.poll() is None for p in procs):
        time.sleep(60)
        have = sum(1 for i, e in want if epub_path(i, e).exists())
        log("  %d/%d" % (have, len(want)))
    for p in procs:
        out = p.stdout.read() if p.stdout else ""
        for line in out.splitlines():
            if line.startswith("MISS"):
                log(line)
    return sum(1 for i, e in want if epub_path(i, e).exists())


def _wsl_path(p):
    p = str(Path(p).resolve()).replace("\\", "/")
    if len(p) > 1 and p[1] == ":":
        return "/mnt/%s%s" % (p[0].lower(), p[2:])
    return p
