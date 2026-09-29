#!/usr/bin/env python3
"""Publish to the NucleoOS-P4 distribution repo (GitHub Pages): the app store and the firmware.

    python tools/dist.py store [-m "message"] [--apps-dir DIR ...]
    python tools/dist.py firmware [--version 1.1.106] [--bin build/nucleos-anima.bin] [--notes "..."]
    python tools/dist.py status

The repo is indecenti/nucleoos-p4-store, checked out at D:\\nucleoos-p4-store (--dist or
NUCLEO_DIST to change it; cloned on first use). Pages serves it at
https://indecenti.github.io/nucleoos-p4-store: the device's default store and OTA URLs from 1.1.106.

store      server/appstore/export_static.py into the checkout (per-language catalogs + app files,
           default sources: the repo's apps/ then D:\\w4store), then commit and push.
firmware   the image goes into a GitHub Release (v<version>, asset nucleos-anima.bin) and
           ota/manifest.json points at the copy the Pages workflow puts in ota/, checked against
           the sha256 written here. The version is read from the image itself.
status     what Pages serves right now.

Pages deploys about a minute after a push; store and firmware wait until the live site shows the
change (--no-wait to skip). Several sessions publish from this PC: every command pulls first and
refuses a checkout with uncommitted changes.
"""
import argparse
import hashlib
import json
import os
import shutil
import subprocess
import sys
import tempfile
import time
import urllib.error
import urllib.request

REPO = "indecenti/nucleoos-p4-store"
PAGES = "https://indecenti.github.io/nucleoos-p4-store"
ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
BIN_NAME = "nucleos-anima.bin"
APP_DESC_MAGIC = 0xABCD5432   # esp_app_desc_t, right after the image + first segment headers


def run(cmd, cwd=None, check=True, capture=False):
    r = subprocess.run(cmd, cwd=cwd, text=True, encoding="utf-8", capture_output=capture)
    if check and r.returncode:
        if capture:
            sys.stderr.write((r.stdout or "") + (r.stderr or ""))
        sys.exit(f"error: '{' '.join(cmd)}' failed (exit {r.returncode})")
    return r


def checkout(dist):
    """Clone on first use, refuse local edits, then catch up with what others pushed."""
    if not os.path.isdir(os.path.join(dist, ".git")):
        run(["gh", "repo", "clone", REPO, dist])
    if run(["git", "status", "--porcelain"], cwd=dist, capture=True).stdout.strip():
        sys.exit(f"error: {dist} has uncommitted changes (another publish half-done?) - look first")
    if run(["git", "ls-remote", "--heads", "origin", "main"], cwd=dist, capture=True).stdout.strip():
        run(["git", "pull", "--rebase", "--quiet", "origin", "main"], cwd=dist)


def commit_push(dist, message):
    """Commit everything in the checkout and push. Returns False when there was nothing to commit."""
    run(["git", "add", "-A"], cwd=dist)
    if run(["git", "diff", "--cached", "--quiet"], cwd=dist, check=False).returncode == 0:
        return False
    run(["git", "commit", "--quiet", "-m", message], cwd=dist)
    for _ in range(3):   # another session may have pushed in between
        if run(["git", "push", "--quiet", "origin", "HEAD:main"], cwd=dist, check=False).returncode == 0:
            return True
        run(["git", "pull", "--rebase", "--quiet", "origin", "main"], cwd=dist)
    sys.exit("error: push rejected three times")


def fetch(url, method="GET"):
    """(status, body) of a URL, bypassing the Pages CDN cache (it keys on the query string)."""
    sep = "&" if "?" in url else "?"
    req = urllib.request.Request(f"{url}{sep}nocache={time.time_ns()}", method=method)
    try:
        with urllib.request.urlopen(req, timeout=20) as r:
            return r.status, (r.read() if method == "GET" else b""), dict(r.headers)
    except urllib.error.HTTPError as e:
        return e.code, b"", {}
    except OSError:
        return 0, b"", {}


def wait_live(what, check, timeout_s=420):
    """Poll until check() is true — Pages takes a minute or so after a push."""
    t0 = time.time()
    while time.time() - t0 < timeout_s:
        if check():
            print(f"live: {what} ({int(time.time() - t0)} s)")
            return True
        time.sleep(10)
    print(f"WARN: {what} not live after {timeout_s} s - check the Actions tab of {REPO}")
    return False


def image_version(path):
    with open(path, "rb") as f:
        head = f.read(0x60)
    if len(head) < 0x60 or head[0] != 0xE9 or int.from_bytes(head[0x20:0x24], "little") != APP_DESC_MAGIC:
        sys.exit(f"error: {path} is not an ESP-IDF app image")
    return head[0x30:0x50].split(b"\0")[0].decode("ascii")


def flash_parts(build, dist):
    """Copy what the web flasher writes besides the app (bootloader, partition table, otadata) into
    flash/, with their offsets in flash/parts.json; the Pages workflow adds the app and writes the
    ESP Web Tools manifest. Taken from build/flash_args, so a new partition table follows along."""
    args = os.path.join(build, "flash_args")
    if not os.path.isfile(args):
        print(f"flash: no {args}, web flasher parts left as they are")
        return
    parts, app = [], None
    os.makedirs(os.path.join(dist, "flash"), exist_ok=True)
    shutil.copyfile(os.path.join(ROOT, "server", "appstore", "flash", "index.html"),
                    os.path.join(dist, "flash", "index.html"))
    with open(args, encoding="utf-8") as f:
        for line in f:
            off, _, rel = line.strip().partition(" ")
            if not off.startswith("0x"):
                continue
            if os.path.basename(rel) == BIN_NAME:
                app = int(off, 16)
                continue
            name = os.path.basename(rel)
            shutil.copyfile(os.path.join(build, rel), os.path.join(dist, "flash", name))
            parts.append({"path": name, "offset": int(off, 16)})
    with open(os.path.join(dist, "flash", "parts.json"), "w", encoding="utf-8", newline="\n") as f:
        f.write(json.dumps({"app_offset": app, "parts": sorted(parts, key=lambda p: p["offset"])}) + "\n")


# ---- commands --------------------------------------------------------------------------------

def cmd_store(a):
    checkout(a.dist)
    cmd = [sys.executable, os.path.join(ROOT, "server", "appstore", "export_static.py"), "--out", a.dist]
    for d in a.apps_dir or []:
        cmd += ["--apps-dir", d]
    run(cmd)
    if not commit_push(a.dist, a.message or "store: update the catalog and apps"):
        print("store: nothing changed, nothing published")
        return
    head = run(["git", "rev-parse", "--short", "HEAD"], cwd=a.dist, capture=True).stdout.strip()
    print(f"store: pushed {head}")
    if not a.no_wait:
        with open(os.path.join(a.dist, "store-en.json"), "rb") as f:
            want = f.read()
        wait_live("store-en.json", lambda: fetch(f"{PAGES}/store-en.json")[1] == want)
    print(f"STORE {PAGES}")


def cmd_firmware(a):
    path = os.path.abspath(a.bin or os.path.join(ROOT, "build", BIN_NAME))
    ver = image_version(path)
    if a.version and a.version != ver:
        sys.exit(f"error: {path} is version {ver}, not {a.version}")
    with open(path, "rb") as f:
        data = f.read()
    sha = hashlib.sha256(data).hexdigest()
    notes = (a.notes or f"release {ver}")[:1000]   # the device reads the manifest into 4 KB
    tag = f"v{ver}"
    checkout(a.dist)

    # the image: a release asset always named nucleos-anima.bin, whatever the local file is called
    exists = run(["gh", "release", "view", tag, "--repo", REPO], check=False, capture=True).returncode == 0
    if exists and not a.replace:
        sys.exit(f"error: release {tag} already exists (--replace swaps its image)")
    tmp = tempfile.mkdtemp()
    try:
        asset = os.path.join(tmp, BIN_NAME)
        shutil.copyfile(path, asset)
        if exists:
            run(["gh", "release", "upload", tag, asset, "--clobber", "--repo", REPO])
        else:
            run(["gh", "release", "create", tag, asset, "--repo", REPO,
                 "--title", f"NucleoOS-P4 {ver}", "--notes", notes])
    finally:
        shutil.rmtree(tmp, ignore_errors=True)

    # the manifest the device polls (BOM-free: a BOM breaks the device's JSON parser)
    manifest = {"version": ver, "url": f"{PAGES}/ota/nucleos-anima-{ver}.bin", "notes": notes,
                "size": len(data), "sha256": sha}
    os.makedirs(os.path.join(a.dist, "ota"), exist_ok=True)
    with open(os.path.join(a.dist, "ota", "manifest.json"), "w", encoding="utf-8", newline="\n") as f:
        f.write(json.dumps(manifest, ensure_ascii=False, separators=(",", ":")) + "\n")
    flash_parts(os.path.dirname(path), a.dist)
    commit_push(a.dist, f"firmware {ver}")

    live = "skipped"
    if not a.no_wait:
        def ready():
            st, body, _ = fetch(f"{PAGES}/ota/manifest.json")
            if st != 200 or json.loads(body or b"{}").get("version") != ver:
                return False
            st, _, hdr = fetch(manifest["url"], method="HEAD")
            return st == 200 and int(hdr.get("Content-Length", -1)) == len(data)
        live = "live" if wait_live(f"firmware {ver}", ready) else "NOT LIVE"
    print(f"PUBLISHED {ver} | bin={len(data)} | manifest={PAGES}/ota/manifest.json | pages={live}")


def cmd_status(a):
    st, body, _ = fetch(f"{PAGES}/ota/manifest.json")
    if st == 200:
        m = json.loads(body)
        bst, _, hdr = fetch(m["url"], method="HEAD")
        print(f"firmware {m['version']}  image {bst} {hdr.get('Content-Length', '?')} B  notes: {m.get('notes', '')}")
    else:
        print(f"firmware: no manifest ({st})")
    st, body, _ = fetch(f"{PAGES}/store-en.json")
    if st == 200:
        c = json.loads(body)
        print(f"store    {c['count']} apps, generated {c['generated']}")
    else:
        print(f"store: no catalog ({st})")


def main():
    try:
        sys.stdout.reconfigure(encoding="utf-8")
    except Exception:
        pass
    ap = argparse.ArgumentParser(description="publish to the NucleoOS-P4 distribution repo")
    ap.add_argument("--dist", default=os.environ.get("NUCLEO_DIST", r"D:\nucleoos-p4-store"),
                    help="local checkout of the distribution repo")
    sub = ap.add_subparsers(dest="cmd", required=True)

    s = sub.add_parser("store", help="export the app store, commit, push")
    s.add_argument("-m", "--message", default="")
    s.add_argument("--apps-dir", action="append", help="apps root (repeat); default apps/ + D:\\w4store")
    s.add_argument("--no-wait", action="store_true")
    s.set_defaults(fn=cmd_store)

    f = sub.add_parser("firmware", help="publish a firmware image")
    f.add_argument("--version", default="", help="must match the image (a guard, not a setting)")
    f.add_argument("--bin", default="", help=f"image (default build/{BIN_NAME})")
    f.add_argument("--notes", default="", help="shown on the device update screen")
    f.add_argument("--replace", action="store_true", help="swap the image of an existing release")
    f.add_argument("--no-wait", action="store_true")
    f.set_defaults(fn=cmd_firmware)

    t = sub.add_parser("status", help="what Pages serves now")
    t.set_defaults(fn=cmd_status)

    a = ap.parse_args()
    a.dist = os.path.abspath(a.dist)
    a.fn(a)


if __name__ == "__main__":
    main()
