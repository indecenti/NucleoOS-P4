"""gen10.py - Vertice Bass: the two fish paintings of the intro, redone in the screen's shape.

intro1 (the leap) and intro2 (the strike) were cells of a square 2x2 grid (gen4.py) cut down to
512x300, which lost the fish's head and dorsal fin at the top. These are generated landscape
(1024x608, the canvas' shape) with the whole fish in frame and room above it, in two seeds each;
the chosen ones are written to img/intro1.565 and img/intro2.565 by --pick.

    python apps/bass/art/gen10.py                 # previews in %TEMP%/bass_art/intro10_*.png
    python apps/bass/art/gen10.py --pick 1 2      # write the chosen variants
"""
import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "..", "tools"))
import qwen_assets as q

APP = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")
OUT = os.environ.get("ART_OUT", os.path.join(os.environ.get("TEMP", "/tmp"), "bass_art")) + "/"
STYLE = ("A dramatic, cinematic 1990s Sega arcade game attract-mode illustration, 16-bit pixel art style, "
         "bold colours, strong lighting, dynamic composition, no text, no letters. ")
FRAME = ("The WHOLE fish is inside the picture, from its open mouth and the tip of its dorsal fin to the "
         "end of its tail, with clear empty space above its head; the fish sits in the middle of the frame "
         "and does not touch any edge. Wide landscape composition.")
JOBS = {
    "intro1": (STYLE + "A huge largemouth bass leaping out of a lake toward the viewer, mouth wide open, a lure "
               "in its jaw, a spray of droplets, the sun behind it, blue sky. " + FRAME, (71, 72)),
    "intro2": (STYLE + "Underwater: a big largemouth bass lunging at a small red crankbait lure, mouth wide open, "
               "bubbles, green weeds on the bottom, light rays from the surface. " + FRAME, (81, 82)),
}

if __name__ == "__main__":
    os.makedirs(OUT, exist_ok=True)
    if "--pick" in sys.argv:
        picks = [int(v) for v in sys.argv[sys.argv.index("--pick") + 1:]]
        for (name, (_, seeds)), k in zip(JOBS.items(), picks):
            from PIL import Image
            img = Image.open(OUT + "intro10_%s_%d.png" % (name, seeds[k - 1]))
            q.to565(q.fit(img, 512, 300), os.path.join(APP, "img", name + ".565"))
            print("wrote", name, "from seed", seeds[k - 1])
        sys.exit(0)
    for name, (prompt, seeds) in JOBS.items():
        for sd in seeds:
            img = q.generate(prompt, 1024, 608, sd)
            img.save(OUT + "intro10_%s_%d.png" % (name, sd))
            print(name, sd, "ok", flush=True)
