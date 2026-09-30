"""App guides — apps/<id>/GUIDE.md rendered as a phone-friendly web page.

The device has no browser: the Store's app page shows a QR code of <store>/docs/<id>.html and the
user reads the guide on their phone. appstore_server.py serves the page live, export_static.py
writes it for GitHub Pages; both call guide_html().

    GUIDE.md        the guide, Italian (the store's first language)
    GUIDE.en.md     optional English version (a language switch appears when present)

Markdown goes through python-markdown when it is installed (tables, fenced code); without it the
text is shown as-is in a <pre>, still readable.
"""
import base64
import html
import os
import struct
import zlib

GUIDE_LANGS = (("it", "GUIDE.md"), ("en", "GUIDE.en.md"))

TEXT = {
    "it": {"console_t": "Programma da Terminale",
           "console": "Non ha una finestra sua: gira dentro l'app Terminale. Toccando l'icona si apre il "
                      "Terminale con il programma già avviato; oppure apri Terminale e scrivi <code>{id}</code>.",
           "license": "Licenza", "source": "Sito del progetto", "store": "Tutte le app",
           "version": "versione"},
    "en": {"console_t": "Terminal program",
           "console": "It has no window of its own: it runs inside the Terminal app. Tapping its icon opens "
                      "the Terminal with the program already started; or open Terminal and type "
                      "<code>{id}</code>.",
           "license": "License", "source": "Project site", "store": "All apps", "version": "version"},
}


def guide_langs(app_dir):
    """Languages with a guide file in this app dir, Italian first."""
    return [l for l, f in GUIDE_LANGS if os.path.isfile(os.path.join(app_dir, f))]


def _md(text):
    try:
        import markdown
    except ImportError:
        return f"<pre>{html.escape(text)}</pre>"
    return markdown.markdown(text, extensions=["tables", "fenced_code", "sane_lists"])


def _png(w, h, rgba):
    """Minimal PNG encoder (RGBA8), stdlib only."""
    def chunk(tag, data):
        c = tag + data
        return struct.pack(">I", len(data)) + c + struct.pack(">I", zlib.crc32(c) & 0xFFFFFFFF)
    rows = b"".join(b"\x00" + rgba[y * w * 4:(y + 1) * w * 4] for y in range(h))
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 6, 0, 0, 0))
            + chunk(b"IDAT", zlib.compress(rows, 9)) + chunk(b"IEND", b""))


def icon_data_uri(app_dir, px=80):
    """icon.z (80x80 BGRA, raw deflate) as a PNG data: URI, or "" when absent/invalid."""
    try:
        with open(os.path.join(app_dir, "icon.z"), "rb") as f:
            raw = zlib.decompress(f.read(), -15)
    except (OSError, zlib.error):
        return ""
    if len(raw) != px * px * 4:
        return ""
    rgba = bytearray(raw)
    rgba[0::4], rgba[2::4] = raw[2::4], raw[0::4]   # BGRA -> RGBA
    return "data:image/png;base64," + base64.b64encode(_png(px, px, bytes(rgba))).decode()


def _pick(mapping, lang):
    if isinstance(mapping, dict):
        return mapping.get(lang) or mapping.get("en") or next(iter(mapping.values()), "")
    return mapping or ""


def guide_html(app_id, app_dir, man, ov=None, store_href="../"):
    """The guide page (bytes), or None when the app has no GUIDE.md."""
    langs = guide_langs(app_dir)
    if not langs:
        return None
    ov = ov or {}
    e = html.escape
    icon = icon_data_uri(app_dir)
    sections, switch = [], []
    for lang in langs:
        t = TEXT[lang]
        with open(os.path.join(app_dir, dict(GUIDE_LANGS)[lang]), "r", encoding="utf-8") as f:
            body = _md(f.read())
        name = _pick(ov.get("names"), lang) or man.get("name", app_id)
        desc = (_pick(ov.get("descriptions"), lang) or _pick(man.get("descriptions"), lang)
                or _pick(man.get("description", ""), lang))
        meta = [f"{t['version']} {e(str(man.get('version', '?')))}"]
        lic = ov.get("license", man.get("license", ""))
        if lic:
            meta.append(f"{t['license']}: {e(lic)}")
        src = ov.get("source", man.get("source", ""))
        if src.startswith(("http://", "https://")):
            meta.append(f"<a href='{e(src)}'>{t['source']}</a>")
        callout = ""
        if man.get("console"):
            callout = (f"<aside><b><span class=prompt>&gt;_</span> {t['console_t']}</b>"
                       f"<p>{t['console'].format(id=e(app_id))}</p></aside>")
        sections.append(
            f"<section id={lang} lang={lang}>"
            f"<header>{f'<img src={chr(39)}{icon}{chr(39)} alt=>' if icon else ''}"
            f"<div><h1>{e(name)}</h1><p class=desc>{e(desc)}</p><p class=meta>{' · '.join(meta)}</p></div>"
            f"</header>{callout}<article>{body}</article>"
            f"<footer><a href='{store_href}'>{t['store']}</a> · NucleoOS P4</footer></section>")
        switch.append(f"<a href='#{lang}'>{lang.upper()}</a>")
    langbar = f"<nav>{' '.join(switch)}</nav>" if len(langs) > 1 else ""
    first = langs[0]
    title = e(man.get("name", app_id))
    css = (
        ":root{--bg:#f6f6f4;--card:#fff;--text:#1d1d1f;--dim:#6b6b70;--line:#e3e3e0;--accent:#2563eb;"
        "--code:#f0f0ec;--term:#0c0e10;--termfg:#4ade80}"
        "@media (prefers-color-scheme:dark){:root{--bg:#111214;--card:#1a1b1e;--text:#ececec;--dim:#9a9aa0;"
        "--line:#2b2c30;--accent:#7aa7ff;--code:#24252a}}"
        "*{box-sizing:border-box}body{margin:0;background:var(--bg);color:var(--text);"
        "font:16px/1.6 system-ui,-apple-system,Segoe UI,sans-serif}"
        "section{max-width:720px;margin:0 auto;padding:24px 16px 40px}"
        "section+section{display:none}"
        "nav{max-width:720px;margin:0 auto;padding:12px 16px 0;text-align:right}"
        "nav a{margin-left:10px;font-weight:600;font-size:14px}"
        "header{display:flex;gap:16px;align-items:flex-start;margin-bottom:20px}"
        "header img{width:80px;height:80px;flex:none;border-radius:18px}"
        "h1{margin:0;font-size:28px;line-height:1.2}.desc{margin:6px 0 4px}.meta{margin:0;color:var(--dim);font-size:14px}"
        "aside{background:var(--term);color:#e6e6e6;border:1px solid #2f3a33;border-radius:12px;"
        "padding:14px 16px;margin:0 0 24px}"
        "aside p{margin:6px 0 0}aside code{background:#23262a;color:var(--termfg)}"
        ".prompt{color:var(--termfg);font-family:ui-monospace,Consolas,monospace;margin-right:4px}"
        "article{background:var(--card);border:1px solid var(--line);border-radius:14px;padding:4px 20px 12px}"
        "article h1{display:none}h2{font-size:20px;margin:24px 0 8px}h3{font-size:17px;margin:18px 0 6px}"
        "code{font-family:ui-monospace,Consolas,monospace;font-size:.92em;background:var(--code);"
        "padding:1px 5px;border-radius:5px}"
        "pre{background:var(--term);color:#e6e6e6;padding:12px 14px;border-radius:10px;overflow-x:auto}"
        "pre code{background:none;padding:0;color:inherit}"
        "table{border-collapse:collapse;width:100%;font-size:15px;display:block;overflow-x:auto}"
        "td,th{border-bottom:1px solid var(--line);padding:6px 8px;text-align:left;vertical-align:top}"
        "a{color:var(--accent);text-decoration:none}"
        "footer{margin-top:24px;color:var(--dim);font-size:14px;text-align:center}"
    )
    return (
        f"<!doctype html><html lang={first}><meta charset=utf-8>"
        "<meta name=viewport content='width=device-width,initial-scale=1'>"
        f"<title>{title} — guida · NucleoOS P4</title><style>{css}</style>"
        f"<body>{langbar}{''.join(sections)}"
        # language: #it / #en from the switch, else the phone's language, else the first guide
        "<script>function pick(){var h=location.hash.slice(1)||(navigator.language||'').slice(0,2),"
        "all=document.querySelectorAll('section'),hit=document.getElementById(h);"
        "all.forEach(function(x,i){x.style.display=(hit&&hit.tagName=='SECTION'?x===hit:i==0)?'block':'none'})}"
        "onhashchange=pick;pick()</script></body></html>"
    ).encode("utf-8")
