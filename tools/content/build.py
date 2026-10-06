"""Build the system content packs (docs/CONTENT_PACKS_PLAN.md) from the sd/ mirror.

A content pack is what the OS needs on the microSD that the firmware can't carry: the web companion,
ANIMA's offline data. Each pack is built from an ALLOWLIST (PACKS below: exact files, or whole folders
that hold only distributable data) and every byte is scanned for secrets before anything is written:
the sd/ mirror also holds the owner's own files (keys, conversations), which must never ship.

Output (default tools/content/out/<version>/, never committed):
  assets/<asset>            what gets uploaded to the GitHub release <tag> (trees as ustar + index)
  store/content/<id>/pack.sig   the signed v2 text (with --sign), for the store repo
  packs.json                what was built: per pack, every line with its source

  python tools/content/build.py                       build every pack, unsigned (a dry run)
  python tools/content/build.py --sign                ... and sign the pack.sig files (store key)
  python tools/content/build.py --only sys-web --sign --out <dir>
  python tools/content/build.py --scan-only           only the secret / allowlist checks

The device side: components/nv_appstore (install_content), nv_store_pkg (v2 text), nv_store_tree
(index, ustar). Publishing (release upload + store graft) is tools/dist.py content.
"""
import argparse
import datetime
import hashlib
import io
import json
import os
import re
import sys
import tarfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, "..", ".."))
SD = os.path.join(ROOT, "sd")
sys.path.insert(0, os.path.join(ROOT, "tools"))
import store_sign  # noqa: E402

REPO = "indecenti/nucleoos-p4-store"
URL = "https://github.com/%s/releases/download/{tag}/{asset}" % REPO

# Files the device writes itself (the owner's data, some encrypted to the chip) and files with keys:
# never in a pack, wherever they appear. Same list as tools/sync-sd.ps1.
USER_DATA = {
    "teacher.json", "telegram.json", "workspace.json", "session.txt", "context.json", "memory.jsonl",
    "MEMORY.md", "SOUL.md", "USER.md", "HEARTBEAT.md", "profile.tsv", "rules.json", "timers.json",
    "permissions.json", "chatlog.ndjson", "telemetry.ndjson", "phrases.user.tsv", "user.tsv", "user.vec",
    "units.txt", "agent_last.txt", "settings.nvb", "tele.bin", "perms.json",
}
# What a real credential looks like (a prefix alone, as in a key validator, is fine).
SECRETS = [
    re.compile(rb"AIza[0-9A-Za-z_-]{35}"),                       # Google / Gemini
    re.compile(rb"AQ\.[A-Za-z0-9_-]{40,}"),                      # Google / Gemini, 2026 format
    re.compile(rb'"(?:api_?key|key|token|secret|password)"\s*:\s*"[^"\s]{20,}"', re.I),   # any JSON credential
    re.compile(rb"sk-ant-[A-Za-z0-9_-]{40,}"),                   # Anthropic
    re.compile(rb"sk-(?:proj-)?[A-Za-z0-9_-]{40,}"),             # OpenAI
    re.compile(rb"gsk_[A-Za-z0-9]{40,}"),                        # Groq
    re.compile(rb"xai-[A-Za-z0-9]{40,}"),                        # xAI
    re.compile(rb"hf_[A-Za-z0-9]{30,}"),                         # Hugging Face
    re.compile(rb"gh[pousr]_[A-Za-z0-9]{36,}|github_pat_[A-Za-z0-9_]{40,}"),
    re.compile(rb"-----BEGIN (?:EC |RSA |OPENSSH |)PRIVATE KEY-----\s+[A-Za-z0-9+/=\s]{64,}"),   # a body, not a placeholder
    re.compile(rb"\b\d{8,10}:AA[0-9A-Za-z_-]{30,}"),             # Telegram bot token
]

REPO_LICENSE = """{title}

Part of NucleoOS (https://github.com/indecenti/NucleoOS-P4), distributed under the same terms as
the project: PolyForm Noncommercial License 1.0.0 (see LICENSE.md in the repository). Commercial
use: see COMMERCIAL.md.
"""

# The packs. Lines: ("f", name, src) a file; ("t", name, src_dir) a tree that replaces <dest>/<name>
# ("." = dest itself); ("u", name, src_dir) a seed tree (adds missing files, keeps the owner's);
# ("lic", name, text_or_src) a license file. src paths are relative to sd/.
def _dict(lang, lex=True):
    lines = [("f", "dict-%s-en.tsv" % lang, "data/anima/dict-%s-en.tsv" % lang),
             ("f", "dict-en-%s.tsv" % lang, "data/anima/dict-en-%s.tsv" % lang),
             ("f", "forms-%s.tsv" % lang, "data/anima/forms-%s.tsv" % lang)]
    if lex:
        lines.append(("f", "lex-%s.tsv" % lang, "data/anima/lex-%s.tsv" % lang))
    lines.append(("lic", "LICENSE-dict-%s.txt" % lang, "data/anima/DICTIONARIES.txt"))
    return lines


PACKS = {
    "sys-web": {
        "dest": "web",
        "lines": [("t", ".", "web")],
        "license": "NucleoOS web companion",
    },
    "anima-core-it": {
        "dest": "data/anima",
        "lines": [
            ("f", "anima-it-encoder.bin", "data/anima/anima-it-encoder.bin"),
            ("f", "anima-it-akb5.bin", "data/anima/anima-it-akb5.bin"),
            ("t", "akb5", "data/anima/akb5"),
            ("f", "learned/facets.it.jsonl", "data/anima/learned/facets.it.jsonl"),
            ("f", "learned/facets.en.jsonl", "data/anima/learned/facets.en.jsonl"),
            ("f", "commands.it.json", "data/anima/commands.it.json"),
            ("f", "anima-it-index.bin.prov", "data/anima/anima-it-index.bin.prov"),
            ("u", "skills", "data/anima/skills"),
            ("lic", "LICENSE-anima-core.txt", None),
        ],
        "license": "ANIMA offline core (semantic encoder, AKB5 knowledge shards, facets, skills)",
    },
    "dict-it": {"dest": "data/anima", "lines": [
        ("f", "lex-it.tsv", "data/anima/lex-it.tsv"), ("f", "forms-it.tsv", "data/anima/forms-it.tsv"),
        ("f", "dict-it-en.tsv", "data/anima/dict-it-en.tsv"), ("f", "dict-en-it.tsv", "data/anima/dict-en-it.tsv"),
        ("lic", "LICENSE-dict-it.txt", "data/anima/DICTIONARIES.txt")]},
    "dict-en": {"dest": "data/anima", "lines": [
        ("f", "lex-en.tsv", "data/anima/lex-en.tsv"), ("f", "forms-en.tsv", "data/anima/forms-en.tsv"),
        ("lic", "LICENSE-dict-en.txt", "data/anima/DICTIONARIES.txt")]},
    "dict-es": {"dest": "data/anima", "lines": _dict("es", lex=False)},
    "dict-fr": {"dest": "data/anima", "lines": _dict("fr", lex=False)},
    "dict-de": {"dest": "data/anima", "lines": _dict("de", lex=False)},
    # drivers-win: NOT built - the Windows driver is a third-party binary whose redistribution terms
    # are unknown (docs/CONTENT_PACKS_PLAN.md).
}


def die(msg):
    sys.exit("content: " + msg)


def scan_bytes(label, data):
    for rx in SECRETS:
        m = rx.search(data)
        if m:
            # never echo the match: the error ends up in terminals and logs
            die("%s looks like it holds a credential (byte %d): refusing to pack it" % (label, m.start()))


def check_name(rel):
    base = rel.replace("\\", "/").rsplit("/", 1)[-1]
    if base in USER_DATA:
        die("%s is owner data and never ships" % rel)


def tree_files(src_dir):
    """[(relpath, abspath)] of a source folder, sorted by path bytes; hidden files skipped."""
    out = []
    for dp, dns, fns in os.walk(src_dir):
        dns[:] = sorted(d for d in dns if not d.startswith("."))
        for n in fns:
            if n.startswith(".") or n in ("Thumbs.db", "desktop.ini"):
                continue
            ap = os.path.join(dp, n)
            out.append((os.path.relpath(ap, src_dir).replace("\\", "/"), ap))
    return sorted(out, key=lambda e: e[0].encode("utf-8"))


def ustar(files):
    """A deterministic ustar archive of [(relpath, bytes)]: no folders, no owners, mtime 0."""
    buf = io.BytesIO()
    with tarfile.open(fileobj=buf, mode="w", format=tarfile.USTAR_FORMAT) as tf:
        for rel, data in files:
            ti = tarfile.TarInfo(rel)
            ti.size, ti.mode, ti.mtime, ti.uid, ti.gid, ti.uname, ti.gname = len(data), 0o644, 0, 0, 0, "", ""
            tf.addfile(ti, io.BytesIO(data))
    return buf.getvalue()


def asset_name(pid, name, ext=""):
    flat = "root" if name == "." else name.replace("/", "__")
    return "%s--%s%s" % (pid, flat, ext)


def build_pack(pid, spec, version, tag, out, scan_only):
    lines = []
    sizes = 0

    def add(kind, name, data, asset):
        nonlocal sizes
        sha = hashlib.sha256(data).hexdigest()
        if not scan_only:
            with open(os.path.join(out, "assets", asset), "wb") as f:
                f.write(data)
        lines.append({"kind": kind, "name": name, "size": len(data), "sha256": sha, "asset": asset,
                      "url": URL.format(tag=tag, asset=asset)})
        sizes += len(data)

    for kind, name, src in spec["lines"]:
        if kind == "lic":
            if src:
                text = open(os.path.join(SD, src), "rb").read()
            else:
                text = REPO_LICENSE.format(title=spec.get("license", pid)).encode("utf-8")
            add("f", name, text, asset_name(pid, name))
        elif kind == "f":
            check_name(src)
            data = open(os.path.join(SD, src), "rb").read()
            scan_bytes(src, data)
            add("f", name, data, asset_name(pid, name))
        elif kind in "tu":
            entries, files = [], []
            for rel, ap in tree_files(os.path.join(SD, src)):
                check_name(src + "/" + rel)
                data = open(ap, "rb").read()
                scan_bytes(src + "/" + rel, data)
                entries.append((rel, len(data), hashlib.sha256(data).hexdigest()))
                files.append((rel, data))
            idx = store_sign.tree_index_text(entries)
            add("i", name, idx, asset_name(pid, name, ".idx"))
            add(kind, name, ustar(files), asset_name(pid, name, ".tar"))
        else:
            die("%s: unknown line kind %r" % (pid, kind))
    pack = {"id": pid, "version": version, "dest": spec["dest"], "files": lines}
    text = store_sign.content_pack_text(pack)      # validates every v2 rule the device checks
    return pack, text, sizes


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    today = datetime.date.today()
    ap.add_argument("--version", default="%d.%d.1" % (today.year, today.month), help="pack version (YYYY.M.N)")
    ap.add_argument("--tag", help="GitHub release tag (default content-<version>)")
    ap.add_argument("--only", action="append", help="pack id (repeatable)")
    ap.add_argument("--out", help="output folder (default tools/content/out/<version>)")
    ap.add_argument("--sign", action="store_true", help="write store/content/<id>/pack.sig (store key)")
    ap.add_argument("--scan-only", action="store_true", help="only the allowlist + secret checks")
    a = ap.parse_args()
    tag = a.tag or "content-" + a.version
    out = a.out or os.path.join(HERE, "out", a.version)
    ids = a.only or list(PACKS)
    for i in ids:
        if i not in PACKS:
            die("unknown pack %s (known: %s)" % (i, ", ".join(PACKS)))
    if not a.scan_only:
        os.makedirs(os.path.join(out, "assets"), exist_ok=True)
    key = None
    if a.sign:
        key = store_sign.load_private()
        store_sign.check_pub(key)
    report = []
    for pid in ids:
        pack, text, size = build_pack(pid, PACKS[pid], a.version, tag, out, a.scan_only)
        report.append(pack)
        print("  %-14s %-12s %3d lines %8.1f MB" % (pid, pack["dest"], len(pack["files"]), size / 1048576))
        if a.sign:
            d = os.path.join(out, "store", "content", pid)
            os.makedirs(d, exist_ok=True)
            p = os.path.join(d, "pack.sig")
            old = open(p, "rb").read() if os.path.isfile(p) else None
            with open(p, "wb") as f:
                f.write(store_sign.sign_text_reuse(text, old, key))
    if a.scan_only:
        print("scan OK: no owner data, no credentials")
        return
    with open(os.path.join(out, "packs.json"), "w", encoding="utf-8") as f:
        json.dump({"version": a.version, "tag": tag, "packs": report}, f, indent=1)
    print("built %d pack(s) in %s" % (len(ids), out))


if __name__ == "__main__":
    main()
