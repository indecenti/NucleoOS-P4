"""Build a clean EPUB 3 (with EPUB 2 NCX fallback) from HTML chapters.

Used for sources that do not ship EPUBs (Wikisource, Wikibooks, curated HTML manuals). Input HTML is
sanitised with lxml into a small XHTML subset the reader app renders (headings, paragraphs, lists,
tables, emphasis, links, footnotes, preformatted code, images when given). A licence/attribution page
is always added at the end.
"""
import datetime
import html
import io
import re
import uuid
import zipfile

from lxml import etree, html as lhtml

ALLOWED = {"p", "div", "span", "h1", "h2", "h3", "h4", "h5", "h6", "em", "i", "strong", "b", "u", "s", "sub",
           "sup", "small", "big", "br", "hr", "blockquote", "pre", "code", "tt", "kbd", "samp", "var", "ul", "ol",
           "li", "dl", "dt", "dd", "table", "thead", "tbody", "tfoot", "tr", "td", "th", "caption", "a", "img",
           "figure", "figcaption", "center", "abbr", "cite", "q", "section", "aside", "math"}
DROP = {"script", "style", "noscript", "form", "input", "button", "select", "textarea", "iframe", "object",
        "embed", "video", "audio", "svg", "map", "link", "meta", "nav"}
KEEP_ATTR = {"href", "id", "colspan", "rowspan", "src", "alt", "lang", "dir", "start", "type"}
CLASS_KEEP = re.compile(r"^(center|centered|right|left|poem|verse|noindent|smallcaps|footnote|reference|math)$")

CSS = """body{margin:0 4%;line-height:1.45}h1,h2,h3{text-align:center;line-height:1.2}
p{margin:0;text-indent:1.3em;text-align:justify}h1+p,h2+p,h3+p,p.noindent{text-indent:0}
.center,center{text-align:center;text-indent:0}.poem p,.verse p{text-indent:0;text-align:left}
pre,code{font-family:monospace;font-size:.9em;white-space:pre-wrap}pre{margin:.6em 0;text-indent:0}
blockquote{margin:.6em 1.5em}table{border-collapse:collapse;margin:.5em 0}td,th{border:1px solid #888;padding:.2em .4em}
li p{text-indent:0}.smallcaps{font-variant:small-caps}img{max-width:100%}.license p{text-indent:0;margin:.5em 0}
"""


def _esc(s):
    return html.escape(s or "", quote=True)


def sanitize(fragment, base_href_map=None, keep_images=False):
    """HTML fragment -> XHTML body string with only ALLOWED tags/attributes."""
    if not fragment or not fragment.strip():
        return ""
    doc = lhtml.fragment_fromstring(fragment, create_parent="div")
    for el in list(doc.iter()):
        if not isinstance(el.tag, str):           # comments / processing instructions
            el.drop_tree() if el.getparent() is not None else None
            continue
        tag = el.tag.lower()
        if tag in DROP or (tag == "img" and not keep_images):
            if el.getparent() is not None:
                el.drop_tree()
            continue
        if tag not in ALLOWED and el is not doc:
            el.drop_tag()
            continue
        cls = " ".join(c for c in (el.get("class") or "").split() if CLASS_KEEP.match(c))
        for k in list(el.attrib):
            if k not in KEEP_ATTR:
                del el.attrib[k]
        if cls:
            el.set("class", cls)
        if tag == "a":
            href = el.get("href") or ""
            if base_href_map is not None:
                new = base_href_map(href)
                if new:
                    el.set("href", new)
                elif not href.startswith(("http://", "https://", "#")):
                    el.drop_tag()
                    continue
        if tag == "center":
            el.tag = "div"
            el.set("class", "center")
        if tag in ("tt",):
            el.tag = "code"
        if tag == "big":
            el.tag = "span"
    out = []
    if doc.text and doc.text.strip():
        out.append(_esc(doc.text))
    for child in doc:
        out.append(etree.tostring(child, method="xml", encoding="unicode", with_tail=True))
    s = "".join(out)
    s = re.sub(r"<(\w+)(\s[^>]*)?>\s*</\1>", lambda m: m.group(0) if m.group(1) in ("td", "th", "a") else "", s)
    return s


def _xhtml(title, body, lang):
    return ('<?xml version="1.0" encoding="utf-8"?>\n<!DOCTYPE html>\n'
            '<html xmlns="http://www.w3.org/1999/xhtml" xmlns:epub="http://www.idpf.org/2007/ops" '
            'xml:lang="%s" lang="%s"><head><meta charset="utf-8"/><title>%s</title>'
            '<link rel="stylesheet" type="text/css" href="style.css"/></head><body>%s</body></html>'
            % (lang, lang, _esc(title), body))


def _check_xml(s):
    etree.fromstring(s.encode("utf-8"))


def build(path, title, authors, lang, chapters, license_html, identifier=None, description="",
          date=None, subjects=(), source_url="", images=None):
    """chapters: [(chapter_title, xhtml_body_fragment)] already sanitised.
    images: {"img/x.png": bytes} referenced by chapters. Returns the output path."""
    identifier = identifier or "urn:uuid:%s" % uuid.uuid4()
    files = []
    nav_items = []
    for i, (ct, body) in enumerate(chapters, 1):
        name = "c%03d.xhtml" % i
        heading = "<h2>%s</h2>" % _esc(ct) if ct and not re.match(r"\s*<h[1-3]", body) else ""
        doc = _xhtml(ct or title, heading + body, lang)
        try:
            _check_xml(doc)
        except etree.XMLSyntaxError:
            # fall back to plain paragraphs if sanitising still left broken markup
            txt = lhtml.fromstring("<div>%s</div>" % body).text_content()
            doc = _xhtml(ct or title, heading + "".join("<p>%s</p>" % _esc(p) for p in txt.split("\n") if p.strip()), lang)
        files.append((name, doc))
        nav_items.append((name, ct or "%d" % i))
    lic_title = {"it": "Licenza e fonte", "de": "Lizenz und Quelle", "fr": "Licence et source",
                 "es": "Licencia y fuente"}.get(lang, "License and source")
    lic = _xhtml(lic_title, '<div class="license">%s</div>' % license_html, lang)
    _check_xml(lic)
    files.append(("license.xhtml", lic))
    nav_items.append(("license.xhtml", lic_title))
    title_page = _xhtml(title, '<div class="center"><h1>%s</h1><p class="center">%s</p></div>'
                        % (_esc(title), _esc(", ".join(authors))), lang)
    files.insert(0, ("title.xhtml", title_page))
    nav = ('<?xml version="1.0" encoding="utf-8"?>\n<!DOCTYPE html>\n<html xmlns="http://www.w3.org/1999/xhtml" '
           'xmlns:epub="http://www.idpf.org/2007/ops" xml:lang="%s"><head><meta charset="utf-8"/><title>%s</title></head>'
           '<body><nav epub:type="toc" id="toc"><h1>%s</h1><ol>%s</ol></nav></body></html>'
           % (lang, _esc(title), _esc(title),
              "".join('<li><a href="%s">%s</a></li>' % (n, _esc(t)) for n, t in nav_items)))
    ncx = ('<?xml version="1.0" encoding="utf-8"?>\n<ncx xmlns="http://www.daisy.org/z3986/2005/ncx/" version="2005-1">'
           '<head><meta name="dtb:uid" content="%s"/></head><docTitle><text>%s</text></docTitle><navMap>%s</navMap></ncx>'
           % (_esc(identifier), _esc(title), "".join(
               '<navPoint id="n%d" playOrder="%d"><navLabel><text>%s</text></navLabel><content src="%s"/></navPoint>'
               % (i, i, _esc(t), n) for i, (n, t) in enumerate(nav_items, 1))))
    modified = datetime.datetime.utcnow().replace(microsecond=0).isoformat() + "Z"
    images = images or {}
    manifest = ['<item id="nav" href="nav.xhtml" media-type="application/xhtml+xml" properties="nav"/>',
                '<item id="ncx" href="toc.ncx" media-type="application/x-dtbncx+xml"/>',
                '<item id="css" href="style.css" media-type="text/css"/>']
    spine = []
    for n, _ in files:
        iid = n.split(".")[0]
        manifest.append('<item id="%s" href="%s" media-type="application/xhtml+xml"/>' % (iid, n))
        spine.append('<itemref idref="%s"/>' % iid)
    for i, name in enumerate(sorted(images)):
        mt = {"png": "image/png", "jpg": "image/jpeg", "jpeg": "image/jpeg", "gif": "image/gif",
              "svg": "image/svg+xml", "webp": "image/webp"}.get(name.rsplit(".", 1)[-1].lower(), "image/png")
        manifest.append('<item id="img%d" href="%s" media-type="%s"/>' % (i, _esc(name), mt))
    meta = ['<dc:identifier id="uid">%s</dc:identifier>' % _esc(identifier),
            '<dc:title>%s</dc:title>' % _esc(title), '<dc:language>%s</dc:language>' % _esc(lang),
            '<meta property="dcterms:modified">%s</meta>' % modified]
    for i, a in enumerate(authors):
        meta.append('<dc:creator id="a%d">%s</dc:creator>' % (i, _esc(a)))
    if description:
        meta.append('<dc:description>%s</dc:description>' % _esc(description))
    if date:
        meta.append('<dc:date>%s</dc:date>' % _esc(str(date)))
    if source_url:
        meta.append('<dc:source>%s</dc:source>' % _esc(source_url))
    for s in subjects:
        meta.append('<dc:subject>%s</dc:subject>' % _esc(s))
    opf = ('<?xml version="1.0" encoding="utf-8"?>\n<package xmlns="http://www.idpf.org/2007/opf" version="3.0" '
           'unique-identifier="uid" xml:lang="%s"><metadata xmlns:dc="http://purl.org/dc/elements/1.1/">%s</metadata>'
           '<manifest>%s</manifest><spine toc="ncx">%s</spine></package>'
           % (lang, "".join(meta), "".join(manifest), "".join(spine)))
    _check_xml(opf)
    _check_xml(nav)
    _check_xml(ncx)
    buf = io.BytesIO()
    with zipfile.ZipFile(buf, "w") as z:
        z.writestr(zipfile.ZipInfo("mimetype"), "application/epub+zip", compress_type=zipfile.ZIP_STORED)
        z.writestr("META-INF/container.xml",
                   '<?xml version="1.0"?><container version="1.0" xmlns="urn:oasis:names:tc:opendocument:xmlns:container">'
                   '<rootfiles><rootfile full-path="OEBPS/content.opf" media-type="application/oebps-package+xml"/>'
                   '</rootfiles></container>', compress_type=zipfile.ZIP_DEFLATED)
        z.writestr("OEBPS/content.opf", opf, compress_type=zipfile.ZIP_DEFLATED)
        z.writestr("OEBPS/nav.xhtml", nav, compress_type=zipfile.ZIP_DEFLATED)
        z.writestr("OEBPS/toc.ncx", ncx, compress_type=zipfile.ZIP_DEFLATED)
        z.writestr("OEBPS/style.css", CSS, compress_type=zipfile.ZIP_DEFLATED)
        for n, doc in files:
            z.writestr("OEBPS/" + n, doc, compress_type=zipfile.ZIP_DEFLATED)
        for name, data in images.items():
            z.writestr("OEBPS/" + name, data, compress_type=zipfile.ZIP_STORED)
    from pathlib import Path
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    tmp = path.with_suffix(".part")
    tmp.write_bytes(buf.getvalue())
    tmp.replace(path)
    return path
