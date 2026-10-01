#!/usr/bin/env python3
"""lua_pack.py - package a Lua app for the NucleoOS Store (engine "luaapp", docs/LUA_APPS.md).

A Lua app lives in apps/<id>/: manifest.json (hand-written, "engine": "luaapp"), src/ (main.lua,
modules, images, sounds), icon.z, GUIDE*.md. This tool turns src/ into what the store serves:

  apps/<id>/app.lpk      the bundle: every .lua/.json/.txt/... file of src/ plus the images
                         converted to the engine's LIMG format (RGB565 + alpha), paths kept
  apps/<id>/snd/*.wav    sounds (.ogg/.wav/.mp3/.flac in src/) as 48 kHz mono WAV store assets,
                         named like love.audio.newSource expects ("sfx/pop.ogg" -> snd/sfx_pop.wav)
  manifest "args"        [sha256 of app.lpk] - the store signature covers the manifest, so the
                         engine trusts a bundle only with exactly that hash

  python tools/lua_pack.py pack apps/<id>              build app.lpk + sounds, update the manifest
  python tools/lua_pack.py pack-all                    every apps/*/ whose manifest has engine luaapp
  python tools/lua_pack.py new <id> "Name"             a new app from the template (apps/<id>/)
  python tools/lua_pack.py run apps/<id> [frames] [script]   PC run under WAMR (WSL), PNG out
  python tools/lua_pack.py push apps/<id>              sideload src/ to the board's
                                                       /sdcard/home/lua/<id>/ (Lua App tile)

Signing is the store's usual step (server/appstore/export_static.py signs every exported
package, app.lpk included).
"""
import hashlib
import json
import os
import re
import shutil
import struct
import subprocess
import sys

from PIL import Image

ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
IMG_EXT = {".png", ".jpg", ".jpeg", ".bmp", ".gif"}
SND_EXT = {".ogg", ".wav", ".mp3", ".flac"}
SKIP_EXT = {".ttf", ".otf", ".mid", ".md", ".love", ".exe", ".dll"}
MAX_BUNDLE = 1024 * 1024
TEST = os.path.join(ROOT, "ports", "_src", "luaapp", "test")


def limg(path):
    im = Image.open(path).convert("RGBA")
    w, h = im.size
    if w > 2048 or h > 2048:
        sys.exit(f"{path}: {w}x{h} is too big (2048 max)")
    px = im.tobytes()
    rgb = bytearray(w * h * 2)
    alpha = bytearray(w * h)
    has_alpha = False
    for i in range(w * h):
        r, g, b, a = px[4 * i:4 * i + 4]
        v = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)
        rgb[2 * i] = v & 255
        rgb[2 * i + 1] = v >> 8
        alpha[i] = a
        if a != 255:
            has_alpha = True
    out = b"LIMG" + struct.pack("<HHB3x", w, h, 1 if has_alpha else 0) + bytes(rgb)
    return out + (bytes(alpha) if has_alpha else b"")


def sound_name(rel):
    """Same rule as love._sound_name in ports/luaapp/lib/love.lua."""
    base = re.sub(r"\.\w+$", "", rel)
    return re.sub(r"[^0-9A-Za-z]", "_", base).lower()[-24:]


def to_wav(src, dst):
    subprocess.run(["ffmpeg", "-v", "error", "-y", "-i", src, "-ac", "1", "-ar", "48000",
                    "-sample_fmt", "s16", "-map_metadata", "-1", "-fflags", "+bitexact", dst],
                   check=True)


def build_bundle(src):
    files = []
    for dp, dn, fn in os.walk(src):
        dn[:] = sorted(d for d in dn if not d.startswith("."))
        for n in sorted(fn):
            if n.startswith("."):
                continue
            ap = os.path.join(dp, n)
            rel = os.path.relpath(ap, src).replace(os.sep, "/")
            ext = os.path.splitext(n)[1].lower()
            if ext in SND_EXT or ext in SKIP_EXT:
                continue
            data = limg(ap) if ext in IMG_EXT else open(ap, "rb").read()
            if len(rel.encode()) > 90:
                sys.exit(f"{rel}: path too long for the bundle (90 bytes max)")
            files.append((rel, data))
    out = bytearray(b"LPK1" + struct.pack("<I", len(files)))
    for rel, data in files:
        nb = rel.encode()
        out += struct.pack("<H", len(nb)) + nb + struct.pack("<I", len(data)) + data
    return bytes(out), [r for r, _ in files]


def pack(app_dir, quiet=False):
    app_dir = os.path.normpath(app_dir)
    src = os.path.join(app_dir, "src")
    man_path = os.path.join(app_dir, "manifest.json")
    man = json.load(open(man_path, encoding="utf-8"))
    if man.get("engine") != "luaapp":
        sys.exit(f"{man_path}: \"engine\" must be \"luaapp\"")
    if not os.path.isfile(os.path.join(src, "main.lua")):
        sys.exit(f"{src}/main.lua is missing")
    bundle, names = build_bundle(src)
    if len(bundle) > MAX_BUNDLE:
        sys.exit(f"{app_dir}: bundle {len(bundle)} bytes > {MAX_BUNDLE} (the device's download cap)")
    with open(os.path.join(app_dir, "app.lpk"), "wb") as f:
        f.write(bundle)
    sha = hashlib.sha256(bundle).hexdigest()
    # sounds -> snd/*.wav store assets
    snd_dir = os.path.join(app_dir, "snd")
    wanted = set()
    for dp, _, fn in os.walk(src):
        for n in sorted(fn):
            if os.path.splitext(n)[1].lower() in SND_EXT:
                rel = os.path.relpath(os.path.join(dp, n), src).replace(os.sep, "/")
                name = sound_name(rel)
                os.makedirs(snd_dir, exist_ok=True)
                dst = os.path.join(snd_dir, name + ".wav")
                if not os.path.isfile(dst) or os.path.getmtime(dst) < os.path.getmtime(os.path.join(dp, n)):
                    to_wav(os.path.join(dp, n), dst)
                wanted.add(name + ".wav")
    if os.path.isdir(snd_dir):
        for n in os.listdir(snd_dir):
            if n not in wanted:
                os.remove(os.path.join(snd_dir, n))
        if not os.listdir(snd_dir):
            os.rmdir(snd_dir)
    # manifest: args = [sha256], requires luaapp; rewrite only when it changed
    changed = False
    if man.get("args") != [sha]:
        man["args"] = [sha]
        changed = True
    req = man.setdefault("requires", {})
    for k, v in (("luaapp", "1.0"), ("wasi", "1.1")):
        if k not in req:
            req[k] = v
            changed = True
    if changed:
        with open(man_path, "w", encoding="utf-8", newline="\n") as f:
            f.write(json.dumps(man, indent=2, ensure_ascii=False) + "\n")
    if not quiet:
        print(f"{app_dir}: app.lpk {len(bundle)} bytes, {len(names)} files, "
              f"{len(wanted)} sounds, sha256 {sha[:16]}...{' (manifest updated)' if changed else ''}")
    return sha


def lua_apps():
    out = []
    for d in sorted(os.listdir(os.path.join(ROOT, "apps"))):
        mp = os.path.join(ROOT, "apps", d, "manifest.json")
        if os.path.isfile(mp):
            try:
                if json.load(open(mp, encoding="utf-8")).get("engine") == "luaapp":
                    out.append(os.path.join(ROOT, "apps", d))
            except ValueError:
                pass
    return out


def wsl_path(p):
    p = os.path.abspath(p).replace("\\", "/")
    return "/mnt/" + p[0].lower() + p[2:]


def run(app_dir, frames=240, script="", host=None, lang="en", quiet=False):
    """Run the packed app under WAMR on the PC (ports/luaapp/build.sh test builds the host)."""
    app_dir = os.path.normpath(app_dir)
    man = json.load(open(os.path.join(app_dir, "manifest.json"), encoding="utf-8"))
    sha = pack(app_dir, quiet=True)
    aid = man["id"]
    fs = os.path.join(TEST, "fs_" + aid)
    shutil.rmtree(fs, ignore_errors=True)
    os.makedirs(os.path.join(fs, "engine"))
    shutil.copyfile(os.path.join(app_dir, "app.lpk"), os.path.join(fs, ".app.lpk"))
    net = os.path.join(app_dir, "test")          # optional canned answers: test/net/*, test/mqtt.txt
    if os.path.isdir(net):
        shutil.copytree(net, fs, dirs_exist_ok=True)
    host = host or os.path.join(TEST, "luahost")
    if not host.startswith("/"):
        host = wsl_path(host)
    w, h = man.get("canvas_w", 1024), man.get("canvas_h", 600)
    out = os.path.join(TEST, aid + ".ppm")
    cmd = ["wsl.exe", "-d", "Ubuntu-24.04", "--", "env", "LUAHOST_ARGS=" + sha, "LUAHOST_LANG=" + lang,
           host, wsl_path(os.path.join(ROOT, "apps", "luaapp", "app.wasm")), aid, wsl_path(fs),
           str(frames), wsl_path(out), script or "", str(w), str(h)]
    r = subprocess.run(cmd, capture_output=True, text=True, env=dict(os.environ, MSYS_NO_PATHCONV="1"))
    log = (r.stdout + r.stderr).strip()
    png = out[:-4] + ".png"
    if os.path.isfile(out):
        Image.open(out).save(png)
        for n in os.listdir(TEST):
            if n.startswith(aid + ".ppm.") and n.endswith(".ppm"):
                Image.open(os.path.join(TEST, n)).save(os.path.join(TEST, n[:-4] + ".png"))
    bad = r.returncode != 0 or "error" in log.lower() and "[log3]" in log
    if not quiet or bad:
        print(f"== {aid}: {'FAIL' if bad else 'ok'}  -> {png}")
        print("   " + "\n   ".join(log.splitlines()[-12:]))
    return not bad


def new_app(aid, name):
    if not re.match(r"^[a-z0-9_-]{1,31}$", aid):
        sys.exit("id: lowercase letters, digits, - and _ (31 max)")
    d = os.path.join(ROOT, "apps", aid)
    if os.path.exists(d):
        sys.exit(f"{d} already exists")
    tpl = os.path.join(ROOT, "sdk", "lua", "template")
    shutil.copytree(tpl, d)
    mp = os.path.join(d, "manifest.json")
    m = json.load(open(mp, encoding="utf-8"))
    m["id"], m["name"] = aid, name
    with open(mp, "w", encoding="utf-8", newline="\n") as f:
        f.write(json.dumps(m, indent=2, ensure_ascii=False) + "\n")
    pack(d)
    print(f"{d}: edit src/main.lua, the manifest, GUIDE.md / GUIDE.en.md; icon: "
          f"python tools/make_mdi_icon.py apps/{aid} <mdi-glyph> \"#rrggbb\"")


def push(app_dir):
    """Sideload src/ to /sdcard/home/lua/<id>/ through the board's web API (tools/nsh.py's token)."""
    sys.path.insert(0, os.path.join(ROOT, "tools"))
    import urllib.request
    import nvtoken
    man = json.load(open(os.path.join(app_dir, "manifest.json"), encoding="utf-8"))
    host = os.environ.get("NUCLEO_HOST", "nucleo.local")
    tok = nvtoken.token()
    src = os.path.join(app_dir, "src")
    for dp, _, fn in os.walk(src):
        for n in fn:
            ap = os.path.join(dp, n)
            rel = os.path.relpath(ap, src).replace(os.sep, "/")
            ext = os.path.splitext(n)[1].lower()
            data = limg(ap) if ext in IMG_EXT else open(ap, "rb").read()
            dest = f"/home/lua/{man['id']}/{rel}"
            req = urllib.request.Request(f"http://{host}/api/fs/write?path={dest}", data=data, method="POST",
                                         headers={"Authorization": "Bearer " + tok})
            urllib.request.urlopen(req, timeout=20).read()
            print("  ->", dest)


def main():
    a = sys.argv[1:]
    if not a:
        sys.exit(__doc__)
    if a[0] == "pack" and len(a) == 2:
        pack(a[1])
    elif a[0] == "pack-all":
        for d in lua_apps():
            pack(d)
    elif a[0] == "new" and len(a) == 3:
        new_app(a[1], a[2])
    elif a[0] == "run" and len(a) >= 2:
        ok = run(a[1], int(a[2]) if len(a) > 2 else 240, a[3] if len(a) > 3 else "")
        sys.exit(0 if ok else 1)
    elif a[0] == "test-all":
        os.makedirs(TEST, exist_ok=True)
        host = a[2] if len(a) > 2 else None
        okn = 0
        apps = lua_apps()
        for d in apps:
            cfg = {}
            tp = os.path.join(d, "test", "test.json")
            if os.path.isfile(tp):
                cfg = json.load(open(tp, encoding="utf-8"))
            okn += run(d, cfg.get("frames", 240), cfg.get("script", ""), host, cfg.get("lang", "en"))
        print(f"{okn}/{len(apps)} Lua apps ran clean")
        sys.exit(0 if okn == len(apps) else 1)
    elif a[0] == "push" and len(a) == 2:
        push(a[1])
    else:
        sys.exit(__doc__)


if __name__ == "__main__":
    main()
