import os
import sys
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "..", "tools"))
import qwen_assets as q

OUT = os.environ.get("ART_OUT", os.path.join(os.environ.get("TEMP", "/tmp"), "bass_art")) + "/"
STYLE = ("Each is a dramatic, cinematic 1990s Sega arcade game attract-mode illustration, 16-bit pixel art style, "
         "bold colours, strong lighting, dynamic composition, no text, no letters.")
jobs = [
    ("intro", ["dawn", "jump", "attack", "champ"], 61,
     "A 2x2 grid of four separate, equally sized square paintings, separated by thin white borders. " + STYLE +
     " Top left: a wide lake at sunrise, golden sun rays over misty mountains, a lone bass boat speeding with a white wake. "
     "Top right: a huge largemouth bass leaping out of the water toward the viewer, mouth wide open, a lure in its jaw, "
     "spray of droplets, sun behind. "
     "Bottom left: underwater close-up of a big bass lunging at a red crankbait lure, bubbles, green weeds, light rays. "
     "Bottom right: a champion angler on a stage holding a golden trophy high, fireworks and spotlights, cheering crowd."),
]
for name, cells, seed, prompt in jobs:
    img = q.generate(prompt, 1024, 1024, seed)
    img.save(OUT + name + "_sheet.png")
    for n, cell in zip(cells, q.split_grid(img)):
        cell.save(OUT + name + "_" + n + ".png")
    print(name, "ok", flush=True)
