# Vertice — il motore 3D di NucleoOS

Vertice è il motore 3D software di NucleoOS per ESP32-P4: scena, luci, nebbia, particelle, modelli,
pavimento Mode-7, panorama a 360° e picking, esposti alle app WASM con gli import `nv.vx_*` (ABI 9).
I giochi restano pacchetti WASM distribuibili dallo store; il motore sta nel firmware ed è un
**componente di sistema** che le app dichiarano come dipendenza.

- Codice: `components/vertice/` (`vertice.cpp` = motore, `core/` = rasterizzatore).
- API C: `components/vertice/include/vertice.h`. API WASM: sezione *Vertice* di `sdk/include/nucleo_sdk.h`.
- Gioco di riferimento: `apps/vxgp` (*Vertice GP*, kart 3D).
- Simulatore PC: `tools/vertice/sim/` (stesso motore, compilato per Linux/WSL).
- Licenza: il rasterizzatore in `core/` deriva da **Jet** di CubeCoders (MIT, commit c56dfc0,
  notice in `core/LICENSE-Jet`, vedi `THIRD_PARTY.md`); tutto il resto è NucleoOS.

## Perché così: il collo di bottiglia del P4

Misure sulla scheda, prima di scrivere una riga di ottimizzazione:

| Fatto misurato | Conseguenza |
|---|---|
| Scritture CPU su PSRAM cached ≈ **33 MB/s per core** (latenza, non banda) | Un canvas 512×300 (300 KB) riscritto 2-3 volte per frame costa decine di ms: **mai rasterizzare in PSRAM** |
| L1 D-cache 64 KB 2-way **condivisa** dai due core; L2 128 KB, linee 64 B | Dati caldi piccoli e contigui; i due core non devono pestarsi le stesse linee |
| SRAM interna libera in gioco ≈ 275 KB (blocco max ≈ 172 KB), serve a Wi-Fi e DMA | Si prende poca SRAM, solo per i dati più caldi, e mai sotto la riserva |
| DMA 2D (`nv_2d_copy`) scrive PSRAM senza passare dalla CPU | Il write-out del frame va in DMA, la CPU intanto rasterizza |
| Interprete WAMR: ogni chiamata host costa | L'app descrive la scena una volta; per frame muove solo oggetti e camera |

Primo profilo reale (1.1.118.1, rendering a bande in PSRAM): **107 ms/frame** — prep 18 ms,
clear 7-11 ms, raster 75-79 ms per banda. Da qui l'architettura a tile descritta sotto.

## Pipeline di un frame

```
app WASM (1 thread)           core 0                           core 1
  vx_obj_pos / vx_camera ...
  vx_render() ─────────────▶  particles_update, floor_relight
                              bg_frame_setup (orizzonte, panorama per colonna, cielo)
                              prepare: cull, transform, sort front-to-back
                              vxBin: coda → liste per tile (counting sort)
                              ┌── tile = atomic++ ──┐          ┌── tile = atomic++ ──┐
                              │ clear_rows (cielo,  │          │   (stesso lavoro,    │
                              │  panorama, Mode-7,  │          │    tile diversi)     │
                              │  z) in SRAM         │          │                      │
                              │ vxRasterTile        │          │                      │
                              │ particelle del tile │          │                      │
                              │ nv_2d_copy → canvas │          │                      │
                              └─────────────────────┘          └──────────────────────┘
                              ◀──────────── join ──────────────
  nv_gfx_present() → PPA scala il canvas sul pannello (canvas_scale fit/stretch/zoom)
```

- **Tile in SRAM**: tile da 8 righe (4 se la SRAM scarseggia); ogni core ha il suo buffer colore + z
  in un unico blocco SRAM contiguo (canvas 512 → 32 KB). Il rasterizzatore lavora con basi virtuali
  (`col - y0*w`), quindi il codice dei triangoli non sa di essere in un tile.
- **Work stealing**: un contatore atomico distribuisce i tile; nessuno split statico da bilanciare.
- **Write-out in DMA** (`nv_2d_copy`), con fallback `memcpy`.
- **Fallback a bande**: se all'`vx_open` la SRAM non basta (resterebbero < 96 KB liberi), il frame
  si divide in due bande renderizzate direttamente nel target, con taglio adattivo sul costo misurato.

## Trucchi (vecchi e nuovi)

| Trucco | Da dove viene | Cosa fa in Vertice |
|---|---|---|
| **Mode 7** | SNES (F-Zero, Mario Kart) | Il prato/terreno non ha triangoli: una divisione per riga, texture in prospettiva esatta, luce, nebbia, e fusione verso il colore medio in lontananza (niente aliasing/sfarfallio) |
| **Panorama a 360°** | skybox dei giochi 16-bit | Montagne/nuvole come striscia texture agganciata all'orizzonte, 1 lookup per colonna |
| **Impostori (billboard)** | Doom, Build engine, Mario Kart | Alberi, monete: 2 triangoli che guardano la camera invece di centinaia |
| **Painter di fondo** | tutti i giochi 2D | Strada, cordoli, linee, ombre: strati a terra senza depth test/write, disegnati per primi nell'ordine di creazione → mai z-fighting |
| **Texel invece di poligoni** | pixel art | La folla delle tribune è una texture sulle alzate dei gradini, non geometria |
| **Nebbia come fusione al cielo** | PS1/N64 | Il colore dei triangoli sfuma verso il colore del cielo *di quella riga*; il gradiente del cielo segue l'orizzonte, così nebbia e foschia combaciano con qualsiasi beccheggio |
| **Span veloci** (`VX_FAST_SPANS`) | rasterizzatori anni '90 | Profondità Q8, luce/UV Q16 a passo costante, nessuna divisione per pixel |
| **Prospettiva a campioni** (`vxPersp`) | Quake (ogni 16 px) | UV corretti in prospettiva ogni 16 pixel, affini in mezzo: niente strade "piegate" |
| **Ordinamento front-to-back** | z-buffer early-out | Gli opachi più vicini riempiono lo z prima: i pixel coperti dopo non si scrivono |
| **Reciproco al posto della divisione** | fixed-point classico | Nebbia con un reciproco Q16 per frame, non una divisione per pixel |
| **Streaming di texture** | caricamento a strisce | `vx_texture_new` + `vx_texture_write`: una texture 512×64 (64 KB) costruita a strisce da un'app con 64 KB di memoria |

## Memoria

- **PSRAM a budget** (`VX_MEM_BUDGET` 12 MB): ogni `new`/`malloc` del motore (core incluso) è
  ridiretto da `objcopy --redefine-syms` (`vertice_alloc.syms`) a un allocatore PSRAM contato;
  anche il `.bss` della libreria finisce in PSRAM (`linker.lf`). `vx_stat(VX_STAT_MEM)` lo riporta.
- **SRAM interna**: solo il blocco dei tile, preso all'apertura e solo se restano ≥ 96 KB liberi.
- **Tetti rigidi** (`vertice.h`): 256 oggetti, 96 materiali, 32 texture (lato 8..512, potenze di 2),
  24 000 triangoli e 32 000 vertici di scena, 8 emettitori × 512 particelle. Ogni chiamata valida
  gli argomenti (arrivano da codice WASM non fidato).
- Il motore si chiude da solo quando l'app termina (`vx_close` a fine `run_worker`).

## API per le app (riassunto)

Manifest:

```json
{ "id": "vxgp", "abi": 9, "requires": { "vertice": "1.0" },
  "canvas_w": 512, "canvas_h": 300, "canvas_scale": "fit", "permissions": ["gfx", "log"] }
```

- Scena: `vx_texture` / `vx_texture_load` (img/*.565) / `vx_texture_new` + `vx_texture_write`,
  `vx_material(colore, shading, alpha, tex, speculare)`, `vx_prim` (cubo, sfera, cilindro, capsula,
  piramide, piano, griglia, quad, billboard), `vx_mesh`, `vx_model` (models/*.vxm, da
  `tools/vertice/obj2vxm.py`), `vx_clone`.
- Oggetti: `vx_obj_pos/rot/show/free`, `vx_obj_depth(id, bias, VX_DEPTH_NOTEST|VX_DEPTH_NOWRITE)`.
- Camera e atmosfera: `vx_camera`, `vx_look_at`, `vx_lens`, `vx_sun`, `vx_ambient`, `vx_sky`,
  `vx_fog`, `vx_depth`, `vx_floor`, `vx_panorama`, `vx_water` (1.2: riflesso del panorama sul
  pavimento, Fresnel + increspature).
- Effetti: `vx_emitter` + `vx_emit` (polvere, fumo, scintille, coriandoli; additivi o alpha).
- Frame: `vx_render()` poi il 2D sopra (HUD con `nv_gfx_*`) e `nv_gfx_present()`.
- Input: `nv_gfx_pad()` — tastiera USB e gamepad in una maschera stile SNES (`NV_PAD_*`); i bit
  `NV_PAD_GAMEPAD` / `NV_PAD_KEYBOARD` dicono se c'è un dispositivo, per nascondere i comandi touch.
- Picking: `vx_pick_at(x,y)` → dopo il render `vx_picked()`.
- Profilo: `vx_stat(VX_STAT_US / PREP_US / TRIS / QUEUED / ...)`.

Guida passo passo per un gioco nuovo (asset con Qwen/ACE-Step, simulatore, store): [GAME_DEV.md](GAME_DEV.md).

Regole per 60 fps: costruisci tutto una volta (mesh unite per materiale, cloni per le copie),
per frame solo `vx_obj_pos/rot` + camera; decalcomanie a terra come strati painter; alberi e
oggetti lontani come billboard; pavimento con `vx_floor`, orizzonte con `vx_panorama`.

## Dipendenze tra pacchetti

Sistema generico, non solo per Vertice:

- `"requires": { "<id>": "<versione minima>" }` nel manifest (fino a 4). L'id può essere un
  **componente di sistema** (`vertice`, `wasi`, `wasm4`: versioni dal firmware) o un **pacchetto**
  installato in `/sdcard/apps/<id>/`.
- `"kind": "library"`: pacchetto senza `app.wasm` (texture, modelli, suoni condivisi); non compare
  nel launcher e non si può avviare.
- Asset di una libreria: `"<id>:<nome>"` (es. `"kartkit:pino"` → `/sdcard/apps/kartkit/img/pino.565`)
  in `vx_texture_load`, `vx_model`, `nv_gfx_image`, `nv_sound` — solo se `<id>` è tra i `requires`.
- All'avvio `nv_wasm` rifiuta le app con dipendenze mancanti o troppo vecchie con un messaggio
  localizzato ("Richiede …").

## Simulatore PC

```bash
bash tools/vertice/sim/run.sh apps/vxgp 520
```

```bash
VX_TOUCH='5-6:256,150;100-700:470,250' VX_DUMP='60,200,330' bash tools/vertice/sim/run.sh apps/vxgp 340
```

Compila il motore vero (simboli rinominati `vxe_*`) + l'app in C nativo, e salva i frame in PNG a
dimensione pannello (1024×600, scalati come la PPA). `VX_TOUCH` = tocchi per intervallo di frame,
`VX_PAD` = maschera pad, `VX_FPS` = clock simulato. `profile.sh` fa la stessa corsa sotto gprof.

## Stato e prossimi passi

- Fatto: tile SRAM dual-core + DMA, binning, span veloci, prospettiva a campioni, Mode-7,
  panorama, impostori, particelle per tile, texture streaming, dipendenze, pad/tastiera.
- Da misurare sulla scheda: il renderer a tile (il sim conferma output identico).
- Prossimi: prepare in parallelo sui due core, riempimenti con SIMD PIE, dati caldi (coda, vertici
  trasformati) in SRAM in "modalità gioco", risoluzione dinamica, layer `nv_disp` senza tearing.
