#!/usr/bin/env python3
# Build ABC 123 sound effects as 48 kHz mono 16-bit WAVs the nv_sound() host import can stream.
#
# HOW THESE ARE SYNTHESIZED (so you can tune them):
#   * Additive synthesis: each note = fundamental sine + a few decaying harmonics (2f at 0.3, 3f at
#     0.15, 4f at 0.08). Harmonics give a warm, "instrument-like" timbre instead of a flat beep.
#   * ADSR envelope per note (attack/decay/sustain/release) so notes don't click and have shape.
#   * POLYPHONY: a "voice" is (start_time, [freqs], dur, gain). Multiple voices overlapping in time,
#     and multiple freqs in one voice (a CHORD), are simply SUMMED — that's real polyphony baked
#     into the sample. The device just streams the finished WAV (no runtime mixing needed).
#   * Everything is summed into a float buffer, then peak-normalized to -1 dBFS and quantized to
#     int16. Output rate is 48000 to match the codec's tone format (no resampling on device).
#
# To use ready-made sounds instead: drop a 48kHz/mono/16-bit WAV named win.wav / lose.wav /
# levelup.wav into apps/abc123/snd/ (e.g. CC0 packs from kenney.nl or mixkit) and skip this script
# for those names. Keep the format exact (the device skips a 44-byte header and assumes 48k/mono/16).
#
#   python tools/build_abc_sounds.py
# Output: apps/abc123/snd/{win,levelup,lose,tap}.wav
import wave, math, os, struct

RATE = 48000
OUT  = os.path.join(os.path.dirname(__file__), "..", "apps", "abc123", "snd")

# note name -> frequency (equal temperament)
_A4 = 440.0
_STEPS = {"C":-9,"C#":-8,"D":-7,"D#":-6,"E":-5,"F":-4,"F#":-3,"G":-2,"G#":-1,"A":0,"A#":1,"B":2}
def f(note):
    name = note[:-1]; octave = int(note[-1])
    semis = _STEPS[name] + (octave - 4) * 12
    return _A4 * (2.0 ** (semis / 12.0))

def adsr(i, n, a=0.010, d=0.060, s=0.72, r=0.14):
    # times in seconds -> sample counts, clamped so short notes still shape nicely
    A = max(1, int(a * RATE)); D = max(1, int(d * RATE)); R = max(1, int(r * RATE))
    if i < A:                return i / A
    if i < A + D:            return 1.0 - (1.0 - s) * (i - A) / D
    if i > n - R:            return s * max(0.0, (n - i) / R)
    return s

# harmonic weights (fundamental + overtones) -> warm timbre
_HARM = [(1, 1.0), (2, 0.30), (3, 0.15), (4, 0.08)]

def render(voices, tail=0.12):
    total = max(st + dur for (st, _fr, dur, _g) in voices) + tail
    N = int(total * RATE)
    buf = [0.0] * N
    for (st, freqs, dur, gain) in voices:
        n = int(dur * RATE); s0 = int(st * RATE)
        for i in range(n):
            t = i / RATE
            e = adsr(i, n)
            smp = 0.0
            for fr in freqs:
                for (h, w) in _HARM:
                    smp += w * math.sin(2 * math.pi * fr * h * t)
            buf[s0 + i] += gain * e * smp / (len(freqs) * 1.5)
    peak = max(1e-9, max(abs(x) for x in buf))
    scale = (10 ** (-1.0 / 20.0)) * 32767.0 / peak      # -1 dBFS
    return [int(max(-32767, min(32767, round(x * scale)))) for x in buf]

def save(name, samples):
    os.makedirs(OUT, exist_ok=True)
    path = os.path.join(OUT, name + ".wav")
    with wave.open(path, "wb") as w:
        w.setnchannels(1); w.setsampwidth(2); w.setframerate(RATE)
        w.writeframes(b"".join(struct.pack("<h", s) for s in samples))
    print(f"OK  {name}.wav  ({len(samples)} samples, {len(samples)/RATE:.2f}s)")

# ---- the sounds -------------------------------------------------------------------------------
# WIN: bright rising arpeggio resolving to a full major chord (played on every correct answer).
def win():
    v = [(0.00,[f("C5")],0.12,0.9),(0.09,[f("E5")],0.12,0.9),(0.18,[f("G5")],0.12,0.9),
         (0.27,[f("C5"),f("E5"),f("G5"),f("C6")],0.45,1.0)]
    return render(v)

# LEVELUP: a longer triumphant fanfare — quick run up, then a big sustained chord.
def levelup():
    v = [(0.00,[f("G4")],0.10,0.8),(0.09,[f("C5")],0.10,0.85),(0.18,[f("E5")],0.10,0.9),
         (0.27,[f("G5")],0.10,0.95),(0.36,[f("C6")],0.12,1.0),
         (0.50,[f("C5"),f("E5"),f("G5"),f("C6"),f("E6")],0.70,1.0)]
    return render(v)

# LOSE: a descending minor "sad trombone" — three drooping notes into a low minor chord.
def lose():
    v = [(0.00,[f("A4")],0.22,0.9),(0.20,[f("G4")],0.22,0.9),(0.40,[f("F4")],0.22,0.9),
         (0.60,[f("D4"),f("F4"),f("A4")],0.55,1.0)]
    return render(v, tail=0.15)

# TAP: a very short soft click (optional; the game may keep the built-in tone for taps).
def tap():
    return render([(0.0,[f("A6")],0.04,0.7)], tail=0.01)

if __name__ == "__main__":
    save("win", win())
    save("levelup", levelup())
    save("lose", lose())
    save("tap", tap())
