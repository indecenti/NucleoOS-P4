# Hands-free ANIMA (wake word)

Say the wake word, then your question: the device chimes, opens ANIMA, records until you
stop talking, transcribes, answers and (if a voice is installed) reads the answer aloud.

## How it works
- **Always-on listening, offline**: ESP-SR WakeNet on the onboard microphone (48 kHz → 16 kHz).
  Nothing leaves the device until you hear the confirmation sound.
- **Automatic pause** while the device plays audio (music, voice, video) or records:
  this way it does not trigger itself.
- **End of the question**: it stops after ~1 s of silence (12 s at most); if nobody speaks within
  5 s it cancels.
- **Transcription**: first the home Whisper server (web Settings ▸ AI ▸ Voice transcription at home (Whisper)),
  then the cloud key; in *Local* mode never the cloud.

## Enabling it in the firmware (once)
The feature is optional in the build (`CONFIG_NV_WAKE_ESP_SR`, off by default):

    cd components/nv_wake && idf.py add-dependency "espressif/esp-sr^2.1" && cd ../..
    idf.py menuconfig    # NucleoOS wake word ▸ Hands-free ANIMA: ON
                         # ESP Speech Recognition ▸ Load Multiple Wake Words: pick the words
    idf.py build flash   # ESP-SR writes the models to the "model" partition

ESP-SR reads the models from a `model` partition (data, spiffs). The v2 layout of `partitions.csv` does not
have one: when you enable the option, carve it out of the reserved `assets` area (e.g. the first 2 MB:
`model, data, spiffs, 0xB20000, 0x200000` and `assets` starting at `0xD20000` with `0x2A0000`), in agreement
with whoever uses `assets`. Without the option nothing is needed.

Without the option everything builds as before and Settings explains that the feature is not in the build.

## Where to configure it
- **Device**: Settings ▸ Anima AI ▸ Hands-free voice (switch, word, sensitivity,
  live status, where the voice goes). In the ANIMA app: `/wake`, `/wake on|off|low|normal|high`.
- **Web**: Settings ▸ AI ▸ Hands-free voice (same thing, refreshed every 3 s).
- **API**: `GET/POST /api/anima/wake` `{on, word, sens}`; the status is also in `device_status` of the
  MCP server (`tools/anima_mcp.py`).

## To verify on the board
The core (decimator, states, end of question) is tested on the PC (`tests/host`, unit `wake`); the hook
into ESP-SR (`wake_esp_sr.c`) must be tried on the device: model names, real sensitivity and
CPU use with WakeNet9 on the P4.
