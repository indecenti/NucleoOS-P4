# Piano: domotica, rete per le app, store per terzi

Stato (2026-09-30): piano basato su 7c0485e (1.1.127). **F1 implementato** (non ancora verificato su HW):
`components/nv_mqtt` (servizio + `nv_ha_proto` puro con unit test e fuzzer `tests/host` `ha`),
Impostazioni > Casa, `nv_ui_screen_sleep/wake/is_asleep`, `LWIP_MAX_SOCKETS` 24. Discovery per-entità
(non device-discovery: payload piccoli, buffer MQTT 1 KB interno). Eventi rete = polling come keydeck
(niente `NV_EV_NET_STATE` per ora). TLS rimandato (F0.1).
Decisioni §10: prese le 4 raccomandazioni.

**F2 + F3 implementati** (firmware 1.1.132, da verificare su HW con `apps/net12`):
- Store: `package.sig` per app (`nv_store_pkg` puro + test/fuzz `pkg`), chiave store separata,
  download in `.tmp` + commit finale, dev switch `store_unsigned`; limiti asset 96→256 (chess ne ha 142).
- Permessi: bit `lan ws mqtt ha camera mic` (`nv_wasm_perms.cpp`), `perms` nel catalogo, consenso a
  doppio tocco nello Store, revoca in Sicurezza (`/sdcard/nucleos/perms.json`), run = manifest − revocati.
- ABI v12 (`nv_wasm_net.cpp`): `http_req/state/status/read/close`, `ws_open/state/send/recv/close`
  (su `esp_transport_ws`, niente dipendenze nuove), `mqtt_sub/pub/recv` via `nv_mqtt` (coda PSRAM,
  il thread app non tocca mai il client), `ha_available/ha_req/ha_ws` (token solo lato host).
  Policy pura `nv_net_policy.c` + `nv_mqtt_topic.c` (test/fuzz `netpol`).
- Impostazioni > Casa: URL + token HA; `/api/home` GET/POST per incollarli da PC. `ha_token` e
  `mqtt_pass` esclusi dal backup SD.
- Non fatto: allowlist `hosts` nel manifest, TLS in PSRAM (F0.1), `mdns_browse` per le app.

Obiettivo: rendere NucleoOS "il pannello per Home Assistant" e aprire lo store ad app di terzi
senza rompere le regole di memoria/sicurezza di `ENGINEERING_RULES.md`.

---

## 0. Situazione attuale (fatti, con riferimenti)

| Area | Oggi | Limite |
|---|---|---|
| HTTP per app | `nv.http_get` (`nv_wasm.cpp:1070-1110`): solo GET, niente header, HTTPS con cert bundle, sincrono fino a 10 s sul worker | niente POST/PUT/header → non si parla con HA, Shelly, Tasmota. L'abort non lo interrompe (`nv_wasm.h:295`) |
| Rete per app | UDP `net_*`, 1 socket globale (`s_exec.net_fd`), chiuso in collect | niente TCP, WebSocket, MQTT |
| Permessi | bit `LOG/UI/NET/FS/GFX/HOME` (`nv_wasm.h:140`), letti dal manifest; `NET` copre tutto; `FS` non protegge niente (BACKLOG 108) | nessun consenso utente, nessuna restrizione di destinazione |
| ABI | `NV_WASM_ABI 10`; SDK `NUCLEO_SDK_ABI 9` + commento "v7" **stantii** (`sdk/include/nucleo_sdk.h:21`) | — |
| Background | nessuno: un modulo non sopravvive alla sua UI (`apps_app.cpp:80`) | niente widget/servizi app |
| Store | nessun sha256 né firma sui file (BACKLOG 32-34), solo magic+size | inaccettabile per app di terzi |
| Firma | `manifest_verify` ECDSA P-256 in `nv_ota.cpp:84-115` (namespace anonimo) | riusabile se estratto |
| MQTT/WS client | assenti (`esp-mqtt` c'è in IDF, non linkato) | — |
| TLS | mbedTLS in SRAM interna, IN 16 KB + OUT 4 KB, ~40 KB per handshake (BACKLOG 43-46); SRAM libera ~200-240 KB | ogni connessione TLS persistente costa caro |
| Socket | `LWIP_MAX_SOCKETS 20`, httpd ne usa 8 | budget da rifare |
| Servizi | `nv_service_mgr` esiste ma nessuno lo usa; pattern reale = keydeck (task + toggle `nv_config` + `NV_EV_SETTINGS_CHANGED`) | — |
| Eventi rete | nessun evento bus per Wi-Fi/Eth; si fa polling `nv_wifi_get_state()` | — |
| Stato esponibile | backlight, volume/mute, DND, lock, TTS, notifiche (ring 8, solo thread LVGL), sysmon (temp/heap/uptime/cpu), Wi-Fi link, screenshot, camera JPEG | `screen_sleep_now/screen_wake` interni a `nv_ui.cpp:4040`, non pubblici |

**Conflitto numerazione:** il lavoro gamepad (`nv_pad`, BLE HOGP) ha prenotato **ABI v11**.
Le import di rete di questo piano sono quindi **ABI v12**, salvo accordo diverso.

---

## 1. Architettura in due livelli

```
                    ┌──────────────────────────────┐
  Home Assistant ◄──┤ nv_mqtt  (servizio firmware) │◄── Impostazioni > Casa / web companion
  (broker MQTT)     │  - HA discovery (device)     │
                    │  - comandi → OS              │
                    │  - bridge per le app (v12)   │
                    └──────────────┬───────────────┘
                                   │ coda PSRAM
  App WASM (store) ── nv.mqtt_* ───┘
                  ── nv.http_*  ──► nv_netreq (worker host, abortibile)
                  ── nv.ws_*    ──► esp_websocket_client
                  ── nv.ha_*    ──► proxy HA con token di sistema (l'app non vede il token)
```

Motivo: la domotica "utile" deve funzionare **sempre**, anche con un gioco aperto o schermo spento.
Le app WASM non possono girare in background (solo mode), quindi l'integrazione HA di base è
firmware; le app aggiungono dashboard e UI ricche solo mentre sono aperte.

---

## 2. Fase F0 — prerequisiti (piccoli, da fare prima)

1. **mbedTLS fuori dalla SRAM interna.** Provare `CONFIG_MBEDTLS_EXTERNAL_MEM_ALLOC=y`
   (oppure `MBEDTLS_CUSTOM_MEM_ALLOC` con soglia: blocchi ≥ 4 KB in PSRAM, piccoli interni)
   + `CONFIG_MBEDTLS_DYNAMIC_BUFFER=y`.
   Verifica HW: SRAM interna libera prima/dopo (`/api/heap`), Anima online, OTA, store, tempo
   handshake. Se regge, sblocca TLS persistenti (MQTT su 8883, HA via https, ws). Va in
   `sdkconfig.defaults` (§8). Senza questo: MQTT/WS **solo in chiaro su LAN** e massimo 1 sessione
   TLS alla volta tramite `nucleo_arb`.
2. **Evento `NV_EV_NET_STATE`** sul bus (up/down, Wi-Fi o Eth). Attenzione: `wifi_evt` ha stack
   2304 e il publish è sincrono → l'handler IDF fa solo `xTaskNotify`/flag verso un piccolo task
   che pubblica sul bus. Utile anche a nv_web (oggi aspetta solo il Wi-Fi, non l'Eth).
3. **API pubblica schermo:** `nv_ui_screen_sleep_async()`, `nv_ui_screen_wake_async()`,
   `nv_ui_screen_is_on()` in `nv_ui.h` (wrapper di `screen_sleep_now/screen_wake` via `lv_async_call`).
4. **`nv_sig`**: estrarre `manifest_verify` da `nv_ota.cpp` in un modulo puro
   (`nv_sig_verify(prefix, fields…, sig_b64)`), host-testato. Lo usano OTA e store.
5. **Budget socket:** `LWIP_MAX_SOCKETS` 20 → 24 (httpd 8, mDNS/keydeck/cast ~4, mqtt 1,
   app max 4, OTA/store/anima 2-3, margine). In `sdkconfig.defaults.esp32p4`.
6. Correggere `NUCLEO_SDK_ABI` e commento nell'header SDK.

---

## 3. Fase F1 — `nv_mqtt` + Home Assistant MQTT Discovery (firmware)

È la funzione più visibile: l'utente inserisce l'host del broker e il pannello compare in HA con
tutte le entità, senza YAML.

### 3.1 Componente
- `components/nv_mqtt` su `esp-mqtt` di IDF (`REQUIRES mqtt`), pattern keydeck:
  `nv_mqtt_init()` legge `mqtt_en`, si abbona a `NV_EV_SETTINGS_CHANGED` e `NV_EV_NET_STATE`,
  avvia/ferma il client. La riconnessione la fa esp-mqtt.
- Task esp-mqtt: stack 6 KB **interno** (tocca `nv_config` e ha vita non infinita → regola §2).
  Buffer RX/TX 2 KB; messaggi oltre 4 KB scartati.
- Chiavi NVS (≤15 caratteri): `mqtt_en`, `mqtt_host`, `mqtt_port`, `mqtt_user`, `mqtt_pass`,
  `mqtt_tls`, `mqtt_pfx` (default `homeassistant`), `mqtt_node` (default `nucleo_<mac6>`).
- Password: mai in `NV_LOG*` (§6: il log è servito da `/api/logs`), mai in `/api/mqtt` in risposta.
  Resta in NVS in chiaro come la password Wi-Fi (stesso livello di esposizione). Nel backup SD
  (`nv_backup`) **escluderla** o accettarlo esplicitamente.
- Broker auto: browse mDNS `_mqtt._tcp` e, se trovato un host HA (`_home-assistant._tcp`),
  proporre lo stesso IP porta 1883.

### 3.2 Topic
- Stato/comandi: `nucleo/<node>/<entità>/state|set`, disponibilità `nucleo/<node>/status`
  (LWT `offline`, retained; `online` alla connessione).
- Discovery: **device discovery** (una sola config retained)
  `homeassistant/device/<node>/config` con `dev`, `o` (origine NucleoOS + versione) e `cmps`.
  Richiede HA ≥ 2024.11; fallback per-entità (`homeassistant/<comp>/<node>/<obj>/config`) dietro
  un flag, se serve compatibilità vecchia.
- Abbonarsi a `homeassistant/status`: su `online` ripubblicare discovery + stati.

### 3.3 Entità (v1)
| Entità HA | Tipo | Lato OS |
|---|---|---|
| Schermo | `light` (on/off + brightness 0-100) | on/off = sleep/wake (F0.3), brightness = `nv_hal_backlight_set` + `nv_config "brightness"` |
| Volume | `number` 0-100 | `nv_audio_set_volume` + persist |
| Muto | `switch` | `nv_audio_set_mute` |
| Non disturbare | `switch` | config `qs_dnd` |
| Blocco | `switch` (solo ON remoto; lo sblocco resta a schermo) | `nv_ui_lock` |
| Notifica | `notify` | `nv_notify_post` via `lv_async_call` |
| Parla | `notify` (seconda) o `text` | `nv_tts_say` |
| App aperta | `select` (opzioni = app installate) + `sensor` app corrente | `nv_ui_open_app_id_async`, `nv_ui_current_app_id` |
| Home | `button` | `nv_ui_go_home_async` |
| Riavvia | `button` (categoria config) | riavvio |
| Aggiornamento | `update` (installed/latest da manifest OTA) | `nv_ota` — installazione da HA solo se l'utente lo abilita |
| Temperatura chip, RSSI, uptime, RAM interna/PSRAM libera, IP | `sensor` diagnostici | `nv_sysmon_perf/mem`, `nv_wifi_get_link` |
| Tocco recente | `binary_sensor` (presenza) — tocco negli ultimi N s | timestamp ultimo touch in nv_hal |
| Istantanea | `camera` (immagine su topic) — **v2**, a richiesta | `nv_camera_save_jpeg` / `nv_hal_screenshot` |

Pubblicazione stato: evento `NV_EV_SETTINGS_CHANGED` sulle chiavi rilevanti → bit "dirty"
(il callback è sincrono sul task di chi scrive: niente I/O lì) → il task mqtt pubblica.
Sensori diagnostici ogni 60 s.

### 3.4 Sicurezza comandi
- Superficie comandi fissa e piccola: niente fs, niente install, niente `/api/ui/*` via MQTT.
- Parser payload in un modulo puro (`nv_mqtt_cmd.c`) + test e fuzzer in `tests/host` (§6).
- Rate limit su notify/TTS (es. 1/s, burst 5); testi troncati a 96/200 caratteri.
- Nuove route web: `GET/POST /api/mqtt` (config, stato connessione) — autenticate di default;
  alzare `max_uri_handlers` (oggi 72, ~62 usati).

### 3.5 UI
- Nuova categoria Impostazioni **"Casa"** (`kCats[]` + `cat_home()` in `settings_app.cpp`):
  toggle, host/porta, utente/password, stato ("Connesso a 192.168.x.x · 23 entità"),
  pulsante "Ripubblica in HA". Stringhe `NV_STR_*` IT/EN.
- Pagina equivalente nella PWA (`/sdcard/web`): digitare host e password da PC è più comodo.

Rilascio: OTA 1.1.12x. Verifica: HA reale + `mosquitto_sub -v -t 'nucleo/#'`.

---

## 4. Fase F2 — store pronto per terzi (integrità + permessi)

Da fare **prima** di accettare app non nostre.

### 4.1 Integrità
- `store.json` porta per ogni app `files: [{path, size, sha256}]`.
- Firma catalogo: `store.sig` = ECDSA P-256 su `"nucleoos-store-v1\n<sha256(store.json)>\n"`,
  stessa chiave OTA (privata solo sul PC di rilascio) o, meglio, **seconda chiave dedicata** allo
  store (una compromissione dello store non deve permettere firmware).
- `install_package`: sha256 in streaming mentre scrive su SD; mismatch → cancella la cartella
  temporanea e rifiuta. Scrittura in `/sdcard/apps/.tmp-<id>` + rename finale (oggi il manifest
  è scritto per ultimo ma i file vecchi vengono sovrascritti sul posto).
- Firma generata da `tools/dist.py store` (già scrive sha256 a `:256`).
- Sideload dev (`push_app.ps1`, `/api/fs/write`) resta possibile: app marcate "sviluppatore"
  nella UI.

### 4.2 Permessi con consenso
- Nuovi permessi nel manifest: `net` (resta, HTTP/UDP verso Internet), `lan` (indirizzi privati),
  `ws`, `mqtt`, `ha`, `camera`, `mic`. Più `hosts: ["api.open-meteo.com", …]`: se presente,
  `http_*`/`ws_*` rifiutano altri host (controllo sull'host risolto dall'URL, non sulla stringa).
- Scheda store: elenco permessi in chiaro **prima** di Installa; aggiornamento che aggiunge
  permessi → richiede nuovo consenso.
- Concessioni in `/sdcard/apps/<id>/.grants` (aggiungere a lista nera di `nv_save`, `nv_wasm.cpp:877`)
  oppure in NVS per app; `RunReq.perms = manifest ∩ grants`.
- Impostazioni > Sicurezza > **Permessi app**: lista app, revoca per permesso.
- Applicare davvero `NV_WPERM_FS` (BACKLOG 108).

### 4.3 Pubblicazione da terzi
- Modello proposto: repo pubblico `nucleoos-apps` con cartella per app (sorgente + manifest +
  GUIDE.md + licenza); PR → CI compila con l'SDK (build riproducibile, niente binari forniti
  dall'autore) → noi firmiamo e pubblichiamo. Licenza dichiarata obbligatoria nel manifest.
- SDK + documentazione ABI pubblici su Pages (oggi `docs/WASM_APPS.md` è interno).

---

## 5. Fase F3 — ABI v12: rete per le app

Principi: niente chiamate che bloccano il guest per secondi (l'abort oggi non le interrompe);
tutto a **handle** con polling non bloccante, lavoro fatto da un worker host; tutte le risorse
registrate per-run e chiuse in `nv_wasm_exec_collect` (aggiungere un piccolo registro cleanup
invece di un'altra chiusura a mano).

### 5.1 HTTP completo
```c
int  nv_http_req(const char *spec_json, const void *body, int body_len); // → handle ≥0 | <0 err
int  nv_http_state(int h);            // 0 in corso, 1 fatto, <0 errore; status HTTP in nv_http_status
int  nv_http_status(int h);
int  nv_http_read(int h, void *buf, int cap);  // copia progressiva dalla risposta bufferizzata
void nv_http_close(int h);
```
- `spec_json`: `{"method":"POST","url":"…","headers":{"Content-Type":"application/json"},"timeout":8000}`
  parsato da modulo puro con limiti (≤ 16 header, ≤ 2 KB), fuzzato.
- Worker `nv_netreq` (1 task, stack interno 6 KB — TLS; coda 4): max **2 handle per app**,
  risposta in PSRAM fino a `min(256 KB, ram_budget/4)`.
- Abort: `esp_http_client` ha timeout; al collect si marca l'handle "orfano" e il worker lo libera
  alla fine. L'app esce subito.
- `http_get` resta (compatibilità) ma implementato sopra lo stesso worker.

### 5.2 WebSocket client
- `espressif/esp_websocket_client` pinnato `==` (regola build: componenti pinnati).
- `nv_ws_open(url$, headers_json$) → h`, `nv_ws_send(h, *~, text)`, `nv_ws_recv(h, *~) → n|0`,
  `nv_ws_state(h)`, `nv_ws_close(h)`. Max 1 per app, coda RX in PSRAM (ring 64 KB, messaggio max
  = coda; frammenti riassemblati dal client).

### 5.3 MQTT per le app (tramite la connessione di sistema)
- `nv_mqtt_sub(filter$)` (max 8), `nv_mqtt_pub(topic$, *~, retain)`, `nv_mqtt_recv(topic_buf*~, payload*~) → n`.
- Nessun socket in più: passa da `nv_mqtt`. Vietato pubblicare su `homeassistant/#`,
  `nucleo/<node>/#`, `$SYS/#`. Iscrizioni rimosse al collect.

### 5.4 Proxy Home Assistant (`ha`)
- Config sistema (Impostazioni > Casa): URL HA + **token long-lived**, salvati una volta.
- `nv_ha_call(method$, path$, body*~, resp*~)` handle-based come http, `path` solo sotto `/api/`;
  `nv_ha_ws_open()` apre il WebSocket HA con auth già fatta dall'host.
- L'app **non vede mai il token**: un'app dello store può fare una dashboard HA senza chiedere
  segreti all'utente. È il motivo per preferire il proxy a "chiedi il token nell'app".
- Avviso: `get_states` su installazioni grandi supera il MB → la dashboard usa
  `subscribe_entities` filtrato o `/api/states/<id>`.

### 5.5 Non in v12 (volutamente)
TCP grezzo (superficie grande, pochi casi d'uso che HTTP/WS/MQTT non coprano), server in ascolto
per app, background. Rivalutare dopo.

SDK: `nucleo_sdk.h` + wrapper comodi (`nv_http_post_json`, JSON minimale già in `cjson` port).
Test: app `nettest` estesa a tutte le import; fuzz sui parser spec/topic.

---

## 6. Fase F4 — app per lo store

| App | Base | Note |
|---|---|---|
| **Casa** (dashboard HA) | `ha` proxy + ws | stanze/aree, luci (dimmer), clima, cover, scene, sensori; layout a tessere 1024×600. App di punta |
| **Dispositivi locali** | `http_*` + `lan` | Shelly (Gen2 RPC), Tasmota (`/cm?cmnd=`), ESPHome (web server REST), WLED (`/json`). Scansione mDNS lato host? → serve import `mdns_browse` (piccola, da valutare in v12) |
| **Telecamere** | `http_*` | snapshot JPEG periodici (Frigate `/api/<cam>/latest.jpg`, HA `camera_proxy`); verificare che `gfx_image` decodifichi JPEG da memoria con JPEG HW; MJPEG continuo = v2 |
| **Radio web** | host | lo stream audio va fatto dal player nativo (MP3/AAC ci sono) → `/api/media/play` con URL http + import `open_url` nel player. Più firmware che WASM |
| **Emulatore Game Boy** | Peanut-GB (MIT) | gfx+audio+pad già esistenti; ROM homebrew libere di esempio |
| **NES** | agnes (MIT) | idem |
| **Calendario** | `http_*` | ICS da URL (Google/Nextcloud "indirizzo segreto"), vista giorno/settimana |
| **Utilità** | — | calcolatrice, sveglia, TOTP, convertitore, note: da fare comunque, "OS completo" |

---

## 7. Fase F5 — da prodotto

- **Widget dichiarativi sulla home** (nessun WASM in background): il manifest dichiara
  `{"widget": {"type":"entity","source":"ha|mqtt","id":"sensor.soggiorno_temp"}}`, reso nativo da
  LVGL e aggiornato da `nv_mqtt`/proxy HA.
- **Modalità pannello a muro**: salvaschermo orologio/meteo, luminosità notturna programmata,
  risveglio da tocco o MQTT (`light` on), avvio fisso di un'app.
- Notifiche: ring da 8 → 32 + persistenza su SD; azioni sulle notifiche (es. "Apri telecamera").
- Setup iniziale guidato (lingua → Wi-Fi → Casa/MQTT → fatto).
- Compatibilità openHASP (pagine JSON via MQTT) solo se c'è domanda: protocollo reimplementato,
  non codice GPL.

---

## 8. Ordine, stime, verifica

| Fase | Contenuto | Stima | Rilascio |
|---|---|---|---|
| F0 | TLS PSRAM (test), NV_EV_NET_STATE, API schermo, nv_sig, socket, SDK fix | 1-2 sessioni | OTA |
| F1 | nv_mqtt + discovery + Impostazioni Casa + pagina web | 2-3 sessioni | OTA, verifica con HA reale |
| F2 | store firmato + sha256 + consenso permessi | 2 sessioni | OTA + store |
| F3 | ABI v12 http/ws/mqtt/ha + SDK + fuzz | 3 sessioni | OTA |
| F4 | Casa, Dispositivi, Telecamere, emulatori… | 1 sessione per app | store |
| F5 | widget, kiosk, notifiche, setup | a seguire | OTA |

F1 non dipende da F2/F3: si può rilasciare subito dopo F0 e dà già il valore principale.

## 9. Rischi

- **SRAM interna**: principale. Ogni client (mqtt task 6 KB, netreq 6 KB, ws task ~4 KB,
  TLS ~40 KB se non in PSRAM) va misurato su HW con Wi-Fi+LVGL+web attivi. Soglia minima libera
  da fissare (es. 100 KB) e controllata da `tools/hil/smoke.py`.
- Contesa con altre sessioni: ABI v11 (gamepad), `nv_web.cpp` modificato ora da un'altra sessione.
- Comandi remoti = superficie d'attacco LAN: broker con utente/password obbligatori consigliati;
  in UI avviso se il broker è anonimo.
- HA cambia schema discovery raramente ma succede: tenere il generatore in un modulo puro testato.

## 10. Decisioni aperte (per Nicola)

1. Chiave firma store separata da quella OTA? (consigliato sì)
2. Password MQTT/token HA nel backup SD: escluderli? (consigliato sì)
3. Modello di pubblicazione terzi: solo sorgenti compilati dalla nostra CI (consigliato) o binari firmati dall'autore?
4. Installazione OTA comandabile da HA (`update.install`): abilitata di default o no? (consigliato no)
