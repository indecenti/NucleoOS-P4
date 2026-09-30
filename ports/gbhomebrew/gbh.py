"""gbh.py — helpers of ports/gbhomebrew/build.sh (the apps/gbh-* Game Boy homebrew apps).

    python gbh.py ids                          selected app ids (games.json)
    python gbh.py header <id> <rom> <out.h>    ROM -> C header (gb_rom_data[], GB_ROM_SIZE, GB_TITLE)
    python gbh.py script <id> | early <id>     harness input script / early-frame count
    python gbh.py check <id> <testdir>         harness verdict: crash, emulator error, blank, CGB-only
    python gbh.py icon <id> <icon.z> <testdir> 80x80 rounded tile from the first screenshot
    python gbh.py shots <id> <dir> <testdir>   store screenshots 1..3.jpg (512x300, baseline q85)
    python gbh.py meta <id> <appdir>           manifest.json, GUIDE.md, GUIDE.en.md
    python gbh.py catalog <out.json>           store overlay entries for server/appstore/catalog.json

Data: games.json (texts, credits) + ports/_src/gbhomebrew (fetch.sh: ROMs, database entries).
"""
import json
import os
import sys
import zlib

from PIL import Image, ImageDraw

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, "..", ".."))
SRC = os.path.join(ROOT, "ports", "_src", "gbhomebrew")
GAMES = json.load(open(os.path.join(HERE, "games.json"), encoding="utf-8"))
BY_ID = {g["id"]: g for g in GAMES}
LICENSES = json.load(open(os.path.join(HERE, "licenses.json"), encoding="utf-8"))

# harness default: press START / A a few times to leave title screens, then move around
DEFAULT_SCRIPT = ("150-156:start,300-306:a,420-426:start,540-546:a,660-666:start,800-806:a,"
                  "900-960:right,1000-1006:a,1100-1160:left,1200-1206:a,1300-1330:up")

SHOT_W, SHOT_H, SHOT_BG = 512, 300, (0x10, 0x14, 0x18)


def game(gid):
    return BY_ID[gid]


def rom_bytes(gid):
    return open(os.path.join(SRC, "roms", gid + ".gb"), "rb").read()


def db_entry(gid):
    return json.load(open(os.path.join(SRC, "db", gid, "game.json"), encoding="utf-8"))


# ---- harness ------------------------------------------------------------------------------------
def game_frame(ppm):
    """The 160x144 picture out of a 1024x600 harness canvas (same logic as make_gb_icon.py)."""
    im = Image.open(ppm).convert("RGB")
    for scale in (4, 3):
        gw, gh = 160 * scale, 144 * scale
        gx, gy = (1024 - gw) // 2, (600 - gh) // 2
        crop = im.crop((gx, gy, gx + gw, gy + gh))
        if im.getpixel((gx - 4, gy + gh // 2)) != im.getpixel((gx + 2, gy + gh // 2)):
            return crop.resize((160, 144), Image.NEAREST)
    return im.crop((192, 12, 832, 588)).resize((160, 144), Image.NEAREST)


def check(gid, testdir):
    rom = rom_bytes(gid)
    problems = []
    if rom[0x143] == 0xC0:
        problems.append("CGB-only header (0x143 = C0)")
    for name in (gid, gid + "_a"):
        log = open(os.path.join(testdir, name + ".log"), encoding="utf-8", errors="replace").read()
        if "presents" not in log:
            problems.append(f"{name}: harness did not finish")
        for line in log.splitlines():
            if "EMU ERROR" in line or "NOT SUPPORTED" in line or "CHECKSUM" in line:
                problems.append(f"{name}: {line.strip()}")
                break
    colors = [len(game_frame(os.path.join(testdir, n + ".ppm")).getcolors(1 << 16) or [])
              for n in (gid, gid + "_a")]
    if max(colors) <= 1:
        problems.append("blank screen in both frames")
    verdict = "FAIL " + "; ".join(problems) if problems else "OK"
    print(f"  {gid}: {verdict} (colours {colors[0]}/{colors[1]}, header 0x143={rom[0x143]:02x}, "
          f"cart {rom[0x147]:02x}, {len(rom) // 1024} KB)")


# ---- pictures -----------------------------------------------------------------------------------
def db_shots(gid):
    """Database screenshots that are real 160x144 game frames (or integer/near multiples of it);
    promo covers are skipped, and so are Game Boy Color captures (the emulator is DMG-only):
    names containing "cgb" and the ones listed in games.json "skip_shots"."""
    g = game(gid)
    out = []
    for s in db_entry(gid).get("screenshots", []):
        if s in g.get("skip_shots", []) or "cgb" in s.lower():
            continue
        p = os.path.join(SRC, "db", gid, s)
        try:
            im = Image.open(p).convert("RGB")
        except OSError:
            continue
        w, h = im.size
        if w >= 160 and abs(w / h - 160 / 144) < 0.02:
            out.append(im.resize((160, 144), Image.NEAREST) if (w, h) != (160, 144) else im)
    return out


def pictures(gid, testdir):
    """Store pictures in order: database screenshots, then harness frames to fill up to three."""
    pics = db_shots(gid)
    for n in (gid, gid + "_a"):
        if len(pics) >= 3:
            break
        p = os.path.join(testdir, n + ".ppm")
        if os.path.exists(p):
            f = game_frame(p)
            if len(f.getcolors(1 << 16) or []) > 1:
                pics.append(f)
    return pics[:3]


def icon(gid, out, testdir):
    g = game(gid)
    pics = pictures(gid, testdir)
    pic = pics[g.get("icon_shot", 0)] if pics else Image.new("RGB", (160, 144), (155, 188, 15))
    # square crop around the chosen point (default: centre), scaled into the rounded tile
    cx = g.get("icon_cx", 80)
    x0 = max(0, min(160 - 144, cx - 72))
    sq = pic.crop((x0, 0, x0 + 144, 144))
    s = 4
    size = 80
    big = sq.resize((76 * s, 76 * s), Image.NEAREST).convert("RGBA")
    mask = Image.new("L", (size * s, size * s), 0)
    ImageDraw.Draw(mask).rounded_rectangle((2 * s, 2 * s, (size - 2) * s, (size - 2) * s), radius=18 * s, fill=255)
    img = Image.new("RGBA", (size * s, size * s), (0, 0, 0, 0))
    img.paste(big, (2 * s, 2 * s))
    img.putalpha(mask)
    img = img.resize((size, size), Image.LANCZOS)
    raw = img.tobytes("raw", "BGRA")                 # LVGL ARGB8888, raw deflate: ports/make_icon.py
    co = zlib.compressobj(9, zlib.DEFLATED, -15)
    open(out, "wb").write(co.compress(raw) + co.flush())
    img.save(os.path.join(testdir, gid + "_icon.png"))   # preview, next to the harness dumps


def shots(gid, outdir, testdir):
    os.makedirs(outdir, exist_ok=True)
    for f in os.listdir(outdir):
        if f.endswith(".jpg"):
            os.remove(os.path.join(outdir, f))
    pics = pictures(gid, testdir)
    for i, pic in enumerate(pics, 1):
        k = min(SHOT_W // 160, SHOT_H // 144)                # nearest-neighbour, integer scale (2x)
        up = pic.resize((160 * k, 144 * k), Image.NEAREST)
        canvas = Image.new("RGB", (SHOT_W, SHOT_H), SHOT_BG)
        canvas.paste(up, ((SHOT_W - up.width) // 2, (SHOT_H - up.height) // 2))
        canvas.save(os.path.join(outdir, f"{i}.jpg"), "JPEG", quality=85, progressive=False, optimize=True)
    print(f"  shots {len(pics)}")


# ---- texts --------------------------------------------------------------------------------------
PEANUT = ("Emulator: Peanut-GB by Mahyar Koshkouei, MiniGB APU sound by Alex Baines and Mahyar "
          "Koshkouei (MIT license).")
PEANUT_IT = ("Emulatore: Peanut-GB di Mahyar Koshkouei, audio MiniGB APU di Alex Baines e Mahyar "
             "Koshkouei (licenza MIT).")


def ram_budget(n):
    return 2097152 if n <= 65536 else 3145728 if n <= 262144 else 4194304


def manifest(gid):
    g = game(gid)
    m = {
        "id": gid,
        "name": g["title"],
        "version": "1.0.0",
        "entry": "run",
        "abi": 11,
        "ram_budget": ram_budget(len(rom_bytes(gid))),
        "stack_kb": 64,
        "timeout_ms": 120000,
        "permissions": ["gfx", "fs", "log"],
        "canvas_w": 1024,
        "canvas_h": 600,
        "system_gestures": False,
        "category": "games",
        "author": g["author"] + "; Peanut-GB by Mahyar Koshkouei",
        "license": g["license"] + " (game); MIT (emulator)",
        "source": g["source"],
        "description": g["desc_en"],
        "descriptions": {"en": g["desc_en"], "it": g["desc_it"]},
    }
    return m


GUIDE = {
    "en": {
        "controls": "Controls",
        "touch": ("- **Touch**: D-pad on the left, **A** and **B** on the right (you can slide your thumb "
                  "from one to the other), **SELECT** bottom left, **START** bottom right. Several fingers "
                  "at once work.\n"
                  "- USB or Bluetooth **gamepad**: D-pad or left stick; A and Y = A; B and X = B; Start; "
                  "Back/View = Select.\n"
                  "- USB **keyboard**: arrows or WASD; Space/X/Enter = A; Z/C/Backspace = B; P or Tab = "
                  "Start; Esc = Select.\n"
                  "- **MENU** (top left), the L/R shoulder buttons or the pad's Guide button pause the game."),
        "ingame": "In the game",
        "menu": ("## The pause menu\n\n**Resume**, **Reset** (power-cycles the console), **Zoom** 4x or 3x "
                 "(smaller, more room for the controls), **Colours** (green, grey, Pocket, Game Boy Color "
                 "style) and **Exit** closes the game. Zoom and colours are remembered. The system back "
                 "gesture opens the menu; twice, it exits."),
        "save": ("## Saves\n\nThe cartridge has a battery: its save data stays on the device between sessions."),
        "sound": ("## Sound\n\nSound is emulated (the Game Boy's four channels). If another app holds the "
                  "speaker (the Music app, say), the game starts muted and retries every few seconds."),
        "credits": "Credits and licenses",
        "rom": ("The ROM is the original, unmodified, from the Homebrew Hub database "
                "(https://hh.gbdev.io/game/{slug})."),
        "src": "Source code and license",
        "peanut": PEANUT,
        "texts": "License texts:",
    },
    "it": {
        "controls": "Comandi",
        "touch": ("- **Touch**: croce direzionale a sinistra, **A** e **B** a destra (puoi far scivolare il "
                  "pollice dall'uno all'altro), **SELECT** in basso a sinistra, **START** in basso a destra. "
                  "Più dita insieme funzionano.\n"
                  "- **Gamepad** USB o Bluetooth: croce o levetta sinistra; A e Y = A; B e X = B; Start; "
                  "Back/View = Select.\n"
                  "- **Tastiera USB**: frecce o WASD; Spazio/X/Invio = A; Z/C/Backspace = B; P o Tab = "
                  "Start; Esc = Select.\n"
                  "- **MENU** (in alto a sinistra), i dorsali L/R o il tasto Guide del gamepad mettono in pausa."),
        "ingame": "Nel gioco",
        "menu": ("## Il menu di pausa\n\n**Continua**, **Ricomincia** (spegne e riaccende la console), **Zoom** "
                 "4x o 3x (più piccolo, lascia più spazio ai comandi), **Colori** (verde, grigio, Pocket, "
                 "colori stile Game Boy Color) e **Esci** chiude il gioco. Zoom e colori restano memorizzati. "
                 "Il gesto indietro del sistema apre il menu; ripetuto, esce."),
        "save": ("## Salvataggi\n\nLa cartuccia ha la batteria: i dati salvati restano sul dispositivo tra una "
                 "partita e l'altra."),
        "sound": ("## Audio\n\nIl suono è emulato (i quattro canali del Game Boy). Se un'altra app sta usando "
                  "l'altoparlante (per esempio Musica), il gioco parte muto e riprova ogni pochi secondi."),
        "credits": "Crediti e licenze",
        "rom": ("La ROM è quella originale, non modificata, dal database Homebrew Hub "
                "(https://hh.gbdev.io/game/{slug})."),
        "src": "Codice sorgente e licenza",
        "peanut": PEANUT_IT,
        "texts": "Testi delle licenze:",
    },
}

BATTERY_CARTS = {0x03, 0x06, 0x09, 0x0D, 0x0F, 0x10, 0x13, 0x1B, 0x1E, 0x22, 0xFF}


def guide(gid, lang):
    g = game(gid)
    t = GUIDE[lang]
    rom = rom_bytes(gid)
    out = [f"# {g['title']}", "", g["guide_" + lang], "", f"## {t['controls']}", "", t["touch"], ""]
    if g.get("controls_" + lang):
        out += [f"### {t['ingame']}", "", g["controls_" + lang], ""]
    out += [t["menu"], ""]
    if rom[0x147] in BATTERY_CARTS:
        out += [t["save"], ""]
    out += [t["sound"], "", f"## {t['credits']}", "", g["credits_" + lang], "",
            t["rom"].format(slug=g["slug"]), "", t["peanut"], "", t["texts"], ""]
    for lic in g["license_texts"]:
        L = LICENSES[lic["key"]]
        out += [f"### {lic['title']}", ""]
        if lic.get("url"):
            out += [lic["url"], ""]
        if lic.get("note_" + lang):
            out += [lic["note_" + lang], ""]
        if "text" in L:
            body = L["text"].replace("{copyright}", lic.get("copyright", ""))
            out += ["```", body.rstrip(), "```", ""]
        else:
            out += [L["link_" + lang], ""]
    for key, title, url in (("mit_peanut", "Peanut-GB", "https://github.com/deltabeard/Peanut-GB"),
                            ("mit_apu", "MiniGB APU",
                             "https://github.com/deltabeard/Peanut-GB/tree/master/examples/sdl2/minigb_apu")):
        out += [f"### {title}", "", url, "", "```", LICENSES[key]["text"].rstrip(), "```", ""]
    return "\n".join(out).rstrip() + "\n"


def meta(gid, appdir):
    text = json.dumps(manifest(gid), indent=2, ensure_ascii=False)   # layout of apps/gbtobu/manifest.json
    text = text.replace('"permissions": [\n    "gfx",\n    "fs",\n    "log"\n  ]', '"permissions": ["gfx", "fs", "log"]')
    open(os.path.join(appdir, "manifest.json"), "w", encoding="utf-8", newline="\n").write(text + "\n")
    open(os.path.join(appdir, "GUIDE.md"), "w", encoding="utf-8", newline="\n").write(guide(gid, "it"))
    open(os.path.join(appdir, "GUIDE.en.md"), "w", encoding="utf-8", newline="\n").write(guide(gid, "en"))


def catalog(out):
    entries = {}
    for g in GAMES:
        entries[g["id"]] = {
            "category": "games", "featured": False, "rating": 4.5, "regions": ["*"],
            "names": {"en": g["title"], "it": g.get("title_it", g["title"])},
            "descriptions": {"en": g["desc_en"], "it": g["desc_it"]},
        }
    json.dump(entries, open(out, "w", encoding="utf-8", newline="\n"), indent=2, ensure_ascii=False)
    with open(out, "a", encoding="utf-8", newline="\n") as f:
        f.write("\n")
    print(f"  {out}: {len(entries)} entries")


def header(gid, rom, out):
    b = open(rom, "rb").read()
    title = game(gid)["title"].upper().encode("ascii", "replace").decode().replace('"', "'")
    with open(out, "w", newline="\n") as f:
        f.write("#include <stdint.h>\n")
        f.write(f'#define GB_TITLE "{title}"\n#define GB_ROM_SIZE {len(b)}u\n')
        f.write(f"static const uint8_t gb_rom_data[{len(b)}] = {{")
        for i in range(0, len(b), 32):
            f.write(",".join(str(x) for x in b[i:i + 32]) + ",\n")
        f.write("};\n")


def main():
    cmd, args = sys.argv[1], sys.argv[2:]
    if cmd == "ids":
        print("\n".join(g["id"] for g in GAMES))
    elif cmd == "script":
        print(game(args[0]).get("script", DEFAULT_SCRIPT))
    elif cmd == "early":
        print(game(args[0]).get("early", 400))
    elif cmd == "header":
        header(*args)
    elif cmd == "check":
        check(*args)
    elif cmd == "icon":
        icon(*args)
    elif cmd == "shots":
        shots(*args)
    elif cmd == "meta":
        meta(*args)
    elif cmd == "catalog":
        catalog(*args)
    else:
        sys.exit(f"unknown command {cmd}")


if __name__ == "__main__":
    main()
