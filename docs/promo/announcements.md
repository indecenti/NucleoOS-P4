# Ready-to-post announcements

Copy-paste these to get NucleoOS P4 in front of people. Post when you have a spare hour to
answer comments (engagement in the first few hours drives GitHub Trending). Attach the demo GIF.

Repo: https://github.com/indecenti/NucleoOS-P4

---

## Hacker News — "Show HN"

**Title:**
`Show HN: NucleoOS P4 – a custom OS for a $40 ESP32-P4 7" touchscreen`

**Body:**
> I built a small operating system for the Guition JC1060P470C — an ESP32-P4 board with a 7"
> 1024×600 touchscreen. It has an LVGL launcher, native apps (settings, files, camera, gallery,
> music, video, system monitor, an offline assistant), a sandboxed WASM app runtime so games/tools
> ship as .wasm files on the SD card, offline text-to-speech (Italian/English), a web companion
> served over Wi-Fi, and over-the-air updates.
>
> It targets ESP-IDF v5.5.2, LVGL 9, FreeRTOS and WAMR. Everything runs on-device — no cloud.
>
> Demo GIF and details in the README. Happy to answer anything about the ESP32-P4, the RAM budget,
> the WASM sandbox, or the HW JPEG/PPA pipeline.

Post at https://news.ycombinator.com/submit — link to the repo, put the note in the text field.

---

## Reddit — r/esp32 (also r/embedded)

**Title:**
`I wrote a full touchscreen OS for the ESP32-P4 (launcher, apps, WASM runtime, OTA)`

**Body:**
> This runs on the Guition JC1060P470C (ESP32-P4 + C6, 7" 1024×600). Features: LVGL launcher,
> native apps (camera, gallery with HW JPEG decode, music, MJPEG/MPEG-1 video, system monitor,
> offline assistant), a WASM app store (games/tools as .wasm on SD), offline TTS (it/en), a Wi-Fi
> web companion, and OTA updates. ESP-IDF v5.5.2 / LVGL 9 / FreeRTOS / WAMR.
>
> Repo + demo GIF: https://github.com/indecenti/NucleoOS-P4
> Free for noncommercial use. Feedback welcome.

---

## LVGL forum — "My projects / Showcase"

**Title:** `NucleoOS P4 — a touchscreen OS on ESP32-P4 built with LVGL 9`

> Sharing a project built on LVGL 9: a small OS for a 7" ESP32-P4 board — launcher, ~15 apps,
> gestures, theming/i18n (5 languages), a WASM app surface, and a web companion. LVGL drives the
> whole UI at 1024×600 with the P4's PPA for scaling and HW JPEG for images.
> Repo + GIF: https://github.com/indecenti/NucleoOS-P4

Board: https://forum.lvgl.io/  (category: My projects)

---

## Hackaday tip line

Short note + the repo link + demo GIF to https://hackaday.com/submit-a-tip/
> "Custom touchscreen OS for the $40 ESP32-P4 7\" board: LVGL launcher, ~15 apps, a WASM app store,
> offline TTS, and OTA. Open (noncommercial). <repo link>"

---

## awesome-esp32 (and awesome-embedded) — pull request

Add a line under the relevant section of https://github.com/agucova/awesome-esp32 (or the most
active fork) and https://github.com/nhivp/Awesome-Embedded :

```
- [NucleoOS P4](https://github.com/indecenti/NucleoOS-P4) - A touchscreen OS for the ESP32-P4 (Guition 7"): LVGL launcher, native + WASM apps, offline TTS, web companion, OTA.
```

---

## esp32.com forum

Post in "ESP-IDF / Projects" with the same Reddit body + repo link.

---

### Tips
- Lead every post with the **demo GIF** — it's the hook.
- Reply fast to the first comments; that window decides whether you hit Trending.
- Don't cross-post everything in the same 10 minutes; space HN / Reddit / forums over a day or two.
- Never buy stars/followers — GitHub flags it and it kills ranking.

---

## Channel plan (ordered by expected return)

People who **already own the board** convert best — go where they are first.

| # | Channel | Why | Angle |
|---|---|---|---|
| 1 | Home Assistant community — thread "Guition esp32-p4-jc1060p470" (community.home-assistant.io/t/959144) | 50+ posts of JC1060P470 owners fighting ESPHome/MIPI-DSI | reply: "if you want the board as a standalone device, here is a full OS" + GIF |
| 2 | r/esp32 | biggest ESP hobby sub, P4 posts do well | Reddit post above, title with "JC1060P470" |
| 3 | GitHub issues of other P4 projects (LovyanGFX #803, ESP32-MiniWebRadio #786) | owners searching support land there | only if on-topic (pin map / JD9165 init that works) — no spam |
| 4 | atomic14.com board page (Chris Greening) | ranks #1 for "JC1060P470" | email/issue asking to list NucleoOS under "firmware" |
| 5 | LVGL forum "My projects" | LVGL team reposts showcases | LVGL post above |
| 6 | esp32.com forum → ESP32-P4 / Showcase | Espressif staff read it | Reddit body |
| 7 | Hackaday tip line + cnx-software (tip form) | cnx covered this exact board in 05/2025 | "custom OS for the $37 Guition 7" P4" |
| 8 | Show HN | reach, Trending | HN text above |
| 9 | r/embedded, r/WebAssembly (WAMR + AOT on RISC-V), r/osdev, r/RISCV | niche tech angles | one post each, different angle, days apart |
| 10 | WASM-4 Discord / r/WASM4 | 149 carts run on the device | "WASM-4 handheld on a 7" P4" video |
| 11 | r/homeassistant, r/selfhosted | dashboard crowd | only after a HA/MQTT tile app exists |
| 12 | Italian: r/ItalyInformatica, forum.arduino.cc sezione italiana | native audience | IT post |
| 13 | YouTube short (60 s tour) + X/Mastodon #ESP32 #LVGL | video gets embedded everywhere | reuse demo.gif footage |

Notes
- r/opensource / r/linux: skip or disclose clearly — PolyForm Noncommercial is **source-available,
  not OSI open source**; calling it "open source" there gets the post removed.
- Every post: exact string "Guition JC1060P470C" in the title or first line (that is what people search).
- Link a flashable binary, not only "build with ESP-IDF" — owners want to flash, not compile.
