# Offline library: ZIM reader

Status as of 2026-10-12:

- **F0 closed**: measurements on the board in §1b, test bench in `ports/zim/bench`.
- **F1 closed locally**: library in `ports/zim/nvzim`, results in §4b. The CI on GitHub will start at the first push.
- **F2 closed locally**: `zim` 1.1.0 is in the Store; ANIMA uses it since firmware `b07d75a` (results in §4c).
- **F3 closed**: the Library app 1.0.0 is in the Store (store `7303ad3`, Knowledge category); tested on the board
  with local firmware 1.2.113 (results in §4d).
- **F4 closed**: images and formulas in Library 1.1.0, in the Store since 2026-10-12 (store `fcca9e6`);
  update from the Store tested on the board (local 1.2.115; results in §4e).
- **F5 closed locally** (branch `f5dl` on top of `feat/office`): system downloader `nv_dl` (ABI 18) and the
  "Download collections" screen in Library 1.2.0. Tested on the board with local firmware 1.2.142, not yet public
  (results in §4g).

Goal: read Kiwix/openZIM ZIM files on the P4
(full Wikipedia, Wiktionary, Wikivoyage, medicine, iFixit, Stack Exchange, DevDocs, Gutenberg...) with a
graphical Store app, and give ANIMA and the Terminal the same access with a `zim` program.

A single reader opens **3,631 collections** of the Kiwix catalog, 382 in Italian, updated every month, without
us having to convert anything.

## 1. What we already know (verified, not hypotheses)

### The format, measured on real files

Probe on the files in `tools/kb/.cache` (`wikipedia_it_top_mini_2026-07.zim`, `wikipedia_en_top_mini_2026-09.zim`):

| What | Value |
|---|---|
| version | 6.3, 80-byte little-endian header, magic `0x044D495A` |
| namespaces | new scheme: `C` contents, `M` metadata (13), `W` `mainPage` (redirect), `X` indexes |
| entries | IT 195,173, of which 121,276 redirects; average dirent 32 bytes |
| title index | `titleIdxPos = -1` (no v0 list): the title order exists **only** in `X/listing/titleOrdered/v1`, an array of `uint32` (entry indexes) |
| title ordering | **by byte**, hence case-sensitive; the list also contains redirects (IT 62%, EN 93% of the sample) |
| clusters | 276 zstd (type 5) and 4 uncompressed (type 1, 23 to 30 MB each: Xapian indexes and lists) |
| zstd frame | level 19, **declared window 8 MiB, content size absent** |
| decompressed cluster | about 2 MiB and 186 blobs (median); compressed about 160 KB in the `mini` variant |
| MIME types | `text/html`, SVG (formulas and icons), WebP, CSS, JS |
| checksum | MD5 in the last 16 bytes |
| Kiwix search | `X/fulltext/xapian` and `X/title/xapian` (Xapian, GPL): not usable by us |

Old files (version 5) use xz (type 4). Blob offsets are 4 bytes, or 8 if bit `0x10` of the cluster
information byte is set.

### The catalog

- OPDS: `https://opds.library.kiwix.org/catalog/v2/entries?lang=ita` (`library.kiwix.org` answers 301 to here).
  For each entry: name, title, language, variant, number of entries, 48x48 illustration, size and `.meta4` link.
- The `.meta4` (Metalink) contains the size, the **SHA-256**, a **SHA-1 for each 4 MiB piece** and the HTTPS
  mirrors (nluug.nl, fau.de, accum.se, dumps.wikimedia.org...). The download can be resumed piece by piece,
  verified.

| Collection | Size |
|---|---|
| `wikipedia_it_top_mini` (introductions, source of AKB6) | 107 MB |
| `wikipedia_it_medicine_nopic` | 92 MB |
| `wikipedia_it_top_nopic` | 898 MB |
| `wikipedia_it_all_mini` | 2.3 GB |
| `wikipedia_it_all_nopic` | 8.4 GB |
| `wikipedia_it_all_maxi` | 30 GB |
| `wiktionary_it_all_nopic` | 357 MB |
| `wikivoyage_it_all_maxi` | 617 MB |
| `gutenberg_it_all` | 824 MB |
| `ifixit_it_all` | 3.2 GB |
| `unix.stackexchange.com_en_all` | 1.2 GB |
| `devdocs_en_python`, `_c`, `_cpp`, `_git`, `_lua`... | 0.4 to 7 MB each |

### The constraints of our system

1. **32-bit offsets on top of FatFs.** FatFs has exFAT and LBA64 on (`components/fatfs/src/ffconf.h:255,284`), but
   newlib has `off_t` = `long`. WAMR's `os_lseek` and `__wrap_pread` (`components/nv_wasm/nv_wasm_wasi.c:336`)
   narrow it, so **an app does not read beyond 2 GiB** of a file.
2. **Seek on FAT32.** `pread` is `f_lseek` + `f_read`. On FAT32 a backward seek walks the cluster chain
   from the start, and `CONFIG_FATFS_USE_FASTSEEK` is off (`sdkconfig:1926`). A ZIM reader does many random
   reads, so this must be measured. On exFAT contiguous files are positioned in constant time.
3. **Text.** `gfx_text` is a 5x7 font with only ASCII capitals. The app must bring its own fonts, as luaapp
   and puzzles already do (4-bit atlases generated at build time).
4. **Images.** The system does not decode images for apps: the app decodes (openhasp already has lodepng).
5. **Network for apps.** `nv.http_req` buffers the whole response (maximum 1 MB), does not follow redirects, does not
   expose headers and has a 30 s timeout. The Store downloader (`nv_appstore`) resumes, computes
   SHA-256 and follows redirects, but accepts only signed packages toward whitelisted folders, and its `fseek` is
   `long` (2 GiB).
6. **Visible files.** With the `home` permission a WASI app sees `/sdcard/home`. The library goes in `~/zim/`, shared
   among the app, the Terminal and the Files app.
7. **Memory.** `ram_budget` reaches at most 16 MB.
8. **ANIMA already has AKB6.** `docs/ANIMA_KB.md` chose on purpose not to read a ZIM at every question: AKB6 remains
   the fast path, the ZIM is the level below.

## 1b. F0 measurements on the board (1.2.100, 16 GB SD, AOT)

The SD file system is not exposed by any API. We deduce FAT32 from the cost that grows with the distance
from the start of the file, which a contiguous file on exFAT does not do.

Bench: `ports/zim/bench/zimbench.c` (WASI program, `bash ports/zim/bench/build.sh`), launched in the Terminal.
Samples in `D:\zimtest\fixtures` (outside the repo, SHA-256 verified against the `.meta4`).

**Where the indexes are.** In all files the dirents and pointer lists occupy the **last 2-8%** of the file
(Wiktionary: 28 MB at the end of 374 MB). The dirents are written **in path order** (100% of cases in
Medicine and Wiktionary): a range of indexes is a contiguous stretch of the file.

**Reads from the SD** (64 MB test file):

| Measurement | `pread` | `lseek` + `read` |
|---|---|---|
| sequential read in 1 MiB blocks | **0.74 MB/s** | **8.55 MB/s** |
| 4 KiB at a random point | 14.2 ms on average (6.7 ms at 0 MB, 18 ms at 57 MB) | 6.9 ms |
| 4 KiB, reads ordered forward | 13.9 ms | **1.5 ms** |
| 64 bytes at a random point | 9.6 ms (1.4 ms at 0 MB, 13.9 ms at 57 MB) | 6.9 ms |
| sequential write | 3.7 MB/s | |

- ESP-IDF's `pread` does `f_lseek` + `f_read` and then returns to the previous position: every read restarts
  from the start of the FAT32 cluster chain.
- On a 1 GB file written on purpose (write at 4.35 MB/s), 64 bytes with `pread` cost 8 ms at the start and
  120 ms in the last tenth: linear, about **0.12 ms per MB** of distance from the start. That means about 50 ms
  at the end of Wiktionary and about 250 ms at the end of a 2 GiB part.
- With `lseek` + `read` the position stays where it was read. On the same 1 GB file:
  - reads ordered forward: **3 ms**;
  - random reads: 46 ms on average, because going back still walks the chain.

**zstd in AOT**: **6.6 MB/s**, about **290 ms** for a 2 MiB cluster (on the PC 1.5 GB/s). It is the most expensive item
when opening an article.
- Decoding in streaming into the stable buffer (`ZSTD_d_stableOutBuffer`) and stopping as soon as the blob is
  complete, on average 44-57% of the cluster is decoded (PC and board).
- On the board it takes **176 ms** instead of 292 (−40%), with a 221 KB context and without the 8 MiB window.

**ZIM on ScoutWiki** (16 MB, 5,256 entries):

| Measurement | Result |
|---|---|
| search by path, no cache | 58 ms (`lseek`) or 88 ms (`pread`), 11 probes and 23 reads |
| search by path, cache of 64 pages of 4 KiB | 2.4 ms median, 1.8 reads |
| title list found | 50 ms |
| title prefix cold | 31-106 ms |
| title prefix warm | 2 ms |
| article cold (search, cluster, full decode) | 430-570 ms |

**Network**: board Wi-Fi at **-86 dBm**. HTTPS from Kiwix 128 KB/s, HTTP on the home network 130-140 KB/s (both
`wget` and `nv.http_req` in 1 MiB pieces). The limit is the radio link, not the server or the SD: 92 MB
take about 12 minutes, a 900 MB collection about 2 hours. A mirror (ftp.fau.de) refuses the board's TLS
connection; download.kiwix.org and mirror.download.kiwix.org work.

**Old format** (`icd10_fr_all_2012-01`, ZIM 5.0): xz clusters, namespace `A` (`.html` articles), `I`, `-`,
`M`, main page from the header and v0 title list in the header.

**What changes in the plan**:

- **D12 (new):** the reader uses `lseek` + `read`, never `pread`, with a cache of 4 KiB pages.
  - Binary searches **memorize the upper levels** (12 by default): the first probes are identical for every
    search, so from the second one on they come from RAM, with no cost at opening.
  - When 64 entries remain, their pointers are read in one read and their dirents (contiguous, because in
    path order) in a second.
- **D13 (new, firmware):** `CONFIG_FATFS_USE_FASTSEEK=y`. ESP-IDF builds the cluster map when a file is
  opened read-only and, if the file is too fragmented, continues without it. It also helps video, music and
  galleries. The opening cost of large files and the SRAM cost (256 bytes per open file) must be measured.
- **D4 updated:** partial streaming decoding instead of the single shot.
- **F5 updated:** with weak Wi-Fi the large collections go through the PC (card reader); the small ones are
  downloaded from the board. The background system downloader (ABI 18) becomes necessary, because a
  download of hours cannot depend on the app being open.

## 2. Decisions

| # | Decision | Why |
|---|---|---|
| D1 | **Our own** reader in C (`nvzim`), written from the specification | libzim and Xapian are GPL-2.0: incompatible with commercial licenses |
| D2 | Three pieces on the same library: `nvzim` (C, testable on the PC), `zim` (WASI, Terminal and ANIMA), `biblioteca` (WASM app in the Store) | no native app: the firmware partition is tight, the Store releases without OTA, and ABI 16 is enough (`gfx_blit` + `gfx_persist`) |
| D3 | Files over 2 GiB split into **parts** `.zimaa`, `.zimab`... under 2 GiB, read as a single file | it is libzim's scheme, so Kiwix reads the same parts; works on FAT32 and exFAT without touching the firmware |
| D4 | zstd **in a single shot** (`ZSTD_decompressDCtx`) into a 2 MiB buffer, which grows to 4 or 8 MiB if needed | with no size in the frame, streaming would allocate an 8 MiB window; the single shot uses the output as the window |
| D5 | For uncompressed clusters **only the needed blob** is read, by offset | they are 23-30 MB |
| D6 | Search by **title prefix** on the v1 list, with variants (as written, initial capital, all lowercase, initial capitals) merged and without duplicates | the list is ordered by byte; the redirects already cover many spellings. Full text (Xapian) stays out of v1 |
| D7 | Layout in **reading mode**, our own engine (`nvhtml`), not litehtml | a Kiwix page loads more than 10 MediaWiki CSS files: a CSS engine would cost seconds and memory on the P4. Reading mode gives consistent typography and is reusable for EPUB, RSS, Markdown and mail |
| D8 | Noto Sans and Noto Sans Mono fonts (OFL), extended Latin subset, Greek and Cyrillic, 4-bit atlases in 3 sizes (regular, bold, italic) | covers the 5 system languages and the proper names of Wikipedia; a missing glyph becomes a box |
| D9 | ANIMA: first AKB6, then `zim` for details and for specialist collections | respects the choice of `ANIMA_KB.md`; the tool line goes in the prompt only if `~/zim` contains collections, and lists them |
| D10 | Name "Biblioteca" / "Library", never "Kiwix" | the trademark is theirs; the card says "reads Kiwix/openZIM ZIM files" |

## 3. Architecture

```
SD   ~/zim/*.zim  |  *.zimaa, *.zimab...   (FAT32 or exFAT, parts < 2 GiB)
          |
nvzim (C)   header, MIME, dirent, zstd/xz clusters, redirects, titles, metadata, parts,
            aligned 4 KiB page cache, LRU of 2 decompressed clusters
          |                                   |
zim (WASI, system program)              biblioteca (WASM, Store)
Terminal and ANIMA                      library, search, reader, catalog and download
html -> text with nvhtml                nvhtml: tokenizer -> blocks -> lines -> glyph atlas
                                        -> RGB565 framebuffer -> gfx_blit (persist, changed rows only)
```

## 4. Phases

Each phase closes with verification on the board, not before.

### F0: measurements and samples (small, no firmware)

- Samples (to download after the OK): `wikipedia_it_medicine_nopic` (92 MB), `wiktionary_it_all_nopic` (357 MB),
  `devdocs_en_c` (1.2 MB), `scoutwiki.org_it_all` maxi (16 MB, with WebP images) and an old xz ZIM from the archive.
  The parts and the file over 2 GiB are tried with synthetic files, without downloading 2 GB.
- Benchmark on the P4 with a test WASI app in AOT:
  - single-shot zstd on 2 MiB;
  - random `pread` on a 2 GiB file, on FAT32 and on exFAT;
  - complete search of a dirent;
  - `nv.http_req` speed in 1 MiB pieces (HTTPS, mirror from the `.meta4`).
- Inventory of the tags and HTML classes in full articles (`nopic`), to fix the subset of reading
  mode.
- **Output:** numbers written here; decision on FASTSEEK and exFAT, and on download inside the app or ABI 18 (F5).

### F1: `nvzim` library and tests (medium, no firmware)

- API: `zim_open(parts)`, `zim_meta(key)`, `zim_find_path(ns, path)`, `zim_find_title(prefix, n)`,
  `zim_get(entry)` (view on the blob), `zim_resolve(redirect)`, `zim_main()`, `zim_random()`, `zim_illustration(48)`,
  `zim_check()` (MD5, incremental).
- Reads in aligned 4 KiB pages, so as not to pay the bounce of one sector per SD command (lesson from
  `sd-dma-alignment`).
- Tests in `tests/host/unit`. Fuzz `fuzz_zim` and `fuzz_zimhtml` in `tests/host/Makefile`, in the CI.
- **Differential test** against python-libzim on the PC (only as a tool, not distributed): 10,000 IT and EN entries
  with identical title, MIME, redirect and content hash.
- **Output:** green CI, differential test with zero differences.

### F1: results (§4b)

**What is there**:
- `ports/zim/nvzim` (`nvzim.h`, `nvzim.c`, `nvzim_md5.c`), in C99 and without global variables (apart from the CRC
  tables of XZ Embedded, read-only);
- `fetch.sh`, which downloads zstd 1.5.7 and XZ Embedded with pinned checksums;
- the tools in `ports/zim/tools`;
- `ports/zim/README.md`.

**Functions**:
- header, MIME types, entries and redirects (chains and cycles);
- search by path with memorized levels and block read of the last 64;
- main page and metadata;
- contents from zstd clusters (partial decoding that resumes), uncompressed (read in place) and xz (ZIM 5);
- 8-byte pointers, split files `.zimaa`…;
- v1 title list and v0 index of old files;
- suggestions with case variants and Latin accents, random entry;
- MD5 verification.

**Tests**:

| Test | Outcome |
|---|---|
| Comparison with python-libzim (`tools/diff_libzim.py`): 10,000 entries of Wikipedia IT and EN top mini and of Wiktionary IT (758,797 entries), 3,000-5,000 of Medicine, DevDocs C, ScoutWiki and ICD-10 2012 (xz); path, title, type, redirect, size and content MD5, main page, metadata, title list order | **0 differences** |
| `tests/host/unit/test_nvzim.cpp` on synthetic files (`tests/host/gen_zim.py`): every entry by every route, default and minimal configuration, split files, in-memory backend, old file, truncations and altered bytes | **18,105 checks, 0 failed**, at 64 and 32 bit with ASan and UBSan |
| `tests/host/fuzz/fuzz_nvzim.cpp` | 2 minutes per architecture (~50,000 and ~30,000 executions): no crash, leak or timeout |
| On the board (WASI + AOT, ScoutWiki 16 MB) | see below |

Measurements on the board:

| Measurement | Result |
|---|---|
| 200 searches by path | **4.1 ms** on average, 21 ms the worst, 1.7 reads each (the F0 bench without cache: 58 ms) |
| cold suggestions | 77-92 ms (32-39 reads) |
| `zim get` of an article, program startup included | 1.3 s |
| MD5 verification of 16 MB | 5.9 s |

**Fixed along the way**:
- with the stable buffer, zstd reports the full output as an error without keeping the progress. The buffer grows
  and it restarts, and the input enters 16 KiB at a time, so it really stops near the blob;
- a cluster over `cluster_max` now gives `NVZIM_EUNSUPPORTED` and not "damaged file".

**Known limit**: the title list is ordered by byte. A search that does not guess the capitalization of an entry
without a redirect ("IPHONE" for "iPhone") does not find it. The side index of §8 will solve it.

### F2: results (§4c)

**Store**: `zim` 1.0 published on 2026-10-11 (store `dd8858d`), then 1.1.0 (`a5f7876`). System program
since firmware 1.2.108: every board installs it by itself.

**Firmware** (`3c6aa48`, tested as local build 1.2.106 and 1.2.108 on the test board):
- `zim` in `kSystemApps`;
- in the ENVIRONMENT block of every turn, when `~/zim` contains collections, a line with their file names and usage
  (`zim search`, `zim get --intro`, then `--sections`/`--section`, "say which collection you used");
- the "what can you do" intent fired for "commands", "list" or "help" in any sentence; now only in
  requests of up to 8 words (new case in `test_anima.cpp`).

**ANIMA on the board**: the device's online model; Wikimed collections (97 MB), ScoutWiki, DevDocs C, ICD-10.

| Question | Outcome |
|---|---|
| "Che raccolte ho nella biblioteca offline?" | ✓ `zim ls`, exact list |
| "Usa zim per leggere cosa dice Wikimed sull'insulina" | ✓ `zim search` + `zim get --intro`, correct summary |
| "Quante persone hanno il diabete nel mondo secondo la biblioteca offline?" | ✓ after the fix: `--intro`, then `--section Epidemiologia`, figure and collection cited |
| "Cosa dice l'enciclopedia medica offline sull'insulina? … elenca i comandi" | ✗ before (intent "what can you do", then command repeated because of truncated output), ✓ after the fixes |
| "In the offline C documentation, what does fprintf return?" | ✗ before (wrong DevDocs titles), ✓ after: `zim get fprintf` finds `io/fprintf` |
| "A cosa serve il paracetamolo? … da dove hai preso l'informazione" | ✗ no command executed, but the answer cites `zim get` and the ICD-10 collection: invented |

**Fixes that came from the tests** (in `zim` 1.1.0, no firmware):
- a word that is not a command counts as `get`;
- search inside titles and paths (`nvzim_scan`), then by the longest word;
- `--intro` ends with the sections;
- `--max` limits the entire output (ANIMA reads 2000 bytes per command);
- `-z` accepts the file name;
- cache `~/zim/.zim-index` (`zim get` from 2.1 to 1.6 s).

**Local build 1.2.109** (2026-10-12):
- **Commit**: `b07d75a` on `main`.
- **Content**:
  - titles and language of the collections in ANIMA's line (from `~/zim/.zim-index`);
  - read-only `zim` for the `ACT sh` grammar (before it asked "proceed?" to models without native tools);
  - "run it now" nudge when an answer cites `zim get`/`search`/`ls` without having run anything;
  - `--max 1800`;
  - new cases in `test_anima.cpp` and `test_anima_agent.cpp`, 64 and 32 bit green.
- **On the board**:
  - paracetamol is now really read (`zim get Paracetamolo`) and the cited source is Wikimed: no more invented
    sources;
  - the answer arrived after 344 s, because the model repeated the same read several times.
- **Fault found (firmware, not `zim`)**: when ANIMA launches a terminal program without a screen, the first
  launch of every chat (not the first chat after boot) dies 23-25 ms after instantiation with
  `wasm: Exception: native stack overflow`.
  - Seen 3 times on 1.2.109.
  - The other launches of the same chat are fine, and from the Terminal (even in a pipe) it never happens.
  - It is too early for a program recursion: it looks like a wrong native stack limit for that launch
    (`run_worker`, `wasm_runtime_set_native_stack_boundary`).

**Local diagnostic build 1.2.111** (2026-10-12, log of task, sp, base and limit at every terminal launch):
- 5 ANIMA chats with the local model (Ollama, qwen3.5:9b, no TLS), 6 launches of `zim` from the `sh` task: the error did not
  return.
- `zim` uses about **1 KB** of the 256 KB native stack: the limit sits 230 KB below sp, and the free minimum at the end of the
  launch is ~254 KB. A true recursion is ruled out: if the error returns, it is a wrong limit or a dirtied stack.
- The 3 times on 1.2.109 the model was the online one (TLS): the suspicion is a link with ongoing HTTPS calls,
  not proven.
- On `main` stays `f8ef52c`: no log in normal launches; if a terminal launch dies of "native stack
  overflow", a WARN says task, sp, base, limit and free minimum. High free minimum = wrong limit, low = real
  use.

**Next steps before a public firmware**:
- repeat the chats with an online provider (keys on the board) and read the WARN of `f8ef52c` in `/api/logs`;
- repeatable evaluation: `zim` cases in `tools/anima_eval.py`, instead of the manual questions of this session;
- fewer repeated reads: paracetamol took 344 s because the model reread the same entry.

### F3: results (§4d)

**What is there**:
- The `apps/biblioteca` app (WASI, `app.wasm` 0.97 MB, AOT 1.5 MB) with the sources in `ports/zim/biblioteca`:
  - `doc.c`: the layout;
  - `font.c`: Noto Sans and Noto Sans Mono (OFL, Latin, Greek and Cyrillic subset in `fonts/`),
    rasterized by stb_truetype at any size;
  - `gfx.c`: the frame buffer;
  - `kbd.c`: the physical keyboard;
  - `lib.c`: collections, covers, history, bookmarks;
  - `i18n.c`: 5 languages;
  - `app.c`: the screens.
- The icons are drawn by `gen_icons.py`, no third-party images.
- `nvhtml` has a "rich" mode (`opts.rich`) with inline markers for bold, italic, code, superscripts and
  subscripts, links, table rows and cells, list items, preformatted text and heading anchors. The rules are
  verified line by line by `tests/host/unit/nvhtml_rich_check.h`, in the tests and in the fuzzer. The plain text
  for ANIMA does not change (3632 checks).
- Tables inside a cell become rows of their own, then the cell resumes (DevDocs tables).
- **Screens**:
  - library with covers and "Continue reading";
  - reader with links, anchors, back and forward;
  - search as you type, with "Find “…” inside titles" when no title starts that way;
  - sections, bookmarks, history;
  - collection information with MD5 verification;
  - on-screen keyboard (letters, accents, symbols);
  - light and dark theme, 5 text sizes, 5 interface languages.
- **Tests on the PC**: `bash ports/zim/biblioteca/test.sh` runs 7 scripts of taps and keys on the test collections.
  - Each script runs twice: with ASan and UBSan, and with the device's memory cap.
  - Screenshots end up in `ports/zim/_out/bib_shots`.
  - App memory peak: 5.8 MiB out of 12.
- With `/appdata/bench.txt` (`FILE<TAB>PATH`) the app opens the entry and scrolls it by itself: it is the repeatable
  scrolling test bench on the board.

**Measurements on the board** (local firmware 1.2.113, Wikimed 97 MB on FAT32; times in the system log):

| Measurement | Result | Goal §5 |
|---|---|---|
| library, 4 collections from cache | 234-283 ms | |
| opening a collection | 26-90 ms | < 300 ms |
| cold entry | 200 ms (Paradenti, 15 KB); 550 ms (Paracetamolo, 79 KB: read 344, HTML 53, layout 120) | < 1.5 s |
| suggestions | first key 254 ms, then 1-76 ms | < 150 ms warm |
| scrolling | 17-20 fps | >= 30 fps |
| memory | peak 5.8 MiB (PC, with cap); `ram_budget` 12 MiB | <= 12 MiB |

**Found and fixed on the board**:
1. **Linear memory.** WAMR grows it with a realloc of the whole block in PSRAM. With fragmented PSRAM the
   growth fails: ENOMEM, or zstd failing an allocation and looking like a damaged file, differently at
   every start.
   - At startup the app allocates and frees a large block, so memory grows only once, when the block in
     which the app was loaded is still free.
   - The `ram_budget` goes from 16 to 12 MiB: with 16 the app sometimes did not start ("allocate linear memory failed").
2. **nvzim.** `slot_grow` kept the old and the new buffer together (12 MiB for an 8 MiB cluster). Now it frees the old
   one first: the content is decoded again anyway.
3. **Scrolling**, from 8 to 20 fps:
   - the memmove of the frame buffer (1.1 MB in 88 ms) is replaced by a table of row pointers that rotates;
   - the 1:1 canvas went through LVGL's software render: now `canvas_scale: fit` goes to the panel with the PPA;
   - firmware `756bff7` also gives these apps the LVGL pointer (mouse and `/api/ui/tap`, before only the raw
     touch arrived) and copies the blit rows with memcpy.
4. **Word-by-word links** (Wiktionary): they merged into a single link. Now every `</a>` closes its own link.

**Open**:
- **30 fps.** The limit is the canvas copy from the app to the host: 34 ms for 1.1 MB, PSRAM bandwidth, not the
  code. Two levers in the firmware, both a new ABI:
  - a scroll done by the host with the PPA, with which the app sends only the new strip;
  - a presentation without copy.
- **Firmware, for all large WASI apps.** Reserve the maximum linear memory at instantiation (or do the
  pregrow in the runtime), instead of relying on each app.
- **Keyboard.** The keys of `/api/ui/hid` go through LVGL's queue and not through the state read by `nv_kbd_state`: the
  keyboard is verified by the tests on the PC, it must be tried with a real keyboard.
- Images (F4) and the Store (F6).

### F4: results (§4e)

**What is there**:
- **`nvhtml`**: rich text marks images with `NVHTML_IMAGE`, then source, declared size (px or ex),
  `vertical-align` of formulas, flags (formula, formula on its own line, inside a figure), alternative text.
  The plain text for ANIMA does not change: a formula remains its TeX.
- **`img.c`**:
  - WebP decoding (libwebp 1.6.0, decoder only, scaled already during decoding);
  - PNG (LodePNG);
  - JPEG, GIF first frame, BMP (stb_image 2.30);
  - SVG (NanoSVG).
  - Images are decoded at the displayed size and kept in a 3 MiB cache (RGB565 + alpha).
  - Never during drawing: a queue, one image per frame when the page is still; meanwhile there is a placeholder.
- **Formulas**:
  - MathJax SVGs (Wikipedia) use `<defs>`/`<use>` and `currentColor`, which NanoSVG does not know: before
    parsing, every `<use transform x y>` becomes its path with `transform translate(x y)`, as the specification says
    (subscripts are `scale(0.707)`).
  - Formulas are drawn as a mask in the text color, so they are readable with the dark theme too.
  - They align to the text baseline with their `vertical-align`; 1 ex is 0.536 em, the x-height of Noto Sans.
- **Viewer**: a tap on an image without a link shows it full screen with the description, up to 3
  times the size in the page. Vector formulas stay sharp on a light background.
- **Fonts**: added IPA (Wiktionary pronunciations), combining diacritics and polytonic Greek: 758 KB in
  total.
- **Fuzzer** `ports/zim/biblioteca/test/fuzz.sh` (decoder and SVG rewriting, ASan and UBSan):
  - found two float-to-int overflows in NanoSVG (an almost horizontal side makes the slope explode);
  - `nanosvgrast.patch` fixes them, applied by `fetch.sh` on a copy: the downloaded file remains verifiable with
    the SHA;
  - after the patch, 99,689 executions in 5 minutes without errors.

**Measurements on the board** (local 1.2.115; times in the log):

| Measurement | Result |
|---|---|
| WebP photo 250x333 (ScoutWiki) | 211 ms |
| SVG formula 64x25 ... 221x30 (Wikimed) | 41-48 ms with the cluster already decoded, 98-338 ms if it must be decoded |
| enlarged formula 663x90 | 297 ms |
| app memory | peak 7.8 MiB (PC with cap), budget 12 |
| app.wasm / app.aot | 1.37 MB / 2.43 MB (limits 2 and 4) |

**Found and fixed**:
1. **"Continue reading" disappeared**: the system closes the app without letting it finish the loop, so the final
   save did not happen. Now the position is saved at every opened page and after 2 s of still page.
2. **Fonts**: without IPA the pronunciations (ɪ) were boxes.
3. **Clusters in memory**: 2 instead of 1, so the page and its images do not evict each other.

4. **Queue order**: first the images whose cluster costs no decompression, because it is already in memory or
   is not compressed (`nvzim_cluster_cached`). With the 7 formulas of Paracetamolo in the queue together, the two in clusters
   already ready go first. The gain here is small (1.2-1.5 s in total): they sit in about 5 clusters, and each
   costs about 0.2 s of zstd at 6.6 MB/s.

**Open**:
- A page with many formulas shows them one after the other, about 0.2 s for each cluster to decompress.
- Images without a link cannot be reached from the keyboard: the viewer opens with touch or mouse.
- An animated GIF shows only the first frame; a PNG or JPEG over 1 Mpixel remains a box.

### F5: design (§4f)

Real data (12 October 2026):
- **Catalog**: OPDS v2 at `https://opds.library.kiwix.org/catalog/v2/entries?lang=ita&count=N&start=K`, with
  filters `category=` and `q=`. The old `library.kiwix.org` answers 301, and ABI 12 does not follow redirects.
  There are about 1.3 KB of Atom XML per entry and 382 collections in Italian.
- **Each entry** carries name, title, summary, language, date, `articleCount`, `tags` (`_pictures:yes`...), the 48x48
  PNG thumbnail and the `.meta4` link on `lb.download.kiwix.org` with `length` (direct 200 answer, no redirect).
- **`.meta4`** (MirrorBrain, RFC 5854):
  - `size`;
  - `md5`, `sha-1` and `sha-256` of the whole file;
  - `<pieces length="4194304" type="sha-1">`, that is, a hash for every 4 MiB piece;
  - about 8 HTTPS mirrors with priority, ordered by proximity.
  - It weighs about 60 bytes per piece: 15 KB for a 1 GB file.

Decisions:
- **System downloader, `components/nv_dl` + ABI 18**. A download of hours cannot depend on the app being open, and
  ABI 12 has 1 MB responses, no redirects and everything tied to the app's execution.
  - Its own task, with the stack in PSRAM: writes only to the SD. One download runs at a time and the others wait in the queue.
  - Does not block the Store: the Store worker stays free, and the two TLS can coexist because mbedTLS
    allocates in PSRAM.
  - A single stream with `Range: bytes=N-` from the verified point. Each 4 MiB piece is verified with SHA-1 as soon as
    it is complete.
  - The job registry on the SD is updated at every verified piece. After an interruption, a reboot or a dropped
    mirror it restarts from the last good piece.
  - A wrong piece makes it change mirror and download it again; the third error on the same piece stops the job.
  - Without pieces in the `.meta4` (or with a simple URL) the SHA-256 of the whole file applies, reread at the end.
- **Large files**: over 2 GiB it writes in parts `name.zimaa`, `name.zimab`... of 2 GiB minus 4 MiB, that is a
  multiple of the pieces, so a piece never sits in two files. `nvzim` reads them as a single file.
- **Writing**: to `name.part` and rename at the end, only after full verification. Before starting the
  free space is checked: the size plus the OTA reserve (64 MB).
- **Security**:
  - only `https`, only public addresses (`nv_net_policy`), redirects followed only toward public `https`;
  - destination inside what the app sees (`home` -> `/sdcard/home`, `fs` -> its data folder), without `..`;
  - an app sees and controls only its own downloads;
  - User-Agent `NucleoOS/<version> (+https://indecenti.github.io/nucleoos-p4-store)`.
- **ABI 18** (17 belongs to the full-screen programs, "tty"), `net` permission:
  - `dl_start(spec)` with spec `{"meta4": URL}` or `{"url": URL, "size": n, "sha256": hex}`, plus `"dest"` and
    `"title"`;
  - `dl_info(id)` and `dl_list()` answer in JSON: state, bytes done, speed, error;
  - `dl_pause(id, on)` and `dl_cancel(id)` (which also deletes the downloaded part).
- **End of the download**: a notification ("Library: Wikipedia downloaded"), and the tap opens the app.
- **Shell**: `dl` lists, pauses, resumes, cancels; `dl get URL.meta4 DEST` serves people and HIL tests.
- **Library**:
  - "Download collections" screen with the Recommended (by name) and search by language, category and text, with thumbnails,
    size, date and number of entries;
  - "Newer edition" when there is the same name with a newer date; the old collection is removed only after
    the new one is verified, and only if the user asks for it;
  - list of downloads with progress.
- **Tests**:
  - pure core `nv_dl_core` (`.meta4` parser, job registry, parts, paths) with unit tests and fuzzer in
    `tests/host`;
  - **HIL** `tools/hil/dl_resume.py`: about 900 MB with the Wi-Fi interrupted and a reboot in the middle, then verification and
    opening of the collection.

### F5: results (§4g)

**What is there**:
- **`components/nv_dl`**: the system downloader described in §4f.
  - The pure core `nv_dl_core.c` (Metalink 4, job registry, parts, app paths) has 95 checks in
    `tests/host` and a fuzzer, which found a registry that was readable but not rewritable (fixed).
  - `nv_net_policy` moves into `nv_kernel`.
- **ABI 18** (17 belongs to the "tty" programs of `feat/office`): `dl_start`, `dl_info`, `dl_list`, `dl_pause`,
  `dl_cancel`, `dl_free`.
- **Shell**: `dl ls`, `dl get`, `dl pause`, `dl resume`, `dl cancel`, `dl wait`.
- **Notification at the end**: the tap opens the app that requested the download.
- **Library 1.2.0** (`cat.c`, `get.c`):
  - OPDS catalog cached for one day, Recommended by name, categories, search;
  - grouped editions, available space, list of downloads with pause, resume and two-tap cancel;
  - "Delete from the card" in the collection information;
  - 39 new phrases in 5 languages.
- **Tests on the PC**:
  - `test/cat_test.c`: 133 checks on 8 real catalog entries;
  - `test/fuzz_cat.sh`: 105,000 clean executions;
  - `test/get.script`: the whole path with simulated catalog and downloader, including the offline case;
  - `test/delete.script`: the deletion.
- **HIL**: `tools/hil/dl_resume.py`.

**Measurements on the board** (local 1.2.142, Wi-Fi at -85 dBm):

| Measurement | Result |
|---|---|
| Vikidia nopic, 14.8 MB, 4 pieces, 5 mirrors | 61 s, SHA-256 equal to the one in the `.meta4` |
| The best of Wikipedia nopic, 941,812,270 bytes, 225 pieces, 6 mirrors | 71 min with a 45 s `wifi off` at 56 MB and a reboot at 173 MB; SHA-256 equal |
| speed | 80-390 KB/s, average about 220 KB/s |
| after the interruption | resumed at 58,720,256 bytes, that is from the last verified piece (14 x 4 MiB) |
| Italian catalog (382 entries, 466 KB) and thumbnails | catalog in one request; thumbnails one at a time, then from the card |
| opening the downloaded collection | main page immediately, images in 0.3-1.5 s |
| static internal RAM | +88 bytes (586 free out of 1,002 with `feat/office`) |
| app memory | peak 5 MiB in the downloads screen (PC with cap) |

**Found and fixed on the board**: `dl ls` said "waiting for: net" for the whole hour in which a job was running
(the label was updated only between one job and the next), and a resume left no trace in the log.

**Open**:
- The main page of Wikipedia "top" is a grid of cards: it comes out as images and links in a column.
- The first scan of a newly downloaded collection reads the metadata (2.5 s for 942 MB), then the cache.
- Publication: first `feat/office` into main, then `f5dl`, the public firmware, then Library 1.2.0 in the
  Store (`abi` 18, `net` permission).

### F2: `zim` in the Terminal and in ANIMA (medium; firmware only for the hook into ANIMA)

- Commands:
  - `zim ls` and `zim info ID`;
  - `zim search [-z ID] [-n N] TEXT`;
  - `zim get [-z ID] TITLE [--intro | --section NAME | --html | --links]`;
  - `zim random [-z ID]` and `zim check ID`.
- The text comes out of `nvhtml` (paragraphs, lists, row tables), without going through `html2text`.
- First in the Store as a terminal program. With the first useful OTA: `kSystemApps`, the dynamic line in
  `ANIMA_SH_TOOLS` and the citation of the source ("Wikipedia IT, July 2026, offline").
- Evaluation: 10 new cases in `tools/anima_eval.py` (medicine, iFixit, DevDocs, Wiktionary, "tell me more about X"),
  measured before and after.
- **Output:** verified on the board, no regression in the evaluation.

### F3: Library app, the reader (large, no firmware)

- Screens:
  - **Library:** 48x48 cover, title, language, date, number of entries, space used.
  - **Search:** suggestions as you type.
  - **Reader:** section index, internal links, back and forward, history, bookmarks, random entry,
    text size, light or dark theme.
  - **Collection information:** author, publisher, source, license, from the metadata.
- `nvhtml`:
  - blocks: `h1`-`h6`, `p`, `ul`/`ol`/`li`, `dl`, `blockquote`, `pre`, `table` (simplified, infobox as pairs),
    `figure`/`figcaption`, `hr`, `details`/`summary`;
  - inline: `b`, `i`, `a`, `sup`/`sub`, `code`, `br`, `small`;
  - `script`, `style` and navigation are discarded; notes are collected at the bottom.
- Each entry is laid out once; only the visible part is drawn, scrolling happens in the app's
  framebuffer and only the changed rows are sent.
- Keyboard and mouse mandatory (skill `keyboard-support`):
  - Tab, arrows, PgUp/PgDn, Space, Home/End;
  - `/` or Ctrl+F to search;
  - Alt+left/right arrow for back/forward;
  - Esc.
- 5 interface languages.
- `.zim` association: `"file_types": [{"ext": "zim", "mime": "application/x-zim", "kind": "other"}]` plus `"opens"`.
  A file outside `~` must be moved to `~/zim/`.
- **Output:** goals of §5 measured on the board, screenshots with the `ui-screenshot` skill.

### F4: images and formulas (medium, no firmware)

- Formats:
  - WebP (libwebp decoder);
  - PNG (lodepng);
  - JPEG (software decoder);
  - SVG with nanosvg (mathematical formulas and icons);
  - GIF (first frame).
- Decoding only when the image enters the view, scaled to the column, with a cache of at most 4 MiB.
- **Output:** correct pages of Wikivoyage maxi and ScoutWiki, memory within budget.

### F5: catalog and download (medium; firmware only if F0 asks for it)

- Catalog:
  - OPDS filtered by language, category and size;
  - "Recommended" chosen by name: Wikipedia top/medicine, Wiktionary, Wikivoyage, Gutenberg, DevDocs;
  - free space check before starting.
- Download:
  1. `.meta4`;
  2. HTTPS mirror;
  3. 1 MiB Range requests;
  4. SHA-1 verification of every 4 MiB piece;
  5. resume from the last good piece;
  6. writing in parts under 2 GiB and final rename.
- Updates: if the same collection exists with a more recent date, the update is proposed; the old one is
  deleted only after the new one is verified.
- If in F0 the download inside the app is slow, or if downloading a GB with the app open is unacceptable: **ABI 18**, a
  background system downloader for any app, derived from that of `nv_appstore`, with a hash fixed by the
  caller and a notification at the end. It would also serve podcasts, RSS and maps.
- Never automatic downloads. Recognizable User-Agent, mirrors from the `.meta4`.
- From the PC: card reader plus `tools/zim_split.py` for files over 2 GiB.
- **Output:** 900 MB downloaded with a Wi-Fi interruption and a reboot in the middle, resumed and verified.

### F6: Store and polish (small)

- `GUIDE.md` and `GUIDE.en.md`, Store card with QR, rows in `THIRD_PARTY.md`, category `knowledge` (empty
  today).
- Screenshots cleaned of personal data (SSID, city) before publishing them.
- **Output:** signed package, installed from the Store on the board and verified.

## 5. Measured goals

On the board, with `wikipedia_it_top_nopic` (898 MB), on exFAT and on FAT32.

| Measurement | Goal |
|---|---|
| opening a collection | < 300 ms |
| suggestions at every key, warm | < 150 ms |
| opening an entry, cold | < 1.5 s |
| opening an entry with the cluster already in cache | < 300 ms |
| scrolling | >= 30 fps |
| app memory | <= 12 MiB (manifest cap 16) |
| `zim get --intro` for ANIMA | < 1 s |
| download with good Wi-Fi | >= 1 MB/s, resume after a reboot |

Planned memory budget:

| Item | MiB |
|---|---|
| zstd context | 0.15 |
| 2 decompressed clusters | 4 |
| index page cache | 0.25 |
| font atlases | about 1 |
| layout of a long entry | 1-2 |
| 1024x600 RGB565 framebuffer | 1.2 |
| images (F4) | up to 4 |
| **total** | **about 12** |

## 6. Risks

| Risk | Countermeasure |
|---|---|
| Slow seeks on FAT32 (cluster chain, FASTSEEK off): tens of ms per read | measurement in F0; recommend exFAT; one open file per region (index, dirent, cluster) to avoid backward seeks; FASTSEEK in the firmware if needed |
| Files over 2 GiB | parts (D3); 64-bit WASI offsets are a separate firmware job, also useful to sqlite and zstd |
| 8 MiB zstd window | single shot (D4) |
| Non-Latin scripts (CJK, Arabic, Devanagari) | box in v1; additional fonts later, on request |
| Untrusted content | fuzzing of parser and layout, WASM sandbox, no JavaScript ever executed |
| HTML differs depending on the scraper | v1 guaranteed for mwoffliner (Wikimedia), DevDocs, Stack Exchange (sotoki), Gutenberg, iFixit; sites made with zimit (full of JS) only best-effort |
| Load on Kiwix servers | mirrors from the `.meta4`, recognizable User-Agent, no automatic downloads |
| Other sessions on the same tree | version slot and coordination rule before every release |

## 7. Licenses

| Component | License | Notes |
|---|---|---|
| `nvzim`, `nvhtml`, app, `zim` | ours | |
| zstd 1.5.7 | BSD-3-Clause | already in `ports/cli` |
| liblzma (xz 5.8.4) | 0BSD | already in `ports/cli` |
| lodepng | zlib | |
| libwebp, decoder only | BSD-3-Clause | |
| nanosvg | zlib | |
| Noto Sans, Noto Sans Mono | OFL 1.1 | |
| libzim, Xapian | GPL-2.0 | **not used**; python-libzim only for the tests on the PC |
| contents | per collection (Wikipedia CC BY-SA 4.0...) | shown by the app from the metadata |

## 8. After v1

- **Full-text search** with a side index of our own (`.zim.nvx`): built on the PC for the recommended
  collections, or on the device for the small ones.
- **Server on the home network:** `nv_web` serves the collections to phone and PC browsers, like kiwix-serve (firmware).
- **64-bit WASI offsets and FASTSEEK** (firmware): parts go away.
- **Reuse of `nvhtml`** for EPUB, RSS and mail.
