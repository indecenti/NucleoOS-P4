# Shock Lobster

Un gioco arcade di **Dave VanEe** (tbsp), realizzato per la Game Boy Competition 2021. Sei un umile astice dotato di poteri magici: parti con poche abilità per evitare gli ostacoli e fare danni, poi spendi le perle raccolte per sbloccare altre abilità, potenziamenti e oggetti. SELECT nella schermata di stato mostra i dettagli di ogni abilità.

## Comandi

- **Touch**: croce direzionale a sinistra, **A** e **B** a destra (puoi far scivolare il pollice dall'uno all'altro), **SELECT** in basso a sinistra, **START** in basso a destra. Più dita insieme funzionano.
- **Gamepad** USB o Bluetooth: croce o levetta sinistra; A e Y = A; B e X = B; Start; Back/View = Select.
- **Tastiera USB**: frecce o WASD; Spazio/X/Invio = A; Z/C/Backspace = B; P o Tab = Start; Esc = Select.
- **MENU** (in alto a sinistra), i dorsali L/R o il tasto Guide del gamepad mettono in pausa.

### Nel gioco

Schermata dell'equipaggiamento: la croce seleziona, A conferma/sblocca/attiva, B torna indietro, SELECT mostra i dettagli, START inizia. In battaglia: la croce sceglie la coppia di pulsanti attiva, A/B usano le abilità corrispondenti, START mette in pausa.

## Il menu di pausa

**Continua**, **Ricomincia** (spegne e riaccende la console), **Zoom** 4x o 3x (più piccolo, lascia più spazio ai comandi), **Colori** (verde, grigio, Pocket, colori stile Game Boy Color) e **Esci** chiude il gioco. Zoom e colori restano memorizzati. Il gesto indietro del sistema apre il menu; ripetuto, esce.

## Salvataggi

La cartuccia ha la batteria: i dati salvati restano sul dispositivo tra una partita e l'altra.

## Audio

Il suono è emulato (i quattro canali del Game Boy). Se un'altra app sta usando l'altoparlante (per esempio Musica), il gioco parte muto e riprova ogni pochi secondi.

## Crediti e licenze

Shock Lobster, copyright 2021 Dave VanEe, licenza zlib. Usa gb-starter-kit e gb-vwf di ISSOtm, hUGEDriver di SuperDisk, il driver degli effetti sonori e il codice BCD di PinoBatch, una libreria per i record di H. Mulder, la grafica Lucky Bestiary di LuckyCassette, i caratteri MinimalPixel di Mounir Tohami ed Electrox di Dennis Ludlow; musiche dai GB Studio Community Assets di DeerTears, Tomas Danko (FridgeMusic, CC BY 4.0) e Tronimal.

La ROM è quella originale, non modificata, dal database Homebrew Hub (https://hh.gbdev.io/game/shock-lobster).

Emulatore: Peanut-GB di Mahyar Koshkouei, audio MiniGB APU di Alex Baines e Mahyar Koshkouei (licenza MIT).

Testi delle licenze:

### Shock Lobster

https://github.com/tbsp/shock-lobster

```
Copyright (c) 2021 Dave VanEe

This software is provided 'as-is', without any express or implied warranty. In
no event will the authors be held liable for any damages arising from the use of
this software.

Permission is granted to anyone to use this software for any purpose, including
commercial applications, and to alter it and redistribute it freely, subject to
the following restrictions:

1.  The origin of this software must not be misrepresented; you must not claim
    that you wrote the original software. If you use this software in a product,
    an acknowledgment in the product documentation would be appreciated but is
    not required.

2.  Altered source versions must be plainly marked as such, and must not be
    misrepresented as being the original software.

3.  This notice may not be removed or altered from any source distribution.
```

### FridgeMusic by Tomas Danko

https://github.com/DeerTears/GB-Studio-Community-Assets

Licenza [Creative Commons Attribuzione 4.0](https://creativecommons.org/licenses/by/4.0/deed.it) (CC BY 4.0).

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
