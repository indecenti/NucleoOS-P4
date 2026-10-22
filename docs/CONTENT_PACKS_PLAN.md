# Downloadable system content — plan

Status (2026-10-06): **F0-F5 done**. Content published (store `content/`, release `content-2026.10.1`); firmware 1.2.66 tested on the board.

- F0: plaintext `teacher.json` removed from the `sd/` mirror (moved to `%USERPROFILE%\.nucleo\anima-teacher.json`);
  `sync-sd.ps1` / `verify-sd.ps1` never touch the user's files; `tools/content/build.py` builds the
  packages from an allowlist and stops on any credential (even the Gemini `AQ.` format), without ever
  printing it; licenses inside every package. Test: `python tools/content/test_build.py`.
- F1: `nucleoos-data-v2` format (`nv_store_pkg`), index/ustar/journal (`nv_store_tree`), installation
  with file-by-file verified extraction and atomic swap, recovery at startup (`app_main`), 64 MB OTA
  reserve also for the `wiki-*`, store backup key (`store_signing_backup_pub.pem`, private one to be
  moved offline), `url2` mirror. The 5 store defects fixed. Tests: `tests/host` unit `content`
  (190 checks, 64/32 bit, including an archive made by Python read by the device code) +
  `content` fuzzer; firmware build and memory budget OK.
- F2: `nv_content` service (persistent queue, waits for SD / safe mode / OTA confirmation (`nv_ota_confirmed`)
  / OTA in progress / store / network, backoff 1-5-15-60 min, automatic update ONLY of installed
  packages older than `kRequired`, never over a web modified with `/api/web/put`), index
  `content/index-v1.json` (names in 5 languages, written by `build.py`), verification and repair with the signature
  kept on the SD, adoption of trees already present without downloading them, cleanup of `.part` files older than 24 h,
  ANIMA releases and reopens the files around the swap, the web cache is rebuilt after an update,
  `GET/POST /api/content`, shell command `content`, G6 in `tools/dist.py` (refuses a firmware that requires
  an unpublished version). Decision logic tested on the PC (`nv_content_plan`).
- F3: "Offline content" step in the wizard (after Wi-Fi, one tap, "Customize"), Settings >
  System content (status card + each package with its action), note when something is missing,
  fallback page in place of the web 404, the Store does not show the `dict-*` twice, 32 strings in 5 languages.
- F4: `tools/dist.py content` (assets in the release + only `content/` of the store), `build.py --reuse` (files
  already published by the v1 `dict-*` packages are not uploaded again: same sha, v1 and v2 adopt each other),
  `--only` (one package without changing the version of the others), `--index-only` (only the index texts).
  Published: 7 packages 2026.10.1, `dict-it` 2026.10.2 (store `7220003`, `959ee81`.., `244553c`).
- F5, verified on the board (test builds 1.2.62-1.2.66, installed from SD):
  - G4: during probation the service waits (`waiting=ota`), loads the list right after confirmation.
  - recommended set (web, ANIMA, it/en dictionaries, 139 MB) installed in 6 min 45 s; adoption of files already
    present (`dict-fr` in 24 s, `akb5` "already matches its index, kept").
  - G9: "swap" journal simulated before the reboot -> completed at boot (`0x4`), journal deleted.
  - verification: a modified web file and a damaged AKB5 shard found; repair with a real folder
    swap, ANIMA releases the files and reloads ("offline data reloaded"), web cache rebuilt (305 files).
  - G12: `/api/web/put` turns on `web_local`; an explicit installation turns it off.
  - resume after a reboot in the middle of a download ("resumes at 3145728/8579382 bytes").
  - wizard: language first, Content step in Italian and Spanish, recommendations per language, first
    keyboard focus on "Download", Enter starts the download; live language change of the package names.
  - fixed during the tests: slow extraction (unaligned SD buffers: 83 MB from ~450 s to <=170 s), empty web
    cache after the update, resume lost after a reboot (fsync), names in the wrong language,
    plurals, index accents.
  - also tested (1.2.67-1.2.69): web fallback page without `web/` (and its button from an unpaired
    browser), "content is missing" notification opening the page directly, web installed from scratch
    where the folder did not exist, `skills` seed (the user's edit stays, a deleted file comes back),
    "Customize" (live selection and total), full SD with the `cfg cnt_reserve_mb 99999` lever (refusal
    before downloading, no half files, translated message, restart as soon as there is space).
  - fixed in this round: wizard list too tall with 4 packages (now one row per package), "Customize"
    total stuck, duplicated English message under the translated one, NVS key too long
    (max 15 characters: `cnt_reserve_mb`).
- Dropped: the "complete" SD zip in the release. The board downloads by itself and adopts files already present
  (signed index), so it is not needed; the README now describes the wizard step. Release: **1.2.66**
  (HIL `ota_safety.py` retry/rescue/boot/ui/net/late 6/6 PASS before publication).
- Excluded for now: `drivers-win` (third-party Windows drivers, redistribution rights to be clarified).

User decisions (2026-10-06):
- TTS voice **postponed**: no `voice-*` package in the first release (the ids stay reserved).
- AKB5 and ANIMA encoder **publishable** on public GitHub, with `LICENSES.txt`.
- **One-tap** wizard: recommended set preselected ("Download · ~140 MB") + "Customize".
- After an OTA, **only the packages required** by the firmware version (e.g. `sys-web`) are updated **automatically**;
  other updates are reported in Settings > System content.

## The problem

Whoever flashes NucleoOS from the web flasher gets a half-working board and does not know it:

| SD content | Size | How it arrives today | Without it |
|---|---|---|---|
| `web/` (web companion) | 8 MB (3.7 MB zip) | zip in the release, to be unpacked by hand from the PC | `/` answers 404 |
| `data/anima` L1: encoder + AKB5 (62 shards) + facets | ~87 MB | **never** (only from this PC, gitignored) | ANIMA without the semantic level |
| `data/anima` it/en/es/fr/de dictionaries | ~100 MB | **never** | "word not in the offline dictionary", no translations |
| `data/tts/<lang>` | 3 MB per language | **never** | no voice, only a log line |
| `nucleos/drivers/windows/*.exe` | 2.3 MB | **never** (exe gitignored) | USB Second Screen without drivers |
| `data/anima/kb/*.akb6` (Wikipedia) | 23-50 MB per language | Store, but only if the user knows | ANIMA without encyclopedic facts |

On the device there is no check and no message: features disappear silently.

## Decisions

### Where the files live

- **Sources** (web, skills, build scripts, licenses): in the `NucleoOS-P4` repo, as today. The large binaries
  (AKB5, dictionaries, voices) **do not go in git**, but the way to rebuild them does (`tools/content/`).
- **Distribution**: in the **store** (`nucleoos-p4-store`), with the same mechanism already used and verified
  on the Wikipedia packages:
  - `data/<id>/pack.sig` on Pages: list of files with sha256, size and URL, signed with the store key.
  - payload as **assets of a GitHub release** (`content-2026.10`, …): up to 2 GB per file, CDN,
    free, none of the Pages limits (1 GB per site, 100 MB per file).
- No Oracle server: it has little RAM and is not needed here.

Why the store and not the firmware release: signature, download with resume, sha check and
space check already exist there (`nv_appstore.cpp` `install_data`). Moreover the contents have their own
life: a dictionary is updated without a new firmware.

### Updatability guarantees (non-negotiable constraints)

The principle: **content must never be able to prevent a firmware update**, and no
existing device must notice that it exists until it has the firmware that knows about it.

| # | Risk found in the analysis | Constraint |
|---|---|---|
| G1 | Firmware already in the field reads `store2-<lang>.json`; a row with an unknown `kind` is treated as an **app** (`nv_appstore.cpp:717`) and the installation fails. The new destinations (`web`, `tts`) are rejected by their parser. | System packages **do not enter** the `store*.json` catalogs. Separate index `content/index-v1.json`, read only by the new firmware. The `wiki-*` stay where they are and in v1 format. |
| G2 | A new `pack.sig` format (`tar` rows) would break the old parsers. | New domain `nucleoos-data-v2`, used only by the packages of the new index. v1 stays unchanged and supported forever. |
| G3 | **The OTA downloads the image onto the SD** and requires image + half slot + 4 MB free (`nv_ota.cpp:478`, about 20 MB). Filling the SD with content would block updates. | Every installation leaves a free **OTA reserve** (64 MB, computed by the same formula multiplied by 2). If space is missing, the content is not installed, never the opposite. No `.part` file left occupying space for more than 24 h without progress. |
| G4 | During **probation** the new firmware must prove it can reach the update server ("net"); a heavy parallel download can make it fail and cause a needless rollback. | `nv_content` starts only with the image confirmed (`s_confirmed`, new API `nv_ota_confirmed()`), never in safe mode, never while `nv_ota_busy()`. |
| G5 | Two parallel TLS connections have already caused SRAM starvation and esp_hosted crashes. | Only one heavy network job at a time: contents use the store worker (already serial) and give way to the OTA. |
| G6 | A firmware requiring `sys-web >= X` published **before** X would send every device into a retry loop. | `dist.py firmware` refuses to publish if the required packages are not already on the store with a valid signature. If they are missing anyway: the device works, shows the fallback page and retries with backoff. |
| G7 | Rollback to an older firmware (LKG) with a newer web on the SD. | The update path is **native only** (Settings + recovery) and never depends on files on the SD. The web declares the APIs it uses and degrades if `/api/info` is older; never an automatic content downgrade. |
| G8 | A malformed content file read at startup could crash every boot. | Only files with a verified sha are activated; the consumers (ANIMA L1, web cache) already check the headers; in safe mode they are not read. The case is tested on purpose in the HIL tests. |
| G9 | Power loss during the folder swap (`web` → `web.old`). | Journal `/sdcard/nucleos/content.swap` written before the swap; at startup it is completed or cancelled. Never a missing folder. |
| G10 | ANIMA and the web cache keep files open while they are replaced. | Hooks before and after the swap: ANIMA closes and reopens L1 and the dictionaries (as it already does for the `wiki-*`), nv_web rebuilds the PSRAM cache. |
| G11 | Hand-prepared cards (the user's own, the testers'): 140 MB downloaded again for nothing, or files overwritten. | **Adoption**: files present with the right sha → only the `.pack` record is written. Different sha → "different version", never overwritten automatically. |
| G12 | On the development board the web is updated with `/api/web/put`: the automatic update would erase work in progress. | `/api/web/put` marks the web as "locally modified": the automatic update pauses and Settings shows it. |
| G13 | The `skills/` are editable by the user. | Distributed as "seed" files: copied only if absent, never overwritten. |
| G14 | Dependence on a single store key and a single host. | The firmware accepts the store key **and** a **backup key** (decided 2026-10-06: the private one is to be kept offline by the user). `pack.sig` v2 allows up to 2 URLs per file; for now only GitHub, the mirror is added without OTA. |

### How it is downloaded

Three entry points, one engine (`nv_content`):

1. **Wizard**: a "Content" step after Wi-Fi.
2. **Settings > System content**: for those who skipped the step, for those who change language and for repair.
3. **At the moment of need**: the feature that does not find its files says so and offers the download
   ("Italian voice not installed · Download 3 MB").

The download **continues in the background**: the user finishes the wizard and uses the board. Progress is in
the shade and in the status icon. It resumes by itself after a reboot or a Wi-Fi drop.

### "Do we offer it only if the store responds?"

The step always appears if Wi-Fi is connected, but the content changes with the state:

| State | What the user sees |
|---|---|
| Store reachable, SD ok | recommended packages already ticked, total size, free space, **Download the recommended** / Later |
| Store not reachable | "The content server is not answering now. The board downloads everything by itself as soon as it is back." + Retry / Continue |
| No SD or full SD | "Insert a microSD card (FAT32) with at least 1 GB free." with real-time checking |
| Wi-Fi skipped | the step does not appear; reminder in Settings and notification on first connection |

Never an empty list or a technical error. If the download does not start in the wizard, the service retries by itself
and sends **one** notification when it has finished (or if it needs something from the user, for example space).

## Packages

Small, per-language packages, so that only what is needed is downloaded. The names below are provisional.

| id | Content | Size | Recommended |
|---|---|---|---|
| `sys-web` | web companion | ~4 MB | always |
| `anima-core-it` | encoder, AKB5 + shards, it/en facets, `commands.it.json` | ~87 MB | always (ANIMA uses the Italian encoder for all languages) |
| `dict-it` | lex-it, forms-it, dict-it-en, dict-en-it | ~31 MB | if language it |
| `dict-en` | lex-en, forms-en | ~18 MB | always (English = bridge language) |
| `dict-es` / `dict-fr` / `dict-de` | dict-xx-en, dict-en-xx, forms-xx | 12-26 MB | if that language |
| `voice-it` / `voice-en` | `tts/<lang>/{index.bin,clips.pcm}` | ~3 MB | **postponed** |
| `drivers-win` | Second Screen Windows drivers | ~2 MB | no (only from Settings or from the Second Screen page) |
| `wiki-<lang>-top` | Wikipedia (already exists) | 23-50 MB | proposed, not ticked |

Recommended total for an Italian user: about 140 MB.

**Never** in the packages, with an allowlist instead of a denylist: `teacher.json` (contains a real API
key), `workspace.json`, `learned/` written by the device, `session.txt`, `conv/`, `memory.jsonl`, profiles,
rules, timers. Neither do the useless files the firmware does not open: `tts/*_final`, `tts/*_merged`,
`anima-it-encoder.json`, `anima-it-index.bin` (fallback replaced by AKB5).

## Work

### F0 — Cleanup and security (first of all)

- Remove the plaintext `teacher.json` from the `sd/` mirror: `sync-sd.ps1` today copies it over the
  encrypted copy on the card. Only `teacher.json.example` remains.
- `tools/content/manifest.py`: explicit allowlist per package, with a check that fails if it finds
  keys or tokens (`"key":`, `sk-`, `AIza`…).
- Licenses: a `LICENSES.txt` file inside each package (Wiktionary CC BY-SA 4.0, WordNet CC BY 4.0, …).
  CC BY-SA requires attribution and the same license: the package declares it.

### F1 — Data package engine (nv_appstore / nv_store_pkg)

- New allowed destinations: `web`, `tts`, `anima`, `anima/kb`, `nucleos/drivers`.
- **Archives**: an entry of type `tar` (uncompressed ustar; the web `.gz` files are already compressed) is
  extracted into `<dest>.new/` and then **swapped atomically** with the old folder
  (`web` → `web.old`, `web.new` → `web`, deletion of `web.old`). Needed for web (547 files) and AKB5
  (62 shards): the 64-row limit of `pack.sig` stays, and a half package never breaks the
  installed one.
- Fix the defects found in the analysis, because we rely on this code:
  1. `store remove wiki-*` from the shell says "removed" but removes nothing (`term_sh.cpp:2509`).
  2. Data package updates are never reported (`abi` 99 is discarded, `nv_appstore.cpp:1651`).
  3. A complete but corrupt `.part` blocks the package forever (`nv_appstore.cpp:513-533`).
  4. The development server does not serve `/data/<id>/pack.sig`.
  5. Every export re-signs all the `pack.sig` (useless differences in the store repo).
- Host tests: `pack.sig` parser and tar extractor under libFuzzer (`../` path traversal, long names,
  truncated files), like the other fuzzers in `tests/host`.

### F2 — `nv_content` service

- **Content manifest compiled into the firmware**: which packages exist, which are recommended per
  language and the **minimum version** this firmware requires (for example `sys-web >= 2026.10.3`).
  After an OTA that requires a newer web, the service updates `sys-web` by itself: the web companion always stays
  aligned with the firmware, and it is today's most fragile part (there is no version check).
- State per package: `Installed` / `Not installed` / `Update available` / `Needs repair` / `Downloading x%`.
- Persistent queue on SD (`/sdcard/nucleos/content.queue`): resumes after a reboot, waits for Wi-Fi
  (`net_up`) and the store, with backoff (1, 5, 15, 60 min).
- On-demand verification: recomputes the sha256 of the installed files and compares with the signed `.pack`.
- API:
  - C: `nv_content_status`, `nv_content_install`, `nv_content_verify`, `nv_content_need(feature)` for the notices.
  - REST: `GET /api/content`, `POST /api/content/install|verify`.
  - Shell: `content status|install|verify|repair`.
  - ANIMA (caps): knows what is missing and can say so ("I don't have the French dictionary, do you want to download it?").

### F3 — Interface

- **Wizard**: step `ST_CONTENT` after `ST_WIFI`, with the states of the table above. There is a single main
  button ("Download · 140 MB"); "Customize" opens the list with the checkboxes.
- **Settings > System content** (new entry in the rail):
  - card at the top with the general state ("Everything is installed" / "Missing: 3 · Download all").
  - list per package: name, size, version, state, Download / Update / Remove.
  - Check and Repair.
  - changing language offers the dictionary for that language.
- **Notices at the moment of need**: missing dictionary or L1 (ANIMA), missing driver (Second Screen); voice when the voice packages exist.
  One row with the Download button, never a technical error.
- **Web companion without `web/`**: instead of the 404 a small page compiled into the firmware: "The web companion
  is not installed yet · Install now" (calls `/api/content/install`).
- Keyboard and mouse on everything, per the project rule (skill keyboard-support).

### F4 — Publication

- `tools/content/build.py`: builds the packages from `sd/` and from the builders' caches, version `YYYY.MM.N`.
- `tools/dist.py content`: uploads the assets to the `content-*` release and writes only the `data/<id>` rows of the
  store (targeted graft: a full export drags along the unpublished apps of other sessions).
- Firmware release: `nucleoos-p4-sdcard.zip` becomes **the entire recommended set**, for those without
  Wi-Fi or who prefer to prepare the SD from the PC. The device recognizes it as installed, because each
  package carries its own `.pack` record.
- README and web flasher page: "At first boot the board downloads by itself what it needs".

### F5 — Hardware verification (before any public OTA)

Factory reset + empty SD, then:

1. Complete wizard: background download, check of ANIMA L1, dictionaries and web companion.
2. Wi-Fi disconnected mid-download: resumes.
3. Reboot mid-download: resumes from the queue.
4. Full SD: clear message, no half files.
5. No SD.
6. Unreachable store (fake `store_url`): message, then automatic recovery.
7. Corrupt `.part`: downloaded again.
8. OTA requiring a newer `sys-web`: automatic update.
9. Language change: offer of the packages for that language.
10. Check and Repair after corrupting a file by hand.
11. **Updatability** (G1-G14):
    - an old firmware (1.2.58) with the new store published: catalog, apps and `wiki-*` unchanged;
    - almost full SD: contents stop at the reserve, the next OTA goes through;
    - new OTA in probation: no download until the image is confirmed;
    - power cut during the swap of the `web` folder;
    - LKG rollback to an old firmware with a new web on the SD: update from Settings working;
    - hand-prepared card: adoption without download;
    - `/api/web/put` on the development board: no automatic update over it.

Plus the host tests (tar and `pack.sig` fuzz) and the `tools/hil/smoke.py` smoke. OTA release >= 1.2.61.
