# openHASP

## What it is for

openHASP turns the panel into a touch interface for **Home Assistant**. It is the HASwitchPlate community project: pages (buttons, switches, sliders, gauges, clocks, icons) are described in a JSONL file and driven over MQTT. If you already use openHASP with Home Assistant, the same pages and the same integration work here.

## Before you start

1. An MQTT broker, usually Home Assistant's **Mosquitto broker** add-on.
2. On the panel: **Settings > Home**, enter the MQTT broker, user and password and turn the connection on.
3. In Home Assistant install the **openHASP** integration from HACS (`HASwitchPlate/openHASP-custom-component`).

## How to use it

- Open **openHASP**: the panel announces itself on MQTT as **plate** (topics `hasp/plate/...`).
- Home Assistant finds it: add it and point the integration to your pages file (`pages_jsonl`). The integration pushes the pages and links objects to entities.
- Without Home Assistant, copy a **pages.jsonl** to `/sdcard/apps/openhasp/data/` (Files app or web companion) and reopen the app.
- The **back** gesture goes to the previous page; on the first page it closes the app.

### Optional configuration

`/sdcard/apps/openhasp/data/config.json` sets node name, group, start page and time zone:

```json
{"mqtt": {"name": "kitchen", "group": "home"}, "time": {"zone": "CET-1CEST,M3.5.0,M10.5.0/3"}}
```

### Quick test from a PC

```
mosquitto_pub -t hasp/plate/command/p1b2.text -m "Hello"
mosquitto_pub -t hasp/plate/command/page -m 2
```

## Limits

- Images from URLs (`http://...`) and the `push_image` service are not supported: copy PNG images to the data folder and use `L:/name.png`.
- No openHASP GPIO, relays, web UI, OTA or Wi-Fi: networking, updates and MQTT come from the system.
- `reboot` and `restart` do nothing.

## Credits

openHASP (MIT) © openHASP contributors, <https://github.com/HASwitchPlate/openHASP>. LVGL 7 (MIT), ArduinoJson (MIT), FreeType (FTL: "Portions of this software are copyright © The FreeType Project (www.freetype.org). All rights reserved."). Full license texts are in the app's `LICENSE.txt`.
