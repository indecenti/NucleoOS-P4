#!/usr/bin/env python3
"""Export the app store as plain static files — what GitHub Pages (or any web host) can serve.

    python export_static.py --out D:\\nucleoos-p4-store --apps-dir ../../apps --apps-dir D:\\w4store

A static host can't read ?lang= / ?region=, so the catalog is pre-rendered once per language:

    store2-<lang>.json  firmware >= 1.1.142 asks for this first: native apps + one summary row per
                        emulated platform / engine (catalog.json "platforms", with a name index)
    store2-<lang>-<platform>-<k>.json   that platform's carts, part k (256 rows each), fetched
                        when its tab opens
    store-<lang>.json   legacy, firmware 1.1.108 .. 1.1.141 (and the fallback on a 404): every native
                        app, then as many carts as fit 192 rows / 192 KB (what <= 1.1.140 can hold)
    store.json          the English legacy one: what an older device asking /store.json?lang=… receives
    index.html          the browsable catalog (index-<lang>.html for the other languages)
    CREDITS.md          author / license / source of every app (CC BY attribution)
    docs/<id>.html      the app's guide (apps/<id>/GUIDE.md), linked by QR from the device
    privacy.html        what the opt-in statistics send (nv_telemetry), linked by the device's QR
    shots/<id>/<n>.jpg  store screenshots (apps/<id>/shots/), shown on the app page, not installed
    apps/<id>/...       every servable file of every app, the live server's layout

Same catalog logic as appstore_server.py (it is imported, not copied), same overlay catalog.json.
Before the catalogs it updates two files next to it (commit them in the main repo afterwards):
history.json (the day an app first appears / changes version: "added", "updated") and
downloads.json (the anonymous install counter's totals, server/stats/README.md).

Idempotent: a catalog whose content didn't change keeps its old "generated" stamp and identical
files aren't rewritten, so a run with nothing new leaves git clean. App dirs no longer in any
source are removed. Anything else at the top of --out (ota/, README, workflows) is left alone.
"""
import argparse
import datetime
import filecmp
import json
import os
import shutil
import sys
import urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import appstore_server as srv  # noqa: E402
from guides import guide_html  # noqa: E402
sys.path.insert(0, os.path.join(HERE, "..", "..", "tools"))
import store_sign  # noqa: E402  apps/<id>/package.sig (store key on this PC)

CATALOG_CAP = srv.LEGACY_BYTES   # kCatalogCap up to firmware 1.1.140 — a bigger catalog is refused
STORE2_CAP = 512 * 1024          # kCatalogCap from 1.1.141 (store2 main file and each part)


def write_if_changed(path, data):
    """Write bytes unless the file already holds exactly them. Returns True when written."""
    try:
        with open(path, "rb") as f:
            if f.read() == data:
                return False
    except OSError:
        pass
    with open(path, "wb") as f:
        f.write(data)
    return True


def write_catalog(path, cat, cap=CATALOG_CAP):
    """Write a catalog, keeping the old "generated" stamp when nothing else changed."""
    try:
        with open(path, "r", encoding="utf-8") as f:
            old = json.load(f)
        if {**old, "generated": None} == {**cat, "generated": None}:
            cat = {**cat, "generated": old.get("generated", cat["generated"])}
    except (OSError, ValueError):
        pass
    # Compact: no spaces after separators, and false flags left out (the device reads a missing
    # "featured" / "game" as false) - the catalog must fit the device's fixed receive buffer.
    slim = {**cat, "apps": [{k: v for k, v in a.items() if v is not False} for a in cat.get("apps", [])]}
    data = json.dumps(slim, ensure_ascii=False, separators=(",", ":")).encode("utf-8")
    if len(data) > cap:
        sys.exit(f"error: {os.path.basename(path)} is {len(data)} bytes, over the device's "
                 f"{cap}-byte catalog cap")
    return write_if_changed(path, data), len(data)


def write_store2(out, cat, lang):
    """store2-<lang>.json + every platform part. Returns the file names written (kept)."""
    main, carts = srv.store2_split(cat, lang)
    if main["count"] > srv.MAIN_ROWS:
        sys.exit(f"error: store2-{lang}.json has {main['count']} native apps, over the device's "
                 f"{srv.MAIN_ROWS} (NV_STORE_MAX - one platform part)")
    names = [f"store2-{lang}.json"]
    _, size = write_catalog(os.path.join(out, names[0]), main, STORE2_CAP)
    info = [f"{names[0]} {main['count']} apps {size // 1024} KB"]
    for p in main["platforms"]:
        for k in range(1, p["parts"] + 1):
            name = f"store2-{lang}-{p['id']}-{k}.json"
            _, size = write_catalog(os.path.join(out, name), srv.store2_part(p["id"], carts[p["id"]], k),
                                    STORE2_CAP)
            names.append(name)
            info.append(f"{p['id']}-{k} {min(srv.PART_ROWS, p['count'] - (k - 1) * srv.PART_ROWS)} "
                        f"{size // 1024} KB")
    print("  " + " | ".join(info))
    return names


ANIMA_DOES_MAX = 220
ANIMA_TAGS_MAX = 16


def anima_index(cat, main, lang):
    """anima-index-<lang>.json: the store as ANIMA (the on-device assistant) needs it to pick an app
    for a request ("c'e' un'app per convertire le unita'?") and install it. One row per installable
    native app (libraries and single game carts are left out; each cart platform is one row), with
    what it does in one line and search tags. An app's manifest may add {"anima": {"tags": [...],
    "does": {"it": "...", "en": "..."}}} to say what it is useful for in words people use."""
    rows = []
    for a in main["apps"]:
        if a.get("kind") in ("library", "data"):
            continue
        man = srv.read_manifest(srv.app_dir_for(a["id"])) or {}
        hint = man.get("anima") or {}
        does = srv.short_desc(srv.pick_lang(hint.get("does"), lang) or a.get("description", ""), ANIMA_DOES_MAX)
        tags = []
        for t in list(hint.get("tags") or []) + [a.get("category_name", ""), a.get("subcategory_name", "")]:
            t = str(t).strip().lower()
            if t and t not in tags:
                tags.append(t)
        row = {"id": a["id"], "name": a["name"], "cat": a.get("category", ""), "does": does,
               "tags": tags[:ANIMA_TAGS_MAX], "kb": max(1, int(a.get("size", 0) or 0) // 1024)}
        if a.get("game"):
            row["game"] = True
        if a.get("perms"):
            row["perms"] = a["perms"]
        if a.get("console"):
            row["console"] = True
        rows.append(row)
    plats = [{"id": p["id"], "name": p["name"], "count": p["count"], "desc": p.get("desc", ""), "host": p.get("host", "")}
             for p in main.get("platforms", [])]
    return {"anima": 1, "lang": lang, "generated": cat.get("generated", ""), "count": len(rows),
            "apps": rows, "platforms": plats}


def sync_assets(src_dir, dst_dir):
    """Mirror img/ snd/ models/ (only what files.json lists) and write files.json. Returns the
    number of files written or removed."""
    n = 0
    assets = srv.app_assets(src_dir)
    listed = {p for p, _ in assets}
    for p, _ in assets:
        s, d = os.path.join(src_dir, p), os.path.join(dst_dir, p)
        os.makedirs(os.path.dirname(d), exist_ok=True)
        if not (os.path.isfile(d) and filecmp.cmp(s, d, shallow=False)):
            shutil.copyfile(s, d)
            n += 1
    for sub in srv.ASSET_KINDS:
        d = os.path.join(dst_dir, sub)
        if not os.path.isdir(d):
            continue
        for name in os.listdir(d):
            if f"{sub}/{name}" not in listed:
                os.remove(os.path.join(d, name))
                n += 1
        if not os.listdir(d):
            os.rmdir(d)
    fj = os.path.join(dst_dir, "files.json")
    if assets:
        n += write_if_changed(fj, srv.files_json(src_dir))
    elif os.path.exists(fj):
        os.remove(fj)
        n += 1
    return n


def sync_app(src_dir, dst_dir):
    """Mirror the servable files of one app. Returns the number of files written or removed."""
    os.makedirs(dst_dir, exist_ok=True)
    n = sync_assets(src_dir, dst_dir)
    for name in srv.SERVABLE:
        s, d = os.path.join(src_dir, name), os.path.join(dst_dir, name)
        if os.path.isfile(s):
            if not (os.path.isfile(d) and filecmp.cmp(s, d, shallow=False)):
                shutil.copyfile(s, d)
                n += 1
        elif os.path.exists(d):
            os.remove(d)
            n += 1
    for name in os.listdir(dst_dir):   # anything that isn't a servable file doesn't belong here
        if (name not in srv.SERVABLE and name not in ("files.json", store_sign.SIG_NAME)
                and name not in srv.ASSET_KINDS):
            path = os.path.join(dst_dir, name)
            if os.path.isdir(path):
                shutil.rmtree(path)
            else:
                os.remove(path)
            n += 1
    return n


def credits_md(cat):
    rows = ["# Credits", "",
            "Every app in this store, who made it and under which license. The WASM-4 carts are "
            "the authors' work published on wasm4.org under CC BY-NC-SA 4.0 "
            "([LICENSE-carts.txt](LICENSE-carts.txt)); the `app.aot` next to a cart is the same "
            "cart compiled ahead of time for the ESP32-P4 and is shared under the same license. "
            "Non-commercial use only.", "",
            "| App | Id | Author | License | Source |", "|---|---|---|---|---|"]
    for a in sorted(cat["apps"], key=lambda a: a["name"].lower()):
        src = f"[link]({a['source']})" if a.get("source") else ""
        rows.append(f"| {a['name']} | `{a['id']}` | {a.get('author') or ''} | "
                    f"{a.get('license') or ''} | {src} |")
    return ("\n".join(rows) + "\n").encode("utf-8")


def update_history(apps):
    """history.json: a new app is "added" today; a version that changed is "updated" today.
    Returns the number of apps whose entry changed."""
    hist = srv.load_history()
    today = datetime.date.today().isoformat()
    n = 0
    for app_id, man, _ in apps:
        ver = str(man.get("version", "?"))
        h = hist.get(app_id)
        if not h:
            hist[app_id] = {"added": today, "version": ver}
        elif h.get("version") != ver:
            h["version"], h["updated"] = ver, today
        else:
            continue
        n += 1
    if n:
        with open(srv.HISTORY_PATH, "w", encoding="utf-8") as f:
            json.dump(hist, f, indent=0, sort_keys=True)
            f.write("\n")
    return n


def refresh_downloads():
    """Fetch the install counter's totals into downloads.json. On any failure the last good copy
    stays (the catalog must never drop to zero because the stats host was down)."""
    try:
        with urllib.request.urlopen(srv.STATS_URL, timeout=8) as r:
            data = json.loads(r.read(1 << 20).decode("utf-8"))
        apps = {k: {"i": int(v.get("i", 0)), "u": int(v.get("u", 0))}
                for k, v in (data.get("apps") or {}).items()
                if srv.ID_RE.match(k) and isinstance(v, dict)}
    except Exception as e:   # noqa: BLE001  network, JSON, shape: all mean "keep the old numbers"
        print(f"  downloads: {srv.STATS_URL} unavailable ({e}), keeping the last totals")
        return
    body = json.dumps({"generated": data.get("generated", ""), "apps": apps}, indent=0, sort_keys=True)
    write_if_changed(srv.DOWNLOADS_PATH, (body + "\n").encode("utf-8"))
    print(f"  downloads: {sum(v['i'] for v in apps.values())} install(s) over {len(apps)} app(s)")


def main():
    repo = os.path.normpath(os.path.join(HERE, "..", ".."))
    ap = argparse.ArgumentParser(description="export the app store as static files")
    ap.add_argument("--out", required=True, help="output folder (the Pages repo checkout)")
    ap.add_argument("--apps-dir", action="append",
                    help="apps root, repeat for several; first wins on an id clash "
                         "(default: the repo's apps/ then D:\\w4store)")
    ap.add_argument("--overlay", default=os.path.join(HERE, "catalog.json"), help="curated overlay")
    ap.add_argument("--unsigned", action="store_true",
                    help="skip package.sig (test exports only: devices refuse unsigned apps)")
    args = ap.parse_args()
    try:
        sys.stdout.reconfigure(encoding="utf-8")
    except Exception:
        pass

    srv.APPS_DIRS = [os.path.abspath(d) for d in
                     (args.apps_dir or [os.path.join(repo, "apps"), r"D:\w4store"])]
    srv.OVERLAY_PATH = os.path.abspath(args.overlay)
    out = os.path.abspath(args.out)
    os.makedirs(os.path.join(out, "apps"), exist_ok=True)

    apps = srv._scan_apps()
    if not apps:
        sys.exit("error: no apps found in " + ", ".join(srv.APPS_DIRS))

    # store dates and install counts first: every catalog below carries them
    hidden = {i for i, o in srv.load_overlay()["apps"].items() if o.get("hidden")}
    changed = update_history([a for a in apps if a[0] not in hidden])
    print(f"  history.json  {changed} app(s) added/updated")
    refresh_downloads()

    # catalogs + browsable pages, one per language
    store2 = set()
    for lang in srv.LANGS:
        cat = srv.build_catalog(lang, "*", 3, public=True)
        legacy = srv.legacy_catalog(cat, lang)
        changed, size = write_catalog(os.path.join(out, f"store-{lang}.json"), legacy)
        print(f"  store-{lang}.json  {legacy['count']}/{cat['count']} apps (legacy)  {size // 1024} KB"
              f"{'  (updated)' if changed else ''}")
        store2.update(write_store2(out, cat, lang))
        idx = anima_index(cat, srv.store2_split(cat, lang)[0], lang)
        changed, size = write_catalog(os.path.join(out, f"anima-index-{lang}.json"), idx)
        print(f"  anima-index-{lang}.json  {idx['count']} apps for ANIMA  {size // 1024} KB"
              f"{'  (updated)' if changed else ''}")
        page = "index.html" if lang == "en" else f"index-{lang}.html"
        write_if_changed(os.path.join(out, page), srv.index_html(cat, static=True))
        if lang == "en":
            write_catalog(os.path.join(out, "store.json"), legacy)
            write_if_changed(os.path.join(out, "CREDITS.md"), credits_md(cat))
    for name in os.listdir(out):   # parts of a platform that shrank or went away
        if name.startswith("store2-") and name.endswith(".json") and name not in store2:
            os.remove(os.path.join(out, name))

    # app files (the overlay's "hidden" apps stay off the public store)
    ids = {app_id for app_id, _, _ in apps} - hidden
    touched = sum(sync_app(srv.app_dir_for(i), os.path.join(out, "apps", i)) for i in sorted(ids))
    # Sign every published package (firmware >= 1.1.128 refuses unsigned store apps). The signature
    # covers exactly the files just mirrored, so it is made last; an unchanged package keeps its sig.
    if args.unsigned:
        print("  WARNING: --unsigned: packages not signed, current firmware will refuse them")
    else:
        key = store_sign.load_private()
        store_sign.check_pub(key)
        signed = sum(store_sign.sign_dir(os.path.join(out, "apps", i), key) for i in sorted(ids))
        touched += signed
        print(f"  package.sig  {signed} (re)signed, {len(ids) - signed} unchanged")
    # ANIMA's knowledge packs: data/<id>/pack.sig, the signed list of the files (on GitHub release assets)
    packs = srv.load_data_packs()
    if packs and not args.unsigned:
        os.makedirs(os.path.join(out, "data"), exist_ok=True)
        for pk in packs:
            text = store_sign.sign_text(store_sign.data_pack_text(pk), key)
            d = os.path.join(out, "data", pk["id"])
            os.makedirs(d, exist_ok=True)
            write_if_changed(os.path.join(d, "pack.sig"), text if isinstance(text, bytes) else text.encode("ascii"))
        print(f"  data/*/pack.sig  {len(packs)} knowledge pack(s) signed")
    gone = [d for d in os.listdir(os.path.join(out, "apps")) if d not in ids]
    for d in gone:
        shutil.rmtree(os.path.join(out, "apps", d))

    # store screenshots: <out>/shots/<id>/<n>.jpg, next to the packages, not inside them (the device
    # never installs them, package.sig doesn't cover them)
    shots_root = os.path.join(out, "shots")
    os.makedirs(shots_root, exist_ok=True)
    for i in sorted(ids):
        src = srv.app_shots(srv.app_dir_for(i))
        dst = os.path.join(shots_root, i)
        want = {f"{n}.jpg" for n in range(1, len(src) + 1)}
        if src:
            os.makedirs(dst, exist_ok=True)
        for n, path in enumerate(src, 1):
            with open(path, "rb") as f:
                touched += write_if_changed(os.path.join(dst, f"{n}.jpg"), f.read())
        if os.path.isdir(dst):
            for name in os.listdir(dst):
                if name not in want:
                    os.remove(os.path.join(dst, name))
                    touched += 1
            if not os.listdir(dst):
                os.rmdir(dst)
    for d in os.listdir(shots_root):
        if d not in ids:
            shutil.rmtree(os.path.join(shots_root, d))

    # the statistics notice the device links by QR (setup wizard, Settings > Security)
    with open(os.path.join(HERE, "privacy.html"), "rb") as f:
        touched += write_if_changed(os.path.join(out, "privacy.html"), f.read())

    # license texts shipped at the top of an apps root (D:\w4store\LICENSE-carts.txt)
    for root in srv.APPS_DIRS:
        for name in os.listdir(root) if os.path.isdir(root) else []:
            if name.upper().startswith("LICENSE") and os.path.isfile(os.path.join(root, name)):
                with open(os.path.join(root, name), "rb") as f:
                    write_if_changed(os.path.join(out, name), f.read())

    # guide pages, one per app with a GUIDE.md (stale ones go)
    docs = os.path.join(out, "docs")
    os.makedirs(docs, exist_ok=True)
    ov_apps = srv.load_overlay()["apps"]
    guided = set()
    for i in sorted(ids):
        d = srv.app_dir_for(i)
        page = guide_html(i, d, srv.read_manifest(d) or {}, ov_apps.get(i), store_href="../index-it.html")
        if page:
            guided.add(f"{i}.html")
            touched += write_if_changed(os.path.join(docs, f"{i}.html"), page)
    for name in os.listdir(docs):
        if name not in guided:
            os.remove(os.path.join(docs, name))
            touched += 1
    print(f"  docs/  {len(guided)} guide page(s)")

    print(f"exported {len(ids)} apps to {out}: {touched} file(s) written/removed"
          + (f", removed {len(gone)} app(s): {', '.join(gone)}" if gone else ""))


if __name__ == "__main__":
    main()
