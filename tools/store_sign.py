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


def sign_text(text, key=None):
    """text + its "sig" line."""
    sig = (key or load_private()).sign(text, ec.ECDSA(hashes.SHA256()))
    return text + b"sig " + sig.hex().encode("ascii") + b"\n"


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
    with open(PUB_PATH, "rb") as f:
        pub = serialization.load_pem_public_key(f.read())
    try:
        pub.verify(bytes.fromhex(sig_hex.decode("ascii")), signed, ec.ECDSA(hashes.SHA256()))
    except (InvalidSignature, ValueError, UnicodeDecodeError):
        return "bad signature"
    return None


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
