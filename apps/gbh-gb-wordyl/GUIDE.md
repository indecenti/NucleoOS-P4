# GB-Wordyl

Un gioco di parole per Game Boy di **bbbbbr**, versione riscritta e molto ampliata dell'originale di stacksmashing. Indovina la parola inglese di cinque lettere in sei tentativi: dopo ogni tentativo vengono segnate le lettere al posto giusto, quelle presenti ma altrove e quelle assenti. Dizionario completo, modalità difficile, riempimento automatico e statistiche (questa è l'edizione inglese, 0.85).

## Comandi

- **Touch**: croce direzionale a sinistra, **A** e **B** a destra (puoi far scivolare il pollice dall'uno all'altro), **SELECT** in basso a sinistra, **START** in basso a destra. Più dita insieme funzionano.
- **Gamepad** USB o Bluetooth: croce o levetta sinistra; A e Y = A; B e X = B; Start; Back/View = Select.
- **Tastiera USB**: frecce o WASD; Spazio/X/Invio = A; Z/C/Backspace = B; P o Tab = Start; Esc = Select.
- **MENU** (in alto a sinistra), i dorsali L/R o il tasto Guide del gamepad mettono in pausa.

### Nel gioco

La croce muove il cursore sulla tastiera, A aggiunge una lettera, B la toglie, START conferma il tentativo. SELECT + B / SELECT + A spostano il cursore sulla griglia, SELECT + START riempie le lettere già indovinate, SELECT tre volte apre le opzioni (statistiche, azzera, arrenditi).

## Il menu di pausa

**Continua**, **Ricomincia** (spegne e riaccende la console), **Zoom** 4x o 3x (più piccolo, lascia più spazio ai comandi), **Colori** (verde, grigio, Pocket, colori stile Game Boy Color) e **Esci** chiude il gioco. Zoom e colori restano memorizzati. Il gesto indietro del sistema apre il menu; ripetuto, esce.

## Audio

Il suono è emulato (i quattro canali del Game Boy). Se un'altra app sta usando l'altoparlante (per esempio Musica), il gioco parte muto e riprova ogni pochi secondi.

## Crediti e licenze

GB-Wordyl di bbbbbr, derivato dall'originale gb-wordle di stacksmashing; effetti sonori e driver CBT-FX di Coffee 'Valen' Bat; lavoro sul dizionario di arpruss e zeta_two; gli altri contributori sono elencati nel repository. Fatto con GBDK-2020. GPL-3.0.

La ROM è quella originale, non modificata, dal database Homebrew Hub (https://hh.gbdev.io/game/gb-wordyl).

Emulatore: Peanut-GB di Mahyar Koshkouei, audio MiniGB APU di Alex Baines e Mahyar Koshkouei (licenza MIT).

Testi delle licenze:

### GB-Wordyl

https://github.com/bbbbbr/gb-wordyl

Licenza [GNU General Public License, versione 3](https://www.gnu.org/licenses/gpl-3.0.html) (GPL-3.0). Il codice sorgente completo è nel repository indicato sopra.

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
