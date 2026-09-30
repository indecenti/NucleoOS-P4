# GB-Wordyl

A word game for the Game Boy by **bbbbbr**, a rewritten and greatly expanded fork of the original by stacksmashing. Guess the hidden five-letter English word in six tries: letters in the right place, letters in the word but elsewhere and absent letters are marked after each guess. Includes a full dictionary, hard mode, auto-fill and stats (this is the English edition, 0.85).

## Controls

- **Touch**: D-pad on the left, **A** and **B** on the right (you can slide your thumb from one to the other), **SELECT** bottom left, **START** bottom right. Several fingers at once work.
- USB or Bluetooth **gamepad**: D-pad or left stick; A and Y = A; B and X = B; Start; Back/View = Select.
- USB **keyboard**: arrows or WASD; Space/X/Enter = A; Z/C/Backspace = B; P or Tab = Start; Esc = Select.
- **MENU** (top left), the L/R shoulder buttons or the pad's Guide button pause the game.

### In the game

D-pad moves the keyboard cursor, A adds a letter, B removes one, START submits the guess. SELECT + B / SELECT + A move the board cursor, SELECT + START auto-fills exact matches, SELECT three times opens the options (stats, reset, forfeit).

## The pause menu

**Resume**, **Reset** (power-cycles the console), **Zoom** 4x or 3x (smaller, more room for the controls), **Colours** (green, grey, Pocket, Game Boy Color style) and **Exit** closes the game. Zoom and colours are remembered. The system back gesture opens the menu; twice, it exits.

## Sound

Sound is emulated (the Game Boy's four channels). If another app holds the speaker (the Music app, say), the game starts muted and retries every few seconds.

## Credits and licenses

GB-Wordyl by bbbbbr, forked from the original gb-wordle by stacksmashing; sound effects and CBT-FX driver by Coffee 'Valen' Bat; dictionary work by arpruss and zeta_two; more contributors are listed in the repository. Built with GBDK-2020. GPL-3.0.

The ROM is the original, unmodified, from the Homebrew Hub database (https://hh.gbdev.io/game/gb-wordyl).

Emulator: Peanut-GB by Mahyar Koshkouei, MiniGB APU sound by Alex Baines and Mahyar Koshkouei (MIT license).

License texts:

### GB-Wordyl

https://github.com/bbbbbr/gb-wordyl

Licensed under the [GNU General Public License, version 3](https://www.gnu.org/licenses/gpl-3.0.html) (GPL-3.0). The complete source code is in the repository linked above.

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
