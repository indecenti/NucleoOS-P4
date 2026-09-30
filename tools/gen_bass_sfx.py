#!/usr/bin/env python3
# Vertice Bass sound effects, synthesised: water, reel, line and the tournament jingles.
# 48 kHz MONO 16-bit WAV — the format nv_sound() streams (44-byte header skipped, no resampling).
# Output: apps/bass/snd/*.wav. Peaks are kept low (-10 dBFS): a loud stream through the board's
# small amplifier can brown out the supply (see the Pianino notes).
#
# Building blocks: filtered noise with envelopes for water (splash, plop, jump), short resonant
# clicks for the reel ratchet, a pitched twang with noise for the line (whip, snap), inharmonic bell
# partials for the stage bell, and a soft brass-like voice (saw through a one-pole low-pass, with a
# little vibrato) for the fanfares. A light noise-convolution reverb gives the jingles some air.
import os
import wave

import numpy as np

RATE = 48000
PEAK = 0.32
OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "apps", "bass", "snd")
rng = np.random.default_rng(7)


def t_axis(dur):
    return np.arange(int(dur * RATE)) / RATE


def lowpass(x, fc):
    """One-pole low-pass; fc may be a number or a per-sample array (a sweep)."""
    a = np.exp(-2 * np.pi * np.broadcast_to(np.asarray(fc, dtype=float), x.shape) / RATE)
    y = np.empty_like(x)
    acc = 0.0
    for i in range(len(x)):
        acc = (1 - a[i]) * x[i] + a[i] * acc
        y[i] = acc
    return y


def highpass(x, fc):
    return x - lowpass(x, fc)


def env(dur, attack, decay):
    t = t_axis(dur)
    e = np.minimum(1.0, t / max(attack, 1e-4)) * np.exp(-t * decay)
    return e


def noise(dur):
    return rng.standard_normal(int(dur * RATE))


def reverb(x, amount=0.25, length=0.6):
    ir = noise(length) * np.exp(-t_axis(length) * 7.0)
    ir /= np.sqrt(np.sum(ir ** 2)) + 1e-9
    wet = np.convolve(x, ir)[: len(x) + int(length * RATE)]
    out = np.zeros(len(wet))
    out[: len(x)] += x
    return out + amount * wet


def mix(*parts):
    n = max(len(p) for p, _ in parts)
    out = np.zeros(n)
    for p, start in parts:
        s = int(start * RATE)
        seg = p[: max(0, n - s)]
        out[s: s + len(seg)] += seg
    return out


def pad(x, dur):
    n = int(dur * RATE)
    return np.concatenate([x, np.zeros(max(0, n - len(x)))])[:n] if n > len(x) else x


def save(name, x):
    x = np.tanh(x / (np.max(np.abs(x)) + 1e-9) * 1.4)
    x = x / (np.max(np.abs(x)) + 1e-9) * PEAK
    fade = min(len(x), int(0.01 * RATE))
    x[-fade:] *= np.linspace(1, 0, fade)
    os.makedirs(OUT, exist_ok=True)
    with wave.open(os.path.join(OUT, name + ".wav"), "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(RATE)
        w.writeframes((x * 32767).astype("<i2").tobytes())
    print(f"{name:8s} {len(x) / RATE:5.2f} s")


# ---- water ----
def splash(dur=0.55, bright=2600, thump=90):
    n = lowpass(highpass(noise(dur), 300), bright) * env(dur, 0.004, 7.0)
    bub = np.zeros(int(dur * RATE))
    for k in range(9):                                  # bubbles: short rising blips
        s = rng.uniform(0.05, dur * 0.8)
        f0 = rng.uniform(500, 1400)
        d = rng.uniform(0.02, 0.05)
        t = t_axis(d)
        b = np.sin(2 * np.pi * (f0 * t + f0 * 3 * t * t)) * np.exp(-t * 60)
        i = int(s * RATE)
        bub[i: i + len(b)] += 0.35 * b[: len(bub) - i]
    t = t_axis(dur)
    low = np.sin(2 * np.pi * thump * t) * np.exp(-t * 18)
    return n + bub + 0.8 * low


def plop():
    t = t_axis(0.18)
    f = 900 * np.exp(-t * 10) + 250
    return np.sin(2 * np.pi * np.cumsum(f) / RATE) * np.exp(-t * 22) + 0.2 * lowpass(noise(0.18), 3000) * env(0.18, 0.002, 30)


def whoosh():
    d = 0.4
    t = t_axis(d)
    n = noise(d)
    lo = lowpass(n, 700 + 3500 * t / d)
    return (lo - lowpass(lo, 300)) * np.sin(np.pi * t / d) ** 2


# ---- tackle ----
def reel():
    out = np.zeros(int(0.075 * RATE))
    for k in range(3):
        t = t_axis(0.012)
        c = np.sin(2 * np.pi * 3100 * t) * np.exp(-t * 500) + 0.5 * highpass(noise(0.012), 2000) * np.exp(-t * 700)
        i = int(k * 0.025 * RATE)
        out[i: i + len(c)] += c
    return out


def whip(f0=1600, f1=500, dur=0.28):
    t = t_axis(dur)
    f = f1 + (f0 - f1) * np.exp(-t * 14)
    tw = np.sin(2 * np.pi * np.cumsum(f) / RATE) * np.exp(-t * 9)
    return tw + 0.4 * highpass(noise(dur), 3000) * env(dur, 0.001, 25)


def snap():
    crack = highpass(noise(0.05), 1500) * env(0.05, 0.0005, 70)
    t = t_axis(0.5)
    twang = (np.sin(2 * np.pi * 180 * t) + 0.5 * np.sin(2 * np.pi * 367 * t)) * np.exp(-t * 6) * (1 + 0.3 * np.sin(2 * np.pi * 7 * t))
    return mix((crack * 1.4, 0.0), (twang, 0.01))


def strike():
    t = t_axis(0.25)
    f = 700 + 900 * t / 0.25
    beep = np.sign(np.sin(2 * np.pi * np.cumsum(f) / RATE)) * np.exp(-t * 6) * 0.35
    return mix((beep, 0.0), (splash(0.4, 2000, 120) * 0.8, 0.05))


# ---- music ----
NOTE = {"C": -9, "D": -7, "E": -5, "F": -4, "G": -2, "A": 0, "B": 2}


def hz(n):
    k = NOTE[n[0]] + (1 if "#" in n else 0) + (int(n[-1]) - 4) * 12
    return 440.0 * 2 ** (k / 12)


def brass(f, dur, gain=1.0):
    t = t_axis(dur)
    vib = 1 + 0.004 * np.sin(2 * np.pi * 5.5 * t) * np.minimum(1, t / 0.15)
    ph = np.cumsum(f * vib) / RATE
    saw = 2 * (ph - np.floor(ph + 0.5))
    a = np.minimum(1, t / 0.03) * np.exp(-t * 1.6) * np.minimum(1, (dur - t) / 0.05)
    return lowpass(saw, 2400) * a * gain


def bell(f, dur, gain=1.0):
    t = t_axis(dur)
    out = np.zeros(len(t))
    for r, g, d in [(1, 1, 1.2), (2.0, 0.5, 1.8), (2.76, 0.4, 2.4), (5.4, 0.2, 4.0), (8.9, 0.1, 6)]:
        out += g * np.sin(2 * np.pi * f * r * t) * np.exp(-t * d)
    return out * np.minimum(1, t / 0.003) * gain


def seq(notes, voice, step):
    parts = []
    for i, n in enumerate(notes):
        if n is None:
            continue
        if isinstance(n, tuple):
            for m in n:
                parts.append((voice(hz(m), step * 2.2, 0.6), i * step))
        else:
            parts.append((voice(hz(n), step * 1.8), i * step))
    return mix(*parts)


def fanfare_catch():
    return reverb(seq(["G4", "C5", "E5", ("G5", "C5", "E5")], brass, 0.12), 0.3)


def fanfare_qualify():
    return reverb(seq(["C5", "C5", "C5", "E5", None, "D5", "F5", ("G5", "E5", "C5")], brass, 0.14), 0.35)


def jingle_fail():
    return reverb(seq(["G4", "F#4", "F4", ("E4", "C4")], brass, 0.22), 0.3)


def stage_bell():
    return reverb(mix((bell(hz("C5"), 1.8), 0.0), (bell(hz("G5"), 1.6, 0.6), 0.0)), 0.25)


def title_jingle():
    melody = ["E5", "G5", "A5", None, "G5", "E5", "D5", ("E5", "C5", "G4")]
    return reverb(seq(melody, brass, 0.16), 0.35)


def click():
    t = t_axis(0.04)
    return np.sin(2 * np.pi * 1800 * t) * np.exp(-t * 120)


def drumroll():
    d = 1.3
    out = np.zeros(int(d * RATE))
    rate = 18
    for k in range(int(d * rate)):
        hit = lowpass(noise(0.05), 1200) * env(0.05, 0.001, 60) * (0.4 + 0.6 * k / (d * rate))
        i = int(k / rate * RATE)
        out[i: i + len(hit)] += hit[: len(out) - i]
    return out


if __name__ == "__main__":
    save("splash", splash())
    save("jump", pad(splash(0.9, 3200, 70) * 1.2, 0.9))
    save("plop", plop())
    save("cast", whoosh())
    save("reel", reel())
    save("hook", whip())
    save("snap", snap())
    save("strike", strike())
    save("catch", fanfare_catch())
    save("qualify", fanfare_qualify())
    save("fail", jingle_fail())
    save("bell", stage_bell())
    save("title", title_jingle())
    save("click", click())
    save("drum", drumroll())
