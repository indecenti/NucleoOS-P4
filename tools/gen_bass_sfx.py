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
import sys
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


def kick():
    t = t_axis(0.25)
    f = 45 + 120 * np.exp(-t * 30)
    return np.sin(2 * np.pi * np.cumsum(f) / RATE) * np.exp(-t * 14)


def snare():
    t = t_axis(0.2)
    return 0.6 * highpass(noise(0.2), 1200) * np.exp(-t * 22) + 0.4 * np.sin(2 * np.pi * 190 * t) * np.exp(-t * 30)


def hat():
    t = t_axis(0.05)
    return 0.25 * highpass(noise(0.05), 6000) * np.exp(-t * 90)


def bass_note(f, dur):
    t = t_axis(dur)
    ph = np.cumsum(np.full(len(t), f)) / RATE
    sq = np.sign(np.sin(2 * np.pi * ph)) * 0.6 + 0.4 * np.sin(2 * np.pi * ph)
    return lowpass(sq, 900) * np.minimum(1, t / 0.005) * np.exp(-t * 3)


def intro_theme():
    """An attract-mode theme (~12 s, 128 BPM): four-on-the-floor drums, a pumping bass line and a
    brass hook, rising to a final chord for the title slam."""
    bpm = 128
    beat = 60 / bpm
    bars = 6
    total = bars * 4 * beat + 1.2
    parts = []
    for b in range(bars * 4):
        t0 = b * beat
        parts.append((kick() * 0.9, t0))
        if b % 2 == 1:
            parts.append((snare(), t0))
        parts.append((hat(), t0 + beat / 2))
    roots = ["A2", "A2", "F2", "G2", "A2", "A2", "F2", "E2"] * 2
    for i in range(bars * 2):
        r = hz(roots[i % len(roots)])
        for k in range(4):
            parts.append((bass_note(r * (2 if k == 3 else 1), beat * 0.45) * 0.7, (i * 2 + k * 0.5) * beat))
    hook = ["A4", None, "C5", "D5", "E5", None, "D5", "C5", "D5", None, "E5", "G5", "E5", None, None, None]
    for rep in range(2, bars):
        for j, n in enumerate(hook):
            if n and (rep * 16 + j) < bars * 16:
                parts.append((brass(hz(n), beat * 0.9, 0.8), (rep * 4 + j * 0.25) * beat))
    end = bars * 4 * beat
    for n in ("A4", "C#5", "E5", "A5"):
        parts.append((brass(hz(n), 1.2, 0.7), end))
    parts.append((kick() * 1.2, end))
    return pad(reverb(mix(*parts), 0.2, 0.4), total)


# ---- the boat ----
def engine(dur, f0, f1, rough=0.5):
    """A two-stroke outboard: a pulse train at the firing rate (gliding f0 -> f1), its buzz, and hiss."""
    t = t_axis(dur)
    f = np.linspace(f0, f1, len(t))
    ph = 2 * np.pi * np.cumsum(f) / RATE
    pulse = np.maximum(0, np.sin(ph)) ** 6 - 0.2
    buzz = lowpass(np.sign(np.sin(ph * 2)) * 0.4 + pulse, 1400)
    hiss = lowpass(noise(dur), 3000) * rough * 0.25
    return buzz + hiss


def motor_start():
    """Pull cord, two coughs, then the engine catches and revs up."""
    cord = lowpass(noise(0.18), 2500) * env(0.18, 0.01, 0.12) * 0.6
    cough = engine(0.12, 22, 16, 0.8) * env(0.12, 0.005, 0.09)
    run = engine(0.75, 18, 46, 0.6) * env(0.75, 0.05, 0.0)
    fade = np.linspace(1, 0.7, len(run))
    return mix((cord, 0.0), (cough, 0.22), (cough * 0.8, 0.40), (run * fade, 0.55), (np.zeros(int(1.3 * RATE)), 0.0))


def motor():
    """One second of the outboard running: loops by repetition every ~0.95 s."""
    x = engine(1.0, 44, 44, 0.5)
    k = int(0.03 * RATE)
    x[:k] *= np.linspace(0, 1, k)
    return x


def fish_on():
    """The hook-set stinger: a bright brass stab over a thump and a splash."""
    stab = sum(brass(hz(n), 0.45, 0.8) for n in ("C5", "E5", "G5", "C6"))
    thump = np.sin(2 * np.pi * 70 * t_axis(0.25)) * env(0.25, 0.002, 0.2)
    return mix((thump, 0.0), (stab, 0.02), (splash(0.4, 3000, 90) * 0.5, 0.0), (np.zeros(int(0.5 * RATE)), 0.0))


def junk():
    """A junk catch: a comic tin clank and a little two-note sting."""
    t = t_axis(0.3)
    clank = sum(np.sin(2 * np.pi * f * t) * env(0.3, 0.001, 0.25) for f in (820, 1370, 2210)) * 0.5
    return mix((clank, 0.0), (bell(hz("G5"), 0.3, 0.6), 0.25), (bell(hz("C6"), 0.5, 0.6), 0.42), (np.zeros(int(0.95 * RATE)), 0.0))


# ---- loops for the mixer (ABI v15): seamless, the game sets their volume and pitch live ----
def save_loop(name, x, seconds, xfade=0.25):
    """Cross-fade the tail over the head so the loop has no seam, then save (no end fade)."""
    n, f = int(seconds * RATE), int(xfade * RATE)
    x = x[: n + f]
    head, tail = x[:f].copy(), x[n: n + f]
    r = np.linspace(0, 1, f)
    out = x[:n].copy()
    out[:f] = head * r + tail * (1 - r)
    out = np.tanh(out / (np.max(np.abs(out)) + 1e-9) * 1.2)
    out = out / (np.max(np.abs(out)) + 1e-9) * PEAK
    with wave.open(os.path.join(OUT, name + ".wav"), "wb") as w:
        w.setnchannels(1); w.setsampwidth(2); w.setframerate(RATE)
        w.writeframes((out * 32767).astype("<i2").tobytes())
    print(f"{name:10s} {n / RATE:5.2f} s loop")


def amb_up():
    """Lapping water against the hull, swelling slowly, and a couple of distant birds."""
    d = 8.5
    t = t_axis(d)
    lap = lowpass(highpass(noise(d), 120), 900) * (0.55 + 0.45 * np.sin(2 * np.pi * 0.35 * t) ** 2)
    slosh = lowpass(noise(d), 300) * (0.5 + 0.5 * np.sin(2 * np.pi * 0.21 * t + 1))
    birds = np.zeros(len(t))
    for s0, f0 in ((1.3, 3100), (1.55, 3400), (5.2, 2700), (5.4, 2900), (5.6, 3300)):
        tt = t_axis(0.14)
        ch = np.sin(2 * np.pi * np.cumsum(f0 + 600 * np.sin(2 * np.pi * 22 * tt)) / RATE) * np.sin(np.pi * tt / 0.14) ** 2
        i = int(s0 * RATE)
        birds[i: i + len(ch)] += 0.10 * ch
    return lap + 0.8 * slosh + birds


def amb_down():
    """Under water: a deep muffled hum and runs of little bubbles."""
    d = 8.5
    t = t_axis(d)
    hum = lowpass(lowpass(noise(d), 160), 160) * 3.0 * (0.8 + 0.2 * np.sin(2 * np.pi * 0.15 * t))
    bub = np.zeros(len(t))
    for k in range(26):
        s0 = rng.uniform(0, d - 0.1)
        f0 = rng.uniform(450, 1300)
        dd = rng.uniform(0.02, 0.06)
        tt = t_axis(dd)
        b = np.sin(2 * np.pi * (f0 * tt + f0 * 4 * tt * tt)) * np.exp(-tt * 50)
        i = int(s0 * RATE)
        bub[i: i + len(b)] += 0.25 * b[: len(bub) - i]
    return hum + bub


def reel_loop():
    """The spinning reel: the ratchet at 24 clicks a second over the hiss of the line on the spool."""
    d = 1.3
    out = 0.08 * lowpass(highpass(noise(d), 2500), 7000)
    for k in range(int(d * 24)):
        tt = t_axis(0.010)
        c = np.sin(2 * np.pi * 3300 * tt) * np.exp(-tt * 600) + 0.6 * highpass(noise(0.010), 2500) * np.exp(-tt * 800)
        i = int(k / 24 * RATE)
        out[i: i + len(c)] += c[: len(out) - i]
    return out


def drag_loop():
    """The drag slipping: a fast buzzing ratchet and the whine of line paid out under load."""
    d = 0.8
    t = t_axis(d)
    out = 0.25 * np.sin(2 * np.pi * np.cumsum(1500 + 80 * np.sin(2 * np.pi * 9 * t)) / RATE)
    for k in range(int(d * 70)):
        tt = t_axis(0.006)
        c = highpass(noise(0.006), 1800) * np.exp(-tt * 900)
        i = int(k / 70 * RATE)
        out[i: i + len(c)] += c[: len(out) - i]
    return out


def creak_loop():
    """The line near breaking: a strained creak, a narrow band of noise that wavers."""
    d = 1.5
    t = t_axis(d)
    band = lowpass(highpass(noise(d), 700), 1300)
    return band * (0.6 + 0.4 * np.sin(2 * np.pi * 3.3 * t)) * (0.7 + 0.3 * np.sin(2 * np.pi * 11 * t))


def bubbles():
    out = np.zeros(int(0.45 * RATE))
    for k in range(7):
        s0 = k * 0.05 + rng.uniform(0, 0.03)
        f0 = rng.uniform(600, 1500)
        tt = t_axis(0.05)
        b = np.sin(2 * np.pi * (f0 * tt + f0 * 5 * tt * tt)) * np.exp(-tt * 45)
        i = int(s0 * RATE)
        out[i: i + len(b)] += b[: len(out) - i]
    return out


def thud():
    """The lure settling on the bed: a soft low knock with a puff of silt."""
    t = t_axis(0.22)
    return np.sin(2 * np.pi * 85 * t) * np.exp(-t * 26) + 0.25 * lowpass(noise(0.22), 900) * env(0.22, 0.002, 18)


def tick():
    """A nibble felt through the rod: a tiny dry tick."""
    t = t_axis(0.03)
    return np.sin(2 * np.pi * 2400 * t) * np.exp(-t * 300) + 0.5 * highpass(noise(0.03), 3000) * np.exp(-t * 400)


def twitch():
    """The rod snapped back: a quick swish."""
    d = 0.16
    t = t_axis(d)
    n = lowpass(highpass(noise(d), 800), 2500 + 4000 * t / d)
    return n * np.sin(np.pi * t / d) ** 2


if __name__ == "__main__":
    if len(sys.argv) > 1 and sys.argv[1] == "loops":
        save_loop("amb_up", amb_up(), 8.0)
        save_loop("amb_down", amb_down(), 8.0)
        save_loop("reel_loop", reel_loop(), 1.0)
        save_loop("drag_loop", drag_loop(), 0.5)
        save_loop("creak_loop", creak_loop(), 1.2)
        for n in ("bubbles", "thud", "tick", "twitch"):
            save(n, globals()[n]())
        sys.exit(0)
    import sys
    if len(sys.argv) > 1:
        for n in sys.argv[1:]:
            save(n, globals()[n]())
        sys.exit(0)
    # intro.wav, menu.wav, victory.wav come from ACE-Step: tools/ace_music.py
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
    save("motor_start", motor_start())
    save("motor", motor())
    save("fish_on", fish_on())
    save("junk", junk())
