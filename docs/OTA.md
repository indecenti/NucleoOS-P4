# Firmware updates (flash layout v2)

NucleoOS updates itself over Wi-Fi from GitHub Pages. From layout v2 on, an update is prepared on the
microSD card and installed by a small, separate **recovery** app. That frees the second firmware slot
the old A/B scheme needed: the system now has 10 MB of flash instead of 4.5 MB.

## Flash map (16 MB, `partitions.csv`)

| Partition | Offset | Size | Contents |
|---|---|---|---|
| `nvs` | 0x9000 | 24 KB | settings (unchanged position: they survive every reinstall) |
| `otadata` | 0xF000 | 8 KB | which app boots, and its verify state |
| `phy_init` | 0x11000 | 4 KB | RF calibration |
| `recovery` (app, ota_1) | 0x20000 | 1 MB | NucleoOS Recovery (`recovery/`), about 0.7 MB used |
| `system` (app, ota_0) | 0x120000 | 10 MB | NucleoOS, about 4.2 MB used |
| `assets` (data 0x40) | 0xB20000 | 4.6 MB | reserved: read-only data mapped straight from flash |
| `coredump` | 0xFC0000 | 256 KB | last crash, for `tools/decode-coredump.ps1` |

Why recovery is `ota_1` and not `factory`: a blank `otadata` (fresh flash) boots `system` directly,
and when a new system fails its survival gate the bootloader's own rollback lands in recovery.

The P4 executes code only from flash (XIP), so the SD card can hold images but never run them.

## Update flow

```
NucleoOS (system)                         SD card /nvupd/                 Recovery (ota_1)
-----------------                         ---------------                 ----------------
fetch ota/v2/manifest.json
verify ECDSA signature  ------------>
download image  -------------------------> dl.bin
hash == signed sha256/size
copy running system  --------------------> prev.bin + prev.jsn (rollback)
rename dl.bin  --------------------------> next.bin
write signed manifest  ------------------> next.jsn   ("go")
boot -> recovery, restart
                                                                          next.jsn present:
                                                                            verify signature again
                                                                            hash next.bin
                                                                            write `system`, read back, hash
                                                                            check version in app descriptor
                                          cur.jsn <--- next.jsn          
                                          result.jsn <------------------    "install ok"
                                                                            boot -> system
system boots PENDING_VERIFY
  survives 60 s -> marked valid
  dies sooner   -> bootloader marks it aborted, starts recovery
                                                                          system aborted:
                                                                            install prev.bin (same checks)
                                          result.jsn <------------------    "rollback"
NucleoOS reads result.jsn -> notification, and a rolled-back version is not
offered again automatically (manual "Install" still allowed).
```

### Guarantees

- **Nothing unsigned reaches `system`.** Recovery re-verifies the signature and the image hash itself;
  it does not trust what NucleoOS checked. The rollback copy needs its own signed manifest too.
- **The slot is erased only after the image file is proven correct.** A bad download or a corrupt card
  never costs the working system.
- **Power cuts are safe.** `next.jsn` stays until the install completes, and `otadata` points at
  recovery until then: the next boot simply starts the install over. After `NV_FWUP_MAX_TRIES` (3)
  failed attempts the update is dropped.
- **A system that does not start is replaced by the previous one.** If even that copy fails, recovery
  stops and shows the repair screen instead of looping.
- **Repair without a PC.** On the repair screen, copying the release's `nucleos-anima.bin` and
  `nucleos-anima.json` to the card's root is enough: recovery installs them as soon as they appear.

### Requirements and messages

- A microSD card with about 20 MB free (the new image, the rollback copy, slack). Without a card,
  Settings → Update says so and nothing is downloaded; the system keeps working.
- The rollback copy needs the signed manifest of the running version (`cur.jsn`). Recovery writes it
  after every install; a board flashed over USB fetches it from `ota/v2/<version>.json` and keeps it
  only if the flash really holds that released image (a local build gets no rollback copy).

## Channels

| Manifest | For | Notes |
|---|---|---|
| `ota/v2/manifest.json` | layout v2 boards | current; `ota/v2/<version>.json` per release |
| `ota/manifest.json` | layout v1 boards | frozen on the last v1 build, which tells the user to reinstall |

A v1 board can't change its partition table over the air. The last v1 build carries this firmware's
code: it runs normally, refuses updates, and shows (Settings → Update and a notification) that one
reinstall from the [web flasher](https://indecenti.github.io/nucleoos-p4-store/flash/) is needed.
Choosing "keep data" there preserves settings, since `nvs` does not move.

`nv_ota_get_url()` maps a saved v1 channel URL to the v2 one, so a v2 board never fetches v1 images.

## Code map

| Where | What |
|---|---|
| `components/nv_fwup/` | shared core: signed manifests, verified install/backup, `nvupd/` file names |
| `components/nv_ota/` | NucleoOS side: check, download, stage, arm recovery, survival gate, notices |
| `recovery/` | recovery app (own ESP-IDF project, no LVGL; built and flashed with the firmware) |
| `tools/gen_recovery_assets.py` | recovery's font (Montserrat, OFL) and logo as C arrays |
| `tools/dist.py`, `tools/dist_pages.yml` | publishing: channel by layout, per-version manifests, web flasher |
| `tools/ci/check_budgets.py` | CI: system image vs `system`, recovery image vs `recovery` |

## Testing on the board

1. `idf.py -p COM5 flash` (writes bootloader, table, recovery and system).
2. Put a signed `nucleos-anima.bin` + `.json` of a newer version in the card's root, Settings → Update →
   Install from SD, restart: recovery shows the progress and boots the new version.
3. Pull the power during the recovery write: the next boot resumes it.
4. Rollback: install a build that crashes at boot; after the crash recovery restores the previous one
   and NucleoOS shows "did not start correctly".
