#!/usr/bin/env python3
# Pianino - professional instrument sample banks, 48 kHz MONO 16-bit WAV (the format nv_sound()
# streams; see tools/gen_abc_sfx.py for the format note).
#
# CRITICAL - LOW AMPLITUDE (peak ~0.25, NOT the usual 0.94): the board's audio goes to a
# bus-powered USB speaker. A full-scale (-1 dBFS) sample draws a big current spike that sags the
# rail and BROWNS OUT the board (clean reset, no coredump - that's how we know it's power, not a
# firmware panic). nv_gfx_tone plays at 0.22 amplitude and never resets; these samples match that
# so nv_sound is safe here. DO NOT raise PEAK without re-testing stability on the real board.
#
# Two banks:
#   piano_*  - acoustic-ish struck string: HARMONIC partials with slight string inharmonicity
#              (f_n = n*f0*sqrt(1+B n^2)), 1/n^1.15 rolloff, fast hammer attack + a short filtered
#              noise "thunk", highs decay faster. Warm, rich - the "professional" piano voice.
#   bell_*   - inharmonic celesta/glockenspiel partials, brighter and shorter. For the XYLO preset.
import numpy as np, wave, os

RATE = 48000
PEAK = 0.25                      # see the brownout note above - keep low
OUT  = os.path.join(os.path.dirname(__file__), "..", "apps", "pianino", "snd")
os.makedirs(OUT, exist_ok=True)

A4 = 440.0
STEPS = {"C":-9,"C#":-8,"D":-7,"D#":-6,"E":-5,"F":-4,"F#":-3,"G":-2,"G#":-1,"A":0,"A#":1,"B":2}
def hz(note):
    return A4 * 2.0 ** ((STEPS[note[:-1]] + (int(note[-1]) - 4) * 12) / 12.0)

# ---- acoustic piano: harmonic series with real-string stretch + hammer transient -----------------
def piano_note(freq, dur):
    n = int(dur * RATE); t = np.arange(n) / RATE
    sig = np.zeros(n)
    B = 0.0004                                   # string inharmonicity coefficient (mid piano)
    for k in range(1, 13):
        fk = freq * k * np.sqrt(1.0 + B * k * k)
        if fk > RATE * 0.45: break
        amp = 1.0 / (k ** 1.15)                  # natural harmonic rolloff
        dec = 3.2 + 0.9 * k                      # higher partials die faster (1/s)
        env = np.exp(-dec * t) * amp
        # two very slightly detuned oscillators = the beating/warmth of paired piano strings
        sig += env * (np.sin(2*np.pi*fk*t) + np.sin(2*np.pi*fk*1.0015*t)) * 0.5
    # hammer "thunk": a few ms of noise shaped by a fast decay, gives the percussive onset
    hn = int(0.010 * RATE)
    noise = np.random.RandomState(3).randn(hn) * np.exp(-np.arange(hn) / (0.0025 * RATE))
    sig[:hn] += noise * 0.18
    atk = int(0.003 * RATE)                      # 3 ms fade-in, no click
    sig[:atk] *= np.linspace(0, 1, atk)
    return sig

# ---- bell / celesta: inharmonic partials (bright, metallic, shorter) -----------------------------
BELL = [(1.00, 1.00, 4.5), (2.00, 0.55, 6.0), (3.01, 0.32, 8.0),
        (4.19, 0.18, 10.5), (5.43, 0.10, 13.5), (6.79, 0.05, 17.0)]
def bell_note(freq, dur):
    n = int(dur * RATE); t = np.arange(n) / RATE
    sig = np.zeros(n)
    for ratio, amp, dec in BELL:
        f = freq * ratio
        if f > RATE * 0.45: continue
        env = np.exp(-dec * t) * amp
        sig += env * (np.sin(2*np.pi*f*t) + np.sin(2*np.pi*f*1.003*t)) * 0.5
    atk = int(0.003 * RATE)
    sig[:atk] *= np.linspace(0, 1, atk)
    return sig

def reverb(buf, decay=0.28, mix=0.16):
    ir_n = int(decay * RATE)
    ir = (np.random.RandomState(7).randn(ir_n) * np.exp(-np.arange(ir_n) / (0.10 * RATE)))
    wet = np.convolve(buf, ir)[:len(buf)]
    wet /= (np.max(np.abs(wet)) + 1e-9)
    return buf * (1 - mix) + wet * mix

def render(fn, freq, dur, tail=0.30):
    buf = np.zeros(int((dur + tail) * RATE))
    sig = fn(freq, dur)
    buf[:len(sig)] += sig
    buf = reverb(buf)
    buf = np.tanh(buf * 1.2)                      # gentle warmth
    buf /= (np.max(np.abs(buf)) + 1e-9)
    return buf * PEAK                            # LOW amplitude - brownout guard

def save(name, buf):
    data = (np.clip(buf, -1, 1) * 32767.0).astype("<i2").tobytes()
    with wave.open(os.path.join(OUT, name + ".wav"), "wb") as w:
        w.setnchannels(1); w.setsampwidth(2); w.setframerate(RATE); w.writeframes(data)
    print(f"  {name}.wav  {int(len(buf)/RATE*1000)} ms  {len(data)} bytes")

# solfège key -> note. One octave, Do (C4) up to Do' (C5).
KEYS = [("do", "C4"), ("re", "D4"), ("mi", "E4"), ("fa", "F4"),
        ("sol", "G4"), ("la", "A4"), ("si", "B4"), ("do2", "C5")]

# Durations kept SHORT (~300 ms): nv_sound plays each WAV fully before the next (queue depth 2), so
# long samples make fast tapping laggy. Short = real instrument timbre (attack + harmonic body) yet
# still responsive for a kid drumming on the keys.
print("generating pianino sample banks (peak %.2f - brownout-safe):" % PEAK)
for name, n in KEYS:
    save("piano_" + name, render(piano_note, hz(n), 0.30, tail=0.06))
    save("bell_"  + name, render(bell_note,  hz(n) * 2.0, 0.24, tail=0.05))   # xylo an octave up
