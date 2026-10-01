"""Signed OTA manifests for NucleoOS P4.

The device installs a remote update only when the manifest carries an ECDSA P-256 signature, made
with the offline release key, over

    "nucleoos-ota-v1\\n<version>\\n<sha256 of the image, lowercase hex>\\n<size in bytes>\\n"

and the image it then writes to flash hashes to exactly that sha256/size. The URL is not signed, so
the same manifest works from GitHub Pages or the local test server.

Private key: %USERPROFILE%\\.nucleo\\ota-signing-key.pem (NUCLEO_OTA_KEY overrides). It never goes in
the repo; back it up: without it no device accepts a new update over the air (only a USB flash).
Public key: components/nv_fwup/ota_signing_pub.pem, compiled into the firmware.

  python tools/ota_sign.py keygen                      one-time: new key pair (refuses to overwrite)
  python tools/ota_sign.py manifest --bin B --url U --notes N --out manifest.json
  python tools/ota_sign.py verify manifest.json [--bin B]
"""
import argparse
import hashlib
import json
import os
import sys

from cryptography.exceptions import InvalidSignature
from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric import ec

ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
PUB_PATH = os.path.join(ROOT, "components", "nv_fwup", "ota_signing_pub.pem")
DOMAIN = "nucleoos-ota-v1"


def key_path():
    return os.environ.get("NUCLEO_OTA_KEY") or os.path.join(os.path.expanduser("~"), ".nucleo", "ota-signing-key.pem")


def message(version, sha256_hex, size):
    return ("%s\n%s\n%s\n%d\n" % (DOMAIN, version, sha256_hex, size)).encode("ascii")


def image_version(data):
    """The version string in the image's esp_app_desc (as tools/dist.py reads it)."""
    return data[0x20 + 0x10:0x20 + 0x30].split(b"\0")[0].decode("ascii")


def load_private():
    p = key_path()
    if not os.path.isfile(p):
        sys.exit("OTA signing key not found: %s\n(run `python tools/ota_sign.py keygen` once, or set "
                 "NUCLEO_OTA_KEY). Refusing to publish an unsigned update: devices would reject it." % p)
    with open(p, "rb") as f:
        return serialization.load_pem_private_key(f.read(), password=None)


def sign_fields(version, data):
    """{"size", "sha256", "sig"} for the image bytes `data` of `version`."""
    sha = hashlib.sha256(data).hexdigest()
    key = load_private()
    pub = key.public_key().public_bytes(serialization.Encoding.PEM, serialization.PublicFormat.SubjectPublicKeyInfo)
    with open(PUB_PATH, "rb") as f:   # compare line endings-agnostic (a CRLF checkout is fine)
        if f.read().replace(b"\r\n", b"\n").strip() != pub.strip():
            sys.exit("the signing key does not match %s: the firmware would reject this manifest" % PUB_PATH)
    sig = key.sign(message(version, sha, len(data)), ec.ECDSA(hashes.SHA256()))
    return {"size": len(data), "sha256": sha, "sig": sig.hex()}


def verify(manifest, data=None):
    with open(PUB_PATH, "rb") as f:
        pub = serialization.load_pem_public_key(f.read())
    msg = message(manifest["version"], manifest["sha256"], int(manifest["size"]))
    try:
        pub.verify(bytes.fromhex(manifest["sig"]), msg, ec.ECDSA(hashes.SHA256()))
    except (InvalidSignature, ValueError, KeyError):
        return "bad signature"
    if data is not None and (len(data) != int(manifest["size"]) or hashlib.sha256(data).hexdigest() != manifest["sha256"]):
        return "image does not match the manifest"
    return None


def cmd_keygen(_):
    p = key_path()
    if os.path.exists(p):
        sys.exit("refusing to overwrite %s (a new key locks out every device that trusts the old one)" % p)
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


def cmd_manifest(a):
    with open(a.bin, "rb") as f:
        data = f.read()
    ver = image_version(data)
    m = {"version": ver, "url": a.url, "notes": (a.notes or "release " + ver)[:1000]}
    m.update(sign_fields(ver, data))
    with open(a.out, "w", encoding="utf-8", newline="\n") as f:   # BOM-free: the device's cJSON
        f.write(json.dumps(m, ensure_ascii=False, separators=(",", ":")) + "\n")
    print("signed manifest %s (v%s, %d bytes)" % (a.out, ver, len(data)))


def cmd_verify(a):
    with open(a.manifest, encoding="utf-8") as f:
        m = json.load(f)
    data = open(a.bin, "rb").read() if a.bin else None
    err = verify(m, data)
    print("OK v%s" % m.get("version") if not err else "FAIL: " + err)
    sys.exit(1 if err else 0)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    sub.add_parser("keygen").set_defaults(fn=cmd_keygen)
    m = sub.add_parser("manifest")
    m.add_argument("--bin", required=True)
    m.add_argument("--url", required=True)
    m.add_argument("--notes", default="")
    m.add_argument("--out", required=True)
    m.set_defaults(fn=cmd_manifest)
    v = sub.add_parser("verify")
    v.add_argument("manifest")
    v.add_argument("--bin")
    v.set_defaults(fn=cmd_verify)
    a = ap.parse_args()
    a.fn(a)


if __name__ == "__main__":
    main()
