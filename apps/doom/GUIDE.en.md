# Doom Engine

## What it is

The classic Doom engine (doomgeneric, from Chocolate Doom) tuned for this board: 4:3 picture, OPL music, keyboard, mouse and gamepads. Runs the Doom games from the Store and your own WADs. Open it to pick one of the WADs in `/home/doom`: the Doom games in the Store (Freedoom, DOOM Shareware, community megawads) install and open as apps of their own.

## Controls

| Action | Keyboard | Mouse | Gamepad |
|---|---|---|---|
| Forward / back | W S or ↑ ↓ | – | left stick |
| Turn | ← → | horizontal motion | right stick |
| Strafe | A D (or Alt + ← →) | middle button + motion | left stick left/right |
| Fire | Ctrl | left button | right trigger (RT) or X |
| Open doors, press switches | E or Space | right button | A |
| Run | Shift (held) | – | left trigger (LT) |
| Change weapon | 1 – 7 | wheel | LB / Y previous, RB / B next |
| Map | Tab | – | Back |
| Menu (save, load, options) | Esc | – | Start |

In menus: arrows and Enter on the keyboard; on a gamepad the d-pad or stick, A confirms, B goes back. At "yes/no" questions press Y (or A on the gamepad).

With touch: a tap opens the menu outside the game and fires while playing. The Back gesture opens the menu. To really play you need a keyboard, a mouse or a gamepad (USB or Bluetooth).

## Saves and settings

Save and load from the menu (Esc → Save Game / Load Game). Saves and settings stay in each game's private folder. Mouse sensitivity, sound and music volume and screen size are in Esc → Options. With music volume at zero the game runs smoother (FM music takes about a fifth of the CPU).

Downloaded WAD files live in `/home/doom`: you can see them in the Files app, and you can put your own there too (for example `doom.wad` or `doom2.wad` from copies you own); they then show up in the Doom Engine app.

## Credits and license

Engine: doomgeneric (ozkl) and Chocolate Doom (Simon Howard and others), GPL-2.0; OPL music via emu8950 (Mitsutaka Okazaki, Graham Sanderson), MIT. Doom is a trademark of id Software: this app contains no data from the original game.
