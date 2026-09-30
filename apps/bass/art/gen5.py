import os
import sys
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "..", "tools"))
import numpy as np
import qwen_assets as q
from PIL import Image

OUT = os.environ.get("ART_OUT", os.path.join(os.environ.get("TEMP", "/tmp"), "bass_art")) + "/"
prompt = ("A 2x2 grid of four separate, equally sized square seamless tileable textures, separated by thin white borders, "
          "flat top-down orthographic view, even lighting, no shadows, no objects, 16-bit video game texture style, rich detail. "
          "Top left: a freshwater lake bed of fine sand with small pebbles, tiny shells and patches of green algae. "
          "Top right: a mossy grey granite boulder surface with cracks and green moss. "
          "Bottom left: waterlogged dark brown tree bark with deep grooves. "
          "Bottom right: a muddy lake bottom with dark silt, twigs and scattered leaves.")
img = q.generate(prompt, 1024, 1024, 81)
img.save(OUT + "tex_sheet.png")


def tileable(im):
    """Make a texture wrap: cross-fade it with a half-shifted copy, weighting toward the copy at the edges."""
    a = np.asarray(im).astype(np.float32)
    h, w, _ = a.shape
    b = np.roll(np.roll(a, h // 2, 0), w // 2, 1)
    y = np.abs(np.linspace(-1, 1, h))[:, None]
    x = np.abs(np.linspace(-1, 1, w))[None, :]
    m = np.clip(np.maximum(x, y) * 1.4 - 0.4, 0, 1)[..., None]
    return Image.fromarray((a * (1 - m) + b * m).astype(np.uint8))


for name, cell in zip(["bed", "rock", "bark", "mud"], q.split_grid(img)):
    cell = cell.crop((14, 14, cell.width - 14, cell.height - 14)).resize((256, 256), Image.LANCZOS)
    t = tileable(cell).resize((128, 128), Image.LANCZOS)
    t.save(OUT + "tex_" + name + ".png")
    q.to565(t, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "img", "t_") + name + ".565")
    print(name)
