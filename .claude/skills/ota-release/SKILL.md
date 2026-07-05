---
name: ota-release
description: Publish a NucleoV2 ESP32-P4 OTA firmware release for this project — bump the version, build with ESP-IDF, and host bin+manifest so the board auto-updates on next boot. Use when the user wants to release/ship/publish firmware over the air, "fai un rilascio OTA", "aggiorna via OTA", "pubblica un update", or after code changes that should reach the board wirelessly. One script does everything; a second verifies the install over serial.
---

# OTA release (NucleoV2)

The board auto-checks `http://<PC-IP>:8080/manifest.json` on every boot and self-updates when a
strictly-newer semver is offered (see [[clock-and-ota]] in memory). A release = bump VERSION,
build, drop the bin + a matching manifest into `ota_serve/`, keep the HTTP server up. All of that
is one script — do NOT do the steps by hand (it wastes tool calls/tokens).

## Publish a release

Run the release script. It bumps the patch version by default; pass `-Version` for an explicit one.

```
powershell -ExecutionPolicy Bypass -File .claude/skills/ota-release/scripts/release.ps1 -Notes "what changed"
```

Options:
- `-Version 1.2.0`  — explicit version (else auto-increments the patch of the current CMake VERSION).
- `-Notes "..."`    — shown on the device update screen (default `"release X.Y.Z"`).
- `-NoServe`        — don't (re)start the HTTP server (just build + stage files).

It prints one summary line: `PUBLISHED <ver> | bin=<bytes> | manifest=<url> | server=up`.
Relay that line to the user. The board picks it up on its next boot (or Settings → System update →
Check). Nothing else is needed on the PC side.

Notes / gotchas:
- Manifest is written **BOM-free** (a BOM breaks the on-device cJSON parser).
- Version compare is **semver strict-greater**, so the manifest version must exceed the running
  build or the board reports "up to date".
- The manifest URL host is the PC's detected Wi-Fi IPv4. If it differs from the value baked into
  the firmware's default (`192.168.0.216`), the script warns — the board only reaches the baked/
  saved URL, so either keep the PC at that IP or update the URL once in Settings → System update
  (it persists in NVS).

## Verify the install (optional, over serial COM5)

Only when the user wants confirmation. This resets the board and prints a compact pass/fail — it
does NOT dump the whole boot log (keeps token use low).

```
powershell -ExecutionPolicy Bypass -File .claude/skills/ota-release/scripts/run_verify.ps1
```

Expected healthy output: `running <old> -> INSTALL`, `installed OK`, `App version: <new>`,
`marked valid`, `panics=0`. The SD-staged download of ~1.9 MB takes ~1–2 min on this card, so the
script waits up to ~200 s.

## When something's off
- `manifest unreachable` in the log → PC server down (re-run release without `-NoServe`) or board
  on a different network than the PC.
- Board installs but reboots to the OLD version → a rollback; means the new image faulted before
  `nv_ota_init()` marked it valid. That call now runs first thing in `app_main` — keep it there.
- `dl: fopen ... EINVAL` → SD staging filename broke 8.3 (FATFS long names are off); keep it ≤8
  chars before the dot.
