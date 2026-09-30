"""gen_assets.py - art, text and sounds for the Chess app (apps/chess).

    python apps/chess/gen_assets.py [--preview <dir>]

Writes, next to this script:
  img/*.565   RGB565 blobs for nv_gfx_image (little-endian u16 w, u16 h, then pixels; 0xF81F =
              transparent). Pieces are pre-blended onto every square colour they can sit on, so
              the device blits one opaque, anti-aliased square per cell (no colour key fringes).
  snd/*.wav   48 kHz mono 16-bit sound effects for nv_sound.
  icon.z      80x80 Store / Home icon, LVGL ARGB8888 byte order (B,G,R,A), raw deflate.
With --preview <dir>: icon.png and a board mock-up there (review only, never shipped).

Pieces are the chess glyphs of DejaVu Sans (free Bitstream Vera / DejaVu licence); UI text uses
Noto Sans (SIL OFL). Both are only rasterised here; no font ships with the app.
"""
import math
import os
import struct
import sys
import wave
import zlib

import numpy as np
from PIL import Image, ImageDraw, ImageFilter, ImageFont

HERE = os.path.dirname(os.path.abspath(__file__))
IMG = os.path.join(HERE, "img")
SND = os.path.join(HERE, "snd")
FONTS = "C:/Windows/Fonts/"
PIECE_FONT = FONTS + "DejaVuSans.ttf"
BOLD = FONTS + "NotoSans-Bold.ttf"
REG = FONTS + "NotoSans-Regular.ttf"

# ---- palette (keep in sync with main.c) ------------------------------------------------------------
SQ = 70
BG = (21, 24, 28)
CARD = (33, 37, 43)
CARD_HI = (46, 51, 59)
TEXT = (236, 238, 240)
MUTED = (140, 148, 158)
GREEN = (118, 150, 86)
GREEN_HI = (138, 172, 102)
LIGHT = (238, 238, 210)
DARK = (118, 150, 86)
LIGHT_HL = (246, 246, 130)
DARK_HL = (186, 202, 68)
RED = (226, 76, 62)
GOLD = (240, 190, 60)

SQUARES = {"l": LIGHT, "d": DARK, "lh": LIGHT_HL, "dh": DARK_HL}
GLYPH_W = {"k": "\u2654", "q": "\u2655", "r": "\u2656", "b": "\u2657", "n": "\u2658", "p": "\u2659"}
GLYPH_B = {"k": "\u265a", "q": "\u265b", "r": "\u265c", "b": "\u265d", "n": "\u265e", "p": "\u265f"}

SS = 4   # supersampling factor for everything drawn here


def save565(name, im, key=False):
    """RGBA image -> img/<name>.565. key=True: alpha<40 becomes the transparent magenta key."""
    im = im.convert("RGBA")
    w, h = im.size
    a = np.asarray(im, dtype=np.uint16)
    r, g, b, al = a[..., 0], a[..., 1], a[..., 2], a[..., 3]
    v = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)
    v = np.where(v == 0xF81F, 0xF81E, v)
    if key:
        v = np.where(al < 40, 0xF81F, v)
    with open(os.path.join(IMG, name + ".565"), "wb") as f:
        f.write(struct.pack("<HH", w, h) + v.astype("<u2").tobytes())


def over(bg, fg):
    """Composite RGBA fg over a solid colour or RGBA image."""
    if isinstance(bg, tuple):
        base = Image.new("RGBA", fg.size, bg + (255,))
    else:
        base = bg.copy()
    base.alpha_composite(fg)
    return base


# ---- pieces ------------------------------------------------------------------------------------------
def glyph_mask(ch, size_px, font_px):
    """8-bit mask of one glyph, centred (by its ink box) in a size_px square, baseline-agnostic."""
    f = ImageFont.truetype(PIECE_FONT, font_px)
    m = Image.new("L", (size_px, size_px), 0)
    d = ImageDraw.Draw(m)
    box = d.textbbox((0, 0), ch, font=f)
    w, h = box[2] - box[0], box[3] - box[1]
    d.text(((size_px - w) / 2 - box[0], (size_px - h) / 2 - box[1] + size_px * 0.01), ch, font=f, fill=255)
    return m


def silhouette(outline_mask):
    """Filled shape of an outline glyph: the outline with its enclosed holes filled."""
    from scipy.ndimage import binary_fill_holes
    a = binary_fill_holes(np.asarray(outline_mask) > 100)
    return Image.fromarray(a.astype(np.uint8) * 255, "L")


def piece_rgba(color, kind, size):
    """Anti-aliased RGBA sprite of a piece, size x size, with a soft contact shadow."""
    S = size * SS
    fp = int(S * 0.97)
    outl = glyph_mask(GLYPH_W[kind], S, fp)
    sil = silhouette(outl)
    sil = Image.fromarray(np.maximum(np.asarray(sil), np.asarray(outl)), "L")
    img = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    # soft shadow
    sh = sil.filter(ImageFilter.GaussianBlur(S * 0.025))
    sh = Image.fromarray((np.asarray(sh) * 0.30).astype(np.uint8), "L")
    shadow = Image.new("RGBA", (S, S), (0, 0, 0, 255))
    shadow.putalpha(sh)
    img.alpha_composite(shadow, (0, int(S * 0.025)))
    if color == "w":
        body = Image.new("RGBA", (S, S), (250, 250, 247, 255)); body.putalpha(sil)
        img.alpha_composite(body)
        line = Image.new("RGBA", (S, S), (28, 28, 30, 255)); line.putalpha(outl)
        img.alpha_composite(line)
    else:
        filled = glyph_mask(GLYPH_B[kind], S, fp)
        body = Image.new("RGBA", (S, S), (44, 44, 48, 255)); body.putalpha(sil)
        img.alpha_composite(body)
        # detail lines = the holes the filled glyph leaves inside the silhouette
        holes = np.clip(np.asarray(sil, np.int16) - np.asarray(filled, np.int16), 0, 255).astype(np.uint8)
        holes = Image.fromarray(holes, "L").filter(ImageFilter.MinFilter(3))
        det = Image.new("RGBA", (S, S), (190, 190, 186, 255)); det.putalpha(holes)
        img.alpha_composite(det)
        rim = Image.new("RGBA", (S, S), (12, 12, 14, 255)); rim.putalpha(outl)
        img.alpha_composite(rim)
    return img.resize((size, size), Image.LANCZOS)


def square_bg(name):
    """A square background (RGBA, SQ x SQ). 'c' = check: red radial glow under the king."""
    if name.endswith("c"):
        base = SQUARES[name[:-1]]
        S = SQ * SS
        yy, xx = np.mgrid[0:S, 0:S]
        r = np.sqrt((xx - S / 2) ** 2 + (yy - S / 2) ** 2) / (S / 2)
        t = np.clip(1.15 - r, 0, 1) ** 0.8
        col = np.zeros((S, S, 4), np.float32)
        for i in range(3):
            col[..., i] = base[i] * (1 - t) + RED[i] * t
        col[..., 3] = 255
        return Image.fromarray(col.astype(np.uint8), "RGBA").resize((SQ, SQ), Image.LANCZOS)
    return Image.new("RGBA", (SQ, SQ), SQUARES[name] + (255,))


def gen_pieces():
    for c in "wb":
        for k in "kqrbnp":
            spr = piece_rgba(c, k, SQ)
            for bg in ("l", "d", "lh", "dh"):
                save565(f"{c}{k}_{bg}", over(square_bg(bg), spr))
            if k == "k":
                for bg in ("lc", "dc"):
                    save565(f"{c}{k}_{bg}", over(square_bg(bg), spr))
            if k != "k":   # small sprites for the captured-pieces tray, keyed on the card colour
                small = piece_rgba(c, k, 30)
                if c == "b":   # a light rim so dark pieces read on the dark card
                    al = small.getchannel("A").filter(ImageFilter.MaxFilter(3))
                    rim = Image.new("RGBA", small.size, (168, 176, 188, 255)); rim.putalpha(al)
                    rim.alpha_composite(small)
                    small = rim
                a = np.asarray(small).copy()
                out = over(CARD, small)
                o = np.asarray(out).copy()
                o[..., 3] = a[..., 3]
                save565(f"s_{c}{k}", Image.fromarray(o, "RGBA"), key=True)
    # move-target dots on empty squares
    for bg in ("l", "d", "lh", "dh"):
        base = SQUARES[bg]
        S = SQ * SS
        im = Image.new("RGBA", (S, S), base + (255,))
        d = ImageDraw.Draw(im)
        dot = tuple(int(v * 0.80) for v in base)
        r = S * 0.16
        d.ellipse((S / 2 - r, S / 2 - r, S / 2 + r, S / 2 + r), fill=dot + (255,))
        save565(f"dot_{bg}", im.resize((SQ, SQ), Image.LANCZOS))


# ---- text / UI ---------------------------------------------------------------------------------------
def rounded(w, h, radius, fill, bg=BG, outline=None):
    S = SS
    im = Image.new("RGBA", (w * S, h * S), bg + (255,))
    d = ImageDraw.Draw(im)
    d.rounded_rectangle((0, 0, w * S - 1, h * S - 1), radius=radius * S, fill=fill + (255,),
                        outline=(outline + (255,)) if outline else None, width=int(1.5 * S) if outline else 0)
    return im, d


def text_on(im, d, s, font_path, px, color, x=None, cy=None, align="center"):
    """Draw text on a supersampled image; x/cy in final pixels (None = centred). Vertical centring
    uses the font's ascender/descender middle (anchor "?m"), so labels share one baseline whether
    or not they have descenders."""
    S = SS
    f = ImageFont.truetype(font_path, px * S)
    W, H = im.size
    y = (cy * S) if cy is not None else H / 2
    if x is None:
        d.text((W / 2, y), s, font=f, fill=color + (255,), anchor="mm")
    elif align == "left":
        d.text((x * S, y), s, font=f, fill=color + (255,), anchor="lm")
    else:
        d.text((x * S, y), s, font=f, fill=color + (255,), anchor="rm")
    return f.getlength(s) / S


def finish(im):
    W, H = im.size
    return im.resize((W // SS, H // SS), Image.LANCZOS)


L10N = {
    "it": {
        "title": "Scacchi", "level": "LIVELLO", "lv": ["Facile", "Medio", "Difficile"],
        "undo": "Annulla", "new": "Nuova partita", "confirm": "Conferma?",
        "you": "Tu", "cpu": "Computer",
        "st": ["Tocca a te", "Il computer pensa", "Scacco! Tocca a te", "Scacco matto: hai vinto!",
               "Scacco matto: hai perso", "Patta per stallo", "Patta: pezzi insufficienti",
               "Patta per ripetizione", "Patta: regola delle 50 mosse"],
        "hint": ["Tocca un tuo pezzo, poi la casella", "di arrivo. Tu hai il bianco."],
        "hint_end": ["Partita finita. Tocca", "Nuova partita per rigiocare."],
    },
    "en": {
        "title": "Chess", "level": "LEVEL", "lv": ["Easy", "Medium", "Hard"],
        "undo": "Undo", "new": "New game", "confirm": "Confirm?",
        "you": "You", "cpu": "Computer",
        "st": ["Your move", "Computer is thinking", "Check! Your move", "Checkmate: you win!",
               "Checkmate: you lose", "Draw by stalemate", "Draw: insufficient material",
               "Draw by repetition", "Draw: 50-move rule"],
        "hint": ["Tap one of your pieces, then the", "square to move to. You play white."],
        "hint_end": ["Game over. Tap New game", "to play again."],
    },
}
ST_DOT = [(250, 250, 247), (60, 64, 70), RED, GOLD, (120, 126, 134), (110, 160, 230), (110, 160, 230),
          (110, 160, 230), (110, 160, 230)]
PW = 380   # right panel width


def gen_ui():
    for lang, t in L10N.items():
        # title: knight glyph + name
        im, d = rounded(PW, 52, 0, BG)
        S = SS
        kn = piece_rgba("w", "n", 46)
        im.alpha_composite(kn.resize((46 * S, 46 * S), Image.LANCZOS), (0, 3 * S))
        text_on(im, d, t["title"], BOLD, 34, TEXT, x=56, cy=26, align="left")
        save565(f"title_{lang}", finish(im))
        # status cards
        for i, s in enumerate(t["st"]):
            im, d = rounded(PW, 64, 14, CARD)
            cx, cy, r = 30 * S, 32 * S, 9 * S
            d.ellipse((cx - r, cy - r, cx + r, cy + r), fill=ST_DOT[i] + (255,),
                      outline=(90, 96, 104, 255) if i == 0 else None, width=S)
            text_on(im, d, s, BOLD, 22, TEXT, x=52, cy=32, align="left")
            save565(f"st{i}_{lang}", finish(im))
        # captured tray labels
        for key in ("you", "cpu"):
            im, d = rounded(100, 30, 0, CARD, bg=CARD)
            text_on(im, d, t[key], BOLD, 17, MUTED, x=0, cy=15, align="left")
            save565(f"lb_{key}_{lang}", finish(im))
        im, d = rounded(PW, 22, 0, BG)
        text_on(im, d, t["level"], BOLD, 14, MUTED, x=4, cy=11, align="left")
        save565(f"lb_level_{lang}", finish(im))
        # level segments
        sw = (PW - 16) // 3
        for i, s in enumerate(t["lv"]):
            for on in (0, 1):
                im, d = rounded(sw, 56, 14, GREEN if on else CARD)
                text_on(im, d, s, BOLD, 20, (255, 255, 255) if on else MUTED)
                save565(f"lv{i}{'on' if on else 'off'}_{lang}", finish(im))
        # buttons
        bw = (PW - 12) // 2
        for name, label, fill, col in (("undo_n", t["undo"], CARD, TEXT), ("undo_p", t["undo"], CARD_HI, TEXT),
                                       ("undo_x", t["undo"], CARD, (82, 88, 96)),
                                       ("new_n", t["new"], GREEN, (255, 255, 255)),
                                       ("new_p", t["new"], GREEN_HI, (255, 255, 255)),
                                       ("new_c", t["confirm"], RED, (255, 255, 255))):
            im, d = rounded(bw, 64, 16, fill)
            text_on(im, d, label, BOLD, 22, col)
            save565(f"b_{name}_{lang}", finish(im))
        # hints
        for key in ("hint", "hint_end"):
            im, d = rounded(PW, 56, 0, BG)
            text_on(im, d, t[key][0], REG, 17, MUTED, x=4, cy=16, align="left")
            text_on(im, d, t[key][1], REG, 17, MUTED, x=4, cy=40, align="left")
            save565(f"{key}_{lang}", finish(im))
    im, d = rounded(PW, 104, 14, CARD)
    save565("tray", finish(im))
    # material advantage digits on the card
    for ch, name in [(str(i), f"n{i}") for i in range(10)] + [("+", "nplus")]:
        im, d = rounded(12, 26, 0, CARD, bg=CARD)
        text_on(im, d, ch, BOLD, 18, MUTED)
        save565(name, finish(im))
    # board coordinates (two halves each: gfx_image assets are at most 512 px a side)
    f = ImageFont.truetype(BOLD, 14 * SS)
    for half in range(2):
        im, d = rounded(4 * SQ, 20, 0, BG)
        for i, ch in enumerate("abcdefgh"[half * 4:half * 4 + 4]):
            d.text(((i * SQ + SQ / 2) * SS, 10 * SS), ch, font=f, fill=MUTED + (255,), anchor="mm")
        save565(f"files{half}", finish(im))
        im, d = rounded(20, 4 * SQ, 0, BG)
        for i, ch in enumerate("87654321"[half * 4:half * 4 + 4]):
            d.text((10 * SS, (i * SQ + SQ / 2) * SS), ch, font=f, fill=MUTED + (255,), anchor="mm")
        save565(f"ranks{half}", finish(im))


# ---- icon --------------------------------------------------------------------------------------------
def gen_icon(preview_dir=None):
    N, S = 80, 8
    W = N * S
    yy, xx = np.mgrid[0:W, 0:W]
    t = (xx + yy) / (2 * W)
    top, bot = np.array((58, 128, 78)), np.array((22, 70, 42))
    col = top[None, None, :] * (1 - t[..., None]) + bot[None, None, :] * t[..., None]
    tile = Image.fromarray(np.dstack([col, np.full((W, W), 255)]).astype(np.uint8), "RGBA")
    # faint checkerboard in the lower third
    chk = Image.new("RGBA", (W, W), (0, 0, 0, 0))
    d = ImageDraw.Draw(chk)
    c = W / 8
    for r in range(5, 8):
        for f in range(8):
            if (r + f) % 2:
                d.rectangle((f * c, r * c, (f + 1) * c, (r + 1) * c), fill=(255, 255, 255, 30 + 12 * (r - 5)))
    tile.alpha_composite(chk)
    mask = Image.new("L", (W, W), 0)
    ImageDraw.Draw(mask).rounded_rectangle((2 * S, 2 * S, (N - 2) * S, (N - 2) * S), radius=18 * S, fill=255)
    # knight: white body, dark outline, soft shadow
    size = int(W * 0.84)
    fp = int(size * 0.80)
    outl = glyph_mask(GLYPH_W["n"], size, fp)
    sil = silhouette(outl)
    sil = Image.fromarray(np.maximum(np.asarray(sil), np.asarray(outl)), "L")
    ox, oy = (W - size) // 2, int(W * 0.05)
    sh = Image.new("L", (W, W), 0); sh.paste(sil, (ox + int(W * 0.02), oy + int(W * 0.035)))
    sh = sh.filter(ImageFilter.GaussianBlur(W * 0.02))
    shadow = Image.new("RGBA", (W, W), (0, 20, 8, 255))
    shadow.putalpha(Image.fromarray((np.asarray(sh) * 0.55).astype(np.uint8)))
    tile.alpha_composite(shadow)
    body = Image.new("RGBA", (size, size), (252, 252, 248, 255)); body.putalpha(sil)
    tile.alpha_composite(body, (ox, oy))
    line = Image.new("RGBA", (size, size), (18, 40, 26, 255)); line.putalpha(outl)
    tile.alpha_composite(line, (ox, oy))
    a = np.asarray(tile).copy()
    a[..., 3] = np.asarray(mask)
    img = Image.fromarray(a, "RGBA").resize((N, N), Image.LANCZOS)
    raw = img.tobytes("raw", "BGRA")
    co = zlib.compressobj(9, zlib.DEFLATED, -15)
    open(os.path.join(HERE, "icon.z"), "wb").write(co.compress(raw) + co.flush())
    if preview_dir:
        img.save(os.path.join(preview_dir, "icon.png"))
        img.resize((320, 320), Image.NEAREST).save(os.path.join(preview_dir, "icon_x4.png"))


# ---- sounds ------------------------------------------------------------------------------------------
RATE = 48000


def wav(name, y):
    y = np.tanh(y * 1.2) / np.tanh(1.2)
    pcm = (np.clip(y, -1, 1) * 0.30 * 32767).astype("<i2")
    with wave.open(os.path.join(SND, name + ".wav"), "wb") as w:
        w.setnchannels(1); w.setsampwidth(2); w.setframerate(RATE)
        w.writeframes(pcm.tobytes())


def knock(freqs, dur, decay, noise=0.35, seed=1):
    t = np.arange(int(RATE * dur)) / RATE
    rng = np.random.default_rng(seed)
    y = np.zeros_like(t)
    for i, f in enumerate(freqs):
        y += np.sin(2 * np.pi * f * t) * np.exp(-t * decay * (1 + 0.6 * i)) / (1 + i)
    n = rng.standard_normal(len(t))
    n = np.convolve(n, np.ones(6) / 6, mode="same") * np.exp(-t * 90)
    y = y + noise * n
    return y / np.max(np.abs(y))


def bell(notes, dur, gap):
    t = np.arange(int(RATE * dur)) / RATE
    y = np.zeros(int(RATE * (dur + gap * len(notes))))
    for k, f in enumerate(notes):
        s = np.zeros_like(t)
        for p, a in ((1, 1), (2.01, 0.35), (3.03, 0.15), (4.2, 0.06)):
            s += a * np.sin(2 * np.pi * f * p * t) * np.exp(-t * (5 + 3 * p))
        o = int(RATE * gap * k)
        y[o:o + len(t)] += s
    return y / np.max(np.abs(y))


def gen_sounds():
    wav("move", knock([420, 1180], 0.16, 40))
    wav("capture", knock([300, 760, 1650], 0.22, 28, noise=0.5, seed=2))
    wav("check", bell([880, 1175], 0.45, 0.09))
    wav("win", bell([523, 659, 784, 1047], 0.8, 0.12))
    wav("lose", bell([440, 370, 294], 0.9, 0.16))
    wav("draw", bell([587, 587], 0.6, 0.18))


def preview(dir_):
    """A static board mock-up from the generated blobs (sanity check of the art)."""
    def load(name):
        b = open(os.path.join(IMG, name + ".565"), "rb").read()
        w, h = struct.unpack("<HH", b[:4])
        v = np.frombuffer(b[4:], "<u2").reshape(h, w).astype(np.uint32)
        rgb = np.dstack([((v >> 11) & 31) * 255 // 31, ((v >> 5) & 63) * 255 // 63, (v & 31) * 255 // 31])
        return Image.fromarray(rgb.astype(np.uint8), "RGB")
    out = Image.new("RGB", (8 * SQ, 8 * SQ))
    back = "rnbqkbnr"
    for r in range(8):
        for f in range(8):
            bg = "d" if (r + f) % 2 else "l"
            name = None
            if r == 0: name = "b" + back[f]
            if r == 1: name = "bp"
            if r == 6: name = "wp"
            if r == 7: name = "w" + back[f]
            if name:
                out.paste(load(f"{name}_{bg}{'c' if name == 'wk' else ''}"), (f * SQ, r * SQ))
            elif r == 4 and f == 4:
                out.paste(load(f"dot_{bg}"), (f * SQ, r * SQ))
            else:
                out.paste(Image.new("RGB", (SQ, SQ), SQUARES[bg]), (f * SQ, r * SQ))
    out.save(os.path.join(dir_, "board_preview.png"))


def main():
    pv = sys.argv[sys.argv.index("--preview") + 1] if "--preview" in sys.argv else None
    os.makedirs(IMG, exist_ok=True)
    os.makedirs(SND, exist_ok=True)
    gen_pieces()
    gen_ui()
    gen_icon(pv)
    gen_sounds()
    if pv:
        preview(pv)
    print("assets:", len(os.listdir(IMG)), "images,", len(os.listdir(SND)), "sounds")


if __name__ == "__main__":
    main()
