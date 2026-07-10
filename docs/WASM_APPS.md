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
- Stato: `nv_save/nv_load(name,buf,len)` ≤8 KB; `nv_millis` `nv_rand` `nv_lang`
- `NV_RGB(r,g,b)` → RGB565. Font 5×7: ` 0-9 A-Z - . : % / < > ! + x`, advance 6*scale.

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
  Punta lo store solo a un host di cui ti fidi. Campi manifest opzionali per lo store: `author`,
  `description`.

## Checklist "finita"

1. Compila pulita `-Wall -Wextra`. 2. Idle ≈ 0 draw calls. 3. Animazioni con finestra temporale.
4. Audio sequenziato. 5. Push con curl e dimensioni verificate. 6. Riapri l'app sul device.
