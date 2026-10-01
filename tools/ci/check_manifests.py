#!/usr/bin/env python3
"""Check every apps/<id>/manifest.json before it reaches the store:

    python tools/ci/check_manifests.py [apps]

Fails (exit 1) on anything that would publish a broken or unlabeled package: invalid JSON, an id
that does not match its folder, a missing name/version/author/license, a version the device cannot
compare (nv_appstore version_is_newer: up to four numeric fields), unknown fields of the wrong type,
unknown permissions, or a description with no English text."""
import json
import os
import re
import sys

VERSION = re.compile(r"^\d+(\.\d+){1,3}$")
ID = re.compile(r"^[a-z0-9][a-z0-9-]{0,47}$")
PERMISSIONS = {"fs", "gfx", "log", "net", "home", "ui", "mqtt", "lan", "ha", "ws"}
REQUIRED = ("id", "name", "version", "author", "license")
TYPES = {
    "id": str, "name": str, "version": str, "author": str, "license": str, "source": str,
    "description": str, "descriptions": dict, "category": str, "entry": str, "engine": str,
    "kind": str, "canvas_scale": str, "abi": int, "ram_budget": int, "stack_kb": int,
    "timeout_ms": int, "canvas_w": int, "canvas_h": int, "permissions": list, "args": list,
    "variants": list, "requires": dict, "system_gestures": bool, "console": bool, "wasm4": bool,
}


def check(app_dir):
    path = os.path.join(app_dir, "manifest.json")
    try:
        with open(path, encoding="utf-8") as f:
            man = json.load(f)
    except (OSError, ValueError) as e:
        return [f"unreadable: {e}"]
    if not isinstance(man, dict):
        return ["not a JSON object"]
    errs = []
    for k in REQUIRED:
        if not isinstance(man.get(k), str) or not man[k].strip():
            errs.append(f"missing {k}")
    for k, t in TYPES.items():
        if k in man and not (isinstance(man[k], t) and not (t is int and isinstance(man[k], bool))):
            errs.append(f"{k} must be {t.__name__}")
    folder = os.path.basename(os.path.normpath(app_dir))
    if man.get("id") != folder:
        errs.append(f"id {man.get('id')!r} does not match folder {folder!r}")
    elif not ID.match(folder):
        errs.append("id must be lowercase letters, digits and '-'")
    if isinstance(man.get("version"), str) and not VERSION.match(man["version"]):
        errs.append(f"version {man['version']!r} is not 2-4 numeric fields")
    perms = man.get("permissions", [])
    if isinstance(perms, list):
        bad = [p for p in perms if p not in PERMISSIONS]
        if bad:
            errs.append(f"unknown permissions {bad}")
    descs = man.get("descriptions")
    if isinstance(descs, dict) and not str(descs.get("en", "")).strip():
        errs.append("descriptions has no 'en' text")
    if "description" in man and not isinstance(descs, dict):
        errs.append("description without descriptions.en")
    return errs


def main():
    root = sys.argv[1] if len(sys.argv) > 1 else "apps"
    dirs = sorted(d for d in (os.path.join(root, n) for n in os.listdir(root))
                  if os.path.isfile(os.path.join(d, "manifest.json")))
    failed = 0
    for d in dirs:
        for e in check(d):
            failed += 1
            print(f"::error file={d}/manifest.json::{e}")
    print(f"{len(dirs)} manifests, {failed} problems")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
