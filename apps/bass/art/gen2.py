import os
import sys
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "..", "tools"))
import qwen_assets as q

OUT = os.environ.get("ART_OUT", os.path.join(os.environ.get("TEMP", "/tmp"), "bass_art")) + "/"
BASE = ("A 2x2 grid of four separate, equally sized square panels, separated by thin white borders, each on a plain "
        "deep navy blue background. Each panel shows one bold, simple, instantly readable video game icon, centred, large, "
        "chunky 16-bit arcade style with a thick dark outline, vivid colours, no text, no letters. ")
jobs = [
    ("hud", ["clock", "scale", "fishes", "hook"], 41, BASE +
     "Top left: a round stopwatch. Top right: a hanging fish weighing scale. Bottom left: two small fish crossed. "
     "Bottom right: a shiny steel fishing hook."),
    ("btn", ["reel", "twitch", "cast", "lure"], 42, BASE +
     "Top left: a fishing reel with a crank handle. Top right: a fishing rod tip jerking with motion lines. "
     "Bottom left: a fishing rod casting a line forward in an arc. Bottom right: a tackle box open with colourful lures."),
    ("award", ["trophy", "gold", "silver", "bronze"], 43, BASE +
     "Top left: a golden trophy cup with a fish on top. Top right: a gold medal with a red ribbon. "
     "Bottom left: a silver medal with a blue ribbon. Bottom right: a bronze medal with a green ribbon."),
]
only = sys.argv[1:]
for name, cells, seed, prompt in jobs:
    if only and name not in only:
        continue
    img = q.generate(prompt, 1024, 1024, seed)
    img.save(OUT + name + "_sheet.png")
    for n, cell in zip(cells, q.split_grid(img)):
        cell.save(OUT + name + "_" + n + ".png")
    print(name, "ok", flush=True)
