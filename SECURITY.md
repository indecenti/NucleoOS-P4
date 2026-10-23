# Security

## Reporting a vulnerability

Please report security problems privately, not in a public issue:

- GitHub: [open a private security advisory](https://github.com/indecenti/NucleoOS-P4/security/advisories/new)
- or email **niki070585@gmail.com** with "SECURITY" in the subject

Include the firmware version (Settings → About), what an attacker needs (network access, physical
access, an installed app, ...) and the steps to reproduce. You should get an answer within a week.
Please give a fix time to ship over OTA before publishing details.

## Supported versions

Only the latest firmware release gets fixes. Devices update themselves over the air, so a fix
reaches every board that is online.

## What is protected

| Area | Protection |
|---|---|
| Firmware updates (OTA) | Manifest signed with ECDSA P-256; the image is installed only if its SHA-256, size and embedded version match the signed manifest, and only if it is newer than the running one. A new image that does not survive 60 s is rolled back by the bootloader. "Install from SD" follows the same rule: the image needs the release's signed `nucleos-anima.json` beside it. |
| Store apps | Every package carries a signature from a separate store key; unsigned packages are refused (unless the developer switch in Settings → Security is turned on). |
| WASM apps | Run in the WAMR sandbox and reach the system only through the `nv` host ABI. Sensitive capabilities (internet, local devices, MQTT, Home Assistant, ...) are declared in the app manifest and can be revoked per app in Settings → Security. Every host function that takes guest memory has WAMR validate the pointer and length. Linear memory is capped at the app's RAM budget. MQTT subscriptions cannot reach the board's own `nucleo/` tree. Terminal programs never reach the system folders of the card (`nucleos/`, `system/`, `web/`, `data/anima/`) or change an installed package, whatever folder they were opened in. |
| AOT app modules | An `app.aot` is native code, outside the sandbox. It runs only when the store-signed `package.sig` kept beside it lists exactly its bytes (SHA-256 checked over the bytes about to run); anything else runs from `app.wasm` in the interpreter. The developer switch lifts this. |
| Network API / web companion | Every `/api` and `/ws` request needs a session token. A client gets one only by entering a 6-digit code shown on the device screen; wrong codes lock pairing with exponential back-off. Only SHA-256 hashes of tokens are stored, and sessions can be revoked from the device. The session cookie is `HttpOnly; SameSite=Strict`; API calls whose `Host` is not an IP or a local name are refused (DNS rebinding), state-changing requests and WebSocket upgrades must carry a same-origin `Origin` (CSRF), and every response forbids framing and MIME sniffing. |
| Stored settings and secrets | Firmware built with `CONFIG_NVS_ENCRYPTION` (tested on hardware, being rolled out; off in the released builds until then) encrypts the NVS store (Wi-Fi passwords, pairing sessions, PINs, API tokens) with XTS-AES. The keys are derived by the HMAC peripheral from a random 256-bit key the chip generates on its first boot and burns into a read-protected eFuse block, so it never leaves the chip. The settings mirror on the SD card is sealed with AES-256-GCM under a key from the same eFuse secret: it restores on the same unit only. Secret files on the card (the AI provider API keys in `data/anima/teacher.json`) are sealed the same way; a plaintext one left by older firmware is sealed on its first read. Paired clients still read and write them in clear through `/api/fs`. |
| Second Screen | NucleoCast (ports 7070/7443) asks on the device before a new browser may show its screen and receive touches ("ask" is on by default; a device can be remembered). A VNC reverse connection (port 5500, listening only while the Second Screen app is open) waits for the same on-screen approval. |
| Lock screen | Optional 4-digit PIN. After 5 wrong PINs the pad pauses for 30 s, doubling up to 15 min; the count survives a reboot. |
| Security event log | Failed and successful pairings, lockouts, revoked devices, refused firmware and apps, the settings-encryption state: Settings → Security and `GET /api/security/events` (paired clients). Kept in RAM since the last boot. |
| Firmware hardening | Stack-smashing protector (`-fstack-protector-strong` on the components that parse network, file, package and WASM input; `-fstack-protector` elsewhere, where STRONG would cost scarce internal RAM) and the hardware stack guard catch stack overflows with a backtrace instead of silent corruption. PMP keeps internal-RAM code read+execute only and internal-RAM data non-executable. The parsers of untrusted input are fuzzed on every CI run (libFuzzer, 64- and 32-bit): AVI, MP4, MPEG-1, JPEG, ZIM, HTML, the web API helpers, pairing, OTA manifests, firmware images, store packages, content packs, downloads, the DMD protocol, Home Assistant and MQTT policy. |
| Wi-Fi | WPA2/WPA3-SAE. |
| KeyDeck (LAN keyboard) | Off by default; while on it accepts keystrokes from the local network without authentication unless a PIN is set. With a PIN, 5 wrong tries pause connections for 30 s, doubling up to 15 min. |

## Known limitations

Stated here so nobody relies on a protection that is not there:

- **No flash encryption, no Secure Boot.** Settings and secrets can be encrypted (see above), but the
  firmware itself is stored in clear and anyone with physical access and a USB cable can flash
  their own. Both features burn eFuses irreversibly and disable plain USB flashing, so they are
  left to whoever builds a product; eFuse blocks KEY0-KEY4 stay free for them (NVS uses KEY5).
  Consequence: the settings encryption defeats a flash dump or a copied SD card, but a firmware
  someone flashes onto the board can still ask the HMAC peripheral to derive the keys. Only Secure
  Boot closes that.
- **Settings encryption is one-way.** Once a board has run a firmware with NVS encryption, an
  older firmware flashed onto it cannot read its settings (they are reset, then restored from
  the SD mirror only if that mirror is still the plaintext kind).
- **The web companion is plain HTTP.** On a shared or untrusted network the session token can be
  sniffed. Use it on a network you trust.
- **Native code shares one privilege domain.** The ESP32-P4 has no MMU for separate processes, and
  ESP-TEE does not support it (ESP-IDF 5.5). Everything native runs in RISC-V machine mode in one
  address space: the kernel, the network services and the decoders of untrusted input (MJPEG, MPEG-1,
  the AVI/MP4 demuxers, PNG/JPEG/BMP images, MP3, the VNC client, the NucleoCast server, the HTTP
  server). A memory bug in any of them is a bug in the whole firmware. Fuzzing and the stack
  protector reduce the odds; they are not a containment boundary. Only WASM apps are contained.
- **PSRAM is executable.** AOT code is loaded there, so the PMP leaves the whole PSRAM range
  readable, writable and executable; heap buffers in PSRAM have no W^X.
- **Secret files are sealed only with settings encryption.** On a board without the eFuse key
  (every released build today), `data/anima/teacher.json` and the other secret files stay in clear
  on the card, and the settings mirror `nucleos/settings.nvb` holds the Wi-Fi passwords in clear.
- **Not every parser is fuzzed yet.** The NucleoCast and VNC parsers, ANIMA's streamed (SSE) and
  JSON answers from online models, the third-party image and audio decoders (LodePNG, TJPGD,
  minimp3) and the prebuilt Espressif codecs (H.264, AAC/M4A/FLAC) are not.

## Isolation roadmap

What would turn the mitigations above into containment, in order:

1. **Parsers into WASM.** Move the demuxers, image and audio decoders and other format parsers that
   do not need hardware acceleration into WASM modules run by the existing sandbox. Native code
   keeps only the hardware paths (JPEG decoder, PPA, H.264) behind a small header parser that is
   fuzzed. A malformed file can then crash a sandbox, not take over the board.
2. **Fuzz what is still unfuzzed** (list above), with the same CI gate.
3. **AOT compiled by the store.** Build every `app.aot` on the store side from the submitted
   `app.wasm` with bounds checks pinned on, so the signature also vouches for how it was compiled.
4. **Non-executable PSRAM data.** Give AOT code its own PSRAM region and mark the rest of PSRAM
   read+write only with the PMP.
5. **User mode for drivers and services.** The P4 cores do have U-mode, PMP (16 entries) and
   per-peripheral and DMA access control, but ESP-IDF uses none of it on this chip: it locks all
   16 PMP entries at boot, and the RTOS port has no user-mode tasks. This is the long-term item.

## Threat model in one line

NucleoOS protects a device on a home network from other devices on that network and from tampered
updates or apps. It does not protect a device an attacker holds in their hands.
