"""Future Pinball scores drawn by the bridge, for tables played with FP's own backbox off.

FP's backbox on the second monitor leaks memory with this cabinet's AMD OpenGL driver (5-11 MB/s on the EM
tables: FP dies after a few minutes). With the backbox off, Popper shows the table's backglass picture there,
and the scores come from FP's memory instead (fp_scores, by the FP session): this module draws them

  - in the score windows of that picture, on the backglass monitor (small topmost windows, one per display,
    redrawn only when a value changes), and
  - on the board, as its DMD (the 'D' message: a 256x64 RGB565 picture).

Light on the cabinet PC: the values are read a few times a second, nothing is drawn while they stay the same.
"""
import os
import queue
import struct
import threading
import time
import zlib

POLL_S = 0.2                  # values read 5 times a second
SEG_ON = (255, 150, 30)       # lit segment (amber), on the board and on lit displays
SEG_OFF = (40, 20, 6)         # unlit segment, faintly visible like a real display
REEL_BG = (232, 222, 196)     # score reel: cream
REEL_FG = (20, 20, 20)

# segments of a 7-segment digit: a b c d e f g
_SEGS = {"0": "abcdef", "1": "bc", "2": "abged", "3": "abgcd", "4": "fgbc", "5": "afgcd", "6": "afgedc",
         "7": "abc", "8": "abcdefg", "9": "abcdfg", " ": ""}


def _seg_polys(x, y, w, h):
    """The 7 segments of a digit cell (x, y, w, h) as polygons, slightly slanted like a real display."""
    t = max(2.0, min(w, h) * 0.16)          # segment thickness
    sl = w * 0.08                           # slant
    def P(px, py):                           # slant with height
        return (x + px + sl * (1 - py / h), y + py)
    hm = h / 2
    L, R = t / 2, w - t / 2                 # centre lines of the vertical segments
    g = t * 0.3                             # gap between segments
    def horiz(cy):
        return [P(L + g, cy), P(L + g + t / 2, cy - t / 2), P(R - g - t / 2, cy - t / 2), P(R - g, cy),
                P(R - g - t / 2, cy + t / 2), P(L + g + t / 2, cy + t / 2)]
    def vert(cx, y0, y1):
        return [P(cx, y0 + g), P(cx + t / 2, y0 + g + t / 2), P(cx + t / 2, y1 - g - t / 2), P(cx, y1 - g),
                P(cx - t / 2, y1 - g - t / 2), P(cx - t / 2, y0 + g + t / 2)]
    return {"a": horiz(t / 2), "g": horiz(hm), "d": horiz(h - t / 2),
            "f": vert(L, t / 2, hm), "b": vert(R, t / 2, hm), "e": vert(L, hm, h - t / 2), "c": vert(R, hm, h - t / 2)}


def digits_of(value, n):
    """A score as n characters, right aligned, blanks before it (an EM display: no leading zeros) and '0' when
    the score is 0, like the machines."""
    s = str(max(0, int(value)))
    if len(s) > n:
        s = s[-n:]
    return s.rjust(n)


def draw_seg_display(draw, box, text, on=SEG_ON, off=SEG_OFF, bg=(0, 0, 0)):
    """A row of 7-segment digits filling box (x, y, w, h) on a PIL ImageDraw."""
    x, y, w, h = box
    draw.rectangle((x, y, x + w - 1, y + h - 1), fill=bg)
    n = max(1, len(text))
    cw = w / n
    pad_y = h * 0.12
    for i, ch in enumerate(text):
        polys = _seg_polys(x + i * cw + cw * 0.12, y + pad_y, cw * 0.76, h - 2 * pad_y)
        lit = _SEGS.get(ch, "")
        for s, poly in polys.items():
            draw.polygon(poly, fill=on if s in lit else off)


def board_picture(cells, values, ndig, w=256, h=64):
    """The board's DMD picture: each value as a 7-segment display in its cell (score_layout's cells:
    (dx, dy, dw, dh, source box))."""
    from PIL import Image, ImageDraw
    img = Image.new("RGB", (w, h), (0, 0, 0))
    d = ImageDraw.Draw(img)
    for (dx, dy, dw, dh, _), v, n in zip(cells, values, ndig):
        draw_seg_display(d, (dx, dy, dw, dh), digits_of(v, n))
    return img




def _hex(c):
    return "#%02x%02x%02x" % tuple(int(v) for v in c[:3])


KEY = "#ff00fe"   # the overlay's see-through colour: the backglass picture shows between our drums


def _no_focus(top):
    """A window of ours never takes the keyboard from the game (FP reads its keys from the foreground window):
    no activation, no taskbar button. Windows only."""
    if os.name != "nt":
        return
    import ctypes
    u = ctypes.windll.user32
    u.GetParent.restype = ctypes.c_void_p
    u.GetWindowLongW.restype = ctypes.c_long
    hwnd = u.GetParent(ctypes.c_void_p(top.winfo_id())) or top.winfo_id()   # Tk's frame around its window
    GWL_EXSTYLE, WS_EX_NOACTIVATE, WS_EX_TOOLWINDOW = -20, 0x08000000, 0x00000080
    style = u.GetWindowLongW(ctypes.c_void_p(hwnd), GWL_EXSTYLE)
    u.SetWindowLongW(ctypes.c_void_p(hwnd), GWL_EXSTYLE, style | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW)
    return hwnd


def _foreground():
    if os.name != "nt":
        return None
    import ctypes
    ctypes.windll.user32.GetForegroundWindow.restype = ctypes.c_void_p
    return ctypes.windll.user32.GetForegroundWindow()


def _give_back(fg):
    """The keyboard back to the window that had it (the game) if showing ours took it."""
    if os.name != "nt" or not fg:
        return
    import ctypes
    if _foreground() != fg:
        ctypes.windll.user32.SetForegroundWindow(ctypes.c_void_p(fg))


def _on_top(hwnd):
    """Back above every other window, without activating it (Popper's windows may come over ours)."""
    if os.name != "nt" or not hwnd:
        return
    import ctypes
    HWND_TOPMOST, SWP_NOSIZE, SWP_NOMOVE, SWP_NOACTIVATE = -1, 0x1, 0x2, 0x10
    ctypes.windll.user32.SetWindowPos(ctypes.c_void_p(hwnd), ctypes.c_void_p(HWND_TOPMOST), 0, 0, 0, 0,
                                      SWP_NOSIZE | SWP_NOMOVE | SWP_NOACTIVATE)


class Overlay:
    """Our windows on the backglass monitor, in their own Tk thread (Tk must stay in one thread), fed through a
    queue: the backglass picture on the whole monitor when it is ours to show (background: (PIL image, (x, y, w,
    h) on the desktop)), and topmost windows over its score windows, each redrawn only when its value changes.
    None of them ever takes the keyboard from the game.
    displays (set_displays): [{"box": (x, y, w, h) on the desktop, "style": "reel" | "lit", "digits": n,
                "cells": [(x, y, w, h) inside the box, one per digit] or None (n even cells),
                "face", "ink", "edge": colours of the reels}]"""

    def __init__(self, background=None):
        self.background = background
        self.displays = []
        self.q = queue.Queue()
        self.ready = threading.Event()
        self.t = threading.Thread(target=self._run, daemon=True)
        self.t.start()
        self.ready.wait(5)

    def set_displays(self, displays):
        self.q.put(("displays", list(displays)))

    def show(self, values):
        self.q.put(("show", list(values)))

    def close(self):
        self.q.put(("close", None))
        self.t.join(3)

    def _run(self):
        import tkinter as tk
        root = tk.Tk()
        root.withdraw()
        wins, shown, hwnds = [], [], []
        photo = None
        fg = _foreground()
        if self.background:   # the backglass on the whole monitor, under the score windows
            import base64
            import io
            img, (x, y, w, h) = self.background
            buf = io.BytesIO()
            img.convert("RGB").resize((w, h)).save(buf, "PNG", compress_level=1)
            bg = tk.Toplevel(root)
            bg.overrideredirect(True)
            bg.attributes("-topmost", True)
            bg.geometry("%dx%d+%d+%d" % (w, h, x, y))
            photo = tk.PhotoImage(master=bg, data=base64.b64encode(buf.getvalue()))
            tk.Label(bg, image=photo, bd=0, highlightthickness=0).pack()
            bg.update_idletasks()
            hwnds.append(_no_focus(bg))
            bg.update()
            _give_back(fg)
        self.ready.set()

        def make(displays):
            fg = _foreground()
            for d in displays:
                x, y, w, h = d["box"]
                top = tk.Toplevel(root)
                top.overrideredirect(True)
                top.attributes("-topmost", True)
                if d.get("cells"):   # our drums only, the picture around them
                    top.attributes("-transparentcolor", KEY)
                top.geometry("%dx%d+%d+%d" % (w, h, x, y))
                c = tk.Canvas(top, width=w, height=h, highlightthickness=0, bd=0,
                              bg=KEY if d.get("cells") else ("#000000" if d["style"] == "lit" else _hex(REEL_BG)))
                c.pack()
                top.update_idletasks()
                hwnds.append(_no_focus(top))
                wins.append((top, c))
                shown.append(None)
                self.displays.append(d)
            root.update()
            _give_back(fg)

        def draw(i, v):
            d = self.displays[i]
            c = wins[i][1]
            w, h = d["box"][2], d["box"][3]
            c.delete("all")
            text = digits_of(v, d["digits"])
            n = len(text)
            cells = d.get("cells") or [(k * w / n, 0, w / n, h) for k in range(n)]
            for (cx, cy, cw, ch), digit in zip(cells, text):
                if d["style"] == "reel":   # a drum: its face, its edge, the digit (a reel shows 0, never blank)
                    c.create_rectangle(cx, cy, cx + cw - 1, cy + ch - 1, fill=_hex(d.get("face", REEL_BG)),
                                       outline=_hex(d.get("edge", (138, 130, 108))), width=max(1, int(cw * 0.05)))
                    c.create_text(cx + cw / 2, cy + ch / 2, text=digit if digit != " " else "0",
                                  fill=_hex(d.get("ink", REEL_FG)), font=("Arial Black", -int(ch * 0.78)))
                else:   # a lit display: amber 7-segment digits
                    polys = _seg_polys(cx + cw * 0.12, cy + ch * 0.12, cw * 0.76, ch * 0.76)
                    lit = _SEGS.get(digit, "")
                    for s, poly in polys.items():
                        c.create_polygon([q for xy in poly for q in xy], fill=_hex(SEG_ON if s in lit else SEG_OFF),
                                         outline="")

        beat = [0]

        def tick():
            try:
                while True:
                    what, arg = self.q.get_nowait()
                    if what == "close":
                        root.destroy()
                        return
                    if what == "displays":
                        make(arg)
                        continue
                    for i, v in enumerate(arg[:len(wins)]):
                        if v != shown[i]:
                            draw(i, v)
                            shown[i] = v
            except queue.Empty:
                pass
            beat[0] += 1
            if beat[0] % 10 == 1:   # once a second: ours above Popper's (the background first, the digits over it)
                for h in hwnds:
                    _on_top(h)
            root.after(100, tick)

        root.after(50, tick)
        try:
            root.mainloop()
        finally:   # every Tk object let go here, in Tk's own thread: freed by another thread, Tcl aborts the process
            wins.clear()
            del draw, tick, make, root, photo
            import gc
            gc.collect()


def box_style(img, box):
    """'reel' for a light window (score reels), 'lit' for a dark one (segments, LEDs)."""
    x, y, w, h = box
    from PIL import ImageStat
    crop = img.crop((x + w // 4, y + h // 4, x + 3 * w // 4, y + 3 * h // 4)).convert("L")
    return "reel" if ImageStat.Stat(crop).mean[0] > 110 else "lit"


def reel_colours(img, reels):
    """The colours of a backglass picture's painted drums: face (light), ink (the digit), edge, so that ours look
    like them."""
    px = []
    for x, y, w, h in reels:
        px += list(img.crop((x, y, x + w, y + h)).convert("RGB").resize((max(1, w // 3), max(1, h // 3))).getdata())
    px.sort(key=sum)
    n = len(px)
    face, ink, edge = px[int(n * 0.85)], px[int(n * 0.04)], px[int(n * 0.25)]

    def lum(c):
        return 0.3 * c[0] + 0.59 * c[1] + 0.11 * c[2]
    if lum(face) < 110:   # empty windows painted dark: the drums of a real machine, cream with black digits
        face, ink, edge = REEL_BG, REEL_FG, (90, 84, 70)
    elif lum(face) - lum(ink) < 90:   # too little contrast for a digit read from the cabinet: black on the face
        ink = REEL_FG
    return {"face": face, "ink": ink, "edge": edge}


PIC_W, PIC_H = 1280, 750   # the space of fp_scores.json: the backglass picture stretched to 1280x750


def known_displays():
    """fp_scores.json next to this file: the score windows measured on the tables' backglass pictures, in PIC_W x
    PIC_H space (the picture stretched to it, whatever its own size), so they follow the picture wherever Popper
    shows it (top of the monitor above its DMD, or the whole monitor):
    {"<table file name>": {"reels": [[x, y, w, h], ...]} (one per drum, player 1) or
     {"box": [x, y, w, h], "style": "lit", "digits": n}}"""
    import json
    try:
        with open(os.path.join(os.path.dirname(os.path.abspath(__file__)), "fp_scores.json"), encoding="utf-8") as f:
            return {k: v for k, v in json.load(f).items() if not k.startswith("_")}
    except (OSError, ValueError):
        return {}


def placement(pic, art, rects):
    """Where on the monitor the backglass picture is shown: the one of rects (x, y, w, h, on the monitor) whose
    content looks most like art (the picture, stretched), or None when none does (Popper showing something else).
    Compared small and grey: a fraction of a second, once."""
    from PIL import ImageChops, ImageStat
    best, bd = None, None
    for r in rects:
        x, y, w, h = r
        if w < 50 or h < 50 or x < 0 or y < 0 or x + w > pic.width or y + h > pic.height:
            continue
        a = art.convert("L").resize((64, 48))
        b = pic.crop((x, y, x + w, y + h)).convert("L").resize((64, 48))
        d = ImageStat.Stat(ImageChops.difference(a, b)).mean[0]
        if bd is None or d < bd:
            best, bd = r, d
    return (best, bd) if best is not None and bd < 20 else (None, bd)


def displays_for(name, pic, screen, find_displays, count, rect=None):
    """The windows where the scores go: measured ones (fp_scores.json, mapped into rect: where the picture is on
    the monitor) or found on the backglass picture."""
    known = known_displays().get(name) if rect else None
    if known:
        rx, ry, rw, rh = rect
        sx, sy = rw / PIC_W, rh / PIC_H

        def m(b):   # picture space -> the monitor
            return (int(round(rx + b[0] * sx)), int(round(ry + b[1] * sy)),
                    max(1, int(round(b[2] * sx))), max(1, int(round(b[3] * sy))))
    if known and known.get("reels"):
        reels = [m(r) for r in known["reels"]]
        x0, y0 = min(r[0] for r in reels), min(r[1] for r in reels)
        x1, y1 = max(r[0] + r[2] for r in reels), max(r[1] + r[3] for r in reels)
        d = {"box": (screen[0] + x0, screen[1] + y0, x1 - x0, y1 - y0), "src": (x0, y0, x1 - x0, y1 - y0),
             "style": "reel", "digits": len(reels), "cells": [(x - x0, y - y0, w, h) for x, y, w, h in reels]}
        d.update(reel_colours(pic, reels))
        return [d]
    if known and known.get("box"):
        x, y, w, h = m(known["box"])
        return [{"box": (screen[0] + x, screen[1] + y, w, h), "src": (x, y, w, h), "style": known.get("style", "lit"),
                 "digits": int(known.get("digits", 6))}]
    if known:   # measured: no score window in the picture (lamps only): the board alone
        return []
    boxes = find_displays(pic)
    # players first (top to bottom, left to right: player 1 first), then credits / ball / match. FP's HUD has
    # usually one display, the score of the player in turn: it goes in player 1's window
    big = sorted((b for b in boxes if b[2] >= 2.8 * b[3]), key=lambda b: (b[1] // 40, b[0]))
    small = [b for b in boxes if b[2] < 2.8 * b[3]]
    return [{"box": (screen[0] + x, screen[1] + y, w, h), "src": (x, y, w, h), "style": box_style(pic, (x, y, w, h)),
             "digits": 6} for x, y, w, h in (big + small)[:count]]


def live(link, table, alive, log, find_displays, score_layout, ScreenCopy, rgb565, fp_pid, art=None, rects=(),
         full=False):
    """The whole loop for one game (bridge thread). fp_pid(): FP's process id once it runs, or None. art: the
    table's backglass picture (Popper's), rects: where Popper may show it on the monitor (x, y, w, h); full: the
    backglass is ours to show, on the whole monitor (chosen on the board), from the start of the game."""
    name = os.path.basename(table)
    screen = list(link.s2) if getattr(link, "s2", None) else [1366, 0, 1280, 1024]
    full = bool(full and art is not None)
    overlay = Overlay((art, tuple(screen))) if full else None
    if full:
        log("FP scores of %s: the backglass on the whole monitor" % name)
    sent = 0
    try:
        try:
            import fp_scores   # the reader of FP's memory (next to the bridge)
        except ImportError:
            log("FP scores: no fp_scores module next to the bridge")
            fp_scores = None
        pid = None
        for _ in range(60):   # FP takes a while to start and load
            if not alive() or not fp_scores:
                break
            pid = fp_pid()
            if pid:
                break
            time.sleep(2)
        elems = []
        for _ in range(12 if pid else 0):   # its score objects exist once the table has loaded
            if not alive():
                break
            try:
                elems = fp_scores.find(pid)
            except Exception as e:   # noqa: BLE001
                log("FP scores of %s: %s" % (name, e))
            if elems:
                break
            time.sleep(5)
        if not elems:
            if alive():
                log("FP scores of %s: %s" % (name, "no score objects in FP's memory" if pid else "FP not found"))
            while alive() and full:   # the backglass stays ours to the end of the game
                time.sleep(1)
            return
        if full:   # our own picture: the windows are where we put it
            pic = art.convert("RGB").resize((screen[2], screen[3]))
            rect, diff = (0, 0, screen[2], screen[3]), 0
        else:   # the backglass picture Popper shows on the second monitor
            sc = ScreenCopy()
            try:
                pic = sc.picture(*screen)
            finally:
                sc.close()
            rect, diff = (placement(pic, art, rects) if art is not None else (None, None))
            if rect is None and art is None and rects:
                rect = rects[0]   # no picture to compare with: where Popper's settings put it
        if name in known_displays() and rect is None:
            log("FP scores of %s: the backglass picture is not on the monitor (difference %s): the board only" % (name, diff))
            disp = []
        else:
            disp = displays_for(name, pic, screen, find_displays, len(elems), rect)
        n = min(len(disp), len(elems))
        log("FP scores of %s: %d score objects, %d windows on the backglass (%s, picture at %s)" %
            (name, len(elems), n, "measured" if name in known_displays() else "found", rect))
        if n:
            if overlay is None:
                overlay = Overlay()
            overlay.set_displays(disp[:n])
            cells = score_layout([d["src"] for d in disp[:n]])
            bndig = [d["digits"] for d in disp[:n]]
        else:   # nothing on the backglass: the board alone
            cells = score_layout([(0, 0, 100, 20)] * min(len(elems), 4))
            bndig = [6] * len(cells)
        head = b'{"w":256,"h":64,"fmt":"rgb565z"}'
        last = None
        while alive():
            try:
                vals = fp_scores.read(pid, elems)
            except Exception:   # noqa: BLE001 - FP gone
                break
            if vals != last:
                last = vals
                shown = vals[:n] if n != 1 else [max(vals)]   # one window, several objects: the score is the largest
                if overlay and n:
                    overlay.show(shown)
                img = board_picture(cells, shown if n else vals[:len(cells)], bndig)
                px = zlib.compress(rgb565(img, 256, 64, (0, 0, 0)), 1)
                if link.send("D", struct.pack("<H", len(head)) + head + px):
                    sent += 1
            time.sleep(POLL_S)
    finally:
        if overlay:
            overlay.close()
        log("FP scores of %s: %d updates sent" % (name, sent))
