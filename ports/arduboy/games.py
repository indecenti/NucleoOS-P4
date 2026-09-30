"""games.py — the Arduboy games packaged as NucleoOS apps (apps/ab-<slug>), with their metadata.

Selection rule: listed in eried/ArduboyCollection with an explicit OSI / CC0 / CC-BY(-SA) license in
game.ini AND a public source repository whose own LICENSE file confirms it. Each entry is pinned in
fetch.sh (commit + sha256). Fields:
    slug, title, repo (owner/name), path (sketch folder in the repo), author, license (SPDX),
    license_file (in the repo), source (URL shown to users), desc_en / desc_it (store text, from
    game.ini), controls_en / controls_it (GUIDE.md), script (harness input), icon_frame /
    shot_frames (harness presents used for the icon and the two store shots), notes_en / notes_it,
    patches {file: [(old, new)]} small build fixes, defines, cxxflags, skip (reason).
Part of ports/arduboy.
"""
import json
import os
import re

from games_meta import META

# Harness input when a game has none: wake up, start, then wander and press buttons.
DEFAULT_SCRIPT = ("40-44:a,80-84:a,130-134:a,180-184:a,230-236:a,260-420:right,300-304:a,340-344:b,"
                  "430-600:left,470-474:a,520-524:a,610-700:up,640-644:a,710-800:down,760-764:a,"
                  "820-900:right,850-854:a,880-884:b,910-1000:left,940-944:a,990-994:a,1010-1100:up,"
                  "1050-1054:a,1110-1190:right,1140-1144:a")

OBONO = "obono/ArduboyWorks"
OBONO_AUTHOR = "OBONO"
OBONO_NOTE_EN = "Part of OBONO's ArduboyWorks collection (MIT)."
OBONO_NOTE_IT = "Fa parte della raccolta ArduboyWorks di OBONO (MIT)."

GAMES = [
    # ---- first five (shim validation) ---------------------------------------------------------------
    dict(slug="crate-confusion", title="Crate Confusion", repo="phoboslab/arduboy-games", path="crate-confusion",
         author="Dominic Szablewski (phoboslab)", license="MIT", license_file="LICENSE",
         source="https://github.com/phoboslab/arduboy-games/tree/master/crate-confusion",
         desc_en="Crate Confusion, a small arcade game by phoboslab: fly your little ship through the warehouse, "
                 "collect fuel and don't hit the crates.",
         desc_it="Crate Confusion, piccolo gioco arcade di phoboslab: guida la navicella nel magazzino, "
                 "raccogli il carburante e non urtare le casse.",
         controls_en="Arrows move, A starts and confirms.",
         controls_it="Le frecce muovono, A avvia e conferma.",
         notes_en="The repository's LICENSE file is the MIT text (its copyright line names OBONO, apparently copied "
                  "from a template); the README states that all games are MIT-licensed.",
         notes_it="Il file LICENSE del repository è il testo MIT (la riga di copyright cita OBONO, probabilmente "
                  "copiata da un modello); il README dichiara che tutti i giochi sono sotto licenza MIT."),
    dict(slug="hopper", title="Hopper", repo=OBONO, path="hopper", author=OBONO_AUTHOR, license="MIT",
         license_file="LICENSE", source="https://github.com/obono/ArduboyWorks/tree/master/hopper",
         desc_en="Hopper by OBONO: jump on the panels and climb as high as you can. If you fall to the bottom, the game "
                 "is over.",
         desc_it="Hopper di OBONO: salta sui pannelli e sali il più in alto possibile. Se cadi in fondo, la partita "
                 "finisce.",
         controls_en="Left/right move, A jumps. On the title screen A starts.",
         controls_it="Sinistra/destra muovono, A salta. Nella schermata del titolo A avvia la partita.",
         notes_en=OBONO_NOTE_EN, notes_it=OBONO_NOTE_IT),
    dict(slug="samegame", title="SameGame", repo=OBONO, path="samegame", author=OBONO_AUTHOR, license="MIT",
         license_file="LICENSE", source="https://github.com/obono/ArduboyWorks/tree/master/samegame",
         desc_en="SameGame by OBONO, the famous tile-matching puzzle: remove groups of adjoining blocks of the same "
                 "type and clear the board.",
         desc_it="SameGame di OBONO, il celebre rompicapo: elimina i gruppi di blocchi uguali vicini e svuota il "
                 "tabellone.",
         patches={"title.cpp": [("static void drawText(const char *p, int lines);",
                                 "static void drawText(const char *p, int16_t lines);")]},
         controls_en="Arrows move the cursor, A removes the group under it, B undoes / goes back.",
         controls_it="Le frecce spostano il cursore, A elimina il gruppo indicato, B annulla / torna indietro.",
         notes_en=OBONO_NOTE_EN, notes_it=OBONO_NOTE_IT),
    dict(slug="reversi", title="Reversi", repo=OBONO, path="reversi", author=OBONO_AUTHOR, license="MIT",
         license_file="LICENSE", source="https://github.com/obono/ArduboyWorks/tree/master/reversi",
         desc_en="Reversi by OBONO: the classic board game against the computer or a friend.",
         desc_it="Reversi di OBONO: il classico gioco da tavolo contro il computer o un amico.",
         controls_en="Arrows move the cursor, A places a disc, B opens the menu.",
         controls_it="Le frecce spostano il cursore, A piazza una pedina, B apre il menu.",
         notes_en=OBONO_NOTE_EN, notes_it=OBONO_NOTE_IT),
    dict(slug="lasers", title="Lasers", repo=OBONO, path="lasers", author=OBONO_AUTHOR, license="MIT",
         license_file="LICENSE", source="https://github.com/obono/ArduboyWorks/tree/master/lasers",
         desc_en="Lasers by OBONO: switch your character's colour so that every ray that hits it has the same colour.",
         desc_it="Lasers di OBONO: cambia il colore del personaggio in modo che ogni raggio che lo colpisce sia "
                 "dello stesso colore.",
         patches={"common.cpp": [(r"re:asm volatile \(.*?\);",
                                  "for (int i = 0; i < 1024; i += 2) { sBuffer[i] = b1; sBuffer[i + 1] = b2; }")]},
         blend=True,
         controls_en="Arrows move, A/B switch the colour.",
         controls_it="Le frecce muovono, A/B cambiano colore.",
         notes_en=OBONO_NOTE_EN, notes_it=OBONO_NOTE_IT),
]


# ---- batch from ArduboyCollection (generated, then reviewed) ------------------------------------
BATCH = [
    dict(slug='beam-em-up', title="Beam 'Em Up", repo='unwiredben/arduboy-beamemup', path='BeamEmUp', author='unwiredben', license='Apache-2.0', license_file='LICENSE', source='https://github.com/unwiredben/arduboy-beamemup', desc_en="Pilot your squid ship over a pastoral landscape, trying to bring all the cows together while dodging meteors. It's like herding cats, but with cows.", desc_it=None),
    dict(slug='choplifter', title='Choplifter', repo='ArduboyCollection/Choplifter', path='', author='Simon Holmes (filmote)', license='BSD-3-Clause', license_file='LICENSE', source='https://github.com/ArduboyCollection/Choplifter', desc_en='The classic Choplifter game for the Arduboy!', desc_it=None),
    dict(slug='glove', title='Glove', repo='ArduboyCollection/glove', path='', author='fuopy', license='MIT', license_file='LICENSE', source='https://github.com/ArduboyCollection/glove', desc_en='Glove is an action-adventure game created in the style of the classic Gauntlet series. Navigate from room to room, blasting bad guys and collecting treasure along the way.', desc_it=None),
    dict(slug='helii', title='Helii', repo='ArduboyCollection/Helii-Arduboy', path='', author='BHSPitMonkey', license='MIT', license_file='LICENSE', source='https://github.com/ArduboyCollection/Helii-Arduboy', desc_en='A side-scrolling tunnel game', desc_it=None),
    dict(slug='poitto', title='Poitto', repo='ArduboyCollection/poitto', path='poitto', author='inajob', license='MIT', license_file='LICENSE.txt', source='https://github.com/ArduboyCollection/poitto', desc_en='The rules are simple: You try to go to the door. Avoid enemies, use switch blocks, spring blocks, and so on', desc_it=None),
    dict(slug='rooftop-rescue', title='Rooftop Rescue', repo='BertVeer/Rooftop', path='rooftop', author="Bert van't Veer", license='MIT', license_file='LICENSE', source='https://github.com/BertVeer/Rooftop', desc_en='Rescue the civilians who are trapped on the roof of imploding buildings.', desc_it=None),
    dict(slug='to', title='To', repo='ArduboyCollection/to', path='', author='waday', license='MIT', license_file='LICENSE', source='https://github.com/ArduboyCollection/to', desc_en='You are the developer of the door with the teleport function. But just before completion, someone is trying to destroy the door. There seems to be no choice but to distort the space with the power of the door and to prevent destruction.', desc_it=None),
    dict(slug='trench-run', title='Space Battle - Trench Run', repo='lscardinali/TrenchRun-Arduboy', path='', author='Lucas Cardinali (lscardinali)', license='MIT', license_file='LICENSE', source='https://github.com/lscardinali/TrenchRun-Arduboy', desc_en='Trench Run is an Arcade game where you play as a rebel pilot on the Doom Sphere assault. You have to fight against Imperial fighters to reach the Exhaustion Port and deliver the shot that will ultimately destroy the battle station.', desc_it=None),
    dict(slug='nineteen43', title='1943', repo='ArduboyCollection/Nineteen43', path='', author='Simon Holmes (filmote)', license='MIT', license_file='LICENSE', source='https://github.com/ArduboyCollection/Nineteen43', desc_en='A version of the classic WWII game 1943: The Battle of Midway. Destroy the enemy Zeroes and Bombers. Zeros with solid wings will die on one shot, others will take multiple hits before they die. Watch your fuel, your health and ammunition - replenish these by running over the power ups!', desc_it=None),
    dict(slug='nineteen44', title='1944', repo='Press-Play-On-Tape/Nineteen44', path='Nineteen44', author='Press Play On Tape (Filmote and Vampirics)', license='MIT', license_file='LICENSE', source='https://github.com/Press-Play-On-Tape/Nineteen44', desc_en='A version of the classic WWII game 1943: The Battle of Midway. Destroy the enemy Zeroes and Bombers. Zeros with solid wings will die on one shot, others will take multiple hits before they die. Watch your fuel, your health and ammunition - replenish these by running over the power ups!', desc_it=None),
    dict(slug='ardu-buggy', title='Ardu Buggy', repo='ArduboyCollection/ArduBuggy', path='', author='NonoNano', license='Unlicense', license_file='LICENSE.txt', source='https://github.com/ArduboyCollection/ArduBuggy', desc_en='Ardu Buggy, inspired by Moon Patrol.', desc_it=None),
    dict(slug='ardu-man', title='Ardu-man', repo='ArduboyCollection/arduman_arduboylib11', path='', author='Seth Robinson', license='MIT', license_file='LICENSE.md', source='https://github.com/ArduboyCollection/arduman_arduboylib11', desc_en="Re-live the nostalgic glory of games like Pac-Man and Lock 'n' Chase (with a few less pixels...) right on your Arduboy!", desc_it=None),
    dict(slug='back-to-the-jungle', title='Back to the Jungle', repo='ArduboyCollection/ArduboyBackToTheJungle', path='back2jungle', author='eried', license='BSD-3-Clause', license_file='LICENSE', source='https://github.com/ArduboyCollection/ArduboyBackToTheJungle/', desc_en='The Zoo had to close, what can the animals do? Entry game for the first Arduboy Game Jam.', desc_it=None),
    dict(slug='boris-goes-skiing', title='Boris goes skiing', repo='ArduboyCollection/boris-goes-skiing', path='', author='Tom Sparrow (sones857)', license='MIT', license_file='LICENSE', source='https://github.com/ArduboyCollection/boris-goes-skiing', desc_en='A clone of the ZX Spectrum game Horace goes skiing for the Arduboy', desc_it=None),
    dict(slug='chri-bocchi-cat', title='Chri-Bocchi Cat', repo='obono/ArduboyWorks', path='chribocchi', author='obono', license='MIT', license_file='LICENSE', source='https://github.com/obono/ArduboyWorks/tree/master/chribocchi', desc_en='Move the cat and bounce gift boxes for 2 minutes.', desc_it=None),
    dict(slug='diamonds', title='Diamonds', repo='ArduboyCollection/diamonds', path='src', author='RedBug', license='MIT', license_file='LICENSE', source='https://github.com/ArduboyCollection/diamonds', desc_en='Diamonds is a mix between a breakout game and a puzzle game. Original game was released on HP48 more than 20 years ago (yes.. Already 20 years...).', desc_it=None),
    dict(slug='evade', title='Evade', repo='ArduboyCollection/evade-arduboy-game', path='arduboy-game', author='Modus Create', license='MIT', license_file='LICENSE.txt', source='https://github.com/ArduboyCollection/evade-arduboy-game', desc_en='A Space Shooter Game for Arduboy by Modus Create', desc_it=None),
    dict(slug='hollow-seeker', title='Hollow Seeker', repo='obono/ArduboyWorks', path='hollow', author='OBONO', license='MIT', license_file='LICENSE', source='https://github.com/obono/ArduboyWorks/tree/master/hollow', desc_en='Go forward in right direction. Seek a hollow as refuge not to be crushed.', desc_it=None),
    dict(slug='kong', title='Kong', repo='Press-Play-On-Tape/Kong', path='Kong', author='Press Play On Tape (Filmote and Vampirics)', license='BSD-3-Clause', license_file='LICENSE', source='https://github.com/Press-Play-On-Tape/Kong', desc_en='A remake of the classic Game and Watch game Donkey Kong', desc_it=None),
    dict(slug='kong-ii', title='Kong II', repo='Press-Play-On-Tape/Kong-II', path='Kong', author='Press Play on Tapes (Vampirics and Filmote)', license='BSD-3-Clause', license_file='LICENSE', source='https://github.com/Press-Play-On-Tape/Kong-II', desc_en='A remake of the classic Game and Watch game Donkey Kong II', desc_it=None),
    dict(slug='loverush', title='LoveRush', repo='vampirics/LoveRush', path='LoveRush', author='Stephane Cote (Vampirics)', license='MIT', license_file='LICENSE', source='https://github.com/vampirics/LoveRush', desc_en="A simple infinite shoot em up where you need to gather hearts to build-up your shields and shoot those angry faces rushing on you. You get an extra shield for every 15 hearts you gather. Speed goes up after a certain amount of points you make. What's your high score?", desc_it=None),
    dict(slug='pong', title='Pong', repo='ArduboyCollection/Pong-Arduboy', path='pong', author='Taylor Hansen (CrazyGuy108)', license='MIT', license_file='LICENSE', source='https://github.com/ArduboyCollection/Pong-Arduboy', desc_en='A Pong clone for the Arduboy', desc_it=None),
    dict(slug='poop-panic', title='Poop Panic!', repo='ArduboyCollection/poop-panic', path='poop-panic', author='inajob', license='MIT', license_file='LICENSE', source='https://github.com/ArduboyCollection/poop-panic', desc_en='You keep the zoo clean!', desc_it=None),
    dict(slug='ravine-despoiler', title='Ravine Despoiler', repo='unwiredben/arduboy-ravine-despoiler', path='RavineDespoiler', author='Ben Combee (@unwiredben)', license='Apache-2.0', license_file='LICENSE', source='https://github.com/unwiredben/arduboy-ravine-despoiler', desc_en="A loose interpretation of Atari's arcade classic Canyon Bomber", desc_it=None),
    dict(slug='space-cab', title='Space Cab', repo='vampirics/SpaceCab', path='SpaceCab/SpaceCab', author='Stephane C (vampirics) and Simon Holmes (filmote)', license='BSD-3-Clause', license_file='LICENSE', source='https://github.com/vampirics/SpaceCab', desc_en='An arcade game inspired a lot by Space Taxi that was released on the Commodore 64.', desc_it=None),
    dict(slug='social-distance', title='The Social Distance Game', repo='msanatan/TheSocialDistanceGame', path='', author='msanatan', license='MIT', license_file='LICENSE', source='https://github.com/msanatan/TheSocialDistanceGame', desc_en="Keep out of people's way as long as you can!", desc_it=None),
    dict(slug='arduboy-life', title='ArduboyLife', repo='MLXXXp/ArduboyLife', path='ArduboyLife', author='Scott Allen (MLXXXp)', license='MIT', license_file='LICENSE.txt', source='https://github.com/MLXXXp/ArduboyLife', desc_en="Conway's Life, a cellular automaton, for the Arduboy", desc_it=None),
    dict(slug='castleboy', title='CastleBoy', repo='ArduboyCollection/CastleBoy', path='CastleBoy', author='jlauener', license='MIT', license_file='LICENSE', source='https://github.com/ArduboyCollection/CastleBoy', desc_en='Castlevania-like game for the Arduboy.', desc_it=None),
    dict(slug='the-bounce', title='The Bounce', repo='ArduboyCollection/TheBounceArduboy', path='TheBounceArduboy', author='Joshimuz', license='MIT', license_file='LICENSE.txt', source='https://github.com/ArduboyCollection/TheBounceArduboy', desc_en="The Bounce is a bouncy ball physics platformer game. Simple to understand yet incredibly hard to master, what seems easy at first glance is actually pretty rage inducing when you get to the later levels. I'm not much of an artist so I focused on simple design, which allows for easily customisable levels (maybe a level editor someday) and awesome gameplay.", desc_it=None),
    dict(slug='ardecipher', title='ARDecipher', repo='databhor/ARDecipher', path='', author='databhor', license='MIT', license_file='LICENSE', source='https://github.com/databhor/ARDecipher', desc_en='Similar to the terminal hack in Fallout 4. Find the password within 5 retries. Number of matched characters will be shown if you choose a non-password string.', desc_it=None),
    dict(slug='ardulo', title='ArduLO', repo='jonthysell/ArduLO', path='src/ArduLO', author='Jon Thysell', license='MIT', license_file='LICENSE.md', source='https://github.com/jonthysell/ArduLO', desc_en='ArduLO is a clone of the puzzle game Lights Out for the Arduboy.', desc_it=None),
    dict(slug='box-stacker', title='Box Stacker', repo='ArduboyCollection/BlockStacker', path='', author='dragula96', license='BSD-3-Clause', license_file='LICENSE', source='https://github.com/ArduboyCollection/BlockStacker', desc_en='Stacker clone like the game at the arcade that gives you a chance to win a prize, however in this one the only thing you win is satisfaction!.', desc_it=None),
    dict(slug='chie-magari-ita', title='', repo='obono/ArduboyWorks', path='chiemagari', author='obono', license='MIT', license_file='LICENSE', source='https://github.com/obono/ArduboyWorks/tree/master/chiemagari', desc_en='There are 10 different pieces. You must place all of them into the frame without overlapping.', desc_it=None),
    dict(slug='dominoes', title="Dominoes 'All Fives'", repo='Press-Play-On-Tape/Dominoes', path='', author='Simon Holmes (filmote) & Stephance C (vampirics)', license='BSD-3-Clause', license_file='LICENSE', source='https://github.com/Press-Play-On-Tape/Dominoes', desc_en='All Fives is a popular variant of Dominoes where players the goal of the game is not just to go out, but to make the open ends of the layout add up to 5 (or a multiple of five).', desc_it=None),
    dict(slug='hangman', title='Hangman!', repo='serisman/arduboy-hangman', path='Hangman', author='serisman', license='MIT', license_file='LICENSE', source='https://github.com/serisman/arduboy-hangman', desc_en="Hangman!  Try to guess a word by suggesting letters that may be in the word.  You win if you can guess the word with less than 6 failed letter guesses, otherwise you're dead.", desc_it=None),
    dict(slug='knight-move', title='Knight Move', repo='obono/ArduboyWorks', path='knightmove', author='obono', license='MIT', license_file='LICENSE', source='https://github.com/obono/ArduboyWorks/tree/master/knightmove', desc_en='This is an action-puzzle game. You control a knight piece of the chess which can move in an L shape on the 8×4 board.', desc_it=None),
    dict(slug='lite-out', title='Lite Out', repo='ArduboyCollection/ArduboyLiteOut', path='', author='K. M. Kroski', license='Unlicense', license_file='LICENSE.md', source='https://github.com/ArduboyCollection/ArduboyLiteOut', desc_en="Lite/Out is a game where you have to turn off all of the lights, but every time you change a light, the surrounding lights change. The game consists of a 5-by-5 grid where some of the blocks are lit up. You'll complete the level when all of the blocks are not lit.", desc_it=None),
    dict(slug='minesweeper', title='Minesweeper', repo='Pharap/Minesweeper', path='Minesweeper', author='Pharap', license='Apache-2.0', license_file='LICENSE', source='https://github.com/Pharap/Minesweeper/tree/v2.0.0', desc_en='Simple Minesweeper game, available in multiple languages.', desc_it=None),
    dict(slug='pipeboy', title='Pipe Boy', repo='Glitsch3n/arduboy-game-collection', path='PipeBoy', author='Glitschen', license='MIT', license_file='LICENSE', source='https://github.com/Glitsch3n/arduboy-game-collection/tree/main/PipeBoy', desc_en='PipeBoy is a fast-paced puzzle game for Arduboy, inspired by Pipe Mania. Build an efficient pipe network before the liquid starts flowing! Play solo or cooperate with a friend in the 2-player mode. Think fast, optimize your connections, and become the ultimate pipe master!', desc_it=None),
    dict(slug='pipes', title='Pipes', repo='ArduboyCollection/LayingPipe', path='', author='Filmote', license='BSD-3-Clause', license_file='LICENSE', source='https://github.com/ArduboyCollection/LayingPipe', desc_en='A version of the classic pipes game where you must lay pip between each pair of nodes without the pipes crossing each other. This version has puzzles ranging from 5 x 5 to 9 x 9.', desc_it=None),
    dict(slug='ponghauki', title='PongHauKi', repo='databhor/PongHauKi', path='', author='databhor', license='MIT', license_file='LICENSE', source='https://github.com/databhor/PongHauKi', desc_en='A simple chess known in China and a few other Asian countries. Move 1 of 2 pieces by turn. Who gets blocked loses game.', desc_it=None),
    dict(slug='psi-colo', title='', repo='obono/ArduboyWorks', path='psicolo', author='obono', license='MIT', license_file='LICENSE', source='https://github.com/obono/ArduboyWorks/tree/master/psicolo', desc_en="This is a puzzle game with dice on the 5x7 grid field. Creating a group of adjacent dice with identical pips - the size of which must be at least the number of pips - causes those dice to slowly sink into the field before disappearing. Chain reactions are possible by adding additional dice to a sinking set. Exceptionally, to get rid of 'one pip' die, you have to move a 'one pip' die next to some disappearing dice. Then all of the 'one pip' dice will start to disappear.", desc_it=None),
    dict(slug='quarto', title='Quarto!', repo='obono/ArduboyWorks', path='quarto', author='obono', license='MIT', license_file='LICENSE', source='https://github.com/obono/ArduboyWorks/tree/master/quarto', desc_en='Abstract strategy board game', desc_it=None),
    dict(slug='ring-puzzle', title='Ring Puzzle', repo='ArduboyCollection/ring_puzzle_game', path='ring_puzzle_game', author='Skuratovich Sergey', license='MIT', license_file='LICENSE', source='https://github.com/ArduboyCollection/ring_puzzle_game', desc_en='Puzzle game using toroidal topology.', desc_it=None),
    dict(slug='stairs-sweep', title='Stairs Sweep', repo='obono/ArduboyWorks', path='stairssweep', author='obono', license='MIT', license_file='LICENSE', source='https://github.com/obono/ArduboyWorks/tree/master/stairssweep', desc_en='Falling block puzzle game, something like Pac-Attack', desc_it=None),
    dict(slug='tictaccurly', title='TicTacCurly', repo='ArduboyCollection/TicTacCurly', path='', author='curly', license='MIT', license_file='LICENSE', source='https://github.com/ArduboyCollection/TicTacCurly', desc_en='tic tac toe game for arduboy 1-2 player', desc_it=None),
    dict(slug='tres', title='Tres', repo='ArduboyCollection/tres', path='tres', author='Casey Gold (cajogold)', license='MIT', license_file='LICENSE', source='https://github.com/ArduboyCollection/tres', desc_en='Clone/Port/Rip-off of the game Threes! for mobile phones', desc_it=None),
    dict(slug='waternet', title='Waternet', repo='joyrider3774/waternet_arduboy', path='source/waternet', author='Willems Davy aka Joyrider3774', license='MIT', license_file='LICENSE', source='https://github.com/joyrider3774/waternet_arduboy', desc_en="A puzzle game based on the Net and Netslide games from Simon Tatham's puzzle collection.", desc_it=None),
    dict(slug='ard-drivin', title='Ard-Drivin', repo='ArduboyCollection/ard-drivin', path='', author='Rem and LP', license='MIT', license_file='LICENSE.txt', source='https://github.com/ArduboyCollection/ard-drivin', desc_en='a little racing game for the good old arduboy.', desc_it=None),
    dict(slug='randocity', title='Randocity', repo='pmwasson/Randocity', path='', author='pmwasson', license='MIT', license_file='LICENSE', source='https://github.com/pmwasson/Randocity', desc_en='Arduboy large open-world procedurally generated city motorcycle game with 3 play modes', desc_it=None),
    dict(slug='dark-and-under', title='Dark & Under', repo='ArduboyCollection/Dark-And-Under', path='', author='Garage Collective (Cyril Guichard (Luxregina), Simon Holmes (Filmote), Pharap)', license='BSD-3-Clause', license_file='LICENSE', source='https://github.com/ArduboyCollection/Dark-And-Under', desc_en="Do you have what it takes to survive the depths of the Undermountain? Alone?  If so, then pack your gear, strap on your shield, explore labyrinthine corridors and battle unimaginable foes to steal the greedy Dragon's treasure.", desc_it=None),
    dict(slug='hello-commander', title='Hello, Commander', repo='ArduboyCollection/HelloCommander', path='', author='Felipe Manga (FManga)', license='MIT', license_file='LICENSE', source='https://github.com/ArduboyCollection/HelloCommander', desc_en='Turn-Based Strategy game for the Arduboy.', desc_it=None),
    dict(slug='star-honor', title='Star Honor', repo='ArduboyCollection/StarHonor', path='', author='Wenceslao Villanueva Jr', license='MIT', license_file='LICENSE', source='https://github.com/ArduboyCollection/StarHonor', desc_en="Star Honor is a Roguelike space adventure where you are field promoted into the Captain's chair of the USS Arduino. Your home world has become the victim of the a biological weapon deployed by enemies. You posses the only hope for your world, a cure for the phage. With a skeleton crew, a limping vessel and time running out, you must travel world to world, seeing supplies, upgrades and doing battle to make it home and save your planet.", desc_it=None),
    dict(slug='ardubullets', title='ARDUBULLETs', repo='obono/ArduboyWorks', path='ardubullets', author='obono', license='MIT', license_file='LICENSE', source='https://github.com/obono/ArduboyWorks/tree/master/ardubullets', desc_en='Avoid voluminous bullets and defeat enemy squadron in one minute.', desc_it=None),
    dict(slug='cosmicpods', title='CosmicPods', repo='ArduboyCollection/CosmicPods', path='', author='cubic9com', license='BSD-3-Clause', license_file='LICENSE', source='https://github.com/ArduboyCollection/CosmicPods', desc_en="CosmicPods is a tiny shoot-'em-up game for Arduboy. As cosmic squid, shoot octopuses up!", desc_it=None),
    dict(slug='galaxion', title='Galaxion', repo='ArduboyCollection/galaxion', path='', author='tako2', license='MIT', license_file='LICENSE', source='https://github.com/ArduboyCollection/galaxion', desc_en='Shooting game for Arduboy. Mission: Destroy aliens', desc_it=None),
    dict(slug='humanity-revenge', title='Humanity Revenge DC', repo='ArduboyCollection/Humanity_Revenge_DC', path='', author='giangregorioc', license='MIT', license_file='LICENSE', source='https://github.com/ArduboyCollection/Humanity_Revenge_DC', desc_en="It's a shoot 'em up quite hard, with 3 ship to choose from (different speed and weapons), 5 enemies with different characteristics, three big bosses, bomb and power-up, highscore and so on.", desc_it=None),
    dict(slug='l4arduboy', title='l4arduboy', repo='ArduboyCollection/i4arduboy', path='', author='Amamoriya Yomogimaru', license='MIT', license_file='LICENSE', source='https://github.com/ArduboyCollection/i4arduboy', desc_en='i4arduboy is a horizontal shmup. Control a submarine and snipe far enemies on off-screen with torpedos! This is a porting from our windows work i-20000.', desc_it=None),
    dict(slug='night-raid', title='Night Raid', repo='ArduboyCollection/night-raid', path='', author='Evan Barger', license='MIT', license_file='LICENSE', source='https://github.com/ArduboyCollection/night-raid', desc_en='a Missile Command inspired game for Arduboy', desc_it=None),
    dict(slug='omega-chase', title='Omega Chase', repo='Karl-Williams/OmegaChase', path='', author='Karl P. Williams', license='MIT', license_file='LICENSE', source='https://github.com/Karl-Williams/OmegaChase', desc_en='Omega Chase is an Arduboy port of the classic arcade style shooter game Omega Race. In this game, you control a spaceship and must navigate through an arena, shooting down enemy ships and avoiding obstacles.', desc_it=None),
    dict(slug='stellar-impact', title='Stellar Impact', repo='ArduboyCollection/Stellar_Impact', path='', author='Nick Allen (gnargle)', license='MIT', license_file='License.txt', source='https://github.com/ArduboyCollection/Stellar_Impact', desc_en="It's a simple shmup, but it has lots of stuff I think is pretty neat, like procgen starfields, a bomb that freezes time, a nice title screen, eeprom saving and more.", desc_it=None),
]
GAMES += BATCH

def full(g):
    """Entry with defaults filled in."""
    g = dict(g)
    g.update(EXTRA.get(g["slug"], {}))
    g.update(META.get(g["slug"], {}))
    g.setdefault("path", "")
    g.setdefault("menu_title", g["title"].upper())
    g.setdefault("icon_frame", 300)
    g.setdefault("shot_frames", [300, 900])
    g.setdefault("controls_en", "The D-pad moves, A and B act as on the Arduboy (the game's own screens say which does what).")
    g.setdefault("controls_it", "La croce direzionale muove, A e B funzionano come sull'Arduboy (le schermate del gioco indicano cosa fanno).")
    if not g.get("desc_it"):
        g["desc_it"] = g["desc_en"]
    return g


def commit_of(slug):
    s = open(os.path.join(os.path.dirname(os.path.abspath(__file__)), "fetch.sh"), encoding="utf-8").read()
    m = re.search(r"^game " + re.escape(slug) + r" \S+ ([0-9a-f]{40})", s, re.M)
    return m.group(1) if m else "?" * 40


def write_docs(root, here):
    """GAMES.md (built + skipped) and catalog_entries.json (store overlay) from this file."""
    built = [full(g) for g in GAMES if not full(g).get("skip")]
    skipped = [full(g) for g in GAMES if full(g).get("skip")] + SKIPPED
    cat = {}
    for g in built:
        cat["ab-" + g["slug"]] = {"category": "games", "featured": False, "rating": 4.5, "regions": ["*"],
                                  "names": {"en": g["title"], "it": g.get("title_it", g["title"])},
                                  "descriptions": {"en": g["desc_en"], "it": g["desc_it"]}}
    with open(os.path.join(here, "catalog_entries.json"), "w", encoding="utf-8") as fh:
        json.dump(cat, fh, indent=2, ensure_ascii=False)
        fh.write("\n")
    lines = ["# Arduboy games on NucleoOS", "",
             "Open-source Arduboy games rebuilt for NucleoOS with the Arduboy2-compatible shim in this folder",
             "(`shim/`, `patch_libs.py`). Candidates come from "
             "[eried/ArduboyCollection](https://github.com/eried/ArduboyCollection) (`game.ini` license + "
             "`source.url`); a game is included only if the license in `game.ini` is OSI / CC0 / CC-BY(-SA)",
             "and the source repository's own LICENSE file confirms it. Every repository is pinned in `fetch.sh`",
             "(commit + sha256 of the tarball). Generated by `abtool.py docs` from `games.py`.", "",
             f"## Built ({len(built)})", "",
             "| App | Title | Author | License | Source @ commit | Notes |", "|---|---|---|---|---|---|"]
    for g in built:
        c = commit_of(g["slug"])
        lines.append(f"| ab-{g['slug']} | {g['title']} | {g['author']} | {g['license']} (`{g['license_file']}`) | "
                     f"[{g['repo']}]({g['source']}) @ `{c[:10]}` | {g.get('notes_en', '')} |")
    lines += ["", f"## Skipped ({len(skipped)})", "", "| Title | Source | Reason |", "|---|---|---|"]
    for g in skipped:
        lines.append(f"| {g['title']} | {g.get('source', g.get('repo', ''))} | {g.get('skip', g.get('reason', ''))} |")
    lines.append("")
    open(os.path.join(here, "GAMES.md"), "w", encoding="utf-8").write("\n".join(lines))


# Candidates examined and not built (not in GAMES): title, source, reason.
_NOLIC = "no LICENSE file in the source repository (game.ini license cannot be verified)"
SKIPPED = [
    dict(title="Bounce", source="https://github.com/ArduboyCollection/Bounce", reason=_NOLIC),
    dict(title="DanceRow", source="https://github.com/ArduboyCollection/DanceRow", reason=_NOLIC),
    dict(title="Millipe", source="https://github.com/ArduboyCollection/Millipe", reason=_NOLIC),
    dict(title="GamesNGoblins", source="https://github.com/ImMrShrike/gng-arduboy-game", reason=_NOLIC),
    dict(title="YouWontSurvive", source="https://github.com/ArduboyCollection/YouWontSurvive", reason=_NOLIC),
    dict(title="Space Quarth", source="https://github.com/ArduboyCollection/SpaceQuarth", reason=_NOLIC),
    dict(title="Flappy Ball", source="https://github.com/ArduboyCollection/FlappyBall", reason=_NOLIC),
    dict(title="Pocket Othello", source="https://github.com/ArduboyCollection/pocket_othello", reason=_NOLIC),
    dict(title="Kunenhrayenhnenh", source="https://github.com/TylerBWright/Kunenhrayenhnenh", reason=_NOLIC),
    dict(title="PAC-Tastic", source="https://github.com/dragula96/pactastic", reason=_NOLIC),
    dict(title="Evade 2", source="https://github.com/ArduboyCollection/evade2",
         reason="game.ini says MIT, the repository carries GPL-2 files (bundled cores): not clear"),
    dict(title="Karateka", source="https://github.com/ArduboyCollection/Karateka",
         reason="ATMlib music engine (timer interrupts, AVR assembly); also a commercial game's name"),
    dict(title="Harambe's Revenge", source="https://github.com/ArduboyCollection/Harambe-s-Revenge",
         reason="ArdVoice speech library (AVR-specific)"),
    dict(title="Santa's Happy Little Packer", source="https://github.com/ArduboyCollection/ArduboyXmasGame",
         reason="ArdVoice speech library (AVR-specific)"),
    dict(title="Solitaire", source="https://github.com/ArduboyCollection/gamebuino-solitaire",
         reason="Gamebuino library port (no Gamebuino compatibility layer)"),
    dict(title="Kung Fu Escape / Pyoro! / Ardu-Racer", source="(several)",
         reason="ArdBitmap library not packaged (Pyoro! is also GPL, Ardu-Racer targets SDL/ESP)"),
    dict(title="Tiny Dungeon, ArduTosh, MicroCity, Little Rook Chess", source="(several)",
         reason="other hardware (ATtiny/SDL/U8glib) or multi-platform builds"),
    dict(title="GPL/LGPL games (Quadrastic, SFCave, Blocks, Under the Tower, Joustish, ...)", source="(several)",
         reason="not attempted in this batch: permissive licenses first (they qualify; see abtool.py pin + games.py)"),
    dict(title="Applications (gamepad, keyboard, light meter, serial terminal, ...)", source="(several)",
         reason="not games, or need USB/serial/sensor hardware"),
]

# Per-game additions to the generated BATCH entries (build fixes, harness input, store text).
EXTRA = {
    "pipeboy": dict(patches={"GameManager.cpp": [(r"re:\b(\w*[mM]enu)->render\(this\)",
                                                  r"\1->render(const_cast<GameManager *>(this))"),
                                                 (r"re:render\(&render", "render(&GameManager::render")]}),
    "beam-em-up": dict(patches={"Game.h": [("constexpr Number RestitutionThreshold", "const Number RestitutionThreshold")]}),
    "nineteen44": dict(drop_files=["src/Utils/wiring.c"]),   # a copy of the Arduino core's AVR timer code
    "stellar-impact": dict(cxxflags=["-O0"]),   # clang -O2 turns some undefined behaviour into a trap
    # const member function calling non-const helpers (avr-gcc -fpermissive let it through)
    "to": dict(patches={"actor.cpp": [("  dron(context);\n  if(isVisible()) {",
                                       "  const_cast<Player *>(this)->dron(context);\n"
                                       "  if(const_cast<Player *>(this)->isVisible()) {")]}),
    # reinterpret_cast in a constexpr (a gcc extension): an inline static const pointer instead
    "minesweeper": dict(cxxflags=["-std=gnu++17"], patches={"SaveSystem.h": [
        (r"re:static constexpr (\w+) \* (\w+) = reinterpret_cast", r"static inline \1 * const \2 = reinterpret_cast")]}),
    "tictaccurly": dict(drop_files=["v1p1.ino"]),   # the previous version of the sketch, marked OLD VERSION
    # OBONO's own timer-driven Playtune variant -> the NucleoOS score player (same score format)
    "ardubullets": dict(replace_files={"MyArduboyPlaytune.cpp": """// MyArduboyPlaytune.cpp for NucleoOS (ports/arduboy, BSD-3-Clause): the audio half of
// OBONO's MyArduboy2 routed to the shim's Playtune score player (nvab_score), which also knows
// this score dialect (0xD0 repeat mark, 0xEn repeat count, per-score pitch).
#include "MyArduboy2.h"
#include "nvab_score.h"

void MyArduboy2::initAudio(uint8_t chans) {
    nvab::score_close_channels();
    nvab::score_set_enable(audio.enabled);
    for (uint8_t c = 0; c < chans && c < 2; c++) nvab::score_init_channel();
}
void MyArduboy2::closeAudio(void) { nvab::score_close_channels(); }
bool MyArduboy2::isAudioEnabled(void) { return audio.enabled(); }
void MyArduboy2::setAudioEnabled(bool on) { (on) ? audio.on() : audio.off(); }
void MyArduboy2::toggleAudioEnabled(void) { audio.toggle(); }
void MyArduboy2::saveAudioOnOff(void) { audio.saveOnOff(); }
void MyArduboy2::playTone(uint16_t frequency, uint16_t duration, uint8_t, uint8_t) {
    nvab::score_tone(frequency, duration);
}
void MyArduboy2::playScore(const byte *score, uint8_t, int8_t pitch) { nvab::score_play(score, pitch); }
void MyArduboy2::stopScore(void) { nvab::score_stop(); }
"""}),
}
