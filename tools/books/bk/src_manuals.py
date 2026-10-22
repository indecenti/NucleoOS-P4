"""Hand-curated manuals (data/manuals.json): the project's own EPUB, or a one-page HTML edition
split into chapters and packed with epubbuild (images of the document included, same licence)."""
import hashlib
import re
from collections import Counter, defaultdict
from urllib.parse import urljoin

from lxml import html as lhtml

from . import epubbuild, rights
from .categories import MAX_CATS, order
from .common import CACHE, DATA, Http, load_json, save_json

DIR = CACHE / "manuals"
CAND = CACHE / "candidates-mn.json"
POP = 700
LIC_TEXT = {
    "gfdl": "GNU Free Documentation License 1.3 (https://www.gnu.org/licenses/fdl-1.3.html)",
    "bsd": "BSD 3-clause licence (text in the 'Copyright information' chapter)",
    "pd": "Public domain (dedication by the author)",
    "mit": "MIT or Apache-2.0 licence",
    "gpl": "GNU General Public License version 2 (https://www.gnu.org/licenses/old-licenses/gpl-2.0.html)",
    "psf-2.0": "PSF License Agreement (https://docs.python.org/3/license.html)",
}
DROP_CLASSES = ("nav-panel", "navheader", "navfooter", "sidebar", "nav-chapters", "menu-bar",
                "mobile-nav-chapters", "toc", "contents", "nav-wrapper", "skip-link", "edit-link",
                "copiable-link", "headerlink", "anchor-link", "header-link")


def _fetch(http, url, dest):
    if not dest.exists():
        r = http.get(url)
        r.raise_for_status()
        dest.parent.mkdir(parents=True, exist_ok=True)
        dest.write_bytes(r.content)
    return dest.read_bytes()


def _split(root, level):
    """Split the document body at h<level> headings -> [(title, html)]."""
    tag = "h%d" % level
    chunks = []
    body = lhtml.tostring(root, encoding="unicode")
    parts = re.split(r"(?=<%s[\s>])" % tag, body)
    for p in parts:
        if not p.strip():
            continue
        m = re.match(r"<%s[^>]*>(.*?)</%s>" % (tag, tag), p, re.S)
        t = re.sub(r"[\s¶]+", " ", lhtml.fromstring("<div>%s</div>" % m.group(1)).text_content()).strip() if m else ""
        chunks.append((t, p))
    return chunks


def build_html1(item, http, log=print):
    raw = _fetch(http, item["url"], DIR / (item["id"] + ".html"))
    doc = lhtml.fromstring(raw)
    root = doc.find(".//%s" % item["main"]) if item.get("main") else doc.find(".//body")
    if root is None:
        root = doc
    for el in list(root.iter()):
        if not isinstance(el.tag, str) or el.getparent() is None:
            continue
        cls = (el.get("class") or "").split()
        if el.tag in ("script", "style", "nav", "header", "footer", "noscript", "button") or \
                any(c in DROP_CLASSES for c in cls) or el.get("role") == "navigation":
            el.drop_tree()
    # images of the document itself (same licence as the text)
    images, total = {}, 0
    for img in list(root.iter("img")):
        src = img.get("src") or ""
        if not src or src.startswith("data:"):
            img.drop_tree()
            continue
        absu = urljoin(item["url"], src)
        ext = absu.split("?")[0].rsplit(".", 1)[-1].lower()
        if ext not in ("png", "jpg", "jpeg", "gif"):
            img.drop_tree()
            continue
        key = hashlib.sha1(absu.encode()).hexdigest()[:16]
        cache = DIR / "img" / (key + "." + ext)
        try:
            data = _fetch(http, absu, cache)
        except Exception:
            img.drop_tree()
            continue
        if total + len(data) > 15_000_000:
            img.drop_tree()
            continue
        path = "img/%s.%s" % (key, ext)
        if path not in images:
            images[path] = data
            total += len(data)
        img.attrib.clear()
        img.set("src", path)
        img.set("alt", "")
    chapters = _split(root, item.get("split", 2))
    # anchor ids -> chapter file, so cross references keep working after the split
    where = {}
    for i, (_, h) in enumerate(chapters, 1):
        for m in re.finditer(r'\s(?:id|name)="([^"]+)"', h):
            where.setdefault(m.group(1), "c%03d.xhtml" % i)

    def remap(href):
        if "#" in href:
            frag = href.split("#", 1)[1]
            if frag in where:
                return "%s#%s" % (where[frag], frag)
        return None
    clean = [(t, epubbuild.sanitize(h, remap, keep_images=True)) for t, h in chapters]
    return clean, images


def build(log=print):
    js = load_json(DATA / "manuals.json")
    http = Http(delay=1.0)
    recs, excl = [], defaultdict(Counter)
    for it in js["items"]:
        lic = it["lic"]
        if lic not in rights.FREE_LICENSES and lic != "psf-2.0":
            excl[it["lang"]]["manual:licence_not_free"] += 1
            continue
        dest = DIR / "epub" / (it["id"] + ".epub")
        try:
            if not dest.exists():
                if it["kind"] == "epub":
                    _fetch(http, it["url"], dest)
                else:
                    chapters, images = build_html1(it, http, log)
                    lic_html = ("<h2>%s</h2><p>%s - %s.</p><p>Source: %s</p><p>Converted to EPUB for NucleoOS Books "
                                "(navigation removed, split into chapters); no other changes.</p>"
                                % (epubbuild._esc(it["t"]), epubbuild._esc(it["a"]), epubbuild._esc(LIC_TEXT[lic]),
                                   epubbuild._esc(it["url"])))
                    epubbuild.build(dest, it["t"], [it["a"]], it["lang"], chapters, lic_html, identifier=it["u2"],
                                    description=it["d"], source_url=it["url"], images=images)
            log("manual %s ok" % it["id"])
        except Exception as e:
            log("manual %s failed: %s" % (it["id"], e))
            excl[it["lang"]]["manual:build_failed"] += 1
            continue
        recs.append({"id": it["id"], "lang": it["lang"], "t": it["t"], "a": it["a"], "c": order(it["c"])[:MAX_CATS],
                     "src": "mn", "lic": lic, "pop": POP, "d": it["d"], "u2": it["u2"], "_file": str(dest),
                     "_attrib": "%s, %s" % (it["a"], LIC_TEXT[lic])})
    save_json(CAND, {"records": recs, "excluded": excl})
    log("manual candidates: %d" % len(recs))
    return recs
