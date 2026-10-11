# ports/zim — offline library (ZIM reader)

Plan, measurements and decisions: `docs/ZIM_READER_PLAN.md`.

| Path | What |
|---|---|
| `nvzim/` | **nvzim**, our ZIM reader in C99 (no libzim, no Xapian: both GPL). Used by the `zim` tool, the Biblioteca app and the host tests. |
| `fetch.sh` | pinned, sha256-checked sources into `_src/` (not committed): zstd 1.5.7 (BSD-3-Clause, decoder only), XZ Embedded 2024-03-22 (0BSD). |
| `tools/zimdump.c` | nvzim on the PC or in the Terminal: info, entries (TSV with MD5), get, suggest, titles, verify, stats. |
| `tools/build_native.sh` | zimdump with gcc in WSL: `_out/native/zimdump`. |
| `tools/build_wasi.sh` | zimdump for the board (WASI + AOT): `_out/zimdump/`, pushed by hand. |
| `tools/diff_libzim.py` | differential test against python-libzim on real Kiwix files (libzim only here, never shipped). |
| `bench/` | zimbench: the F0 measurements on the board (SD reads, zstd, lookups, HTTP). |

Tests: `tests/host/unit/test_nvzim.cpp` and `tests/host/fuzz/fuzz_nvzim.cpp` (in `make -C tests/host ci`);
their synthetic ZIM files come from `tests/host/gen_zim.py` (corpus/nvzim, unit/nvzim_fixtures.h).

```bash
bash ports/zim/tools/build_native.sh
python ports/zim/tools/diff_libzim.py --wsl-dir /root/zimtest --dir D:/zimtest/fixtures -n 10000 FILE.zim
```

What nvzim does differently from libzim, and why (all measured on the ESP32-P4, see the plan, §1b):

- every read is `lseek` + `read`, never `pread` (ESP-IDF's `pread` walks the FAT32 cluster chain from the
  start of the file on each call), and index reads go through an LRU of 4 KiB pages;
- path and title searches memoise the top of their binary search; a path search reads its last 64 entries
  as one span (dirents are stored in path order);
- zstd clusters are decoded in place (`ZSTD_d_stableOutBuffer`, no 8 MiB window) and only as far as the
  blob asked for, fed 16 KiB at a time; a later blob of the same cluster resumes the decoding;
- uncompressed clusters are never loaded whole; xz clusters (ZIM 5) use XZ Embedded in single-call mode;
- split files (`name.zimaa`, `name.zimab`, ...) are one file, for SD cards and FAT32's limits.
