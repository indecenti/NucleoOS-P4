---
name: ota-release
description: Publish a NucleoV2 ESP32-P4 OTA firmware release for this project — bump the version, build with ESP-IDF, and publish bin+manifest (GitHub Pages distribution repo, plus the local PC server) so the board auto-updates on next boot. Use when the user wants to release/ship/publish firmware over the air, "fai un rilascio OTA", "aggiorna via OTA", "pubblica un update", or after code changes that should reach the board wirelessly. One script does everything; a second verifies the install over serial.
---

# OTA release (NucleoV2)

Firmware from 1.1.108 checks `https://indecenti.github.io/nucleoos-p4-store/ota/manifest.json`
on every boot (GitHub Pages, repo `indecenti/nucleoos-p4-store`) and self-updates when a
strictly-newer semver is offered. Older firmware polls the PC at `http://<PC-IP>:8080/manifest.json`
(see [[clock-and-ota]] in memory). A release = bump VERSION, build, publish. All of that is one
script — do NOT do the steps by hand (it wastes tool calls/tokens).

## Publish a release

Several sessions share `D:\NucleoV2`: first `git status` for files you didn't touch and agree the
version with the peers (SendMessage) — see [[multi-session-coordination]].

```
powershell -ExecutionPolicy Bypass -File .claude/skills/ota-release/scripts/release.ps1 -Notes "what changed"
```

Options:
- `-Version 1.2.0`  — explicit version (else auto-increments the patch of the CMake VERSION).
- `-Notes "..."`    — shown on the device update screen (default `"release X.Y.Z"`).
- `-Target github|local|both` — where to publish. Default `github`: release asset +
  `ota/manifest.json` via `tools/dist.py`, waits until Pages serves it (~20-60 s). `local` stages
  `ota_serve/` and starts the server on :8080, to test a build without publishing it (point the
  board at `http://<PC-IP>:8080/manifest.json` in Settings → Update); `both` does the two.
- `-Proj D:\nvXXX`  — bump + build an isolated worktree instead of the shared tree (peer WIP left
  out); `tools/dist.py` and `ota_serve/` still come from `D:\NucleoV2`.
- `-NoServe`        — local: don't (re)start the HTTP server.

It prints `PUBLISHED <ver> | bin=<bytes> | manifest=<url> | pages=live` (GitHub) and/or
`PUBLISHED-LOCAL <ver> | … | server=up` (local). Relay them. The board picks it up on its next boot
(or Settings → System update → Check). Then commit the VERSION bump right away.

Publishing only the image (e.g. built by hand in a worktree):
`python tools/dist.py firmware --bin <path>\build\nucleos-anima.bin --notes "..."` — the version
is read from the image. `python tools/dist.py status` shows what Pages serves.

Notes / gotchas:
- Manifests are written **BOM-free** (a BOM breaks the on-device cJSON parser).
- Version compare is **semver strict-greater**: publishing X.Y.Z makes every lower number
  unreachable for boards that already have it — never publish a number below a published one.
- The firmware image lives in a GitHub Release (`v<ver>`, asset `nucleos-anima.bin`); the Pages
  workflow copies it next to the manifest after checking its sha256, so the device downloads it
  from Pages with a plain 200 (no redirect).
- Local target: the manifest host is the PC's detected Wi-Fi IPv4; a board pointed at the PC
  needs that URL in Settings → System update.

## Verify the install (optional, over serial COM5)

Only when the user wants confirmation. This resets the board and prints a compact pass/fail — it
does NOT dump the whole boot log (keeps token use low).

```
powershell -ExecutionPolicy Bypass -File .claude/skills/ota-release/scripts/run_verify.ps1
```

Expected healthy output: `running <old> -> INSTALL`, `installed OK`, `App version: <new>`,
`marked valid`, `panics=0`. The SD-staged download of ~3.5 MB takes ~1–2 min on this card, so the
script waits up to ~200 s.

## When something's off
- `manifest unreachable` in the log → no Wi-Fi/Internet (GitHub), or PC server down / other
  network (local target).
- Board installs but reboots to the OLD version → a rollback; means the new image faulted before
  `nv_ota_init()` marked it valid. That call now runs first thing in `app_main` — keep it there.
- `dl: fopen ... EINVAL` → SD staging filename broke 8.3 (FATFS long names are off); keep it ≤8
  chars before the dot.
