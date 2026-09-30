# Game Boy

Un emulatore del **Game Boy** originale (DMG). Gioca i file `.gb` che copi nella cartella **/sdcard/home/roms**: giochi homebrew liberi (per esempio da [Homebrew Hub](https://hh.gbdev.io)) o le copie delle tue cartucce.

## Aggiungere i giochi

Copia i file `.gb` in **/sdcard/home/roms** con l'app File o dal web companion (la cartella viene creata al primo avvio). Tocca un gioco nella lista, oppure sceglilo con la croce del gamepad e premi A. Massimo 4 MB per gioco. I giochi **solo Game Boy Color** non sono supportati; quelli compatibili con entrambe le console funzionano, in bianco e nero.

## Comandi

- **Touch**: croce direzionale a sinistra, **A** e **B** a destra (puoi far scivolare il pollice dall'uno all'altro), **SELECT** in basso a sinistra, **START** in basso a destra. Più dita insieme funzionano.
- **Gamepad** USB o Bluetooth: croce o levetta sinistra; A e Y = A; B e X = B; Start; Back/View = Select.
- **Tastiera USB**: frecce o WASD; Spazio/X/Invio = A; Z/C/Backspace = B; P o Tab = Start; Esc = Select.
- **MENU** (in alto a sinistra), i dorsali L/R o il tasto Guide del gamepad mettono in pausa.

## Il menu di pausa

**Continua**, **Ricomincia** (spegne e riaccende la console), **Zoom** 4x o 3x (più piccolo, lascia più spazio ai comandi), **Colori** (verde, grigio, Pocket, colori stile Game Boy Color) e **Altro gioco** torna alla lista. Zoom e colori restano memorizzati. Il gesto indietro del sistema apre il menu; ripetuto, esce.

## Salvataggi

I giochi con la batteria nella cartuccia salvano come sul Game Boy: il file `<nome>.sav` compare accanto al gioco in /sdcard/home/roms pochi secondi dopo il salvataggio e quando esci.

## Audio

Il suono è emulato (i quattro canali del Game Boy). Se un'altra app sta usando l'altoparlante (per esempio Musica), il gioco parte muto e riprova ogni pochi secondi.

## Crediti e licenze

Emulatore: Peanut-GB di Mahyar Koshkouei, audio MiniGB APU di Alex Baines e Mahyar Koshkouei (licenza MIT). Testi delle licenze:

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
