#!/usr/bin/env python3
"""Build and run the DMD menu simulator in WSL, then turn its RGB565 shots into PNGs (out/*.png)."""
import glob, os, subprocess, sys
from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
wsl = subprocess.run(["wsl", "wslpath", "-a", HERE.replace("\\", "/")], capture_output=True, text=True).stdout.strip()
only = sys.argv[1] if len(sys.argv) > 1 else ""
os.makedirs(os.path.join(HERE, "out"), exist_ok=True)
for f in glob.glob(os.path.join(HERE, "out", "*")):
    os.remove(f)
cmd = "cd '%s' && make -j8 -s && FIND=512,148 LETTER=827,449 ALL=284,148 ./build/sim data out %s" % (wsl, only)
r = subprocess.run(["wsl", "-e", "bash", "-lc", cmd])
if r.returncode:
    sys.exit(r.returncode)
for f in sorted(glob.glob(os.path.join(HERE, "out", "*.rgb565"))):
    raw = open(f, "rb").read()
    im = Image.frombuffer("RGB", (1024, 600), raw, "raw", "BGR;16", 0, 1)
    im.save(f[:-7] + ".png")
    os.remove(f)
print("pngs:", len(glob.glob(os.path.join(HERE, "out", "*.png"))))
