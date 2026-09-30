# Game Boy

An emulator of the original **Game Boy** (DMG). It plays the `.gb` files you copy to **/sdcard/home/roms**: free homebrew games (for example from [Homebrew Hub](https://hh.gbdev.io)) or backups of your own cartridges.

## Adding games

Copy `.gb` files to **/sdcard/home/roms** with the Files app or the web companion (the folder is created on first start). Tap a game in the list, or pick it with the pad's D-pad and press A. Up to 4 MB per game. **Game Boy Color only** games are not supported; dual-mode ones work, in black and white.

## Controls

- **Touch**: D-pad on the left, **A** and **B** on the right (you can slide your thumb from one to the other), **SELECT** bottom left, **START** bottom right. Several fingers at once work.
- USB or Bluetooth **gamepad**: D-pad or left stick; A and Y = A; B and X = B; Start; Back/View = Select.
- USB **keyboard**: arrows or WASD; Space/X/Enter = A; Z/C/Backspace = B; P or Tab = Start; Esc = Select.
- **MENU** (top left), the L/R shoulder buttons or the pad's Guide button pause the game.

## The pause menu

**Resume**, **Reset** (power-cycles the console), **Zoom** 4x or 3x (smaller, more room for the controls), **Colours** (green, grey, Pocket, Game Boy Color style) and **Other game** goes back to the list. Zoom and colours are remembered. The system back gesture opens the menu; twice, it exits.

## Saves

Games with a battery in the cartridge save as on a Game Boy: `<name>.sav` appears next to the game in /sdcard/home/roms a few seconds after the game saves, and when you leave.

## Sound

Sound is emulated (the Game Boy's four channels). If another app holds the speaker (the Music app, say), the game starts muted and retries every few seconds.

## Credits and licenses

Emulator: Peanut-GB by Mahyar Koshkouei, MiniGB APU sound by Alex Baines and Mahyar Koshkouei (MIT license). License texts:

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
