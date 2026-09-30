# Sviluppare app WASM per NucleoOS (ESP32-P4)

Guida condensata — la fonte di verità per gli import è `sdk/include/nucleo_sdk.h`; app di
riferimento `apps/abc123`. Le app girano nell'interprete WAMR con canvas full-screen 1024×600
(ABI v2 "gfx"). Si compilano **sul PC** (clang wasm32) e si caricano via Wi-Fi: il device esegue
solo il `.wasm`.

## Struttura

```
apps/<id>/manifest.json   { "id":"myapp", "name":"My App", "version":"1.0", "entry":"run",
                            "abi":2, "ram_budget":65536, "stack_kb":16, "timeout_ms":120000,
                            "permissions":["gfx"], "canvas_w":1024, "canvas_h":600 }
apps/<id>/main.c          loop immediate-mode (forma OBBLIGATORIA, vedi sotto)
apps/<id>/img/*.565       asset RGB565 opzionali (tools/build_abc_assets.py)
apps/<id>/snd/*.wav       SFX 48 kHz mono 16-bit opzionali
apps/<id>/icon.argb       icona launcher 80×80 ARGB8888 opzionale (tools/make_app_icon.py)
```

## Skeleton main.c

```c
#include "nucleo_sdk.h"
NV_EXPORT("run")
void run(void) {
    int W = nv_gfx_width(), H = nv_gfx_height();
    int redraw = 2, prev_down = 0;
    while (nv_gfx_present()) {                 // pacing frame; 0 = l'OS chiude l'app
        int x, y, down = nv_touch(&x, &y);
        int tap = (!down && prev_down); prev_down = down;
        if (nv_gfx_back()) break;              // gesture back dell'OS alla tua radice
        if (tap) { /* gestisci il tap */ redraw = 2; }
        if (redraw > 0) { /* ridisegna TUTTO */ redraw--; }  // 2 frame: double buffer
    }
}
```

## ABI (modulo "nv", permesso "gfx")

- Frame: `nv_gfx_present` `nv_gfx_width/height` `nv_gfx_clear(col)` `nv_gfx_back`
- Draw: `nv_gfx_rect` `nv_gfx_circle` `nv_gfx_line` `nv_gfx_tri` `nv_gfx_text(x,y,s,col,scale)` `nv_gfx_image(name,x,y,w,h)` `nv_gfx_blit`
- Input: `nv_touch(&x,&y)` → premuto 1/0 (il TAP è il fronte di rilascio); multi-touch ABI v3 `nv_gfx_touch_count/_point`
- Audio: `nv_gfx_tone(hz,ms)` `nv_sound(name)` (WAV in snd/) `nv_speak(text,lang)` (voce offline)
- ABI v4 (`"abi":4`): `nv_gfx_text_width(s,scale)` + helper `nv_gfx_text_center(y,s,col,scale)`; `nv_backlight(0..100)` (torcia; l'OS ripristina la luminosità utente all'uscita)
- ABI v5 (`"abi":5`, permesso `net`): UDP LAN — `nv_net_open/close/send/bcast/recv/from_ip/from_port/ip` (IP = token opaqui, li rigiri; un socket per app, chiuso all'uscita). Fondamenta multiplayer.
- **ABI v6 (`"abi":6`) — motore dirty-rect** (chiave per FPS alti su P4, banda PSRAM è il collo): `nv_gfx_persist(1)` una volta → l'OS tiene UN buffer persistente (no swap/clear) e **riblitta solo i pixel che disegni** (auto-tracked, flush PPA). Pattern: disegna la scena statica una volta → `nv_gfx_bg_save()`; ogni frame **cancella** gli oggetti mobili con `nv_gfx_bg_restore(x,y,w,h)` (ricopia lo sfondo salvato) e ridisegnali. Un repaint full-screen (banda-bound, ~2 fps) diventa pochi blit piccoli. Riferimento: `apps/tanks` (scena statica + overlay barrel/traiettoria/proiettile). Perché serve: framebuffer 1024×600 in PSRAM → ridisegnare tutto ogni frame non regge; i giochi seri fanno dirty-rect/sprite + 2D hardware (PPA).
- ABI v7 (`"abi":7`, nessun permesso): aprire file — `nv_open_path/size/read`, vedi sotto.
- **ABI v9 (`"abi":9`) — motore 3D Vertice** (`nv.vx_*`): scena 3D renderizzata sui due core nel canvas, poi 2D sopra. Manifest `"requires": {"vertice": "1.0"}`, `canvas_scale` `fit`/`stretch`/`zoom`. Input `nv_gfx_pad()` (tastiera + gamepad, maschera SNES). Dipendenze tra pacchetti (`requires`, `"kind": "library"`, asset `"<id>:<nome>"`). Tutto in [VERTICE.md](VERTICE.md); esempio `apps/vxgp`.
- **ABI v11 (`"abi":11`) — controller** (`nv.pad_*`, permesso `gfx`): ogni controller è un giocatore separato, già nel layout standard Xbox (A B X Y, LB RB, LT RT analogici, due stick, Back/Start/Guide) qualunque sia il modello. USB HID generico mappato con SDL_GameControllerDB (~1000 modelli + euristica + righe utente in `/sdcard/data/pads.txt`, stesso formato di `gamecontrollerdb.txt`), USB XInput (Xbox 360/One/Series e cloni), Switch Pro e NSO via USB, DualShock 4 / DualSense via USB (vibrazione + light bar), Bluetooth LE HID (Xbox Series/One S, 8BitDo, Stadia...). `nv_pad_count()`, `nv_pad_state(i, &st, sizeof st)`, `nv_pad_name(i, buf, len)`, `nv_pad_rumble(i, low, high, ms)`; struttura `nv_pad_state_t` in `nucleo_sdk.h`. `nv_gfx_pad()` continua a funzionare e fonde tutti i controller. I pad Bluetooth Classic (DualShock 4, DualSense, Switch Pro, Joy-Con) vanno solo via cavo: il C6 è solo BLE.
- Stato: `nv_save/nv_load(name,buf,len)` ≤8 KB; `nv_millis` `nv_rand` `nv_lang`
- `NV_RGB(r,g,b)` → RGB565. Font 5×7: ` 0-9 A-Z - . : % / < > ! + x ' , = ? ( ) # *`, advance 6*scale.

## Opening files (ABI v7) — aprire file

Un'app può farsi aprire dai file: compare in **File → Apri con** e tra le **app predefinite**
(Impostazioni), accanto ai viewer di sistema. Due campi manifest opzionali:

- `"opens"`: i MIME che l'app apre — `type/subtype` o `type/*` (minuscolo, `[a-z0-9.+-]`, max 8;
  `*/*` vietato). I pattern malformati vengono scartati con un log, non troncati.
- `"file_types"`: fino a 4 estensioni nuove che l'app insegna all'OS — `ext` `[a-z0-9]{1,11}` senza
  punto, `mime` senza `*`, `kind` fra `text image audio video app archive other` (default `other`).
  Un'estensione già nota tiene il tipo di sistema.

```json
{ "id":"mdview", "name":"MD Viewer", "version":"1.0", "entry":"run", "abi":7,
  "ram_budget":131072, "permissions":["gfx","log"], "canvas_w":1024, "canvas_h":600,
  "opens":["text/plain", "text/markdown", "application/x-mdnote"],
  "file_types":[ { "ext":"mdn", "mime":"application/x-mdnote", "kind":"text" } ] }
```

**Modello a concessione.** Nessun permesso `fs`: la scelta dell'utente ("apri QUESTO file con QUESTA
app") è il permesso. L'app legge **solo quel file, in sola lettura** — nessun altro path è
raggiungibile. Aperta dalla Home, `nv_open_path` ritorna 0 e gli altri -1. La concessione finisce
quando la run termina (e decade se la SD viene rimontata).

- `nv_open_path(buf,len)` → path assoluto (troncato a `len`, sempre NUL); ritorna la lunghezza piena, 0 = nessun file
- `nv_open_size()` → byte; -1 se nessun file / illeggibile
- `nv_open_read(off,buf,len)` → byte letti (max 64 KB per chiamata), 0 a EOF, -1 errore. Si legge a
  pezzi: il file non deve stare nei 64 KB di memoria lineare.

```c
#include "nucleo_sdk.h"
static char chunk[4096];
NV_EXPORT("run")
void run(void) {
    char path[256];                                 // i path concessi sono < 256 byte
    if (nv_open_path(path, sizeof path) == 0) { /* aperta dalla Home: UI normale */ return; }
    int32_t size = nv_open_size(), off = 0, n;
    while ((n = nv_open_read(off, chunk, sizeof chunk)) > 0) {
        /* elabora chunk[0..n) */
        off += n;
    }
    if (n < 0) nv_print("errore di lettura");
    nv_printf("%s: %d/%d byte", path, off, size);
}
```

Le app gfx partono subito sul file; le app console mostrano la loro scheda e il file è concesso alla
pressione di Esegui. Le associazioni si registrano al boot insieme ai tile (un'app appena installata
dallo store compare in *Apri con* dopo il riavvio); disinstallarla la toglie subito. Ogni chiamata
riapre il file (sicuro con lo swap a caldo della SD): meglio pezzi da 4-64 KB che mille letture da
pochi byte.

## Regole d'oro (lag = numero di chiamate draw per frame)

1. **Frame-skip**: schermata ferma = ~0 chiamate host. Ridisegna 2 frame solo quando cambia qualcosa.
2. **Niente float nei path caldi** (l'interprete è lento sui float): fixed-point + LUT seno (abc123 `SINQ`).
3. **Poche primitive**: un'immagine `.565` = 1 chiamata; un gradiente = N rect = N chiamate. Meglio flat.
4. **Audio in sequenza**: voce e SFX non devono sovrapporsi — clock di timeline app-side
   (`g_speak_until`, `schedule_sfx`; vedi abc123). Numeri per nv_speak = PAROLE ("TRE" non "3").
5. **Buffer grossi dentro i 64 KB di memoria lineare**, non enormi array statici.

## Build + push (PC)

```powershell
.\.claude\skills\wasm-app\scripts\build_push.ps1 -AppDir apps\myapp          # build+push wasm+manifest
.\.claude\skills\wasm-app\scripts\build_push.ps1 -AppDir apps\myapp -Assets  # anche img/ e snd/
```
(clang `--target=wasm32 -mcpu=mvp -O2 -ffreestanding -nostdlib`, 64 KB linear memory, 8 KB stack;
upload `curl --data-binary` su `POST /api/fs/write?path=/sdcard/apps/<id>/...`.)
Un'istanza in esecuzione tiene il VECCHIO wasm: riapri l'app. Debug senza cavo: `GET /api/screen`,
`GET /api/logs`, `POST /api/app/run?id=<id>`.

## Distribuzione: store remoto (Wi-Fi)

Oltre al push dev, le app si installano da un **server remoto** senza cavo né reflash.

- **Server** (`server/appstore/`, solo stdlib Python): `python appstore_server.py` serve la cartella
  `apps/` del repo su `:8090`. Espone `GET /store.json?lang=&region=` (catalogo **categorizzato,
  multilingua, geolocalizzato**) e `GET /apps/<id>/{manifest.json,app.wasm,icon.argb}`. Metadati store
  curati in `server/appstore/catalog.json` (categoria, nomi/descrizioni per lingua, featured, rating,
  `regions[]`) uniti sopra i manifest. Vedi `server/appstore/README.md`.
- **Device**: Impostazioni → Aggiornamento → *App store* = `http://<PC>:8090` → SAVE, e scegli la
  *Regione*. Poi Apps → tab **Store**: chip categorie (Tutte/In evidenza/Giochi/Bambini…), card con
  rating + descrizione localizzata, INSTALL/UPDATE. La lingua della UI (`nv_lang`) e la regione vanno
  in querystring → lo store risponde localizzato e filtrato. Modulo in `/sdcard/apps/<id>/` (tile Home
  dopo reboot/scan). Firmware: `components/nv_appstore` + stringhe in `nv_i18n`.
- Il device valida magic wasm + cap 2 MB prima di scrivere; WAMR isola il guest e limita i permessi.
  Campi manifest opzionali per lo store: `author`, `description`.
- **Firma (firmware ≥ 1.1.132)**: ogni app dello store ha `apps/<id>/package.sig` (sha256 + size di
  ogni file, firmato con la chiave STORE — non quella OTA — in `%USERPROFILE%\.nucleo\store-signing-key.pem`).
  `export_static.py` e `appstore_server.py` lo generano da soli; `python tools/store_sign.py
  verify <dir>` lo controlla. Il device scarica tutto in `.tmp`, verifica, e solo alla fine rinomina
  (manifest per ultimo). App non firmate: rifiutate, salvo Impostazioni → Sicurezza → *Consenti app
  dello store non firmate (sviluppatore)*. Il push dev (`push_app.ps1`) non passa dallo store.
- **Permessi sensibili** (`net`, `lan`, `ws`, `mqtt`, `ha`, `fs`, `camera`, `mic`): il catalogo li
  porta in `perms`, la scheda dell'app li elenca e l'installazione chiede un secondo tocco
  ("Accetta e installa"); un aggiornamento che ne aggiunge li evidenzia. Revoca per app in
  Impostazioni → Sicurezza → *Permessi delle app* (`/sdcard/nucleos/perms.json`).

## Rete per le app (ABI v12)

`"abi": 12`. Niente blocca: ogni richiesta restituisce un handle, l'app lo interroga dal suo loop.
Permessi: `net` = Internet pubblico, `lan` = rete di casa (IP privati, `*.local`), `ws` = WebSocket,
`mqtt` = broker di sistema (Impostazioni → Casa), `ha` = Home Assistant con il token di sistema
(l'app non lo vede mai). L'host risolve il nome e classifica l'IP prima di connettersi, i redirect
non vengono seguiti. Dettagli e codici d'errore in `nucleo_sdk.h` (sezione ABI v12); esempio
completo `apps/net12` + `tools/net12_server.py`.

```c
char spec[160];
nv_snprintf(spec, sizeof spec, "{\"url\":\"http://%s/rpc/Switch.Set?id=0&on=true\"}", shelly_ip);
int h = nv_http_req(spec, 0, 0);                 // subito: handle
... nel loop: if (nv_http_state(h) == 1) { n = nv_http_read(h, buf, sizeof buf); nv_http_close(h); }
int ha = nv_ha_req("POST", "/api/services/light/toggle", "{\"entity_id\":\"light.cucina\"}", 29);
nv_mqtt_sub("zigbee2mqtt/+/state");  ...  n = nv_mqtt_recv(topic, sizeof topic, buf, sizeof buf);
```

## Programmi da terminale (WASI, ABI 8)

App WASI con `"console": true` nel manifest: le lancia il **Terminale** (`lua`, `js file.js`,
`sqlite3 note.db`: prima parola = id app, il resto = argv), la tile in Home apre un Terminale che le
esegue. stdin = righe digitate (bottone EOF = fine input, STOP = kill), stdout/stderr nello
scrollback (testo semplice, escape ANSI scartati). Niente timeout né tetto opcode. Permesso `home` =
`/sdcard/home` come `/`. Port pronti in `ports/` (Lua 5.4, QuickJS-ng, SQLite): `bash ports/build.sh`,
test su PC `bash ports/test.sh`; nello Store stanno nella categoria **Terminale**. Lua usa
`nv.try_call`/`nv.throw` al posto di setjmp/longjmp (`ports/common/nv_sjlj.h`).

## Checklist "finita"

1. Compila pulita `-Wall -Wextra`. 2. Idle ≈ 0 draw calls. 3. Animazioni con finestra temporale.
4. Audio sequenziato. 5. Push con curl e dimensioni verificate. 6. Riapri l'app sul device.
