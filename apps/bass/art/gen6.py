import os
import sys
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "..", "tools"))
import qwen_assets as q
from PIL import Image, ImageDraw

OUT = os.environ.get("ART_OUT", os.path.join(os.environ.get("TEMP", "/tmp"), "bass_art")) + "/"
IMG = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "img") + "/"
STYLE = ("Each is a beautiful stylised 16-bit era arcade video game illustration, vivid colours, clean shapes, "
         "rich detail, no text.")
HEAD = ("A 2x2 grid of four separate, equally sized square panels, separated by thin white borders, each on a plain deep navy "
        "blue background. " + STYLE + " Each panel shows one freshwater fish in side view facing right, whole body visible, centred. ")
SMALL = "a small young slim juvenile "
BIG = "a huge fat trophy-sized old monster "
jobs = [
    ("fs1", [0, 1, 2, 3], "_s", 61, HEAD + "Top left: " + SMALL + "largemouth bass, light green with a faint stripe. Top right: " + SMALL +
     "rainbow trout, small silver fingerling with parr marks. Bottom left: " + SMALL + "northern pike, thin pencil-like, pale green. "
     "Bottom right: " + SMALL + "catfish, small brown with whiskers."),
    ("fs2", [4, 5, 6, 7], "_s", 62, HEAD + "Top left: " + SMALL + "common carp, small bronze. Top right: " + SMALL +
     "yellow perch, tiny with dark bars. Bottom left: " + SMALL + "zander, slim grey. Bottom right: " + SMALL + "golden bass, small shiny gold."),
    ("fb1", [0, 1, 2, 3], "_b", 63, HEAD + "Top left: " + BIG + "largemouth bass, deep bellied, dark green, huge mouth open. Top right: " + BIG +
     "rainbow trout, thick with a vivid red band and hooked jaw. Bottom left: " + BIG + "northern pike, very long, toothy jaws, scarred. "
     "Bottom right: " + BIG + "catfish, massive dark wide head, long whiskers."),
    ("fb2", [4, 5, 6, 7], "_b", 64, HEAD + "Top left: " + BIG + "common carp, enormous golden bronze with big mirror scales. Top right: " + BIG +
     "yellow perch, humped back, bold bars, bright orange fins. Bottom left: " + BIG + "zander, long with fangs and glassy eyes. "
     "Bottom right: " + BIG + "legendary golden bass, radiant glowing gold with sparkles."),
]


def key_fish(cell, path):
    c = cell.convert("RGB")
    c = c.crop((16, 16, c.width - 16, c.height - 16))
    bg = c.getpixel((4, 4))
    c.thumbnail((150, 100), Image.LANCZOS)
    out = Image.new("RGB", (150, 100), bg)
    out.paste(c, ((150 - c.width) // 2, (100 - c.height) // 2))
    for x in range(0, 150, 5):
        for y in (0, 99):
            if out.getpixel((x, y)) != (255, 0, 255):
                ImageDraw.floodfill(out, (x, y), (255, 0, 255), thresh=60)
    for y in range(0, 100, 5):
        for x in (0, 149):
            if out.getpixel((x, y)) != (255, 0, 255):
                ImageDraw.floodfill(out, (x, y), (255, 0, 255), thresh=60)
    out.save(path.replace(".565", ".png").replace(IMG, OUT + "k_"))
    q.to565(out, path)


if __name__ != "__main__":
    jobs = []
only = sys.argv[1:] or ([] if __name__ == "__main__" else ["none"])
for name, sps, suf, seed, prompt in jobs:
    if only and name not in only:
        continue
    img = q.generate(prompt, 1024, 1024, seed)
    img.save(OUT + name + "_sheet.png")
    for sp, cell in zip(sps, q.split_grid(img)):
        key_fish(cell, IMG + "fish" + str(sp) + suf + ".565")
    print(name, "ok", flush=True)

if not only or "bg" in only:
    prompt = ("A 2x2 grid of four separate, equally sized square paintings, separated by thin white borders. " + STYLE +
              " Each painting is a calm pond shore scene, wide view, empty centre with open sky and water, no people, no fish. "
              "Top left: a small green pond with reeds, lily pads and a wooden jetty on a bright sunny morning. "
              "Top right: a pond with cattails glowing under an orange sunset sky. "
              "Bottom left: a moonlit pond at night with fireflies, blue tones. "
              "Bottom right: a pond in autumn surrounded by red and orange trees and falling leaves.")
    img = q.generate(prompt, 1024, 1024, 65)
    img.save(OUT + "pond_sheet.png")
    for i, cell in enumerate(q.split_grid(img)):
        cell = cell.crop((10, 10, cell.width - 10, cell.height - 10))
        q.to565(q.fit(cell, 512, 300), IMG + "pond" + str(i) + ".565")
    print("bg ok", flush=True)
