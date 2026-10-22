# Plan: home automation, networking for apps, store for third parties

Status (2026-09-30): plan based on 7c0485e (1.1.127). **F1 implemented** (not yet verified on HW):
`components/nv_mqtt` (service + pure `nv_ha_proto` with unit test and `tests/host` fuzzer `ha`),
Settings > Home, `nv_ui_screen_sleep/wake/is_asleep`, `LWIP_MAX_SOCKETS` 24. Per-entity discovery
(not device-discovery: small payloads, 1 KB internal MQTT buffer). Network events = polling like keydeck
(no `NV_EV_NET_STATE` for now). TLS postponed (F0.1).
Decisions §10: the 4 recommendations were taken.

**F2 + F3 implemented** (firmware 1.1.132, to be verified on HW with `apps/net12`):
- Store: per-app `package.sig` (pure `nv_store_pkg` + test/fuzz `pkg`), separate store key,
  download to `.tmp` + final commit, dev switch `store_unsigned`; asset limits 96→256 (chess has 142).
- Permissions: bits `lan ws mqtt ha camera mic` (`nv_wasm_perms.cpp`), `perms` in the catalog, double-tap
  consent in the Store, revocation in Security (`/sdcard/nucleos/perms.json`), run = manifest − revoked.
- ABI v12 (`nv_wasm_net.cpp`): `http_req/state/status/read/close`, `ws_open/state/send/recv/close`
  (on `esp_transport_ws`, no new dependencies), `mqtt_sub/pub/recv` via `nv_mqtt` (PSRAM queue,
  the app thread never touches the client), `ha_available/ha_req/ha_ws` (token host-side only).
  Pure policy `nv_net_policy.c` + `nv_mqtt_topic.c` (test/fuzz `netpol`).
- Settings > Home: HA URL + token; `/api/home` GET/POST to paste them from a PC. `ha_token` and
  `mqtt_pass` excluded from the SD backup.
- Not done: `hosts` allowlist in the manifest, TLS in PSRAM (F0.1), `mdns_browse` for apps.

**ANIMA + shell (2026-10)**: commands `ha` (Assist, lists filtered with /api/template, services) and `dev`
(Shelly Gen2/Gen1, Tasmota, WLED, mDNS discovery) in `nv_apps/term_sh.cpp`; skill `casa.md`; ANIMA's
automations can call them; `ha_state` events in the automations (5 s polling with
/api/template, only of the watched entities). **Home app** (F4, `apps/casa`, ABI v12 + `ha`): per-room
tiles from a single /api/template, toggle, brightness, thermostat, scenes; catalog `smarthome/ha`.
To be verified on HW.

Goal: make NucleoOS "the panel for Home Assistant" and open the store to third-party apps
without breaking the memory/security rules of `ENGINEERING_RULES.md`.

---

## 0. Current situation (facts, with references)

| Area | Today | Limit |
|---|---|---|
| HTTP for apps | `nv.http_get` (`nv_wasm.cpp:1070-1110`): GET only, no headers, HTTPS with cert bundle, synchronous up to 10 s on the worker | no POST/PUT/headers → cannot talk to HA, Shelly, Tasmota. Abort does not interrupt it (`nv_wasm.h:295`) |
| Network for apps | UDP `net_*`, 1 global socket (`s_exec.net_fd`), closed in collect | no TCP, WebSocket, MQTT |
| Permissions | bits `LOG/UI/NET/FS/GFX/HOME` (`nv_wasm.h:140`), read from the manifest; `NET` covers everything; `FS` protects nothing (BACKLOG 108) | no user consent, no destination restriction |
| ABI | `NV_WASM_ABI 10`; SDK `NUCLEO_SDK_ABI 9` + comment "v7" **stale** (`sdk/include/nucleo_sdk.h:21`) | — |
| Background | none: a module does not outlive its UI (`apps_app.cpp:80`) | no app widgets/services |
| Store | no sha256 or signature on files (BACKLOG 32-34), only magic+size | unacceptable for third-party apps |
| Signature | `manifest_verify` ECDSA P-256 in `nv_ota.cpp:84-115` (anonymous namespace) | reusable if extracted |
| MQTT/WS client | absent (`esp-mqtt` is in IDF, not linked) | — |
| TLS | mbedTLS in internal SRAM, IN 16 KB + OUT 4 KB, ~40 KB per handshake (BACKLOG 43-46); free SRAM ~200-240 KB | every persistent TLS connection is expensive |
| Sockets | `LWIP_MAX_SOCKETS 20`, httpd uses 8 | budget to be redone |
| Services | `nv_service_mgr` exists but nobody uses it; real pattern = keydeck (task + `nv_config` toggle + `NV_EV_SETTINGS_CHANGED`) | — |
| Network events | no bus event for Wi-Fi/Eth; `nv_wifi_get_state()` is polled | — |
| Exposable state | backlight, volume/mute, DND, lock, TTS, notifications (ring 8, LVGL thread only), sysmon (temp/heap/uptime/cpu), Wi-Fi link, screenshot, camera JPEG | `screen_sleep_now/screen_wake` are internal to `nv_ui.cpp:4040`, not public |

**Numbering conflict:** the gamepad work (`nv_pad`, BLE HOGP) has reserved **ABI v11**.
The network imports of this plan are therefore **ABI v12**, unless agreed otherwise.

---

## 1. Two-level architecture

```
                    ┌──────────────────────────────┐
  Home Assistant ◄──┤ nv_mqtt  (firmware service)  │◄── Settings > Home / web companion
  (MQTT broker)     │  - HA discovery (device)     │
                    │  - commands → OS             │
                    │  - bridge for apps (v12)     │
                    └──────────────┬───────────────┘
                                   │ PSRAM queue
  WASM app (store) ── nv.mqtt_* ───┘
                  ── nv.http_*  ──► nv_netreq (host worker, abortable)
                  ── nv.ws_*    ──► esp_websocket_client
                  ── nv.ha_*    ──► HA proxy with system token (the app does not see the token)
```

Reason: "useful" home automation must work **always**, even with a game open or the screen off.
WASM apps cannot run in the background (solo mode), so the basic HA integration is
firmware; apps add dashboards and rich UI only while they are open.

---

## 2. Phase F0 — prerequisites (small, to do first)

1. **mbedTLS out of internal SRAM.** Try `CONFIG_MBEDTLS_EXTERNAL_MEM_ALLOC=y`
   (or `MBEDTLS_CUSTOM_MEM_ALLOC` with a threshold: blocks ≥ 4 KB in PSRAM, small ones internal)
   + `CONFIG_MBEDTLS_DYNAMIC_BUFFER=y`.
   HW verification: free internal SRAM before/after (`/api/heap`), Anima online, OTA, store, handshake
   time. If it holds, it unlocks persistent TLS (MQTT on 8883, HA via https, ws). Goes into
   `sdkconfig.defaults` (§8). Without this: MQTT/WS **plaintext on LAN only** and at most 1 TLS
   session at a time via `nucleo_arb`.
2. **`NV_EV_NET_STATE` event** on the bus (up/down, Wi-Fi or Eth). Caution: `wifi_evt` has a 2304
   stack and the publish is synchronous → the IDF handler only does `xTaskNotify`/flag toward a small task
   that publishes on the bus. Useful also to nv_web (today it only waits for Wi-Fi, not Eth).
3. **Public screen API:** `nv_ui_screen_sleep_async()`, `nv_ui_screen_wake_async()`,
   `nv_ui_screen_is_on()` in `nv_ui.h` (wrappers of `screen_sleep_now/screen_wake` via `lv_async_call`).
4. **`nv_sig`**: extract `manifest_verify` from `nv_ota.cpp` into a pure module
   (`nv_sig_verify(prefix, fields…, sig_b64)`), host-tested. Used by OTA and store.
5. **Socket budget:** `LWIP_MAX_SOCKETS` 20 → 24 (httpd 8, mDNS/keydeck/cast ~4, mqtt 1,
   apps max 4, OTA/store/anima 2-3, margin). In `sdkconfig.defaults.esp32p4`.
6. Fix `NUCLEO_SDK_ABI` and the comment in the SDK header.

---

## 3. Phase F1 — `nv_mqtt` + Home Assistant MQTT Discovery (firmware)

This is the most visible feature: the user enters the broker host and the panel appears in HA with
all the entities, without YAML.

### 3.1 Component
- `components/nv_mqtt` on IDF's `esp-mqtt` (`REQUIRES mqtt`), keydeck pattern:
  `nv_mqtt_init()` reads `mqtt_en`, subscribes to `NV_EV_SETTINGS_CHANGED` and `NV_EV_NET_STATE`,
  starts/stops the client. Reconnection is done by esp-mqtt.
- esp-mqtt task: 6 KB **internal** stack (touches `nv_config` and is not infinite-lived → rule §2).
  RX/TX buffers 2 KB; messages over 4 KB discarded.
- NVS keys (≤15 characters): `mqtt_en`, `mqtt_host`, `mqtt_port`, `mqtt_user`, `mqtt_pass`,
  `mqtt_tls`, `mqtt_pfx` (default `homeassistant`), `mqtt_node` (default `nucleo_<mac6>`).
- Password: never in `NV_LOG*` (§6: the log is served by `/api/logs`), never in `/api/mqtt` in the response.
  Stays in NVS in plaintext like the Wi-Fi password (same exposure level). In the SD backup
  (`nv_backup`) **exclude it** or explicitly accept it.
- Auto broker: mDNS browse `_mqtt._tcp` and, if an HA host is found (`_home-assistant._tcp`),
  propose the same IP port 1883.

### 3.2 Topics
- State/commands: `nucleo/<node>/<entity>/state|set`, availability `nucleo/<node>/status`
  (LWT `offline`, retained; `online` on connection).
- Discovery: **device discovery** (a single retained config)
  `homeassistant/device/<node>/config` with `dev`, `o` (origin NucleoOS + version) and `cmps`.
  Requires HA ≥ 2024.11; per-entity fallback (`homeassistant/<comp>/<node>/<obj>/config`) behind
  a flag, if old compatibility is needed.
- Subscribe to `homeassistant/status`: on `online` republish discovery + states.

### 3.3 Entities (v1)
| HA entity | Type | OS side |
|---|---|---|
| Screen | `light` (on/off + brightness 0-100) | on/off = sleep/wake (F0.3), brightness = `nv_hal_backlight_set` + `nv_config "brightness"` |
| Volume | `number` 0-100 | `nv_audio_set_volume` + persist |
| Mute | `switch` | `nv_audio_set_mute` |
| Do not disturb | `switch` | config `qs_dnd` |
| Lock | `switch` (remote ON only; unlocking stays on screen) | `nv_ui_lock` |
| Notification | `notify` | `nv_notify_post` via `lv_async_call` |
| Speak | `notify` (second one) or `text` | `nv_tts_say` |
| Open app | `select` (options = installed apps) + current app `sensor` | `nv_ui_open_app_id_async`, `nv_ui_current_app_id` |
| Home | `button` | `nv_ui_go_home_async` |
| Restart | `button` (config category) | reboot |
| Update | `update` (installed/latest from the OTA manifest) | `nv_ota` — installation from HA only if the user enables it |
| Chip temperature, RSSI, uptime, free internal RAM/PSRAM, IP | diagnostic `sensor` | `nv_sysmon_perf/mem`, `nv_wifi_get_link` |
| Recent touch | `binary_sensor` (presence) — touch in the last N s | last touch timestamp in nv_hal |
| Snapshot | `camera` (image on topic) — **v2**, on request | `nv_camera_save_jpeg` / `nv_hal_screenshot` |

State publishing: `NV_EV_SETTINGS_CHANGED` event on the relevant keys → "dirty" bit
(the callback is synchronous on the writer's task: no I/O there) → the mqtt task publishes.
Diagnostic sensors every 60 s.

### 3.4 Command security
- Fixed and small command surface: no fs, no install, no `/api/ui/*` via MQTT.
- Payload parser in a pure module (`nv_mqtt_cmd.c`) + tests and fuzzer in `tests/host` (§6).
- Rate limit on notify/TTS (e.g. 1/s, burst 5); texts truncated to 96/200 characters.
- New web routes: `GET/POST /api/mqtt` (config, connection state) — authenticated by default;
  raise `max_uri_handlers` (today 72, ~62 used).

### 3.5 UI
- New Settings category **"Home"** (`kCats[]` + `cat_home()` in `settings_app.cpp`):
  toggle, host/port, user/password, status ("Connected to 192.168.x.x · 23 entities"),
  "Publish again to Home Assistant" button. `NV_STR_*` strings IT/EN.
- Equivalent page in the PWA (`/sdcard/web`): typing host and password from a PC is more convenient.

Release: OTA 1.1.12x. Verification: real HA + `mosquitto_sub -v -t 'nucleo/#'`.

---

## 4. Phase F2 — store ready for third parties (integrity + permissions)

To be done **before** accepting apps that are not ours.

### 4.1 Integrity
- `store.json` carries for each app `files: [{path, size, sha256}]`.
- Catalog signature: `store.sig` = ECDSA P-256 over `"nucleoos-store-v1\n<sha256(store.json)>\n"`,
  same OTA key (private only on the release PC) or, better, a **second key dedicated** to the
  store (a store compromise must not allow firmware).
- `install_package`: streaming sha256 while writing to SD; mismatch → delete the temporary
  folder and refuse. Write to `/sdcard/apps/.tmp-<id>` + final rename (today the manifest
  is written last but old files are overwritten in place).
- Signature generated by `tools/dist.py store` (already writes sha256 at `:256`).
- Dev sideload (`push_app.ps1`, `/api/fs/write`) remains possible: apps marked "developer"
  in the UI.

### 4.2 Permissions with consent
- New permissions in the manifest: `net` (stays, HTTP/UDP to the Internet), `lan` (private addresses),
  `ws`, `mqtt`, `ha`, `camera`, `mic`. Plus `hosts: ["api.open-meteo.com", …]`: if present,
  `http_*`/`ws_*` refuse other hosts (check on the host resolved from the URL, not on the string).
- Store card: list of permissions in plain language **before** Install; an update that adds
  permissions → requires new consent.
- Grants in `/sdcard/apps/<id>/.grants` (add to the `nv_save` blacklist, `nv_wasm.cpp:877`)
  or in NVS per app; `RunReq.perms = manifest ∩ grants`.
- Settings > Security > **App permissions**: app list, revocation per permission.
- Actually enforce `NV_WPERM_FS` (BACKLOG 108).

### 4.3 Publication by third parties
- Proposed model: public repo `nucleoos-apps` with a folder per app (source + manifest +
  GUIDE.md + license); PR → CI builds with the SDK (reproducible build, no binaries supplied
  by the author) → we sign and publish. License declaration mandatory in the manifest.
- Public SDK + ABI documentation on Pages (today `docs/WASM_APPS.md` is internal).

---

## 5. Phase F3 — ABI v12: networking for apps

Principles: no calls that block the guest for seconds (abort does not interrupt them today);
everything via **handles** with non-blocking polling, work done by a host worker; all resources
registered per-run and closed in `nv_wasm_exec_collect` (add a small cleanup registry
instead of another manual closure).

### 5.1 Full HTTP
```c
int  nv_http_req(const char *spec_json, const void *body, int body_len); // → handle ≥0 | <0 err
int  nv_http_state(int h);            // 0 in progress, 1 done, <0 error; HTTP status in nv_http_status
int  nv_http_status(int h);
int  nv_http_read(int h, void *buf, int cap);  // progressive copy from the buffered response
void nv_http_close(int h);
```
- `spec_json`: `{"method":"POST","url":"…","headers":{"Content-Type":"application/json"},"timeout":8000}`
  parsed by a pure module with limits (≤ 16 headers, ≤ 2 KB), fuzzed.
- `nv_netreq` worker (1 task, internal stack 6 KB — TLS; queue 4): max **2 handles per app**,
  response in PSRAM up to `min(256 KB, ram_budget/4)`.
- Abort: `esp_http_client` has a timeout; at collect the handle is marked "orphan" and the worker frees it
  at the end. The app exits immediately.
- `http_get` stays (compatibility) but implemented on top of the same worker.

### 5.2 WebSocket client
- `espressif/esp_websocket_client` pinned `==` (build rule: pinned components).
- `nv_ws_open(url$, headers_json$) → h`, `nv_ws_send(h, *~, text)`, `nv_ws_recv(h, *~) → n|0`,
  `nv_ws_state(h)`, `nv_ws_close(h)`. Max 1 per app, RX queue in PSRAM (64 KB ring, max message
  = queue; fragments reassembled by the client).

### 5.3 MQTT for apps (through the system connection)
- `nv_mqtt_sub(filter$)` (max 8), `nv_mqtt_pub(topic$, *~, retain)`, `nv_mqtt_recv(topic_buf*~, payload*~) → n`.
- No extra socket: goes through `nv_mqtt`. Publishing on `homeassistant/#`,
  `nucleo/<node>/#`, `$SYS/#` is forbidden. Subscriptions removed at collect.

### 5.4 Home Assistant proxy (`ha`)
- System config (Settings > Home): HA URL + **long-lived token**, saved once.
- `nv_ha_call(method$, path$, body*~, resp*~)` handle-based like http, `path` only under `/api/`;
  `nv_ha_ws_open()` opens the HA WebSocket with auth already done by the host.
- The app **never sees the token**: a store app can make an HA dashboard without asking the user
  for secrets. This is the reason to prefer the proxy over "ask for the token in the app".
- Warning: `get_states` on large installations exceeds 1 MB → the dashboard uses
  filtered `subscribe_entities` or `/api/states/<id>`.

### 5.5 Not in v12 (deliberately)
Raw TCP (large surface, few use cases that HTTP/WS/MQTT do not cover), per-app listening
server, background. Reevaluate later.

SDK: `nucleo_sdk.h` + convenient wrappers (`nv_http_post_json`, minimal JSON already in the `cjson` port).
Tests: `nettest` app extended to all imports; fuzz on spec/topic parsers.

---

## 6. Phase F4 — apps for the store

| App | Base | Notes |
|---|---|---|
| **Home** (HA dashboard) | `ha` proxy + ws | rooms/areas, lights (dimmer), climate, covers, scenes, sensors; 1024×600 tile layout. Flagship app |
| **Local devices** | `http_*` + `lan` | Shelly (Gen2 RPC), Tasmota (`/cm?cmnd=`), ESPHome (REST web server), WLED (`/json`). mDNS scan host-side? → needs a `mdns_browse` import (small, to be evaluated in v12) |
| **Cameras** | `http_*` | periodic JPEG snapshots (Frigate `/api/<cam>/latest.jpg`, HA `camera_proxy`); verify that `gfx_image` decodes JPEG from memory with HW JPEG; continuous MJPEG = v2 |
| **Web radio** | host | the audio stream must be handled by the native player (MP3/AAC exist) → `/api/media/play` with http URL + `open_url` import in the player. More firmware than WASM |
| **Game Boy emulator** | Peanut-GB (MIT) | gfx+audio+pad already existing; free homebrew example ROMs |
| **NES** | agnes (MIT) | same |
| **Calendar** | `http_*` | ICS from URL (Google/Nextcloud "secret address"), day/week view |
| **Utilities** | — | calculator, alarm, TOTP, converter, notes: to be done anyway, "complete OS" |

---

## 7. Phase F5 — product polish

- **Declarative widgets on the home** (no WASM in the background): the manifest declares
  `{"widget": {"type":"entity","source":"ha|mqtt","id":"sensor.soggiorno_temp"}}`, rendered natively by
  LVGL and updated by `nv_mqtt`/HA proxy.
- **Wall panel mode**: clock/weather screensaver, scheduled night brightness,
  wake on touch or MQTT (`light` on), fixed launch of an app.
- Notifications: ring from 8 → 32 + persistence on SD; actions on notifications (e.g. "Open camera").
- Guided initial setup (language → Wi-Fi → Home/MQTT → done).
- openHASP compatibility (JSON pages via MQTT) only if there is demand: protocol reimplemented,
  not GPL code.

---

## 8. Order, estimates, verification

| Phase | Content | Estimate | Release |
|---|---|---|---|
| F0 | PSRAM TLS (test), NV_EV_NET_STATE, screen API, nv_sig, sockets, SDK fix | 1-2 sessions | OTA |
| F1 | nv_mqtt + discovery + Settings Home + web page | 2-3 sessions | OTA, verification with real HA |
| F2 | signed store + sha256 + permission consent | 2 sessions | OTA + store |
| F3 | ABI v12 http/ws/mqtt/ha + SDK + fuzz | 3 sessions | OTA |
| F4 | Home, Devices, Cameras, emulators… | 1 session per app | store |
| F5 | widgets, kiosk, notifications, setup | to follow | OTA |

F1 does not depend on F2/F3: it can be released right after F0 and already gives the main value.

## 9. Risks

- **Internal SRAM**: the main one. Every client (mqtt task 6 KB, netreq 6 KB, ws task ~4 KB,
  TLS ~40 KB if not in PSRAM) must be measured on HW with Wi-Fi+LVGL+web active. Minimum free
  threshold to be set (e.g. 100 KB) and checked by `tools/hil/smoke.py`.
- Contention with other sessions: ABI v11 (gamepad), `nv_web.cpp` currently modified by another session.
- Remote commands = LAN attack surface: broker with mandatory user/password recommended;
  warning in the UI if the broker is anonymous.
- HA rarely changes the discovery schema but it happens: keep the generator in a tested pure module.

## 10. Open decisions (for Nicola)

1. Store signing key separate from the OTA one? (recommended yes)
2. MQTT password/HA token in the SD backup: exclude them? (recommended yes)
3. Third-party publication model: only sources compiled by our CI (recommended) or binaries signed by the author?
4. OTA installation commandable from HA (`update.install`): enabled by default or not? (recommended no)
