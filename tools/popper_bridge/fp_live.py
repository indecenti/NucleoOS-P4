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


class Overlay:
    """Small topmost windows over the score windows of the backglass picture, on the backglass monitor; their
    own Tk thread (Tk must stay in one thread), fed through a queue; a window is redrawn only when its value
    changes."""

    def __init__(self, boxes, styles, ndig):
        self.boxes, self.styles, self.ndig = boxes, styles, ndig
        self.q = queue.Queue()
        self.ready = threading.Event()
        self.t = threading.Thread(target=self._run, daemon=True)
        self.t.start()
        self.ready.wait(5)

    def show(self, values):
        self.q.put(("show", list(values)))

    def close(self):
        self.q.put(("close", None))
        self.t.join(3)

    def _run(self):
        import tkinter as tk
        root = tk.Tk()
        root.withdraw()
        wins, shown = [], []
        for (x, y, w, h), style in zip(self.boxes, self.styles):
            top = tk.Toplevel(root)
            top.overrideredirect(True)
            top.attributes("-topmost", True)
            top.geometry("%dx%d+%d+%d" % (w, h, x, y))
            c = tk.Canvas(top, width=w, height=h, highlightthickness=0, bd=0,
                          bg="#%02x%02x%02x" % (REEL_BG if style == "reel" else (0, 0, 0)))
            c.pack()
            wins.append((top, c, w, h, style))
            shown.append(None)
        self.ready.set()

        def draw(i, v):
            top, c, w, h, style = wins[i]
            c.delete("all")
            text = digits_of(v, self.ndig[i])
            n = len(text)
            if style == "reel":   # score reels: black digits on cream drums
                cw = w / n
                for k, ch in enumerate(text):
                    c.create_rectangle(k * cw + 1, 1, (k + 1) * cw - 1, h - 1, fill="#e8dec4", outline="#8a826c")
                    c.create_text((k + 0.5) * cw, h / 2, text=ch if ch != " " else "0", fill="#141414",
                                  font=("Arial Black", -int(h * 0.62)))
            else:   # lit display: amber 7-segment digits
                cw = w / n
                for k, ch in enumerate(text):
                    polys = _seg_polys(k * cw + cw * 0.12, h * 0.12, cw * 0.76, h * 0.76)
                    lit = _SEGS.get(ch, "")
                    for s, poly in polys.items():
                        col = SEG_ON if s in lit else SEG_OFF
                        c.create_polygon([p for xy in poly for p in xy], fill="#%02x%02x%02x" % col, outline="")

        def tick():
            try:
                while True:
                    what, arg = self.q.get_nowait()
                    if what == "close":
                        root.destroy()
                        return
                    for i, v in enumerate(arg[:len(wins)]):
                        if v != shown[i]:
                            draw(i, v)
                            shown[i] = v
                    for top, *_ in wins:   # Popper's windows may come over ours: back on top, cheaply
                        top.lift()
            except queue.Empty:
                pass
            root.after(100, tick)

        root.after(50, tick)
        try:
            root.mainloop()
        finally:   # every Tk object let go here, in Tk's own thread: freed by another thread, Tcl aborts the process
            wins.clear()
            del draw, tick, top, c, root
            import gc
            gc.collect()


def box_style(img, box):
    """'reel' for a light window (score reels), 'lit' for a dark one (segments, LEDs)."""
    x, y, w, h = box
    from PIL import ImageStat
    crop = img.crop((x + w // 4, y + h // 4, x + 3 * w // 4, y + 3 * h // 4)).convert("L")
    return "reel" if ImageStat.Stat(crop).mean[0] > 110 else "lit"


def live(link, table, alive, log, find_displays, score_layout, ScreenCopy, rgb565, fp_pid):
    """The whole loop for one game (bridge thread). fp_pid(): FP's process id once it runs, or None."""
    try:
        import fp_scores   # the reader of FP's memory (next to the bridge)
    except ImportError:
        log("FP scores: no fp_scores module next to the bridge")
        return
    name = os.path.basename(table)
    pid = None
    for _ in range(60):   # FP takes a while to start and load
        if not alive():
            return
        pid = fp_pid()
        if pid:
            break
        time.sleep(2)
    if not pid:
        log("FP scores of %s: FP not found" % name)
        return
    elems = []
    for _ in range(12):   # its score objects exist once the table has loaded
        if not alive():
            return
        try:
            elems = fp_scores.find(pid)
        except Exception as e:   # noqa: BLE001
            log("FP scores of %s: %s" % (name, e))
        if elems:
            break
        time.sleep(5)
    if not elems:
        log("FP scores of %s: no score objects in FP's memory" % name)
        return
    # the score windows of the backglass picture Popper shows on the second monitor
    screen = list(link.s2) if getattr(link, "s2", None) else [1366, 0, 1280, 1024]
    sc = ScreenCopy()
    try:
        pic = sc.picture(*screen)
    finally:
        sc.close()
    boxes = find_displays(pic)
    big = [b for b in boxes if b[2] >= 2.8 * b[3]]
    small = [b for b in boxes if b[2] < 2.8 * b[3]]
    ordered = big + small                       # players first, then credits / ball / match
    n = min(len(ordered), len(elems))
    ndig = [max(1, min(8, e.get("digits") or 6)) for e in elems[:n]]
    log("FP scores of %s: %d score objects, %d windows on the backglass, %d used" % (name, len(elems), len(ordered), n))
    overlay = None
    if n:
        boxes_abs = [(screen[0] + x, screen[1] + y, w, h) for x, y, w, h in ordered[:n]]
        overlay = Overlay(boxes_abs, [box_style(pic, b) for b in ordered[:n]], ndig)
    cells = score_layout(ordered[:n] if n else [(0, 0, 100, 20)] * min(len(elems), 4))
    bndig = ndig if n else [max(1, min(8, e.get("digits") or 6)) for e in elems[:len(cells)]]
    head = b'{"w":256,"h":64,"fmt":"rgb565z"}'
    last, sent = None, 0
    try:
        while alive():
            try:
                vals = fp_scores.read(pid, elems)
            except Exception:   # noqa: BLE001 - FP gone
                break
            if vals != last:
                last = vals
                if overlay:
                    overlay.show(vals[:n])
                img = board_picture(cells, vals[:len(cells)], bndig)
                px = zlib.compress(rgb565(img, 256, 64, (0, 0, 0)), 1)
                if link.send("D", struct.pack("<H", len(head)) + head + px):
                    sent += 1
            time.sleep(POLL_S)
    finally:
        if overlay:
            overlay.close()
        log("FP scores of %s: %d updates sent" % (name, sent))
