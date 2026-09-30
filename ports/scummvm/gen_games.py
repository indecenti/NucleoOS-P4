"""gen_games.py — from games.json: the engine's download table and the store packages.

    python ports/scummvm/gen_games.py

Writes
  ports/scummvm/backend/nucleo-games.h   every file the installer may fetch (URL, size, sha256) and,
                                         per game and language variant, which files and --language
  apps/<id>/manifest.json + icon.z       one store package per game ("engine": "scummvm", no module)

Sizes come from a HEAD request, hashes from the .sha256 file downloads.scummvm.org keeps next to
every archive; the engine data files (raw.githubusercontent.com, ScummVM v2.9.1 tag) are downloaded
and hashed here. Results are cached in ports/_src/scummvm-files.json (delete it to refresh).
"""
import hashlib
import json
import os
import subprocess
import sys
import urllib.parse
import urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
CACHE = os.path.join(ROOT, "ports", "_src", "scummvm-files.json")
ENGINE_VERSION = "1.0.0"
MAX_URL = 160


def http(url, method="GET"):
    req = urllib.request.Request(url, method=method, headers={"User-Agent": "nucleoos-gen/1"})
    return urllib.request.urlopen(req, timeout=60)


def quote(url):
    p = urllib.parse.urlsplit(url)
    return urllib.parse.urlunsplit((p.scheme, p.netloc, urllib.parse.quote(p.path), p.query, ""))


def file_info(url, cache):
    if url in cache:
        return cache[url]
    with http(url, "HEAD") as r:
        size = int(r.headers["Content-Length"])
        # No redirects allowed on the device (nv_http_req never follows them).
        assert r.geturl() == url, f"{url} redirects to {r.geturl()}"
    if "downloads.scummvm.org" in url:
        with http(url + ".sha256") as r:
            sha = r.read().decode().split()[0].lower()
    else:
        with http(url) as r:
            sha = hashlib.sha256(r.read()).hexdigest()
    assert len(sha) == 64
    cache[url] = {"size": size, "sha256": sha}
    print(f"  {size:>10}  {url}")
    return cache[url]


GUIDE_TEXT = {
    "it": ("## A cosa serve", "## Come si usa",
           "Nella pagina dello Store scegli la lingua, poi installa. Al primo avvio il gioco scarica i suoi "
           "file originali dal sito di ScummVM ({size}, serve il Wi-Fi), li controlla e parte; le volte "
           "successive parte subito. Il download si può interrompere con il gesto Indietro: riprende da dove "
           "era rimasto. Gira con il motore ScummVM, che lo Store installa insieme al gioco.",
           "Lingue: {langs}.", "Licenza: {license}. I file sono quelli originali, non modificati."),
    "en": ("## What it is for", "## How to use it",
           "Pick the language on the Store page, then install. On first start the game downloads its original "
           "files from the ScummVM website ({size}, Wi-Fi needed), checks them and starts; after that it starts "
           "straight away. The Back gesture stops the download: it resumes where it stopped. It runs on the "
           "ScummVM engine, which the Store installs together with the game.",
           "Languages: {langs}.", "Licence: {license}. The files are the original ones, unmodified."),
}


def write_guides(g, d):
    """GUIDE.md (it) and GUIDE.en.md for a game package: its own intro plus the ScummVM controls,
    saves and troubleshooting sections of apps/scummvm/GUIDE*.md."""
    for lang, fname in (("it", "GUIDE.md"), ("en", "GUIDE.en.md")):
        src = open(os.path.join(ROOT, "apps", "scummvm", fname), encoding="utf-8").read()
        common = src[src.index("## " + ("Comandi" if lang == "it" else "Controls")):]
        what, how, body, langs, lic = GUIDE_TEXT[lang]
        sizes = sorted(v["size"] for v in g["vs"])
        size = (f"{sizes[0] / 1e6:.0f} MB" if sizes[0] == sizes[-1]
                else f"{sizes[0] / 1e6:.0f}-{sizes[-1] / 1e6:.0f} MB")
        desc = g.get("descriptions", {}).get(lang, g["description"]) if lang != "en" else g["description"]
        text = (f"# {g['name']}\n\n{what}\n\n{desc} ({g['author']}, {g['year']}.)\n\n{how}\n\n"
                f"{body.format(size=size)}\n\n{langs.format(langs=', '.join(v['name'] for v in g['vs']))}\n\n"
                f"{lic.format(license=g['license'])}\n\n{common}")
        with open(os.path.join(d, fname), "w", encoding="utf-8", newline="\n") as f:
            f.write(text)


def main():
    spec = json.load(open(os.path.join(HERE, "games.json"), encoding="utf-8"))
    cache = json.load(open(CACHE)) if os.path.exists(CACHE) else {}
    files, index = [], {}

    def fid(url):
        url = quote(url)
        assert len(url) < MAX_URL, url
        if url not in index:
            info = file_info(url, cache)
            index[url] = len(files)
            files.append((url, info["size"], info["sha256"]))
        return index[url]

    games = []
    for g in spec["games"]:
        extra = [fid(spec["engine_data"] + n) for n in g.get("engine_data", [])]
        vs = []
        for v in g["variants"]:
            ids = [fid(spec["base"] + f) for f in v["files"]] + extra
            vs.append({**v, "fids": ids, "size": sum(files[i][1] for i in ids)})
        games.append({**g, "vs": vs})
    json.dump(cache, open(CACHE, "w"), indent=1)

    # ---- engine table ----------------------------------------------------------------------------
    out = ["// Generated by ports/scummvm/gen_games.py from games.json — do not edit.",
           "#ifndef BACKENDS_PLATFORM_NUCLEO_GAMES_H", "#define BACKENDS_PLATFORM_NUCLEO_GAMES_H", "",
           "struct NucleoFile { const char *url; unsigned size; const char *sha256; };",
           "struct NucleoVariant { const char *id; const char *lang; int files[6]; };   // -1 ends",
           "struct NucleoGame { const char *key; const char *gameid; const char *name;",
           "                    int nvariants; const NucleoVariant *variants; };", "",
           "static const NucleoFile kNucleoFiles[] = {"]
    for url, size, sha in files:
        out.append(f'\t{{ "{url}", {size}u, "{sha}" }},')
    out.append("};")
    for g in games:
        out.append(f"static const NucleoVariant kVariants_{g['key']}[] = {{")
        for v in g["vs"]:
            ids = ", ".join(str(i) for i in v["fids"] + [-1] * (6 - len(v["fids"])))
            out.append(f'\t{{ "{v["id"]}", "{v["lang"]}", {{ {ids} }} }},')
        out.append("};")
    out.append("static const NucleoGame kNucleoGames[] = {")
    for g in games:
        out.append(f'\t{{ "{g["key"]}", "{g["gameid"]}", "{g["name"]}", {len(g["vs"])}, kVariants_{g["key"]} }},')
    out += ["};", "", "#endif", ""]
    with open(os.path.join(HERE, "backend", "nucleo-games.h"), "w", newline="\n") as f:
        f.write("\n".join(out))

    # ---- store packages --------------------------------------------------------------------------
    for g in games:
        d = os.path.join(ROOT, "apps", g["id"])
        os.makedirs(d, exist_ok=True)
        m = {
            "id": g["id"],
            "name": g["name"],
            "version": ENGINE_VERSION,
            "engine": "scummvm",
            "args": [f"--nucleo-game={g['key']}"],
            "requires": {"scummvm": ENGINE_VERSION},
            "abi": 14,
            "ram_budget": g.get("ram_mb", 12) * 1024 * 1024,
            "stack_kb": 64,
            "timeout_ms": 120000,
            "permissions": ["gfx", "net", "fs", "log"],
            "canvas_w": 320,
            "canvas_h": 200,
            "canvas_scale": "fit",
            "category": "games",
            "author": f"{g['author']} ({g['year']}); ScummVM",
            "license": g["license"],
            "source": "https://www.scummvm.org/games/",
            "description": g["description"],
            "descriptions": {"en": g["description"], **g.get("descriptions", {})},
            "variants": [{"id": v["id"], "name": v["name"], "lang": v["ui"], "size": v["size"]}
                         for v in g["vs"]],
        }
        with open(os.path.join(d, "manifest.json"), "w", encoding="utf-8", newline="\n") as f:
            json.dump(m, f, indent=2, ensure_ascii=False)
            f.write("\n")
        write_guides(g, d)
        subprocess.check_call([sys.executable, os.path.join(HERE, "make_icon.py"),
                               os.path.join(d, "icon.z"), "--badge", g["badge"]])
        print(f"{g['id']}: {len(g['vs'])} variants, " +
              ", ".join(f"{v['id']} {v['size'] / 1e6:.0f} MB" for v in g["vs"]))
    print(f"{len(files)} files, nucleo-games.h written")


if __name__ == "__main__":
    main()
