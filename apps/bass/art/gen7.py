import os
import sys
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "..", "tools"))
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import qwen_assets as q
from gen6 import key_fish, STYLE, OUT, IMG
prompt = ("A 2x2 grid of four separate, equally sized square panels, separated by thin white borders, each on a plain deep navy "
          "blue background. " + STYLE + " Each panel shows one object fished out of a lake, dripping water, centred, large, side view. "
          "Top left: a rusty dented tin can with a faded label. Top right: an old soggy brown leather boot with weeds hanging. "
          "Bottom left: a small black rubber tyre with algae. Bottom right: a small wooden treasure chest overflowing with gold coins.")
img = q.generate(prompt, 1024, 1024, 71)
img.save(OUT + "junk_sheet.png")
for i, cell in enumerate(q.split_grid(img)):
    key_fish(cell, IMG + "junk" + str(i) + ".565")
print("junk ok")
