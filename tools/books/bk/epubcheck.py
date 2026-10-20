"""Minimal EPUB validator + text extractor.

Checks what the reader app needs to open a book: a valid ZIP, META-INF/container.xml pointing to an
OPF, an OPF with a manifest and a non-empty spine whose items exist in the archive and parse as
(X)HTML. Returns the plain text of the spine for the quality heuristic.

Hook for the app's own parser: if NV_BOOKS_EPUB_PARSER is set (path to the PC build of the reader's
C engine, e.g. a `books_probe` binary built from ports/books), each EPUB is also run through it as
`<parser> <file.epub>`; exit code 0 = the app opens it. Until the app session adds that binary the
Python checks below are the gate.
"""
import html
import os
import posixpath
import re
import subprocess
import zipfile
from urllib.parse import unquote
import xml.etree.ElementTree as ET

OPF_NS = "{http://www.idpf.org/2007/opf}"
CONT_NS = "{urn:oasis:names:tc:opendocument:xmlns:container}"
DC_NS = "{http://purl.org/dc/elements/1.1/}"


class EpubError(Exception):
    pass


_BLOCK = re.compile(r"(?i)</?(p|div|br|h[1-6]|li|tr|td|th|blockquote|section|pre|table|ul|ol|dd|dt|hr)\b[^>]*>")


def _strip_html(s):
    """Block tags become line breaks, inline tags vanish (so 'Malavogli<span>a</span>' stays one word)."""
    s = re.sub(r"(?is)<(script|style|head)\b.*?</\1>", " ", s)
    s = _BLOCK.sub("\n", s)
    s = re.sub(r"(?s)<[^>]+>", "", s)
    s = html.unescape(s)
    return re.sub(r"[ \t\r\f\v]+", " ", s)


def check(path, want_text=True):
    """Validate an EPUB. Returns dict(ok, error, title, lang, spine, text, chars)."""
    res = {"ok": False, "error": None}
    try:
        info = _check(path, want_text)
        res.update(info)
        res["ok"] = True
    except (EpubError, zipfile.BadZipFile, ET.ParseError, KeyError, UnicodeDecodeError) as e:
        res["error"] = "%s: %s" % (type(e).__name__, str(e)[:200])
        return res
    probe = os.environ.get("NV_BOOKS_EPUB_PARSER")
    if probe:
        r = subprocess.run([probe, str(path)], capture_output=True, text=True, timeout=120)
        if r.returncode != 0:
            res["ok"] = False
            res["error"] = "app parser: %s" % (r.stderr or r.stdout).strip()[:200]
    return res


def _check(path, want_text):
    with zipfile.ZipFile(path) as z:
        names = set(z.namelist())
        bad = z.testzip()
        if bad:
            raise EpubError("corrupt member %s" % bad)
        if "mimetype" in names:
            mt = z.read("mimetype").decode("ascii", "replace").strip()
            if mt != "application/epub+zip":
                raise EpubError("mimetype is %r" % mt)
        if "META-INF/container.xml" not in names:
            raise EpubError("no META-INF/container.xml")
        cont = ET.fromstring(z.read("META-INF/container.xml"))
        rf = cont.find(".//%srootfile" % CONT_NS)
        if rf is None or not rf.get("full-path"):
            raise EpubError("container.xml has no rootfile")
        opf_path = rf.get("full-path")
        if opf_path not in names:
            raise EpubError("OPF %s missing" % opf_path)
        opf = ET.fromstring(z.read(opf_path))
        base = posixpath.dirname(opf_path)
        meta = opf.find("%smetadata" % OPF_NS)
        title = lang = ""
        if meta is not None:
            title = (meta.findtext("%stitle" % DC_NS) or "").strip()
            lang = (meta.findtext("%slanguage" % DC_NS) or "").strip()
        manifest = {}
        man = opf.find("%smanifest" % OPF_NS)
        if man is None:
            raise EpubError("OPF has no manifest")
        for it in man.findall("%sitem" % OPF_NS):
            href = it.get("href") or ""
            full = posixpath.normpath(posixpath.join(base, unquote(href.split("#")[0]))) if base else \
                posixpath.normpath(unquote(href.split("#")[0]))
            manifest[it.get("id")] = (full, it.get("media-type") or "")
        spine = opf.find("%sspine" % OPF_NS)
        if spine is None:
            raise EpubError("OPF has no spine")
        refs = [ir.get("idref") for ir in spine.findall("%sitemref" % OPF_NS)]
        if not refs:
            raise EpubError("empty spine")
        parts = []
        for r in refs:
            if r not in manifest:
                raise EpubError("spine idref %s not in manifest" % r)
            full, mt = manifest[r]
            if full not in names:
                raise EpubError("spine item %s missing from zip" % full)
            if want_text and ("html" in mt or full.endswith((".xhtml", ".html", ".htm"))):
                raw = z.read(full)
                s = raw.decode("utf-8", "replace")
                parts.append(_strip_html(s))
        text = "\n".join(parts) if want_text else ""
        return {"title": title, "lang": lang, "spine": len(refs), "text": text, "chars": len(text)}
