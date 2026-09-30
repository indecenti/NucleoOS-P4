#!/usr/bin/env python3
"""Instrumental game music with ACE-Step 1.5 (turbo) through a local ComfyUI, saved as device WAVs.

    python tools/ace_music.py "tags / style caption" out.wav --seconds 20 --bpm 140 --key "A minor"

Builds the ACE-Step 1.5 graph over ComfyUI's API (turbo UNet, the two Qwen text encoders, the 1.5
VAE, 8 steps), waits, downloads the FLAC/WAV and converts it to what nv_sound() plays: 48 kHz mono
16-bit PCM WAV, peak-limited (a loud stream through the board's small amp can brown out the supply),
with a short fade in/out so a loop or a cut never clicks.
"""
import argparse
import io
import json
import os
import random
import sys
import time
import urllib.parse
import urllib.request
import wave

import numpy as np
import soundfile as sf

HOST = os.environ.get("COMFY", "http://127.0.0.1:8188")


def _post(path, obj):
    req = urllib.request.Request(HOST + path, json.dumps(obj).encode(), {"Content-Type": "application/json"})
    return json.load(urllib.request.urlopen(req, timeout=60))


def _get(path):
    return urllib.request.urlopen(HOST + path, timeout=120).read()


def graph(tags, seconds, bpm, key, seed):
    return {
        "1": {"class_type": "UNETLoader", "inputs": {"unet_name": "acestep_v1.5_turbo.safetensors", "weight_dtype": "default"}},
        "2": {"class_type": "DualCLIPLoader", "inputs": {"clip_name1": "qwen_0.6b_ace15.safetensors",
                                                         "clip_name2": "qwen_1.7b_ace15.safetensors", "type": "ace", "device": "default"}},
        "3": {"class_type": "VAELoader", "inputs": {"vae_name": "ace_1.5_vae.safetensors"}},
        "4": {"class_type": "TextEncodeAceStepAudio1.5", "inputs": {
            "clip": ["2", 0], "tags": tags, "lyrics": "[Instrumental]", "seed": seed, "bpm": bpm, "duration": seconds,
            "timesignature": "4", "language": "en", "keyscale": key, "generate_audio_codes": True,
            "cfg_scale": 2.0, "temperature": 0.85, "top_p": 0.9, "top_k": 0, "min_p": 0.0}},
        "5": {"class_type": "ConditioningZeroOut", "inputs": {"conditioning": ["4", 0]}},
        "6": {"class_type": "EmptyAceStep1.5LatentAudio", "inputs": {"seconds": seconds, "batch_size": 1}},
        "7": {"class_type": "KSampler", "inputs": {
            "model": ["1", 0], "positive": ["4", 0], "negative": ["5", 0], "latent_image": ["6", 0],
            "seed": seed, "steps": 8, "cfg": 1.0, "sampler_name": "euler", "scheduler": "simple", "denoise": 1.0}},
        "8": {"class_type": "VAEDecodeAudio", "inputs": {"samples": ["7", 0], "vae": ["3", 0]}},
        "9": {"class_type": "SaveAudio", "inputs": {"audio": ["8", 0], "filename_prefix": "audio/nucleo_music"}},
    }


def generate(tags, seconds, bpm, key, seed=None, tries=3):
    seed = seed if seed is not None else random.randint(1, 2 ** 31)
    for attempt in range(tries):
        pid = _post("/prompt", {"prompt": graph(tags, seconds, bpm, key, seed)})["prompt_id"]
        t0 = time.time()
        while True:
            hist = json.loads(_get("/history/" + pid))
            if pid in hist:
                st = hist[pid].get("status", {})
                if st.get("status_str") == "error":
                    msg = json.dumps(st)[-400:]
                    print(f"  error (attempt {attempt + 1}): {msg}", file=sys.stderr)
                    break
                for node in hist[pid]["outputs"].values():
                    for a in node.get("audio", []):
                        q = urllib.parse.urlencode({"filename": a["filename"], "subfolder": a["subfolder"], "type": a["type"]})
                        data, rate = sf.read(io.BytesIO(_get("/view?" + q)), dtype="float32")
                        print(f"  generated {seconds:.0f} s in {time.time() - t0:.0f} s (seed {seed})", file=sys.stderr)
                        return data, rate
            if time.time() - t0 > 1200:
                raise TimeoutError("ComfyUI took too long")
            time.sleep(1.5)
        time.sleep(3)
    raise RuntimeError("generation failed")


def to_device_wav(data, rate, path, peak=0.35, fade_s=0.03):
    x = data.mean(axis=1) if data.ndim == 2 else data
    if rate != 48000:                                   # linear resample to 48 kHz
        n = int(len(x) * 48000 / rate)
        x = np.interp(np.linspace(0, len(x) - 1, n), np.arange(len(x)), x)
    x = x / (np.max(np.abs(x)) + 1e-9) * peak
    f = int(fade_s * 48000)
    x[:f] *= np.linspace(0, 1, f)
    x[-f:] *= np.linspace(1, 0, f)
    os.makedirs(os.path.dirname(os.path.abspath(path)), exist_ok=True)
    with wave.open(path, "wb") as w:
        w.setnchannels(1); w.setsampwidth(2); w.setframerate(48000)
        w.writeframes((x * 32767).astype("<i2").tobytes())
    print(f"{path}: {len(x) / 48000:.1f} s", file=sys.stderr)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("tags")
    ap.add_argument("out")
    ap.add_argument("--seconds", type=float, default=20)
    ap.add_argument("--bpm", type=int, default=120)
    ap.add_argument("--key", default="A minor")
    ap.add_argument("--seed", type=int)
    ap.add_argument("--raw", help="also keep the model's output here")
    a = ap.parse_args()
    data, rate = generate(a.tags, a.seconds, a.bpm, a.key, a.seed)
    if a.raw:
        sf.write(a.raw, data, rate)
    to_device_wav(data, rate, a.out)


if __name__ == "__main__":
    main()
