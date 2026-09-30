# Wyrmhole

A tube shooter by **Quinn Painter**, written in C and assembly with GBSDK. Your ship runs around the edge of the wormhole while enemies fly out of the middle: keep shooting inward and don't get hit. The best score is shown on the game over screen.

## Controls

- **Touch**: D-pad on the left, **A** and **B** on the right (you can slide your thumb from one to the other), **SELECT** bottom left, **START** bottom right. Several fingers at once work.
- USB or Bluetooth **gamepad**: D-pad or left stick; A and Y = A; B and X = B; Start; Back/View = Select.
- USB **keyboard**: arrows or WASD; Space/X/Enter = A; Z/C/Backspace = B; P or Tab = Start; Esc = Select.
- **MENU** (top left), the L/R shoulder buttons or the pad's Guide button pause the game.

## The pause menu

**Resume**, **Reset** (power-cycles the console), **Zoom** 4x or 3x (smaller, more room for the controls), **Colours** (green, grey, Pocket, Game Boy Color style) and **Exit** closes the game. Zoom and colours are remembered. The system back gesture opens the menu; twice, it exits.

## Sound

Sound is emulated (the Game Boy's four channels). If another app holds the speaker (the Music app, say), the game starts muted and retries every few seconds.

## Credits and licenses

Wyrmhole by Quinn Painter, MIT license. Music and SFX by Coffee Bat (CBT-FX); WitchFont8 by Lavenfurr; Old School Shooter Set by Mumu (OpenGameArt); wormhole image by AlexAntropov86 (Pixabay); hUGEDriver by SuperDisk.

The ROM is the original, unmodified, from the Homebrew Hub database (https://hh.gbdev.io/game/wyrmhole).

Emulator: Peanut-GB by Mahyar Koshkouei, MiniGB APU sound by Alex Baines and Mahyar Koshkouei (MIT license).

License texts:

### Wyrmhole

https://github.com/QuinnPainter/Wyrmhole

```
Copyright (c) 2022 Quinn Painter

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
