#!/usr/bin/env python3
"""NucleoV2 remote WASM app store — reference server (Python stdlib only).

A proper little app store: categorized, multilingual, and region-aware. It serves a catalog and the
app files that the on-device store (components/nv_appstore) installs from.

    GET /                          human-readable HTML index (browse in a browser)
    GET /store.json?lang=&region=  catalog, localized + region-filtered for the caller
    GET /apps/<id>/manifest.json   one app's manifest.json (the schema nv_wasm validates)
    GET /apps/<id>/app.wasm        the WebAssembly module
    GET /apps/<id>/icon.argb       optional 80x80 ARGB8888 launcher icon

An "app" is any sub-directory of the apps root holding BOTH manifest.json and app.wasm — the exact
layout the device uses under /sdcard/apps/<id>/.  Store metadata (category, localized name/description,
featured flag, rating, region gating) lives in a curated overlay file `catalog.json`, merged over each
manifest so the app folders stay clean.

MULTILINGUAL: pass ?lang=it|en|es|fr|de. The server resolves each app's name/description and every
category name to that language (falling back to English, then any).  GEOLOCATED: pass ?region=IT|US|EU|…
(the device reads its Settings → region). Apps whose `regions` list doesn't include the caller's region
(nor "*") are hidden; if no region is given the server infers one from the language.

Usage:
    python appstore_server.py                       # serve ../../apps on 0.0.0.0:8090
    python appstore_server.py --apps-dir ./apps --port 8090
    python appstore_server.py --host 127.0.0.1      # localhost only
"""
import argparse
import json
import os
import re
import sys
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
}

LANGS = ("en", "it", "es", "fr", "de")

# Countries grouped as "EU" so an app tagged regions:["EU"] reaches every EU device.
EU = {"IT", "ES", "FR", "DE", "PT", "NL", "BE", "IE", "AT", "FI", "GR", "PL", "SE", "DK", "CZ"}

# Fallback region when the caller sends a language but no region.
LANG_REGION = {"it": "IT", "es": "ES", "fr": "FR", "de": "DE", "en": "US"}

APPS_DIR = ""      # set in main()
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


def scan_apps():
    """Raw scan: [(id, manifest, wasm_size, has_icon)] for every valid app dir."""
    out = []
    if os.path.isdir(APPS_DIR):
        for name in sorted(os.listdir(APPS_DIR)):
            app_dir = os.path.join(APPS_DIR, name)
            if not os.path.isdir(app_dir):
                continue
            man = read_manifest(app_dir)
            if man is None:
                continue
            app_id = str(man.get("id") or name)
            if not ID_RE.match(app_id):
                print(f"  skip {name}: invalid id '{app_id}'", file=sys.stderr)
                continue
            out.append((app_id, man,
                        os.path.getsize(os.path.join(app_dir, "app.wasm")),
                        os.path.isfile(os.path.join(app_dir, "icon.argb"))))
    return out


def build_catalog(lang="en", region=""):
    """Assemble the store.json payload for one (lang, region)."""
    lang = lang if lang in LANGS else "en"
    if not region:
        region = LANG_REGION.get(lang, "")
    overlay = load_overlay()
    ov_apps = overlay["apps"]
    # localized category-name lookup, and a place to count apps per category
    cat_name = {c["id"]: pick_lang(c.get("name", {}), lang) for c in overlay["categories"]}
    cat_icon = {c["id"]: c.get("icon", "") for c in overlay["categories"]}
    cat_count = {}

    apps = []
    for app_id, man, wasm_size, has_icon in scan_apps():
        ov = ov_apps.get(app_id, {})
        regions = ov.get("regions", ["*"])
        if not region_allowed(regions, region):
            continue
        abi = int(man.get("abi", 1) or 1)
        perms = man.get("permissions") or []
        category = ov.get("category", "other")
        name = pick_lang(ov.get("names"), lang) or man.get("name", app_id)
        desc = pick_lang(ov.get("descriptions"), lang) or man.get("description", "")
        apps.append({
            "id":            app_id,
            "name":          name,
            "version":       str(man.get("version", "?")),
            "author":        ov.get("author", man.get("author", "")),
            "description":   desc,
            "category":      category,
            "category_name": cat_name.get(category, category.title()),
            "abi":           abi,
            "size":          wasm_size,
            "game":          abi >= 2 and "gfx" in perms,
            "icon":          has_icon,
            "featured":      bool(ov.get("featured", False)),
            "rating":        float(ov.get("rating", 0) or 0),
            "downloads":     int(ov.get("downloads", 0) or 0),
            "regions":       regions,
        })
        cat_count[category] = cat_count.get(category, 0) + 1

    # featured first, then most-downloaded, then name
    apps.sort(key=lambda a: (not a["featured"], -a["downloads"], a["name"].lower()))

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
    rows = []
    for a in cat["apps"]:
        kind = "GAME" if a["game"] else "APP"
        star = " ★" if a["featured"] else ""
        rating = f"{a['rating']:.1f}" if a["rating"] else "—"
        rows.append(
            f"<tr><td><b>{a['name']}</b>{star}<br><small>{a['id']}</small></td>"
            f"<td>{a['category_name']}</td><td>v{a['version']}</td><td>{kind}</td>"
            f"<td>ABI {a['abi']}</td><td>{a['size'] // 1024} KB</td><td>{rating}</td>"
            f"<td><a href='/apps/{a['id']}/manifest.json'>manifest</a> · "
            f"<a href='/apps/{a['id']}/app.wasm'>wasm</a></td></tr>"
        )
    body = "\n".join(rows) or "<tr><td colspan=8><i>no apps for this region</i></td></tr>"
    chips = " ".join(f"<span class=c>{c['name']} · {c['count']}</span>" for c in cat["categories"])
    langbar = " ".join(f"<a href='/?lang={l}'>{l.upper()}</a>" for l in LANGS)
    return (
        "<!doctype html><meta charset=utf-8><title>NucleoV2 App Store</title>"
        "<style>body{font:15px/1.5 system-ui,sans-serif;max-width:920px;margin:40px auto;padding:0 16px}"
        "table{border-collapse:collapse;width:100%}td,th{border-bottom:1px solid #ddd;padding:8px;text-align:left}"
        "small{color:#888}a{color:#2563eb;text-decoration:none;margin-right:8px}"
        ".c{display:inline-block;background:#eef;border-radius:12px;padding:3px 10px;margin:2px;font-size:13px}</style>"
        f"<h1>NucleoV2 App Store</h1>"
        f"<p>{cat['count']} app(s) · lang <b>{cat['lang']}</b> · region <b>{cat['region']}</b> · "
        f"catalog: <a href='/store.json'>/store.json</a></p>"
        f"<p>Language: {langbar}</p><p>{chips}</p>"
        "<table><tr><th>App</th><th>Category</th><th>Version</th><th>Type</th><th>ABI</th>"
        f"<th>Size</th><th>Rating</th><th>Files</th></tr>{body}</table>"
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
        return lang, region

    def do_HEAD(self):
        self.do_GET()

    def do_GET(self):
        route = urlparse(self.path).path

        if route in ("/", "/index.html"):
            lang, region = self._query()
            self._send(200, index_html(build_catalog(lang, region)), "text/html; charset=utf-8")
            return
        if route == "/store.json":
            lang, region = self._query()
            payload = json.dumps(build_catalog(lang, region)).encode("utf-8")
            self._send(200, payload, "application/json")
            return

        m = re.match(r"^/apps/([^/]+)/([^/]+)$", route)
        if m:
            app_id, fname = m.group(1), m.group(2)
            if not ID_RE.match(app_id) or fname not in SERVABLE:
                self._send(404, b"not found")
                return
            self._serve_file(os.path.join(APPS_DIR, app_id, fname), SERVABLE[fname])
            return

        self._send(404, b"not found")

    def log_message(self, fmt, *args):
        print(f"  {self.address_string()} {fmt % args}")


def main():
    global APPS_DIR, OVERLAY_PATH
    here = os.path.dirname(os.path.abspath(__file__))
    OVERLAY_PATH = os.path.join(here, "catalog.json")
    default_apps = os.path.normpath(os.path.join(here, "..", "..", "apps"))

    ap = argparse.ArgumentParser(description="NucleoV2 remote WASM app store server")
    ap.add_argument("--apps-dir", default=default_apps,
                    help="directory of <id>/{manifest.json,app.wasm} apps (default: repo apps/)")
    ap.add_argument("--overlay", default=OVERLAY_PATH, help="curated catalog.json overlay")
    ap.add_argument("--host", default="0.0.0.0", help="bind address (default: all interfaces)")
    ap.add_argument("--port", type=int, default=8090, help="port (default: 8090)")
    args = ap.parse_args()

    try:
        sys.stdout.reconfigure(encoding="utf-8")   # category names carry accents; don't die on cp1252
    except Exception:
        pass

    APPS_DIR = os.path.abspath(args.apps_dir)
    OVERLAY_PATH = os.path.abspath(args.overlay)

    cat = build_catalog("en", "")
    print(f"NucleoV2 App Store v2 - {cat['count']} app(s) from {APPS_DIR}")
    print("  categories: " + ", ".join(f"{c['name']}({c['count']})" for c in cat["categories"]))
    for a in cat["apps"]:
        star = "*" if a["featured"] else " "
        print(f"  {star} {a['id']:<14} {a['category']:<10} v{a['version']:<6} "
              f"{'GAME' if a['game'] else 'APP':<4} {a['size']//1024} KB")
    print(f"Listening on http://{args.host}:{args.port}   (catalog: /store.json?lang=it&region=IT)")
    print("Set the device Settings -> Update -> App store to this host, then open Apps -> Store.")

    ThreadingHTTPServer((args.host, args.port), Handler).serve_forever()


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        print("\nstopped")
