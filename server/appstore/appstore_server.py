#!/usr/bin/env python3
"""NucleoV2 remote WASM app store — reference server (Python stdlib only).

A proper little app store: categorized, multilingual, and region-aware. It serves a catalog and the
app files that the on-device store (components/nv_appstore) installs from.

    GET /                          human-readable HTML index (browse in a browser)
    GET /store.json?lang=&region=&api=  catalog, localized + region-filtered for the caller
                                   (api=3: longer descriptions + author, license, icon.z sizes)
    GET /apps/<id>/manifest.json   one app's manifest.json (the schema nv_wasm validates)
    GET /apps/<id>/app.wasm        the WebAssembly module
    GET /apps/<id>/icon.argb       optional 80x80 ARGB8888 launcher icon
    GET /apps/<id>/app.aot         optional precompiled (wamrc) image the device runs instead
    GET /apps/<id>/icon.z          optional 80x80 ARGB8888 icon, raw-deflate compressed (~1-2 KB)

An "app" is any sub-directory of an apps root holding BOTH manifest.json and app.wasm — the exact
layout the device uses under /sdcard/apps/<id>/.  Store metadata (category, localized name/description,
featured flag, rating, region gating) lives in a curated overlay file `catalog.json`, merged over each
manifest so the app folders stay clean. Without an overlay entry an app falls back to its manifest's
own name/author/description, and a WASM-4 cart ("wasm4": true) always lands in the "wasm4" category.

Several apps roots can be served at once (repeat --apps-dir): e.g. the repo's apps/ plus a folder of
WASM-4 carts made by tools/w4harness/store.sh. On an id clash the first root wins.

MULTILINGUAL: pass ?lang=it|en|es|fr|de. The server resolves each app's name/description and every
category name to that language (falling back to English, then any).  GEOLOCATED: pass ?region=IT|US|EU|…
(the device reads its Settings → region). Apps whose `regions` list doesn't include the caller's region
(nor "*") are hidden; if no region is given the server infers one from the language.

Usage:
    python appstore_server.py                       # serve ../../apps on 0.0.0.0:8090
    python appstore_server.py --apps-dir ./apps --port 8090
    python appstore_server.py --apps-dir ../../apps --apps-dir ~/w4harness/store-apps
    python appstore_server.py --host 127.0.0.1      # localhost only
"""
import argparse
import html
import json
import os
import re
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import urlparse, parse_qs

# App ids become path segments on the device (directory names) — keep the charset tight (the firmware
# rejects anything else, and it blocks path traversal here).
ID_RE = re.compile(r"^[A-Za-z0-9_-]{1,31}$")

# Only these files are ever served out of an app directory (no arbitrary reads).
SERVABLE = {
    "app.wasm":      "application/wasm",
    "manifest.json": "application/json",
    "icon.argb":     "application/octet-stream",
    "app.aot":       "application/octet-stream",
    "icon.z":        "application/octet-stream",
}

# Description length the device can hold: 128 bytes up to firmware 1.1.88, 256 from the store
# client that sends ?api=3 (which also shows author, license and icon.z). UTF-8, so leave room.
DESC_MAX_OLD = 120
DESC_MAX = 240
SCAN_TTL_S = 15    # a catalog request reuses the folder scan this long (150 carts = 750 file stats)

# The device fonts cover Latin-1 only: anything else draws as an empty box. Typographic
# punctuation gets its ASCII look-alike, the rest (emoji, other scripts) is dropped.
TYPO = {"\u2018": "'", "\u2019": "'", "\u201a": "'", "\u201c": '"', "\u201d": '"', "\u201e": '"',
        "\u2013": "-", "\u2014": "-", "\u2012": "-", "\u2212": "-", "\u2026": "...", "\u00a0": " ",
        "\u2022": "-", "\u2122": "(TM)"}

LANGS = ("en", "it", "es", "fr", "de")

# Countries grouped as "EU" so an app tagged regions:["EU"] reaches every EU device.
EU = {"IT", "ES", "FR", "DE", "PT", "NL", "BE", "IE", "AT", "FI", "GR", "PL", "SE", "DK", "CZ"}

# Fallback region when the caller sends a language but no region.
LANG_REGION = {"it": "IT", "es": "ES", "fr": "FR", "de": "DE", "en": "US"}

APPS_DIRS = []     # set in main()
OVERLAY_PATH = ""  # catalog.json next to this script


def load_overlay():
    """Return the curated overlay {categories:[...], apps:{id:{...}}}, or empty on any problem."""
    try:
        with open(OVERLAY_PATH, "r", encoding="utf-8") as f:
            data = json.load(f)
        return {"categories": data.get("categories", []), "apps": data.get("apps", {})}
    except (OSError, ValueError):
        return {"categories": [], "apps": {}}


def pick_lang(mapping, lang):
    """Resolve a {lang: text} map (or a plain string) to `lang`, falling back en -> any."""
    if isinstance(mapping, str):
        return mapping
    if not isinstance(mapping, dict) or not mapping:
        return ""
    for k in (lang, "en"):
        if mapping.get(k):
            return mapping[k]
    return next((v for v in mapping.values() if v), "")


def read_manifest(app_dir):
    mpath = os.path.join(app_dir, "manifest.json")
    wpath = os.path.join(app_dir, "app.wasm")
    if not (os.path.isfile(mpath) and os.path.isfile(wpath)):
        return None
    try:
        with open(mpath, "r", encoding="utf-8") as f:
            return json.load(f)
    except (OSError, ValueError) as e:
        print(f"  skip {app_dir}: bad manifest.json ({e})", file=sys.stderr)
        return None


def region_allowed(regions, region):
    if not region or region in ("*", "ALL"):
        return True
    if not regions or "*" in regions:
        return True
    if region in regions:
        return True
    if region in EU and "EU" in regions:
        return True
    return False


def latin1(text):
    """What the device can draw: Latin-1, typographic punctuation replaced (see TYPO)."""
    out = "".join(TYPO.get(c, c) for c in str(text))
    return " ".join("".join(c for c in out if ord(c) < 256).split())


def short_desc(text, limit=DESC_MAX):
    """A Latin-1 description of at most `limit` characters (UTF-8 bytes on the device: a Latin-1
    letter above 127 takes two), cut on a word boundary."""
    text = latin1(text)
    if len(text.encode("utf-8")) <= limit:
        return text
    while len((text + "...").encode("utf-8")) > limit:
        text = text[:-1]
    cut = text.rsplit(" ", 1)[0].rstrip(",;:.") if " " in text else text
    return cut + "..."


def app_dir_for(app_id):
    """The directory serving `app_id` (first apps root that has it), or None."""
    for root in APPS_DIRS:
        d = os.path.join(root, app_id)
        if os.path.isfile(os.path.join(d, "manifest.json")) and os.path.isfile(os.path.join(d, "app.wasm")):
            return d
    return None


_scan_lock = threading.Lock()
_scan_cache = (0.0, [])


def scan_apps():
    """The folder scan, reused for SCAN_TTL_S (every language / region asks for the same)."""
    global _scan_cache
    with _scan_lock:
        at, apps = _scan_cache
        if time.time() - at > SCAN_TTL_S:
            apps = _scan_apps()
            _scan_cache = (time.time(), apps)
        return apps


def _file_size(path):
    return os.path.getsize(path) if os.path.isfile(path) else 0


def _scan_apps():
    """Raw scan: [(id, manifest, sizes)] for every valid app dir, sizes = {wasm, aot, icon_z}
    (bytes, 0 = absent) + icon (icon.argb present)."""
    out, seen = [], set()
    for root in APPS_DIRS:
        if not os.path.isdir(root):
            continue
        for name in sorted(os.listdir(root)):
            app_dir = os.path.join(root, name)
            if not os.path.isdir(app_dir):
                continue
            man = read_manifest(app_dir)
            if man is None:
                continue
            app_id = str(man.get("id") or name)
            if not ID_RE.match(app_id) or app_id != name:
                print(f"  skip {name}: invalid id '{app_id}'", file=sys.stderr)
                continue
            if app_id in seen:
                continue
            seen.add(app_id)
            out.append((app_id, man, {
                "wasm":   _file_size(os.path.join(app_dir, "app.wasm")),
                "aot":    _file_size(os.path.join(app_dir, "app.aot")),
                "icon_z": _file_size(os.path.join(app_dir, "icon.z")),
                "icon":   os.path.isfile(os.path.join(app_dir, "icon.argb")),
            }))
    return out


def build_catalog(lang="en", region="", api=2):
    """Assemble the store.json payload for one (lang, region, client api level)."""
    lang = lang if lang in LANGS else "en"
    desc_max = DESC_MAX if api >= 3 else DESC_MAX_OLD
    if not region:
        region = LANG_REGION.get(lang, "")
    overlay = load_overlay()
    ov_apps = overlay["apps"]
    # localized category-name lookup, and a place to count apps per category
    cat_name = {c["id"]: latin1(pick_lang(c.get("name", {}), lang)) for c in overlay["categories"]}
    cat_icon = {c["id"]: c.get("icon", "") for c in overlay["categories"]}
    cat_count = {}

    apps = []
    for app_id, man, sz in scan_apps():
        ov = ov_apps.get(app_id, {})
        regions = ov.get("regions", ["*"])
        if not region_allowed(regions, region):
            continue
        wasm4 = bool(man.get("wasm4"))
        abi = int(man.get("abi", 1) or 1)
        if wasm4:
            abi = max(abi, 2)   # what the device derives for a cart (graphics surface)
        perms = man.get("permissions") or []
        # Every WASM-4 cart lives in the "wasm4" category (Console WASM-4), whatever the curation
        # or the cart's manifest says: they only run inside that console, so they are shown together.
        category = "wasm4" if wasm4 else ov.get("category", man.get("category") or "other")
        name = latin1(pick_lang(ov.get("names"), lang) or man.get("name", app_id)) or app_id
        desc = short_desc(pick_lang(ov.get("descriptions"), lang) or pick_lang(man.get("descriptions"), lang)
                          or pick_lang(man.get("description", ""), lang), desc_max)
        apps.append({
            "id":            app_id,
            "name":          name,
            "version":       str(man.get("version", "?")),
            "author":        latin1(ov.get("author", man.get("author", ""))),
            "description":   desc,
            "category":      category,
            "category_name": cat_name.get(category, category.title()),
            "abi":           abi,
            "size":          sz["wasm"],
            "game":          wasm4 or (abi >= 2 and "gfx" in perms),
            "icon":          sz["icon"],
            "icon_z":        sz["icon_z"],
            "aot":           sz["aot"],
            "license":       latin1(ov.get("license", man.get("license", ""))),
            "source":        latin1(ov.get("source", man.get("source", ""))),
            "featured":      bool(ov.get("featured", man.get("featured", False))),
            "rating":        float(ov.get("rating", 0) or 0),
            "downloads":     int(ov.get("downloads", 0) or 0),
            "regions":       regions,
        })
        cat_count[category] = cat_count.get(category, 0) + 1

    # featured first, then most-downloaded, then name
    apps.sort(key=lambda a: (not a["featured"], -a["downloads"], a["name"].lower()))
    if api < 3:   # older store clients: fields they don't know stay out of their 32 KB buffer
        for a in apps:
            for k in ("icon_z", "license", "source"):
                a.pop(k, None)

    # only categories that actually have visible apps, in overlay order
    categories = []
    for c in overlay["categories"]:
        cid = c["id"]
        if cat_count.get(cid):
            categories.append({"id": cid, "name": cat_name.get(cid, cid.title()),
                               "icon": cat_icon.get(cid, ""), "count": cat_count[cid]})

    return {
        "store":      "NucleoV2 App Store",
        "version":    2,
        "generated":  time.strftime("%Y-%m-%dT%H:%M:%S"),
        "lang":       lang,
        "region":     region or "*",
        "categories": categories,
        "count":      len(apps),
        "apps":       apps,
    }


def index_html(cat):
    e = html.escape
    rows = []
    for a in cat["apps"]:
        kind = "GAME" if a["game"] else "APP"
        star = " ★" if a["featured"] else ""
        by = f"<br><small>{e(a['author'])}</small>" if a["author"] else ""
        lic = e(a.get("license") or "—")
        if a.get("source"):
            lic += f"<br><small><a href='{e(a['source'])}'>source</a></small>"
        files = [f"<a href='/apps/{a['id']}/manifest.json'>manifest</a>",
                 f"<a href='/apps/{a['id']}/app.wasm'>wasm</a>"]
        if a["aot"]:
            files.append(f"<a href='/apps/{a['id']}/app.aot'>aot</a>")
        rows.append(
            f"<tr><td><b>{e(a['name'])}</b>{star}<br><small>{a['id']}</small>{by}</td>"
            f"<td>{e(a['category_name'])}</td><td>{kind}</td>"
            f"<td>{(a['size'] + a['aot']) // 1024} KB</td><td>{lic}</td>"
            f"<td><small>{e(a['description'])}</small></td><td>{' · '.join(files)}</td></tr>"
        )
    body = "\n".join(rows) or "<tr><td colspan=7><i>no apps for this region</i></td></tr>"
    chips = " ".join(f"<span class=c>{e(c['name'])} · {c['count']}</span>" for c in cat["categories"])
    langbar = " ".join(f"<a href='/?lang={l}'>{l.upper()}</a>" for l in LANGS)
    return (
        "<!doctype html><meta charset=utf-8><title>NucleoV2 App Store</title>"
        "<style>body{font:15px/1.5 system-ui,sans-serif;max-width:1100px;margin:40px auto;padding:0 16px}"
        "table{border-collapse:collapse;width:100%}td,th{border-bottom:1px solid #ddd;padding:8px;text-align:left;vertical-align:top}"
        "small{color:#666}a{color:#2563eb;text-decoration:none}"
        ".c{display:inline-block;background:#eef;border-radius:12px;padding:3px 10px;margin:2px;font-size:13px}</style>"
        f"<h1>NucleoV2 App Store</h1>"
        f"<p>{cat['count']} app(s) · lang <b>{cat['lang']}</b> · region <b>{cat['region']}</b> · "
        f"catalog: <a href='/store.json?api=3'>/store.json</a></p>"
        f"<p>Language: {langbar}</p><p>{chips}</p>"
        "<table><tr><th>App</th><th>Category</th><th>Type</th><th>Size</th><th>License</th>"
        f"<th>Description</th><th>Files</th></tr>{body}</table>"
    ).encode("utf-8")


class Handler(BaseHTTPRequestHandler):
    server_version = "NucleoStore/2.0"

    def _send(self, code, body, ctype="text/plain; charset=utf-8"):
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Access-Control-Allow-Origin", "*")
        self.end_headers()
        if self.command != "HEAD":
            self.wfile.write(body)

    def _serve_file(self, path, ctype):
        try:
            with open(path, "rb") as f:
                data = f.read()
        except OSError:
            self._send(404, b"not found")
            return
        self._send(200, data, ctype)

    def _query(self):
        q = parse_qs(urlparse(self.path).query)
        lang = (q.get("lang", ["en"])[0] or "en").lower()[:2]
        region = (q.get("region", [""])[0] or "").upper()[:4]
        if region in ("*", "ALL"):
            region = ""
        try:
            api = int(q.get("api", ["2"])[0])
        except ValueError:
            api = 2
        return lang, region, api

    def do_HEAD(self):
        self.do_GET()

    def do_GET(self):
        route = urlparse(self.path).path

        if route in ("/", "/index.html"):
            lang, region, _ = self._query()
            self._send(200, index_html(build_catalog(lang, region, 3)), "text/html; charset=utf-8")
            return
        if route == "/store.json":
            lang, region, api = self._query()
            payload = json.dumps(build_catalog(lang, region, api)).encode("utf-8")
            self._send(200, payload, "application/json")
            return

        m = re.match(r"^/apps/([^/]+)/([^/]+)$", route)
        if m:
            app_id, fname = m.group(1), m.group(2)
            if not ID_RE.match(app_id) or fname not in SERVABLE:
                self._send(404, b"not found")
                return
            app_dir = app_dir_for(app_id)
            if not app_dir:
                self._send(404, b"not found")
                return
            self._serve_file(os.path.join(app_dir, fname), SERVABLE[fname])
            return

        self._send(404, b"not found")

    def log_message(self, fmt, *args):
        print(f"  {self.address_string()} {fmt % args}")


def main():
    global APPS_DIRS, OVERLAY_PATH
    here = os.path.dirname(os.path.abspath(__file__))
    OVERLAY_PATH = os.path.join(here, "catalog.json")
    default_apps = os.path.normpath(os.path.join(here, "..", "..", "apps"))

    ap = argparse.ArgumentParser(description="NucleoV2 remote WASM app store server")
    ap.add_argument("--apps-dir", action="append",
                    help="directory of <id>/{manifest.json,app.wasm} apps; repeat for several "
                         "(default: repo apps/)")
    ap.add_argument("--overlay", default=OVERLAY_PATH, help="curated catalog.json overlay")
    ap.add_argument("--host", default="0.0.0.0", help="bind address (default: all interfaces)")
    ap.add_argument("--port", type=int, default=8090, help="port (default: 8090)")
    args = ap.parse_args()

    try:
        sys.stdout.reconfigure(encoding="utf-8")   # category names carry accents; don't die on cp1252
    except Exception:
        pass

    APPS_DIRS = [os.path.abspath(os.path.expanduser(d)) for d in (args.apps_dir or [default_apps])]
    OVERLAY_PATH = os.path.abspath(args.overlay)

    cat = build_catalog("en", "")
    print(f"NucleoV2 App Store v2 - {cat['count']} app(s) from {', '.join(APPS_DIRS)}")
    print("  categories: " + ", ".join(f"{c['name']}({c['count']})" for c in cat["categories"]))
    for a in cat["apps"]:
        star = "*" if a["featured"] else " "
        print(f"  {star} {a['id']:<14} {a['category']:<10} v{a['version']:<6} "
              f"{'GAME' if a['game'] else 'APP':<4} {a['size']//1024} KB"
              f"{' +aot' if a['aot'] else ''}")
    print(f"Listening on http://{args.host}:{args.port}   (catalog: /store.json?lang=it&region=IT)")
    print("Set the device Settings -> Update -> App store to this host, then open Apps -> Store.")

    ThreadingHTTPServer((args.host, args.port), Handler).serve_forever()


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        print("\nstopped")
