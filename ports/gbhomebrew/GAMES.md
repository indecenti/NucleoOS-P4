# Game Boy homebrew apps (`apps/gbh-*`) — games, licenses, verification

Each app is the `ports/gameboy` front-end (Peanut-GB + MiniGB APU, MIT; see `ports/gameboy/ROMS.md`)
with one ROM compiled in, unmodified. All ROMs come from the Homebrew Hub database
(https://github.com/gbdev/database) at commit `50293559a496a3e20382fbf6a2e84b70ec622f88`, pinned by
sha256 in `fetch.sh`. Texts and credits: `games.json`; build: `build.sh`.

## Selection rules

From the database entries with `platform: "GB"` and `typetag: "game"` (428 at the pinned commit):
only licenses that allow redistribution in a free store (MIT, Zlib, 0BSD, Unlicense, CC0, GPL,
CC BY, CC BY-SA). Entries with an empty license (393) or a non-commercial / no-derivatives license
were not considered in the first pass. A second pass (same pinned commit, which is still the latest
`master` on 2026-09-30) looked at every `typetag: "game"` entry with platform `GB` or no platform
that links a GitHub/GitLab repository, whatever the database license field says, and read the
license detected in the repository (LICENSE file) plus the README for asset terms: 17 more games.
Every remaining license was checked on 2026-09-30 against the game's own
repository (LICENSE file + README, via the GitHub/GitLab API) or website, because the database can
be wrong. No ROM is CGB-only (header byte 0x143 = 0xC0); dual-mode ROMs (0x80) run in DMG mode.
Each ROM was then run 1500 frames in the native harness (`build.sh test`) with scripted input,
and the dumps were looked at: every game below reaches its title/gameplay and reacts to input.

## Published (36)

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
| `gbh-alien-invasion` | Alien Invasion | NiliusJulius (FerrantePescara) | GPL-3.0 | https://github.com/NiliusJulius/Alien-Invasion | `78cbb13080f587c108d8a18661db7d7b4a5c14c730f5bb86794a41132035fbf0` | Repo LICENSE GPL-3.0; database license empty, no platform field. |
| `gbh-brekstas-cat` | Breksta's Cat | NotImplementedLife | GPL-3.0 | https://github.com/NotImplementedLife/brekstascat | `e46dc09ce51b0bf3ca5c4539350ab7e2d4ea4d428329540605aa0a978ac3ece8` | Repo LICENSE GPL-3.0; database license empty. |
| `gbh-bustfree` | Bustfree! | Adam Smith (adamsmasher) | MIT | https://github.com/adamsmasher/bustfree | `05640bb1aac6201ab271cf9f6fd9ef5e37fef235514106f81ffa3fcadf06e23a` | Repo LICENSE MIT (c) 2021 Adam Smith; README states MIT. Database license empty. |
| `gbh-slime-trials` | Slime Trials | Canight (Canite) | MIT | https://github.com/Canite/SlimeTrials | `38879f865221131e83f59e1168417566356fb8139e3e59c76422bd2b05305b14` | Repo LICENSE MIT (c) 2023 Canite; database license empty. |
| `gbh-toko` | Toko | demekala (sabaKopala) | MIT | https://github.com/kopalaGitHub/tokoGame | `dfcda97fdca2c8cdb2ade7c0fbf07d3f32f7cf893c4fd313d5ee1a9df0d7d010` | Repo LICENSE MIT (c) 2023 sabaKopala; database license empty. |
| `gbh-el-dueloroso` | El Dueloroso | Adrián JG | GPL-3.0 | https://github.com/ajgalan/el-dueloroso | `21b8c78aec985c9c9335db154b186adde9e2aaba32fc7ed76c9cf37cb8ffcf3c` | Repo LICENSE GPL-3.0, README "Licenses" GPLv3; custom graphics and original music by the author. |
| `gbh-gb-corp` | GB Corp. | Dr. Ludos | MIT | https://github.com/drludos/GBcorp | `5a39926a23ff50448859b2d924d8bba5a8c68db91563783f0c593a3aa8e7bad3` | Repo LICENSE MIT (c) 2021 Dr. Ludos. Compo ROM from the database; the later commercial edition is not used. Dual-mode. |
| `gbh-abducted` | Abducted | Micheal MacLean (grimmrobegames) | MIT | https://github.com/mrmmaclean/Abducted | `bc8e9612e8cdddc77a5c7ae72f549b95f1356c53eeae929ca02a3bd5278d79cc` | Repo License.md MIT (c) 2023 Micheal MacLean; database license empty. GB Studio game. |
| `gbh-libbet` | Libbet and the Magic Floor | Damian Yerrick (PinoBatch); Magic Floor by Martin Korth | Zlib | https://github.com/pinobatch/libbet | `079d161bf2bff4f3baec01339b4f6f02ff6f966c69456885a165b97aac11fa12` | Repo LICENSE zlib (c) 2018 Damian Yerrick; README: zlib, Magic Floor design by Martin Korth. Dual-mode. |
| `gbh-7heaven` | 7heaven | Nikku4211 | MIT | https://github.com/nikku4211/7heaven | `2afb448affe9a4b6de1b34c2b9e6ffd06e5ddbde42387f9a2057eb0cfc5bd38b` | Repo LICENSE MIT (c) 2023 Nikku4211; database license empty. Dual-mode. |
| `gbh-rhythm-land` | Rhythm Land | martendo (programming, music), Adrian-kwok (art, design) | MIT | https://github.com/sinusoid-studios/rhythm-land | `ca7ae6e97011fb423e85514aa9ca245967c01fcfe084ca4232106a39dd6ee9a4` | Repo LICENSE MIT (c) 2021 martendo and co.; credits in README/ATTRIBUTION.md. Shows an accuracy warning under Peanut-GB; the documented bypass (Left+Up+SELECT+START) was scripted in the harness and gameplay reached. |
| `gbh-square-fall` | Square Fall | bjorn_nah | MIT | https://github.com/bjorn-nah/square_fall | `d16024a37bec3d7efa640bdbdb5ef7e33329302219022f81a59e2963a2c053ed` | Repo LICENSE MIT (c) 2021 Bjorn; database license empty. |
| `gbh-skeleton-crew` | Skeleton Crew | Chris Lewis-Hou (staticlinkage); music by Nikku4211 | MIT | https://github.com/chrislewisdev/skeleton-crew | `58b34052aa52e7d466432779dadfa2eeed299d6810cf4dd028c01519b444c9cd` | Repo LICENSE MIT (c) 2023 Chris Lewis-Hou; README: art by the author, music by Nikku4211. |
| `gbh-sushi-nights` | Sushi Nights | Zalo, Kirblue, Maikel Ortega, Sergio de Prado | MIT | https://github.com/Zal0/SushiNights | `5b3203e60e8815acf75e25c5ae369815da047c0f46b81ad3bcc8513ce7167e25` | Repo LICENSE MIT (c) 2021 Zalo (database: MIT, no platform field). |
| `gbh-unstoppable-knight` | Unstoppable Knight | Rafael Garcia (Rafagars) | MIT | https://github.com/Rafagars/Unstoppable-Knight-GB | `42637b68a3d2806062ba410eb848e7ff2c021829a0fb616d15e3b56643f241fc` | Repo LICENSE MIT (c) 2021 Rafael Garcia; music from GB Studio Community Assets (credited). Dual-mode. |
| `gbh-wyrmhole` | Wyrmhole | Quinn Painter (quinnp); music and SFX by Coffee Bat | MIT | https://github.com/QuinnPainter/Wyrmhole | `a5e07f89119ee9c93aa50ec1ff828b2441781fa9b1a137f03af7b6c53ce03252` | Repo LICENSE MIT (c) 2022 Quinn Painter; third-party assets listed in the README are credited in the guide. |
| `gbh-zypher` | Zypher | PixelPhobicGames | GPL-3.0 | https://github.com/PixelPhobicGames/Zypher | `1baa54454a61ebb0fef8b34dbd07785d556eb35f381c7b39d7d56f37b034ab81` | Repo LICENSE GPL-3.0; database license empty. Database pictures showing the "GAME BOY" cover wording are not used. |

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
| `keychan` (MIT) | README: the voice was generated with an AI model imitating a character from a third-party anime (third-party IP). |
| `mateusdigital_el-jamon-volador` (GPL-3.0) | Background tileset taken from bitnenfer/FlappyBoy, which has no license; Flappy Bird homage. |
| `quinevere__gigant-golf` (GPL-3.0, assets CC BY-SA) | Blank screen in the harness (no frame drawn in 1500 frames). |
| `patmorita__hermano-game-boy` (GPL-3.0 code) | Graphics and music CC BY-NC-ND (non-commercial, no derivatives). |
| `porklike-gb` (MIT code) | README: "All game content is owned by the original copyright holders" (Porklike by Krystman); only the code is free. |
| `dawn-will-come`, `dusky-dungeon`, `gb-snake-reini1305`, `gbstadium`, `joestrong__monster-within`, `rebound`, `trabant`, `zone-booth_pizza-palace` | Licensed (MIT / CC0 / GPL / CC BY) but CGB-only (0x143 = C0): Peanut-GB is DMG-only. |
| `adjustris` (CC0) | Falling-block game close to Tetris trade dress: skipped (third-party IP risk). |
| `dd-character-sheet-demade`, `videogamestorytime__froggykong`, `gbcspelunky` | Third-party IP (Dungeons & Dragons, Donkey Kong, Spelunky). |
| `sleepingpandagames__moki-the-monster-kid` (MPL-2.0) | Not in the accepted license set of this store pipeline. |
| `bubble-factory`, `flappy-boy`, `hitorigb`, `hover-defender`, `kupman`, `lazerpong`, `lumberjack`, `postie`, `pixelloren__third-grade-noir`, `retroreflector__banished`, `runtodatabay`, `snake-gb`, `snake-gbdk`, `speedp`, `sukus__towizz`, `super-princess-2092`, `zakari-games__frontiers-of-the-fallen` | Repository has no license file: not verifiable. |
| `evolandgb`, `timespacewarrior__temporal-light-jem-of-twilight` | Repository license not recognised (NOASSERTION): not verified. |
| `corrib75` (GPL-3.0), `exeman`, `snek-gbc` | Repository no longer exists: not verifiable. |
| Other GB / no-platform game entries with no license and no linked repository | Unknown license. |

Not considered: GBC-only entries (platform `GBC`; Peanut-GB is DMG-only) and entries of other types (demos, tools, music).
