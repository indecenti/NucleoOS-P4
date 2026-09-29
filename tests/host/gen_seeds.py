#!/usr/bin/env python3
"""Regenerate the fuzz seeds in tests/host/corpus (they are committed, so this only runs when a seed
format changes). Media seeds are tiny synthetic ffmpeg test patterns (64x48, a fraction of a second):
no third-party content. Needs ffmpeg on PATH.

    python tests/host/gen_seeds.py
"""
import os
import shutil
import struct
import subprocess
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
CORPUS = os.path.join(HERE, "corpus")
SRC = ["-f", "lavfi", "-i", "testsrc=size=64x48:rate=25:duration=0.28",   # MPEG-1: standard fps only
       "-f", "lavfi", "-i", "sine=frequency=440:sample_rate=%d:duration=0.28"]


def ffmpeg(rate, args, out):
    src = [a % rate if "%d" in a else a for a in SRC]
    subprocess.run(["ffmpeg", "-hide_banner", "-loglevel", "error", "-y", *src, *args, out], check=True)


def write(name, data):
    path = os.path.join(CORPUS, name)
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "wb") as f:
        f.write(data)
    print(f"{name:32s} {len(data):6d} B")


def moov_of(mp4):
    p = 0
    while p + 8 <= len(mp4):
        size, kind = struct.unpack(">I4s", mp4[p:p + 8])
        if kind == b"moov":
            return mp4[p:p + size]
        if size < 8:
            break
        p += size
    raise SystemExit("no moov box")


def main():
    if not shutil.which("ffmpeg"):
        raise SystemExit("ffmpeg not found")
    tmp = tempfile.mkdtemp()
    try:
        f = os.path.join(tmp, "a.mpg")
        ffmpeg(32000, ["-c:v", "mpeg1video", "-b:v", "40k", "-c:a", "mp2", "-b:a", "32k", "-f", "mpeg"], f)
        write("mpeg1/testsrc_av.mpg", open(f, "rb").read())
        ffmpeg(32000, ["-an", "-c:v", "mpeg1video", "-b:v", "40k", "-bf", "2", "-f", "mpeg"], f)
        write("mpeg1/testsrc_video_bframes.mpg", open(f, "rb").read())

        f = os.path.join(tmp, "a.avi")
        ffmpeg(8000, ["-c:v", "mjpeg", "-q:v", "20", "-c:a", "pcm_s16le", "-ac", "1"], f)
        avi = open(f, "rb").read()
        write("avi/mjpeg_pcm.avi", avi)
        write("avi/mjpeg_pcm_truncated.avi", avi[: len(avi) * 2 // 3])

        f = os.path.join(tmp, "a.mp4")
        ffmpeg(16000, ["-c:v", "libx264", "-pix_fmt", "yuv420p", "-profile:v", "baseline", "-g", "2",
                       "-c:a", "aac", "-b:a", "24k"], f)
        write("mp4/moov_x264_aac.moov", moov_of(open(f, "rb").read()))
    finally:
        shutil.rmtree(tmp, ignore_errors=True)

    web = {
        "plain": b"/DCIM/a.jpg\0{\"ssid\":\"home\",\"n\":3}",
        "encoded": b"%2Fsettings%2Envb\0{\"ssid\":\"a\\\"b\"}",
        "usb": b"/mnt/usb0/Music/x.mp3\0{}",
        "web": b"/web./index.html\0{\"k\":\"\\u0001\"}",
        "traversal": b"/a/..%2F..%2Fx\0",
    }
    for name, data in web.items():
        write(f"web/{name}.txt", data)


if __name__ == "__main__":
    main()
