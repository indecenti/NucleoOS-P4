#!/usr/bin/env python3
"""NucleoV2 remote WASM app store — reference server (Python stdlib only).

A proper little app store: categorized, multilingual, and region-aware. It serves a catalog and the
app files that the on-device store (components/nv_appstore) installs from.

    GET /                          human-readable HTML index (browse in a browser)
    GET /store.json?lang=&region=&api=  catalog, localized + region-filtered for the caller
                                   (api=3: longer descriptions + author, license, icon.z sizes)
    GET /docs/<id>.html            the app's guide (GUIDE.md), a phone page the device links by QR
    GET /store-<lang>.json         the static store's catalog (export_static.py): one language, all
                                   regions, api 3 — what the device asks for first
    GET /apps/<id>/manifest.json  one app's manifest.json (the schema nv_wasm validates)
    GET /apps/<id>/app.wasm        the WebAssembly module
    GET /apps/<id>/icon.argb       optional 80x80 ARGB8888 launcher icon
    GET /apps/<id>/app.aot         optional precompiled (wamrc) image the device runs instead
    GET /apps/<id>/icon.z          optional 80x80 ARGB8888 icon, raw-deflate compressed (~1-2 KB)
    GET /apps/<id>/files.json      the package's assets {"files":[{"p":"img/x.565","n":bytes}]}
    GET /apps/<id>/<img|snd|models>/<name>   one asset (.565 texture, .wav sound, .vxm model)
    GET /shots/<id>/<n>.jpg        store screenshot n (1..), from apps/<id>/shots/: shown on the
                                   device's app page and the web page, never installed

An "app" is any sub-directory of an apps root holding BOTH manifest.json and app.wasm — the exact
layout the device uses under /sdcard/apps/<id>/. A library ("kind": "library" in the manifest) is a
package other apps require: it may ship assets only, no app.wasm. A catalog row carries the
manifest's "requires" ({"<id>": "<min version>"}), "kind" and the number of asset files, so the
device installs a game's packages before the game.  Store metadata (category, localized name/description,
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

from guides import guide_html, guide_langs
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "tools"))
try:
    import store_sign  # apps/<id>/package.sig, signed with the store key when this PC has it
except ImportError:    # no `cryptography` package: the local store serves unsigned apps
    store_sign = None

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

# Manifest permissions shown to the user before install (the rest are harmless and not listed).
SENSITIVE_PERMS = ("net", "lan", "ws", "mqtt", "ha", "fs", "camera", "mic")

# Assets a package may ship next to its module: sub-folder -> extension (what nv_wasm can open).
ASSET_KINDS = {"img": ".565", "snd": ".wav", "models": ".vxm"}
ASSET_NAME_RE = re.compile(r"^[A-Za-z0-9_-]{1,31}$")
ASSET_MAX = 4 * 1024 * 1024        # per file, and 24 MB a package: the device's own caps
ASSETS_MAX = 24 * 1024 * 1024
ASSET_CTYPE = {".565": "application/octet-stream", ".wav": "audio/wav", ".vxm": "application/octet-stream"}

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

# Store bookkeeping next to this script, kept up to date by export_static.py (the live server only
# reads them): when each app first appeared in the public store and when its version last changed
# ({"<id>": {"added": "YYYY-MM-DD", "updated": "YYYY-MM-DD", "version": "1.2"}}), and the install
# counter's totals ({"<id>": {"i": installs, "u": updates}}, fetched from STATS_URL, see
# server/stats/README.md). The catalog's "downloads" is the real install count, nothing curated.
HISTORY_PATH = os.path.join(os.path.dirname(os.path.abspath(__file__)), "history.json")
DOWNLOADS_PATH = os.path.join(os.path.dirname(os.path.abspath(__file__)), "downloads.json")
STATS_URL = "https://nucleoos.indexhub.it/stats/downloads.json"
NOTES_MAX = 160    # "what's new" line (device: nv_store_entry_t.notes)
SHOTS_MAX = 6      # screenshots per app: apps/<id>/shots/1.jpg .. 6.jpg (baseline JPEG, <= 512x300)
SHOT_BYTES = 96 * 1024


def app_shots(app_dir):
    """Paths of the app's store screenshots, shots/1.jpg, 2.jpg, ... (stops at the first gap)."""
    out = []
    for n in range(1, SHOTS_MAX + 1):
        path = os.path.join(app_dir, "shots", f"{n}.jpg")
        if not os.path.isfile(path) or os.path.getsize(path) > SHOT_BYTES:
            break
        out.append(path)
    return out


def load_overlay():
    """Return the curated overlay {categories:[...], apps:{id:{...}}}, or empty on any problem."""
    try:
        with open(OVERLAY_PATH, "r", encoding="utf-8") as f:
            data = json.load(f)
        return {"categories": data.get("categories", []), "apps": data.get("apps", {})}
    except (OSError, ValueError):
        return {"categories": [], "apps": {}}


def load_json(path, default):
    try:
        with open(path, "r", encoding="utf-8") as f:
            data = json.load(f)
        return data if isinstance(data, type(default)) else default
    except (OSError, ValueError):
        return default


def load_history():
    return load_json(HISTORY_PATH, {})


def load_downloads():
    return load_json(DOWNLOADS_PATH, {}).get("apps", {})


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
    if not os.path.isfile(mpath):
        return None
    try:
        with open(mpath, "r", encoding="utf-8") as f:
            man = json.load(f)
    except (OSError, ValueError) as e:
        print(f"  skip {app_dir}: bad manifest.json ({e})", file=sys.stderr)
        return None
    if not isinstance(man, dict):
        return None
    if not os.path.isfile(wpath) and man.get("kind") != "library":   # a library may be data only
        return None
    return man


def app_assets(app_dir):
    """[(relative path, bytes)] of the servable assets in img/ snd/ models/, sorted; files over the
    per-file cap or past the package cap are left out (the device would refuse them)."""
    out, total = [], 0
    for sub, ext in ASSET_KINDS.items():
        d = os.path.join(app_dir, sub)
        if not os.path.isdir(d):
            continue
        for name in sorted(os.listdir(d)):
            stem, e = os.path.splitext(name)
            path = os.path.join(d, name)
            if e != ext or not ASSET_NAME_RE.match(stem) or not os.path.isfile(path):
                continue
            n = os.path.getsize(path)
            if n > ASSET_MAX or total + n > ASSETS_MAX:
                print(f"  {app_dir}: asset {sub}/{name} over the size cap, not published", file=sys.stderr)
                continue
            total += n
            out.append((f"{sub}/{name}", n))
    return out


def requires_of(man):
    """The manifest's "requires", validated like the device does (id -> version string)."""
    req = man.get("requires")
    if not isinstance(req, dict):
        return {}
    out = {}
    for k, v in list(req.items())[:4]:
        v = str(v)
        if ID_RE.match(k) and re.match(r"^[0-9.]{1,11}$", v):
            out[k] = v
    return out


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
        if read_manifest(d) is not None:   # an app (manifest + module) or a data-only library
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
                "assets": app_assets(app_dir),
                "guide":  bool(guide_langs(app_dir)),
                "shots":  len(app_shots(app_dir)),
            }))
    return out


def build_catalog(lang="en", region="", api=2, public=False):
    """Assemble the store.json payload for one (lang, region, client api level). `public` (the
    GitHub Pages export) leaves out the overlay's "hidden" apps: SDK samples and test apps stay on
    the local dev store only."""
    lang = lang if lang in LANGS else "en"
    desc_max = DESC_MAX if api >= 3 else DESC_MAX_OLD
    if not region:
        region = LANG_REGION.get(lang, "")
    overlay = load_overlay()
    ov_apps = overlay["apps"]
    history = load_history()
    counts = load_downloads()
    # localized category-name lookup, and a place to count apps per category
    cat_name = {c["id"]: latin1(pick_lang(c.get("name", {}), lang)) for c in overlay["categories"]}
    cat_icon = {c["id"]: c.get("icon", "") for c in overlay["categories"]}
    cat_count = {}

    apps = []
    for app_id, man, sz in scan_apps():
        ov = ov_apps.get(app_id, {})
        if public and ov.get("hidden"):
            continue
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
            "downloads":     int((counts.get(app_id) or {}).get("i", 0) or 0),
            "regions":       regions,
        })
        # Store dates (history.json): "added" = first published, "updated" only when a later
        # version replaced the first one. What's new in this version: overlay / manifest "notes".
        h = history.get(app_id) or {}
        if h.get("added"):
            apps[-1]["added"] = h["added"]
            if h.get("updated") and h["updated"] != h["added"]:
                apps[-1]["updated"] = h["updated"]
        notes = short_desc(pick_lang(ov.get("notes"), lang) or pick_lang(man.get("notes", ""), lang), NOTES_MAX)
        if notes:
            apps[-1]["notes"] = notes
        # Permissions the user is asked to accept before install (device: nv_appstore "perms").
        # Only the sensitive ones: gfx/ui/log/home are harmless and would just bloat the catalog.
        sensitive = [p for p in perms if p in SENSITIVE_PERMS]
        if sensitive:
            apps[-1]["perms"] = sensitive
        if man.get("kind") == "library":
            apps[-1]["kind"] = "library"
            apps[-1]["game"] = False
        req = requires_of(man)
        if req:
            apps[-1]["requires"] = req
        if sz["assets"]:
            apps[-1]["files"] = len(sz["assets"])
        if sz["shots"]:
            apps[-1]["shots"] = sz["shots"]   # <store>/shots/<id>/<n>.jpg (device: app page)
        if sz["guide"]:
            apps[-1]["doc"] = True        # <store>/docs/<id>.html (device: QR on the app page)
        if man.get("console"):
            apps[-1]["console"] = True    # terminal program: no window, runs in the Terminal
        cat_count[category] = cat_count.get(category, 0) + 1

    # featured first, then most-downloaded, then name
    apps.sort(key=lambda a: (not a["featured"], -a["downloads"], a["name"].lower()))
    if api < 3:   # older store clients: fields they don't know stay out of their 32 KB buffer
        for a in apps:
            for k in ("icon_z", "license", "source", "doc", "console", "added", "updated", "notes", "shots"):
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


def files_json(app_dir):
    """apps/<id>/files.json: the asset list the device downloads with the package."""
    files = [{"p": p, "n": n} for p, n in app_assets(app_dir)]
    return json.dumps({"files": files}, separators=(",", ":")).encode("utf-8")


_SIG_CACHE = {}
_SIG_LOCK = threading.Lock()


def package_sig(app_id, app_dir):
    """apps/<id>/package.sig over exactly what this server serves for the app (tools/store_sign.py),
    or None without a store key. Cached until a served file changes."""
    if not store_sign or not store_sign.have_key():
        return None
    names = [n for n in SERVABLE if os.path.isfile(os.path.join(app_dir, n))]
    assets = [p for p, _ in app_assets(app_dir)]
    stamp = tuple((p, os.path.getmtime(os.path.join(app_dir, p)), os.path.getsize(os.path.join(app_dir, p)))
                  for p in names + assets)
    with _SIG_LOCK:
        hit = _SIG_CACHE.get(app_id)
        if hit and hit[0] == stamp:
            return hit[1]
    entries = [("files.json", files_json(app_dir))] if assets else []   # export writes it only then
    for p in names + assets:
        with open(os.path.join(app_dir, p), "rb") as f:
            entries.append((p, f.read()))
    ver = str((read_manifest(app_dir) or {}).get("version", "?"))
    try:
        body = store_sign.sign_text(store_sign.package_text(app_id, ver, entries))
    except ValueError as e:
        print(f"  {app_id}: not signable ({e})", file=sys.stderr)
        return None
    with _SIG_LOCK:
        _SIG_CACHE[app_id] = (stamp, body)
    return body


def index_html(cat, static=False):
    """The browsable catalog page. `static` makes every link relative (a GitHub Pages project site
    lives under /<repo>/, and it can't answer ?lang=): languages become index-<lang>.html files."""
    e = html.escape
    root = "" if static else "/"
    rows = []
    for a in cat["apps"]:
        kind = "GAME" if a["game"] else "APP"
        star = " ★" if a["featured"] else ""
        by = f"<br><small>{e(a['author'])}</small>" if a["author"] else ""
        lic = e(a.get("license") or "—")
        if a.get("source"):
            lic += f"<br><small><a href='{e(a['source'])}'>source</a></small>"
        files = [f"<a href='{root}apps/{a['id']}/manifest.json'>manifest</a>",
                 f"<a href='{root}apps/{a['id']}/app.wasm'>wasm</a>"]
        if a["aot"]:
            files.append(f"<a href='{root}apps/{a['id']}/app.aot'>aot</a>")
        if a.get("doc"):
            files.insert(0, f"<a href='{root}docs/{a['id']}.html'><b>guida</b></a>")
        dates = e(a.get("added", ""))
        if a.get("updated"):
            dates += f"<br><small>upd {e(a['updated'])}</small>"
        dl = f"<br><small>{a['downloads']} installs</small>" if a.get("downloads") else ""
        notes = f"<br><small><i>{e(a['notes'])}</i></small>" if a.get("notes") else ""
        shots = "".join(f"<a href='{root}shots/{a['id']}/{n}.jpg'><img class=sh1 loading=lazy "
                        f"src='{root}shots/{a['id']}/{n}.jpg' alt=''></a>" for n in range(1, a.get("shots", 0) + 1))
        if shots:
            notes += f"<div class=shots>{shots}</div>"
        rows.append(
            f"<tr id='{a['id']}'><td><b>{e(a['name'])}</b>{star} <small>v{e(a['version'])}</small>"
            f"<br><small>{a['id']}</small>{by}{dl}</td>"
            f"<td>{e(a['category_name'])}</td><td>{kind}</td>"
            f"<td>{(a['size'] + a['aot']) // 1024} KB</td><td>{dates}</td><td>{lic}</td>"
            f"<td><small>{e(a['description'])}</small>{notes}</td><td>{' · '.join(files)}</td></tr>"
        )
    # The shelves the device shows on its Discover page: most installed, newest, last updated.
    shown = [a for a in cat["apps"] if a.get("kind") != "library"]

    def shelf(title, items, fact):
        if not items:
            return ""
        li = "".join(f"<li><a href='#{a['id']}'>{e(a['name'])}</a> <small>{fact(a)}</small></li>" for a in items)
        return f"<div class=s><h3>{title}</h3><ol>{li}</ol></div>"
    top = sorted((a for a in shown if a.get("downloads")), key=lambda a: -a["downloads"])[:8]
    new = sorted((a for a in shown if a.get("added")),
                 key=lambda a: (a["added"], a["category"] != "wasm4"), reverse=True)[:8]
    upd = sorted((a for a in shown if a.get("updated")), key=lambda a: a["updated"], reverse=True)[:8]
    shelves = (shelf("Most downloaded", top, lambda a: f"{a['downloads']} installs")
               + shelf("New", new, lambda a: e(a["added"]))
               + shelf("Recently updated", upd, lambda a: f"v{e(a['version'])} · {e(a['updated'])}"))
    body = "\n".join(rows) or "<tr><td colspan=8><i>no apps for this region</i></td></tr>"
    chips = " ".join(f"<span class=c>{e(c['name'])} · {c['count']}</span>" for c in cat["categories"])
    if static:
        langbar = " ".join(f"<a href='{'index' if l == 'en' else 'index-' + l}.html'>{l.upper()}</a>" for l in LANGS)
        catalog = f"store-{cat['lang']}.json"
        head = ("<title>NucleoOS P4 App Store — Guition JC1060P470C (ESP32-P4)</title>"
                "<meta name=description content=\"Apps, games and firmware for NucleoOS P4, the custom OS "
                "for the Guition JC1060P470C ESP32-P4 7-inch display. Flash it from the browser.\">")
        intro = ("<p><b>New board?</b> <a href='flash/'>Install NucleoOS P4 from the browser</a> on a "
                 "Guition JC1060P470C (ESP32-P4 7\") · "
                 "<a href='https://github.com/indecenti/NucleoOS-P4'>source on GitHub</a></p>")
    else:
        langbar = " ".join(f"<a href='/?lang={l}'>{l.upper()}</a>" for l in LANGS)
        catalog = "/store.json?api=3"
        head, intro = "<title>NucleoV2 App Store</title>", ""
    return (
        f"<!doctype html><meta charset=utf-8>{head}"
        "<meta name=viewport content='width=device-width,initial-scale=1'>"
        "<style>body{font:15px/1.5 system-ui,sans-serif;max-width:1100px;margin:40px auto;padding:0 16px}"
        "table{border-collapse:collapse;width:100%}td,th{border-bottom:1px solid #ddd;padding:8px;text-align:left;vertical-align:top}"
        "small{color:#666}a{color:#2563eb;text-decoration:none}"
        ".c{display:inline-block;background:#eef;border-radius:12px;padding:3px 10px;margin:2px;font-size:13px}"
        ".sh{display:flex;flex-wrap:wrap;gap:16px;margin:16px 0}.s{flex:1 1 280px;background:#f6f7fb;"
        "border-radius:10px;padding:4px 16px}.s h3{margin:10px 0 4px}.s ol{margin:0 0 12px;padding-left:20px}"
        "tr:target{background:#fff8d6}.shots{display:flex;gap:6px;overflow-x:auto;margin-top:6px}"
        ".sh1{height:90px;border-radius:6px}</style>"
        f"<h1>NucleoV2 App Store</h1>{intro}"
        f"<p>{cat['count']} app(s) · lang <b>{cat['lang']}</b> · region <b>{cat['region']}</b> · "
        f"catalog: <a href='{catalog}'>{catalog.split('?')[0]}</a></p>"
        f"<p>Language: {langbar}</p><p>{chips}</p><div class=sh>{shelves}</div>"
        "<table><tr><th>App</th><th>Category</th><th>Type</th><th>Size</th><th>Added</th><th>License</th>"
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
        # The static layout (export_static.py / GitHub Pages): one pre-rendered catalog per
        # language, every region. Served here too so a device can't tell the two stores apart.
        m = re.match(r"^/store-([a-z]{2})\.json$", route)
        if m and m.group(1) in LANGS:
            payload = json.dumps(build_catalog(m.group(1), "*", 3)).encode("utf-8")
            self._send(200, payload, "application/json")
            return

        m = re.match(r"^/docs/([^/]+)\.html$", route)
        if m and ID_RE.match(m.group(1)) and app_dir_for(m.group(1)):
            app_dir = app_dir_for(m.group(1))
            page = guide_html(m.group(1), app_dir, read_manifest(app_dir) or {},
                              load_overlay()["apps"].get(m.group(1)), store_href="/")
            if page:
                self._send(200, page, "text/html; charset=utf-8")
                return

        m = re.match(r"^/shots/([^/]+)/([1-9])\.jpg$", route)
        if m and ID_RE.match(m.group(1)) and app_dir_for(m.group(1)):
            shots = app_shots(app_dir_for(m.group(1)))
            if int(m.group(2)) <= len(shots):
                self._serve_file(shots[int(m.group(2)) - 1], "image/jpeg")
                return

        m = re.match(r"^/apps/([^/]+)/package\.sig$", route)
        if m and ID_RE.match(m.group(1)) and app_dir_for(m.group(1)):
            body = package_sig(m.group(1), app_dir_for(m.group(1)))
            if body is None:
                self._send(404, b"not signed (no store key on this PC)")
            else:
                self._send(200, body, "text/plain; charset=us-ascii")
            return

        m = re.match(r"^/apps/([^/]+)/files\.json$", route)
        if m and ID_RE.match(m.group(1)) and app_dir_for(m.group(1)):
            self._send(200, files_json(app_dir_for(m.group(1))), "application/json")
            return
        m = re.match(r"^/apps/([^/]+)/(img|snd|models)/([^/]+)$", route)
        if m:
            app_id, sub, name = m.groups()
            stem, ext = os.path.splitext(name)
            app_dir = app_dir_for(app_id) if ID_RE.match(app_id) else None
            if not app_dir or ASSET_KINDS[sub] != ext or not ASSET_NAME_RE.match(stem):
                self._send(404, b"not found")
                return
            self._serve_file(os.path.join(app_dir, sub, name), ASSET_CTYPE[ext])
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
