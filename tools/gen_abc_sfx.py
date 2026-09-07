#!/usr/bin/env python3
# ABC 123 sound effects — beautiful polyphonic music-box / celesta jingles.
# 48 kHz MONO 16-bit WAV (the format nv_sound() streams: it skips a 44-byte header and assumes
# 48k/mono/16, no resampling on device). Output: apps/abc123/snd/{win,levelup,lose,star,tap}.wav
#
# SYNTHESIS (documented so it can be tuned):
#   * TIMBRE = struck bell / celesta: a note is a sum of PARTIALS at slightly INHARMONIC ratios
#     (1, 2, 3, 4.19, 5.43 ...) — real struck metal is inharmonic, which gives the sparkly "music box"
#     shimmer instead of a flat organ tone. Each partial has its OWN exponential decay (high partials
#     die faster) and a soft ~4 ms attack (no click). A tiny detuned second layer adds chorus warmth.
#   * POLYPHONY: notes are summed into one float buffer. A CHORD = several notes at the same start;
#     an ARPEGGIO = the same notes staggered a few tens of ms. All baked into the sample (device just
#     streams it — no runtime mixing).
#   * SPACE: a light algorithmic reverb (a short exponentially-decaying noise convolution) is mixed in
#     for air/beauty, then everything is soft-clipped (tanh) and peak-normalised to -1 dBFS.
#
# Durations are printed at the end — copy them into abc123's *_MS constants so the app can sequence
# each sound so it NEVER overlaps the voice.
import numpy as np, wave, os, struct

RATE = 48000
OUT  = os.path.join(os.path.dirname(__file__), "..", "apps", "abc123", "snd")
os.makedirs(OUT, exist_ok=True)

A4 = 440.0
STEPS = {"C":-9,"C#":-8,"D":-7,"D#":-6,"E":-5,"F":-4,"F#":-3,"G":-2,"G#":-1,"A":0,"A#":1,"B":2}
def hz(note):
    return A4 * 2.0 ** ((STEPS[note[:-1]] + (int(note[-1]) - 4) * 12) / 12.0)

# inharmonic partial ratios + relative gains + per-partial decay rate (1/s). Bell/celesta-like,
# snappy "music-box pluck" (fast decays) so the jingles are short and never drag over gameplay.
PARTIALS = [(1.00, 1.00, 5.0), (2.00, 0.55, 6.5), (3.00, 0.32, 8.5),
            (4.19, 0.18, 11.0), (5.43, 0.10, 14.0), (6.79, 0.05, 18.0)]

def note(freq, dur, gain=1.0, bright=1.0):
    n = int(dur * RATE); t = np.arange(n) / RATE
    sig = np.zeros(n)
    for ratio, amp, dec in PARTIALS:
        f = freq * ratio
        if f > RATE * 0.45: continue                    # skip partials above Nyquist
        env = np.exp(-dec * t) * (amp ** bright)
        # two very slightly detuned oscillators = gentle chorus/warmth
        sig += env * (np.sin(2*np.pi*f*t) + np.sin(2*np.pi*f*1.003*t)) * 0.5
    atk = int(0.004 * RATE)                              # soft attack, no click
    sig[:atk] *= np.linspace(0, 1, atk)
    return sig * gain

def place(buf, sig, start_s):
    i = int(start_s * RATE); end = min(len(buf), i + len(sig))
    buf[i:end] += sig[:end - i]

def reverb(buf, decay=0.28, mix=0.22):
    ir_n = int(decay * RATE)
    ir = (np.random.RandomState(7).randn(ir_n) * np.exp(-np.arange(ir_n) / (0.10 * RATE)))
    wet = np.convolve(buf, ir)[:len(buf)]
    wet /= (np.max(np.abs(wet)) + 1e-9)
    return buf * (1 - mix) + wet * mix

def render(voices, tail=0.35, rev=True):
    total = max(s + len(sig)/RATE for s, sig in voices) + tail
    buf = np.zeros(int(total * RATE))
    for s, sig in voices: place(buf, sig, s)
    if rev: buf = reverb(buf)
    buf = np.tanh(buf * 1.4)                             # gentle soft-clip for warmth/loudness
    buf /= (np.max(np.abs(buf)) + 1e-9)
    return (buf * 0.94)

def save(name, buf):
    pcm = np.clip(buf, -1, 1)
    data = (pcm * 32767.0).astype("<i2").tobytes()
    with wave.open(os.path.join(OUT, name + ".wav"), "wb") as w:
        w.setnchannels(1); w.setsampwidth(2); w.setframerate(RATE); w.writeframes(data)
    ms = int(len(buf) / RATE * 1000)
    print(f"  {name}.wav  {ms} ms  {len(data)} bytes")
    return ms

def arp(notes, step, dur, gain=1.0, bright=1.0, start=0.0):
    return [(start + i*step, note(hz(nn), dur, gain, bright)) for i, nn in enumerate(notes)]
def chord(notes, dur, gain=1.0, bright=1.0, start=0.0):   # several notes struck together
    return [(start, note(hz(nn), dur, gain, bright)) for nn in notes]

print("generating abc123 SFX:")
ms = {}
# WIN — bright quick up-arpeggio C major + high sparkle over a warm root, short punchy ring
v  = arp(["C6","E6","G6"], 0.038, 0.30, gain=0.85)
v += [(0.0,  note(hz("C5"), 0.34, 0.35, bright=0.6))]           # warm root gives it body
v += [(0.09, note(hz("C7"), 0.24, 0.50, bright=1.3))]           # sparkle top
ms["win"] = save("win", render(v, tail=0.05))
# LEVELUP — ascending run into a big major chord over a bass, with sparkle (triumphant, ~1.1 s)
v  = arp(["C5","E5","G5","C6","E6","G6"], 0.058, 0.42, gain=0.78)
ch = 0.058*6
v += chord(["C6","E6","G6","C7"], 0.62, 0.72, start=ch)
v += [(ch,      note(hz("C4"), 0.60, 0.42, bright=0.5))]        # bass anchor
v += [(ch+0.04, note(hz("E7"), 0.50, 0.32, bright=1.4))]        # sparkle
ms["levelup"] = save("levelup", render(v, tail=0.16))
# LOSE — gentle warm descending two notes over a soft low harmony (not harsh; fires every miss)
v  = [(0.0, note(hz("A4"), 0.28, 0.78, bright=0.7)), (0.13, note(hz("F4"), 0.34, 0.78, bright=0.7))]
v += [(0.0, note(hz("C4"), 0.30, 0.32, bright=0.5))]            # soft cushion underneath
ms["lose"] = save("lose", render(v, tail=0.06, rev=True))
# GAMEOVER — a little melancholic phrase that RESOLVES warm/hopeful (kids: sad-but-ok, ~1.7 s).
# Am pad -> descending melody A-G-E -> lands on a warm F major with a gentle sparkle.
v  = chord(["A3","C4","E4"], 0.95, 0.40, bright=0.6, start=0.0)  # soft minor pad
v += [(0.00, note(hz("A5"), 0.50, 0.60, bright=0.8))]
v += [(0.28, note(hz("G5"), 0.50, 0.60, bright=0.8))]
v += [(0.56, note(hz("E5"), 0.52, 0.60, bright=0.8))]
v += chord(["F3","A3","C4"], 1.05, 0.46, bright=0.6, start=0.86) # warm F major resolve (hopeful)
v += [(0.86, note(hz("F5"), 0.85, 0.60, bright=0.9))]
v += [(0.92, note(hz("C6"), 0.75, 0.28, bright=1.2))]            # tender sparkle on the resolve
ms["gameover"] = save("gameover", render(v, tail=0.22))
# RECORD — a bigger, brighter triumphant fanfare than levelup, with a sparkle rain (~2 s)
v  = arp(["C5","E5","G5","C6","E6","G6","C7"], 0.050, 0.50, gain=0.80)
ch = 0.050*7
v += chord(["C6","E6","G6","C7","E7"], 0.90, 0.70, start=ch)     # big sustained major
v += [(ch, note(hz("C4"), 0.90, 0.44, bright=0.5))]             # bass anchor
for i, nn in enumerate(["G7","E7","C7","G6"]):                   # cascading sparkle rain
    v += [(ch+0.16+i*0.06, note(hz(nn), 0.42, 0.30, bright=1.4))]
v += chord(["C7","E7","G7"], 0.75, 0.34, bright=1.1, start=ch+0.55)   # bright octave-up bloom to finish
ms["record"] = save("record", render(v, tail=0.20))
# UNLOCK — a magical twinkle for a NEW STICKER (ascending sparkle over a soft chord bloom, ~0.8 s)
v  = arp(["E6","G6","C7","E7"], 0.045, 0.34, gain=0.72, bright=1.2)
v += chord(["C6","E6","G6"], 0.55, 0.40, bright=0.9, start=0.14)
v += [(0.20, note(hz("G7"), 0.30, 0.26, bright=1.5))]
ms["unlock"] = save("unlock", render(v, tail=0.10))
# STAR — tiny sparkle chime (optional collectible cue)
v = arp(["G6","C7","E7"], 0.030, 0.26, gain=0.7, bright=1.2)
ms["star"] = save("star", render(v, tail=0.04))
# TAP — very short soft tick
v = [(0.0, note(hz("C6"), 0.08, 0.6, bright=0.8))]
ms["tap"] = save("tap", render(v, tail=0.03, rev=False))

print("\n// abc123 durations (ms):")
print("#define WIN_MS %d" % ms["win"])
print("#define LVL_MS %d" % ms["levelup"])
print("#define LOSE_MS %d" % ms["lose"])
print("#define GAMEOVER_MS %d" % ms["gameover"])
print("#define RECORD_MS %d" % ms["record"])
print("#define UNLOCK_MS %d" % ms["unlock"])
print("#define STAR_MS %d" % ms["star"])
