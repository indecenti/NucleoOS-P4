# Rhythm Land

A rhythm game by **Sinusoid Studios**: martendo (programming, music and sound effects) and Adrian-kwok (art and design). Pick a game from the menu, listen to the cues and press the button exactly on the beat; the jukebox plays the songs. At start-up the game tests the emulator's accuracy and shows a 'bad emulator' warning here: press Left, Up, SELECT and START together to continue, then it plays normally.

## Controls

- **Touch**: D-pad on the left, **A** and **B** on the right (you can slide your thumb from one to the other), **SELECT** bottom left, **START** bottom right. Several fingers at once work.
- USB or Bluetooth **gamepad**: D-pad or left stick; A and Y = A; B and X = B; Start; Back/View = Select.
- USB **keyboard**: arrows or WASD; Space/X/Enter = A; Z/C/Backspace = B; P or Tab = Start; Esc = Select.
- **MENU** (top left), the L/R shoulder buttons or the pad's Guide button pause the game.

### In the game

At the warning screen: Left + Up + SELECT + START together. Menu: D-pad chooses, A confirms, B goes back. In the games: A on the beat.

## The pause menu

**Resume**, **Reset** (power-cycles the console), **Zoom** 4x or 3x (smaller, more room for the controls), **Colours** (green, grey, Pocket, Game Boy Color style) and **Exit** closes the game. Zoom and colours are remembered. The system back gesture opens the menu; twice, it exits.

## Saves

The cartridge has a battery: its save data stays on the device between sessions.

## Sound

Sound is emulated (the Game Boy's four channels). If another app holds the speaker (the Music app, say), the game starts muted and retries every few seconds.

## Credits and licenses

Rhythm Land by martendo and Adrian-kwok, MIT license. Uses Game Boy Tracker by Stéphane Hockenhull, the SoundSystem sound driver by Bob Koon and the gb-vwf text engine by Eldred Habert (ISSOtm); see ATTRIBUTION.md in the repository.

The ROM is the original, unmodified, from the Homebrew Hub database (https://hh.gbdev.io/game/rhythm-land).

Emulator: Peanut-GB by Mahyar Koshkouei, MiniGB APU sound by Alex Baines and Mahyar Koshkouei (MIT license).

License texts:

### Rhythm Land

https://github.com/sinusoid-studios/rhythm-land

```
Copyright (c) 2021 martendo and eat_butt_loser_butt

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
