"""loudness.py - Vertice Bass: even out the soundtrack. Every music track to the same integrated
loudness (-21 LUFS: the music sits under the effects), true peak kept under -9 dBFS (the board's small
amplifier browns out on loud peaks), with a gentle soft-clip for the few transients above it.

    python apps/bass/art/loudness.py [names]      # default: the gen_music2 tracks
"""
import os
import sys

import numpy as np
import pyloudnorm as pyln
import soundfile as sf

SND = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "bass-music", "snd")   # the soundtrack package
TRACKS = ["play0", "play1", "play2", "play3", "play4", "play5", "menu2", "champ", "fight"]
TARGET, PEAK = -21.0, 10 ** (-9 / 20)

for n in sys.argv[1:] or TRACKS:
    p = os.path.join(SND, n + ".wav")
    x, rate = sf.read(p, dtype="float32")
    m = pyln.Meter(rate)
    before = m.integrated_loudness(x)
    y = pyln.normalize.loudness(x, before, TARGET)
    over = np.abs(y) > PEAK * 0.8                                  # soft knee near the ceiling
    knee = PEAK * 0.8
    y = np.where(over, np.sign(y) * (knee + (PEAK - knee) * np.tanh((np.abs(y) - knee) / (PEAK - knee))), y)
    sf.write(p, y.astype(np.float32), rate, subtype="PCM_16")
    print(f"{n:6s} {before:6.1f} -> {m.integrated_loudness(y):6.1f} LUFS, peak {20 * np.log10(np.max(np.abs(y)) + 1e-9):5.1f} dBFS")
