#!/usr/bin/env python3
"""nucleo_bridge — PinUP Popper <-> NucleoOS "DMD Flipper" board, over the board's USB port.

The board is the cabinet's DMD (dmdext drives it as a ZeDMD over USB while a table runs). While no table
runs, this bridge owns the same USB port and serves the board's touch menu: lists of tables (recently
played, new, most played, favourites, all), a table's card (stats, high scores), its images (wheel,
instruction card) already sized for the board, and launching a table through Popper.

A Windows COM port has one owner at a time, so the bridge hands the port over around every game:
  * Popper's emulator Launch script calls  http://127.0.0.1:58090/game/start  -> the bridge tells the
    board "a game starts" and closes the port; dmdext opens it and the board shows the DMD;
  * the Close script calls  http://127.0.0.1:58090/game/end  -> the bridge waits for dmdext to let go,
    opens the port again and the board shows the menu.

Messages on the wire: b"NVDM" type(1) len(u32 LE) payload sum(u16 LE, the 16-bit sum of type, len and
payload bytes); JSON payloads, except 'I' (image): u16 LE json_len, JSON, the RGB565 LE pixels zlib-compressed
(fmt "rgb565z", "size" = their raw length). Same
framing as dmd_core.c (dmd_link_*). Board -> bridge: 'Q' requests. Bridge -> board: 'H' hello, 'P' ping,
'L' list, 'T' table, 'I' image, 'G' game state, 'R' launch result, 'E' error.

    python nucleo_bridge.py                      run (finds Popper and the board by themselves)
    python nucleo_bridge.py --check              report what it finds (Popper, database, media, Web
                                                 Remote, USB port) and exit; --report FILE also writes it
    python nucleo_bridge.py --popper D:\\PinUPSystem --port COM7 --web-port 80

Needs Python 3.8+, pyserial and Pillow (pip install pyserial pillow). Nothing is ever written to Popper's
database (it is opened read-only).
"""
import argparse
import http.client
import http.server
import io
import json
import os
import random
import re
import socketserver
import sqlite3
import struct
import subprocess
import sys
import threading
import time
import urllib.parse
import urllib.request
import zlib

VERSION = "1.1"
MAGIC = b"NVDM"
HOOK_PORT = 58090
BOARD_VID_PID = (0x303A, 0x1001)          # Espressif USB-Serial-JTAG: the board's programming port
# Emulators whose DMD reaches the board through dmdext (VPinMAME's DmdDevice.dll): Visual Pinball X. For
# them the bridge hands the USB port over during a game. Every other one (Pinball FX2, MAME, PC games...)
# keeps the bridge on the port: the board shows the game's pictures and its quick actions instead.
ALT_MEDIA = re.compile(r"^\s*\(\d+\)$")   # what may follow a game's name in an alternative media file
DMD_EMUS = {1}
# Processes of a running table, to notice one already running when the bridge starts (a restart mid-game):
# name prefixes, lower case, and whether its DMD goes through dmdext (which then holds the board's port).
GAME_EXES = (("vpinball", True), ("pinball fx", False), ("pinballfx", False), ("future pinball", False),
             ("futurepinball", False), ("mame", False), ("pinball arcade", False))
DEFAULT_POPPER_DIRS = [r"C:\vPinball\PinUPSystem", r"C:\PinUPSystem", r"D:\vPinball\PinUPSystem"]
IMAGE_EXTS = (".png", ".jpg", ".jpeg", ".apng", ".gif", ".bmp")


LOG_FILE = None   # --log: pythonw has no console


def log(*a):
    line = " ".join([time.strftime("%H:%M:%S")] + [str(x) for x in a])
    if LOG_FILE:
        try:
            with open(LOG_FILE, "a", encoding="utf-8") as f:
                f.write(line + "\n")
        except OSError:
            pass
    elif sys.stdout:
        print(line, flush=True)


# ------------------------------------------------------------------------------------------------ framing

def frame(t, payload):
    """One message: the same bytes dmd_link_header / dmd_link_trailer produce on the board."""
    if isinstance(payload, str):
        payload = payload.encode("utf-8")
    head = bytes([ord(t)]) + struct.pack("<I", len(payload))
    s = (sum(head) + sum(payload)) & 0xFFFF
    return MAGIC + head + payload + struct.pack("<H", s)


class Parser:
    """Incremental parser of the board's messages (the bridge ignores everything else on the port)."""

    def __init__(self, cap=1 << 20):
        self.cap = cap
        self.buf = bytearray()

    def feed(self, data):
        self.buf += data
        out = []
        while True:
            i = self.buf.find(MAGIC)
            if i < 0:
                del self.buf[:max(0, len(self.buf) - 3)]   # keep a possible partial magic
                return out
            if i:
                del self.buf[:i]
            if len(self.buf) < 9:
                return out
            t = self.buf[4]
            n = struct.unpack_from("<I", self.buf, 5)[0]
            if n > self.cap:
                del self.buf[:4]
                continue
            if len(self.buf) < 9 + n + 2:
                return out
            payload = bytes(self.buf[9:9 + n])
            want = struct.unpack_from("<H", self.buf, 9 + n)[0]
            if (sum(self.buf[4:9]) + sum(payload)) & 0xFFFF == want:
                out.append((chr(t), payload))
                del self.buf[:9 + n + 2]
            else:
                del self.buf[:4]


# ------------------------------------------------------------------------------------------------ Popper

class Popper:
    """Read-only access to PinUP Popper's database (PUPDatabase.db) and media folders, and launching."""

    LIST_SQL = {
        # columns as PinUP Popper has them (the same ones VPin Studio's PinUPConnector reads)
        "recent": "s.LastPlayed IS NOT NULL ORDER BY s.LastPlayed DESC",
        "new": "1 ORDER BY g.DateAdded DESC",
        "top": "IFNULL(s.NumberPlays, 0) > 0 ORDER BY s.NumberPlays DESC, s.TimePlayedSecs DESC",
        "fav": "g.GameID IN (SELECT GameID FROM PlayListDetails WHERE isFav IN (1, 2)) ORDER BY g.GameDisplay",
        "all": "1 ORDER BY g.GameDisplay COLLATE NOCASE",
    }

    def __init__(self, root, web_port):
        self.root = root
        self.db = os.path.join(root, "PUPDatabase.db")
        self.web_port = web_port
        self.dirs = {}     # media folder -> (mtime, its image files)
        self.shapes = {}   # (file, mtime) -> wider than tall

    def q(self, sql, args=()):
        last = None
        for _ in range(5):   # Popper may be writing (stats at the end of a game): short retries
            try:
                con = sqlite3.connect("file:%s?mode=ro" % urllib.parse.quote(self.db.replace("\\", "/")), uri=True, timeout=2)
                con.row_factory = sqlite3.Row
                try:
                    return [dict(r) for r in con.execute(sql, args).fetchall()]
                finally:
                    con.close()
            except sqlite3.OperationalError as e:
                last = e
                time.sleep(0.2)
        raise last

    BASE = ("SELECT g.GameID, g.GameName, g.GameDisplay, g.GameFileName, g.GameYear, g.Manufact, g.GameRating, "
            "g.DateAdded, g.EMUID, s.LastPlayed, s.NumberPlays, s.TimePlayedSecs "
            "FROM Games g LEFT JOIN GamesStats s ON s.GameID = g.GameID WHERE g.Visible = 1 AND ")

    @staticmethod
    def days_ago(when):
        """Calendar days since a Popper date ("YYYY-MM-DD HH:MM:SS", local time); -1 if none or unreadable."""
        import datetime
        try:
            d = datetime.datetime.strptime((when or "")[:10], "%Y-%m-%d").date()
        except ValueError:
            return -1
        return max(0, (datetime.date.today() - d).days)

    def favourites(self):
        try:
            return {r["GameID"] for r in self.q("SELECT GameID FROM PlayListDetails WHERE isFav IN (1, 2)")}
        except sqlite3.OperationalError:   # no playlists table: no favourites
            return set()

    def item(self, r, favs=None):
        return {
            "id": r["GameID"],
            "name": (r.get("GameDisplay") or r.get("GameName") or "?").strip(),
            "year": r.get("GameYear") or 0,
            "maker": (r.get("Manufact") or "").strip(),
            "plays": r.get("NumberPlays") or 0,
            "secs": r.get("TimePlayedSecs") or 0,
            "last": (r.get("LastPlayed") or "")[:16],
            "ago": self.days_ago(r.get("LastPlayed")),
            "added": (r.get("DateAdded") or "")[:10],
            "rating": r.get("GameRating") or 0,
            "fav": r["GameID"] in (favs if favs is not None else self.favourites()),
            "emu": r.get("EMUID") or 0,
        }

    def tables(self, which, limit):
        cond = self.LIST_SQL.get(which, self.LIST_SQL["all"])
        rows = self.q(self.BASE + cond + " LIMIT ?", (int(limit),))
        favs = self.favourites()
        return [self.item(r, favs) for r in rows]

    def table(self, gid):
        rows = self.q("SELECT g.*, s.LastPlayed, s.NumberPlays, s.TimePlayedSecs, s.HighScores "
                      "FROM Games g LEFT JOIN GamesStats s ON s.GameID = g.GameID WHERE g.GameID = ?", (int(gid),))
        if not rows:
            return None
        r = rows[0]
        t = self.item(r)
        scores = (r.get("HighScores") or "").replace("\r", "").strip()
        t.update({
            "rom": r.get("ROM") or "",
            "author": r.get("Author") or "",
            "category": r.get("Category") or "",
            "scores": scores[:800],
            "wheel": self.media(r, "Wheel") is not None,
            "help": self.media(r, "GameHelp") is not None,
            "info": self.media(r, "GameInfo") is not None,
        })
        return t

    def random_id(self):
        rows = self.q("SELECT GameID FROM Games WHERE Visible = 1")
        return random.choice(rows)["GameID"] if rows else None

    def media_dir(self, emuid):
        rows = self.q("SELECT EmuName, DirMedia FROM Emulators WHERE EMUID = ?", (emuid,))
        if rows and rows[0].get("DirMedia"):
            return rows[0]["DirMedia"]
        name = rows[0]["EmuName"] if rows else "Visual Pinball X"
        return os.path.join(self.root, "POPMedia", name)

    def images_in(self, folder):
        """The image files of a media folder, (lower-case stem, name) sorted; listed again only when the
        folder changes (a MAME folder holds hundreds of files: no listing per request)."""
        try:
            mt = os.stat(folder).st_mtime_ns
        except OSError:
            return []
        hit = self.dirs.get(folder)
        if hit and hit[0] == mt:
            return hit[1]
        files = sorted((os.path.splitext(f)[0].lower(), f) for f in os.listdir(folder) if f.lower().endswith(IMAGE_EXTS))
        self.dirs[folder] = (mt, files)
        return files

    def media(self, r, screen):
        """The image file of a game for a Popper screen folder (Wheel, GameHelp, GameInfo, Backglass, ...)."""
        folder = os.path.join(self.media_dir(r["EMUID"]), screen)
        stem = (r.get("GameName") or "").lower()
        if not stem:
            return None
        best = None
        for base, f in self.images_in(folder):
            if base == stem:
                return os.path.join(folder, f)
            # Popper's alternatives, "Name(1).png" / "Name (2).png"; not any name that only starts the same
            # ("Blade" must not take "Blade Runner.png": another table's picture)
            if best is None and base.startswith(stem) and ALT_MEDIA.match(base[len(stem):]):
                best = os.path.join(folder, f)
        return best

    def landscape(self, path):
        """True for a picture wider than tall (the board shows only those: a portrait manual would be
        unreadable on it). Only the header is read, once per file."""
        try:
            key = (path, os.stat(path).st_mtime_ns)
        except OSError:
            return False
        if key not in self.shapes:
            from PIL import Image
            try:
                with Image.open(path) as im:
                    self.shapes[key] = im.width >= im.height
            except Exception:   # noqa: BLE001
                self.shapes[key] = False
        return self.shapes[key]

    # GameInfo / GameHelp are not in it: on this cabinet every one of them (1358, checked) is a portrait card
    # saved on its side for a rotated screen; upright it is a portrait page, unreadable in a 1024x600 slide.
    # The board shows the instructions on their own, upright and scrollable (image(..., rot=90)).
    SHOWCASE = ("BackGlass", "DMD", "Topper")

    def showcase(self, gid):
        """The horizontal pictures Popper has for a table, in showing order."""
        rows = self.q("SELECT GameID, GameName, EMUID, GameDisplay FROM Games WHERE GameID = ?", (int(gid),))
        if not rows:
            return None
        r = rows[0]
        media = [sc for sc in self.SHOWCASE if (lambda f: f and self.landscape(f))(self.media(r, sc))]
        return {"id": r["GameID"], "name": (r.get("GameDisplay") or r.get("GameName") or "").strip(), "media": media}

    def media_for(self, gid, screen):
        rows = self.q("SELECT GameID, GameName, EMUID FROM Games WHERE GameID = ?", (int(gid),))
        return self.media(rows[0], screen) if rows else None

    def b2s_for(self, gid):
        """The table's .directb2s (next to its table file, same name), or None."""
        rows = self.q("SELECT g.GameFileName, e.DirGames FROM Games g JOIN Emulators e ON e.EMUID = g.EMUID "
                      "WHERE g.GameID = ?", (int(gid),))
        if not rows or not rows[0].get("GameFileName") or not rows[0].get("DirGames"):
            return None
        path = os.path.splitext(os.path.join(rows[0]["DirGames"], rows[0]["GameFileName"]))[0] + ".directb2s"
        return path if os.path.isfile(path) else None

    def dmd_ids(self):
        """Visible tables whose DMD goes through dmdext (the ones a FullDMD picture is for)."""
        return [r["GameID"] for r in self.q("SELECT GameID FROM Games WHERE Visible = 1 AND EMUID IN (%s)"
                                            % ",".join(str(e) for e in DMD_EMUS))]

    def emulators(self):
        """The visible emulators with tables: [{id, name}] (the board's system filter)."""
        rows = self.q("SELECT e.EMUID, e.EmuName FROM Emulators e WHERE e.Visible = 1 AND EXISTS "
                      "(SELECT 1 FROM Games g WHERE g.EMUID = e.EMUID AND g.Visible = 1) ORDER BY e.EMUID")
        return [{"id": r["EMUID"], "name": (r["EmuName"] or "").strip()} for r in rows]

    def emu_of(self, gid):
        rows = self.q("SELECT EMUID FROM Games WHERE GameID = ?", (int(gid),))
        return rows[0]["EMUID"] if rows else None

    def exit_game(self):
        """Popper closes the running game: its pupEvent 15 (fEmuExit), as its own Exit key."""
        url = "http://127.0.0.1:%d/pupkey/15" % self.web_port
        try:
            with urllib.request.urlopen(url, timeout=4) as resp:
                return resp.status == 200, ""
        except Exception as e:   # noqa: BLE001
            return False, "Popper Web Remote: %s" % e

    def id_for_file(self, game_full_name):
        """Popper's [GAMEFULLNAME] (a file name, maybe with a path) -> GameID."""
        base = os.path.basename(game_full_name or "").strip().lower()
        stem = os.path.splitext(base)[0]
        for r in self.q("SELECT GameID, GameFileName, GameName FROM Games"):
            f = os.path.basename(r.get("GameFileName") or "").lower()
            if f == base or os.path.splitext(f)[0] == stem or (r.get("GameName") or "").lower() == stem:
                return r["GameID"]
        return None

    def launch(self, gid):
        """Popper's Web Remote: /function/launchgame/<GameID> (useWEB=true in Popper's script options)."""
        url = "http://127.0.0.1:%d/function/launchgame/%d" % (self.web_port, int(gid))
        try:
            with urllib.request.urlopen(url, timeout=4) as resp:
                return resp.status == 200, ""
        except Exception as e:   # noqa: BLE001 - reported to the board as text
            return False, "Popper Web Remote: %s" % e


def fit(img, w, h, bg, alpha=False, box=None, rot=0):
    """img fitted (aspect kept) and centred on a w x h canvas: RGBA on a transparent canvas (alpha), else RGB
    on colour bg."""
    from PIL import Image
    img.draft("RGB", (w, h))          # JPEG: decode straight at 1/2, 1/4 or 1/8 scale (much less work)
    img = img.convert("RGBA")
    if rot == 90 and img.width > img.height:   # a portrait card saved on its side: upright (90 degrees clockwise)
        img = img.transpose(Image.Transpose.ROTATE_270)
    # box = (x, y, w, h): fitted into that part of the canvas instead of the whole (a framed picture)
    bx, by, bw, bh = box if box else (0, 0, w, h)
    img.thumbnail((bw, bh), Image.BILINEAR, reducing_gap=2.0)
    pos = (bx + (bw - img.width) // 2, by + (bh - img.height) // 2)
    if alpha:
        canvas = Image.new("RGBA", (w, h), (0, 0, 0, 0))
        canvas.paste(img, pos)
        return canvas
    canvas = Image.new("RGBA", (w, h), bg + (255,))
    canvas.alpha_composite(img, pos)
    return canvas.convert("RGB")


def jpeg(img, quality=88):
    """A picture without transparency as a baseline JPEG (what the board's hardware decoder takes: no
    progressive), 4:2:0. A 1024x600 backglass: ~60-120 KB instead of 1.2 MB of pixels (~250 KB zlib), a
    few tens of ms here, ~10 ms on the board."""
    buf = io.BytesIO()
    img.save(buf, "JPEG", quality=quality, subsampling=2, progressive=False, optimize=False)
    return buf.getvalue()


def jpeg_quality(kind):
    """By use: instruction cards are text (90); the showcase is shown as it is (85); the stage's backglass
    is enlarged 2x and darkened on the board, and so is the game's (80). Part of the cache key."""
    return 90 if kind == "help" else 80 if kind in ("backglass", "gamebg") else 85


def picture_key(path, w, h, bg, alpha, box, rot, kind, as_jpeg):
    """The conversion's key: the same file (path, time, size) and the same request -> the same bytes."""
    st = os.stat(path)
    return "%08x" % (zlib.crc32(("%s|%d|%d|%d|%d|%d|%d|%s|%d%s" % (path, st.st_mtime_ns, st.st_size, w, h, bg, alpha, box, rot,
                                                                   "|jpeg%d" % jpeg_quality(kind) if as_jpeg else "")).encode()))


def jpeg_file(path, kind, w, h, bg, box, rot, key):
    """(w, h, JPEG bytes) of a picture for the board, converted once and kept in the cache (.nvj)."""
    from PIL import Image
    cached = os.path.join(cache_dir(), key + ".nvj")
    try:
        with open(cached, "rb") as f:
            w, h = struct.unpack("<HH", f.read(4))
            return w, h, f.read()
    except OSError:
        pass
    with Image.open(path) as im:
        im.seek(0)
        if kind == "help":   # a page as wide as asked (a multiple of 16: whole JPEG blocks), as tall as it is
            page = im.convert("RGB")
            if rot == 90 and page.width > page.height:
                page = page.transpose(Image.Transpose.ROTATE_270)
            page.thumbnail((w // 16 * 16, h), Image.BILINEAR, reducing_gap=2.0)
            pic = page
        else:
            pic = fit(im, w, h, ((bg >> 16) & 255, (bg >> 8) & 255, bg & 255), False, box, rot)
        data = jpeg(pic, jpeg_quality(kind))
        w, h = pic.width, pic.height
    try:
        with open(cached + ".tmp", "wb") as f:
            f.write(struct.pack("<HH", w, h) + data)
        os.replace(cached + ".tmp", cached)
    except OSError:
        pass
    return w, h, data


# What the board asks for, so a picture prepared ahead has the key of its request (dmd_menu.cpp: req_image,
# kBgW x kBgH, the showcase's box).
SHOW_W, SHOW_H, SHOW_BOX = 1024, 600, (32, 18, 960, 480)
BG_W, BG_H = 512, 300


def rgb565(img, w, h, bg, alpha=False, box=None, rot=0):
    """img fitted (aspect kept) and centred on a w x h canvas, as RGB565 little-endian. alpha: the canvas is
    transparent and the 8-bit alpha plane follows the pixels (LVGL's RGB565A8: the board blends the wheel on
    whatever is behind it); else the canvas is colour bg.

    Every step runs inside Pillow's C code (no per-pixel Python): the cabinet PC is often old and slow."""
    from PIL import Image, ImageChops
    canvas = fit(img, w, h, bg, alpha, box, rot)
    if alpha:
        r, g, b, a = canvas.split()
    else:
        r, g, b = canvas.split()
    # RRRRRGGG GGGBBBBB: the two bytes of each pixel never share bits, so add() cannot carry
    hi = ImageChops.add(r.point(lambda v: v & 0xF8), g.point(lambda v: v >> 5))
    lo = ImageChops.add(g.point(lambda v: (v << 3) & 0xE0), b.point(lambda v: v >> 3))
    px = Image.merge("LA", (lo, hi)).tobytes()     # lo, hi interleaved = little-endian
    return px + a.tobytes() if alpha else px


FULLDMD_W, FULLDMD_H = 1024, 600


def fulldmd(b2s, jpg=False):
    """The FullDMD picture of a .directb2s (its DMDImage: the art made for a big DMD screen, with a dark
    window where the DMD goes), as zlib-compressed RGB565 at the board's size (jpg: a JPEG, a fifth of it,
    for a board that decodes them): (raw size, bytes, true width, true height), or None
    when it has none (or one that is not about 16:9). Converted once, then read from the cache (a "none"
    answer too): the backglass file is only opened again when it changes. Finding the window and drawing
    the DMD in it is the board's work."""
    from PIL import Image
    import base64
    import io
    st = os.stat(b2s)
    key = "%08x" % zlib.crc32(("fulldmd2|%s|%s|%d|%d" % ("jpg" if jpg else "565", b2s, st.st_mtime_ns, st.st_size)).encode())
    cached = os.path.join(cache_dir(), key + ".fulldmd")
    try:
        with open(cached, "rb") as f:
            head = f.read(12)
            if len(head) < 12:
                return None
            size, aw, ah = struct.unpack("<III", head)
            return size, f.read(), aw, ah
    except OSError:
        pass
    with open(b2s, "rb") as f:
        data = f.read()
    res = None
    i = data.find(b'<DMDImage Value="')
    if i >= 0:
        j = data.find(b'"', i + 17)
        raw = data[i + 17:j].replace(b"&#xD;", b"").replace(b"&#xA;", b"")
        del data
        with Image.open(io.BytesIO(base64.b64decode(b"".join(raw.split())))) as im:
            if 1.5 <= im.width / im.height <= 2.0:   # a FullDMD screen, not a DMD-sized picture
                aw, ah = im.size   # its true shape: the board's copy is stretched to 1024x600
                im.draft("RGB", (FULLDMD_W, FULLDMD_H))
                im = im.convert("RGB").resize((FULLDMD_W, FULLDMD_H), Image.BILINEAR)   # 16:9 -> 1024x600: 4% taller
                if jpg:
                    px = jpeg(im, 88)
                    res = (len(px), px, aw, ah)
                else:
                    px = rgb565(im, FULLDMD_W, FULLDMD_H, (0, 0, 0))
                    res = (len(px), zlib.compress(px, 6), aw, ah)
    try:
        with open(cached + ".tmp", "wb") as f:
            if res:
                f.write(struct.pack("<III", res[0], res[2], res[3]) + res[1])
        os.replace(cached + ".tmp", cached)
    except OSError:
        pass
    return res


def b2s_grill(b2s, jpg=False):
    """The backglass of a .directb2s with its grill: the picture a 2-screen cabinet shows, whose bottom
    (GrillHeight rows) holds the speaker panel with the DMD's housing (a backglass without one: gh 0). Fitted whole into the board's
    1024x600 (aspect kept, black around), as zlib-compressed RGB565 (jpg: a JPEG): (raw size, bytes, grill_y, grill_h,
    pic_x, pic_w) with the grill's rows and the picture's columns in it, or None when the file has none. Converted once, then read
    from the cache (a "none" answer too). Finding the housing in the grill is the board's work."""
    from PIL import Image
    import base64
    import io
    st = os.stat(b2s)
    key = "%08x" % zlib.crc32(("b2sbg|%s|%s|%d|%d" % ("jpg" if jpg else "565", b2s, st.st_mtime_ns, st.st_size)).encode())
    cached = os.path.join(cache_dir(), key + ".b2sgrill")
    try:
        with open(cached, "rb") as f:
            head = f.read(20)
            if head == b"none2":   # none, by this version (an empty file: by one that missed the lit / unlit pair)
                return None
            if len(head) < 20:
                raise OSError("again")
            size, gy, gh, gx, gw = struct.unpack("<IIIII", head)
            return size, f.read(), gy, gh, gx, gw
    except OSError:
        pass
    with open(b2s, "rb") as f:
        data = f.read()
    res = None
    m = re.search(rb'<GrillHeight Value="(\d+)"', data)
    grill = int(m.group(1)) if m else 0
    # the backglass: its picture, or (some files: TRON, Terminator 2) its pair of lit / unlit pictures, unlit
    i, tag = -1, b""
    for tag in (b'<BackglassImage Value="', b'<BackglassOffImage Value="', b'<BackglassOnImage Value="'):
        i = data.find(tag)
        if i >= 0:
            break
    if i >= 0:   # no grill too: the backglass alone (gh 0)
        j = data.find(b'"', i + len(tag))
        raw = data[i + len(tag):j].replace(b"&#xD;", b"").replace(b"&#xA;", b"")
        del data
        with Image.open(io.BytesIO(base64.b64decode(b"".join(raw.split())))) as im:
            W, H = im.size
            if 0 <= grill < H:
                k = min(FULLDMD_W / W, FULLDMD_H / H)
                w, h = max(1, round(W * k)), max(1, round(H * k))
                ox, oy = (FULLDMD_W - w) // 2, (FULLDMD_H - h) // 2
                im.draft("RGB", (w, h))
                gy = oy + round((H - grill) * k)
                if jpg:
                    canvas = Image.new("RGB", (FULLDMD_W, FULLDMD_H))
                    canvas.paste(im.convert("RGB").resize((w, h), Image.BILINEAR), (ox, oy))
                    px = jpeg(canvas, 88)
                    res = (len(px), px, gy, oy + h - gy, ox, w)
                else:
                    px = rgb565(im.convert("RGB"), FULLDMD_W, FULLDMD_H, (0, 0, 0))   # contain, centred
                    res = (len(px), zlib.compress(px, 6), gy, oy + h - gy, ox, w)
    try:
        with open(cached + ".tmp", "wb") as f:
            if res:
                f.write(struct.pack("<IIIII", res[0], res[2], res[3], res[4], res[5]) + res[1])
            else:
                f.write(b"none2")
        os.replace(cached + ".tmp", cached)
    except OSError:
        pass
    return res


# ---- The second screen (the backglass monitor), per table: what B2S and dmdext show there ----
# A composition is chosen on the board, which computes every rectangle (in the second screen's pixels) from
# the pictures it has; the bridge only writes them where B2S and dmdext read them at the table's start:
#   * the table's .res (B2S ScreenRes V2): the backglass's size and place, the B2S DMD window's (the
#     .directb2s's DMDImage, the FullDMD art) size and place;
#   * B2STableSettings.xml, the table's block (by ROM): HideGrill, HideB2SDMD;
#   * DmdDevice.ini, the game's section (by ROM): dmdext's virtual DMD, on or off, and where.
# Each file is backed up once (S2_BACKUP) before the bridge first changes it: its own suffix, so a backup
# made earlier by hand (".prima-di-nucleo", ".prima-di-tmnt"...) is never taken for the state to restore.
RES_LINES = 17   # V2: pf w, pf h, bg w, bg h, bg screen, bg x, bg y, dmd w, dmd h, dmd x, dmd y, dmd flip,
                 # background x, y, w, h, background picture


S2_BACKUP = ".prima-di-s2"


def backup_once(path):
    b = path + S2_BACKUP
    if os.path.isfile(path) and not os.path.exists(b):
        with open(path, "rb") as f, open(b, "wb") as o:
            o.write(f.read())


def write_text(path, text):
    """Written whole or not at all; a file still open in a closing emulator (dmdext keeps DmdDevice.ini for a
    moment after a game) is tried again for a few seconds."""
    for i in range(20):
        try:
            with open(path + ".tmp", "w", encoding="utf-8", newline="") as f:
                f.write(text)
            os.replace(path + ".tmp", path)
            return
        except PermissionError:
            if i == 19:
                raise
            time.sleep(0.5)


def s2_res(b2s, bg, dmd):
    """The table's .res: bg = [x, y, w, h] of the backglass, dmd = [x, y, w, h] of B2S's DMD window or None
    (both in the second screen's pixels). B2S places its DMD window from the backglass's corner (seen on the
    cabinet: a backglass at y 152 took the window 152 rows lower), so the file gets it relative to it."""
    path = os.path.splitext(b2s)[0] + ".res"
    lines = []
    if os.path.isfile(path):
        with open(path, encoding="utf-8", errors="replace") as f:
            lines = [l.rstrip("\r\n") for l in f]
    if not lines or lines[0].strip() != "# V2" or len(lines) < RES_LINES + 1:
        base = os.path.join(os.path.dirname(b2s), "ScreenRes.txt")   # B2S's own default for every table
        old = []
        if os.path.isfile(base):
            with open(base, encoding="utf-8", errors="replace") as f:
                old = [l.rstrip("\r\n") for l in f]
        if old and old[0].strip() == "# V2" and len(old) >= RES_LINES + 1:
            lines = old[:RES_LINES + 1]
        else:
            raise ValueError("no V2 ScreenRes to start from")
    backup_once(path)
    if dmd and dmd[0] == bg[0] and dmd[1] == bg[1]:
        # 0,0 from the backglass means "no DMD window" to B2S (seen: none opened): the backglass, hidden
        # under the art, a pixel smaller each side, the window then at -1,-1 from it
        bg = [bg[0] + 1, bg[1] + 1, bg[2] - 2, bg[3] - 2]
    v = lines[1:RES_LINES + 1]
    v[2], v[3] = str(int(bg[2])), str(int(bg[3]))
    v[5], v[6] = str(int(bg[0])), str(int(bg[1]))
    d = [dmd[0] - bg[0], dmd[1] - bg[1], dmd[2], dmd[3]] if dmd else [0, 0, 0, 0]
    v[7], v[8], v[9], v[10] = str(int(d[2])), str(int(d[3])), str(int(d[0])), str(int(d[1]))
    write_text(path, "\r\n".join([lines[0]] + v + lines[RES_LINES + 1:]) + "\r\n")


def s2_settings(b2s, key, hide_grill, hide_dmd):
    """The table's block of B2STableSettings.xml (next to the tables): HideGrill and HideB2SDMD."""
    path = os.path.join(os.path.dirname(b2s), "B2STableSettings.xml")
    with open(path, encoding="utf-8", errors="replace", newline="") as f:
        x = f.read()
    backup_once(path)
    vals = {"HideGrill": int(bool(hide_grill)), "HideB2SDMD": int(bool(hide_dmd))}
    m = re.search(r"<%s>(.*?)</%s>" % (re.escape(key), re.escape(key)), x, re.S)
    if m:
        body = m.group(1)
        for k, val in vals.items():
            if re.search(r"<%s>\d*</%s>" % (k, k), body):
                body = re.sub(r"<%s>\d*</%s>" % (k, k), "<%s>%d</%s>" % (k, val, k), body)
            else:
                body = body.rstrip() + "\r\n    <%s>%d</%s>\r\n  " % (k, val, k)
        x = x[:m.start(1)] + body + x[m.end(1):]
    else:
        block = "  <%s>\r\n%s  </%s>\r\n" % (key, "".join("    <%s>%d</%s>\r\n" % (k, val, k) for k, val in vals.items()), key)
        i = x.rfind("</B2STableSettings>")
        if i < 0:
            raise ValueError("B2STableSettings.xml without its root")
        x = x[:i] + block + x[i:]
    write_text(path, x)


def s2_vdmd(ini, rom, rect):
    """dmdext's virtual DMD for one game (DmdDevice.ini, section [rom]): rect = [x, y, w, h] on the desktop,
    or None (off)."""
    with open(ini, encoding="utf-8", errors="replace") as f:
        lines = f.read().splitlines()
    backup_once(ini)
    keys = {"virtualdmd enabled": "true" if rect else "false"}
    if rect:
        keys.update({"virtualdmd left": str(int(rect[0])), "virtualdmd top": str(int(rect[1])),
                     "virtualdmd width": str(int(rect[2])), "virtualdmd height": str(int(rect[3]))})
    head = "[%s]" % rom   # dmdext matches the game's name exactly, capitals included (Configuration.cs)
    start = next((i for i, l in enumerate(lines) if l.strip() == head), None)
    if start is None:
        lines += [head] + ["%s = %s" % kv for kv in keys.items()]
    else:
        end = next((i for i in range(start + 1, len(lines)) if lines[i].strip().startswith("[")), len(lines))
        body = [l for l in lines[start + 1:end]
                if l.split("=")[0].strip().lower() not in keys and not (not rect and l.strip().lower().startswith("virtualdmd "))]
        while body and not body[-1].strip():
            body.pop()
        lines[start + 1:end] = ["%s = %s" % kv for kv in keys.items()] + body + ([""] if end < len(lines) else [])
    write_text(ini, "\r\n".join(lines) + "\r\n")


def s2_restore(b2s, ini, rom, dmd_key=None):
    """The table as it came: its .res from the backup, its B2S block and its dmdext section as the backups
    of the shared files have them (gone when they had none). Nothing to do when it was never changed."""
    path = os.path.splitext(b2s)[0] + ".res"
    if os.path.isfile(path + S2_BACKUP):
        with open(path + S2_BACKUP, "rb") as f, open(path, "wb") as o:
            o.write(f.read())
    xml = os.path.join(os.path.dirname(b2s), "B2STableSettings.xml")
    if os.path.isfile(xml + S2_BACKUP):
        with open(xml + S2_BACKUP, encoding="utf-8", errors="replace", newline="") as f:
            old = f.read()
        with open(xml, encoding="utf-8", errors="replace", newline="") as f:
            x = f.read()
        pat = r"[ \t]*<%s>.*?</%s>[ \t]*\r?\n?" % (re.escape(rom), re.escape(rom))
        m_old, m_new = re.search(pat, old, re.S), re.search(pat, x, re.S)
        if m_new:
            x = x[:m_new.start()] + (m_old.group(0) if m_old else "") + x[m_new.end():]
        elif m_old:
            i = x.rfind("</B2STableSettings>")
            x = x[:i] + m_old.group(0) + x[i:]
        write_text(xml, x)
    if os.path.isfile(ini + S2_BACKUP):
        def section(lines, head):
            st = next((i for i, l in enumerate(lines) if l.strip() == head), None)
            if st is None:
                return None, None
            en = next((i for i in range(st + 1, len(lines)) if lines[i].strip().startswith("[")), len(lines))
            return st, en
        with open(ini + S2_BACKUP, encoding="utf-8", errors="replace") as f:
            old = f.read().splitlines()
        with open(ini, encoding="utf-8", errors="replace") as f:
            cur = f.read().splitlines()
        head = "[%s]" % (dmd_key or rom)
        a, b = section(old, head)
        c, d = section(cur, head)
        keep = old[a:b] if a is not None else []
        if c is not None:
            cur[c:d] = keep
        elif keep:
            cur += keep
        write_text(ini, "\r\n".join(cur) + "\r\n")


TABLE_NAMES = re.compile(rb'\b(cGameName|GameName|B2SName)\s*=\s*"([A-Za-z0-9_\-. ]{2,40})"', re.I)


def table_names(vpx):
    """The names B2S and dmdext know a table by when Popper has no ROM for it (an original table, an EM, a
    FlexDMD one): (B2S's, dmdext's): its game name when it has one, else its B2SName, from
    its script inside the .vpx, read in 8 MB pieces. (None, None) when it has none."""
    found = {}
    with open(vpx, "rb") as f:
        tail = b""
        while True:
            b = f.read(8 << 20)
            if not b:
                break
            for m in TABLE_NAMES.finditer(tail + b):
                found.setdefault(m.group(1).decode().lower(), m.group(2).decode())
            tail = b[-120:]
    game = found.get("cgamename") or found.get("gamename")
    b2s = found.get("b2sname")
    if b2s:
        b2s = b2s.replace(" ", "")   # B2S drops the spaces (Server.vb)
    return game or b2s, game or b2s   # B2S: its GameName when there is one, else B2SName (B2SSettings.vb)


def b2s_dmdtype(b2s):
    """The .directb2s's DMDType: B2S opens its DMD window (the FullDMD art) only for 3 or 4 where the bridge
    puts it (2: only at the backglass's own place, over its grill; 0/1: never). Read from the file's head."""
    with open(b2s, "rb") as f:
        m = re.search(rb'<DMDType Value="(\d+)"', f.read(1 << 20))
    return int(m.group(1)) if m else 0


def second_screen():
    """The second monitor's rectangle on the desktop [x, y, w, h] (the first one that is not the primary),
    or None. Once, at start."""
    if os.name != "nt":
        return None
    try:
        import ctypes
        from ctypes import wintypes
        found = []

        class MI(ctypes.Structure):
            _fields_ = [("cbSize", wintypes.DWORD), ("rcMonitor", wintypes.RECT), ("rcWork", wintypes.RECT),
                        ("dwFlags", wintypes.DWORD)]

        proto = ctypes.WINFUNCTYPE(ctypes.c_int, wintypes.HMONITOR, wintypes.HDC, ctypes.POINTER(wintypes.RECT),
                                   wintypes.LPARAM)

        def cb(hmon, hdc, rc, lp):
            mi = MI()
            mi.cbSize = ctypes.sizeof(MI)
            if ctypes.windll.user32.GetMonitorInfoW(hmon, ctypes.byref(mi)) and not mi.dwFlags & 1:
                r = mi.rcMonitor
                found.append([r.left, r.top, r.right - r.left, r.bottom - r.top])
            return 1
        ctypes.windll.user32.EnumDisplayMonitors(None, None, proto(cb), 0)
        return found[0] if found else None
    except Exception as e:   # noqa: BLE001
        log("second screen:", e)
        return None


def raise_vdmd(token_alive):
    """dmdext's virtual DMD in front: B2S keeps its windows on top (FormToFront), which would hide a DMD put
    over the backglass. The window is looked for every 2 s while the table loads (EnumWindows: a few
    microseconds), raised once found and once more a little later (B2S may come up after it)."""
    if os.name != "nt":
        return
    import ctypes
    from ctypes import wintypes
    u32 = ctypes.windll.user32
    proto = ctypes.WINFUNCTYPE(ctypes.c_int, wintypes.HWND, wintypes.LPARAM)

    def find():
        hits = []

        def cb(h, lp):
            buf = ctypes.create_unicode_buffer(32)
            u32.GetWindowTextW(h, buf, 32)
            if buf.value == "Virtual DMD" and u32.IsWindowVisible(h):
                hits.append(h)
            return 1
        u32.EnumWindows(proto(cb), 0)
        return hits

    raised = 0
    for _ in range(90):
        time.sleep(2)
        if not token_alive():
            return
        hits = find()
        if not hits:
            continue
        for h in hits:
            u32.SetWindowPos(h, wintypes.HWND(-1), 0, 0, 0, 0, 0x0013)   # TOPMOST, no move/size/activate
        raised += 1
        if raised == 1:
            log("virtual DMD in front")
        if raised >= 3:
            return
        time.sleep(6)


def s2_apply(b2s, ini, rom, comp, section=None):
    """Writes one table's composition: comp = {"bg": [x,y,w,h], "grill": bool, "b2sdmd": [..] or None,
    "vdmd": [..] or None, "x0": the second screen's left on the desktop}."""
    s2_res(b2s, comp["bg"], comp.get("b2sdmd"))
    s2_settings(b2s, rom, not comp.get("grill"), not comp.get("b2sdmd"))
    v = comp.get("vdmd")
    s2_vdmd(ini, section or rom, [v[0] + comp.get("x0", 0), v[1] + comp.get("y0", 0), v[2], v[3]] if v else None)


def processes():
    """The running processes: [(name lower case, pid)]. One process list (Windows' tasklist: a fraction of a
    second), only at a game's start or end, or the bridge's start: never polled."""
    if os.name != "nt":
        return []
    try:
        out = subprocess.run(["tasklist", "/FO", "CSV", "/NH"], capture_output=True, text=True, timeout=20,
                             creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0)).stdout
    except Exception as e:   # noqa: BLE001
        log("process list:", e)
        return []
    procs = []
    for ln in out.splitlines():
        f = ln.strip('"').split('","')
        if len(f) >= 2:
            try:
                procs.append((f[0].lower(), int(f[1])))
            except ValueError:
                pass
    return procs


def game_process(procs=None):
    """A running table's emulator: (process name, its DMD goes through dmdext, pid), or None."""
    for name, pid in (processes() if procs is None else procs):
        for pre, dmd in GAME_EXES:
            if name.startswith(pre):
                return name, dmd, pid
    return None


# What a table leaves behind when its emulator crashes or is killed: VPX's B2S backglass server stays open on
# its own, and Popper then refuses to launch anything (seen on this cabinet). Closed only with no table running.
ORPHANS = ("b2sbackglassserverexe",)


def close_orphans():
    procs = processes()
    if game_process(procs):   # a table runs (maybe a new one): what is open is its own
        return 0
    n = 0
    for name, pid in procs:
        if any(name.startswith(o) for o in ORPHANS):
            try:
                subprocess.run(["taskkill", "/F", "/PID", str(pid)], capture_output=True, timeout=20,
                               creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
                log("closed %s (pid %d), left open by a table that is gone: Popper launches again" % (name, pid))
                n += 1
            except Exception as e:   # noqa: BLE001
                log("could not close %s: %s" % (name, e))
    return n


def nav_item(cur):
    """Popper's selection on an entry that is not a table (GameID 0, mtType 2 here: "Go Back" is GameName
    "menuback"): (kind, Popper's label). kind: back / exit / playlist / menu."""
    label = str(cur.get("GameDisplay") or cur.get("PlayListName") or "").strip()
    words = (str(cur.get("GameName") or "") + " " + label).lower()
    if "back" in words:
        kind = "back"
    elif any(w in words for w in ("exit", "quit", "shutdown", "close")):
        kind = "exit"
    elif cur.get("PlayListName") or int(cur.get("mtPlaylistID") or 0) > 0:
        kind = "playlist"
    else:
        kind = "menu"
    return kind, label[:60]


def lan_ip():
    """This PC's address on the LAN (the board reaches Popper's Web Remote there during a game, when the
    USB port is dmdext's). A UDP "connect" sends nothing: it only picks the route."""
    import socket
    try:
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as s:
            s.connect(("192.168.0.1", 9))
            return s.getsockname()[0]
    except OSError:
        return ""


def cache_dir():
    d = os.path.join(os.environ.get("LOCALAPPDATA") or os.path.expanduser("~"), "nucleo_bridge", "cache")
    os.makedirs(d, exist_ok=True)
    return d


def low_priority():
    """Below-normal CPU priority: the bridge never competes with the table being played."""
    if os.name == "nt":
        import ctypes
        ctypes.windll.kernel32.SetPriorityClass(ctypes.windll.kernel32.GetCurrentProcess(), 0x4000)
    else:
        try:
            os.nice(10)
        except OSError:
            pass


# ------------------------------------------------------------------------------------------------ the link

class Link:
    """The board's USB port: opened while no game runs, handed to dmdext while one does."""

    def __init__(self, port, popper):
        self.port_name = port
        self.popper = popper
        self.sp = None
        self.lock = threading.Lock()
        self.want_open = True
        self.game = None
        self.game_msg = None         # the board's "game started" message (sent again if the port reopens)
        self.game_token = None       # this game (its process watcher ends only its own game)
        self.show_id = 0             # the table whose showcase the board shows (Popper's selection, or the game's)
        self.idle_lock = threading.Lock()   # the idle-time conversions (FullDMD art, pictures ahead): one at a time
        self.active_at = 0.0         # the last move in Popper (a player at the cabinet)
        self.alive = True
        self.board_fulldmd = False   # the board's "Fit" is FullDMD: send that picture as a game starts
        self.board_jpeg = False      # the board decodes JPEG (it said so in a request): opaque pictures so
        self.last_q = time.time()    # the board's last request (the menu in use)
        self.s2 = second_screen()    # the backglass monitor (the board's compositions are made for it)
        self.ended_at = 0.0          # when the last game ended (Popper needs a moment before the next)
        self.s2_store = os.path.join(cache_dir(), "s2.json")   # tables whose composition has a virtual DMD

    def find_port(self):
        if self.port_name:
            return self.port_name
        from serial.tools import list_ports
        for p in list_ports.comports():
            if (p.vid, p.pid) == BOARD_VID_PID:
                return p.device
        return None

    def open(self):
        import serial
        dev = self.find_port()
        if not dev:
            return False
        sp = serial.Serial()
        # DTR and RTS low before opening: on the board's USB-Serial-JTAG some DTR/RTS transitions reset
        # the chip (that is how esptool resets it), and pyserial raises both by default.
        sp.port, sp.baudrate, sp.timeout, sp.write_timeout = dev, 115200, 1, 2
        sp.dtr = False
        sp.rts = False
        try:
            sp.open()
        except Exception:   # noqa: BLE001 - busy (dmdext has it during a game) or unplugged: retry later
            return False
        with self.lock:
            self.sp = sp
        log("USB: %s open" % dev)
        self.hello()
        if self.game and not self.game.get("dmd", True) and self.game_msg:
            self.send_json("G", self.game_msg)   # a game without DMD runs: its pictures on the board again
        return True

    def close(self):
        with self.lock:
            sp, self.sp = self.sp, None
        if sp:
            try:
                sp.flush()
                sp.close()
            except Exception:   # noqa: BLE001
                pass
            log("USB: released")

    def send(self, t, payload):
        data = frame(t, payload)
        with self.lock:
            if not self.sp:
                return False
            try:
                # in 16 KB pieces: the board's USB-Serial-JTAG takes a few hundred KB/s, and one write of a
                # big image would outlast the write timeout
                for o in range(0, len(data), 16384):
                    self.sp.write(data[o:o + 16384])
                return True
            except Exception as e:   # noqa: BLE001
                log("USB write failed:", e)
                try:
                    self.sp.close()
                except Exception:   # noqa: BLE001
                    pass
                self.sp = None
                return False

    def send_json(self, t, obj):
        return self.send(t, json.dumps(obj, ensure_ascii=False, separators=(",", ":")))

    def hello(self):
        try:
            n = self.popper.q("SELECT COUNT(*) AS n FROM Games WHERE Visible = 1")[0]["n"]
            ok = True
        except Exception as e:   # noqa: BLE001
            n, ok = 0, False
            log("Popper database:", e)
        try:
            emus = self.popper.emulators()
        except Exception:   # noqa: BLE001
            emus = []
        self.send_json("H", {"bridge": VERSION, "popper": ok, "tables": n, "game": self.game,
                             "ip": lan_ip(), "web": self.popper.web_port, "emus": emus, "s2": self.s2})

    # ---- requests from the board
    def handle(self, t, payload):
        if t != "Q":
            return
        try:
            r = json.loads(payload.decode("utf-8"))
        except ValueError:
            return
        q = r.get("q")
        self.last_q = time.time()
        try:
            if q == "hello":
                self.hello()
            elif q == "list":
                which = r.get("list", "recent")
                self.send_json("L", {"list": which, "items": self.popper.tables(which, min(int(r.get("limit", 40)), 1000))})
            elif q in ("table", "random"):
                gid = r.get("id") if q == "table" else self.popper.random_id()
                t = self.popper.table(gid) if gid is not None else None
                self.send_json("T", t) if t else self.send_json("E", {"q": q, "err": "no such table"})
            elif q == "image":
                self.image(r)
            elif q == "prefs":
                self.board_fulldmd = bool(r.get("fulldmd"))
            elif q == "s2":
                ok, err = self.apply_s2(r)
                self.send_json("R", {"q": "s2", "id": r.get("id"), "ok": ok, "err": err})
            elif q == "exit":
                ok, err = self.popper.exit_game()
                self.send_json("R", {"q": "exit", "ok": ok, "err": err})
            elif q == "launch":
                # right after a game Popper is still closing it and ignores a launch: a few seconds first
                wait = self.ended_at + 6 - time.time()
                if wait > 0:
                    threading.Thread(target=self.launch_later, args=(r.get("id"), wait), daemon=True).start()
                else:
                    ok, err = self.popper.launch(r.get("id"))
                    self.send_json("R", {"q": "launch", "id": r.get("id"), "ok": ok, "err": err})
        except Exception as e:   # noqa: BLE001 - the board shows it, the bridge keeps running
            log("request %s failed: %s" % (q, e))
            # with what it was about: a picture that cannot be converted (a damaged file) is marked missing on
            # the board, instead of being asked again and again
            self.send_json("E", {"q": q, "id": r.get("id"), "kind": r.get("kind", ""), "screen": r.get("screen", ""),
                                 "err": str(e)[:200]})

    def apply_s2(self, r):
        """A table's composition for the second screen, from the board: written now, read by B2S and dmdext
        when the table next starts. mode 0: the table as it came."""
        gid = int(r.get("id") or 0)
        b2s = self.popper.b2s_for(gid) if gid else None
        if not b2s:
            return False, "no .directb2s"
        rows = self.popper.q("SELECT ROM FROM Games WHERE GameID = ?", (gid,))
        rom = (rows[0].get("ROM") or "").strip() if rows else ""
        try:
            with open(self.s2_store, encoding="utf-8") as f:
                st = json.load(f)
        except (OSError, ValueError):
            st = {}
        section = rom
        if not rom:   # no ROM in Popper: the names from the table's script, once (then kept)
            names = st.get("names", {}).get(str(gid))
            if not names:
                vpx = os.path.splitext(b2s)[0] + ".vpx"
                names = list(table_names(vpx)) if os.path.isfile(vpx) else [None, None]
                st.setdefault("names", {})[str(gid)] = names
                write_text(self.s2_store, json.dumps(st))
                log("table %d without a ROM: B2S %s, dmdext %s" % (gid, names[0], names[1]))
            rom, section = names
            if not rom:
                return False, "no game name in the table"
        ini = os.path.join(os.path.dirname(self.popper.root), "VisualPinball", "VPinMAME", "DmdDevice.ini")
        if not os.path.isfile(ini):
            return False, "no DmdDevice.ini"
        mode = int(r.get("mode") or 0)
        if r.get("art") and b2s_dmdtype(b2s) not in (3, 4):
            return False, "this backglass opens no DMD window for the art"
        if mode == 0:
            s2_restore(b2s, ini, rom, section)
        else:
            if not self.s2:
                return False, "no second screen"
            s2_apply(b2s, ini, rom, {"bg": r["bg"], "grill": bool(r.get("grill")), "b2sdmd": r.get("art"),
                                     "vdmd": r.get("dmd"), "x0": self.s2[0], "y0": self.s2[1]}, section)
        try:
            with open(self.s2_store, encoding="utf-8") as f:
                st = json.load(f)
        except (OSError, ValueError):
            st = {}
        st[str(gid)] = {"mode": mode, "dmd": bool(mode and r.get("dmd"))}
        write_text(self.s2_store, json.dumps(st))
        log("second screen of %d: composition %d %s" % (gid, mode, json.dumps({k: r.get(k) for k in ("bg", "art", "dmd")})))
        return True, ""

    def launch_later(self, gid, wait):
        """A table to start right after a game: Popper ignores a launch while it is still closing the last one
        (after a crash, for ~90 s, until its own end of game): asked again every 25 s until it starts."""
        time.sleep(wait)
        for attempt in range(5):
            ok, err = self.popper.launch(gid)
            log("launch %s after the last game (%d): %s" % (gid, attempt + 1, "ok" if ok else err))
            if attempt == 0:
                self.send_json("R", {"q": "launch", "id": gid, "ok": ok, "err": err})
            for _ in range(25):
                time.sleep(1)
                if self.game:
                    return
        log("launch %s: Popper never started it" % gid)

    def vdmd_wanted(self, gid):
        try:
            with open(self.s2_store, encoding="utf-8") as f:
                return bool(json.load(f).get(str(gid), {}).get("dmd"))
        except (OSError, ValueError):
            return False

    def image(self, r):
        from PIL import Image
        gid, kind = r.get("id"), r.get("kind", "wheel")
        # the instructions may be a tall page (upright, scrolled on the board): up to 2048 rows
        w, h = max(8, min(int(r.get("w", 160)), 1024)), max(8, min(int(r.get("h", 160)), 2048 if kind == "help" else 600))
        rot = 90 if int(r.get("rot", 0) or 0) == 90 else 0
        bg = int(str(r.get("bg", "0")), 0)
        alpha = bool(r.get("alpha"))
        box = r.get("box")
        box = tuple(int(v) for v in box) if isinstance(box, list) and len(box) == 4 else None
        if kind == "fulldmd":
            self.send_fulldmd(gid)
            return
        if r.get("jpeg"):
            self.board_jpeg = True
        # opaque pictures as JPEG when the board takes it (a board without it gets RGB565 as before)
        as_jpeg = not alpha and (bool(r.get("jpeg")) or (self.board_jpeg and "jpeg" not in r))
        screen = {"wheel": "Wheel", "help": "GameHelp", "info": "GameInfo", "backglass": "Backglass",
                  "gamebg": "Backglass"}.get(kind, "Wheel")
        if kind == "media":   # the showcase: any of its screens, by name
            screen = r.get("screen") if r.get("screen") in self.popper.SHOWCASE else "BackGlass"
        path = self.popper.media_for(gid, screen)
        if not path:
            log("image %s %s: none" % (kind, gid))
            self.send_json("E", {"q": "image", "id": gid, "kind": kind, "screen": r.get("screen", ""), "err": "no image"})
            return
        t0 = time.time()
        # converted once, then served from disk: same file, size and background -> same bytes
        key = picture_key(path, w, h, bg, alpha, box, rot, kind, as_jpeg)
        if r.get("have") == key:   # the board has these very bytes (its microSD cache): just say so
            self.send_json("K", {"id": gid, "kind": kind, "w": w, "screen": r.get("screen", ""), "k": key})
            return
        cached = os.path.join(cache_dir(), key + ".nv2")   # size, w, h, zlib pixels (v2: with the size)
        if as_jpeg:
            return self.image_jpeg(r, gid, kind, path, w, h, bg, box, rot, key, t0)
        try:
            with open(cached, "rb") as f:
                size = struct.unpack("<I", f.read(4))[0]
                w, h = struct.unpack("<HH", f.read(4))   # the picture's own size (a page may be narrower)
                z = f.read()
        except OSError:
            with Image.open(path) as im:
                im.seek(0)
                if kind == "help":
                    # a page as big as the picture (no black canvas), narrower until it fits the board's
                    # link (one message: ~1.2 MB)
                    page = im.convert("RGB")
                    if rot == 90 and page.width > page.height:
                        page = page.transpose(Image.Transpose.ROTATE_270)
                    tw = w
                    while True:
                        p2 = page.copy()
                        p2.thumbnail((tw, h), Image.BILINEAR, reducing_gap=2.0)
                        px = rgb565(p2, p2.width, p2.height, (0, 0, 0))
                        z = zlib.compress(px, 6)
                        if len(z) <= 1100000 or tw <= 400:
                            break
                        tw = tw * 85 // 100
                    w, h = p2.width, p2.height
                else:
                    px = rgb565(im, w, h, ((bg >> 16) & 255, (bg >> 8) & 255, bg & 255), alpha, box, rot)
                    z = None
            # zlib (the board inflates it with the ESP ROM's tinfl): instruction cards shrink a lot, wheels too
            size, z = len(px), (z if z is not None else zlib.compress(px, 6))
            try:
                with open(cached + ".tmp", "wb") as f:
                    f.write(struct.pack("<I", size) + struct.pack("<HH", w, h) + z)
                os.replace(cached + ".tmp", cached)
            except OSError:
                pass
        head = json.dumps({"id": gid, "kind": kind, "w": w, "h": h, "fmt": "rgb565a8z" if alpha else "rgb565z", "size": size,
                           "screen": r.get("screen", ""), "k": key}).encode()
        ok = self.send("I", struct.pack("<H", len(head)) + head + z)
        log("image %s %s %dx%d %s: %d KB in %.0f ms%s" % (kind, gid, w, h, os.path.basename(path), len(z) >> 10,
                                                        (time.time() - t0) * 1000, "" if ok else " NOT SENT"))

    def image_jpeg(self, r, gid, kind, path, w, h, bg, box, rot, key, t0):
        """image() for the board that decodes JPEG: the picture as a baseline JPEG, converted once and kept
        (w, h, JPEG) next to the other conversions."""
        if kind == "media" and not r.get("pf") and self.show_id and gid != self.show_id:
            return   # the player moved on in Popper meanwhile: the CPU for the table on screen now
        w, h, data = jpeg_file(path, kind, w, h, bg, box, rot, key)
        head = json.dumps({"id": gid, "kind": kind, "w": w, "h": h, "fmt": "jpeg", "size": len(data),
                           "screen": r.get("screen", ""), "k": key}).encode()
        ok = self.send("I", struct.pack("<H", len(head)) + head + data)
        log("image %s %s %dx%d %s: %d KB jpeg in %.0f ms%s" % (kind, gid, w, h, os.path.basename(path), len(data) >> 10,
                                                             (time.time() - t0) * 1000, "" if ok else " NOT SENT"))

    def send_fulldmd(self, gid):
        t0 = time.time()
        b2s = self.popper.b2s_for(gid) if gid is not None else None
        jpg = False   # lossless: the board's window search needs the art's true black (a JPEG's is not)
        got = fulldmd(b2s, jpg) if b2s else None
        if not got:
            log("fulldmd %s: none" % gid)
            return False
        head = json.dumps({"id": gid, "kind": "fulldmd", "w": FULLDMD_W, "h": FULLDMD_H, "fmt": "jpeg" if jpg else "rgb565z",
                           "size": got[0], "aw": got[2], "ah": got[3]}).encode()
        ok = self.send("I", struct.pack("<H", len(head)) + head + got[1])
        log("fulldmd %s: %d KB in %.0f ms%s" % (gid, len(got[1]) >> 10, (time.time() - t0) * 1000, "" if ok else " NOT SENT"))
        return ok

    def send_grill(self, gid):
        t0 = time.time()
        b2s = self.popper.b2s_for(gid) if gid is not None else None
        jpg = False   # lossless: the housing search measures the grill's dark exactly
        got = b2s_grill(b2s, jpg) if b2s else None
        if not got:
            log("backglass with grill %s: none" % gid)
            return False
        head = json.dumps({"id": gid, "kind": "b2sgrill", "w": FULLDMD_W, "h": FULLDMD_H, "fmt": "jpeg" if jpg else "rgb565z",
                           "size": got[0], "gy": got[2], "gh": got[3], "gx": got[4], "gw": got[5]}).encode()
        ok = self.send("I", struct.pack("<H", len(head)) + head + got[1])
        log("backglass with grill %s: %d KB in %.0f ms%s" % (gid, len(got[1]) >> 10, (time.time() - t0) * 1000,
                                                             "" if ok else " NOT SENT"))
        return ok

    def prepare_fulldmd(self):
        """Once, in the background: convert the FullDMD picture of every table that has one, so a game start
        only reads a small file. Only while the cabinet is idle (no game, nobody using the board's menu for
        5 minutes), one table every few seconds; the ones already in the cache cost a stat and a small read."""
        def idle():
            return not self.game and time.time() - self.last_q > 300
        time.sleep(60)
        try:
            ids = self.popper.dmd_ids()
        except Exception as e:   # noqa: BLE001
            log("fulldmd prepare:", e)
            return
        done = 0
        for gid in ids:
            while self.alive and not idle():
                time.sleep(10)
            if not self.alive:
                return
            try:
                b2s = self.popper.b2s_for(gid)
                with self.idle_lock:   # never alongside the pictures being prepared ahead: one conversion at a time
                    if b2s:
                        b2s_grill(b2s)   # the backglass with its grill too (cached the same way)
                    if b2s and fulldmd(b2s):
                        done += 1
            except Exception as e:   # noqa: BLE001 - a broken backglass file: skip it
                log("fulldmd %s: %s" % (gid, e))
            time.sleep(3)
        log("fulldmd: %d of %d tables have one" % (done, len(ids)))

    def reader(self):
        parser = Parser()
        last_ping = 0
        misses = 0   # opens in a row that found no board
        while self.alive:
            if self.want_open and not self.sp and not self.open():
                # board unplugged or busy: listing the USB ports costs ~25 ms of CPU on this PC, so every 3 s for
                # half a minute (a game just ended: the port comes back in seconds), then every 15 s
                misses += 1
                time.sleep(3 if misses <= 10 else 15)
                continue
            misses = 0
            sp = self.sp
            if not sp:
                time.sleep(0.2 if self.want_open else 1)   # a game runs: dmdext has the port, nothing to do
                continue
            try:
                # blocks until a byte arrives (or 1 s, for the ping): one wake-up a second when idle, and a
                # request is read the moment it starts
                data = sp.read(max(1, sp.in_waiting))
            except Exception:   # noqa: BLE001 - unplugged or taken: reopen later
                self.close()
                continue
            for t, payload in parser.feed(data):
                threading.Thread(target=self.handle, args=(t, payload), daemon=True).start()
            if time.time() - last_ping > 2:
                self.send("P", b"")
                last_ping = time.time()

    # ---- Popper's selection (the cabinet's joystick)
    def follow_popper(self):
        """While no game runs, read Popper's selected item twice a second (its Web Remote, a few bytes on
        localhost: the joystick shows on the board within half a second). When the player uses the
        cabinet's controls — Popper leaves its attract mode, or the selection moves outside it — the board
        gets 'S': a table and its horizontal pictures (a showcase; a table without any: its name and wheel),
        or an entry that is not a table ("Go Back", "Exit", a playlist: what it is). Popper's own attract
        mode moving the selection is ignored, and Popper going back to it gives the board its menu again."""
        # How often: 5 times a second while the player moves (the joystick on the board within 0.2 s), twice
        # when Popper is on a table untouched, once in its attract mode. One reading is ~2.6 ms of CPU on this
        # PC with http.client (urllib: 5-8 ms), so: less CPU than a fixed half second, and faster when it counts.
        last, last_attract = None, True
        while self.alive:
            busy = time.time() - self.active_at < 20 and not self.game
            time.sleep(2.0 if self.game else 0.2 if busy else 1.0 if last_attract else 0.5)
            if not self.sp or self.game:   # a game runs: nothing to read until it ends (every 2 s, a flag)
                continue
            try:
                conn = http.client.HTTPConnection("127.0.0.1", self.popper.web_port, timeout=1)
                try:
                    conn.request("GET", "/function/getcuritem")
                    cur = json.loads(conn.getresponse().read().decode("utf-8", "replace"))
                finally:
                    conn.close()
            except Exception:   # noqa: BLE001 - Popper closed or busy: try again later
                time.sleep(4)
                continue
            if not isinstance(cur, dict):   # "" for a moment while Popper changes screen or list: next reading
                continue
            gid, attract = int(cur.get("GameID") or 0), str(cur.get("inAttractMode", "0")) == "1"
            what = gid if gid > 0 else nav_item(cur)
            if attract:
                if not last_attract:
                    self.show_id = 0
                    self.send_json("S", {"idle": True})
            elif last_attract or what != last:
                self.active_at = time.time()
                if gid > 0:
                    sc = self.popper.showcase(gid)
                    if sc:
                        self.show_id = gid
                        self.send_json("S", sc)
                else:
                    self.show_id = 0
                    self.send_json("S", {"id": 0, "nav": what[0], "name": what[1]})
            last, last_attract = what, attract

    def prepare_pictures(self):
        """In idle time — no game, nobody at Popper for half a minute — the showcase pictures and backglasses
        of every table are converted ahead, once (the PC's cache keeps them): then a joystick move only reads
        a file (~5 ms instead of ~300 on this PC). One picture at a time, at below-normal priority, resting
        6x as long as it worked (measured: resting 2x took ~18% of a core): ~13 minutes of a slow core,
        once, spread over an hour and a half of idle time."""
        time.sleep(90)   # after the start: the board's own requests first
        try:
            ids = [t["id"] for t in self.popper.tables("recent", 1000)]   # recently played first
            seen = set(ids)
            ids += [r["GameID"] for r in self.popper.q("SELECT GameID FROM Games WHERE Visible = 1 ORDER BY GameID")
                    if r["GameID"] not in seen]
        except Exception as e:   # noqa: BLE001
            log("pictures ahead:", e)
            return
        made = had = 0
        t_start = time.time()
        for gid in ids:
            jobs = []
            try:
                sc = self.popper.showcase(gid)
                for screen in (sc["media"] if sc else []):
                    jobs.append(("media", self.popper.media_for(gid, screen), SHOW_W, SHOW_H, SHOW_BOX))
                jobs.append(("backglass", self.popper.media_for(gid, "Backglass"), BG_W, BG_H, None))
            except Exception:   # noqa: BLE001
                continue
            for kind, path, w, h, box in jobs:
                if not path:
                    continue
                while self.alive and (self.game or time.time() - self.active_at < 30):
                    time.sleep(10)   # someone is playing or at Popper: later
                if not self.alive:
                    return
                try:
                    key = picture_key(path, w, h, 0, False, box, 0, kind, True)
                    if os.path.exists(os.path.join(cache_dir(), key + ".nvj")):
                        had += 1
                        continue
                    with self.idle_lock:   # never alongside the FullDMD preparation: one conversion at a time
                        t0 = time.time()
                        jpeg_file(path, kind, w, h, 0, box, 0, key)
                    made += 1
                    # resting 6x as long as it worked: never over ~14% of a core on this PC, even in idle time
                    time.sleep(max(1.0, 6 * (time.time() - t0)))
                except Exception:   # noqa: BLE001 - a damaged file: the next one
                    continue
        log("pictures ahead: %d converted, %d were ready, %.0f min" % (made, had, (time.time() - t_start) / 60))

    # ---- Popper's launch / close scripts
    def game_start(self, name):
        gid = None
        try:
            gid = self.popper.id_for_file(name)
        except Exception:   # noqa: BLE001
            pass
        self.begin_game(gid, os.path.splitext(os.path.basename(name))[0], name)

    def adopt_running_game(self):
        """At start only: a table already running (the bridge restarted mid-game, or started after it).
        One process list (no polling, the PC is slow); the table is Popper's selected one. Without this the
        board would not know: no way to close it from there, and the port fought over with dmdext."""
        found = game_process()
        if not found:
            return
        gid = None
        try:
            with urllib.request.urlopen("http://127.0.0.1:%d/function/getcuritem" % self.popper.web_port, timeout=2) as r:
                gid = int(json.loads(r.read().decode("utf-8", "replace")).get("GameID") or 0) or None
        except Exception:   # noqa: BLE001 - no Web Remote: the table stays unnamed
            pass
        log("a game was already running:", found[0], gid)
        self.begin_game(gid, "", found[0], dmd_default=found[1])

    def watch_game(self, token):
        """Popper's end hook does not always come (an exit pressed while the table was still loading, seen
        on this cabinet): the bridge would wait for ever, the board stuck "in game". So: the emulator's
        process, found once, then a kernel wait on it (no polling, no CPU). It exits and no hook followed
        in a few seconds: the game is over. Then what a crashed table leaves open is closed (close_orphans)."""
        if os.name != "nt":
            return
        import ctypes
        pid = None
        for _ in range(8):           # the emulator's process may take a while to appear
            time.sleep(8)
            if self.game_token is not token:
                return
            found = game_process()
            if found:
                pid = found[2]
                break
        if not pid:
            return
        k32 = ctypes.windll.kernel32
        h = k32.OpenProcess(0x00100000, False, pid)   # SYNCHRONIZE: enough to wait on it
        if not h:
            return
        try:
            k32.WaitForSingleObject(h, 0xFFFFFFFF)
        finally:
            k32.CloseHandle(h)
        time.sleep(6)                # Popper's hook, normally; a backglass server closes itself by now
        close_orphans()
        if self.game_token is token and self.game:
            log("the emulator exited without Popper's end hook: game over")
            self.game_end()

    def begin_game(self, gid, shown, name, dmd_default=True):
        if gid is not None:
            try:
                t = self.popper.table(gid)
                shown = t["name"] if t else shown
            except Exception:   # noqa: BLE001
                pass
        try:
            dmd = dmd_default if gid is None else self.popper.emu_of(gid) in DMD_EMUS
        except Exception:   # noqa: BLE001
            dmd = dmd_default
        self.game = {"id": gid, "name": shown, "dmd": dmd}
        log("game start:", name, gid, shown, "(DMD through dmdext)" if dmd else "(no DMD: showcase)")
        msg = {"game": "start", "id": gid, "name": shown, "dmd": dmd}
        if not dmd and gid is not None:
            try:
                msg["showcase"] = self.popper.showcase(gid)
                self.show_id = gid   # its pictures are the ones on the board now
            except Exception as e:   # noqa: BLE001
                log("showcase:", e)
        self.game_msg = msg
        self.game_token = token = object()
        threading.Thread(target=self.watch_game, args=(token,), daemon=True).start()
        if gid is not None and self.vdmd_wanted(gid):   # a DMD over the backglass: in front of B2S
            threading.Thread(target=raise_vdmd, args=(lambda: self.game_token is token,), daemon=True).start()
        self.send_json("G", msg)
        if not dmd:
            return               # the board keeps showing us: the game's pictures, its quick actions
        if gid is not None:
            if self.board_fulldmd:   # FullDMD first: its art with the DMD's window (the board finds the window)
                try:
                    self.send_fulldmd(gid)
                except Exception as e:   # noqa: BLE001
                    log("game fulldmd:", e)
                try:   # and the backglass with its grill (the DMD's housing in it)
                    self.send_grill(gid)
                except Exception as e:   # noqa: BLE001
                    log("game backglass with grill:", e)
            try:   # the backbox: the table's backglass above the DMD (converted once, cached)
                self.image({"id": gid, "kind": "gamebg", "w": 512, "h": 300})
            except Exception as e:   # noqa: BLE001
                log("game backglass:", e)
        # the board must have read every byte before the port goes to dmdext (VPX takes far longer to load)
        time.sleep(0.8)
        self.want_open = False
        self.close()             # dmdext takes the port for the DMD

    def game_end(self):
        log("game end")
        self.ended_at = time.time()
        kept = bool(self.game and not self.game.get("dmd", True) and self.sp)
        self.game = self.game_msg = self.game_token = None
        self.show_id = 0
        if kept:                 # the port never left: just say so
            self.send_json("G", {"game": "end"})
            return
        time.sleep(1.5)          # dmdext closes the port as the emulator exits
        self.want_open = True    # the reader reopens it (retrying while dmdext still has it)
        for _ in range(40):
            if self.sp:
                self.send_json("G", {"game": "end"})
                return
            time.sleep(0.25)


def hook_server(link):
    class H(http.server.BaseHTTPRequestHandler):
        def do_GET(self):   # noqa: N802
            u = urllib.parse.urlparse(self.path)
            qs = urllib.parse.parse_qs(u.query)
            if u.path == "/game/start":
                threading.Thread(target=link.game_start, args=(qs.get("name", [""])[0],), daemon=True).start()
            elif u.path == "/game/end":
                threading.Thread(target=link.game_end, daemon=True).start()
            elif u.path != "/status":
                self.send_response(404)
                self.end_headers()
                return
            body = json.dumps({"bridge": VERSION, "usb": bool(link.sp), "game": link.game}).encode()
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.end_headers()
            self.wfile.write(body)

        def log_message(self, *a):
            pass

    socketserver.TCPServer.allow_reuse_address = True
    srv = socketserver.ThreadingTCPServer(("127.0.0.1", HOOK_PORT), H)
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    log("hooks on http://127.0.0.1:%d/game/start|end" % HOOK_PORT)


# ------------------------------------------------------------------------------------------------ check

def check(popper_dir, args):
    lines = []

    def out(s=""):
        lines.append(s)
        print(s)

    out("nucleo_bridge %s check, %s" % (VERSION, time.strftime("%Y-%m-%d %H:%M")))
    out("Python %s" % sys.version.split()[0])
    for mod in ("serial", "PIL", "numpy"):
        try:
            m = __import__(mod)
            out("  %-7s %s" % (mod, getattr(m, "__version__", getattr(m, "VERSION", "ok"))))
        except ImportError:
            out("  %-7s MISSING%s" % (mod, "" if mod == "numpy" else "  (pip install pyserial pillow)"))
    out("Popper folder: %s" % (popper_dir or "NOT FOUND (use --popper)"))
    if popper_dir:
        p = Popper(popper_dir, args.web_port)
        out("Database: %s %s" % (p.db, "(found)" if os.path.isfile(p.db) else "(MISSING)"))
        try:
            for tname in ("Games", "GamesStats", "Emulators", "PlayListDetails", "GlobalSettings"):
                cols = [r["name"] for r in p.q("PRAGMA table_info(%s)" % tname)]
                out("  %s: %s" % (tname, ", ".join(cols) if cols else "MISSING"))
            for which in ("recent", "new", "top", "fav", "all"):
                items = p.tables(which, 5)
                out("  list %-6s %d -> %s" % (which, len(items), "; ".join(i["name"] for i in items[:5])))
            emus = p.q("SELECT EMUID, EmuName, DirMedia, Visible, LaunchScript, PostScript FROM Emulators")
            for e in emus:
                out("  emulator %s '%s' media=%s visible=%s" % (e["EMUID"], e["EmuName"], e["DirMedia"], e["Visible"]))
                out("    launch script has hook: %s" % ("58090/game/start" in (e.get("LaunchScript") or "")))
                out("    close script has hook:  %s" % ("58090/game/end" in (e.get("PostScript") or "")))
            first = p.tables("all", 3)
            for i in first:
                for scr in ("Wheel", "GameHelp", "GameInfo"):
                    out("  media %-8s %-30s %s" % (scr, i["name"][:30], p.media_for(i["id"], scr) or "-"))
        except Exception as e:   # noqa: BLE001
            out("  DATABASE ERROR: %s" % e)
        try:
            with urllib.request.urlopen("http://127.0.0.1:%d/function/getcuritem" % args.web_port, timeout=3) as r:
                out("Popper Web Remote: on (port %d, HTTP %d)" % (args.web_port, r.status))
        except Exception as e:   # noqa: BLE001
            out("Popper Web Remote: NOT reachable on port %d (%s) - launching tables needs it" % (args.web_port, e))
    try:
        from serial.tools import list_ports
        ports = [(p.device, p.vid, p.pid, p.description) for p in list_ports.comports()]
        out("Serial ports:")
        for d, v, pi, desc in ports:
            mark = "  <- the board" if (v, pi) == BOARD_VID_PID else ""
            out("  %s %s:%s %s%s" % (d, "%04X" % v if v else "-", "%04X" % pi if pi else "-", desc, mark))
    except ImportError:
        pass
    ini = None
    for cand in (r"C:\vPinball\VisualPinball\VPinMAME\DmdDevice.ini", r"C:\vPinball\VPinMAME\DmdDevice.ini"):
        if os.path.isfile(cand):
            ini = cand
    out("DmdDevice.ini: %s" % (ini or "not found in the usual places"))
    if ini:
        sect = None
        for raw in open(ini, encoding="utf-8", errors="replace"):
            s = raw.strip()
            if s.startswith("["):
                sect = s.lower()
            elif sect in ("[zedmd]", "[networkstream]") and s and not s.startswith(";"):
                out("  %s %s" % (sect, s))
    if args.report:
        with open(args.report, "w", encoding="utf-8") as f:
            f.write("\n".join(lines) + "\n")
        print("report written to", args.report)


def find_popper(arg):
    for d in ([arg] if arg else DEFAULT_POPPER_DIRS):
        if d and os.path.isfile(os.path.join(d, "PUPDatabase.db")):
            return d
    return None


def main():
    ap = argparse.ArgumentParser(description="PinUP Popper <-> NucleoOS DMD Flipper bridge (USB)")
    ap.add_argument("--popper", help="PinUPSystem folder (default: found in the usual places)")
    ap.add_argument("--port", help="the board's COM port (default: found by its USB id 303A:1001)")
    ap.add_argument("--web-port", type=int, default=80, help="Popper Web Remote port (default 80)")
    ap.add_argument("--check", action="store_true", help="report what the bridge finds and exit")
    ap.add_argument("--report", help="with --check: also write the report to this file")
    ap.add_argument("--log", help="write the log to this file instead of the console")
    a = ap.parse_args()
    global LOG_FILE
    LOG_FILE = a.log
    popper_dir = find_popper(a.popper)
    if a.check:
        check(popper_dir, a)
        return
    if not popper_dir:
        sys.exit("PinUP Popper not found: give its folder with --popper (the one with PUPDatabase.db)")
    low_priority()
    link = Link(a.port, Popper(popper_dir, a.web_port))
    link.adopt_running_game()   # before the port: dmdext may hold it for a table already running
    hook_server(link)
    threading.Thread(target=link.follow_popper, daemon=True).start()
    threading.Thread(target=link.prepare_fulldmd, daemon=True).start()
    threading.Thread(target=link.prepare_pictures, daemon=True).start()
    log("nucleo_bridge %s, Popper in %s" % (VERSION, popper_dir))
    log("second screen:", link.s2)
    try:
        link.reader()
    except KeyboardInterrupt:
        link.alive = False
        link.close()


if __name__ == "__main__":
    main()
