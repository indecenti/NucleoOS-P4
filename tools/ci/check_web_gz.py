#!/usr/bin/env python3
"""Keep the precompressed web files in step with their sources.

nv_web serves `<file>.gz` in preference to `<file>` (components/nv_web/nv_web.cpp, PSRAM asset
cache), so a source edited without regenerating its twin ships the OLD code to the device. This
checks every `sd/web/**/*.gz` against the file next to it and, with --fix, rewrites the stale ones.

    python tools/ci/check_web_gz.py            # CI: exit 1 when a twin is stale or orphaned
    python tools/ci/check_web_gz.py --fix      # regenerate the stale twins

Text sources are compared with CRLF folded to LF (a Windows checkout has CRLF in the work tree,
the repository and the device have LF). Output is reproducible: gzip -9, no name, mtime 0.
"""
import gzip
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
WEB = os.path.join(ROOT, "sd", "web")
TEXT = {".html", ".js", ".mjs", ".css", ".json", ".svg", ".webmanifest", ".txt", ".md"}


def source_bytes(path):
    data = open(path, "rb").read()
    if os.path.splitext(path)[1].lower() in TEXT:
        data = data.replace(b"\r\n", b"\n")
    return data


def compress(data):
    return gzip.compress(data, compresslevel=9, mtime=0)


def main():
    fix = "--fix" in sys.argv
    stale, orphans = [], []
    for dirpath, _, files in os.walk(WEB):
        for name in files:
            if not name.endswith(".gz"):
                continue
            gz = os.path.join(dirpath, name)
            src = gz[:-3]
            rel = os.path.relpath(gz, ROOT).replace(os.sep, "/")
            if not os.path.isfile(src):
                orphans.append(rel)
                continue
            want = source_bytes(src)
            try:
                have = gzip.decompress(open(gz, "rb").read())
            except OSError:
                have = None
            if have is not None and have.replace(b"\r\n", b"\n") == want.replace(b"\r\n", b"\n"):
                continue
            stale.append(rel)
            if fix:
                open(gz, "wb").write(compress(want))
    for rel in stale:
        print(("regenerated " if fix else "STALE ") + rel)
    for rel in orphans:
        print("ORPHAN " + rel + " (no source next to it)")
    if (stale and not fix) or orphans:
        print("Run: python tools/ci/check_web_gz.py --fix")
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
