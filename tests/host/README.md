# Host tests and fuzzers

PC-side unit tests and [libFuzzer](https://llvm.org/docs/LibFuzzer.html) fuzzers for the firmware code
that parses **untrusted input**: media files that reach the SD card over the LAN, the web API's query
strings and paths, pairing tokens, signed OTA manifests and store packages, and replies from network
services. Everything builds with clang on Linux/WSL, with no ESP-IDF, under
AddressSanitizer and UndefinedBehaviorSanitizer, as **64-bit and 32-bit** binaries. The P4 is RV32:
pointer and `size_t` overflows only show up in the 32-bit build.

| Target | Firmware code | Input |
|---|---|---|
| `avi` | `components/nv_vplayer/vp_avi.c` | AVI (MJPEG/PCM): header, OpenDML / idx1 / scan indexes |
| `mp4` | `components/nv_vplayer/vp_mp4.c` | MP4 `moov` box: sample tables, avcC, keyframe seek |
| `mpeg1` | `components/nv_vplayer/pl_mpeg.h` | MPEG-PS / MPEG-1 video / MP2 audio (fuzz only) |
| `web` | `components/nv_web/nv_web_util.cpp` | URL decoding, logical→physical paths, write guards, JSON helpers |
| `auth` | `components/nv_auth/nv_auth_core.cpp` | pairing: token/cookie parsing, stored session list, PIN state machine |
| `ota` | `components/nv_ota/nv_ota_manifest.cpp` | signed OTA manifest fields and the signed message |
| `pkg` | `components/nv_appstore/nv_store_pkg.cpp` | store `package.sig`: signed file list, hashes, sizes |
| `netpol` | `components/nv_wasm/nv_net_policy.c`, `components/nv_mqtt/nv_mqtt_topic.c` | app network policy (LAN / internet destinations), MQTT topic filters |
| `ha` | `components/nv_mqtt/nv_ha_proto.c` | Home Assistant REST / WebSocket replies |
| `anima` | `components/nv_anima/*` (unit only) | the ANIMA engine end to end, offline: L0 commands, apps, settings, solver, reminders, profile memory |

## Running the tests

```sh
make                              # unit tests, 64 + 32 bit
make fuzz-run F=avi A=x86 S=300   # one fuzzer, 5 minutes (A=x64|x86)
make ci CI_SECS=60                # unit tests + every fuzzer on both arches
```

From Windows: `wsl make -C /mnt/d/NucleoV2/tests/host ci`.

Setup on Ubuntu: `sudo apt-get install clang gcc-multilib g++-multilib`.

## Seeds, finds and crashes

- **Seeds** live in `corpus/<target>` and are committed. The media seeds are tiny synthetic ffmpeg test
  patterns; to regenerate them, run `python gen_seeds.py` (needs ffmpeg).
- **Finds:** what the fuzzers discover goes to `out/<target>`, which is not committed and grows across
  runs.
- **Crashes:** a crash is saved as `out/<target>/<arch>-crash-*`. Replay it with
  `build/<arch>/fuzz_<target> <file>`, fix the code, then add a unit test next to the fuzzer.

## Adding a target

1. Move the parsing code into a pure module: no ESP-IDF, FreeRTOS or LVGL. Map the PSRAM allocator with
   `#ifdef ESP_PLATFORM`, as `vp_avi.c` does.
2. Add `unit/test_<name>.cpp` and `fuzz/fuzz_<name>.cpp`.
3. List the target's sources in the Makefile as `SRC_<name>`.
