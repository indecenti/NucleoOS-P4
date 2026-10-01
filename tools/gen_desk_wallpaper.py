"""NucleoOS desktop wallpapers (classic desktop) with the local Qwen-Image 2.1 (tools/qwen_assets.py).

The NucleoOS identity: near-black navy space, the blue crystal nucleus of the logo, cyan orbits
(the Cardputer boot splash), a faint starfield. Left side kept empty for the desktop icons.

    python tools/gen_desk_wallpaper.py 11 23 37      # seeds -> tools/assets/desk_wallpaper_<seed>.jpg
"""
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import qwen_assets as q  # noqa: E402

OUT = Path(__file__).resolve().parent / "assets"

PROMPT = (
    "A premium minimalist computer desktop wallpaper, wide landscape format. Deep near-black navy "
    "blue space background, very dark, calm and clean. On the right third, a glowing faceted blue "
    "crystal sphere like an atomic nucleus, bright cyan-white light at its core, geometric crystal "
    "facets, with three thin luminous cyan orbital rings tilted around it at different angles and "
    "tiny glowing electrons on the rings. A soft subtle blue glow around the sphere. A faint sparse "
    "starfield. The whole left two thirds is almost empty dark space with only faint stars. "
    "Elegant, high detail, sharp, cinematic, no text, no letters, no logo text, no watermark."
)


def main() -> None:
    OUT.mkdir(exist_ok=True)
    seeds = [int(a) for a in sys.argv[1:]] or [11]
    for seed in seeds:
        img = q.generate(PROMPT, 1024, 640, seed)
        img = q.fit(img.convert("RGB"), 1024, 600, "cover")
        path = OUT / f"desk_wallpaper_{seed}.jpg"
        img.save(path, "JPEG", quality=90)
        print("saved", path, flush=True)


if __name__ == "__main__":
    main()
