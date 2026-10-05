#!/usr/bin/env python3
"""Refuse to build with an sdkconfig that contradicts the committed sdkconfig.defaults*.

ESP-IDF applies sdkconfig.defaults only to options missing from an existing sdkconfig, so an old,
hand-edited sdkconfig silently wins over every later change to the defaults. That is how a shared
checkout ended up building with the GATT server off (link error in nv_bt), a 192 KB LVGL pool, the
mbedTLS heap in internal RAM - and NVS encryption on, which on first boot burns an HMAC key into
eFuse KEY5 and leaves every existing setting unreadable. The defaults are the source of truth: the
top-level CMakeLists.txt (and recovery/) run this check after configure and stop the build on drift.

    python tools/check_sdkconfig.py <sdkconfig> <defaults> [<defaults> ...]

Exit 1 on drift, with the options and the way out (regenerate the sdkconfig from the defaults).
Options that burn eFuses (NVS / flash encryption, secure boot) are refused unless the build sets
NV_ALLOW_EFUSE_SECURITY=1 in the environment: they change the chip for good.
"""
import os
import re
import sys

LINE_SET = re.compile(r"^(CONFIG_[A-Za-z0-9_]+)=(.*)$")
LINE_UNSET = re.compile(r"^# (CONFIG_[A-Za-z0-9_]+) is not set$")

# One-way switches: on a real board these program eFuses (keys, encryption / secure-boot bits).
EFUSE_OPTIONS = (
    "CONFIG_NVS_ENCRYPTION",
    "CONFIG_FLASH_ENCRYPTION_ENABLED",
    "CONFIG_SECURE_BOOT",
    "CONFIG_SECURE_BOOT_V2_ENABLED",
)


def norm(v):
    """Kconfig value as written in sdkconfig: bools are y / n, strings keep their quotes."""
    v = v.strip()
    return "n" if v in ("", "n") else v


def parse(path):
    vals = {}
    with open(path, encoding="utf-8", errors="replace") as f:
        for raw in f:
            line = raw.strip()
            m = LINE_SET.match(line)
            if m:
                vals[m.group(1)] = norm(m.group(2))
                continue
            m = LINE_UNSET.match(line)
            if m:
                vals[m.group(1)] = "n"
            # defaults files may carry other comments; sdkconfig "# CONFIG_X is not set" handled above
    return vals


def main(argv):
    if len(argv) < 3:
        print(__doc__)
        return 2
    sdk_path, defaults_paths = argv[1], argv[2:]
    sdk = parse(sdk_path)
    want = {}
    for p in defaults_paths:            # later files override earlier ones, as in ESP-IDF
        if os.path.isfile(p):
            want.update(parse(p))

    allow_efuse = os.environ.get("NV_ALLOW_EFUSE_SECURITY") == "1"
    drift = []
    for key, value in sorted(want.items()):
        if key not in sdk:
            continue   # unknown symbol or a dependency off: nothing to contradict
        if allow_efuse and key in EFUSE_OPTIONS:
            continue   # a deliberate security build may turn these on
        have = sdk[key]
        if have != value:
            drift.append((key, value, have))

    efuse = [k for k in EFUSE_OPTIONS if sdk.get(k, "n") == "y" and want.get(k, "n") != "y"]

    if not drift and (not efuse or allow_efuse):
        if efuse:
            print("check_sdkconfig: WARNING eFuse-burning options on by request: %s" % ", ".join(efuse))
        return 0

    out = ["", "=" * 78, "sdkconfig drift: %s contradicts the committed defaults" % sdk_path]
    for key, value, have in drift:
        out.append("  %-52s defaults %-10s sdkconfig %s" % (key, value, have))
    if efuse and not allow_efuse:
        out.append("")
        out.append("  eFuse-burning options are ON: %s" % ", ".join(efuse))
        out.append("  A board flashed with this build burns eFuse keys / bits for good (NVS encryption also")
        out.append("  makes every existing setting unreadable). Set NV_ALLOW_EFUSE_SECURITY=1 only for a")
        out.append("  deliberate security build in its own build directory.")
    out.append("")
    out.append("The defaults (sdkconfig.defaults*) are the source of truth. Regenerate this sdkconfig:")
    out.append("  move it aside (e.g. rename to sdkconfig.stale) and run  idf.py reconfigure")
    out.append("To change an option for good, edit sdkconfig.defaults* (and commit it), not sdkconfig.")
    out.append("=" * 78)
    print("\n".join(out))
    return 1


if __name__ == "__main__":
    sys.exit(main(sys.argv))
