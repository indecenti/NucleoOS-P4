"""Cover thumbnails for the catalog (books/covers/<id>.jpg, row field "cv": 1).

Only genuine covers: Standard Ebooks artwork (CC0 production, PD painting), Gutenberg covers that are
real scans/artwork (the PNG covers Gutenberg generates from the title text are skipped), the comics'
own title art. Books we build from wikis/HTML have no cover: the app draws a typographic one.

Thumbnail: exactly 132x192, cover fitted and centre-cropped to fill, baseline JPEG, no metadata,
quality ~72 lowered step by step until the file is <= 6 KB. Total covers + indexes stay within the
Pages budget (most popular books first).
"""
import io
import posixpath
import re
import zipfile
from urllib.parse import unquote

from .common import CACHE

W, H = 132, 192
MAX_BYTES = 6 * 1024
THUMB_DIR = CACHE / "covers"
PG_GENERATED = re.compile(r"(^|/)(\d+_)?\d+-cover\.png$")


def find_cover(epub_path):
    """(href, bytes) of the cover image declared in the OPF, or (None, None)."""
    try:
        z = zipfile.ZipFile(epub_path)
        cont = z.read("META-INF/container.xml").decode("utf-8", "replace")
        opf_path = re.search(r'full-path="([^"]+)"', cont).group(1)
        opf = z.read(opf_path).decode("utf-8", "replace")
    except Exception:
        return None, None
    base = posixpath.dirname(opf_path)
    items = re.findall(r"<item\b[^>]*>", opf)
    cid = None
    m = re.search(r'<meta[^>]*name="cover"[^>]*content="([^"]+)"', opf) or \
        re.search(r'<meta[^>]*content="([^"]+)"[^>]*name="cover"', opf)
    if m:
        cid = m.group(1)
    pick = None
    for it in items:
        if "cover-image" in it:
            pick = it
            break
    if pick is None and cid:
        pick = next((it for it in items if re.search(r'\bid="%s"' % re.escape(cid), it)), None)
    if pick is None:
        return None, None
    hm = re.search(r'href="([^"]+)"', pick)
    mt = re.search(r'media-type="([^"]+)"', pick)
    if not hm or (mt and not mt.group(1).startswith("image/")):
        return None, None
    href = posixpath.normpath(posixpath.join(base, unquote(hm.group(1))))
    try:
        return href, z.read(href)
    except KeyError:
        return None, None


def first_image(epub_path):
    """First image in the archive (comics: the title art of the first episode)."""
    z = zipfile.ZipFile(epub_path)
    for n in sorted(z.namelist()):
        if n.lower().endswith((".jpg", ".jpeg", ".png")):
            return n, z.read(n)
    return None, None


def is_genuine(href, data, src):
    """True for real artwork/scans; False for text-only generated covers."""
    from PIL import Image
    if src == "pg" and PG_GENERATED.search(href or ""):
        return False
    try:
        im = Image.open(io.BytesIO(data)).convert("RGB")
    except Exception:
        return False
    if im.width < 120 or im.height < 160:
        return False
    small = im.resize((66, 96))
    colours = len(set(small.tobytes()[i:i + 3] for i in range(0, 66 * 96 * 3, 3)))
    hist = sorted(small.quantize(16).histogram(), reverse=True)
    top = hist[0] / max(1, sum(hist))
    if src in ("se", "mn"):
        return True
    # plain text on a flat background: few colours, one colour dominates
    return colours >= 1200 and top <= 0.6


def thumbnail(data):
    """Bytes of a 132x192 baseline JPEG <= 6 KB, or None."""
    from PIL import Image
    im = Image.open(io.BytesIO(data)).convert("RGB")
    scale = max(W / im.width, H / im.height)
    nw, nh = max(W, round(im.width * scale)), max(H, round(im.height * scale))
    im = im.resize((nw, nh), Image.LANCZOS)
    left, top = (nw - W) // 2, (nh - H) // 2
    im = im.crop((left, top, left + W, top + H))
    for q in (72, 66, 60, 54, 48, 42, 36):
        out = io.BytesIO()
        im.save(out, "JPEG", quality=q, optimize=True, progressive=False, subsampling=2)
        b = out.getvalue()
        if len(b) <= MAX_BYTES:
            return b
    return None


def cover_for(rec):
    """Cached thumbnail bytes for a catalog row, or None (no genuine cover)."""
    key = "%s-%s" % (rec["id"], (rec.get("h") or "")[:12])
    p = THUMB_DIR / (key + ".jpg")
    miss = THUMB_DIR / (key + ".none")
    if p.exists():
        return p.read_bytes()
    if miss.exists():
        return None
    f = rec.get("_file")
    href, data = (first_image(f) if rec["id"].startswith("mn-pepper") else find_cover(f)) if f else (None, None)
    thumb = None
    if data and is_genuine(href, data, rec["src"]):
        try:
            thumb = thumbnail(data)
        except Exception:
            thumb = None
    THUMB_DIR.mkdir(parents=True, exist_ok=True)
    if thumb:
        p.write_bytes(thumb)
    else:
        miss.write_bytes(b"")
    return thumb
