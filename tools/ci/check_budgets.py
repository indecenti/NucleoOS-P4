#!/usr/bin/env python3
"""Fail the build when the firmware grows past its memory budgets.

Internal SRAM is the scarce tier on the P4 (see docs/ENGINEERING_RULES.md section 1): static growth
there is what starved the Wi-Fi RX pool into random reboots and aborted the boot with "Could not
reserve DMA pool". These budgets turn that slow creep into a visible CI failure; raising one is a
reviewed, deliberate change to this file.

    python tools/ci/check_budgets.py [build_dir]

Needs ESP-IDF's environment (esp_idf_size). Writes a Markdown table to $GITHUB_STEP_SUMMARY when set.
"""
import json
import os
import re
import subprocess
import sys

# --- budgets (bytes) -----------------------------------------------------------------------------
# 2026-09-29, 1.1.118: internal static 192530, internal .bss 57784, image 3.63 MB of 4.5 MB (77%),
# PSRAM static ~490 KB. Headroom is deliberate but small: every new static buffer should go to PSRAM
# (NV_PSRAM_BSS) or be allocated per app open.
BUDGETS = {
    "internal_static": 210_000,   # .data + .bss + IRAM text in internal RAM (esp_idf_size used_diram)
    "internal_bss": 72_000,       # zero-initialised statics in internal RAM
    "psram_static": 840 * 1024,   # .ext_ram.bss (640 -> 800 KB: LVGL pool 192 -> 320 KB, 1.1.143;
                                  # 800 -> 840 KB: vertice engine state out of internal .data)
    "image_pct": 90.0,            # system image vs the `system` slot (layout v2: 10 MB, docs/OTA.md)
    "recovery_pct": 90.0,         # recovery image vs the `recovery` slot (1 MB)
}


def partition_size(partitions_csv, name):
    """Size of the partition called `name` in the table."""
    for line in open(partitions_csv, encoding="utf-8", errors="replace"):
        line = line.split("#", 1)[0].strip()
        if not line:
            continue
        cols = [c.strip() for c in line.split(",")]
        if len(cols) >= 5 and cols[0] == name:
            s = cols[4].upper()
            mult = 1024 * 1024 if s.endswith("M") else 1024 if s.endswith("K") else 1
            return int(s.rstrip("MK"), 0) * mult
    raise SystemExit(f"no partition '{name}' in {partitions_csv}")


def psram_static(map_file):
    """Size of .ext_ram.bss from the linker map (esp_idf_size doesn't report it for the P4)."""
    rx = re.compile(r"^\.ext_ram\.bss\s+0x[0-9a-fA-F]+\s+(0x[0-9a-fA-F]+)")
    for line in open(map_file, encoding="utf-8", errors="replace"):
        m = rx.match(line)
        if m:
            return int(m.group(1), 16)
    return 0


def main():
    build = sys.argv[1] if len(sys.argv) > 1 else "build"
    root = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
    maps = [f for f in os.listdir(build) if f.endswith(".map")]
    if len(maps) != 1:
        raise SystemExit(f"expected one .map in {build}, found {maps}")
    map_file = os.path.join(build, maps[0])
    out = subprocess.run([sys.executable, "-m", "esp_idf_size", "--format", "json", map_file],
                         check=True, capture_output=True, text=True).stdout
    size = json.loads(out)
    bin_file = map_file[:-4] + ".bin"
    image = os.path.getsize(bin_file) if os.path.exists(bin_file) else size["total_size"]
    csv = os.path.join(root, "partitions.csv")
    slot = partition_size(csv, "system")
    rec_slot = partition_size(csv, "recovery")
    rec_bin = os.path.join(build, "recovery", "nucleo-recovery.bin")
    rec_image = os.path.getsize(rec_bin) if os.path.exists(rec_bin) else 0

    rows = [
        ("internal_static", "Internal RAM, static", size["used_diram"], BUDGETS["internal_static"]),
        ("internal_bss", "Internal RAM, .bss", size["diram_bss"], BUDGETS["internal_bss"]),
        ("psram_static", "PSRAM, static (.ext_ram.bss)", psram_static(map_file), BUDGETS["psram_static"]),
    ]
    pct = 100.0 * image / slot
    failed = []
    lines = ["| Budget | Used | Limit | Headroom | |", "|---|---:|---:|---:|---|"]
    for key, label, used, limit in rows:
        ok = used <= limit
        failed += [] if ok else [label]
        lines.append(f"| {label} | {used:,} B | {limit:,} B | {limit - used:,} B | {'ok' if ok else '**OVER**'} |")
    ok = pct <= BUDGETS["image_pct"]
    failed += [] if ok else ["System image"]
    lines.append(f"| System image vs `system` slot | {image:,} B ({pct:.1f}%) | {BUDGETS['image_pct']:.0f}% of {slot:,} B "
                 f"| {BUDGETS['image_pct'] - pct:.1f} pt | {'ok' if ok else '**OVER**'} |")
    if not rec_image:
        failed.append("Recovery image missing")
    rpct = 100.0 * rec_image / rec_slot
    ok = 0 < rpct <= BUDGETS["recovery_pct"]
    failed += [] if ok else ["Recovery image"]
    lines.append(f"| Recovery image vs `recovery` slot | {rec_image:,} B ({rpct:.1f}%) | {BUDGETS['recovery_pct']:.0f}% of "
                 f"{rec_slot:,} B | {BUDGETS['recovery_pct'] - rpct:.1f} pt | {'ok' if ok else '**OVER**'} |")

    report = "\n".join(lines)
    print(report)
    summary = os.environ.get("GITHUB_STEP_SUMMARY")
    if summary:
        with open(summary, "a", encoding="utf-8") as f:
            f.write("### Firmware memory budgets\n\n" + report + "\n")
    if failed:
        print(f"\nOVER BUDGET: {', '.join(failed)}. Move the new static data to PSRAM (NV_PSRAM_BSS) or "
              "allocate it per app open; raise a budget in tools/ci/check_budgets.py only on purpose.")
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
