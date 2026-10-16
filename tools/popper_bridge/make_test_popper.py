#!/usr/bin/env python3
"""make_test_popper — a fake PinUP Popper install, to test nucleo_bridge and the board's menu without a
cabinet: PUPDatabase.db with the tables and columns the bridge reads (as Popper has them), wheel and
instruction images, and optionally a fake Popper Web Remote that logs launch requests.

    python make_test_popper.py C:\\temp\\FakePUP
    python make_test_popper.py C:\\temp\\FakePUP --serve 8099      # + a fake Web Remote on port 8099
    python nucleo_bridge.py --popper C:\\temp\\FakePUP --web-port 8099

Needs Pillow.
"""
import argparse
import http.server
import os
import random
import sqlite3
import sys

TABLES = [
    ("Medieval Madness", "Williams", 1997), ("The Addams Family", "Bally", 1992),
    ("Twilight Zone", "Bally", 1993), ("Attack from Mars", "Bally", 1995),
    ("Monster Bash", "Williams", 1998), ("Cirqus Voltaire", "Bally", 1997),
    ("Theatre of Magic", "Bally", 1995), ("Fish Tales", "Williams", 1992),
    ("Scared Stiff", "Bally", 1996), ("Indiana Jones", "Williams", 1993),
    ("Star Trek TNG", "Williams", 1993), ("White Water", "Williams", 1993),
    ("Junk Yard", "Williams", 1996), ("Safe Cracker", "Bally", 1996),
    ("Cactus Canyon", "Bally", 1998), ("No Good Gofers", "Williams", 1997),
    ("Funhouse", "Williams", 1990), ("Black Knight 2000", "Williams", 1989),
    ("Addams Family Values", "Bally", 1993), ("Bride of Pinbot", "Williams", 1991),
    ("Doctor Who", "Bally", 1992), ("Elvira", "Bally", 1989), ("Getaway", "Williams", 1992),
    ("High Speed", "Williams", 1986), ("Judge Dredd", "Bally", 1993), ("Kiss", "Stern", 2015),
    ("Lord of the Rings", "Stern", 2003), ("Metallica", "Stern", 2013), ("Pinbot", "Williams", 1986),
    ("Revenge from Mars", "Bally", 1999), ("Space Shuttle", "Williams", 1984), ("Taxi", "Williams", 1988),
    ("Whirlwind", "Williams", 1990), ("X-Men", "Stern", 2012), ("Zaccaria Farfalla", "Zaccaria", 1983),
    ("1-2-3", "Talleres de Llobregat", 1973),
]


def make(root):
    from PIL import Image, ImageDraw
    os.makedirs(root, exist_ok=True)
    media = os.path.join(root, "POPMedia", "Visual Pinball X")
    for scr in ("Wheel", "GameHelp", "GameInfo"):
        os.makedirs(os.path.join(media, scr), exist_ok=True)
    db = os.path.join(root, "PUPDatabase.db")
    if os.path.exists(db):
        os.remove(db)
    con = sqlite3.connect(db)
    c = con.cursor()
    c.executescript("""
    CREATE TABLE Emulators (EMUID INTEGER PRIMARY KEY, EmuName VARCHAR(100), Description TEXT, DirGames TEXT,
        DirMedia VARCHAR(255), EmuDisplay TEXT, Visible INTEGER DEFAULT 1, LaunchScript TEXT, PostScript TEXT);
    CREATE TABLE Games (GameID INTEGER PRIMARY KEY, EMUID INTEGER, GameName TEXT, GameFileName TEXT,
        GameDisplay TEXT, Visible INTEGER DEFAULT 1, DateAdded DATETIME, DateUpdated DATETIME, GameYear INTEGER,
        Manufact TEXT, Category TEXT, GameRating INTEGER, TAGS TEXT, ROM TEXT, Author TEXT, IPDBNum TEXT);
    CREATE TABLE GamesStats (GameID INTEGER, LastPlayed DATETIME, NumberPlays INTEGER, TimePlayedSecs INTEGER,
        HighScores TEXT);
    CREATE TABLE Playlists (PlayListID INTEGER PRIMARY KEY, PlayName TEXT);
    CREATE TABLE PlayListDetails (PlayListID INTEGER, GameID INTEGER, Visible INTEGER, DisplayOrder INTEGER,
        NumPlayed INTEGER, isFav INTEGER);
    CREATE TABLE GlobalSettings (GlobalMediaDir TEXT, StartupBatch TEXT, SQLVersion INTEGER);
    """)
    c.execute("INSERT INTO Emulators VALUES (1, 'Visual Pinball X', 'VPX', 'C:\\vPinball\\VisualPinball\\Tables', ?, 'Visual Pinball X', 1, '', '')", (media,))
    c.execute("INSERT INTO GlobalSettings VALUES (?, '', 64)", (os.path.join(root, "POPMedia", "Default"),))
    c.execute("INSERT INTO Playlists VALUES (1, 'Home')")
    rnd = random.Random(7)
    for i, (name, maker, year) in enumerate(TABLES, 1):
        file = name.replace(" ", "_") + ".vpx"
        c.execute("INSERT INTO Games (GameID, EMUID, GameName, GameFileName, GameDisplay, Visible, DateAdded, GameYear,"
                  " Manufact, GameRating, ROM, Author) VALUES (?,1,?,?,?,?,?,?,?,?,?,?)",
                  (i, name + " (" + maker + " " + str(year) + ")", file, name, 1 if i != 9 else 2,
                   "2026-%02d-%02d 20:00:00" % (1 + i % 9, 1 + i), year, maker, rnd.randint(0, 5), "rom%d" % i, "VPW"))
        if i % 4:
            c.execute("INSERT INTO GamesStats VALUES (?,?,?,?,?)",
                      (i, "2026-10-%02d 2%d:%02d:00" % (1 + i % 15, i % 4, i), rnd.randint(1, 60), rnd.randint(300, 40000),
                       "GRAND CHAMPION\nNIC 1.250.000.000\nHIGH SCORE #1\nCLD 980.000.000\nHIGH SCORE #2\nANN 640.000.000"))
        if i in (1, 3, 6, 10):
            c.execute("INSERT INTO PlayListDetails VALUES (1, ?, 1, ?, 0, ?)", (i, i, 2 if i == 1 else 1))
        stem = name + " (" + maker + " " + str(year) + ")"
        hue = (i * 47) % 360
        col = tuple(int(v) for v in __import__("colorsys").hsv_to_rgb(hue / 360, 0.8, 255))
        w = Image.new("RGBA", (400, 400), (0, 0, 0, 0))
        d = ImageDraw.Draw(w)
        d.ellipse((10, 10, 390, 390), fill=col + (255,))
        d.multiline_text((200, 200), "\n".join(name.split()), fill=(255, 255, 255, 255), anchor="mm", align="center",
                         font_size=48)
        w.save(os.path.join(media, "Wheel", stem + ".png"))
        if i % 2:
            hp = Image.new("RGB", (1280, 720), (20, 20, 40))
            dh = ImageDraw.Draw(hp)
            dh.text((640, 80), name.upper() + " - RULES", fill=(255, 200, 0), anchor="mm", font_size=56)
            for k in range(8):
                dh.text((80, 180 + k * 60), "%d. Shoot the ramps to light the jackpot, mode %d" % (k + 1, k + 1),
                        fill=(220, 220, 220), font_size=36)
            hp.save(os.path.join(media, "GameHelp", stem + ".jpg"))
    con.commit()
    con.close()
    print("fake Popper in", root, "-", len(TABLES), "tables")


def serve(port):
    class H(http.server.BaseHTTPRequestHandler):
        def do_GET(self):   # noqa: N802
            print("Web Remote:", self.path, flush=True)
            self.send_response(200)
            self.end_headers()
            self.wfile.write(b"ok")

        def log_message(self, *a):
            pass
    print("fake Popper Web Remote on port", port)
    http.server.ThreadingHTTPServer(("127.0.0.1", port), H).serve_forever()


if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("root")
    ap.add_argument("--serve", type=int, help="also run a fake Popper Web Remote on this port")
    a = ap.parse_args()
    make(a.root)
    if a.serve:
        serve(a.serve)
    sys.exit(0)
