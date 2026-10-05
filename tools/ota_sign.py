"""Signed OTA manifests for NucleoOS P4.

The device installs a remote update only when the manifest carries an ECDSA P-256 signature, made
with the offline release key, over

    "nucleoos-ota-v1\\n<version>\\n<sha256 of the image, lowercase hex>\\n<size in bytes>\\n"

and the image it then writes to flash hashes to exactly that sha256/size. The URL is not signed, so
the same manifest works from GitHub Pages or the local test server.

Private key: %USERPROFILE%\\.nucleo\\ota-signing-key.pem (NUCLEO_OTA_KEY overrides). It never goes in
the repo; back it up: without it no device accepts a new update over the air (only a USB flash).
Public key: components/nv_fwup/ota_signing_pub.pem, compiled into the firmware.

Backup key: %USERPROFILE%\\.nucleo\\ota-signing-key-backup.pem, public half
components/nv_fwup/ota_signing_pub_backup.pem. Every firmware trusts both, so if the primary key is
lost or leaks, releases signed with the backup key still reach every device (and can move them to a
new key). Keep the backup private key OFFLINE (USB stick in a safe), not on the build PC.

Lock-out guard: an image is signed only if it embeds the production public key (the device also
refuses an image that would no longer trust it). A test build (-DNV_FWUP_PUBKEY) is therefore never
signed with the release key.

  python tools/ota_sign.py keygen                      one-time: new key pair (refuses to overwrite)
  python tools/ota_sign.py keygen-backup               one-time: the offline backup key pair
  python tools/ota_sign.py fingerprint                 fingerprints of the embedded and local keys
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
PUB_BACKUP_PATH = os.path.join(ROOT, "components", "nv_fwup", "ota_signing_pub_backup.pem")
DOMAIN = "nucleoos-ota-v1"


def key_path():
    return os.environ.get("NUCLEO_OTA_KEY") or os.path.join(os.path.expanduser("~"), ".nucleo", "ota-signing-key.pem")


def backup_key_path():
    return os.path.join(os.path.expanduser("~"), ".nucleo", "ota-signing-key-backup.pem")


def pub_paths():
    return [p for p in (PUB_PATH, PUB_BACKUP_PATH) if os.path.isfile(p)]


def pem_bytes(path):
    with open(path, "rb") as f:
        return f.read().replace(b"\r\n", b"\n")


def embeds_release_key(data):
    """True when the image carries the production public key (nv_fwup embeds the PEM text)."""
    return pem_bytes(PUB_PATH).strip() in data


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


def sign_fields(version, data, image=True):
    """{"size", "sha256", "sig"} for the image bytes `data` of `version`. `image`: `data` is a firmware
    image, which must embed the production key (lock-out guard)."""
    if image and not embeds_release_key(data):
        sys.exit("refusing to sign v%s: the image does not embed %s (a test build, or a key change gone "
                 "wrong). A device that installed it could never be updated over the air again." % (version, PUB_PATH))
    sha = hashlib.sha256(data).hexdigest()
    key = load_private()
    pub = key.public_key().public_bytes(serialization.Encoding.PEM, serialization.PublicFormat.SubjectPublicKeyInfo)
    # compare line endings-agnostic (a CRLF checkout is fine)
    if all(pem_bytes(p).strip() != pub.strip() for p in pub_paths()):
        sys.exit("the signing key matches none of %s: the firmware would reject this manifest" % pub_paths())
    sig = key.sign(message(version, sha, len(data)), ec.ECDSA(hashes.SHA256()))
    return {"size": len(data), "sha256": sha, "sig": sig.hex()}


def verify(manifest, data=None):
    """None when one of the embedded release keys verifies the manifest (and `data` matches it)."""
    try:
        msg = message(manifest["version"], manifest["sha256"], int(manifest["size"]))
        sig = bytes.fromhex(manifest["sig"])
    except (ValueError, KeyError, TypeError):
        return "bad signature"
    good = False
    for p in pub_paths():
        try:
            serialization.load_pem_public_key(pem_bytes(p)).verify(sig, msg, ec.ECDSA(hashes.SHA256()))
            good = True
            break
        except InvalidSignature:
            pass
    if not good:
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


def cmd_keygen_backup(_):
    p = backup_key_path()
    if os.path.exists(p) or os.path.exists(PUB_BACKUP_PATH):
        sys.exit("refusing to overwrite the backup key (%s / %s): devices already trust it" % (p, PUB_BACKUP_PATH))
    key = ec.generate_private_key(ec.SECP256R1())
    os.makedirs(os.path.dirname(p), exist_ok=True)
    with open(p, "wb") as f:
        f.write(key.private_bytes(serialization.Encoding.PEM, serialization.PrivateFormat.PKCS8,
                                  serialization.NoEncryption()))
    with open(PUB_BACKUP_PATH, "wb") as f:
        f.write(key.public_key().public_bytes(serialization.Encoding.PEM,
                                              serialization.PublicFormat.SubjectPublicKeyInfo))
    print("backup private key: %s" % p)
    print("  -> copy it to OFFLINE storage (two USB sticks), check it with `fingerprint`, then delete it here")
    print("backup public key:  %s  (commit it: every firmware embeds it)" % PUB_BACKUP_PATH)


def fingerprint_pub(pub):
    der = pub.public_bytes(serialization.Encoding.DER, serialization.PublicFormat.SubjectPublicKeyInfo)
    return hashlib.sha256(der).hexdigest()[:16]


def cmd_fingerprint(_):
    for label, p in (("embedded primary", PUB_PATH), ("embedded backup ", PUB_BACKUP_PATH)):
        if os.path.isfile(p):
            print("%s %s  %s" % (label, fingerprint_pub(serialization.load_pem_public_key(pem_bytes(p))), p))
        else:
            print("%s MISSING           %s" % (label, p))
    for label, p in (("private primary ", key_path()), ("private backup  ", backup_key_path())):
        if os.path.isfile(p):
            with open(p, "rb") as f:
                k = serialization.load_pem_private_key(f.read(), password=None)
            print("%s %s  %s" % (label, fingerprint_pub(k.public_key()), p))
        else:
            print("%s not on this PC    %s" % (label, p))


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
    sub.add_parser("keygen-backup").set_defaults(fn=cmd_keygen_backup)
    sub.add_parser("fingerprint").set_defaults(fn=cmd_fingerprint)
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
