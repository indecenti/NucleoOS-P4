# Ready-to-post announcements

Copy-paste these to get NucleoOS P4 in front of people. Post when you have a spare hour to
answer comments (engagement in the first few hours drives GitHub Trending). Attach the demo GIF.

Repo: https://github.com/indecenti/NucleoOS-P4

---

## Hacker News — "Show HN"

**Title:**
`Show HN: NucleoOS P4 – a custom OS for a $40 ESP32-P4 7" touchscreen`

**Body:**
> I built a small operating system for the Guition JC1060P420C — an ESP32-P4 board with a 7"
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
> This runs on the Guition JC1060P420C (ESP32-P4 + C6, 7" 1024×600). Features: LVGL launcher,
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
