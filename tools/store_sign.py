"""Signed app packages for the NucleoOS store (apps/<id>/package.sig).

From firmware 1.1.128 the device installs a store app only when apps/<id>/package.sig carries an
ECDSA P-256 signature, made with the offline STORE key, over

    nucleoos-app-v1
    <id>
    <version>
    <sha256> <size> <path>        every file of the package, paths ascending (byte order)

and every file it downloads is listed there with a matching sha256 and size. The line format is
parsed by components/nv_appstore/nv_store_pkg.cpp (keep both in step).

The store key is NOT the OTA key: a leaked store key must not be able to ship firmware.
Private key: %USERPROFILE%\\.nucleo\\store-signing-key.pem (NUCLEO_STORE_KEY overrides). Never in the
repo; back it up. Public key: components/nv_appstore/store_signing_pub.pem (compiled in).

  python tools/store_sign.py keygen                 one-time: new key pair (refuses to overwrite)
  python tools/store_sign.py sign <app dir>         write <app dir>/package.sig
  python tools/store_sign.py verify <app dir>       check package.sig against the files

Data packs (catalog rows "kind":"data": ANIMA's knowledge) have their own signed text, data/<id>/pack.sig:

    nucleoos-data-v1
    <id>
    <version>
    <dest>                                   anima/kb | anima  (under /sdcard/data on the device)
    <sha256> <size> <name> <url>             one line per file, or per part (same name, consecutive)

They are described in server/appstore/data_packs.json (tools/kb/publish.py writes it) and signed by
server/appstore/export_static.py with data_pack_text() + sign_text().

System content packs (web companion, ANIMA's offline data, drivers: content/<id>/pack.sig, never in a
store*.json catalog, read only by firmware >= 1.2.61) use the v2 text, content_pack_text():

    nucleoos-data-v2
    <id>
    <version>
    <dest>                                   web | data/anima | data/anima/kb | data/tts | nucleos/drivers
    <sha256> <size> <kind> <name> <url> [<url2>]     kind f file, i tree index, t tree (replace), u tree (seed)

and a tree's index is tree_index_text(). tools/content/build.py writes both.

Backup key: a second store key pair whose private half lives OFFLINE (keygen-backup writes it to
%USERPROFILE%\\.nucleo\\store-signing-backup-key.pem; move it to offline storage). Firmware >= 1.2.61
accepts a signature from either key, so losing the main key never strands the devices.
"""
import argparse
import hashlib
import json
import os
import re
import sys

from cryptography.exceptions import InvalidSignature
from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric import ec

ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
PUB_PATH = os.path.join(ROOT, "components", "nv_appstore", "store_signing_pub.pem")
BACKUP_PUB_PATH = os.path.join(ROOT, "components", "nv_appstore", "store_signing_backup_pub.pem")
DOMAIN = "nucleoos-app-v1"
SIG_NAME = "package.sig"
# Never part of a package: the signature itself, the human guide (served as docs/<id>.html),
# editor/OS junk.
SKIP = {SIG_NAME, "GUIDE.md", "Thumbs.db", "desktop.ini", ".DS_Store"}
PATH_RE = re.compile(r"^[A-Za-z0-9._-]+(/[A-Za-z0-9._-]+){0,2}$")
MAX_FILES = 264          # 256 assets + module, aot, manifest, icons, files.json (nv_store_pkg kMaxFiles)
MAX_FILE = 24 * 1024 * 1024


def key_path():
    return os.environ.get("NUCLEO_STORE_KEY") or os.path.join(os.path.expanduser("~"), ".nucleo",
                                                             "store-signing-key.pem")


def backup_key_path():
    return os.environ.get("NUCLEO_STORE_BACKUP_KEY") or os.path.join(os.path.expanduser("~"), ".nucleo",
                                                                    "store-signing-backup-key.pem")


def have_key():
    return os.path.isfile(key_path())


def load_private():
    p = key_path()
    if not os.path.isfile(p):
        sys.exit("store signing key not found: %s\n(run `python tools/store_sign.py keygen` once, or "
                 "set NUCLEO_STORE_KEY). Devices refuse unsigned store apps." % p)
    with open(p, "rb") as f:
        return serialization.load_pem_private_key(f.read(), password=None)


def package_files(app_dir):
    """[(relpath, abspath)] of every file the package ships, sorted by path bytes."""
    out = []
    for dirpath, dirnames, filenames in os.walk(app_dir):
        dirnames[:] = [d for d in dirnames if not d.startswith(".")]
        for n in filenames:
            if n in SKIP or n.startswith((".", "GUIDE")) or n.endswith(".tmp"):
                continue
            ap = os.path.join(dirpath, n)
            rel = os.path.relpath(ap, app_dir).replace(os.sep, "/")
            if not PATH_RE.match(rel) or len(rel) > 63 or any(s in (".", "..") for s in rel.split("/")):
                sys.exit("%s: file name not allowed in a package: %s" % (app_dir, rel))
            out.append((rel, ap))
    out.sort(key=lambda t: t[0].encode("utf-8"))
    if not out or len(out) > MAX_FILES:
        sys.exit("%s: %d files (1..%d allowed)" % (app_dir, len(out), MAX_FILES))
    return out


def manifest_id_version(app_dir):
    with open(os.path.join(app_dir, "manifest.json"), encoding="utf-8-sig") as f:
        m = json.load(f)
    app_id = m.get("id") or os.path.basename(os.path.normpath(app_dir))
    return app_id, str(m.get("version", "1"))


def package_text(app_id, ver, entries):
    """The signed body for [(relpath, bytes)] entries (any order)."""
    if not re.match(r"^[A-Za-z0-9_-]{1,31}$", app_id) or not re.match(r"^[0-9.]{1,15}$", ver):
        raise ValueError("id '%s' / version '%s' not allowed" % (app_id, ver))
    entries = sorted(entries, key=lambda t: t[0].encode("utf-8"))
    if not entries or len(entries) > MAX_FILES:
        raise ValueError("%d files (1..%d allowed)" % (len(entries), MAX_FILES))
    lines = [DOMAIN, app_id, ver]
    prev = None
    for rel, data in entries:
        if (not PATH_RE.match(rel) or len(rel) > 63 or any(x in (".", "..") for x in rel.split("/"))
                or rel == prev):
            raise ValueError("file name not allowed in a package: %s" % rel)
        if len(data) > MAX_FILE:
            raise ValueError("%s is over %d bytes" % (rel, MAX_FILE))
        lines.append("%s %d %s" % (hashlib.sha256(data).hexdigest(), len(data), rel))
        prev = rel
    return ("\n".join(lines) + "\n").encode("ascii")


DATA_DOMAIN = "nucleoos-data-v1"
DATA_DESTS = ("anima/kb", "anima")                 # nv_store_pkg kDataDests
DATA_NAME_RE = re.compile(r"^[A-Za-z0-9._-]{1,47}$")
DATA_FILE_MAX = 0xFFFFFFFF                          # per file (all its parts): FAT32


def data_pack_text(pack):
    """The signed span of a data pack: pack = {"id", "version", "dest", "files": [{"name", "size",
    "sha256", "url"}]} (parts of one file = consecutive entries with the same name)."""
    if pack["dest"] not in DATA_DESTS:
        raise ValueError("dest %r not allowed" % pack["dest"])
    lines = [DATA_DOMAIN, pack["id"], str(pack["version"]), pack["dest"]]
    seen, prev, total = set(), None, 0
    for f in pack["files"]:
        name, url, size = f["name"], f["url"], int(f["size"])
        if not DATA_NAME_RE.match(name) or name in (".", ".."):
            raise ValueError("file name not allowed: %s" % name)
        if not url.startswith("https://") or any(c <= " " or c > "~" for c in url) or len(url) > 255:
            raise ValueError("url not allowed: %s" % url)
        if name != prev:
            if name in seen:
                raise ValueError("%s: parts of a file must be consecutive" % name)
            seen.add(name); total = 0
        total += size
        if size <= 0 or total > DATA_FILE_MAX:
            raise ValueError("%s: size %d not allowed" % (name, total))
        if not re.fullmatch(r"[0-9a-f]{64}", f["sha256"]):
            raise ValueError("%s: bad sha256" % name)
        lines.append("%s %d %s %s" % (f["sha256"], size, name, url))
        prev = name
    if not pack["files"]:
        raise ValueError("a data pack needs at least one file")
    return ("\n".join(lines) + "\n").encode("ascii")


CONTENT_DOMAIN = "nucleoos-data-v2"
CONTENT_DESTS = ("web", "data/anima", "data/anima/kb", "data/tts", "nucleos/drivers")   # nv_store_pkg kContentDests
CONTENT_KINDS = "fitu"
CONTENT_PATH_RE = re.compile(r"^[A-Za-z0-9._-]+(/[A-Za-z0-9._-]+){0,2}$")             # nv_store_pkg path_ok
TREE_DOMAIN = "nucleoos-tree-v1"
TREE_PATH_RE = re.compile(r"^[A-Za-z0-9._+-]+(/[A-Za-z0-9._+-]+){0,7}$")               # nv_store_tree path_ok
TREE_INDEX_MAX = 512 * 1024
TREE_FILES_MAX = 4096


def _url_ok(url):
    return url.startswith("https://") and len(url) <= 255 and not any(c <= " " or c > "~" for c in url)


def content_pack_text(pack):
    """The signed span of a system content pack: pack = {"id", "version", "dest", "files": [{"kind",
    "name", "size", "sha256", "url", "url2"?}]}. Mirrors nv_store_pkg::parse_data's v2 rules."""
    if pack["dest"] not in CONTENT_DESTS:
        raise ValueError("dest %r not allowed" % pack["dest"])
    if not re.fullmatch(r"[A-Za-z0-9_-]{1,31}", pack["id"]) or not re.fullmatch(r"[0-9.]{1,15}", str(pack["version"])):
        raise ValueError("bad id/version")
    files = pack["files"]
    if not files or len(files) > 64:
        raise ValueError("a content pack has 1..64 lines")
    lines = [CONTENT_DOMAIN, pack["id"], str(pack["version"]), pack["dest"]]
    groups, total = [], 0
    for i, f in enumerate(files):
        kind, name, size = f["kind"], f["name"], int(f["size"])
        if kind not in CONTENT_KINDS:
            raise ValueError("%s: kind %r" % (name, kind))
        if name == ".":
            if kind == "f":
                raise ValueError("a file can't be '.'")
        elif (not CONTENT_PATH_RE.match(name) or len(name) > 63 or any(x in (".", "..") for x in name.split("/"))
              or name.endswith((".part", ".new", ".old", ".tmp"))):
            raise ValueError("name not allowed: %s" % name)
        if not _url_ok(f["url"]) or (f.get("url2") and not _url_ok(f["url2"])):
            raise ValueError("%s: url not allowed" % name)
        if not re.fullmatch(r"[0-9a-f]{64}", f["sha256"]) or size <= 0:
            raise ValueError("%s: bad sha256/size" % name)
        prev = files[i - 1] if i else None
        if kind == "i" and (i + 1 >= len(files) or files[i + 1]["kind"] not in "tu" or files[i + 1]["name"] != name
                            or size > TREE_INDEX_MAX):
            raise ValueError("%s: an index must be followed by its tree" % name)
        if kind in "tu" and (not prev or prev["kind"] != "i"):
            raise ValueError("%s: a tree must follow its index" % name)
        cont = prev is not None and prev["name"] == name and (
            (kind == "f" and prev["kind"] == "f") or (kind in "tu" and prev["kind"] == "i"))
        if prev is not None and prev["name"] == name and not cont:
            raise ValueError("%s: listed twice" % name)
        if not cont:
            for g in groups:
                if g == name or "." in (g, name) or name.startswith(g + "/") or g.startswith(name + "/"):
                    raise ValueError("%s overlaps %s" % (name, g))
            groups.append(name)
            total = 0
        total += size
        if total > DATA_FILE_MAX:
            raise ValueError("%s: over 4 GB" % name)
        line = "%s %d %s %s %s" % (f["sha256"], size, kind, name, f["url"])
        if f.get("url2"):
            line += " " + f["url2"]
        lines.append(line)
    return ("\n".join(lines) + "\n").encode("ascii")


def tree_index_text(entries):
    """A tree's index: entries = [(relpath, size, sha256 hex)]. Mirrors nv_store_tree::parse_index."""
    entries = sorted(entries, key=lambda e: e[0].encode("utf-8"))
    if not entries or len(entries) > TREE_FILES_MAX:
        raise ValueError("a tree has 1..%d files" % TREE_FILES_MAX)
    paths = set()
    lines = [TREE_DOMAIN]
    for rel, size, sha in entries:
        if (not TREE_PATH_RE.match(rel) or len(rel) > 159 or any(x in (".", "..") for x in rel.split("/"))
                or rel in paths):
            raise ValueError("path not allowed in a tree: %s" % rel)
        if not re.fullmatch(r"[0-9a-f]{64}", sha) or not 0 <= size <= DATA_FILE_MAX:
            raise ValueError("%s: bad sha256/size" % rel)
        paths.add(rel)
        lines.append("%s %d %s" % (sha, size, rel))
    for rel in paths:
        parts = rel.split("/")
        for k in range(1, len(parts)):
            if "/".join(parts[:k]) in paths:
                raise ValueError("%s is a file and a folder" % "/".join(parts[:k]))
    text = ("\n".join(lines) + "\n").encode("ascii")
    if len(text) > TREE_INDEX_MAX:
        raise ValueError("tree index over 512 KB")
    return text


def sign_text(text, key=None):
    """text + its "sig" line."""
    sig = (key or load_private()).sign(text, ec.ECDSA(hashes.SHA256()))
    return text + b"sig " + sig.hex().encode("ascii") + b"\n"


def sign_text_reuse(text, old, key=None):
    """sign_text(), but an `old` signed text that still covers exactly `text` with a valid signature is
    kept: ECDSA signatures are randomized, and re-signing unchanged files churns the store repo."""
    if old and old.startswith(text) and old[len(text):].startswith(b"sig ") and verify_bytes(old) is None:
        return old
    return sign_text(text, key)


def body(app_dir):
    app_id, ver = manifest_id_version(app_dir)
    entries = []
    for rel, ap in package_files(app_dir):
        with open(ap, "rb") as f:
            entries.append((rel, f.read()))
    try:
        return package_text(app_id, ver, entries)
    except ValueError as e:
        sys.exit("%s: %s" % (app_dir, e))


def check_pub(key):
    pub = key.public_key().public_bytes(serialization.Encoding.PEM,
                                        serialization.PublicFormat.SubjectPublicKeyInfo)
    with open(PUB_PATH, "rb") as f:
        if f.read().replace(b"\r\n", b"\n").strip() != pub.strip():
            sys.exit("the store key does not match %s: devices would reject these packages" % PUB_PATH)


def sign_dir(app_dir, key=None):
    """(Re)write app_dir/package.sig. Returns True when the file changed."""
    key = key or load_private()
    b = body(app_dir)
    path = os.path.join(app_dir, SIG_NAME)
    try:                                   # ECDSA signatures are randomized: keep a still-valid one
        with open(path, "rb") as f:
            old = f.read()
        if old.startswith(b) and verify_bytes(old) is None:
            return False
    except OSError:
        pass
    with open(path, "wb") as f:
        f.write(sign_text(b, key))
    return True


def verify_bytes(text):
    idx = text.rfind(b"\nsig ")
    if idx < 0 or not text.endswith(b"\n"):
        return "no signature line"
    signed, sig_hex = text[:idx + 1], text[idx + 5:-1]
    # the main key, or (firmware >= 1.2.61) the offline backup key
    for path in (PUB_PATH, BACKUP_PUB_PATH):
        if not os.path.isfile(path):
            continue
        with open(path, "rb") as f:
            pub = serialization.load_pem_public_key(f.read())
        try:
            pub.verify(bytes.fromhex(sig_hex.decode("ascii")), signed, ec.ECDSA(hashes.SHA256()))
            return None
        except (InvalidSignature, ValueError, UnicodeDecodeError):
            continue
    return "bad signature"


def verify_dir(app_dir):
    try:
        with open(os.path.join(app_dir, SIG_NAME), "rb") as f:
            text = f.read()
    except OSError:
        return "no package.sig"
    err = verify_bytes(text)
    if err:
        return err
    if not text.startswith(body(app_dir)):
        return "files differ from package.sig"
    return None


def cmd_keygen(_):
    p = key_path()
    if os.path.exists(p):
        sys.exit("refusing to overwrite %s (a new key breaks installs on every device trusting the old one)" % p)
    key = ec.generate_private_key(ec.SECP256R1())
    os.makedirs(os.path.dirname(p), exist_ok=True)
    with open(p, "wb") as f:
        f.write(key.private_bytes(serialization.Encoding.PEM, serialization.PrivateFormat.PKCS8,
                                  serialization.NoEncryption()))
    with open(PUB_PATH, "wb") as f:
        f.write(key.public_key().public_bytes(serialization.Encoding.PEM,
                                              serialization.PublicFormat.SubjectPublicKeyInfo))
    print("private key: %s  (BACK IT UP, never commit it)" % p)
    print("public key:  %s  (commit it: the firmware embeds it)" % PUB_PATH)


def cmd_keygen_backup(_):
    p = backup_key_path()
    if os.path.exists(p) or os.path.exists(BACKUP_PUB_PATH):
        sys.exit("refusing to overwrite the backup key (%s / %s): devices already trust it" % (p, BACKUP_PUB_PATH))
    key = ec.generate_private_key(ec.SECP256R1())
    os.makedirs(os.path.dirname(p), exist_ok=True)
    with open(p, "wb") as f:
        f.write(key.private_bytes(serialization.Encoding.PEM, serialization.PrivateFormat.PKCS8,
                                  serialization.NoEncryption()))
    with open(BACKUP_PUB_PATH, "wb") as f:
        f.write(key.public_key().public_bytes(serialization.Encoding.PEM,
                                              serialization.PublicFormat.SubjectPublicKeyInfo))
    print("backup private key: %s" % p)
    print("  -> MOVE IT OFFLINE (USB stick in a drawer) and keep a second copy; delete it from this PC")
    print("backup public key:  %s  (commit it: the firmware embeds it)" % BACKUP_PUB_PATH)


def cmd_sign(a):
    key = load_private()
    check_pub(key)
    for d in a.dirs:
        print("%s %s" % ("signed " if sign_dir(d, key) else "current", d))


def cmd_verify(a):
    bad = 0
    for d in a.dirs:
        err = verify_dir(d)
        print("%s %s%s" % ("OK  " if not err else "FAIL", d, "" if not err else ": " + err))
        bad += bool(err)
    sys.exit(1 if bad else 0)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    sub.add_parser("keygen").set_defaults(fn=cmd_keygen)
    sub.add_parser("keygen-backup").set_defaults(fn=cmd_keygen_backup)
    s = sub.add_parser("sign")
    s.add_argument("dirs", nargs="+")
    s.set_defaults(fn=cmd_sign)
    v = sub.add_parser("verify")
    v.add_argument("dirs", nargs="+")
    v.set_defaults(fn=cmd_verify)
    a = ap.parse_args()
    a.fn(a)


if __name__ == "__main__":
    main()
