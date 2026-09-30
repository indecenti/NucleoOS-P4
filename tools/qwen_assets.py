#!/usr/bin/env python3
"""Generate game art with Qwen-Image 2.1 through a local ComfyUI, and turn it into NucleoOS assets.

    python tools/qwen_assets.py gen "prompt" out.png [--w 1024 --h 1024 --seed 1 --lora name:0.8 ...]
    python tools/qwen_assets.py grid "prompt with a 2x2 grid" out_prefix [--names a,b,c,d]
    python tools/qwen_assets.py to565 in.png out.565 --w 512 --h 300 [--fit cover|contain]

Talks to ComfyUI's HTTP API (default http://127.0.0.1:8188): builds the same graph as the saved
"Qwen-Image-2.1" workflow (GGUF UNet, Qwen3-VL text encoder, 2.1 VAE, KSampler), optionally with
LoRAs, queues it, waits for it and downloads the image. With the 6-step turbo LoRA (default) a
1024x1024 image takes seconds. `grid` asks for a 2x2 sheet and cuts it into four images, so four
assets come out of one generation.

Device images (.565): a 4-byte header (uint16 width, uint16 height, little-endian) then RGB565
pixels, little-endian — what nv_gfx_image() and vx_texture_load() read from the app's img/ folder.
"""
import argparse
import io
import json
import os
import random
import struct
import sys
import time
import urllib.parse
import urllib.request

from PIL import Image

HOST = os.environ.get("COMFY", "http://127.0.0.1:8188")
UNET = "qwen_image_2.1-Q8_0.gguf"
CLIP = "qwen3vl_8b_fp8_scaled.safetensors"
VAE = "qwen_image_2.1_vae_bf16.safetensors"
TURBO = "qwen21_viggle_turbo_6step_r128.safetensors"
NEG = "blurry, low quality, watermark, signature, text, letters, logo, jpeg artifacts, deformed"


def _post(path, obj):
    req = urllib.request.Request(HOST + path, json.dumps(obj).encode(), {"Content-Type": "application/json"})
    return json.load(urllib.request.urlopen(req, timeout=60))


def _get(path):
    return urllib.request.urlopen(HOST + path, timeout=60).read()


def graph(prompt, w, h, seed, loras, steps, cfg, neg):
    g = {
        "1": {"class_type": "UnetLoaderGGUF", "inputs": {"unet_name": UNET}},
        "2": {"class_type": "CLIPLoader", "inputs": {"clip_name": CLIP, "type": "qwen_image", "device": "default"}},
        "3": {"class_type": "VAELoader", "inputs": {"vae_name": VAE}},
    }
    model = ["1", 0]
    for i, (name, strength) in enumerate(loras):
        nid = str(10 + i)
        g[nid] = {"class_type": "LoraLoaderModelOnly",
                  "inputs": {"model": model, "lora_name": name, "strength_model": strength}}
        model = [nid, 0]
    g["4"] = {"class_type": "TextEncodeQwenImage21",
              "inputs": {"clip": ["2", 0], "prompt": prompt, "negative_prompt": neg, "resolution": 1024}}
    g["5"] = {"class_type": "EmptyLatentImage", "inputs": {"width": w, "height": h, "batch_size": 1}}
    g["6"] = {"class_type": "KSampler", "inputs": {
        "model": model, "positive": ["4", 0], "negative": ["4", 1], "latent_image": ["5", 0],
        "seed": seed, "steps": steps, "cfg": cfg, "sampler_name": "euler", "scheduler": "simple", "denoise": 1.0}}
    g["7"] = {"class_type": "VAEDecode", "inputs": {"samples": ["6", 0], "vae": ["3", 0]}}
    g["8"] = {"class_type": "SaveImage", "inputs": {"images": ["7", 0], "filename_prefix": "nucleo_asset"}}
    return g


def generate(prompt, w=1024, h=1024, seed=None, loras=None, turbo=True, steps=None, cfg=None, neg=NEG, tries=4):
    """One image; ComfyUI's model loader fails now and then (HostBuffer.read_file_slice): retried."""
    for attempt in range(tries):
        try:
            return _generate(prompt, w, h, seed, loras, turbo, steps, cfg, neg)
        except RuntimeError as e:
            if attempt == tries - 1:
                raise
            print(f"  retry after: {str(e)[:120]}", file=sys.stderr)
            time.sleep(3)


def _generate(prompt, w, h, seed, loras, turbo, steps, cfg, neg):
    loras = list(loras or [])
    if turbo:
        loras.append((TURBO, 1.0))
    steps = steps or (6 if turbo else 25)
    cfg = cfg if cfg is not None else 1.0
    seed = seed if seed is not None else random.randint(1, 2 ** 31)
    pid = _post("/prompt", {"prompt": graph(prompt, w, h, seed, loras, steps, cfg, neg)})["prompt_id"]
    t0 = time.time()
    while True:
        hist = json.loads(_get("/history/" + pid))
        if pid in hist:
            outs = hist[pid]["outputs"]
            for node in outs.values():
                for im in node.get("images", []):
                    q = urllib.parse.urlencode({"filename": im["filename"], "subfolder": im["subfolder"], "type": im["type"]})
                    img = Image.open(io.BytesIO(_get("/view?" + q))).convert("RGB")
                    print(f"  generated {w}x{h} seed {seed} in {time.time() - t0:.1f} s", file=sys.stderr)
                    return img
            status = hist[pid].get("status", {})
            if status.get("status_str") == "error":
                raise RuntimeError(json.dumps(status)[:600])
        if time.time() - t0 > 900:
            raise TimeoutError("ComfyUI took too long")
        time.sleep(1.0)


def split_grid(img, n=2):
    w, h = img.size
    cw, ch = w // n, h // n
    return [img.crop((c * cw, r * ch, (c + 1) * cw, (r + 1) * ch)) for r in range(n) for c in range(n)]


def fit(img, w, h, mode="cover"):
    sw, sh = img.size
    if mode == "cover":
        k = max(w / sw, h / sh)
        img = img.resize((max(w, round(sw * k)), max(h, round(sh * k))), Image.LANCZOS)
        x, y = (img.width - w) // 2, (img.height - h) // 2
        return img.crop((x, y, x + w, y + h))
    k = min(w / sw, h / sh)
    img = img.resize((round(sw * k), round(sh * k)), Image.LANCZOS)
    out = Image.new("RGB", (w, h), (0, 0, 0))
    out.paste(img, ((w - img.width) // 2, (h - img.height) // 2))
    return out


def to565(img, path):
    img = img.convert("RGB")
    w, h = img.size
    px = img.tobytes()
    buf = bytearray(struct.pack("<HH", w, h))
    for i in range(0, len(px), 3):
        r, g, b = px[i], px[i + 1], px[i + 2]
        buf += struct.pack("<H", ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3))
    os.makedirs(os.path.dirname(os.path.abspath(path)), exist_ok=True)
    with open(path, "wb") as f:
        f.write(buf)


def main():
    ap = argparse.ArgumentParser()
    sub = ap.add_subparsers(dest="cmd", required=True)
    for name in ("gen", "grid"):
        p = sub.add_parser(name)
        p.add_argument("prompt")
        p.add_argument("out")
        p.add_argument("--w", type=int, default=1024)
        p.add_argument("--h", type=int, default=1024)
        p.add_argument("--seed", type=int)
        p.add_argument("--lora", action="append", default=[], help="name:strength")
        p.add_argument("--no-turbo", action="store_true")
        p.add_argument("--names", default="")
    p = sub.add_parser("to565")
    p.add_argument("inp")
    p.add_argument("out")
    p.add_argument("--w", type=int, required=True)
    p.add_argument("--h", type=int, required=True)
    p.add_argument("--fit", default="cover")
    a = ap.parse_args()
    if a.cmd == "to565":
        to565(fit(Image.open(a.inp), a.w, a.h, a.fit), a.out)
        return
    loras = [(l.split(":")[0], float(l.split(":")[1]) if ":" in l else 1.0) for l in a.lora]
    img = generate(a.prompt, a.w, a.h, a.seed, loras, not a.no_turbo)
    if a.cmd == "gen":
        img.save(a.out)
        print(a.out)
    else:
        names = a.names.split(",") if a.names else [str(i) for i in range(4)]
        img.save(a.out + "_sheet.png")
        for name, cell in zip(names, split_grid(img)):
            cell.save(f"{a.out}_{name}.png")
            print(f"{a.out}_{name}.png")


if __name__ == "__main__":
    main()
