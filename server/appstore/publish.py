#!/usr/bin/env python3
"""Copy a built NucleoV2 app into a store folder so appstore_server.py can serve it.

    python publish.py ../../apps/pianino --store ./store-apps

Copies manifest.json + app.wasm (+ icon.argb if present) into <store>/<id>/. The id comes from the
manifest (falling back to the source directory name). Overwrites an existing app of the same id.
"""
import argparse
import json
import os
import re
import shutil
import sys

ID_RE = re.compile(r"^[A-Za-z0-9_-]{1,31}$")
FILES = ["manifest.json", "app.wasm", "icon.argb"]   # icon is optional


def main():
    ap = argparse.ArgumentParser(description="publish an app into a store folder")
    ap.add_argument("app_dir", help="source app dir (contains manifest.json + app.wasm)")
    ap.add_argument("--store", default="./store-apps", help="destination store folder")
    args = ap.parse_args()

    src = os.path.abspath(args.app_dir)
    man_path = os.path.join(src, "manifest.json")
    if not os.path.isfile(man_path) or not os.path.isfile(os.path.join(src, "app.wasm")):
        sys.exit(f"error: {src} has no manifest.json + app.wasm")

    with open(man_path, "r", encoding="utf-8") as f:
        man = json.load(f)
    app_id = str(man.get("id") or os.path.basename(src.rstrip("/\\")))
    if not ID_RE.match(app_id):
        sys.exit(f"error: invalid app id '{app_id}'")

    dst = os.path.join(os.path.abspath(args.store), app_id)
    os.makedirs(dst, exist_ok=True)
    for name in FILES:
        s = os.path.join(src, name)
        if os.path.isfile(s):
            shutil.copy2(s, os.path.join(dst, name))
            print(f"  {name}")
    print(f"published '{app_id}' -> {dst}")


if __name__ == "__main__":
    main()
