"""games.py — the Doom games published on the NucleoOS store, built on the Doom engine app.

    python ports/doom/games.py data <out>    WADs + license texts + <id>.game descriptors into
                                             <out> (= <store site>/data/doom, served by Pages)
    python ports/doom/games.py packages      apps/<id>/manifest.json for every game

Each game is a store package with no module ("engine": "doom"): the engine downloads what the
game's descriptor lists into /sdcard/home/doom (shared: Freedoom is fetched once for every game
built on it). Sources are the pinned zips from ports/doom/fetch.sh; files are re-hosted
byte-identical, each with the text file its license asks to ship with it.
"""
import hashlib
import json
import os
import shutil
import sys
import zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, "..", ".."))
SRC = os.path.join(ROOT, "ports", "_src", "doom")
ENGINE_VERSION = "1.0"
GAME_VERSION = "1.0.1"
# firmware 1.1.140: WASI opens files bigger than free PSRAM (the WADs), AOT float->int64 helpers
WASI_VERSION = "1.1"

# Hosted files: name on the site -> (zip under ports/_src/doom, member inside it) or a plain file.
FILES = {
    "freedoom1.wad":     ("freedoom-0.13.0.zip", "freedoom-0.13.0/freedoom1.wad"),
    "freedoom2.wad":     ("freedoom-0.13.0.zip", "freedoom-0.13.0/freedoom2.wad"),
    "freedoom-copying.txt": ("freedoom-0.13.0.zip", "freedoom-0.13.0/COPYING.txt"),
    "freedoom-credits.txt": ("freedoom-0.13.0.zip", "freedoom-0.13.0/CREDITS.txt"),
    "doom1.wad":         ("games/x/doom19s/ext/DOOM1.WAD", None),
    "doom1-license.txt": ("games/doom1-license.txt", None),
    "scythe.wad":        ("games/scythe.zip", "SCYTHE.WAD"),
    "scythe.txt":        ("games/scythe.zip", "scythe.txt"),
    "mm.wad":            ("games/mm_allup.zip", "MM.WAD"),
    "mmmus.wad":         ("games/mm_allup.zip", "MMMUS.WAD"),
    "mm.txt":            ("games/mm_allup.zip", "MM.TXT"),
    "zone300.wad":       ("games/zone300.zip", "zone300.wad"),
    "zone300.txt":       ("games/zone300.zip", "zone300.txt"),
    "dtwid.wad":         ("games/dtwid.zip", "DTWID.wad"),
    "dtwid.txt":         ("games/dtwid.zip", "DTWID.txt"),
    "sigildos.wad":      ("games/sigildos.zip", "SIGILDOS.wad"),
    "sigildos.txt":      ("games/sigildos.zip", "SIGILDOS.txt"),
    "pl2.wad":           ("games/pl2.zip", "PL2.WAD"),
    "pl2.txt":           ("games/pl2.zip", "PL2.TXT"),
    "d2reload.wad":      ("games/d2reload.zip", "D2RELOAD.WAD"),
    "d2reload.txt":      ("games/d2reload.zip", "D2RELOAD.TXT"),
    "1klinecp2.wad":     ("games/1klinecp2.zip", "1klinecp2.wad"),
    "1klinecp2.txt":     ("games/1klinecp2.zip", "1klinecp2.txt"),
    "udtwid.wad":        ("games/udtwid.zip", "UDTWiD.wad"),
    "udtwid.txt":        ("games/udtwid.zip", "UDTWiD.txt"),
    "dtwid-le.wad":      ("games/dtwid-le.zip", "DTWID-LE.wad"),
    "dtwid-le.txt":      ("games/dtwid-le.zip", "DTWID-LE.txt"),
    "neis.wad":          ("games/neis.zip", "NEIS.wad"),
    "neis.txt":          ("games/neis.zip", "NEIS.txt"),
    "dbimpact.wad":      ("games/dbimpact.zip", "dbimpact.wad"),
    "dbimpact.txt":      ("games/dbimpact.zip", "dbimpact.txt"),
    "bgcomp.wad":        ("games/bgcomp.zip", "BGComp.wad"),
    "bgcomp.txt":        ("games/bgcomp.zip", "BGComp.txt"),
    "lunar.wad":         ("games/lunar.zip", "Lunar/Lunar.Wad"),
    "lunar.txt":         ("games/lunar.zip", "Lunar/Lunar.txt"),
    "standard.wad":      ("games/standard.zip", "standard.wad"),
    "standard.txt":      ("games/standard.zip", "standard.txt"),
    "doom404.wad":       ("games/doom404.zip", "doom404.wad"),
    "doom404.txt":       ("games/doom404.zip", "doom404.txt"),
    "hr2final.wad":      ("games/hr2final.zip", "hr2final.wad"),
    "hr2final.txt":      ("games/hr2final.zip", "hr2final.txt"),
    "darken2.wad":       ("games/darken2.zip", "Darken2.wad"),
    "darken2.txt":       ("games/darken2.zip", "darken2.txt"),
    "sinseven.wad":      ("games/sinseven.zip", "SinSeven.wad"),
    "sinseven.txt":      ("games/sinseven.zip", "SinSeven.txt"),
    "mutiny.wad":        ("games/mutiny.zip", "MUTINY.wad"),
    "mutiny.txt":        ("games/mutiny.zip", "mutiny.txt"),
    "ur_final.wad":      ("games/ur.zip", "ur_final.wad"),
    "ur.txt":            ("games/ur.zip", "ur.txt"),
    "nova.wad":          ("games/nova.zip", "NOVA/NOVA.wad"),
    "nova.txt":          ("games/nova.zip", "NOVA/NOVA.txt"),
    "d2twid.wad":        ("games/d2twid.zip", "D2TWID.wad"),
    "d2twid.deh":        ("games/d2twid.zip", "D2TWID.deh"),
    "d2twid.txt":        ("games/d2twid.zip", "D2TWID.txt"),
    "dtwid.deh":         ("games/dtwid.zip", "DTWID.deh"),
    "udtwid.deh":        ("games/udtwid.zip", "UDTWiD.deh"),
    "dtwid-le.deh":      ("games/dtwid-le.zip", "DTWID-LE.deh"),
    "neis.deh":          ("games/neis.zip", "NEIS.deh"),
    "pl2.deh":           ("games/pl2.zip", "PL2.DEH"),
    "1klinecp2.deh":     ("games/1klinecp2.zip", "1klinecp2.deh"),
    "doom404.deh":       ("games/doom404.zip", "doom404.deh"),
    "mutiny.deh":        ("games/mutiny.zip", "MUTINY.deh"),
    "scythe2.wad":       ("games/scythe2.zip", "scythe2.wad"),
    "scythe2.txt":       ("games/scythe2.zip", "scythe2.txt"),
    "deathless.wad":     ("games/deathless.zip", "deathless.wad"),
    "deathless.txt":     ("games/deathless.zip", "deathless1.1.txt"),
    "cyber110.wad":      ("games/cydreams.zip", "Cyber110.wad"),
    "cyber110.deh":      ("games/cydreams.zip", "Cyber110.deh"),
    "cydreams.txt":      ("games/cydreams.zip", "Cydreams.txt"),
    "rudy2.wad":         ("games/rudy2.zip", "rudy2.wad"),
    "rudy2.deh":         ("games/rudy2.zip", "rudy2.deh"),
    "rudy2.txt":         ("games/rudy2.zip", "rudy2.txt"),
    "1k3v1a.wad":        ("games/1k3v1a.zip", "1k3v1a.wad"),
    "1k3v1a.deh":        ("games/1k3v1a.zip", "1k3v1a.deh"),
    "1k3v1a.txt":        ("games/1k3v1a.zip", "1k3v1a.txt"),
    "rekkr.wad":         ("games/rekkr.zip", "REKKR.wad"),
    "rekkr.deh":         ("games/rekkr.zip", "REKKR.deh"),
    "rekkr.txt":         ("games/rekkr.zip", "REKKR.txt"),
    "rek_creds.txt":     ("games/rekkr.zip", "REK_CREDS.txt"),
}

IDG = "https://www.doomworld.com/idgames/"

# id, title, iwad, pwads, license texts, author, license, source, EN / IT descriptions
GAMES = [
    ("freedoom1", "Freedoom: Phase 1", "freedoom1.wad", [], ["freedoom-copying.txt", "freedoom-credits.txt"],
     "The Freedoom project", "BSD-3-Clause", "https://freedoom.github.io",
     "A complete free game for the Doom engine: four episodes of demons, bases and hell.",
     "Un gioco completo e libero per il motore di Doom: quattro episodi tra basi, demoni e inferno."),
    ("freedoom2", "Freedoom: Phase 2", "freedoom2.wad", [], ["freedoom-copying.txt", "freedoom-credits.txt"],
     "The Freedoom project", "BSD-3-Clause", "https://freedoom.github.io",
     "32 levels of free, Doom II-compatible action; also the base for the community megawads.",
     "32 livelli liberi compatibili con Doom II; anche la base per i megawad della comunità."),
    ("doomsw", "DOOM Shareware", "doom1.wad", [], ["doom1-license.txt"],
     "id Software", "Shareware (freely distributable, unmodified)", "https://www.doomworld.com/idgames/idstuff/doom/doom19s",
     "The original 1993 shareware episode: Knee-Deep in the Dead, nine levels.",
     "L'episodio shareware originale del 1993: Knee-Deep in the Dead, nove livelli."),
    ("scythe", "Scythe", "freedoom2.wad", ["scythe.wad"], ["scythe.txt"],
     "Erik Alm", "Freely distributable (idgames)", "https://www.doomworld.com/idgames/levels/doom2/megawads/scythe",
     "32 short, fast maps that ramp up to a brutal third episode. Plays on Freedoom 2.",
     "32 mappe brevi e veloci, fino a un terzo episodio brutale. Si gioca su Freedoom 2."),
    ("mementomori", "Memento Mori", "freedoom2.wad", ["mm.wad", "mmmus.wad"], ["mm.txt"],
     "The Memento Mori team", "Freely distributable (idgames)", "https://www.doomworld.com/idgames/themes/mm/mm_allup",
     "The 1995 classic: 32 levels by 21 top mappers of the time, with its own soundtrack.",
     "Il classico del 1995: 32 livelli di 21 grandi autori dell'epoca, con la sua colonna sonora."),
    ("zone300", "Zone 300", "freedoom2.wad", ["zone300.wad"], ["zone300.txt"],
     "Paul Corfiatis", "Freely distributable (idgames)", "https://www.doomworld.com/idgames/levels/doom2/megawads/zone300",
     "32 compact maps of at most 300 lines each, with an original soundtrack.",
     "32 mappe compatte da massimo 300 linee l'una, con colonna sonora originale."),
    ("dtwid", "Doom The Way id Did", "freedoom1.wad", ["dtwid.wad"], ["dtwid.txt"],
     "Various authors", "Freely distributable (idgames)", "https://www.doomworld.com/idgames/levels/doom/megawads/dtwid",
     "Three new episodes as if id Software had made them in 1993. Plays on Freedoom 1.",
     "Tre nuovi episodi come se li avesse fatti id Software nel 1993. Si gioca su Freedoom 1."),
    ("sigil", "SIGIL (DOS edition)", "freedoom1.wad", ["sigildos.wad"], ["sigildos.txt"],
     "John Romero, adapted by the_kovic", "Freely distributable (idgames, with Romero's permission)",
     "https://www.doomworld.com/idgames/levels/doom/s-u/sigildos",
     "John Romero's unofficial fifth episode, reworked to vanilla limits. Replaces episode 3.",
     "Il quinto episodio non ufficiale di John Romero, rifatto nei limiti vanilla. Sostituisce l'episodio 3."),
    ("plutonia2", "Plutonia 2", "freedoom2.wad", ["pl2.wad"], ["pl2.txt"],
     "The PL2 team", "Freely distributable (idgames)", "https://www.doomworld.com/idgames/levels/doom2/megawads/pl2",
     "Fan-made sequel to Final Doom: Plutonia. 32 very hard maps.",
     "Seguito amatoriale di Final Doom: Plutonia. 32 mappe difficilissime."),
    ("d2reload", "Doom 2 Reloaded", "freedoom2.wad", ["d2reload.wad"], ["d2reload.txt"],
     "Andy Stewart", "Freely distributable, non-commercial (idgames)",
     "https://www.doomworld.com/idgames/levels/doom2/megawads/d2reload",
     "The story of Doom II retold in 32 new, linear maps.",
     "La storia di Doom II raccontata di nuovo in 32 mappe lineari."),
    ("thousandlines2", "1000 Lines 2", "freedoom2.wad", ["1klinecp2.wad"], ["1klinecp2.txt"],
     "Liberation and the 1000 Lines team", "CC BY 4.0", "https://www.doomworld.com/idgames/levels/doom2/megawads/1klinecp2",
     "34-map community project, every map under 1000 lines, with its own textures.",
     "Progetto della comunità da 34 mappe, ognuna sotto le 1000 linee, con texture proprie."),
    ('udtwid', 'Ultimate Doom The Way id Did', 'freedoom1.wad', ['udtwid.wad'], ['udtwid.txt'],
     'Various authors', 'Freely distributable (idgames)', IDG + 'levels/doom/s-u/udtwid',
     'A new fourth episode made as if id Software had shipped it in 1995. Plays on Freedoom 1.',
     "Un nuovo quarto episodio come se l'avesse fatto id Software nel 1995. Si gioca su Freedoom 1."),
    ('dtwidle', 'DTWID: The Lost Episodes', 'freedoom1.wad', ['dtwid-le.wad'], ['dtwid-le.txt'],
     'Various authors', 'Freely distributable (idgames)', IDG + 'levels/doom/Ports/megawads/dtwid-le',
     'Four more episodes of classic 1993-style Doom maps. Plays on Freedoom 1.',
     'Altri quattro episodi di mappe nello stile classico del 1993. Si gioca su Freedoom 1.'),
    ('neis', 'No End In Sight', 'freedoom1.wad', ['neis.wad'], ['neis.txt'],
     'Brundage, Xaser, Lutz', 'Freely distributable (idgames)', IDG + 'levels/doom/Ports/megawads/neis',
     'Four big episodes for Ultimate Doom, a 2016 Cacoward winner.',
     'Quattro grandi episodi per Ultimate Doom, vincitore di un Cacoward 2016.'),
    ('dbimpact', 'Double Impact', 'freedoom1.wad', ['dbimpact.wad'], ['dbimpact.txt'],
     'RottKing and Ralphis', 'Freely distributable (idgames)', IDG + 'levels/doom/Ports/d-f/dbimpact',
     'A polished, tough replacement for the first episode, Knee-Deep in the Dead.',
     'Un nuovo primo episodio, curato e impegnativo, al posto di Knee-Deep in the Dead.'),
    ('bganymede', 'Base Ganymede', 'freedoom1.wad', ['bgcomp.wad'], ['bgcomp.txt'],
     'Khorus', 'Freely distributable (idgames)', IDG + 'levels/doom/megawads/bgcomp',
     "Three vanilla episodes on a base on Jupiter's moon Ganymede.",
     'Tre episodi vanilla in una base sulla luna di Giove Ganimede.'),
    ('lunar', 'Lunar Catastrophe', 'freedoom1.wad', ['lunar.wad'], ['lunar.txt'],
     'Miss Bubbles and Count651', 'Freely distributable (idgames)', IDG + 'levels/doom/Ports/megawads/lunar',
     '36 classic-style maps across four episodes on the Moon.',
     '36 mappe in stile classico in quattro episodi sulla Luna.'),
    ('deadlystd', 'Deadly Standards', 'freedoom1.wad', ['standard.wad'], ['standard.txt'],
     'Various authors', 'CC BY-NC 4.0', IDG + 'levels/doom/Ports/s-u/standard',
     'A community first episode built under strict design rules.',
     'Un primo episodio della comunità costruito con regole di design rigide.'),
    ('doom404', 'Doom 404', 'freedoom2.wad', ['doom404.wad'], ['doom404.txt'],
     'Adam Windsor', 'Freely distributable (idgames)', IDG + 'levels/doom2/megawads/doom404',
     '32 short maps starring a marine from the IT help desk.',
     "32 mappe brevi con un marine dell'help desk informatico."),
    ('hr2', 'Hell Revealed II', 'freedoom2.wad', ['hr2final.wad'], ['hr2final.txt'],
     'Jonas Feragen et al.', 'Freely distributable (idgames)', IDG + 'themes/hr/hr2final',
     'The very hard sequel to Hell Revealed: 32 maps.',
     'Il seguito difficilissimo di Hell Revealed: 32 mappe.'),
    ('darken2', 'The Darkening Episode 2', 'freedoom2.wad', ['darken2.wad'], ['darken2.txt'],
     'The Darkening team', 'Freely distributable (idgames)', IDG + 'levels/doom2/megawads/darken2',
     '12 single-player maps plus 12 for deathmatch, with new textures.',
     '12 mappe per giocatore singolo e 12 deathmatch, con texture nuove.'),
    ('sinseven', 'Sinister Seven', 'freedoom2.wad', ['sinseven.wad'], ['sinseven.txt'],
     'Doomkid', 'Freely distributable (idgames)', IDG + 'levels/doom2/s-u/sinseven',
     'Seven compact vanilla maps.',
     'Sette mappe vanilla compatte.'),
    ('mutiny', 'Mutiny', 'freedoom2.wad', ['mutiny.wad'], ['mutiny.txt'],
     'Doomworld community', 'Freely distributable (idgames)', IDG + 'levels/doom2/Ports/megawads/mutiny',
     '16 retro maps set in a city taken over by demons.',
     '16 mappe retrò in una città invasa dai demoni.'),
    ('unholyrealms', 'Unholy Realms', 'freedoom2.wad', ['ur_final.wad'], ['ur.txt'],
     'Brian Knox', 'Freely distributable (idgames)', IDG + 'levels/doom2/megawads/ur',
     '32 maps by a single author, from techbases to hell.',
     "32 mappe di un solo autore, dalle basi tecnologiche all'inferno."),
    ('nova', 'NOVA: The Birth', 'freedoom2.wad', ['nova.wad'], ['nova.txt'],
     'TeamNOVA', 'Freely distributable (idgames)', IDG + 'levels/doom2/Ports/megawads/nova',
     'A community megawad made only with the stock Doom II textures.',
     'Un megawad della comunità fatto solo con le texture originali di Doom II.'),
    ('d2twid', 'Doom 2 The Way id Did', 'freedoom2.wad', ['d2twid.wad'], ['d2twid.txt'],
     'Various authors', 'Freely distributable (idgames)', IDG + 'levels/doom2/megawads/d2twid',
     "A whole new Doom II in id Software's original 1994 style.",
     'Un Doom II tutto nuovo nello stile originale id Software del 1994.'),
    ('scythe2', 'Scythe 2', 'freedoom2.wad', ['scythe2.wad'], ['scythe2.txt'],
     'Erik Alm', 'Freely distributable (idgames)', IDG + 'levels/doom2/Ports/megawads/scythe2',
     "Erik Alm's classic sequel: fast maps that grow into huge battles, with a new enemy.",
     'Il classico seguito di Erik Alm: mappe veloci che crescono fino a battaglie enormi, con un nuovo nemico.'),
    ('deathless', 'Deathless', 'freedoom1.wad', ['deathless.wad'], ['deathless.txt'],
     'Jimmy (James Paddock)', 'Freely distributable (idgames)', IDG + 'levels/doom/Ports/megawads/deathless',
     '36 maps for Ultimate Doom, all made in November 2018.',
     '36 mappe per Ultimate Doom, tutte create a novembre 2018.'),
    ('cyberdreams', 'Cyberdreams', 'freedoom2.wad', ['cyber110.wad'], ['cydreams.txt'],
     'Pérez de la Ossa and Valls', 'Freely distributable (idgames)', IDG + 'levels/doom2/megawads/cydreams',
     'A 1998 cyberpunk megawad with reworked monsters.',
     'Un megawad cyberpunk del 1998 con mostri rielaborati.'),
    ('rowdyrudy2', 'Rowdy Rudy II', 'freedoom2.wad', ['rudy2.wad'], ['rudy2.txt'],
     'Doomkid and co.', 'CC BY 4.0', IDG + 'levels/doom2/megawads/rudy2',
     '20 punchy vanilla maps with new weapons and monsters.',
     '20 mappe vanilla grintose con armi e mostri nuovi.'),
    ('thousandlines3', '1000 Lines 3', 'freedoom2.wad', ['1k3v1a.wad'], ['1k3v1a.txt'],
     'Liberation and the 1000 Lines team', 'CC BY 4.0', IDG + 'levels/doom2/megawads/1k3v1a',
     '32 community maps of at most 1000 lines, with new monsters.',
     '32 mappe della comunità da massimo 1000 linee, con nuovi mostri.'),
    ('rekkr', 'REKKR', 'freedoom1.wad', ['rekkr.wad'], ['rekkr.txt', 'rek_creds.txt'],
     'Revae and co.', 'CC BY-NC 4.0', IDG + 'levels/doom/megawads/rekkr',
     'A full Viking total conversion: new world, weapons, monsters and music.',
     'Una conversione totale vichinga: mondo, armi, mostri e musica tutti nuovi.'),
]

# DeHackEd patches shipped as separate .deh files (a DEHACKED lump inside a PWAD loads by itself)
DEH = {
    "dtwid": ["dtwid.deh"], "udtwid": ["udtwid.deh"], "dtwidle": ["dtwid-le.deh"], "neis": ["neis.deh"],
    "plutonia2": ["pl2.deh"], "thousandlines2": ["1klinecp2.deh"], "doom404": ["doom404.deh"],
    "mutiny": ["mutiny.deh"], "d2twid": ["d2twid.deh"],
    "cyberdreams": ["cyber110.deh"], "rowdyrudy2": ["rudy2.deh"], "thousandlines3": ["1k3v1a.deh"],
    "rekkr": ["rekkr.deh"],
}

# es / fr / de store copy (en / it live in GAMES)
MORE = {
    "freedoom1": ("Un juego completo y libre para el motor de Doom: cuatro episodios de bases, demonios e infierno.",
                  "Un jeu complet et libre pour le moteur de Doom : quatre épisodes de bases, de démons et d'enfer.",
                  "Ein komplettes freies Spiel für die Doom-Engine: vier Episoden mit Basen, Dämonen und Hölle."),
    "freedoom2": ("32 niveles libres compatibles con Doom II; también la base de los megawads de la comunidad.",
                  "32 niveaux libres compatibles Doom II ; aussi la base des mégawads de la communauté.",
                  "32 freie, Doom-II-kompatible Level; auch die Basis für die Community-Megawads."),
    "doomsw": ("El episodio shareware original de 1993: Knee-Deep in the Dead, nueve niveles.",
               "L'épisode shareware original de 1993 : Knee-Deep in the Dead, neuf niveaux.",
               "Die originale Shareware-Episode von 1993: Knee-Deep in the Dead, neun Level."),
    "scythe": ("32 mapas cortos y rápidos que acaban en un tercer episodio brutal. Se juega sobre Freedoom 2.",
               "32 cartes courtes et rapides jusqu'à un troisième épisode brutal. Se joue sur Freedoom 2.",
               "32 kurze, schnelle Karten bis zu einer brutalen dritten Episode. Läuft auf Freedoom 2."),
    "mementomori": ("El clásico de 1995: 32 niveles de 21 grandes autores de la época, con su propia banda sonora.",
                    "Le classique de 1995 : 32 niveaux de 21 grands auteurs de l'époque, avec sa bande-son.",
                    "Der Klassiker von 1995: 32 Level von 21 Top-Mappern der Zeit, mit eigenem Soundtrack."),
    "zone300": ("32 mapas compactos de 300 líneas como máximo, con banda sonora original.",
                "32 cartes compactes de 300 lignes au plus, avec une bande-son originale.",
                "32 kompakte Karten mit höchstens 300 Linien, mit eigenem Soundtrack."),
    "dtwid": ("Tres episodios nuevos como si id Software los hubiera hecho en 1993. Se juega sobre Freedoom 1.",
              "Trois nouveaux épisodes comme si id Software les avait faits en 1993. Se joue sur Freedoom 1.",
              "Drei neue Episoden, als hätte id Software sie 1993 gebaut. Läuft auf Freedoom 1."),
    "sigil": ("El quinto episodio no oficial de John Romero, adaptado a los límites vanilla. Sustituye al episodio 3.",
              "Le cinquième épisode non officiel de John Romero, adapté aux limites vanilla. Remplace l'épisode 3.",
              "John Romeros inoffizielle fünfte Episode, an die Vanilla-Grenzen angepasst. Ersetzt Episode 3."),
    "plutonia2": ("Secuela no oficial de Final Doom: Plutonia. 32 mapas muy difíciles.",
                  "Suite non officielle de Final Doom : Plutonia. 32 cartes très difficiles.",
                  "Inoffizielle Fortsetzung von Final Doom: Plutonia. 32 sehr schwere Karten."),
    "d2reload": ("La historia de Doom II contada de nuevo en 32 mapas lineales.",
                 "L'histoire de Doom II racontée à nouveau en 32 cartes linéaires.",
                 "Die Geschichte von Doom II neu erzählt in 32 linearen Karten."),
    "thousandlines2": ("Proyecto comunitario de 34 mapas, cada uno con menos de 1000 líneas y texturas propias.",
                       "Projet communautaire de 34 cartes, chacune sous 1000 lignes, avec ses propres textures.",
                       "Community-Projekt mit 34 Karten unter je 1000 Linien, mit eigenen Texturen."),
    'udtwid': ('Un nuevo cuarto episodio como si id Software lo hubiera hecho en 1995. Se juega sobre Freedoom 1.',
        "Un nouveau quatrième épisode comme si id Software l'avait fait en 1995. Se joue sur Freedoom 1.",
        'Eine neue vierte Episode, als hätte id Software sie 1995 gebaut. Läuft auf Freedoom 1.'),
    'dtwidle': ('Cuatro episodios más de mapas al estilo clásico de 1993. Se juega sobre Freedoom 1.',
        'Quatre épisodes de plus de cartes dans le style classique de 1993. Se joue sur Freedoom 1.',
        'Vier weitere Episoden mit Karten im klassischen Stil von 1993. Läuft auf Freedoom 1.'),
    'neis': ('Cuatro grandes episodios para Ultimate Doom, ganador de un Cacoward 2016.',
        "Quatre grands épisodes pour Ultimate Doom, lauréat d'un Cacoward 2016.",
        'Vier große Episoden für Ultimate Doom, Cacoward-Gewinner 2016.'),
    'dbimpact': ('Un nuevo primer episodio, pulido y exigente, en lugar de Knee-Deep in the Dead.',
        'Un nouveau premier épisode, soigné et exigeant, à la place de Knee-Deep in the Dead.',
        'Eine neue, ausgefeilte und schwere erste Episode anstelle von Knee-Deep in the Dead.'),
    'bganymede': ('Tres episodios vanilla en una base de Ganímedes, la luna de Júpiter.',
        'Trois épisodes vanilla sur une base de Ganymède, la lune de Jupiter.',
        'Drei Vanilla-Episoden auf einer Basis auf dem Jupitermond Ganymed.'),
    'lunar': ('36 mapas de estilo clásico en cuatro episodios en la Luna.',
        '36 cartes de style classique en quatre épisodes sur la Lune.',
        '36 Karten im klassischen Stil in vier Episoden auf dem Mond.'),
    'deadlystd': ('Un primer episodio comunitario construido con reglas de diseño estrictas.',
        'Un premier épisode communautaire construit avec des règles de design strictes.',
        'Eine erste Community-Episode nach strengen Designregeln.'),
    'doom404': ('32 mapas cortos protagonizados por un marine del servicio técnico.',
        '32 cartes courtes avec un marine du support informatique.',
        '32 kurze Karten mit einem Marine vom IT-Helpdesk.'),
    'hr2': ('La secuela muy difícil de Hell Revealed: 32 mapas.',
        'La suite très difficile de Hell Revealed : 32 cartes.',
        'Die sehr schwere Fortsetzung von Hell Revealed: 32 Karten.'),
    'darken2': ('12 mapas para un jugador y 12 de deathmatch, con texturas nuevas.',
        '12 cartes solo et 12 de deathmatch, avec de nouvelles textures.',
        '12 Einzelspieler- und 12 Deathmatch-Karten mit neuen Texturen.'),
    'sinseven': ('Siete mapas vanilla compactos.',
        'Sept cartes vanilla compactes.',
        'Sieben kompakte Vanilla-Karten.'),
    'mutiny': ('16 mapas retro en una ciudad tomada por demonios.',
        '16 cartes rétro dans une ville envahie par les démons.',
        '16 Retro-Karten in einer von Dämonen übernommenen Stadt.'),
    'unholyrealms': ('32 mapas de un solo autor, de bases técnicas al infierno.',
        "32 cartes d'un seul auteur, des bases techniques à l'enfer.",
        '32 Karten eines einzigen Autors, von Techbasen bis zur Hölle.'),
    'nova': ('Un megawad comunitario hecho solo con las texturas de Doom II.',
        'Un mégawad communautaire fait uniquement avec les textures de Doom II.',
        'Ein Community-Megawad nur mit den Original-Texturen von Doom II.'),
    'd2twid': ('Un Doom II completamente nuevo al estilo original de id Software de 1994.',
        "Un tout nouveau Doom II dans le style original d'id Software de 1994.",
        'Ein komplett neues Doom II im Originalstil von id Software von 1994.'),
    'scythe2': ('La secuela clásica de Erik Alm: mapas rápidos que crecen hasta batallas enormes, con un nuevo enemigo.',
        "La suite classique d'Erik Alm : des cartes rapides jusqu'à d'énormes batailles, avec un nouvel ennemi.",
        'Erik Alms klassische Fortsetzung: schnelle Karten bis zu riesigen Schlachten, mit neuem Gegner.'),
    'deathless': ('36 mapas para Ultimate Doom, todos creados en noviembre de 2018.',
        '36 cartes pour Ultimate Doom, toutes créées en novembre 2018.',
        '36 Karten für Ultimate Doom, alle im November 2018 entstanden.'),
    'cyberdreams': ('Un megawad ciberpunk de 1998 con monstruos rehechos.',
        'Un mégawad cyberpunk de 1998 avec des monstres retravaillés.',
        'Ein Cyberpunk-Megawad von 1998 mit überarbeiteten Monstern.'),
    'rowdyrudy2': ('20 mapas vanilla contundentes con armas y monstruos nuevos.',
        '20 cartes vanilla percutantes avec armes et monstres nouveaux.',
        '20 knackige Vanilla-Karten mit neuen Waffen und Monstern.'),
    'thousandlines3': ('32 mapas comunitarios de hasta 1000 líneas, con nuevos monstruos.',
        "32 cartes communautaires d'au plus 1000 lignes, avec de nouveaux monstres.",
        '32 Community-Karten mit höchstens 1000 Linien und neuen Monstern.'),
    'rekkr': ('Una conversión total vikinga: mundo, armas, monstruos y música nuevos.',
        'Une conversion totale viking : monde, armes, monstres et musique inédits.',
        'Eine Wikinger-Totalkonversion: neue Welt, Waffen, Monster und Musik.'),
}

ENGINE = {
    "name": "Doom Engine",
    "en": "The classic Doom engine (doomgeneric, from Chocolate Doom) tuned for this board: 4:3 picture, "
          "OPL music, keyboard, mouse and gamepads. Runs the Doom games from the Store and your own WADs.",
    "it": "Il motore classico di Doom (doomgeneric, da Chocolate Doom) su misura per questa scheda: immagine 4:3, "
          "musica OPL, tastiera, mouse e joypad. Fa girare i giochi Doom dello Store e i tuoi WAD.",
    "es": "El motor clásico de Doom (doomgeneric, de Chocolate Doom) ajustado a esta placa: imagen 4:3, música OPL, "
          "teclado, ratón y mandos. Ejecuta los juegos Doom de la Store y tus propios WAD.",
    "fr": "Le moteur classique de Doom (doomgeneric, issu de Chocolate Doom) adapté à cette carte : image 4:3, "
          "musique OPL, clavier, souris et manettes. Lance les jeux Doom du Store et vos propres WAD.",
    "de": "Die klassische Doom-Engine (doomgeneric, aus Chocolate Doom) für dieses Board: 4:3-Bild, OPL-Musik, "
          "Tastatur, Maus und Gamepads. Startet die Doom-Spiele aus dem Store und eigene WADs.",
}


def guide(title, lang, intro, credits, first=True):
    """GUIDE.md (it) / GUIDE.en.md (en): intro, first start, the shared controls page, credits."""
    body = open(os.path.join(HERE, f"guide_{lang}.md"), encoding="utf-8").read().strip()
    if lang == "it":
        head, cred = "## A cosa serve", "## Crediti e licenza"
        start = ("## Primo avvio\n\nIl primo avvio scarica i file del gioco dallo Store (serve il Wi-Fi) con una "
                 "barra di avanzamento; se si interrompe, riaprendo il gioco riprende da dove era rimasto. Dopo "
                 "funziona anche senza rete.\n\n")
    else:
        head, cred = "## What it is", "## Credits and license"
        start = ("## First start\n\nThe first start downloads the game files from the Store (Wi-Fi needed) with a "
                 "progress bar; if it stops, opening the game again resumes it. After that it also works "
                 "offline.\n\n")
    return f"# {title}\n\n{head}\n\n{intro}\n\n{start if first else ''}{body}\n\n{cred}\n\n{credits}\n"


def write(path, text):
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        f.write(text)


def read_file(name):
    src, member = FILES[name]
    path = os.path.join(SRC, src)
    if member is None:
        return open(path, "rb").read()
    with zipfile.ZipFile(path) as z:
        return z.read(member)


def data(out):
    os.makedirs(out, exist_ok=True)
    meta = {}
    for name in FILES:
        b = read_file(name)
        dst = os.path.join(out, name)
        if not (os.path.isfile(dst) and open(dst, "rb").read() == b):
            open(dst, "wb").write(b)
        meta[name] = (len(b), hashlib.sha1(b).hexdigest())
    for gid, title, iwad, pwads, texts, *_ in GAMES:
        lines = [f"title {title}"]
        dehs = DEH.get(gid, [])
        for f in [iwad] + pwads + dehs + texts:
            size, sha = meta[f]
            lines.append(f"file {f} {size} {sha}")
        lines.append(f"iwad {iwad}")
        lines += [f"pwad {p}" for p in pwads]
        lines += [f"deh {d}" for d in dehs]
        open(os.path.join(out, gid + ".game"), "w", newline="\n").write("\n".join(lines) + "\n")
    total = sum(s for s, _ in meta.values())
    print(f"{len(FILES)} files ({total / 1e6:.1f} MB) + {len(GAMES)} descriptors in {out}")


def packages():
    for gid, title, iwad, pwads, texts, author, lic, source, en, it in GAMES:
        d = os.path.join(ROOT, "apps", gid)
        os.makedirs(d, exist_ok=True)
        man = {
            "id": gid, "name": title, "version": GAME_VERSION, "entry": "run", "abi": 14,
            "engine": "doom", "requires": {"doom": ENGINE_VERSION, "wasi": WASI_VERSION},
            "ram_budget": 12582912, "stack_kb": 64, "timeout_ms": 120000,
            "permissions": ["gfx", "fs", "home", "net", "log"],
            "canvas_w": 320, "canvas_h": 240, "canvas_scale": "fit",
            "category": "games", "author": author, "license": lic, "source": source,
            "description": en, "descriptions": {"en": en, "it": it},
        }
        write(os.path.join(d, "manifest.json"), json.dumps(man, ensure_ascii=False, indent=2) + "\n")
        texts_s = ", ".join(texts)
        cred_it = (f"{title}: {author}. Licenza: {lic}. Fonte: {source}. I file sono ridistribuiti invariati "
                   f"insieme al loro file di testo ({texts_s}), che trovi anche in `/home/doom`.")
        cred_en = (f"{title}: {author}. License: {lic}. Source: {source}. The files are redistributed unchanged "
                   f"together with their text file ({texts_s}), which you also find in `/home/doom`.")
        if pwads and iwad.startswith("freedoom"):
            cred_it += f" Gira sopra Freedoom ({iwad}, licenza BSD), scaricato una volta sola per tutti i giochi."
            cred_en += f" It runs on top of Freedoom ({iwad}, BSD license), downloaded once for every game."
        write(os.path.join(d, "GUIDE.md"), guide(title, "it", it, cred_it))
        write(os.path.join(d, "GUIDE.en.md"), guide(title, "en", en, cred_en))
    # the engine itself
    d = os.path.join(ROOT, "apps", "doom")
    os.makedirs(d, exist_ok=True)
    man = {
        "id": "doom", "name": ENGINE["name"], "version": ENGINE_VERSION + ".4", "entry": "run", "abi": 14,
        "requires": {"wasi": WASI_VERSION},
        "ram_budget": 12582912, "stack_kb": 64, "timeout_ms": 120000,
        "permissions": ["gfx", "fs", "home", "net", "log"],
        "canvas_w": 320, "canvas_h": 240, "canvas_scale": "fit",
        "category": "games", "author": "NucleoOS (doomgeneric, Chocolate Doom, emu8950)",
        "license": "GPL-2.0", "source": "https://github.com/indecenti/NucleoOS-P4/tree/main/ports/doom",
        "description": ENGINE["en"], "descriptions": {"en": ENGINE["en"], "it": ENGINE["it"]},
    }
    write(os.path.join(d, "manifest.json"), json.dumps(man, ensure_ascii=False, indent=2) + "\n")
    cred_it = ("Motore: doomgeneric (ozkl) e Chocolate Doom (Simon Howard e altri), GPL-2.0; musica OPL con "
               "emu8950 (Mitsutaka Okazaki, Graham Sanderson), MIT. Doom è un marchio di id Software: "
               "questa app non contiene dati del gioco originale.")
    cred_en = ("Engine: doomgeneric (ozkl) and Chocolate Doom (Simon Howard and others), GPL-2.0; OPL music via "
               "emu8950 (Mitsutaka Okazaki, Graham Sanderson), MIT. Doom is a trademark of id Software: this "
               "app contains no data from the original game.")
    intro_it = (ENGINE["it"] + " Aprila per scegliere tra i WAD presenti in `/home/doom`: i giochi Doom dello "
                "Store (Freedoom, DOOM Shareware, megawad della comunità) si installano e si aprono come app a sé.")
    intro_en = (ENGINE["en"] + " Open it to pick one of the WADs in `/home/doom`: the Doom games in the Store "
                "(Freedoom, DOOM Shareware, community megawads) install and open as apps of their own.")
    write(os.path.join(d, "GUIDE.md"), guide(ENGINE["name"], "it", intro_it, cred_it, first=False))
    write(os.path.join(d, "GUIDE.en.md"), guide(ENGINE["name"], "en", intro_en, cred_en, first=False))
    print(f"{len(GAMES)} game packages + the engine in apps/")


def catalog():
    """Add the store overlay entries (catalog.json) that are missing, keeping its hand formatting."""
    p = os.path.join(ROOT, "server", "appstore", "catalog.json")
    s = open(p, encoding="utf-8").read()
    have = json.loads(s)["apps"]
    rows = [("doom", ENGINE["name"], ENGINE["en"], ENGINE["it"], ENGINE["es"], ENGINE["fr"], ENGINE["de"])]
    rows += [(g[0], g[1], g[8], g[9]) + MORE[g[0]] for g in GAMES]

    def j(v):
        return json.dumps(v, ensure_ascii=False)

    block = ""
    for gid, name, en, it, es, fr, de in rows:
        if gid in have:
            continue
        names = ", ".join(f'"{lang}": {j(name)}' for lang in ("en", "it", "es", "fr", "de"))
        featured = "true" if gid in ("doom", "freedoom1") else "false"
        block += (f'    "{gid}": {{\n'
                  f'      "category": "games", "featured": {featured}, "rating": 4.8, "regions": ["*"],\n'
                  f'      "names": {{ {names} }},\n'
                  f'      "descriptions": {{\n'
                  f'        "en": {j(en)},\n        "it": {j(it)},\n        "es": {j(es)},\n'
                  f'        "fr": {j(fr)},\n        "de": {j(de)}\n      }}\n    }},\n')
    if not block:
        print("catalog: nothing to add")
        return
    anchor = '    "bench":'
    assert s.count(anchor) == 1
    s = s.replace(anchor, block + anchor)
    json.loads(s)
    with open(p, "w", encoding="utf-8", newline="") as f:
        f.write(s)
    print("catalog: entries added")


if __name__ == "__main__":
    if len(sys.argv) >= 3 and sys.argv[1] == "data":
        data(sys.argv[2])
    elif len(sys.argv) >= 2 and sys.argv[1] == "packages":
        packages()
    elif len(sys.argv) >= 2 and sys.argv[1] == "catalog":
        catalog()
    else:
        sys.exit(__doc__)
