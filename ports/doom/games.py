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
}

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
]

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
        for f in [iwad] + pwads + texts:
            size, sha = meta[f]
            lines.append(f"file {f} {size} {sha}")
        lines.append(f"iwad {iwad}")
        lines += [f"pwad {p}" for p in pwads]
        open(os.path.join(out, gid + ".game"), "w", newline="\n").write("\n".join(lines) + "\n")
    total = sum(s for s, _ in meta.values())
    print(f"{len(FILES)} files ({total / 1e6:.1f} MB) + {len(GAMES)} descriptors in {out}")


def packages():
    for gid, title, iwad, pwads, texts, author, lic, source, en, it in GAMES:
        d = os.path.join(ROOT, "apps", gid)
        os.makedirs(d, exist_ok=True)
        man = {
            "id": gid, "name": title, "version": "1.0.0", "entry": "run", "abi": 14,
            "engine": "doom", "requires": {"doom": ENGINE_VERSION},
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
        "id": "doom", "name": ENGINE["name"], "version": ENGINE_VERSION + ".0", "entry": "run", "abi": 14,
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
