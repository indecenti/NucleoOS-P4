#!/usr/bin/env python3
"""Check the split store catalogs without exporting anything (no files written, no signing):

    python server/appstore/test_store2.py [--apps-dir apps --apps-dir D:\\w4store]

For every language: the legacy store-<lang>.json still fits firmware <= 1.1.140 (192 rows, 192 KB)
and carries every native app; store2-<lang>.json + its platform parts hold every app exactly once,
within the new firmware's caps (256 native rows + one 256-row part = NV_STORE_MAX 512, 512 KB a
file); each platform's name index matches its carts in part order; the web page renders."""
import argparse
import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import appstore_server as srv  # noqa: E402

STORE2_CAP = 512 * 1024


def size(doc):
    slim = {**doc, "apps": [{k: v for k, v in a.items() if v is not False} for a in doc.get("apps", [])]}
    return len(json.dumps(slim, ensure_ascii=False, separators=(",", ":")).encode("utf-8"))


def main():
    repo = os.path.normpath(os.path.join(HERE, "..", ".."))
    ap = argparse.ArgumentParser()
    ap.add_argument("--apps-dir", action="append")
    args = ap.parse_args()
    try:
        sys.stdout.reconfigure(encoding="utf-8")
    except Exception:
        pass
    srv.APPS_DIRS = [os.path.abspath(d) for d in (args.apps_dir or [os.path.join(repo, "apps"), r"D:\w4store"])]
    srv.OVERLAY_PATH = os.path.join(HERE, "catalog.json")
    fails = []

    def check(ok, msg):
        if not ok:
            fails.append(msg)

    for lang in srv.LANGS:
        cat = srv.build_catalog(lang, "*", 3, public=True)
        ids = [a["id"] for a in cat["apps"]]
        legacy = srv.legacy_catalog(cat, lang)
        lsz = size(legacy)
        check(legacy["count"] <= srv.LEGACY_ROWS, f"{lang}: legacy has {legacy['count']} rows")
        check(lsz <= srv.LEGACY_BYTES, f"{lang}: legacy is {lsz} bytes")
        check(all("platform" not in a for a in legacy["apps"]), f"{lang}: legacy rows carry platform")
        natives = {a["id"] for a in cat["apps"] if not a.get("platform")}
        check(natives <= {a["id"] for a in legacy["apps"]}, f"{lang}: legacy lost a native app")

        main2, carts = srv.store2_split(cat, lang)
        msz = size(main2)
        check(main2["count"] <= srv.MAIN_ROWS, f"{lang}: store2 main has {main2['count']} rows")
        check(msz <= STORE2_CAP, f"{lang}: store2 main is {msz} bytes")
        check(main2["api"] == srv.STORE2_API and main2["platforms"], f"{lang}: store2 header")
        seen = [a["id"] for a in main2["apps"]]
        parts_info = []
        for p in main2["platforms"]:
            rows = carts[p["id"]]
            check(p["count"] == len(rows), f"{lang}/{p['id']}: count")
            check(p["names"].split("\n") == [a["name"] for a in rows], f"{lang}/{p['id']}: name index")
            check(not p.get("host") or p["host"] in seen, f"{lang}/{p['id']}: host not in main")
            for k in range(1, p["parts"] + 1):
                part = srv.store2_part(p["id"], rows, k)
                psz = size(part)
                check(0 < part["count"] <= srv.PART_ROWS, f"{lang}/{p['id']}-{k}: {part['count']} rows")
                check(psz <= STORE2_CAP, f"{lang}/{p['id']}-{k}: {psz} bytes")
                check(all(a.get("platform") == p["id"] for a in part["apps"]), f"{lang}/{p['id']}-{k}: foreign row")
                seen += [a["id"] for a in part["apps"]]
                parts_info.append(f"{p['id']}-{k}:{part['count']}/{psz // 1024}KB")
        check(sorted(seen) == sorted(ids), f"{lang}: store2 files don't hold every app exactly once")
        page = srv.index_html(cat, static=True)
        check(b"Consoles &amp; engines" in page, f"{lang}: web page lacks the platform sections")
        print(f"{lang}: full {len(ids)} | legacy {legacy['count']} rows {lsz // 1024} KB | store2 main "
              f"{main2['count']} rows {msz // 1024} KB + {' '.join(parts_info)}")

    for f in fails:
        print("FAIL", f)
    print("OK" if not fails else f"{len(fails)} failure(s)")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
