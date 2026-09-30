# Game Boy homebrew apps (`apps/gbh-*`) — games, licenses, verification

Each app is the `ports/gameboy` front-end (Peanut-GB + MiniGB APU, MIT; see `ports/gameboy/ROMS.md`)
with one ROM compiled in, unmodified. All ROMs come from the Homebrew Hub database
(https://github.com/gbdev/database) at commit `50293559a496a3e20382fbf6a2e84b70ec622f88`, pinned by
sha256 in `fetch.sh`. Texts and credits: `games.json`; build: `build.sh`.

## Selection rules

From the database entries with `platform: "GB"` and `typetag: "game"` (428 at the pinned commit):
only licenses that allow redistribution in a free store (MIT, Zlib, 0BSD, Unlicense, CC0, GPL,
CC BY, CC BY-SA). Entries with an empty license (393) or a non-commercial / no-derivatives license
were not considered. Every remaining license was checked on 2026-09-30 against the game's own
repository (LICENSE file + README, via the GitHub/GitLab API) or website, because the database can
be wrong. No ROM is CGB-only (header byte 0x143 = 0xC0); dual-mode ROMs (0x80) run in DMG mode.
Each ROM was then run 1500 frames in the native harness (`build.sh test`) with scripted input,
and the dumps were looked at: every game below reaches its title/gameplay and reacts to input.

## Published (19)

| App id | Game | Author | License | Source | ROM sha256 | Verification |
|---|---|---|---|---|---|---|
| `gbh-5-mazes` | 5 Mazes | godai (Gniazdo Światów); music AJ Booker | MIT | https://github.com/godai78/5-Mazes | `4cb6a4a021056164dfad11ba041cb1c36d7453d2ebe6bb7fa9fe4b3eadbc4c60` | Repo LICENSE = MIT (c) 2023 godai78; the database says CC BY-SA 4.0 — both allow redistribution, credits given for both. GB Studio game. |
| `gbh-5-more-mazes` | 5 More Mazes | godai; music AJ Booker | MIT | https://github.com/godai78/5-more-mazes | `95d095823ecbf9f31482e7981a06e34a1d5b966d4a5b62bf89f4f89b2f1cf318` | As above (repo MIT, database CC BY-SA 4.0). |
| `gbh-5-mazes-master-levels` | 5 Mazes: Master levels | godai; music AJ Booker | MIT | https://github.com/godai78/5-mazes-Master-levels | `abc7cb1a6f83a8770822c189dc2a978116a025cd393106791e1b6c977c8b4034` | As above; 512 KB ROM. |
| `gbh-airplanz` | AIRPLANZ | NotImplementedLife | GPL-3.0 | https://github.com/NotImplementedLife/AIRPLANZ | `736b821e17f5509d9b0088289977e1232e18338cad923f3e5588300d8622a662` | Repo LICENSE = GPL-3.0 (database: GPL-3.0-only). Source link in the description. |
| `gbh-big2small` | Big2Small | Matthew D. Steele | GPL-3.0-or-later | https://github.com/mdsteele/big2small | `59c096333f93c12c8eb25a5fcffa2be15001abcd5d20b9a82c2bc2dae7e625b2` | Repo LICENSE GPL-3.0; README: "GPL version 3 or later". Dual-mode (0x80). |
| `gbh-bit-bang` | Bit Bang | StudioGuma | GPL-3.0-or-later code, CC BY-SA 4.0 assets | https://github.com/StudioGuma/bit_bang | `2a4e25370cab3fa7b76a7ca630321b585be70aa4030ac776228c0641154c7735` | Repo LICENSE GPL-3.0; README + itch page: assets CC BY-SA 4.0 (attribution in guide). |
| `gbh-carazu` | Carazu | Martin Holtkamp | GPL-3.0 | https://github.com/mholtkamp/carazu | `eef9dea9ecba15be7b21f3495e8c8bad937a0b2245cac09eec8baf61dfc91fb5` | Repo LICENSE.md GPL-3.0; README "Licensed under GPLv3". |
| `gbh-crossconnect` | CrossConnect | Quinn Painter | MIT | https://github.com/QuinnPainter/CrossConnect | `5465e7b4aa37ee0d630d2e63a026f282e8d17c213a2bd9f087417e68061e8b95` | Repo LICENSE MIT (c) 2022 Quinn Painter. Third-party: WitchFont8 (Lavenfurr), credited. Dual-mode; the colour title screenshot is not used. |
| `gbh-dashy-no-witch` | Dashy no Witch | voxel | 0BSD | https://voxel.itch.io/dashy-no-witch | `102e04c6559afec6d007c1ded5c32c14492a1fa680b86384e3fb8100e189bdf1` | No repository; the itch.io page states "Dashy no Witch is supplied under the 0BSD license". |
| `gbh-game-boy-of-life` | Game (Boy) of Life | StudioGuma | GPL-3.0-or-later code, CC BY-SA 4.0 assets | https://github.com/StudioGuma/game_boy_of_life | `d491f5d18f0fad55e660277210d1a07a7b52b65c480927e5c78f2f70d824895a` | Repo LICENSE GPL-3.0; README: assets CC BY-SA 4.0. Store pictures are harness frames (the database only has a cover). |
| `gbh-gb-wordyl` | GB-Wordyl (English 0.85) | bbbbbr, after stacksmashing | GPL-3.0 | https://github.com/bbbbbr/gb-wordyl | `acc12e3e920e0c432606846da60a5ca9bba21f8012304e8de4a26f88273d14de` | Repo LICENSE.txt GPL-3.0 (database entry points to the old bbbbbr/gb-wordle URL, which redirects). Dual-mode; the colour screenshot is not used. |
| `gbh-pelmanism` | Pelmanism | TeamKNOx (Osamu Ohashi) | MIT | https://gitlab.com/teamknox/gbccardgame | `3dcfc0c06543f3006a2d8344661c9da94b5266c34a4e310d8f42b8f17ded6a0a` | No LICENSE file; README section "License": "Copyright (c) Osamu OHASHI, Distributed under the MIT License". |
| `gbh-jp` | JP | Graham Coulby (IonicLimb) | MIT | https://github.com/gcoulby/JP | `417fa2e7287fb26f27bc90f51e36c3edb7f5a6745654af870b2cb6fa587bdc8a` | Repo LICENSE MIT, Copyright 2021 Graham Coulby. |
| `gbh-postbot` | PostBot | Tobias Rojahn | MIT | https://github.com/MasterIV/PostBot | `65824d3d7df8003f19923b38055a0660c20542cc363894091da661d9d49d13ad` | Repo LICENSE MIT (c) 2018 Tobias Rojahn. |
| `gbh-rex-run` | Rex Run | el_seyf | GPL-3.0 | https://github.com/elseyf/rex-run-gb | `9e074f66648f709528e4fc908dd979d098d74ab54cb24d09a75fa69d674eccaf` | Repo LICENSE GPL-3.0. Port of the Chrome dinosaur game (Chromium's game is BSD-licensed open source). |
| `gbh-rex-runner` | Rex Runner GB | The Void | MIT | https://github.com/etdv-thevoid/rex-runner-gb | `91bd12159d30e86cf4eb0312f28ff1c394e701085d6a8ca641ed92b2bcc8429c` | Repo LICENSE MIT (c) 2024 The Void. Dual-mode. Same Chromium-game note as Rex Run. |
| `gbh-shock-lobster` | Shock Lobster | Dave VanEe (tbsp) | Zlib | https://github.com/tbsp/shock-lobster | `f14efd0340903b1b00d7e8b4b39687d4bc33355ac7742267ccfddfe92f54a611` | Repo LICENSE zlib (c) 2021 Dave VanEe. Third-party assets listed in the README (all free-use; FridgeMusic CC BY 4.0) are credited in the guide. |
| `gbh-squishy-the-turtle` | Squishy the Turtle (MAGFest edition) | cppchriscpp (potatolain) | MIT | https://github.com/potatolain/SquishyTheTurtle | `ac1e2eff95cdcf71ed931bd16c88f080038977b8b9dcde433bc023ee6b86c021` | Repo LICENSE MIT (c) 2015. The entry's playable file `squishy-magfest.gb` is used (not `ludum-dare.gb`). |
| `gbh-tobu-deluxe` | Tobu Tobu Girl Deluxe | Tangram Games (sound potato-tan) | MIT code, CC BY 4.0 assets | https://github.com/SimonLarsen/tobutobugirl-dx | `0a0e8018dbbc8d7f8cd99f05e7cdc7b4cc9e358ecfe9377ebfb2291a84c6e310` | Repo LICENSE MIT (c) 2017 Tangram Games; README: all assets CC BY 4.0 (attribution in description + guide). Dual-mode; runs as DMG, so the colour screenshots are not used. |

## Excluded

| Database slug | Reason |
|---|---|
| `tobutobugirl`, `2048gb` | Already in the store as `gbtobu` / `gb2048` (ports/gameboy). |
| `renegade-rush` (MIT, verified) | Does not react to START/A under Peanut-GB: still on the title screen after 1500 frames of scripted input. Unplayable here. |
| `snake` (WTFPL) | Hangs the emulator: the harness produced no frame for 10 minutes. |
| `tuff` (database: MIT) | README: graphics, characters, sounds and maps "Copyright (c) 2014 Ivo Wetzel. All rights reserved"; only the code is free. |
| `maxpirate`, `maxpirateeb` (MIT code) | Graphics from GibbonGL's paid *GB Oracles* packs and music by beatscribe, with no redistribution terms published: unclear. |
| `domination` (database: MIT) | No LICENSE file in the repository, author page unreachable: not verifiable. |
| `grub-glide` (database: Zlib) | Repository has no license: not verifiable. |
| `madpews-battlegrounds` (database: Unlicense) | Repository (madpew/hello-asm) and itch.io page state no license: not verifiable. |
| `plantboy` (database: MIT) | Repository exezin/PlantBoy no longer exists: not verifiable. |
| `crystal-lake` (database: Zlib) | No repository, the bundled readme has no license, and it uses Friday the 13th characters (third-party IP). |
| `dorotea`, `scorching-light` (CC BY-NC-ND 4.0), `plutos-corner`, `superconnard` (CC BY-NC-SA) | Non-commercial / no-derivatives licenses. |
| 393 GB game entries with no license | Unknown license. |

Not considered: entries without `platform` (245 games) and GBC-only entries (Peanut-GB is DMG-only).
