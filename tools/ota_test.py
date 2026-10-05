"""Test images for the update safety net (docs/OTA.md "Testing on the board").

Test builds trust ONLY a test key (-DNV_FWUP_PUBKEY), which lives outside the repo, so a test image is
never signed with - or accepted by - the release key, and a production board never accepts a test image.

  python tools/ota_test.py keygen                      test key pair in %TEMP%\\nucleo-ota-test (once)
  python tools/ota_test.py pub                         path of the test public key (for -DNV_FWUP_PUBKEY)
  python tools/ota_test.py sign  BIN                   BIN + BIN.json (signed with the test key) for the card
  python tools/ota_test.py push  BIN [--host H]        sign, then copy both to the card root as
                                                       nucleos-anima.bin/.json (for `update sd`)

Build a test image (its own build dir: the key is a cached CMake variable):
  idf.py -B build_t -DNV_FWUP_PUBKEY=<pub> -DNV_TEST_VERSION=1.9.1 build
  idf.py -B build_t -DNV_TEST_VERSION=1.9.2 -DNV_OTA_FAULT=boot build      (boot | late | ui | net | "")
"""
import argparse
import hashlib
import json
import os
import sys
import tempfile
import urllib.parse
import urllib.request

from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric import ec

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import ota_sign  # noqa: E402  message() and image_version(), shared with the release signer
from nvtoken import auth_headers  # noqa: E402

KEY_DIR = os.path.join(tempfile.gettempdir(), "nucleo-ota-test")
PRIV = os.path.join(KEY_DIR, "ota-test-key.pem")
PUB = os.path.join(KEY_DIR, "ota-test-pub.pem")


def cmd_keygen(_):
    if os.path.exists(PRIV):
        print("test key already there: %s" % PUB)
        return
    os.makedirs(KEY_DIR, exist_ok=True)
    k = ec.generate_private_key(ec.SECP256R1())
    with open(PRIV, "wb") as f:
        f.write(k.private_bytes(serialization.Encoding.PEM, serialization.PrivateFormat.PKCS8,
                                serialization.NoEncryption()))
    with open(PUB, "wb") as f:
        f.write(k.public_key().public_bytes(serialization.Encoding.PEM,
                                            serialization.PublicFormat.SubjectPublicKeyInfo))
    print("test public key: %s" % PUB)


def sign(path):
    data = open(path, "rb").read()
    if ota_sign.embeds_release_key(data):
        sys.exit("refusing: %s embeds the RELEASE key - it is not a test build" % path)
    with open(PUB, "rb") as f:
        if f.read().replace(b"\r\n", b"\n").strip() not in data:
            sys.exit("refusing: %s does not embed the test key %s (would lock the test board out)" % (path, PUB))
    ver = ota_sign.image_version(data)
    sha = hashlib.sha256(data).hexdigest()
    with open(PRIV, "rb") as f:
        key = serialization.load_pem_private_key(f.read(), password=None)
    sig = key.sign(ota_sign.message(ver, sha, len(data)), ec.ECDSA(hashes.SHA256()))
    m = {"version": ver, "url": "nucleos-anima.bin", "notes": "TEST " + ver, "size": len(data), "sha256": sha,
         "sig": sig.hex()}
    out = path + ".json"
    with open(out, "w", encoding="utf-8", newline="\n") as f:
        f.write(json.dumps(m, separators=(",", ":")) + "\n")
    print("test-signed v%s -> %s" % (ver, out))
    return out


def cmd_pub(_):
    print(PUB)


def cmd_sign(a):
    sign(a.bin)


def host_default():
    h = os.environ.get("NV_BOARD_IP") or os.environ.get("NUCLEO_HOST")
    if not h:
        try:
            h = open(os.path.join(os.path.expanduser("~"), ".nucleo", "host"), encoding="utf-8").read().strip()
        except OSError:
            h = "nucleov2.local"
    return h


def put(host, dest, data):
    url = "http://%s/api/fs/write?path=%s" % (host, urllib.parse.quote(dest))
    req = urllib.request.Request(url, data=data, method="POST", headers=auth_headers())
    with urllib.request.urlopen(req, timeout=300) as r:
        r.read()


def cmd_push(a):
    man = sign(a.bin)
    host = a.host or host_default()
    put(host, "/nucleos-anima.bin", open(a.bin, "rb").read())
    put(host, "/nucleos-anima.json", open(man, "rb").read())
    print("pushed to %s: /sdcard/nucleos-anima.bin + .json  (then: nsh \"update sd\" ; nsh \"update restart\")" % host)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    sub.add_parser("keygen").set_defaults(fn=cmd_keygen)
    sub.add_parser("pub").set_defaults(fn=cmd_pub)
    s = sub.add_parser("sign")
    s.add_argument("bin")
    s.set_defaults(fn=cmd_sign)
    p = sub.add_parser("push")
    p.add_argument("bin")
    p.add_argument("--host", default="")
    p.set_defaults(fn=cmd_push)
    a = ap.parse_args()
    a.fn(a)


if __name__ == "__main__":
    main()
