# Firmware updates (flash layout v2)

NucleoOS updates itself over Wi-Fi from GitHub Pages. An update is prepared on the microSD card and
installed by a small, separate **recovery** app, so the system has 10 MB of flash instead of the 4.5 MB
an A/B scheme would leave. Around that sits a safety net whose one job is that **no device is ever
lost**: not to a bad release, a power cut, a broken SD card, or a lost signing key.

## Flash map (16 MB, `partitions.csv`)

| Partition | Offset | Size | Contents |
|---|---|---|---|
| `nvs` | 0x9000 | 24 KB | settings (unchanged position: they survive every reinstall) |
| `otadata` | 0xF000 | 8 KB | which app boots, and its verify state |
| `phy_init` | 0x11000 | 4 KB | RF calibration |
| `recovery` (app, ota_1) | 0x20000 | 1 MB | NucleoOS Recovery (`recovery/`), about 0.75 MB used |
| `system` (app, ota_0) | 0x120000 | 10 MB | NucleoOS, about 5.7 MB used |
| `assets` (data 0x40) | 0xB20000 | 4.6 MB | journal + **safety copy** of the previous system (below) |
| `coredump` | 0xFC0000 | 256 KB | last crash, for `tools/decode-coredump.ps1` |

Why recovery is `ota_1` and not `factory`: a blank `otadata` (fresh flash) boots `system` directly,
and when a new system fails its probation the bootloader's own rollback lands in recovery.

The P4 executes code only from flash (XIP), so the SD card can hold images but never run them.

### `assets` layout (`components/nv_fwup/include/nv_fwup_policy.h`)

| Offset | What |
|---|---|
| 0x0000, 0x1000 | **journal**, two sectors written alternately (seq + CRC): a power cut mid-write leaves the previous record. System → recovery: rescue request, install flags. Recovery → system: what happened (result), power-cut retry count, "nothing to restore" mark. Works without an SD card. |
| 0x2000 | **LKG header** (version, raw/stored size, both SHA-256, CRC), written *last* |
| 0x3000… | **LKG data**: the system image that was running before the last update, raw-deflated (≈58 % of the image) by the P4 ROM's tdefl |

## Update flow

```
NucleoOS (system, confirmed)              SD card /nvupd/                 Recovery (ota_1)
----------------------------              ---------------                 ----------------
fetch ota/v2/manifest.json
verify ECDSA signature (primary OR backup key)
rollout bucket reached? (hands-free only)
download image  -------------------------> dl.bin
hash == signed sha256/size
image still embeds our release key?
copy running system  --------------------> prev.bin + prev.jsn (2nd rollback source)
journal: install flags (hands-free -> needs a safety copy)
rename dl.bin  --------------------------> next.bin
write signed manifest  ------------------> next.jsn   ("go")
boot -> recovery, restart
                                                                          next.jsn present:
                                                                            verify signature again
                                                                            save the running system into the
                                                                              LKG (compress, re-read, test-
                                                                              decompress, hash, header last)
                                                                            hash next.bin, check it trusts the key
                                                                            write `system`, read back, hash
                                                                            check version in app descriptor
                                          cur.jsn <--- next.jsn
                                          journal + result.jsn <-------    "install ok"
                                                                            boot -> system (NEW)
system boots PENDING_VERIFY = probation (below)
  proves itself         -> confirmed; then writes the recovery it carries if the slot differs
  dies / freezes / can't reach the server -> recovery restores the LKG (or prev.bin)
NucleoOS reads the journal -> notification; a rolled-back version is not offered hands-free again.
```

## Never lose a device

Each failure mode, and what catches it:

| What goes wrong | What happens | Code |
|---|---|---|
| Bad download, corrupt card, wrong image | refused before the slot is touched (signature, SHA-256, size, app-descriptor version) | `nv_fwup_install` |
| Image that would not trust our key (test build, key mix-up) | refused by `ota_sign.py` at signing time **and** by device + recovery (`NV_FWUP_E_KEYS`) | `nv_fwup_file_trusts_us` |
| Power cut while recovery writes | `next.jsn` and `otadata` still point at recovery: the install restarts (3 attempts) | `recovery.cpp run_update` |
| Power cut while the safety copy is written | header is erased first and written last: "no copy", never a half one | `nv_fwup_lkg_save` |
| New image crashes at boot | dies on probation → bootloader marks it ABORTED → recovery restores the LKG | bootloader + `restore()` |
| New image boots but the UI freezes | probation sees the UI probe fail for 60 s → marks the image invalid → recovery restores | `nv_fwup_policy::probation` |
| New image breaks Wi-Fi / TLS / HTTP (the update path) | online (DNS resolves) but the server never answers in 10 min → rolled back while it still can be | same |
| User unplugs during the first minutes, brownout | not the image's fault: recovery boots it again (2 retries), then restores | `on_aborted` |
| Image crashes later, after being confirmed | crash streak in RTC memory: 3 crashes → **safe mode** (UI, network, web console, updates only; no audio, USB, BT, ANIMA services, MQTT); 5 → recovery restores the LKG | `next_streak`, `boot_mode`, `app_main.cpp` |
| No safety copy anywhere | stay in safe mode: it keeps the update path alive, so the next release can fix it | `rescue_source` |
| SD card missing or dead | LKG and journal are in flash: rollbacks work without a card (updates still need one) | `assets` |
| Recovery itself crashes (card that hangs FAT) | after 3 deaths in a row it stops touching the card and works from flash | `recovery.cpp Guard` |
| A bug in recovery | every confirmed system rewrites the `recovery` slot with the recovery it was built with | `nv_ota sync_recovery` |
| Nothing bootable at all | repair screen; a signed `nucleos-anima.bin` + `.json` in the card's root is installed as soon as it appears; or the web flasher | `repair_screen` |
| Primary signing key lost or leaked | every firmware also trusts the **backup key** (kept offline): a release signed with it reaches every device and can move them to a new key | `ota_signing_pub_backup.pem` |
| A bad release reaches everyone | **staged rollout**: `dist.py firmware --rollout 10`, widen with `dist.py rollout 50/100`, stop with `dist.py rollout 0`; a **beta channel** before stable | `rollout_bucket`, `dist.py` |
| A hands-free update with no way back | recovery refuses it (journal install flag) and keeps the working system | `kInstallNeedSafetyCopy` |
| An update armed while the image is still on probation | refused: the safety copy would be of an unproven image | `install_blocker` |

### Probation (`nv_fwup_policy::probation`)

A fresh image (otadata `PENDING_VERIFY`) is confirmed only when **all** of these hold:

- it has run at least 60 s (the 1.1.57 lesson: marking valid at boot turned a boot loop into a brick);
- the UI answers its probe (`lvgl_port_lock` within 1 s); `nv_ota_expect_ui()` is called as soon as the
  display works, so a boot that hangs before the UI is up fails too;
- the update server answered (any HTTP status over TLS) — or the device is plainly offline (the server's
  name does not resolve) after 180 s.

It is rolled back when the UI is silent for 60 s, or when it is online but the server never answers
within 10 minutes. A rolled-back version is marked `ota_bad`: not installed hands-free again (a manual
"Install" in Settings still can).

### Crash streak and safe mode

`nv_ota_early_boot()` is the first call in `app_main`. It classifies the reset (`nv_fwup_reset_kind`):
crashes and brownouts count, a deliberate restart neither counts nor clears, a cold start clears. The
streak lives in RTC memory, keyed to the firmware version; 180 s of normal uptime clears it.

Safe mode is left with **Settings → Update → Restart normally**, `update normal` in the terminal, or a
power cycle. Restarting from the menu keeps it (the crash cause is still there).

### Staged rollout and channels

- Manifest field `"rollout"` (0..100, default 100) — not signed: it only decides *when* a signed release
  installs itself. Each device computes `rollout_bucket(MAC, version)` locally (nothing identifying
  leaves it); hands-free install and the "update available" notice need `bucket < rollout`. A manual
  check in Settings still offers the release.
- Channels: `ota/v2/manifest.json` (stable) and `ota/v2/beta/manifest.json` (beta). A device follows
  its channel (Settings → Update → Update channel, or `update channel beta`) unless a custom URL is set.

## Release keys

| Key | Where | Use |
|---|---|---|
| primary | `%USERPROFILE%\.nucleo\ota-signing-key.pem` (this PC) | signs every release |
| backup | **offline** (two USB sticks in a safe) — `ota-signing-key-backup.pem` | only if the primary is lost or leaks |

Both public halves are compiled into every system and recovery (`components/nv_fwup/*.pem`). A key
change: sign a release with the backup key whose firmware embeds the new primary (and keeps the
backup), let it reach the fleet, then sign with the new key. `python tools/ota_sign.py fingerprint`
shows which keys this PC holds and which the firmware trusts. **Never** regenerate a key that devices
trust (`keygen` / `keygen-backup` refuse to overwrite).

## Requirements and messages

- A microSD card with about 20 MB free (the new image, the card rollback copy, slack). Without a card,
  Settings → Update says so and nothing is downloaded; the system keeps working.
- Settings → Update shows the probation / confirmed state, the safety copy, the recovery version, the
  crash streak and the channel; so does `update status` in the terminal (`tools/nsh.py "update status"`).

## Channels on Pages

| Manifest | For | Notes |
|---|---|---|
| `ota/v2/manifest.json` | layout v2 boards, stable | `ota/v2/<version>.json` per release (the signed manifest of what runs) |
| `ota/v2/beta/manifest.json` | layout v2 boards on the beta channel | never the web flasher, no main-repo release |
| `ota/manifest.json` | layout v1 boards | frozen on the last v1 build, which tells the user to reinstall |

A v1 board can't change its partition table over the air: one reinstall from the
[web flasher](https://indecenti.github.io/nucleoos-p4-store/flash/) ("keep data" preserves settings).

## Publishing

```
python tools/dist.py firmware --channel beta                 # beta devices first
python tools/dist.py promote --rollout 10 --build build      # stable, 10 % of devices
python tools/dist.py rollout 50                              # widen
python tools/dist.py rollout 100
python tools/dist.py rollout 0                               # HALT: nobody else installs it hands-free
python tools/dist.py status                                  # what each channel serves, rollout, signature
```

A halted or bad release is replaced by publishing a fixed, higher version: devices that took the bad
one either rolled back already or are in safe mode, where updates keep working.

## Code map

| Where | What |
|---|---|
| `components/nv_fwup/nv_fwup_policy.*` | pure decisions + records (probation, streak, retries, rollout, journal, LKG header) — host-tested |
| `components/nv_fwup/nv_fwup_codec.*` | streaming deflate/inflate of the safety copy (ROM miniz) — host-tested and fuzzed |
| `components/nv_fwup/nv_fwup_safety.cpp` | `assets` partition: journal, LKG save / restore |
| `components/nv_fwup/nv_fwup.cpp` | signed manifests (two keys), verified install / backup, key-trust check |
| `components/nv_ota/` | NucleoOS side: check, download, stage, probation guard, crash streak, safe mode, recovery self-update, notices |
| `recovery/` | recovery app 2.x (own ESP-IDF project, no LVGL; built with the firmware, embedded in it) |
| `tools/ota_sign.py` | keys, signing (refuses images without the production key), fingerprints |
| `tools/dist.py`, `tools/dist_pages.yml` | publishing: channels, rollout, promote, per-version manifests, web flasher |
| `tools/ci/check_budgets.py` | CI: system vs `system`, recovery vs `recovery`, deflated image vs `assets` |
| `tests/host/unit/test_fwup.cpp`, `fuzz/fuzz_fwup.cpp` | policy, records, codec |

## Testing on the board

Test images must never be signed with the release key. A test build trusts a **test key** only and
carries a fault on purpose:

```
python tools/ota_test.py keygen                        # test key pair in %TEMP%, never committed
idf.py -B build_t -DNV_FWUP_PUBKEY=<test pub> -DNV_TEST_VERSION=1.9.1 build      # base, flashed over USB
idf.py -B build_t -DNV_TEST_VERSION=1.9.2 -DNV_OTA_FAULT=boot build             # dies 8 s after boot
                                     ... NV_OTA_FAULT=ui | late | net           # UI freeze / crash at 120 s / no server
```

Push the image and its test-signed `.json` to the card (`/api/fs/write`), `update sd`, `update restart`,
and read the outcome with `update status` and `GET /api/logs`. Scenarios:

1. Normal update: recovery saves the LKG, installs, the image is confirmed after the server answers.
2. `boot`: rolled back from the LKG with the card's `prev.bin` removed (flash-only rollback).
3. `ui`: rolled back after 60 s of a silent UI.
4. `net`: rolled back after 10 min online without reaching the server.
5. `late`: safe mode after 3 crashes, LKG restored after 5.
6. Power cut during probation (`esptool --after hard_reset` / unplug): the image is retried, not dropped.
7. Recovery self-update: a system with a newer embedded recovery rewrites the slot once confirmed.

Afterwards: flash the production build over USB and clear the test LKG (`update status` must not show a
test version as the safety copy; `esptool erase_region 0xB20000 0x3000` wipes journal + header).
