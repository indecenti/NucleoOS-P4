import os
import sys
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "..", "tools"))
import qwen_assets as q

OUT = os.environ.get("ART_OUT", os.path.join(os.environ.get("TEMP", "/tmp"), "bass_art")) + "/"
STYLE = ("Each is a beautiful stylised 16-bit era arcade video game illustration, vivid colours, clean shapes, "
         "rich detail, no text.")
jobs = [
    ("lakes2", ["autumn", "king", "title", "weigh"], 12,
     "A 2x2 grid of four separate, equally sized square paintings, separated by thin white borders. " + STYLE +
     " Top left: a lake in autumn with orange and red forests, a small bass fishing boat. "
     "Top right: a vast majestic lake with a golden castle on a hill and a grand fishing tournament flotilla of small boats. "
     "Bottom left: an angler standing in a small bass boat casting a fishing rod at dawn, the line arcing over a misty lake. "
     "Bottom right: a wooden tournament dock with a big hanging fish scale, flags and a cheering crowd."),
    ("fish1", ["bass", "trout", "pike", "catfish"], 21,
     "A 2x2 grid of four separate, equally sized square panels, separated by thin white borders, each on a plain deep navy "
     "blue background. " + STYLE + " Each panel shows one freshwater fish in side view facing right, whole body visible, "
     "centred. Top left: a largemouth bass, green with a dark lateral stripe. Top right: a rainbow trout, silver with a pink band "
     "and black speckles. Bottom left: a northern pike, long, green with pale spots. Bottom right: a brown catfish with long whiskers."),
    ("fish2", ["carp", "perch", "zander", "gold"], 22,
     "A 2x2 grid of four separate, equally sized square panels, separated by thin white borders, each on a plain deep navy "
     "blue background. " + STYLE + " Each panel shows one freshwater fish in side view facing right, whole body visible, "
     "centred. Top left: a common carp, bronze with big scales. Top right: a yellow perch with dark vertical bars and orange fins. "
     "Bottom left: a zander, grey-olive with dark bars and glassy eyes. Bottom right: a legendary golden bass, glowing gold, sparkles."),
    ("lures", ["crank", "popper", "worm", "rod"], 31,
     "A 2x2 grid of four separate, equally sized square panels, separated by thin white borders, each on a plain deep navy "
     "blue background. " + STYLE + " Each panel shows one fishing item, centred, large. Top left: a red and white crankbait "
     "fishing lure with a diving lip and treble hooks. Top right: a yellow and black popper fishing lure with a cupped face. "
     "Bottom left: a purple soft plastic worm fishing lure on a hook. Bottom right: a fishing rod with a spinning reel."),
]
only = sys.argv[1:]
for name, cells, seed, prompt in jobs:
    if only and name not in only:
        continue
    img = q.generate(prompt, 1024, 1024, seed)
    img.save(OUT + name + "_sheet.png")
    for n, cell in zip(cells, q.split_grid(img)):
        cell.save(OUT + name + "_" + n + ".png")
    print(name, "ok")
