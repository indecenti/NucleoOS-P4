"""games_meta.py — store text (en/it), controls and harness frames of the Arduboy apps; merged into
the games.py entries by games.full(). icon_frame = the harness present showing the game's own title
screen (the icon is cut from it), shot_frames = the two store screenshots. Part of ports/arduboy.
"""


def M(icon, shots, en=None, it=None, c_en=None, c_it=None, **kw):
    d = dict(icon_frame=icon, shot_frames=shots, **kw)
    if en:
        d["desc_en"] = en
    if it:
        d["desc_it"] = it
    if c_en:
        d["controls_en"] = c_en
    if c_it:
        d["controls_it"] = c_it
    return d


# whole title frame as the icon (the largest blob is a menu or a detail)
ICON_FULL = {"arduboy-life", "evade", "hangman", "knight-move", "night-raid", "poitto", "psi-colo", "to", "ardu-man",
             "hopper", "hollow-seeker"}

OBONO_EN = "Part of OBONO's ArduboyWorks collection (MIT)."
OBONO_IT = "Fa parte della raccolta ArduboyWorks di OBONO (MIT)."
TM = "remake that uses the name of a commercial game (trademark): not published"

META = {
    "crate-confusion": M(20, [20, 100],
        "Crate Confusion by phoboslab: a simple arcade game. Collect the fuel and don't hit the crates.",
        "Crate Confusion di phoboslab: un semplice gioco arcade. Raccogli il carburante e non urtare le casse."),
    "hopper": M(150, [150, 450]),
    "samegame": M(150, [150, 600]),
    "reversi": M(150, [150, 900]),
    "lasers": M(600, [600, 900]),
    "beam-em-up": M(60, [60, 600],
        "Beam 'Em Up by Ben Combee: pilot a squid-shaped ship over the countryside and use the tractor beam to bring "
        "all the cows together while dodging meteors. Like herding cats, but with cows.",
        "Beam 'Em Up di Ben Combee: pilota una navicella a forma di calamaro sopra la campagna e usa il raggio traente "
        "per radunare tutte le mucche schivando i meteoriti.",
        "Arrows fly the ship, A works the tractor beam and starts the game.",
        "Le frecce guidano la navicella, A aziona il raggio traente e avvia la partita."),
    "glove": M(20, [20, 900],
        "Glove by fuopy, an action-adventure in the style of the classic dungeon crawlers: go from room to room, blast "
        "the bad guys and their spawners, find keys and collect treasure. Progress is saved.",
        "Glove di fuopy, un'avventura d'azione nello stile dei classici dungeon crawler: passa da una stanza all'altra, "
        "abbatti i nemici e i loro generatori, trova le chiavi e raccogli i tesori. I progressi vengono salvati.",
        "Arrows move, A and B fire and confirm (the game's menus show which).",
        "Le frecce muovono, A e B sparano e confermano (i menu del gioco indicano quale)."),
    "helii": M(20, [20, 600],
        "Helii by BHSPitMonkey: a side-scrolling tunnel game. Keep the helicopter away from the cave walls as long as "
        "you can.",
        "Helii di BHSPitMonkey: un gioco a scorrimento in una galleria. Tieni l'elicottero lontano dalle pareti della "
        "caverna il più a lungo possibile.",
        "Hold A to climb, release it to sink.",
        "Tieni premuto A per salire, rilascialo per scendere."),
    "poitto": M(20, [20, 250],
        "Poitto by inajob: reach the door of each stage, avoiding the enemies and using switch blocks, spring blocks "
        "and more.",
        "Poitto di inajob: raggiungi la porta di ogni livello evitando i nemici e usando blocchi interruttore, molle e "
        "altro ancora."),
    "rooftop-rescue": M(150, [150, 1199],
        "Rooftop Rescue by Bert van't Veer: fly your helicopter over the city and rescue the people trapped on the "
        "roofs of collapsing buildings.",
        "Rooftop Rescue di Bert van't Veer: pilota l'elicottero sopra la città e salva le persone intrappolate sui "
        "tetti dei palazzi che crollano."),
    "to": M(20, [20, 300],
        "To by waday: you built a door that can teleport, and someone wants to destroy it. Bend space with the door's "
        "power to protect it.",
        "To di waday: hai costruito una porta capace di teletrasportare e qualcuno vuole distruggerla. Piega lo spazio "
        "con il potere della porta per proteggerla."),
    "ardu-buggy": M(450, [450, 250],
        "Ardu Buggy by NonoNano: drive the moon buggy over craters and rocks, jumping and shooting along the way.",
        "Ardu Buggy di NonoNano: guida il rover lunare tra crateri e rocce, saltando e sparando lungo il percorso."),
    "ardu-man": M(20, [20, 450],
        "Ardu-man by Seth Robinson: a maze chase in the spirit of the early-80s arcade classics. Eat every dot and keep "
        "away from the ghosts.",
        "Ardu-man di Seth Robinson: un inseguimento nel labirinto nello spirito dei classici da sala giochi dei primi "
        "anni '80. Mangia tutti i puntini e stai lontano dai fantasmi.",
        "Arrows steer; A selects in the menu.",
        "Le frecce guidano; A sceglie nel menu."),
    "boris-goes-skiing": M(20, [20, 450],
        "Boris Goes Skiing by Tom Sparrow: cross the road, rent your skis and race down the slalom between the flags "
        "and the trees. A tribute to a ZX Spectrum classic.",
        "Boris Goes Skiing di Tom Sparrow: attraversa la strada, noleggia gli sci e scendi nello slalom tra bandierine "
        "e alberi. Un omaggio a un classico dello ZX Spectrum."),
    "chri-bocchi-cat": M(150, [150, 450],
        "Chri-Bocchi Cat by OBONO: move the cat and bounce the falling gift boxes for two minutes. Made for the second "
        "Arduboy game jam.",
        "Chri-Bocchi Cat di OBONO: muovi il gatto e fai rimbalzare i pacchi regalo che cadono per due minuti. Creato per "
        "la seconda game jam Arduboy.",
        "Left/right move the cat; A starts.", "Sinistra/destra muovono il gatto; A avvia.",
        notes_en=OBONO_EN, notes_it=OBONO_IT),
    "diamonds": M(20, [20, 250],
        "Diamonds by RedBug: a mix of brick breaker and puzzle game, first released on the HP48 calculator more than "
        "20 years ago. Guide the ball to clear the diamonds.",
        "Diamonds di RedBug: un misto tra spaccamattoni e rompicapo, uscito più di 20 anni fa sulla calcolatrice HP48. "
        "Guida la pallina per eliminare i diamanti.",
        "Left/right steer the ball, A starts.", "Sinistra/destra dirigono la pallina, A avvia."),
    "evade": M(600, [600, 1199],
        "Evade by Modus Create: aliens destroyed your colony. Escape through space, dodging and shooting the invaders.",
        "Evade di Modus Create: gli alieni hanno distrutto la tua colonia. Fuggi nello spazio schivando e abbattendo gli "
        "invasori.",
        "Arrows move, A fires and selects.", "Le frecce muovono, A spara e sceglie."),
    "hollow-seeker": M(150, [150, 600],
        "Hollow Seeker by OBONO: keep going right and look for a hollow to hide in, so you don't get crushed by the "
        "walls.",
        "Hollow Seeker di OBONO: vai sempre verso destra e cerca una cavità in cui ripararti per non restare schiacciato.",
        notes_en=OBONO_EN, notes_it=OBONO_IT),
    "kong": M(60, [60, 250],
        "Kong by Press Play On Tape: a remake of a classic 1980s handheld game. Climb the girders, jump over the "
        "barrels and reach the top.",
        "Kong di Press Play On Tape: il remake di un classico videogioco tascabile degli anni '80. Sali sulle travi, "
        "salta i barili e arriva in cima.",
        "Arrows move and climb, A jumps.", "Le frecce muovono e fanno arrampicare, A salta."),
    "kong-ii": M(60, [60, 300],
        "Kong II by Press Play On Tape: the sequel, again a remake of a classic 1980s handheld game. Climb the vines, "
        "avoid the birds and free the prisoner.",
        "Kong II di Press Play On Tape: il seguito, anche questo remake di un classico tascabile degli anni '80. Sali "
        "sulle liane, evita gli uccelli e libera il prigioniero.",
        "Arrows move and climb, A jumps.", "Le frecce muovono e fanno arrampicare, A salta."),
    "loverush": M(150, [150, 250],
        "LoveRush by Vampirics: an endless shoot 'em up. Collect hearts to build up your shields and shoot the angry "
        "faces rushing at you; every 15 hearts give an extra shield and the speed grows with your score.",
        "LoveRush di Vampirics: uno sparatutto infinito. Raccogli i cuori per rinforzare gli scudi e colpisci le facce "
        "arrabbiate che ti corrono incontro; ogni 15 cuori guadagni uno scudo e la velocità cresce con il punteggio."),
    "poop-panic": M(20, [20, 600],
        "Poop Panic! by inajob: you keep the zoo clean. Pick up what the animals leave behind before it piles up.",
        "Poop Panic! di inajob: tieni pulito lo zoo. Raccogli quello che lasciano gli animali prima che si accumuli."),
    "ravine-despoiler": M(100, [100, 900],
        "Ravine Despoiler by Ben Combee: fly over the ravine and drop bombs on the boulders below, in the spirit of an "
        "Atari arcade classic. A missed shot costs five points.",
        "Ravine Despoiler di Ben Combee: sorvola il burrone e sgancia bombe sui massi sottostanti, nello spirito di un "
        "classico arcade Atari. Un colpo a vuoto costa cinque punti.",
        "Left/right adjust the speed, A drops a bomb.", "Sinistra/destra regolano la velocità, A sgancia una bomba."),
    "space-cab": M(60, [60, 300],
        "Space Cab by Vampirics and Filmote: fly your taxi around the level, pick up the passengers and land them "
        "at their gate. Watch your fuel.",
        "Space Cab di Vampirics e Filmote: pilota il tuo taxi nel livello, carica i passeggeri e portali al loro "
        "cancello. Attento al carburante.",
        "Left/right steer, A thrusts, B lowers the landing gear. On the title screen fly to a gate to start.",
        "Sinistra/destra sterzano, A dà spinta, B abbassa il carrello. Nel titolo vola fino a un cancello per iniziare."),
    "social-distance": M(20, [20, 450],
        "The Social Distance Game by msanatan: keep out of people's way for as long as you can.",
        "The Social Distance Game di msanatan: stai lontano dalle persone il più a lungo possibile."),
    "arduboy-life": M(20, [20, 250],
        "ArduboyLife by Scott Allen: Conway's Game of Life, the famous cellular automaton, with random or drawn "
        "starting patterns and adjustable speed.",
        "ArduboyLife di Scott Allen: il Gioco della vita di Conway, il celebre automa cellulare, con configurazioni "
        "casuali o disegnate e velocità regolabile.",
        "The help screen lists the buttons (run/pause, step, speed, sound, new pattern).",
        "La schermata di aiuto elenca i tasti (avvio/pausa, passo, velocità, suono, nuova configurazione)."),
    "the-bounce": M(20, [20, 600],
        "The Bounce by Joshimuz: a bouncy-ball physics platformer. Easy to understand, very hard to master in the "
        "later levels.",
        "The Bounce di Joshimuz: un platform con la fisica di una pallina che rimbalza. Facile da capire, difficilissimo "
        "da padroneggiare nei livelli avanzati."),
    "ardecipher": M(20, [20, 150],
        "ARDecipher by databhor: a terminal-hacking puzzle. Find the password among the words within 5 tries: each "
        "wrong guess tells you how many letters were in the right place.",
        "ARDecipher di databhor: un rompicapo in cui forzi un terminale. Trova la password tra le parole in 5 tentativi: "
        "ogni errore ti dice quante lettere erano al posto giusto.",
        "Arrows choose a word, A tries it, B shows the help.", "Le frecce scelgono una parola, A la prova, B mostra l'aiuto.",
        notes_en="The LICENSE file is MIT; it also carries the BSD notice of the bundled Font4x6.",
        notes_it="Il file LICENSE è MIT; riporta anche la nota BSD del font Font4x6 incluso."),
    "ardulo": M(60, [60, 600],
        "ArduLO by Jon Thysell: the classic lights-out puzzle. Every press toggles a light and its neighbours: switch "
        "them all off.",
        "ArduLO di Jon Thysell: il classico rompicapo delle luci. Ogni pressione accende o spegne una luce e le vicine: "
        "spegnile tutte.",
        "Arrows move the cursor, A toggles, B goes back.", "Le frecce spostano il cursore, A commuta, B torna indietro."),
    "box-stacker": M(150, [150, 900],
        "Box Stacker by dragula96: stack the moving boxes as high as you can, like the arcade prize machine, but the "
        "only prize is satisfaction.",
        "Box Stacker di dragula96: impila le scatole in movimento il più in alto possibile, come la macchina a premi "
        "delle sale giochi, ma l'unico premio è la soddisfazione.",
        "A stops the moving row.", "A ferma la fila in movimento."),
    "chie-magari-ita": M(150, [150, 900], title="Chie Magari Ita",
        en="Chie Magari Ita by OBONO: a traditional Japanese tiling puzzle. Place all 10 pieces into the frame without "
           "overlapping them.",
        it="Chie Magari Ita di OBONO: un rompicapo tradizionale giapponese. Sistema tutti i 10 pezzi nella cornice "
           "senza sovrapporli.",
        notes_en=OBONO_EN, notes_it=OBONO_IT),
    "dominoes": M(80, [80, 1199], title="Dominoes: All Fives",
        en="Dominoes 'All Fives' by Filmote and Vampirics: the popular dominoes variant where you score when the open "
           "ends of the layout add up to five or a multiple of five.",
        it="Dominoes 'All Fives' di Filmote e Vampirics: la variante del domino in cui si fa punto quando le estremità "
           "aperte della fila sommano cinque o un multiplo di cinque."),
    "hangman": M(20, [20, 250], title="Hangman",
        en="Hangman! by serisman: guess the word letter by letter before six wrong guesses complete the drawing.",
        it="Hangman! di serisman: indovina la parola lettera per lettera prima che sei errori completino il disegno "
           "dell'impiccato. Le parole sono in inglese.",
        c_en="Arrows pick a letter, A tries it, B pauses.", c_it="Le frecce scelgono una lettera, A la prova, B mette in pausa."),
    "knight-move": M(150, [150, 450],
        "Knight Move by OBONO: an action puzzle with a chess knight that jumps in an L shape across an 8x4 board.",
        "Knight Move di OBONO: un rompicapo d'azione con un cavallo degli scacchi che salta a L su una scacchiera 8x4.",
        notes_en=OBONO_EN, notes_it=OBONO_IT),
    "lite-out": M(20, [20, 250],
        "Lite/Out by K. M. Kroski: turn off every light on the 5x5 grid, but each change also flips the lights around it.",
        "Lite/Out di K. M. Kroski: spegni tutte le luci della griglia 5x5, ma ogni mossa cambia anche quelle intorno.",
        "Arrows move, A toggles.", "Le frecce muovono, A commuta."),
    "minesweeper": M(100, [100, 1199],
        "Minesweeper by Pharap: the classic mine-clearing puzzle, with themes and statistics.",
        "Minesweeper di Pharap: il classico rompicapo delle mine, con temi grafici e statistiche.",
        "Arrows move, A reveals a cell, B places a flag.", "Le frecce muovono, A scopre una casella, B mette una bandierina."),
    "pipes": M(60, [60, 300],
        "Pipes by Filmote: connect each pair of matching nodes with a pipe, without any pipes crossing. Puzzles from "
        "5x5 to 9x9.",
        "Pipes di Filmote: collega con un tubo ogni coppia di nodi uguali senza che i tubi si incrocino. Rompicapi da "
        "5x5 a 9x9."),
    "ponghauki": M(20, [20, 900], title="Pong Hau K'i",
        en="Pong Hau K'i by databhor: a traditional two-player blocking game from China. Move one of your two pieces "
           "each turn; whoever gets blocked loses.",
        it="Pong Hau K'i di databhor: un gioco tradizionale cinese di blocco. A turno muovi una delle tue due pedine; "
           "chi resta bloccato perde."),
    "psi-colo": M(150, [150, 900], title="Psi Colo",
        en="Psi Colo by OBONO: a dice puzzle on a 5x7 field. Make groups of dice with the same pips, at least as many "
           "as the pips shown, to sink them; chain reactions are possible.",
        it="Psi Colo di OBONO: un rompicapo con i dadi su un campo 5x7. Forma gruppi di dadi con lo stesso numero, "
           "grandi almeno quanto il numero, per farli sprofondare; sono possibili le reazioni a catena.",
        notes_en=OBONO_EN, notes_it=OBONO_IT),
    "quarto": M(150, [150, 450],
        "Quarto! by OBONO: the abstract strategy board game on a 4x4 board with 16 unique pieces, against the "
        "computer or a friend.",
        "Quarto! di OBONO: il gioco di strategia astratto su una scacchiera 4x4 con 16 pezzi tutti diversi, contro il "
        "computer o un amico.",
        notes_en=OBONO_EN, notes_it=OBONO_IT),
    "stairs-sweep": M(150, [150, 450],
        "Stairs Sweep by OBONO: a falling-block puzzle played sideways. Line up boxes to clear them; the enemies can "
        "only be beaten with a ball.",
        "Stairs Sweep di OBONO: un rompicapo a blocchi che cadono, giocato in verticale. Allinea le scatole per "
        "eliminarle; i nemici si battono solo con una palla.",
        "The picture is turned sideways (the game is played in portrait); the arrows follow the rotation.",
        "L'immagine è ruotata (si gioca in verticale); le frecce seguono la rotazione.",
        notes_en=OBONO_EN, notes_it=OBONO_IT),
    "tictaccurly": M(20, [20, 1199],
        "TicTacCurly by curly: tic-tac-toe for one or two players, with difficulty levels against the computer.",
        "TicTacCurly di curly: il tris per uno o due giocatori, con livelli di difficoltà contro il computer.",
        "Arrows move the cursor, A places your mark, B confirms in the menus.",
        "Le frecce spostano il cursore, A mette il segno, B conferma nei menu."),
    "tres": M(35, [35, 900],
        "Tres by Casey Gold: a sliding number puzzle. Slide the tiles, merge 1 with 2 and equal numbers from 3 up, "
        "and aim for the highest score.",
        "Tres di Casey Gold: un rompicapo di numeri scorrevoli. Fai scorrere le tessere, unisci 1 con 2 e i numeri "
        "uguali da 3 in su e punta al punteggio più alto.",
        "Arrows slide the tiles, B opens the menu (resume, save, exit).",
        "Le frecce fanno scorrere le tessere, B apre il menu (riprendi, salva, esci)."),
    "waternet": M(60, [60, 250],
        "Waternet by Willems Davy: connect every pipe to the water source by rotating or sliding the tiles, based "
        "on the Net and Netslide puzzles from Simon Tatham's collection.",
        "Waternet di Willems Davy: collega tutti i tubi alla sorgente d'acqua ruotando o facendo scorrere le tessere, "
        "ispirato ai rompicapi Net e Netslide della raccolta di Simon Tatham.",
        "Arrows move, A rotates or slides, B goes back.", "Le frecce muovono, A ruota o fa scorrere, B torna indietro."),
    "randocity": M(20, [20, 450],
        "Randocity by pmwasson: ride a motorcycle through a large procedurally generated city, with free play, a "
        "game mode, records and a map.",
        "Randocity di pmwasson: guida una moto in una grande città generata proceduralmente, con giro libero, partita, "
        "record e mappa.",
        "Arrows steer, B selects in the menu, A+B brings the menu back.",
        "Le frecce guidano, B sceglie nel menu, A+B riporta al menu."),
    "dark-and-under": M(20, [20, 450], title="Dark & Under",
        en="Dark & Under by the Garage Collective: a first-person dungeon crawler. Explore the corridors of the "
           "Undermountain, fight its creatures and steal the greedy dragon's treasure.",
        it="Dark & Under del Garage Collective: un dungeon crawler in prima persona. Esplora i corridoi del Sottomonte, "
           "combatti le sue creature e ruba il tesoro dell'avido drago."),
    "hello-commander": M(20, [20, 600],
        "Hello, Commander by Felipe Manga: a turn-based strategy game. Lead your squad from bunker to bunker to "
        "deliver the message.",
        "Hello, Commander di Felipe Manga: un gioco di strategia a turni. Guida la tua squadra di bunker in bunker per "
        "consegnare il messaggio.",
        "Arrows move, A walks or confirms, B opens the commands.",
        "Le frecce muovono, A cammina o conferma, B apre i comandi."),
    "ardubullets": M(150, [150, 600],
        "ARDUBULLETs by OBONO: a bullet-hell shooter. Weave through dense bullet patterns and defeat the enemy squadron "
        "within one minute.",
        "ARDUBULLETs di OBONO: uno sparatutto bullet hell. Infilati tra fitte raffiche di proiettili e sconfiggi la "
        "squadriglia nemica entro un minuto.",
        notes_en=OBONO_EN + " Its sound code is replaced by the NucleoOS score player.",
        notes_it=OBONO_IT + " Il suo codice audio è sostituito dal lettore di spartiti di NucleoOS."),
    "cosmicpods": M(20, [450, 200],
        "CosmicPods by cubic9com: a tiny shoot 'em up. You are a cosmic squid: shoot the octopuses!",
        "CosmicPods di cubic9com: un piccolo sparatutto. Sei un calamaro cosmico: abbatti i polpi!",
        "Arrows move, A fires.", "Le frecce muovono, A spara."),
    "galaxion": M(150, [150, 900],
        "Galaxion by tako2: a fixed-screen space shooter. Destroy the alien formations as they swoop down on you.",
        "Galaxion di tako2: uno sparatutto spaziale a schermo fisso. Distruggi le formazioni aliene che ti piombano "
        "addosso.",
        "Left/right move, A fires.", "Sinistra/destra muovono, A spara."),
    "humanity-revenge": M(20, [20, 900], title="Humanity Revenge DC",
        en="Humanity Revenge DC by giangregorioc: a hard shoot 'em up with three ships to choose from, five kinds of "
           "enemies, three big bosses, bombs, power-ups and high scores.",
        it="Humanity Revenge DC di giangregorioc: uno sparatutto impegnativo con tre navi tra cui scegliere, cinque tipi "
           "di nemici, tre grandi boss, bombe, potenziamenti e record."),
    "l4arduboy": M(20, [20, 150], title="i4arduboy",
        en="i4arduboy by Amamoriya Yomogimaru: a horizontal shooter. Steer a submarine and snipe far-away enemies, even "
           "off-screen, with torpedoes.",
        it="i4arduboy di Amamoriya Yomogimaru: uno sparatutto orizzontale. Guida un sottomarino e colpisci con i siluri "
           "i nemici lontani, anche fuori dallo schermo."),
    "night-raid": M(20, [20, 1199],
        "Night Raid by Evan Barger: defend your cities from the incoming missiles, in the spirit of the arcade "
        "missile-defence classics.",
        "Night Raid di Evan Barger: difendi le tue città dai missili in arrivo, nello spirito dei classici arcade di "
        "difesa missilistica."),
    "omega-chase": M(60, [60, 300],
        "Omega Chase by Karl P. Williams: fly your ship around the arena and shoot down the enemy ships while "
        "avoiding the mines, inspired by an early-80s arcade shooter.",
        "Omega Chase di Karl P. Williams: pilota la navicella nell'arena e abbatti le navi nemiche evitando le mine, "
        "ispirato a uno sparatutto da sala giochi dei primi anni '80."),
    "stellar-impact": M(20, [20, 900],
        "Stellar Impact by Nick Allen: a shoot 'em up with procedurally generated starfields, a time-freezing bomb and "
        "saved high scores.",
        "Stellar Impact di Nick Allen: uno sparatutto con campi stellari generati proceduralmente, una bomba che ferma "
        "il tempo e record salvati."),
    # built by the harness but not published
    "trench-run": dict(skip="fan game set in a third-party film universe (its names and logo appear in the game)"),
    "choplifter": dict(skip=TM),
    "nineteen43": dict(skip=TM),
    "nineteen44": dict(skip=TM),
    "pong": dict(skip=TM),
    # do not build
    "back-to-the-jungle": dict(skip="needs the PGMWrap library (not packaged)"),
    "castleboy": dict(skip="own display/sound core (Arglib) with AVR assembly"),
    "ard-drivin": dict(skip="own display core (ArduboyRem) with AVR assembly and 16-bit pointer casts"),
    "star-honor": dict(skip="own display core with AVR assembly; delete on arrays"),
    "pipeboy": dict(skip="const-correctness and overload errors that only avr-gcc -fpermissive accepts"),
    "ring-puzzle": dict(skip="const-correctness errors that only avr-gcc -fpermissive accepts"),
}
for _slug in ICON_FULL:
    META[_slug]["icon_full"] = True
_BSD_NOTE = dict(notes_en="game.ini lists {ini}; the repository's LICENSE file is BSD 3-clause, which applies.",
                 notes_it="game.ini indica {ini}; il file LICENSE del repository è BSD a 3 clausole, ed è quello che vale.")
for _slug, _ini in (("kong", "MIT"), ("kong-ii", "MIT"), ("space-cab", "GPL-3.0")):
    META[_slug]["notes_en"] = _BSD_NOTE["notes_en"].format(ini=_ini)
    META[_slug]["notes_it"] = _BSD_NOTE["notes_it"].format(ini=_ini)
META["ponghauki"]["notes_en"] = META["ardecipher"]["notes_en"]
META["ponghauki"]["notes_it"] = META["ardecipher"]["notes_it"]
