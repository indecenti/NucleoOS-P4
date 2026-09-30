# Bit Bang

Un rompicapo per Game Boy di **StudioGuma** (2024). Converti numeri decimali in binario con quattro operazioni — incrementa, decrementa, scorri a sinistra, scorri a destra — cercando di risolvere ogni puzzle con il minimo di mosse.

## Comandi

- **Touch**: croce direzionale a sinistra, **A** e **B** a destra (puoi far scivolare il pollice dall'uno all'altro), **SELECT** in basso a sinistra, **START** in basso a destra. Più dita insieme funzionano.
- **Gamepad** USB o Bluetooth: croce o levetta sinistra; A e Y = A; B e X = B; Start; Back/View = Select.
- **Tastiera USB**: frecce o WASD; Spazio/X/Invio = A; Z/C/Backspace = B; P o Tab = Start; Esc = Select.
- **MENU** (in alto a sinistra), i dorsali L/R o il tasto Guide del gamepad mettono in pausa.

### Nel gioco

La croce si sposta tra i pulsanti, A ne attiva uno, B azzera o salta il puzzle, START accende/spegne la musica, SELECT gli effetti sonori.

## Il menu di pausa

**Continua**, **Ricomincia** (spegne e riaccende la console), **Zoom** 4x o 3x (più piccolo, lascia più spazio ai comandi), **Colori** (verde, grigio, Pocket, colori stile Game Boy Color) e **Esci** chiude il gioco. Zoom e colori restano memorizzati. Il gesto indietro del sistema apre il menu; ripetuto, esce.

## Audio

Il suono è emulato (i quattro canali del Game Boy). Se un'altra app sta usando l'altoparlante (per esempio Musica), il gioco parte muto e riprova ogni pochi secondi.

## Crediti e licenze

Bit Bang di StudioGuma, fatto con GBDK 2020, GBTD/GBMB e hUGETracker. Codice con licenza GNU GPL versione 3 o successive; grafica e suoni con licenza Creative Commons Attribuzione - Condividi allo stesso modo 4.0.

La ROM è quella originale, non modificata, dal database Homebrew Hub (https://hh.gbdev.io/game/bit_bang).

Emulatore: Peanut-GB di Mahyar Koshkouei, audio MiniGB APU di Alex Baines e Mahyar Koshkouei (licenza MIT).

Testi delle licenze:

### Bit Bang (code)

https://github.com/StudioGuma/bit_bang

Licenza [GNU General Public License, versione 3](https://www.gnu.org/licenses/gpl-3.0.html) (GPL-3.0). Il codice sorgente completo è nel repository indicato sopra.

### Bit Bang (assets)

Licenza [Creative Commons Attribuzione - Condividi allo stesso modo 4.0](https://creativecommons.org/licenses/by-sa/4.0/deed.it) (CC BY-SA 4.0).

### Peanut-GB

https://github.com/deltabeard/Peanut-GB

```
Copyright (c) 2018-2023 Mahyar Koshkouei

Permission is hereby granted, free of charge, to any person obtaining a copy of this software and
associated documentation files (the "Software"), to deal in the Software without restriction,
including without limitation the rights to use, copy, modify, merge, publish, distribute,
sublicense, and/or sell copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all copies or
substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT
NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM,
DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT
OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
```

### MiniGB APU

https://github.com/deltabeard/Peanut-GB/tree/master/examples/sdl2/minigb_apu

```
Copyright (c) 2017 Alex Baines
Copyright (c) 2019 Mahyar Koshkouei

Permission is hereby granted, free of charge, to any person obtaining a copy of this software and
associated documentation files (the "Software"), to deal in the Software without restriction,
including without limitation the rights to use, copy, modify, merge, publish, distribute,
sublicense, and/or sell copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all copies or
substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT
NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM,
DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT
OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
```
