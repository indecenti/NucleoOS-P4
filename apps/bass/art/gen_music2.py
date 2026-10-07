"""gen_music2.py - Vertice Bass 2.0 soundtrack: full loopable tracks for the OS mixer (ABI v15),
which streams music from the SD under the effects.

Each track is generated a little longer than its loop, cut on a whole number of bars and the tail
cross-faded over the head, so it repeats without a seam. Saved as 24 kHz mono WAV (half the space of
48 kHz; the mixer resamples) before mastering (see the mastering skill), peaks kept low for the
board's amplifier.

    python apps/bass/art/gen_music2.py [names]       # -> apps/bass/snd/<name>.wav
"""
import os
import sys
import wave

import numpy as np

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "..", "tools"))
import ace_music as a

OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "snd") + "/"
RAW = os.path.join(os.environ.get("TEMP", "/tmp"), "bass_music") + "/"
os.makedirs(RAW, exist_ok=True)
BASE = ("instrumental, 1990s arcade video game music, Konami style, catchy memorable melody, "
        "punchy mix, loopable, ")
# name, tags, bpm, key, seed, bars in the loop
TRACKS = [
    ("play0", BASE + "upbeat bass fishing tournament, sunny alpine lake morning, bright electric guitar riff, "
              "slap bass, synth brass stabs, driving drums, optimistic", 124, "G major", 201, 32),
    ("play1", BASE + "laid-back sunset marsh, funky groove, wah guitar, electric piano, warm synth lead, "
              "swinging drums, relaxed but lively", 100, "F major", 202, 24),
    ("play2", BASE + "night dam, cool mysterious groove, pulsing synth bass, glassy arpeggios, "
              "tense filtered drums, moonlight", 104, "D minor", 203, 24),
    ("play3", BASE + "red canyon, western rock, twangy baritone guitar, harmonica-like synth, "
              "galloping drums, adventurous", 128, "E minor", 204, 32),
    ("play4", BASE + "autumn lake, gentle rock ballad tempo, clean guitar arpeggios, piano, "
              "warm strings, nostalgic and hopeful", 112, "A minor", 205, 28),
    ("play5", BASE + "championship final, heroic rock anthem, soaring guitar lead, orchestral brass, "
              "big drums, epic and triumphant", 136, "C major", 206, 32),
    ("menu2", BASE + "title screen theme of a bass fishing arcade game, catchy rock shuffle, "
              "bright guitar and synth brass, inviting", 120, "D major", 207, 24),
    ("fight", BASE + "fish on! intense fight music, fast driving rock, aggressive guitar riff, pounding drums, "
              "rising synth brass, urgent and exciting", 150, "E minor", 209, 32),
    ("champ", "instrumental, triumphant arcade champion ending theme, rock band with brass and bells, "
              "celebration, 1990s Konami style, big finish", 132, "C major", 208, 0),
]


def save24(x, rate, path):
    """Mono float -> 24 kHz 16-bit, peak 0.35."""
    if x.ndim > 1:
        x = x.mean(axis=0) if x.shape[0] <= 2 else x.mean(axis=1)
    n = int(len(x) * 24000 / rate)
    xi = np.interp(np.linspace(0, len(x) - 1, n), np.arange(len(x)), x)
    xi = xi / (np.max(np.abs(xi)) + 1e-9) * 0.35
    with wave.open(path, "wb") as w:
        w.setnchannels(1); w.setsampwidth(2); w.setframerate(24000)
        w.writeframes((xi * 32767).astype("<i2").tobytes())


def loop_cut(x, rate, bpm, bars, xfade=1.5):
    """A loop of `bars` bars starting after the intro (bar 2), tail cross-faded over the head."""
    bar = 4 * 60.0 / bpm
    start = int(bar * 2 * rate)
    n = int(bar * bars * rate)
    f = int(xfade * rate)
    seg = x[start: start + n + f].copy()
    if len(seg) < n + f:
        return x
    r = np.linspace(0, 1, f)
    out = seg[:n].copy()
    out[:f] = seg[:f] * r + seg[n: n + f] * (1 - r)
    return out


only = sys.argv[1:]
for name, tags, bpm, key, seed, bars in TRACKS:
    if only and name not in only:
        continue
    secs = (4 * 60.0 / bpm) * (bars + 3) + 2 if bars else 24
    data, rate = a.generate(tags, secs, bpm, key, seed)
    x = np.asarray(data, dtype=np.float32)
    if x.ndim > 1:
        x = x.mean(axis=0) if x.shape[0] <= 2 else x.mean(axis=1)
    np.save(RAW + name + ".npy", x)
    with open(RAW + name + ".rate", "w") as fh:
        fh.write(str(rate))
    if bars:
        x = loop_cut(x, rate, bpm, bars)
    save24(x, rate, OUT + name + ".wav")
    print(name, "ok", round(len(x) / rate, 1), "s", flush=True)
