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
| WASM apps | Run in the WAMR sandbox and reach the system only through the `nv` host ABI. Sensitive capabilities (internet, local devices, MQTT, Home Assistant, ...) are declared in the app manifest and can be revoked per app in Settings → Security. |
| Network API / web companion | Every `/api` and `/ws` request needs a session token. A client gets one only by entering a 6-digit code shown on the device screen; wrong codes lock pairing with exponential back-off. Only SHA-256 hashes of tokens are stored, and sessions can be revoked from the device. The session cookie is `HttpOnly; SameSite=Strict`; API calls whose `Host` is not an IP or a local name are refused (DNS rebinding), state-changing requests and WebSocket upgrades must carry a same-origin `Origin` (CSRF), and every response forbids framing and MIME sniffing. |
| Stored settings and secrets | Firmware built with `CONFIG_NVS_ENCRYPTION` (tested on hardware, being rolled out; off in the released builds until then) encrypts the NVS store (Wi-Fi passwords, pairing sessions, PINs, API tokens) with XTS-AES. The keys are derived by the HMAC peripheral from a random 256-bit key the chip generates on its first boot and burns into a read-protected eFuse block, so it never leaves the chip. The settings mirror on the SD card is sealed with AES-256-GCM under a key from the same eFuse secret: it restores on the same unit only. Secret files on the card (the AI provider API keys in `data/anima/teacher.json`) are sealed the same way; a plaintext one left by older firmware is sealed on its first read. Paired clients still read and write them in clear through `/api/fs`. |
| Second Screen | NucleoCast (ports 7070/7443) asks on the device before a new browser may show its screen and receive touches ("ask" is on by default; a device can be remembered). A VNC reverse connection (port 5500, listening only while the Second Screen app is open) waits for the same on-screen approval. |
| Lock screen | Optional 4-digit PIN. After 5 wrong PINs the pad pauses for 30 s, doubling up to 15 min; the count survives a reboot. |
| Security event log | Failed and successful pairings, lockouts, revoked devices, refused firmware and apps, the settings-encryption state: Settings → Security and `GET /api/security/events` (paired clients). Kept in RAM since the last boot. |
| Firmware hardening | Stack-smashing protector (`-fstack-protector`) and the hardware stack guard catch stack overflows with a backtrace instead of silent corruption. |
| Wi-Fi | WPA2/WPA3-SAE. |
| KeyDeck (LAN keyboard) | Off by default; while on it accepts keystrokes from the local network without authentication. |

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
- **AOT app modules are native code.** Install apps from the signed Store or from sources you trust.

## Threat model in one line

NucleoOS protects a device on a home network from other devices on that network and from tampered
updates or apps. It does not protect a device an attacker holds in their hands.
