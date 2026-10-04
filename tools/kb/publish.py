#!/usr/bin/env python3
"""Describe ANIMA's knowledge packs for the store: server/appstore/data_packs.json.

    python tools/kb/publish.py [--tag kb-2026.10] [--repo indecenti/nucleoos-p4-store]

For every tools/kb/.cache/wikipedia_<lang>_top.akb6 it writes one store row (kind "data"): names and
descriptions in five languages, the pack's version (the Wikipedia dump's month), its sha256 and size, and
the URL it will have as an asset of the GitHub release <tag>. A file over PART_MAX is listed as parts
(name.001, .002, ... uploaded separately; the device concatenates them). Nothing is uploaded or published:
the commands to do it are printed at the end, for the owner to run.

Then: python server/appstore/export_static.py --out <store checkout> signs data/<id>/pack.sig with the
store key and adds the rows to the catalogs (category "knowledge").
"""
import argparse
import hashlib
import json
import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, "..", ".."))
CACHE = os.path.join(HERE, ".cache")
OUT = os.path.join(ROOT, "server", "appstore", "data_packs.json")
PART_MAX = 1900 * 1024 * 1024          # GitHub release assets: 2 GiB each

LANG_NAME = {
    "it": {"it": "italiano", "en": "Italian", "es": "italiano", "fr": "italien", "de": "Italienisch"},
    "en": {"it": "inglese", "en": "English", "es": "inglés", "fr": "anglais", "de": "Englisch"},
    "es": {"it": "spagnolo", "en": "Spanish", "es": "español", "fr": "espagnol", "de": "Spanisch"},
    "fr": {"it": "francese", "en": "French", "es": "francés", "fr": "français", "de": "Französisch"},
    "de": {"it": "tedesco", "en": "German", "es": "alemán", "fr": "allemand", "de": "Deutsch"},
}
NAME = {"it": "Wikipedia in {l}", "en": "Wikipedia in {l}", "es": "Wikipedia en {l}", "fr": "Wikipédia en {l}",
        "de": "Wikipedia auf {l}"}
DESC = {
    "it": "Le {n} voci più importanti di Wikipedia in {l}, offline: ANIMA risponde a \"chi è\" e \"cos'è\" senza rete, "
          "citando la fonte. {mb} MB sulla SD.",
    "en": "The {n} most important Wikipedia articles in {l}, offline: ANIMA answers \"who is\" and \"what is\" with no "
          "network, citing the source. {mb} MB on the SD.",
    "es": "Los {n} artículos más importantes de Wikipedia en {l}, sin conexión: ANIMA responde «quién es» y «qué es» "
          "sin red, citando la fuente. {mb} MB en la SD.",
    "fr": "Les {n} articles les plus importants de Wikipédia en {l}, hors ligne : ANIMA répond à « qui est » et "
          "« qu'est-ce que » sans réseau, en citant la source. {mb} Mo sur la carte SD.",
    "de": "Die {n} wichtigsten Wikipedia-Artikel auf {l}, offline: ANIMA beantwortet „wer ist“ und „was ist“ ohne "
          "Netz und nennt die Quelle. {mb} MB auf der SD-Karte.",
}


def pack_meta(path):
    """META of an AKB6 file (tools/kb/akb6.py): source date, entities, attribution."""
    with open(path, "rb") as f:
        h = f.read(64)
        if h[:4] != b"AKB6":
            sys.exit(f"{path}: not an AKB6 pack")
        nsec = struct.unpack("<I", h[24:28])[0]
        for _ in range(nsec):
            d = f.read(20)
            if d[:4] == b"META":
                off, sz = struct.unpack("<QQ", d[4:])
                f.seek(off)
                return json.loads(f.read(sz))
    return {}


def sha256_file(path, start=0, size=None):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        f.seek(start)
        left = size if size is not None else os.path.getsize(path) - start
        while left > 0:
            b = f.read(min(1 << 20, left))
            if not b:
                break
            h.update(b); left -= len(b)
    return h.hexdigest()


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--tag", default="")
    ap.add_argument("--repo", default="indecenti/nucleoos-p4-store")
    a = ap.parse_args()
    import datetime
    tag = a.tag or "kb-" + datetime.date.today().strftime("%Y.%m")   # one release holds every pack
    packs, uploads = [], []
    for lang in ("it", "en", "es", "fr", "de"):
        path = os.path.join(CACHE, f"wikipedia_{lang}_top.akb6")
        if not os.path.exists(path):
            print(f"  (no {os.path.basename(path)}: skipped)")
            continue
        meta = pack_meta(path)
        y, m = (meta.get("source_date", "2026-01") + "-01").split("-")[:2]
        version = f"{int(y)}.{int(m)}"
        size = os.path.getsize(path)
        fname = os.path.basename(path)
        files = []
        if size <= PART_MAX:
            files.append({"name": fname, "size": size, "sha256": sha256_file(path),
                          "url": f"https://github.com/{a.repo}/releases/download/{tag}/{fname}"})
            uploads.append(path)
        else:                                                     # parts: name.001, .002, ... (same name)
            k, off = 1, 0
            while off < size:
                n = min(PART_MAX, size - off)
                pname = f"{fname}.{k:03d}"
                files.append({"name": fname, "size": n, "sha256": sha256_file(path, off, n),
                              "url": f"https://github.com/{a.repo}/releases/download/{tag}/{pname}"})
                uploads.append(f"{path} [bytes {off}..{off + n}] as {pname}")
                off += n; k += 1
        mb = (size + (1 << 19)) >> 20
        ents = meta.get("entities", 50000)
        n_txt = f"{ents // 1000 * 1000:,}".replace(",", ".")
        packs.append({
            "id": f"wiki-{lang}-top",
            "version": version,
            "dest": "anima/kb",
            "lang": lang,
            "category": "knowledge",
            "author": "Wikipedia · Kiwix/openZIM · Wikidata",
            "license": "CC BY-SA 4.0",
            "source": f"https://{lang}.wikipedia.org",
            "names": {ul: NAME[ul].format(l=LANG_NAME[lang][ul]) for ul in NAME},
            "descriptions": {ul: DESC[ul].format(n=n_txt, l=LANG_NAME[lang][ul], mb=mb) for ul in DESC},
            "featured": lang in ("it", "en"),
            "files": files,
            "tag": tag,
        })
        print(f"  wiki-{lang}-top  v{version}  {mb} MB  {len(files)} file(s)  sha256 {files[0]['sha256'][:12]}...")
    with open(OUT, "w", encoding="utf-8", newline="\n") as f:
        json.dump({"_comment": "Written by tools/kb/publish.py. Store rows kind=data (ANIMA knowledge packs).",
                   "packs": packs}, f, ensure_ascii=False, indent=2)
        f.write("\n")
    print(f"-> {os.path.relpath(OUT, ROOT)}  ({len(packs)} packs)")
    if packs:
        tag = packs[0]["tag"]
        print("\nTo publish (run them yourself when ready):")
        print(f"  gh release create {tag} --repo {a.repo} --title \"ANIMA knowledge {tag}\" \\")
        print(f"     --notes \"Wikipedia (CC BY-SA 4.0) via Kiwix/openZIM, Wikidata (CC0): ANIMA offline knowledge packs.\" \\")
        print("     " + " ".join(os.path.relpath(u, ROOT) for u in uploads if "[" not in u))
        print("  python server/appstore/export_static.py --out <checkout of the store repo>   # signs data/<id>/pack.sig")


if __name__ == "__main__":
    main()
