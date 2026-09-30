import os
import sys
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "..", "tools"))
import qwen_assets as q

OUT = os.environ.get("ART_OUT", os.path.join(os.environ.get("TEMP", "/tmp"), "bass_art")) + "/"
STYLE = "Each is a beautiful stylised 16-bit era arcade video game illustration, vivid colours, clean shapes, rich detail, no text."
jobs = [
    ("scenes2", ["win", "lose", "school", "dock"], 51,
     "A 2x2 grid of four separate, equally sized square paintings, separated by thin white borders. " + STYLE +
     " Top left: a happy angler in a bass boat holding up a huge largemouth bass, confetti, sunny lake. "
     "Top right: a tired angler rowing a small boat back to shore at dusk, calm purple lake. "
     "Bottom left: underwater view of a school of bass swimming among green weeds and sun rays. "
     "Bottom right: a wooden fishing pier with a small cabin by a lake, a bass boat moored, morning light."),
    ("lures2", ["jig", "spinner", "crank2", "tackle"], 52,
     "A 2x2 grid of four separate, equally sized square panels, separated by thin white borders, each on a plain deep navy "
     "blue background. " + STYLE + " Each panel shows one fishing lure, centred, large, side view. Top left: a black and blue "
     "jig lure with a heavy round lead head, a silicone skirt and a single hook. Top right: a spinnerbait with a shiny silver "
     "blade and a chartreuse skirt. Bottom left: a green firetiger crankbait with a diving lip. Bottom right: a lead fishing sinker weight."),
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
