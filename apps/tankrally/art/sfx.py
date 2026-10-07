#!/usr/bin/env python3
# Tank Rally sound effects, synthesised: 48 kHz MONO 16-bit WAV for the OS mixer (nv_snd_play).
# Peaks kept low (-10 dBFS: loud streams can brown out the board's small amplifier).
#   python apps/tankrally/art/sfx.py            -> apps/tankrally/snd/*.wav
# Why not nv_gfx_tone: every tone costs the game ~30 ms of CPU on the board (measured); the mixer
# plays in its own task.
import os
import wave

import numpy as np

RATE = 48000
PEAK = 0.32
OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'snd')
rng = np.random.default_rng(11)


def t_axis(d):
    return np.arange(int(d * RATE)) / RATE


def lowpass(x, fc):
    a = np.exp(-2 * np.pi * np.broadcast_to(np.asarray(fc, dtype=float), x.shape) / RATE)
    y = np.empty_like(x)
    acc = 0.0
    for i in range(len(x)):
        acc = (1 - a[i]) * x[i] + a[i] * acc
        y[i] = acc
    return y


def highpass(x, fc):
    return x - lowpass(x, fc)


def env(n, attack, decay_tau):
    t = np.arange(n) / RATE
    e = np.exp(-t / decay_tau)
    na = max(1, int(attack * RATE))
    e[:na] *= np.linspace(0, 1, na)
    return e


def noise(d):
    return rng.standard_normal(int(d * RATE))


def sweep(f0, f1, d, shape=np.sin):
    t = t_axis(d)
    f = f0 * (f1 / f0) ** (t / d)
    return shape(2 * np.pi * np.cumsum(f) / RATE)


def save(name, x, peak=PEAK):
    x = np.asarray(x, dtype=float)
    fade = min(len(x), int(0.004 * RATE))
    x[-fade:] *= np.linspace(1, 0, fade)
    m = np.max(np.abs(x)) or 1
    x = x / m * peak
    os.makedirs(OUT, exist_ok=True)
    with wave.open(os.path.join(OUT, name + '.wav'), 'wb') as w:
        w.setnchannels(1); w.setsampwidth(2); w.setframerate(RATE)
        w.writeframes((x * 32767).astype('<i2').tobytes())


def boom(d, cut0, cut1, sub_f, tau):
    n = noise(d)
    x = lowpass(n, np.geomspace(cut0, cut1, len(n))) * env(len(n), 0.002, tau)
    x += 0.9 * sweep(sub_f * 1.8, sub_f * 0.6, d) * env(len(n), 0.001, tau * 0.7)
    return x


def shot():          # the cannon: a sharp crack and a low thump
    d = 0.5
    crack = highpass(noise(d), 1500) * env(int(d * RATE), 0.0005, 0.02)
    thump = sweep(120, 45, d) * env(int(d * RATE), 0.001, 0.12)
    body = lowpass(noise(d), 900) * env(int(d * RATE), 0.001, 0.08)
    return crack * 0.6 + thump + body * 0.8


def crate():         # wood cracking apart and a small blast
    d = 0.7
    n = int(d * RATE)
    x = boom(d, 2500, 300, 70, 0.16) * 0.8
    for k in range(6):                                  # splinters: short resonant clicks
        at = int((0.01 + 0.05 * k + rng.uniform(0, 0.03)) * RATE)
        f = rng.uniform(600, 1400)
        L = int(0.04 * RATE)
        if at + L < n:
            x[at:at + L] += np.sin(2 * np.pi * f * np.arange(L) / RATE) * env(L, 0.0005, 0.008) * 0.6
    return x


def coin():          # two bright bell notes
    d = 0.45
    t = t_axis(d)
    x = np.zeros_like(t)
    for f, at in ((1318.5, 0.0), (1975.5, 0.07)):
        i = int(at * RATE)
        tt = t[: len(t) - i]
        tone = (np.sin(2 * np.pi * f * tt) + 0.35 * np.sin(2 * np.pi * f * 2.76 * tt) * np.exp(-tt / 0.03))
        x[i:] += tone * np.exp(-tt / 0.16)
    return x


def hit():           # a shell striking armour: clang + rumble
    d = 0.8
    t = t_axis(d)
    clang = sum(np.sin(2 * np.pi * f * t) * np.exp(-t / tau) for f, tau in ((420, 0.25), (1130, 0.12), (1890, 0.07), (2730, 0.05)))
    return clang * 0.5 + boom(d, 1500, 200, 60, 0.2)


def launch():        # rocket launch: a pop and a rising hiss
    d = 0.9
    n = noise(d)
    hiss = lowpass(highpass(n, 400), np.geomspace(800, 5000, len(n))) * env(len(n), 0.03, 0.45)
    pop = sweep(200, 70, d) * env(len(n), 0.001, 0.05)
    return hiss * 0.9 + pop * 0.7


def alarm():         # two short warning beeps
    t = t_axis(0.36)
    sq = np.sign(np.sin(2 * np.pi * 880 * t)) * 0.5 + np.sin(2 * np.pi * 880 * t) * 0.5
    gate = ((t < 0.13) | ((t > 0.19) & (t < 0.32))).astype(float)
    return lowpass(sq * gate, 4000)


def kit():           # repair: a rising arpeggio with a ratchet
    d = 0.5
    t = t_axis(d)
    x = np.zeros_like(t)
    for k, f in enumerate((523.3, 659.3, 784.0, 1046.5)):
        i = int(k * 0.07 * RATE)
        tt = t[: len(t) - i]
        x[i:] += np.sin(2 * np.pi * f * tt) * np.exp(-tt / 0.12)
    return x


def tower():         # a launcher blowing up: the big one, with debris
    d = 1.6
    x = boom(d, 3000, 120, 50, 0.45)
    t = t_axis(d)
    x += 0.25 * np.sin(2 * np.pi * 760 * t) * np.exp(-t / 0.15)
    return x


def bump():          # driving into a rock
    d = 0.3
    return sweep(90, 40, d) * env(int(d * RATE), 0.001, 0.06) + lowpass(noise(d), 600) * env(int(d * RATE), 0.001, 0.04) * 0.6


def ricochet():      # a shell glancing off rock
    d = 0.35
    return sweep(2400, 900, d) * env(int(d * RATE), 0.001, 0.08) * 0.5 + highpass(noise(d), 2000) * env(int(d * RATE), 0.0005, 0.02) * 0.5


def tick():          # UI tick (initials)
    t = t_axis(0.05)
    return np.sin(2 * np.pi * 1500 * t) * np.exp(-t / 0.012)


def brass(notes, step, hold):
    """A soft brass voice: saw through a low-pass, a little vibrato."""
    d = step * len(notes) + hold
    t = t_axis(d)
    x = np.zeros_like(t)
    for k, f in enumerate(notes):
        i = int(k * step * RATE)
        L = len(t) - i if k == len(notes) - 1 else int(step * 1.6 * RATE)
        L = min(L, len(t) - i)
        tt = np.arange(L) / RATE
        ph = 2 * np.pi * np.cumsum(f * (1 + 0.004 * np.sin(2 * np.pi * 5.5 * tt))) / RATE
        saw = 2 * ((ph / (2 * np.pi)) % 1) - 1
        e = np.minimum(1, tt / 0.015) * np.exp(-tt / (hold if k == len(notes) - 1 else step * 2))
        x[i:i + L] += lowpass(saw, 2200) * e
    return x


def go():
    return brass([523.3, 784.0], 0.12, 0.35)


def clear():
    return brass([523.3, 659.3, 784.0, 1046.5], 0.11, 0.5)


def over():
    return brass([392.0, 311.1, 261.6, 196.0], 0.2, 0.7)


def record():
    return brass([523.3, 659.3, 784.0, 1046.5, 784.0, 1046.5], 0.1, 0.6)


def engine():        # the tank's engine, a seamless 1 s loop (whole cycles): pitch follows speed
    t = t_axis(1.0)
    f = 36.0
    x = np.zeros_like(t)
    for h, a in ((1, 1.0), (2, 0.6), (3, 0.45), (4, 0.3), (6, 0.18), (8, 0.1)):
        x += a * np.sin(2 * np.pi * f * h * t + h)
    x *= 0.75 + 0.25 * np.sin(2 * np.pi * 9 * t)           # the cylinders' throb (9 Hz: whole cycles)
    rumble = np.zeros_like(t)                               # a periodic rattle of the tracks
    for k in range(18):
        i = int(k / 18 * RATE)
        L = int(0.012 * RATE)
        rumble[i:i + L] += rng.standard_normal(L) * np.exp(-np.arange(L) / RATE / 0.004)
    x += lowpass(np.concatenate([rumble, rumble]), 1800)[len(t):] * 0.5   # filtered twice round: wraps clean
    x = x - x.mean()
    os.makedirs(OUT, exist_ok=True)
    m = np.max(np.abs(x))
    with wave.open(os.path.join(OUT, 'engine.wav'), 'wb') as w:       # no fade: it loops
        w.setnchannels(1); w.setsampwidth(2); w.setframerate(RATE)
        w.writeframes((x / m * 0.22 * 32767).astype('<i2').tobytes())


def main():
    for name, fn in (('shot', shot), ('crate', crate), ('coin', coin), ('hit', hit), ('launch', launch),
                     ('alarm', alarm), ('kit', kit), ('tower', tower), ('bump', bump), ('ricochet', ricochet),
                     ('tick', tick), ('go', go), ('clear', clear), ('over', over), ('record', record)):
        save(name, fn())
    save('boom', boom(1.1, 2600, 150, 55, 0.3))
    engine()
    print('sfx ok')


if __name__ == '__main__':
    main()
