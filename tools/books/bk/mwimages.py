"""Images for MediaWiki-built books, only when each file's own licence is free.

Every <img> that links to its file page is looked up with prop=imageinfo (Commons or local), its
licence normalised with rights.normalise_license (no NC/ND, no non-free/fair-use files) and its
author recorded for the credits page. Accepted images are fetched as <=800 px thumbnails, cached,
and referenced from the chapter as img/<hash>.<ext>. Everything else is dropped.
"""
import hashlib
import re
from urllib.parse import unquote

from lxml import html as lhtml

from . import rights
from .common import CACHE, Http

IMG_DIR = CACHE / "mw" / "img"
MAX_BOOK_IMAGES_BYTES = 25_000_000
MIN_SIDE = 40


def _file_title(img, site):
    a = img.getparent()
    while a is not None and a.tag != "a":
        a = a.getparent()
    href = (a.get("href") if a is not None else "") or ""
    m = re.search(r"/wiki/([^:/?#]+:[^?#]+)", href)
    if not m:
        return None
    t = unquote(m.group(1)).replace("_", " ")
    ns, _, name = t.partition(":")
    return "File:" + name if name else None


def _strip(s):
    """Artist credit as plain text, without e-mail addresses (the name is the attribution)."""
    if not s:
        return ""
    t = lhtml.fromstring("<div>%s</div>" % s).text_content()
    t = re.sub(r"[,;(<\s]*[\w.+-]+@[\w-]+(\.[\w-]+)+[>)]?\.?", "", t)
    return re.sub(r"\s+", " ", t).strip(" ,;")


def embed(mw, site, chapters):
    """chapters: [(title, html)] -> (chapters, images {path: bytes}, credits [(file, artist, licence, url)])."""
    docs = []
    wanted = {}
    for ct, body in chapters:
        doc = lhtml.fragment_fromstring(body, create_parent="div")
        for img in list(doc.iter("img")):
            try:
                w = int(img.get("width") or 999)
                h = int(img.get("height") or 999)
            except ValueError:
                w = h = 999
            ft = _file_title(img, site)
            if not ft or w < MIN_SIDE or h < MIN_SIDE:
                img.drop_tree()
                continue
            wanted.setdefault(ft, []).append(img)
        docs.append((ct, doc))
    info = {}
    titles = sorted(wanted)
    for i in range(0, len(titles), 50):
        js = mw.api(site, action="query", titles="|".join(titles[i:i + 50]), prop="imageinfo",
                    iiprop="url|extmetadata|size|mime", iiurlwidth=800,
                    iiextmetadatafilter="LicenseShortName|License|Artist|NonFree|Restrictions")
        for p in js.get("query", {}).get("pages", []):
            ii = (p.get("imageinfo") or [None])[0]
            if ii:
                info[p["title"]] = ii
        for n in js.get("query", {}).get("normalized", []):
            if n["to"] in info:
                info[n["from"]] = info[n["to"]]
    http = Http(delay=0.1)
    images, credits, total = {}, [], 0
    for ft, imgs in wanted.items():
        ii = info.get(ft)
        ok_lic = None
        if ii:
            md = ii.get("extmetadata", {})
            nonfree = (md.get("NonFree", {}).get("value") or "").lower() in ("true", "1")
            lic = md.get("LicenseShortName", {}).get("value") or md.get("License", {}).get("value") or ""
            ok_lic = None if nonfree else rights.normalise_license(lic)
        src = ii.get("thumburl") or ii.get("url") if ii else None
        if not ok_lic or not src or (ii.get("mime") or "").startswith(("video", "audio", "application")):
            for im in imgs:
                if im.getparent() is not None:
                    im.drop_tree()
            continue
        ext = (src.split("?")[0].rsplit(".", 1)[-1] or "png").lower()
        if ext not in ("png", "jpg", "jpeg", "gif"):
            ext = "png"
        key = hashlib.sha1(src.split("?")[0].encode()).hexdigest()[:16]
        cache = IMG_DIR / key[:2] / (key + "." + ext)
        if not cache.exists():
            r = http.get(src)
            if r.status_code != 200 or len(r.content) < 100:
                for im in imgs:
                    if im.getparent() is not None:
                        im.drop_tree()
                continue
            cache.parent.mkdir(parents=True, exist_ok=True)
            cache.write_bytes(r.content)
        data = cache.read_bytes()
        if total + len(data) > MAX_BOOK_IMAGES_BYTES:
            for im in imgs:
                if im.getparent() is not None:
                    im.drop_tree()
            continue
        path = "img/%s.%s" % (key, ext)
        if path not in images:
            images[path] = data
            total += len(data)
            md = ii.get("extmetadata", {})
            credits.append((ft, _strip(md.get("Artist", {}).get("value", "")),
                            md.get("LicenseShortName", {}).get("value", ok_lic), ii.get("descriptionurl", "")))
        for im in imgs:
            for k in list(im.attrib):
                if k not in ("alt",):
                    del im.attrib[k]
            im.set("src", path)
            im.set("alt", im.get("alt") or ft[5:])
    out = [(ct, lhtml.tostring(doc, encoding="unicode")) for ct, doc in docs]
    return out, images, credits


def credits_html(credits, heading):
    if not credits:
        return ""
    from .epubbuild import _esc
    rows = "".join("<li>%s — %s — %s — %s</li>" % (_esc(f[5:]), _esc(a or "?"), _esc(l), _esc(u))
                   for f, a, l, u in credits)
    return "<h3>%s</h3><ul>%s</ul>" % (_esc(heading), rows)
