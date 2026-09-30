#!/usr/bin/env python3
"""Publish to the NucleoOS-P4 distribution repo (GitHub Pages): the app store and the firmware.

    python tools/dist.py store [-m "message"] [--apps-dir DIR ...]
    python tools/dist.py firmware [--version 1.1.106] [--bin build/nucleos-anima.bin] [--notes "..."]
    python tools/dist.py main-release [--bin build/nucleos-anima.bin] [--notes "..."] [--replace]
    python tools/dist.py status

The repo is indecenti/nucleoos-p4-store, checked out at D:\\nucleoos-p4-store (--dist or
NUCLEO_DIST to change it; cloned on first use). Pages serves it at
https://indecenti.github.io/nucleoos-p4-store: the device's default store and OTA URLs from 1.1.106.

store      server/appstore/export_static.py into the checkout (per-language catalogs + app files,
           default sources: the repo's apps/ then D:\\w4store), then commit and push.
firmware   the image goes into a GitHub Release (v<version>, asset nucleos-anima.bin) and
           ota/manifest.json points at the copy the Pages workflow puts in ota/, checked against
           the sha256 written here. The version is read from the image itself.
           Then (unless --no-main-release) the same image as a release of indecenti/NucleoOS-P4:
           factory image for 0x0, separate parts, the SD web pack, SHA256SUMS, install notes;
           tagged on the commit the image was built from, which must already be on origin/main.
           Also writes the web flasher's parts into flash/ (see flash_parts).
main-release  only that second release, e.g. after pushing the "chore: release" commit.
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

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import ota_sign  # noqa: E402  the manifest signature the firmware requires (release key on this PC)

REPO = "indecenti/nucleoos-p4-store"
MAIN_REPO = "indecenti/NucleoOS-P4"
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


def chip_revs(path):
    """(min, max) ESP32-P4 chip revision the image accepts, as "v1.99" strings."""
    with open(path, "rb") as f:
        h = f.read(0x13)
    lo, hi = int.from_bytes(h[15:17], "little"), int.from_bytes(h[17:19], "little")
    return f"v{lo // 100}.{lo % 100}", f"v{hi // 100}.{hi % 100}"


def idf_python():
    """A Python that can run esptool (merge_bin): this one, $IDF_PYTHON_ENV_PATH, or the D:\\esp env."""
    cands = [sys.executable]
    if os.environ.get("IDF_PYTHON_ENV_PATH"):
        cands.append(os.path.join(os.environ["IDF_PYTHON_ENV_PATH"], "Scripts", "python.exe"))
        cands.append(os.path.join(os.environ["IDF_PYTHON_ENV_PATH"], "bin", "python"))
    envs = os.path.join(os.environ.get("IDF_TOOLS_PATH", r"D:\esp\tools"), "python_env")
    if os.path.isdir(envs):
        cands += [os.path.join(envs, d, "Scripts", "python.exe") for d in sorted(os.listdir(envs), reverse=True)]
    for py in cands:
        if os.path.isfile(py) and run([py, "-m", "esptool", "version"], check=False, capture=True).returncode == 0:
            return py
    return None


MAIN_NOTES = """Custom firmware / OS for the **Guition JC1060P470C** (JC1060P470C_I_W): ESP32-P4 + ESP32-C6, 7" 1024×600 MIPI-DSI touchscreen.

## Install

**Easiest: [flash from the browser]({pages}/flash/)** (Chrome or Edge, USB-C cable, nothing to install).

Or with esptool, the single factory image at offset 0 (wipes the board's settings):

```
pip install esptool
esptool.py --chip esp32p4 -b 921600 write_flash 0x0 {factory}
```

Needs an ESP32-P4 chip revision between {rmin} and {rmax}. After the first install the board updates itself over Wi-Fi (OTA) from GitHub Pages; a FAT32 microSD must be inserted, since updates download to the card first.

| File | What |
|---|---|
| `{factory}` | bootloader + partition table + app, write at `0x0` |
{parts_rows}
| `nucleoos-p4-sdcard.zip` | web companion files: unzip to the root of the microSD |
| `nucleos-anima.bin` + `nucleos-anima.json` | offline update: copy both to the root of the microSD, then Settings → Install from SD (the signed `.json` is required) |

## What's new in {ver}
{notes}

Every OTA build: [nucleoos-p4-store releases](https://github.com/{store}/releases).

Free for noncommercial use (PolyForm Noncommercial 1.0.0). Commercial licensing: see COMMERCIAL.md.
"""


def main_release(path, ver, notes, replace=False):
    """The same image as a GitHub Release of the source repo, for people who flash by hand: a factory
    image (merge_bin at 0x0), the separate parts, the web-companion SD pack and SHA256SUMS. Tagged on
    the commit checked out where the image was built, which has to be on origin already."""
    build = os.path.dirname(path)
    proj = os.path.dirname(build)
    if not os.path.isdir(os.path.join(proj, ".git")) and not os.path.isfile(os.path.join(proj, ".git")):
        print(f"main release: {proj} is not a git checkout, skipped")
        return
    git = lambda *c, **k: run(["git", *c], cwd=proj, capture=True, **k)
    commit = git("rev-parse", "HEAD").stdout.strip()
    git("fetch", "--quiet", "origin", "main")
    later = f"python tools/dist.py main-release --bin {path}"
    if git("merge-base", "--is-ancestor", commit, "origin/main", check=False).returncode:
        print(f"main release: {commit[:7]} is not on origin/main yet, skipped - push, then: {later}")
        return
    dirty = {l[3:].strip() for l in git("status", "--porcelain", "--untracked-files=no").stdout.splitlines()}
    if dirty - {"CMakeLists.txt"}:   # the version bump release.ps1 makes is fine, other edits aren't
        print(f"main release: uncommitted changes in {proj} ({', '.join(sorted(dirty)[:4])}), so the "
              f"image isn't {commit[:7]}; skipped - commit and push, then: {later}")
        return
    tag = f"v{ver}"
    exists = run(["gh", "release", "view", tag, "--repo", MAIN_REPO], check=False, capture=True).returncode == 0
    if exists and not replace:
        print(f"main release: {tag} already exists, left as it is (--replace to refresh its files)")
        return

    tmp = tempfile.mkdtemp()
    try:
        assets, rows = [], []
        shutil.copyfile(path, os.path.join(tmp, BIN_NAME))
        merge = []
        with open(os.path.join(build, "flash_args"), encoding="utf-8") as f:
            lines = f.read().split("\n")
        flash_opts = lines[0].split()           # --flash_mode dio --flash_freq 80m --flash_size 16MB
        for line in lines[1:]:
            off, _, rel = line.strip().partition(" ")
            if not off.startswith("0x"):
                continue
            name = os.path.basename(rel)
            if name != BIN_NAME:
                shutil.copyfile(os.path.join(build, rel), os.path.join(tmp, name))
            merge += [off, os.path.join(tmp, name)]
            rows.append(f"| `{name}` | write at `{off}` |")
            assets.append(name)
        factory = f"nucleoos-p4-{ver}-jc1060p470c-factory.bin"
        py = idf_python()
        if py:
            run([py, "-m", "esptool", "--chip", "esp32p4", "merge_bin", "-o", os.path.join(tmp, factory),
                 *flash_opts, *merge], capture=True)
            assets.insert(0, factory)
        else:
            print("main release: no Python with esptool found, factory image left out")
        # the signed manifest "Install from SD" requires beside the image (nv_ota sd_manifest)
        with open(path, "rb") as b:
            data = b.read()
        sd_manifest = {"version": ver, "url": BIN_NAME, "notes": notes[:1000]}
        sd_manifest.update(ota_sign.sign_fields(ver, data))
        if ota_sign.verify(sd_manifest, data):
            sys.exit("error: the SD manifest does not verify against ota_signing_pub.pem")
        sd_json = BIN_NAME[:-4] + ".json"
        with open(os.path.join(tmp, sd_json), "w", encoding="utf-8", newline="\n") as f:
            f.write(json.dumps(sd_manifest, ensure_ascii=False, separators=(",", ":")) + "\n")
        assets.append(sd_json)
        sdzip = "nucleoos-p4-sdcard.zip"
        git("archive", "--format=zip", "-o", os.path.join(tmp, sdzip), f"{commit}:sd", "web")
        assets.append(sdzip)
        with open(os.path.join(tmp, "SHA256SUMS.txt"), "w", encoding="utf-8", newline="\n") as f:
            for n in assets:
                with open(os.path.join(tmp, n), "rb") as b:
                    f.write(f"{hashlib.sha256(b.read()).hexdigest()}  {n}\n")
        assets.append("SHA256SUMS.txt")

        rmin, rmax = chip_revs(path)
        body = MAIN_NOTES.format(pages=PAGES, factory=factory, rmin=rmin, rmax=rmax, ver=ver,
                                 parts_rows="\n".join(rows), notes=notes, store=REPO)
        with open(os.path.join(tmp, "notes.md"), "w", encoding="utf-8") as f:
            f.write(body)
        files = [os.path.join(tmp, n) for n in assets]
        if exists:
            run(["gh", "release", "upload", tag, *files, "--clobber", "--repo", MAIN_REPO])
            run(["gh", "release", "edit", tag, "--notes-file", os.path.join(tmp, "notes.md"), "--repo", MAIN_REPO])
        else:
            run(["gh", "release", "create", tag, *files, "--repo", MAIN_REPO, "--target", commit, "--latest",
                 "--title", f"NucleoOS P4 {ver} — firmware for Guition JC1060P470C (ESP32-P4 7\")",
                 "--notes-file", os.path.join(tmp, "notes.md")])
        print(f"main release: https://github.com/{MAIN_REPO}/releases/tag/{tag} ({len(assets)} files)")
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


def cmd_main_release(a):
    path = os.path.abspath(a.bin or os.path.join(ROOT, "build", BIN_NAME))
    ver = image_version(path)
    if a.version and a.version != ver:
        sys.exit(f"error: {path} is version {ver}, not {a.version}")
    main_release(path, ver, a.notes or f"release {ver}", replace=a.replace)


# ---- commands --------------------------------------------------------------------------------

def sha256_file(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def restore_aot(dist, apps_dirs):
    """app.aot is gitignored, so a clean checkout has none and the export would drop every
    published one (and re-sign the packages without it). Take each missing app.aot back from the
    published store when that app's app.wasm is byte-identical there."""
    restored = 0
    for base in apps_dirs:
        if not os.path.isdir(base):
            continue
        for app_id in sorted(os.listdir(base)):
            src = os.path.join(base, app_id)
            wasm, aot = os.path.join(src, "app.wasm"), os.path.join(src, "app.aot")
            pub = os.path.join(dist, "apps", app_id)
            pub_wasm, pub_aot = os.path.join(pub, "app.wasm"), os.path.join(pub, "app.aot")
            if (os.path.isfile(wasm) and not os.path.isfile(aot) and os.path.isfile(pub_aot)
                    and os.path.isfile(pub_wasm) and sha256_file(wasm) == sha256_file(pub_wasm)):
                shutil.copyfile(pub_aot, aot)
                restored += 1
    if restored:
        print(f"store: {restored} app.aot taken back from the published store (app.wasm unchanged)")


def cmd_store(a):
    checkout(a.dist)
    restore_aot(a.dist, [os.path.join(ROOT, "apps")] + list(a.apps_dir or []))
    cmd =[sys.executable, os.path.join(ROOT, "server", "appstore", "export_static.py"), "--out", a.dist]
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
    # Sign first: without the release key nothing is published (devices would refuse it anyway).
    signed = ota_sign.sign_fields(ver, data)
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
    manifest = {"version": ver, "url": f"{PAGES}/ota/nucleos-anima-{ver}.bin", "notes": notes}
    manifest.update(signed)   # size, sha256, sig
    if ota_sign.verify(manifest, data):
        sys.exit("error: the signed manifest does not verify against ota_signing_pub.pem")
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
    if not a.no_main_release:
        main_release(path, ver, notes, replace=a.replace)


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
    f.add_argument("--no-main-release", action="store_true", help=f"don't also release it on {MAIN_REPO}")
    f.set_defaults(fn=cmd_firmware)

    m = sub.add_parser("main-release", help=f"release an image on {MAIN_REPO} (factory image + parts)")
    m.add_argument("--version", default="", help="must match the image (a guard, not a setting)")
    m.add_argument("--bin", default="", help=f"image (default build/{BIN_NAME}); flash_args must sit next to it")
    m.add_argument("--notes", default="", help="the 'what's new' paragraph")
    m.add_argument("--replace", action="store_true", help="refresh the files and notes of an existing release")
    m.set_defaults(fn=cmd_main_release)

    t = sub.add_parser("status", help="what Pages serves now")
    t.set_defaults(fn=cmd_status)

    a = ap.parse_args()
    a.dist = os.path.abspath(a.dist)
    a.fn(a)


if __name__ == "__main__":
    main()
