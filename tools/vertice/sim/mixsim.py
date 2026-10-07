"""mixsim.py - rebuild a game's audio from a Vertice simulator log, the way the board's mixer plays it.

The simulator logs every sound call ("snd @<frame> play|set|stop|master|sound ..."). This replays
them over the game's own WAV files with the mixer's rules (components/nv_wasm/nv_wasm_snd.cpp):
48 kHz mono, per-voice volume 0..256 gliding one step per sample, pitch 256 = 1.0 by linear
interpolation, loops, fade-outs on stop, master gain, the soft knee above half scale and the
peak cap. Output: a 48 kHz 16-bit mono WAV of frames [f0, f1].

    python tools/vertice/sim/mixsim.py <sim log> <f0> <f1> <out.wav> [fps] [app id]

A gameplay video: run the simulator with VX_DUMP_RANGE=0-N (every frame as PPM), mix its log, then
    ffmpeg -framerate 30 -i frame_%05d.ppm -i out.wav -vf scale=1024:600 -c:v libx264 -c:a aac video.mp4
"""
import os
import re
import sys
import wave

import numpy as np

APP = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "..", "apps")
GAME = "bass"
RATE = 48000


def path_of(name):
    if ":" in name:                       # "lib:name": a package the game requires
        lib, leaf = name.split(":", 1)
        return os.path.join(APP, lib, "snd", leaf + ".wav")
    return os.path.join(APP, GAME, "snd", name + ".wav")


_cache = {}


def load(name):
    if name in _cache:
        return _cache[name]
    try:
        w = wave.open(path_of(name))
    except FileNotFoundError:
        _cache[name] = None
        return None
    ch, rate, n = w.getnchannels(), w.getframerate(), w.getnframes()
    x = np.frombuffer(w.readframes(n), dtype="<i2").astype(np.float32)
    if ch == 2:
        x = x.reshape(-1, 2).mean(axis=1)
    _cache[name] = (x, rate)
    return _cache[name]


class Voice:
    def __init__(self, data, rate, vol, pitch, loop):
        self.x, self.rate, self.loop = data, rate, loop
        self.pos = 0.0
        self.vol = float(vol)
        self.vol_t = float(vol)
        self.set_pitch(pitch)
        self.fade = 0
        self.fade_len = 0
        self.done = False

    def set_pitch(self, pitch):
        pitch = max(32, min(1024, pitch))
        self.step = self.rate * pitch / 256.0 / RATE

    def render(self, n):
        x, m = self.x, len(self.x)
        idx = self.pos + self.step * np.arange(n)
        if self.loop:
            idx = np.mod(idx, m - 1)
            valid = n
        else:
            valid = int(np.searchsorted(idx, m - 1))
        i0 = idx[:valid].astype(np.int64)
        fr = idx[:valid] - i0
        s = np.zeros(n, np.float32)
        s[:valid] = x[i0] + (x[np.minimum(i0 + 1, m - 1)] - x[i0]) * fr
        # volume glides toward its target one step per sample
        d = self.vol_t - self.vol
        k = np.arange(1, n + 1, dtype=np.float32)
        g = self.vol + np.sign(d) * np.minimum(k, abs(d))
        self.vol = float(g[-1])
        if self.fade_len:
            rem = self.fade - k
            g = g * np.clip(rem / self.fade_len, 0, 1)
            self.fade -= n
            if self.fade <= 0:
                self.done = True
        self.pos = float(self.pos + self.step * n)
        if self.loop:
            self.pos %= (m - 1)
        elif valid < n:
            self.done = True
        return s * g / 256.0


def main():
    log, f0, f1, out = sys.argv[1], int(sys.argv[2]), int(sys.argv[3]), sys.argv[4]
    fps = int(sys.argv[5]) if len(sys.argv) > 5 else 30
    global GAME
    if len(sys.argv) > 6:
        GAME = sys.argv[6]
    ev = {}
    pat = re.compile(r"^snd @(\d+) (\w+) (.*)$")
    for line in open(log, encoding="utf-8", errors="replace"):
        m = pat.match(line.strip())
        if m:
            ev.setdefault(int(m.group(1)), []).append((m.group(2), m.group(3).split()))
    per = RATE // fps
    voices, master, chunks = {}, 256.0, []
    for f in range(0, f1 + 1):
        for op, a in ev.get(f, []):
            if op in ("play", "sound"):
                if op == "play":
                    h, name, vol, pitch, flags = int(a[0]), a[1], int(a[2]), int(a[3]), int(a[4])
                else:
                    h, name, vol, pitch, flags = -1 - len(voices), a[0], 256, 256, 0
                d = load(name)
                if d is not None:
                    voices[h] = Voice(d[0], d[1], vol, pitch, bool(flags & 1))
            elif op == "set":
                v = voices.get(int(a[0]))
                if v:
                    if int(a[1]) >= 0:
                        v.vol_t = float(int(a[1]))
                    if int(a[2]) > 0:
                        v.set_pitch(int(a[2]))
            elif op == "stop":
                v = voices.get(int(a[0]))
                if v:
                    ms = int(a[1])
                    if ms <= 0:
                        v.done = True
                    else:
                        v.fade = v.fade_len = ms * RATE // 1000
            elif op == "master":
                master = float(int(a[0]))
        acc = np.zeros(per, np.float32)
        for h in list(voices):
            v = voices[h]
            if not v.done:
                acc += v.render(per)
            if v.done:
                del voices[h]
        if f >= f0:
            x = acc * master / 256.0
            ax = np.abs(x)
            over = np.maximum(ax - 16384, 0)
            y = np.where(ax > 16384, np.sign(x) * (16384 + over * 8192 / (over + 8192)), x)
            chunks.append(np.clip(y, -24576, 24576))
    pcm = np.concatenate(chunks).astype("<i2")
    w = wave.open(out, "wb")
    w.setnchannels(1); w.setsampwidth(2); w.setframerate(RATE)
    w.writeframes(pcm.tobytes())
    w.close()
    print(out, f"{len(pcm) / RATE:.1f} s, peak {np.abs(pcm).max()}")


if __name__ == "__main__":
    main()
