# Waternet

Waternet di Willems Davy: collega tutti i tubi alla sorgente d'acqua ruotando o facendo scorrere le tessere, ispirato ai rompicapi Net e Netslide della raccolta di Simon Tatham.

## Comandi

Le frecce muovono, A ruota o fa scorrere, B torna indietro.

- **Touch**: croce direzionale a sinistra, **A** e **B** a destra (A in basso, B in alto, come sull'Arduboy). Più dita insieme funzionano.
- **Gamepad** USB o Bluetooth: croce o levetta sinistra; A, X e Start = A; B e Y = B.
- **Tastiera USB**: frecce o WASD; Spazio/X/Invio = A; Z/C/Backspace = B.
- **MENU** (in alto a sinistra), i dorsali L/R, Select o il tasto Guide del gamepad mettono in pausa.

## Il menu di pausa

**Continua**, **Zoom** 5x, 6x o 7x (a 7x lo schermo è più grande e i comandi touch sono nascosti: comodo con gamepad o tastiera), **Colori** (bianco, ambra, verde, blu, LCD), **Audio** acceso o spento, **Esci**. Zoom, colori e audio restano memorizzati. Il gesto indietro del sistema apre il menu; ripetuto, esce.

## Salvataggi

La EEPROM dell'Arduboy (1 KB, dove i giochi tengono record e progressi) è salvata nella cartella dell'app poco dopo ogni scrittura e all'uscita.

## Crediti e licenze

**Waternet** è di Willems Davy aka Joyrider3774 (MIT). Sorgente: https://github.com/joyrider3774/waternet_arduboy (commit `2f5e8ce47f`).

Gira sulla libreria Arduboy2 di Scott Allen e collaboratori (licenza BSD a 3 clausole), adattata a NucleoOS; i nomi delle note musicali vengono da ArduboyTones (MIT). Arduboy è un marchio di Arduboy Inc.; questo port non è affiliato.

### Licenza del gioco

```
MIT License

Copyright (c) 2022 Willems Davy

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

### Arduboy2

```
Licensed under the BSD 3-clause license:

Arduboy2 library:
Copyright (c) 2016-2021, Scott Allen
All rights reserved.

The Arduboy2 library was forked from the Arduboy library:
https://github.com/Arduboy/Arduboy
Copyright (c) 2016, Kevin "Arduboy" Bates
Copyright (c) 2016, Chris Martinez
Copyright (c) 2016, Josh Goebel
Copyright (c) 2016, Scott Allen
All rights reserved.

- which is in turn partially based on the Adafruit_SSD1306 library
https://github.com/adafruit/Adafruit_SSD1306
Copyright (c) 2012, Adafruit Industries
All rights reserved.

SetSystemEEPROM example sketch:
Copyright (c) 2018-2020, Scott Allen
All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:
1. Redistributions of source code must retain the above copyright
notice, this list of conditions and the following disclaimer.
2. Redistributions in binary form must reproduce the above copyright
notice, this list of conditions and the following disclaimer in the
documentation and/or other materials provided with the distribution.
3. Neither the name of the copyright holders nor the
names of its contributors may be used to endorse or promote products
derived from this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS ''AS IS'' AND ANY
EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER BE LIABLE FOR ANY
DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
(INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

-------------------------------------------------------------------------------
Licensed under the BSD 2-clause license:

Portions of the Arduboy library, and thus portions of the Arduboy2 library,
based on the Adafruit-GFX library:
https://github.com/adafruit/Adafruit-GFX-Library
Copyright (c) 2012 Adafruit Industries
All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:

- Redistributions of source code must retain the above copyright notice,
  this list of conditions and the following disclaimer.
- Redistributions in binary form must reproduce the above copyright notice,
  this list of conditions and the following disclaimer in the documentation
  and/or other materials provided with the distribution.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
POSSIBILITY OF SUCH DAMAGE.

-------------------------------------------------------------------------------
Licensed under the MIT license:

Code from ArduboyExtra:
https://github.com/yyyc514/ArduboyExtra
Copyright (c) 2015 Josh Goebel

Code for drawing compressed bitmaps:
Copyright (c) 2016 TEAM a.r.g.

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```
