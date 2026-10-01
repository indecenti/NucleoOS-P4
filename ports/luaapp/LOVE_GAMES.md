# LÖVE games in the Store

Third-party LÖVE 11 games packaged for the Lua App engine (`apps/love-*`). Each `apps/<id>/src/` is the upstream tree at the pinned commit (fonts, music, docs and screenshots removed) plus `nucleo.lua` (on-screen controls, set before `main.lua`). `ports/luaapp/fetch_love.sh` downloads and checks the exact tarballs.

| app | upstream | commit | tarball sha256 | license | port changes |
|---|---|---|---|---|---|
| love-2048 | [Przemekkkth/love-2048](https://github.com/Przemekkkth/love-2048) | `bb7fe3a14728` | `79d8b8940799a63666df7d03fa6bcecdfee7b7efa74080108d2801faf32e0999` | MIT | nucleo.lua; dropped: assets/docs |
| love-minesweeper | [henriqueblang/minesweeper](https://github.com/henriqueblang/minesweeper) | `6095210a51ee` | `2b0fdc24fa93a8689f5f57fad22b1c5cf06f8869ac7993e55fdc1a3c37f1352e` | MIT | - |
| love-slide-puzzle | [Przemekkkth/love-slide-puzzle](https://github.com/Przemekkkth/love-slide-puzzle) | `1e2b893f897b` | `832599b55fef11973af984ed9da276e3bf7eacd9f63ac7856cb5399ce868f522` | MIT | dropped: assets/example |
| love-tictactoe | [rafaeljacov/tictactoe_minimax](https://github.com/rafaeljacov/tictactoe_minimax) | `c16b90b84412` | `33602b395c8c92090e9da74e854fe48cc71477363ed83e2ef79209d5cd64a0bd` | MIT | - |
| love-wormy | [Przemekkkth/love-wormy](https://github.com/Przemekkkth/love-wormy) | `699c01649972` | `7aac45c4b3d67b86cacdcb0c0b443438a44da670d6403020e213f935100a2129` | MIT (code), CC0 (pick-up sound) | dropped: assets/doc, assets/music |
| love-gravity-wars | [whyboris/Gravity-Wars](https://github.com/whyboris/Gravity-Wars) | `e66964fc33a3` | `7b9e9468cfc245d36a1adf84dca2425201bb8139f0570902e2dde201b29f2abb` | MIT | dropped: todo.md |
| love-moon-pong | [omrawaley/Moon-Pong](https://github.com/omrawaley/Moon-Pong) | `ea35ba83cb68` | `d3ed09777974532feb00a8e37e4901a5c91b26f8912b7ad14ae54a45a31dd701` | Apache-2.0 | nucleo.lua |
| love-fly-swatter | [sleepycharlyy/fly_swatter.love](https://github.com/sleepycharlyy/fly_swatter.love) | `3f8a07117776` | `57694dc28989d2d7272c14800f7f421e2a3c783dd2d4dfa22c4b2d860211a7fe` | GPL-3.0 | - |
| love-panda-shooter | [PR454D/panda-shooter](https://github.com/PR454D/panda-shooter) | `8cc402521034` | `5aba928139958c8afe2fe96269c7824fe0e5be3d5fbbe1a3fbe40d4457c2f3ed` | GPL-3.0 | nucleo.lua |
| love-robo-house | [Foppygames/robo-house](https://github.com/Foppygames/robo-house) | `a84da6333222` | `ab174feb57ed78e7c49ba0eea42efb5a8e2b6a7f87aa64579df9b49fa3978dd4` | GPL-3.0 | nucleo.lua |
| love-total-breakout | [Shepardeon/total-breakout](https://github.com/Shepardeon/total-breakout) | `8fb63da3b47b` | `2b59c92e201f568c114abb44dd1aa0fc869a40417c68d45920f94bae9970caec` | MIT | nucleo.lua |
| love-ubo | [chezrom/ubo](https://github.com/chezrom/ubo) | `1c69fe290fb0` | `0e7ae2c7eb1194fec301d00f9dbcf4eb284e4e6be54d12fc25ad3335b1bf0b39` | MIT | nucleo.lua |
| love-mr-rescue | [SimonLarsen/mrrescue](https://github.com/SimonLarsen/mrrescue) | `a5be73c60acb` | `2d739a277c6e34593e639e675eaa249c40bafd4214859f0d46033b8acfc21659` | zlib (code), CC BY-SA 3.0 (graphics, sounds) | nucleo.lua; config.lua patched; conf.lua patched; conf.lua patched; dropped: data/sfx/menujazz.ogg, data/sfx/happyfeerings.ogg, data/sfx/roof.ogg, data/sfx/opening.ogg, data/sfx/bundesliga.ogg, data/sfx/scooterfest.ogg, data/sfx/rockerronni.ogg; canvas 256x200 scaled by the OS |
| love-compressed | [Redoxee/CompressedGame](https://github.com/Redoxee/CompressedGame) | `4f3483300447` | `892d73613b232493355e970dc87716304415bc5547e3f3e2f3dd125910d69f2a` | Apache-2.0 | nucleo.lua |
| love-orc-king | [Foppygames/sword-of-the-orc-king](https://github.com/Foppygames/sword-of-the-orc-king) | `d1a3adc094d4` | `f3aa3afa5d6d924aed93be6089fb5cc239fc9ef8f2f79888003d8aa37970ec97` | GPL-3.0 | nucleo.lua |
| love-checkers | [henriqueblang/checkers](https://github.com/henriqueblang/checkers) | `b7f2ce7ab99a` | `87760e068f0b363fce8fe1c009e722891afed142fb78cff79a2c827914a4f6fe` | MIT | - |

Sounds: the games' own short effects only (converted to WAV by tools/lua_pack.py). Wormy's pick-up sound is CC0 (wobbleboxx, OpenGameArt); its music is not included. Mr. Rescue's graphics and sounds are CC BY-SA 3.0 by Tangram Games; its music tracks are not included.

## Looked at and left out

| game | reason |
|---|---|
| bastienleonard/7drl2022 (GPL-3.0) | needs Unicode box-drawing glyphs and a monospace font |
| andmatand/anput (BSD-2) | room generation needs a real background thread: minutes of blocking on the device |
| devapromix/geometry-rush (Apache-2.0) | about 90 ms of Lua per frame |
| TheShouting/Forestor (GPL-3.0) | about 100 ms per frame in the software renderer |
| 44100hertz/gridbattle (zlib) | its canvas scaling and key-config screens need more work |
| tboydston/supremetictactoe (GPL-3.0) | needs typed text (player name) |
| ViorelRoman/belt-mine (GPL-3.0) | uses love.physics |
| SimonLarsen/sienna | assets CC BY-NC / music CC BY-NC-ND |
| dbrabera/ultrabugs | assets CC BY-NC-SA |
| mua/BlockingBad | music CC BY-NC-ND, graphics' licence unclear |
| esadek/jungle-jumper | itch.io asset packs with unclear redistribution terms |
| Przemekkkth/love-sokoban, love-memory-puzzle, love-ink-spill, love-bomberman | no licence |
| Przemekkkth/love-clicker, plageoj/jumper, ashkanfeyzollahi/roadway, KaffeDiem/TowerDefence, Ballance100/LOVE2D-Jam-2022, ecbambrick/ThrustVector | asset origin or licence unclear |
| Przemekkkth flappy-bird, pacman, frogger, battle-city, space-invaders, super-mario-bros, dino-chrome; chadpaulson/missile-command; Jigoku/lovewordle | trademarked titles / third-party IP |
| noooway/love2d_arkanoid_tutorial | sound effects from many sources, licences not verified in time |
| a327ex/SNKRX, Hawkthorne, Mari0 | too big / IP |
