"""copy_live_catalog.py — a copy of the store's published books catalog (index, one language's first
shard, the covers of its first rows) laid out like make_catalog.py's, for the store's screenshots.

    python ports/books/test/copy_live_catalog.py OUT [LANG] [COVERS]
"""
import json
import os
import sys
import urllib.request

BASE = "https://indecenti.github.io/nucleoos-p4-store/books/"
PAGES = "nucleoos-p4-store/books"


def get(url):
    with urllib.request.urlopen(url, timeout=60) as r:
        return r.read()


def main():
    out = sys.argv[1]
    lang = sys.argv[2] if len(sys.argv) > 2 else "it"
    ncov = int(sys.argv[3]) if len(sys.argv) > 3 else 40
    root = os.path.join(out, "http", PAGES)
    os.makedirs(os.path.join(root, lang), exist_ok=True)
    os.makedirs(os.path.join(root, "covers"), exist_ok=True)
    index = get(BASE + "index-v1.json")
    open(os.path.join(root, "index-v1.json"), "wb").write(index)
    shard = get(BASE + "%s/1.json" % lang)
    open(os.path.join(root, lang, "1.json"), "wb").write(shard)
    rows = json.loads(shard)["books"]
    n = 0
    for r in rows:
        if n >= ncov:
            break
        if r.get("cv"):
            open(os.path.join(root, "covers", r["id"] + ".jpg"), "wb").write(get(BASE + "covers/%s.jpg" % r["id"]))
            n += 1
    print("catalog copy: %d rows, %d covers in %s" % (len(rows), n, root))


if __name__ == "__main__":
    main()
