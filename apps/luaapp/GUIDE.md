# Lua App

## Cos'è

Il motore delle app Lua dello Store: Lua 5.4 con disegno, testo nitido in più dimensioni, touch e multitouch, tastiera e joypad, timer, salvataggi, HTTP, WebSocket, MQTT e Home Assistant. Ogni app Lua dello Store (Sudoku, RPN Calc, Aria, MQTT Explorer, RetroLove...) ha la sua icona e usa questo motore, che si installa da solo quando serve.

## Le tue app Lua

Aperto dalla sua icona, Lua App elenca gli script che copi nella scheda SD:

- un file singolo: `/sdcard/home/lua/ciao.lua`
- una cartella con `main.lua` (più moduli e immagini): `/sdcard/home/lua/meteo/main.lua`

Tocca un'app per avviarla; il gesto Indietro torna all'elenco. Gli script girano con i permessi di Lua App (schermo, file, rete); per MQTT e Home Assistant serve un pacchetto vero dello Store.

## Per chi programma

Un'app completa:

```lua
function nv.draw()
  ui.clear()
  ui.label(ui.W / 2, 200, "Ciao!", ui.font.huge, ui.theme.accent, "center")
  if ui.button(412, 360, 200, 70, "Suona") then nv.beep() end
end
```

Il riferimento completo dell'API, gli esempi e come pubblicare nello Store sono in `docs/LUA_APPS.md` nel repository. Anche molti giochi scritti per LÖVE (love2d.org) girano senza modifiche.

## Crediti e licenza

Lua 5.4 (Lua.org, PUC-Rio, MIT), json.lua (rxi, MIT), font Montserrat (SIL OFL 1.1). Motore NucleoOS.
