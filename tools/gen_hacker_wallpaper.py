"""NucleoOS "hacker" desktop wallpaper with the local Qwen-Image 2.1 (tools/qwen_assets.py).

NucleoOS identity (the crystal nucleus with three orbits) in a terminal / hacker mood: deep dark
green-black, phosphor green and teal light, faint code rain. No blue, no purple, not bright.

    python tools/gen_hacker_wallpaper.py 5 17 29     # -> tools/assets/hacker_wallpaper_<seed>.jpg
"""
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import qwen_assets as q  # noqa: E402

OUT = Path(__file__).resolve().parent / "assets"

PROMPT = (
    "A dark premium computer desktop wallpaper in a hacker terminal style, wide landscape. Very dark "
    "green-black background like an old phosphor terminal. On the right third, a faceted crystal "
    "sphere like an atomic nucleus glowing in phosphor green and teal, with three thin luminous green "
    "orbital rings tilted around it and tiny glowing electrons. Faint vertical streams of tiny green "
    "code characters falling softly in the background, dim and out of focus, subtle scanlines and a "
    "faint grid. The left two thirds stay calm and dark for desktop icons. Low brightness, moody, "
    "cinematic, sharp. Colour palette strictly black, dark green, phosphor green and teal only. "
    "No blue, no purple, no pink, no text, no letters, no words, no logo, no watermark."
)


def main() -> None:
    OUT.mkdir(exist_ok=True)
    for seed in [int(a) for a in sys.argv[1:]] or [5]:
        img = q.generate(PROMPT, 1024, 640, seed)
        img = q.fit(img.convert("RGB"), 1024, 600, "cover")
        path = OUT / f"hacker_wallpaper_{seed}.jpg"
        img.save(path, "JPEG", quality=90, progressive=False)   # baseline: the board's decoder
        print("saved", path, flush=True)


if __name__ == "__main__":
    main()
