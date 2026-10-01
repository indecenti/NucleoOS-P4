# Piano: interfaccia "Classica 95", mouse e tastiera

Stato: v3 (2026-10-01). Scritto e compilato, NON committato, NON provato su HW.
- Fasi 1-2 (input, navigazione da tastiera): fatte, audit app risolto.
- Fasi 3-6 (shell Classica) prima versione: `nv_ui_classic.cpp` (desktop, taskbar, Start, menu
  contestuali, title bar), integrazione in `nv_ui.cpp` (geometria per shell, `shell_apply`,
  `shell_tick` con isteresi), Impostazioni > Schermo: "Desktop classico" + "Automatico con mouse e
  tastiera". Logo NucleoOS (cristallo, dalla Cardputer edition, web/shell/icon.png) via
  `tools/gen_logo.py`. Finestre: sempre massimizzate, riducibili a icona (app viva, nascosta) e
  chiudibili.
- Sfondi desktop generati in locale (`tools/gen_desk_wallpaper.py`, Qwen-Image 2.1).

## 1. Obiettivo e decisioni

- **Impostazioni > Schermo > Interfaccia**: `Tablet` / `Classica` / `Automatica`
  (chiave `nv_config` `ui_shell` = 0/1/2, default 0).
- **Automatica**: mouse **e** tastiera presenti (USB o BLE) → Classica, altrimenti Tablet.
- **Classica** = desktop con icone, taskbar, menu Start, tray; app sempre **massimizzate**
  con barra del titolo.
- Mouse e tastiera diventano input di prima classe **in entrambe le shell**.

Fuori scopo, deciso:

| Escluso | Perché |
|---|---|
| Finestre sovrapposte / ridimensionabili / più app vive | solo-mode con `nv_mem_request`; collo = banda PSRAM; giochi WASM scalati scrivono su tutto il pannello |
| Classica in verticale | taskbar + menu pensati per 1024×600; rotazione disattivata in Classica (torna quella salvata uscendo) |
| Restyling app una per una | le app usano i token tema; camera/galleria/terminale (colori fissi) restano scure, sono a tutto schermo |

## 2. Cosa dice il codice

### 2.1 Shell (`components/nv_ui/nv_ui.cpp`, 4575 righe)
- Un solo screen. Figli: launcher → status bar → shade → strisce gesture → tastiera IME;
  poi app plane, ricerca, cartelle. `lv_layer_top`: Recenti, toast, blocco, PIN, wake-catcher,
  pairing. `lv_layer_sys`: solo cursore.
- Home = launcher mostrato/nascosto (`open_app` :1485, `close_app` :1634), non cambio screen.
- **Superficie di accoppiamento**: ~30 funzioni fuori dai builder toccano launcher/status/
  shade/dock/Recenti; righe ~1640-3470 sono solo launcher. Cuciture naturali: `open_app`,
  `close_app`, callback delle strisce gesture, `ui_refresh_async` (:3374), `nv_ui_go_home` (:3663).
- Niente è fisso a 1024/600 (tutto `LV_HOR_RES`), tranne header/status/inset (`kStatusH` 34,
  `kHeaderH` 44, `kHomeInset` 24, duplicato in `nv_gesture.cpp:19`).
- `NV_EV_APP_LIFECYCLE` esiste ma nessuno lo pubblica → la taskbar lo può usare.
- Rotazione: chip nelle quick settings (:950), rebuild completo; wallpaper 1024×600 RGB565
  in PSRAM (1,2 MB, +1,2 MB copia verticale).
- Icone: 80×80 ARGB8888 in PSRAM; `icon_scaled()` (:511) fa ridimensionamento di qualità con
  cache PSRAM **da 96 slot** (dock 56, ricerca 50, cartelle 30).

### 2.2 Strisce gesture (`nv_gesture.cpp`)
Oggetti cliccabili da 24 px su bordo alto, sinistro, basso: **rubano i clic** sul bordo.
In Classica coprirebbero taskbar (basso), pulsante Start e icone (sinistra), barra titolo e
`X` (alto). → in Classica **tutte spente**; la navigazione passa da taskbar/titolo/tastiera.

### 2.3 Tap guard e indev
`tap_guard_cb` (:2489) è agganciato a **ogni** indev pointer (anche il mouse): annulla il clic
se il puntatore si è mosso > 20 px o c'è gesture. Stato **globale** (`s_press_pt`,
`s_touch_claimed`), condiviso fra touch, mouse e indev di automazione → possibili
interferenze. Da rendere per-indev.

### 2.4 Mouse (`nv_hid_host.cpp:50-92`)
- Indev POINTER + pallino blu 14 px su `layer_sys`. Solo tasto sinistro a LVGL.
- Destro/centrale **letti ma scartati** (`s_mbuttons`, :83); rotella accumulata in
  `s_acc_wheel` ma data solo ai giochi.
- Cursore non nascosto allo stacco.
- **LVGL 9.5 gestisce la rotella sul pointer**: `data->enc_diff` → scroll verticale
  dell'oggetto sotto il cursore (`lv_indev.c:767`, :1620-1660). Basta passarla.
- **Hover già supportato** (`LV_STATE_HOVERED`, `lv_indev.c:1455`) se il read_cb riporta
  posizione con stato RELEASED durante il movimento.
- Clic destro: **non esiste in LVGL** → gestito da noi (`lv_indev_search_obj` + evento custom).

### 2.5 Tastiera (`nv_hid_host.cpp:96-164`, `nv_bt.c:1235`)
- USB e BLE passano dallo stesso `keyboard_report`.
- Letti solo Shift e Ctrl; **Alt e Win ignorati** (ma il byte è in `s_mods`).
- **Nessun evento di rilascio, nessun auto-repeat.**
- **Solo layout US**: niente italiano, niente AltGr (`@ # [ ] è à ù` sbagliati).
- Mappati: Invio, Esc, Backspace, Tab, Canc, frecce, Home, Fine. Mancano PgSu/PgGiù, F1-F12, Ins.
- Gira nel task `hid_host` e prende `lvgl_port_lock(50)`: se scade, **il tasto è perso**.
- Senza campo di testo attivo **ogni tasto è scartato**. Nessun `lv_group` nel progetto.

### 2.6 IME (`nv_ime.cpp:306`)
La tastiera a schermo sale su ogni campo di testo **anche con tastiera fisica collegata**
(nessun controllo `keyboard_present`). Occupa il 42% dello schermo. Da sistemare in ogni caso.

### 2.7 Focus in LVGL 9.5
- Keypad: frecce/Tab/Invio/Esc; Tab = focus next, Invio = CLICKED, Esc = `LV_EVENT_CANCEL`.
- Aggiunti da soli al gruppo di default: button, checkbox, dropdown, slider, switch,
  textarea, roller, table, msgbox, buttonmatrix, bottoni delle liste.
- **Non** aggiunti: `lv_obj` generici cliccabili, `lv_win`, `lv_menu`.
- Il textarea si prende frecce e Invio (multiriga); esce solo con Tab. OK.
- Nelle app: `lv_button_create` usato 12 volte in tutto; la maggior parte dei controlli sono
  `lv_obj` + `LV_EVENT_CLICKED`. Stima fuori dal gruppo: ~13 in Impostazioni, 8 Second Screen,
  8 Camera, 6 Registratore, 6 Video, 4 Galleria, 3 Musica, più launcher/shade/Recenti.

### 2.8 Blocco schermo
PIN su tastierino custom (`build_pin_pad` :3886) in `layer_top`: **non accetta cifre dalla
tastiera fisica**. Da collegare.

### 2.9 Disegno
- Niente ombre/transform/opa-layer (hang renderer SW). Verificato in `lv_obj_style.c:1084`:
  **outline, border_side parziale e `bg_grad` non creano layer** → usabili.
- Bevel 95 = `border_side` TOP|LEFT chiaro su un oggetto + linee BOTTOM|RIGHT scure in
  `LV_EVENT_DRAW_POST` con `lv_draw_line` (nessun layer, nessun oggetto in più).
- Outline tratteggiato non esiste → focus classico con 4 linee `dash_width/gap` in DRAW_POST.
- Cursore: `lv_image` ARGB8888 (RGB565A8 è disabilitato nella config).

### 2.10 Budget
| Risorsa | Oggi | Limite CI | Margine |
|---|---|---|---|
| Firmware / slot OTA 4,5 MB | 4 009 KB (85%) | 90% | ~238 KB |
| SRAM interna statica | 192,5 KB | 210 KB | ~17 KB |
| .bss interna | 57,8 KB | 72 KB | ~14 KB |
| PSRAM statica | ~490 KB | 640 KB | ~150 KB |

Regola: ogni nuovo statico va in `NV_PSRAM_BSS`, oggetti UI nel pool LVGL (già in PSRAM).
Obiettivo del progetto intero: < 2 KB SRAM interna, < 60 KB flash.

## 3. Architettura

```
 nv_hid_host (task hid_host)                      nv_config "ui_shell"
   keyboard_report ── layout IT/US, mods, repeat         │
        │  coda eventi tasto (no lock LVGL)               │
        ▼                                                 ▼
 nv_ui_input (nuovo)  ◄── NV_EV_INPUT_DEVICES ──►  nv_shell (nuovo)
   ├ indev KEYPAD + stack di lv_group                 decide shell effettiva
   ├ scorciatoie globali                              applica solo in home / in close_app
   ├ tasti → IME se campo attivo                           │
   └ mouse: rotella, destro, cursore                 ┌─────┴──────┐
                                                 shell_tablet  shell_classic
                                                 (codice attuale) (nv_ui_classic.cpp)
                         NvShellOps + ShellGeom usati da open_app/close_app/fullscreen/refresh
```

```c
typedef struct { int16_t top_h, bottom_h, hdr_h, inset; bool edge_gestures; } ShellGeom;

typedef struct {
    void (*build_home)(lv_obj_t *scr);
    void (*destroy_home)(void);
    void (*show_home)(bool show);
    void (*frame_build)(lv_obj_t *plane, const NvApp *a);   /* header o barra titolo */
    void (*on_app_opened)(const NvApp *a);
    void (*on_app_closed)(const NvApp *a);
    void (*go_home)(void);
    ShellGeom geom;
} NvShellOps;
```

File nuovi (niente codice aggiunto a `nv_ui.cpp` oltre alle cuciture):
`nv_ui_input.cpp`, `nv_shell.cpp`, `nv_ui_classic.cpp`, `nv_ui_classic_theme.cpp`.

## 4. Fasi

Ogni fase: build pulita, budget CI verdi, OTA, verifica a schermo con `/api/screen`.

### Fase 1 — Fondamenta input (utile subito, anche senza Classica)

| # | Lavoro | Dove |
|---|---|---|
| 1.1 | Coda eventi tasto (FreeRTOS, ~32 elementi, PSRAM): `{key, mods, pressed}`; il task HID non prende più il lock LVGL → niente tasti persi | `nv_hid_host.cpp` |
| 1.2 | Eventi di rilascio (diff `s_prev_keys`), auto-repeat software (500 ms / 33 ms) | `nv_hid_host.cpp` |
| 1.3 | Alt, AltGr, Win; PgSu/PgGiù, F1-F12, Ins | `nv_hid_host.cpp` |
| 1.4 | **Layout italiano** (+ US), scelta in Impostazioni > Lingua > Tastiera fisica; default dalla lingua di sistema | `nv_hid_host.cpp`, `settings_app.cpp` |
| 1.5 | `NV_EV_INPUT_DEVICES` pubblicato su ogni cambio presenza (USB + `ext_*` BLE) | `nv_event_bus.h`, `nv_hid_host.cpp` |
| 1.6 | IME: con tastiera fisica presente la tastiera a schermo **non sale**; un pulsante nella barra la richiama | `nv_ime.cpp` |
| 1.7 | Rotella → `enc_diff`; tasti destro/centrale esposti | `nv_hid_host.cpp` |
| 1.8 | Cursore freccia 12×19 ARGB8888 (~1 KB), nascosto allo stacco e quando il touch viene usato, rimostrato al movimento | `nv_hid_host.cpp` |
| 1.9 | Tap guard per-indev (stato in `lv_indev_get_driver_data`) | `nv_ui.cpp` |
| 1.10 | PIN del blocco schermo da tastiera (cifre, Backspace, Invio) | `nv_ui.cpp` |
| 1.11 | `/api/ui/key` e `/api/ui/type` passano dalla coda (test reali di navigazione) | `nv_web.cpp` |

Accettazione: in Note scrivo `perché @ [x]` su tastiera italiana senza tastiera a schermo;
rotella scorre Impostazioni; 20 tasti al secondo senza perdite; hotplug 20× senza errori.

### Fase 2 — Navigazione da tastiera (entrambe le shell)

1. Indev **KEYPAD** in `nv_ui_input.cpp`, legge la coda. Se un campo IME è attivo, i
   caratteri vanno al campo (come oggi), il resto al gruppo.
2. **Stack di gruppi**: push/pop in home, app, Start, Recenti, ricerca, cartelle, shade,
   dialog "Apri con", blocco. Pop sempre in `LV_EVENT_DELETE` dell'overlay.
3. `nv_ui_focusable(obj)`: aggiunge un `lv_obj` cliccabile al gruppo corrente e mappa
   Invio → CLICKED. Usato dentro i `nv_kit_*` e nei ~50 controlli custom elencati in 2.7
   (Impostazioni rail, launcher, dock, shade chip, Recenti, Files, Musica, Galleria, Camera,
   Video, Registratore, Second Screen).
4. Stile focus: Tablet = anello accento 2 px (outline); Classica = rettangolo tratteggiato.
   Visibile solo dopo l'uso della tastiera (sparisce al primo tocco/clic).
5. Scorciatoie globali (intercettate prima del gruppo):

| Tasti | Tablet | Classica |
|---|---|---|
| Win | home | apre/chiude Start |
| Esc | indietro (`back_clicked`) | indietro |
| Alt+Tab | Recenti, Tab cicla, rilascio Alt apre | idem sulla taskbar |
| Alt+F4 | chiude app | chiude app |
| Win+E / Win+I | Files / Impostazioni | idem |
| Win+L | blocca | blocca |
| Ctrl+Alt+Canc | Monitor di sistema | idem |
| Stamp | screenshot | screenshot |
| F5 | — | aggiorna desktop |

6. App WASM: invariate (leggono `nv_kbd_state`/`nv_mouse_read`, ABI 14). Con input catturato
   da WASM il keypad LVGL non riceve tasti; restano solo Alt+F4 e Win (uscita garantita).

Accettazione: Impostazioni, Note, Files, Calcolatrice e Store usabili senza mouse né touch;
nessun focus perso dopo apertura/chiusura di 10 overlay di fila.

### Fase 3 — Refactor shell (nessun cambio visibile)

1. `ShellGeom` sostituisce le costanti in `open_app`, `nv_ui_app_fullscreen`,
   `ui_refresh_async`, `nv_gesture.cpp`.
2. Launcher attuale dietro `NvShellOps` = `shell_tablet`.
3. `NV_EV_APP_LIFECYCLE` pubblicato in `open_app`/`close_app`.
4. Audit app con altezze fisse: l'area app passa da 1024×498 (Tablet) a 1024×548 (Classica);
   le app devono usare `lv_pct`/flex. Grep `lv_obj_set_height|set_size` con costanti.

**Coordinamento**: `nv_ui.cpp` è pulito ora, ma altre sessioni lo toccano. Fase 3 in un colpo,
commit immediato, avviso ai peer (`SendMessage`) prima e dopo.

Accettazione: screenshot home/app/Recenti/shade/rotazione identici a prima.

### Fase 4 — Stile Classico (solo shell, identità NucleoOS)

Regola: la Classica **richiama** Windows 95, ma resta NucleoOS. Si cambia solo la shell
(desktop, taskbar, Start, tray, cornice finestra). **Le app non si toccano**: niente palette nuova
spinta nei token, niente restyling, niente icone ridisegnate.

1. Stile shell: bordi in rilievo 95 (luce/ombra, raggio 0) su taskbar, pulsanti della taskbar,
   menu Start, popup tray, cornice finestra. Colori presi dal tema NucleoOS attivo (superfici,
   testo, **accento** per barra titolo attiva e selezione): chiaro/scuro e accento dell'utente
   restano validi anche in Classica.
2. Bevel senza ombre: `border_side` + linee in `LV_EVENT_DRAW_POST` (nessun layer).
3. Font: Montserrat/nv_font attuali. Nessun font bitmap.
4. Icone: quelle esistenti (80 px, ridotte con `icon_scaled`), nessun set nuovo.
5. Logo/marchio NucleoOS nel pulsante Start e nella banda laterale del menu.

Accettazione: nessun hang/WDT; le app aperte in Classica sono identiche a Tablet (solo la cornice
cambia); FPS del launcher non peggiorano (`/api/display`).

### Fase 5 — Shell Classica

**Desktop** (1024×570)
- Sfondo: `wallpaper.jpg` o il gradiente NucleoOS attuale (stesso buffer PSRAM).
- Icone **48 px** (`icon_scaled`) in colonne dall'alto a sinistra, etichetta sotto,
  selezione blu. Tap = apre (touch); col mouse: clic seleziona, doppio clic apre.
- Il desktop mostra: Risorse (Files), Impostazioni, Store, Cestino no (non esiste), più le
  app scelte dall'utente ("Aggiungi al desktop" dal menu Start). Chiave `cdesk` (lista id).
- Menu contestuale col destro: Apri, Rimuovi dal desktop, Disinstalla (app store), Proprietà
  (scheda app); su vuoto: Disponi icone, Sfondo, Impostazioni schermo.
- Cache icone: portare `icon_scaled` da 96 slot a LRU (o 160 slot) perché desktop 48 +
  Start 24 raddoppiano le taglie.

**Taskbar** (30 px, in basso)
- `Start` | pulsante app aperta (premuto) | fino a 5 Recenti | tray.
- Tray: Wi-Fi (senza SSID nel testo: privacy screenshot), volume, USB/SD, notifiche
  (contatore), orologio `HH:MM`.
- Popup tray = chip delle quick settings esistenti e lista notifiche (riuso di
  `build_shade_content`, montato in un riquadro 95 invece che nello shade a tendina).

**Menu Start** (in `lv_layer_top`)
- Banda laterale "NucleoOS", voci: Programmi ▸ per categoria, Giochi ▸, Documenti (Files),
  Impostazioni, Trova… (ricerca esistente, digitare filtra subito), Guida (Store guide),
  Blocca, Riavvia, Spegni schermo.
- Sottomenu a cascata: aperti su hover (mouse), su freccia destra (tastiera), su tap (touch).
- Si chiude con Esc, Win, clic fuori.

**Cornice app**
- Barra titolo 22 px: icona 16, nome, `←` (se l'app ha back handler), `_`, `X`.
- `_` = torna al desktop; in solo-mode l'app viene chiusa e resta nelle Recenti in taskbar
  (onesto: niente app "ridotte a icona" vive).
- Area app 1024×548. Fullscreen (giochi, video, camera) copre la taskbar come oggi.
- Uscita da fullscreen: Alt+F4, Win, Esc tenuto 1 s.

**Blocco schermo, dialog "Apri con", toast**: restano quelli attuali, con tema classico.

Accettazione: tutte le app native aperte e chiuse dalla Classica; un gioco WASM da Start
ed uscita con Alt+F4; cambio lingua e tema dal vivo; nessuna striscia gesture attiva.

### Fase 6 — Selezione e modalità automatica

1. Impostazioni > Schermo > "Interfaccia": 3 pillole (pattern `sleep_pick_cb` :396) +
   riga stato "Mouse ✓ Tastiera ✗".
2. `nv_shell` ascolta `NV_EV_SETTINGS_CHANGED` (`ui_shell`) e `NV_EV_INPUT_DEVICES`
   (via `lv_async_call`: il dispatch è sincrono nel task HID).
3. Isteresi: Classica dopo 3 s con entrambi presenti; Tablet dopo 5 s senza.
4. Con app aperta: toast "Mouse e tastiera rilevati: interfaccia classica all'uscita" e
   cambio in `close_app`. In fullscreen nessun toast.
5. Al boot: attesa fine enumerazione USB/BLE (max 3 s) prima di scegliere, per non
   costruire due shell.
6. Cambio shell = `destroy_home` + `build_home` sul percorso di `ui_refresh_async`; in
   Classica rotazione forzata orizzontale, ripristinata in uscita.
7. `/api/ui/shell?mode=` per i test automatici.

Accettazione: collego e stacco mouse/tastiera USB e BLE 20× → nessun doppio switch,
nessun crash, heap PSRAM stabile (`/api/heap/map`).

## 5. Rilasci

| OTA | Fasi | L'utente vede | Rischio |
|---|---|---|---|
| A | 1 | tastiera italiana, niente tastiera a schermo con tastiera fisica, rotella, freccia, PIN da tastiera | basso |
| B | 2 | tutto il sistema usabile da tastiera, scorciatoie | medio (tocca molte app) |
| C | 3 + 4 + 5 + 6 | interfaccia Classica manuale e automatica | medio-alto (shell) |

Stima: A 1 sessione, B 1-2, C 2-3. A e B hanno valore anche se C non si fa.

## 6. Test

| Caso | Come |
|---|---|
| Tasti persi / repeat | `/api/ui/type` 500 caratteri + tastiera reale tenuta premuta |
| Layout IT | tutti i tasti della riga numeri e AltGr in Note |
| Hotplug | USB 20×, BLE 20×, entrambi insieme |
| Focus | 10 overlay aperti/chiusi da tastiera, focus sempre visibile |
| Classica | ogni app nativa, 3 giochi WASM, terminale, Store |
| Regressioni Tablet | swipe, pager, cartelle, Recenti, shade, rotazione |
| Budget | `tools/ci/check_budgets.py`, `/api/heap/map` prima/dopo |
| Privacy | screenshot con SSID/città oscurati prima di pubblicarli |

## 7. Rischi

| Rischio | Mitigazione |
|---|---|
| `nv_ui.cpp` toccato da sessioni parallele | Fase 3 in un colpo solo, commit subito, avviso peer |
| Focus perso dopo overlay | pop del gruppo sempre in `LV_EVENT_DELETE` |
| Tap guard globale fra touch/mouse/automazione | stato per-indev (1.9) |
| Controlli custom senza focus | `nv_ui_focusable` + lista di audit (2.7) |
| Altezza area app diversa fra shell | audit altezze fisse (Fase 3.4) |
| Flash al 85% | < 60 KB totali; font bitmap opzionale |
| SRAM interna al limite | tutto in PSRAM / pool LVGL; misura prima e dopo |
| Cache icone piena | LRU in `icon_scaled` |
| Suono di avvio → brownout | niente suono di avvio; al massimo `nv_audio_chime` esistente |

## 8. Domande aperte

2. Icone del desktop: set fisso (Files, Impostazioni, Store) + scelte utente, o tutte le app?
3. Layout tastiera: solo IT e US, o anche ES/FR/DE come le lingue del sistema?
4. In Classica senza mouse (attivata a mano, solo touch): strisce gesture spente comunque?
   (proposto: sì, taskbar e barra titolo bastano)
