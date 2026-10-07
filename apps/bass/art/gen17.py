"""gen17.py - Vertice Bass: the weigh-in with nothing to weigh (img/weigh0.565).

When the clock runs out and the livewell is empty, the weigh-in shows this instead of weigh.565 (the
station with a bass on the scale): the same dockside weigh station, the hook of the scale hanging
empty, the crowd small and quiet, a cloudier sky. Same recipe and framing as gen11.py's WEIGH.

    python apps/bass/art/gen17.py             # previews in %TEMP%/bass_art/g17_weigh0_<seed>.png
    python apps/bass/art/gen17.py --pick 1
"""
import os
import sys

from PIL import Image

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "..", "tools"))
import qwen_assets as q
from gen11 import IMG, OUT, STYLE

PROMPT = (STYLE + "A fishing tournament weigh-in station on a wooden lakeside dock: a big round dial hanging scale on "
          "a wooden gantry in the middle, its needle at zero and its hook hanging EMPTY, no fish anywhere, pennant "
          "bunting hanging loose, a few flags, a small quiet crowd far away at the bottom, a grey cloudy sky, a "
          "lake and pine trees behind. The whole scale inside the picture with sky above it. Wide landscape view.")
SEEDS = (341, 342)

if __name__ == "__main__":
    os.makedirs(OUT, exist_ok=True)
    if "--pick" in sys.argv:
        sd = SEEDS[int(sys.argv[sys.argv.index("--pick") + 1]) - 1]
        q.to565(q.fit(Image.open(os.path.join(OUT, "g17_weigh0_%d.png" % sd)), 512, 300), os.path.join(IMG, "weigh0.565"))
        print("wrote weigh0 from seed", sd)
        sys.exit(0)
    for sd in SEEDS:
        p = os.path.join(OUT, "g17_weigh0_%d.png" % sd)
        if not os.path.exists(p):
            q.generate(PROMPT, 1024, 608, sd).save(p)
        print("weigh0", sd, "ok", flush=True)
