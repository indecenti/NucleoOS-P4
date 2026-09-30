# Creare un gioco per NucleoOS (Vertice)

Guida pratica, ricavata dallo sviluppo di **Vertice GP** (kart) e **Vertice Bass** (pesca arcade).
Il motore è descritto in [VERTICE.md](VERTICE.md), l'ABI delle app in [WASM_APPS.md](WASM_APPS.md).
Qui c'è il *come*: struttura, asset, test, pubblicazione, e gli errori già fatti.

## 1. Anatomia di un gioco

```
apps/<id>/
  manifest.json     id, versione, abi, requires, canvas, permessi, descrizioni it/en
  main.c            stati del gioco, input, HUD, schermate
  *.c / *.h         il resto (mondo, IA, costruttori di mesh…): tutti i .c vengono compilati
  icon.z            icona (vedi le altre app)
  img/*.565         immagini e texture (RGB565, header uint16 w,h little-endian)
  snd/*.wav         suoni, 48 kHz MONO 16 bit
  models/*.vxm      modelli (tools/vertice/obj2vxm.py)
```

Manifest tipico:

```json
{ "id": "bass", "name": "Vertice Bass", "version": "1.6.0", "entry": "run", "abi": 9,
  "requires": { "vertice": "1.2" },
  "ram_budget": 262144, "stack_kb": 32, "timeout_ms": 120000,
  "permissions": ["gfx", "log"], "canvas_w": 512, "canvas_h": 300, "canvas_scale": "fit",
  "system_gestures": false, "category": "games",
  "descriptions": { "it": "…", "en": "…" } }
```

- `requires.vertice` = la versione del motore di cui hai bisogno (`VX_VERSION` in
  `components/vertice/include/vertice.h`). Se usi una funzione nuova del motore, alza la versione
  nel motore **e** nel manifest: sul firmware vecchio l'app viene rifiutata con un messaggio chiaro
  invece di non partire.
- `system_gestures: false`: il gioco ha i suoi pulsanti per uscire; le gesture di bordo dell'OS
  sono spente finché il gioco è a schermo (firmware ≥ 1.1.129).
- Canvas 512×300 scalato a 1024×600: metà dei pixel da riempire, stessa resa.

## 2. Il loop

```c
NV_EXPORT("run") void run(void) {
    build_world();                          // tutto costruito UNA volta
    while (nv_gfx_present()) {
        read_input();                       // touch + pad in una struct
        update(dt);                         // macchina a stati
        if (!full_screen_painting) vx_render();
        draw_2d();                          // HUD, pannelli, pulsanti sopra il 3D
    }
}
```

- **Macchina a stati** (`ST_TITLE`, `ST_SELECT`, `ST_AIM`, …): ogni stato ha un blocco di update e uno
  di disegno. `go(state, now)` registra l'ora d'ingresso: le animazioni sono funzioni di
  `now - s_state_ms`, mai contatori.
- Se una schermata copre tutto con un dipinto, **non chiamare `vx_render()`**: frame gratis.
- `dt` limitato a 50 ms: un frame lento non deve teletrasportare nulla.

## 3. Input: touch, tastiera, joypad

- Touch: zone fisse (croce a sinistra, A/B a destra) lette con `nv_touch_count/at` (multitouch),
  più il "tap" al rilascio per i pulsanti dei menu.
- `nv_gfx_pad()` unisce tastiera USB e joypad in una maschera SNES. Se `NV_PAD_KEYBOARD` o
  `NV_PAD_GAMEPAD` è acceso: **nascondi tutti i controlli touch** e disegna una striscia di
  suggerimenti coi tasti veri (tastiera: SPAZIO=A, Z=B, P=START, ESC=SELECT). Ogni azione deve
  avere un tasto: menu compresi (A conferma, B indietro, START pausa).
- Pensa ai comandi come al gesto reale: nella pesca GIÙ = canna indietro (strappo, ferrata), B = mollare
  la lenza. I giocatori lo capiscono senza leggere.

## 4. Il mondo 3D (Vertice)

- Costruisci le mesh con un builder (vedi `apps/bass/mesh.c`): vertici + triangoli orientati +
  materiale per triangolo, **una mesh per materiale/zona**, cloni per le copie.
- Texture sulle scatole: `mb_box_uv(..., tile)` mappa le unità del mondo sui texel, così una texture
  ha la stessa scala su oggetti di qualunque dimensione.
- **Limite UV**: un triangolo con uno span UV > 8192 cade nel percorso lento (~2000 cicli/pixel).
  Tieni `lato / tile * 1024` sotto 8192 (il motore già sposta gli UV per ripetizioni intere).
- Sfondo senza triangoli: `vx_floor` (Mode-7, l'acqua o il prato), `vx_panorama` (360° attorno
  all'orizzonte, texel magenta = cielo a gradiente), `vx_water(forza, onda)` (Vertice 1.2: il
  pavimento riflette panorama e cielo con Fresnel e increspature; 0 sott'acqua).
- Alberi, canne, cespugli lontani: `VX_BILLBOARD` con texture keyed (magenta trasparente).
- Particelle: `vx_emitter` una volta, `vx_emit` quando serve (spruzzi, bolle, scintille additive).
- Per mostrare/nascondere gruppi (sopra/sotto l'acqua) tieni gli id in liste e usa `vx_obj_show`.
- Grandezza per oggetto: `vx_obj_scale(id, percento)` (es. pesci per peso).

## 5. Asset con i modelli locali

Tutto gira sul PC, niente servizi esterni.

### Immagini — Qwen-Image 2.1 in ComfyUI (`tools/qwen_assets.py`)

- **Griglie 2×2**: un'immagine 1024×1024 = quattro asset coerenti fra loro (`split_grid`).
  Prompt: "A 2x2 grid of four separate, equally sized square panels, separated by thin white
  borders…", poi un pannello per posizione (Top left…, Bottom right…).
- **Oggetti ritagliati** (pesci, esche, alberi, icone): chiedi "on a plain flat deep navy blue
  background", poi flood-fill dai bordi verso il magenta (`0xF81F`) = trasparente. Buchi interni
  (es. uno pneumatico) vanno riempiti a mano.
- **Texture ripetibili**: "seamless tileable texture, flat orthographic view, even lighting";
  poi `tileable()` (dissolvenza con la copia spostata di mezzo tile) e 128×128.
- **Panorami**: 2048×512, "the shoreline runs along the very bottom edge"; si avvolge in orizzontale
  (dissolvenza delle estremità), si mette il cielo in magenta con flood-fill dall'alto, poi si
  ritaglia la fascia 1024×128 con la riva in basso (riga d'orizzonte 124).
- **Dipinti a schermo intero** (titolo, laghi, vittoria, intro): 512×300 con `fit(..., "cover")`.
- Errore intermittente `HostBuffer.read_file_slice`: `generate()` ritenta da solo.
- Strumento pronto: `tools/game_assets.py` (`grid` / `pano` / `art` / `music`), istruzioni complete
  nella skill `.claude/skills/game-assets/SKILL.md`. Ricette esatte di Bass (prompt + seed) in
  `apps/bass/art/`: rigenerare con lo stesso seed dà lo stesso asset.
- La GPU è condivisa: se ComfyUI ha poca VRAM libera (altri processi dell'utente) la generazione si
  ferma; controlla `system_stats` prima di lanciare un lotto.

### Musica — ACE-Step 1.5 turbo via ComfyUI (`tools/ace_music.py`)

- `generate(tags, secondi, bpm, tonalità, seed)`; tag strumentali ("instrumental, 1990s arcade video
  game music, …"), **sempre bpm e tonalità**. Poi `to_device_wav` → 48 kHz mono, picco 0.35.
- Brani corti (7–30 s): intro, menu (a ciclo), un tema per livello, fanfare.
- Non far girare ComfyUI e il server ACE-Step standalone insieme (stessa VRAM).

### Effetti — sintesi (`tools/gen_bass_sfx.py`)

- numpy: rumore filtrato (acqua), click risonanti (mulinello), campane inarmoniche, ottoni,
  motore a impulsi. `python tools/gen_bass_sfx.py motor fish_on` rigenera solo quelli.
- Picco basso (-10 dBFS): suoni forti via l'amplificatore della scheda possono causare cali di
  tensione. `nv_sound` ne suona **uno alla volta**: il nuovo interrompe il vecchio.

## 6. Provare sul PC (simulatore)

```bash
bash tools/vertice/sim/run.sh apps/bass 600
```

- Compila il motore vero + l'app in C nativo, salva i frame in PNG (1024×600).
- `VX_PAD="10-11:16;90-150:1"` = bit del pad per intervalli di frame (UP=1 DOWN=2 LEFT=4 RIGHT=8
  A=16 B=32 START=1024; `1<<30` simula una tastiera collegata). `VX_DUMP="50,120"` = frame da salvare.
- `APP_CFLAGS="-DBASS_TEST_CATCH=2 ..."`: build di prova che apre direttamente una schermata
  (vedi gli `#ifdef BASS_TEST_*` in `apps/bass/main.c`), così si controllano schermate rare.
- Da Windows lancia gli script con `wsl bash /mnt/c/...` da PowerShell (Git Bash storpia `/mnt/c`).
- Guarda sempre i frame (fogli di contatto con PIL): è così che si trovano sovrapposizioni,
  artefatti, oggetti fuori scala.

## 7. Sulla scheda

```powershell
.\sdk\push_app.ps1 -AppDir apps\bass -Aot -NoRun          # wasm + AOT + manifest
```

- Gli asset nuovi (img/snd) si caricano a parte con `curl --data-binary` su
  `/api/fs/write?path=/apps/<id>/img/<nome>` (header `Authorization: Bearer <token>`).
- Chi ha il gioco aperto deve **chiuderlo e riaprirlo**.
- Se il gioco usa una funzione nuova del motore serve il firmware nuovo (release OTA) prima.

## 8. Pubblicare

1. `git fetch` + `git rebase origin/main` (altre sessioni lavorano in parallelo), commit, push.
2. Firmware se serve (release firmata; vedi le note OTA).
3. Store: `python tools/dist.py store` **dopo** il rebase (esporta il catalogo corrente).
4. Controllo privacy prima di ogni push: niente token, IP di casa, nomi rete, percorsi personali, e
   screenshot ripuliti.

## 9. Prestazioni: regole misurate

- Il collo è la banda PSRAM: il renderer a tile in SRAM (due core + DMA) lo aggira; tu tieni
  poche mesh grandi invece di tante piccole.
- RV32 non ha conversione int64→float: niente conti 64 bit per pixel o per riga.
- Niente `float` nei loop per pixel del 2D; il 2D costa per chiamata (`nv_gfx_*` attraversa il
  confine WASM): meno chiamate, dipinti interi invece di mille rettangoli.
- `NV_RGB` è una funzione: per le costanti usa una macro `C565(r,g,b)`.
- Memoria lineare WASM 64 KB per lo stack/dati dell'app: buffer grandi statici con giudizio.
- Il font 5×7 ha un set limitato di caratteri (vedi WASM_APPS.md): niente `^`, accenti come `'`.

## 10. Gameplay "arcade": cosa ha funzionato

- Ogni successo ha un **momento**: banner che sbatte ("FISH ON!"), lampo, piccolo fermo immagine
  (hit-stop 120–150 ms), scossa della camera, suono dedicato.
- Numeri che salgono (peso, punti) invece di comparire; barre che si riempiono.
- Record: schermata speciale per il podio (raggiera, medaglie che cadono, lettere che saltellano).
- Rarità vere: la maggior parte piccoli, i giganti rari (distribuzione `r^5`, 2% mostri).
- Regole leggibili a schermo nei primi secondi e suggerimenti contestuali ("ROSSO! SMETTI DI
  RECUPERARE").
- Copia le **meccaniche** dei classici (qui Fisherman's Bait: vivaio dei 3 più pesanti, tempo
  bonus per peso, spazzatura "CLEAN UP", sonar) — mai nomi, marchi o grafica altrui.
