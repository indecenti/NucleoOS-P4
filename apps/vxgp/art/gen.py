"""Vertice GP: art and music from the local models (Qwen-Image 2.1 + ACE-Step 1.5 in ComfyUI).

Same prompt + same seed = same asset, so this script *is* the recipe. Writes apps/vxgp/img/*.565 and
apps/vxgp/snd/*.wav; previews go to ART_OUT (default %TEMP%/vxgp_art).
Run all:   python apps/vxgp/art/gen.py   (road needs ground; menu_split needs music_menu)
Run some:  python apps/vxgp/art/gen.py title pano0 music_menu
"""
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, "..", "..", ".."))
IMG = os.path.join(HERE, "..", "img")
SND = os.path.join(HERE, "..", "snd")
PREVIEW = os.environ.get("ART_OUT", os.path.join(os.environ.get("TEMP", "/tmp"), "vxgp_art"))
TOOL = [sys.executable, os.path.join(ROOT, "tools", "game_assets.py"), "--preview", PREVIEW]

STEPS = {
    # full-screen title painting: karts low and centre, the sky free for the logo
    "title": ["art", os.path.join(IMG, "title.565"), "--seed", "11", "--scene",
              "four small colourful cartoon go-karts (red, blue, green, orange) racing side by side toward the "
              "viewer on a sunny asphalt race circuit, low dynamic angle, red and white kerbs, a grandstand and "
              "chequered flags far behind, the karts in the lower half, a big clear blue sky with a few white "
              "clouds filling the upper half"],
    # the three circuits plus the podium, as cards for the menus
    "cards": ["grid", IMG, "--kind", "art", "--size", "256x150", "--seed", "23",
              "--names", "card0,card1,card2,podium", "--cells",
              "a kart race circuit winding through green meadows and pine woods, rolling hills, blue lakes, sunny day",
              "a kart race circuit through a red rock desert canyon, tall mesas, cactus, dusty warm afternoon light",
              "a kart race circuit in snowy alpine mountains, snow banks, snowy pine trees, crisp blue sky",
              "a winners podium with a golden trophy, confetti and fireworks at a kart race track at dusk"],
    # driver portraits (keyed sprites) for the driver select and the results
    "drivers": ["grid", IMG, "--kind", "sprite", "--size", "96x96", "--seed", "37",
                "--names", "drv0,drv1,drv2,drv3",
                "--subject", "the head and shoulders portrait of one cheerful cartoon kart racer, facing the viewer, "
                             "centred, filling the panel",
                "--cells",
                "a boy with a red racing helmet, visor up, red racing suit, big grin",
                "a girl with a blue racing helmet, visor up, blue racing suit, confident smile",
                "a bearded man with a green racing helmet, visor up, green racing suit, thumbs up",
                "a girl with an orange racing helmet, visor up, orange racing suit, winking"],
    # scenery billboards, one kind per circuit
    "trees": ["grid", IMG, "--kind", "sprite", "--size", "64x128", "--seed", "84",
              "--names", "tr_pine,tr_oak,tr_cactus,tr_snow",
              "--subject", "one whole tree standing, side view, centred, filling the panel height, trunk to the bottom",
              "--cells", "a tall dark green pine fir", "a round leafy green oak tree",
              "a tall saguaro cactus with two arms", "a pine fir covered in thick snow"],
    # trackside props + the coin and the trophy
    "props": ["grid", IMG, "--kind", "sprite", "--size", "64x64", "--seed", "52",
              "--names", "coin,tyres,hay,trophy",
              "--subject", "one single object, centred, whole object visible",
              "--cells", "a shiny gold coin with an embossed star, seen from the front",
              "a stack of black rubber tyres painted with red and white stripes",
              "a round golden hay bale", "a golden winners trophy cup with two handles"],
    # ground textures (Mode-7 floor) and the base grain for the asphalt
    "ground": ["grid", IMG, "--kind", "tex", "--size", "128x128", "--seed", "61",
               "--names", "g_grass,g_sand,g_snow,g_asphalt",
               "--cells", "short mowed green grass lawn", "warm orange desert sand with small pebbles",
               "fresh white snow with soft blue shadows", "dark grey road asphalt with fine grain"],
    # 360-degree horizons
    "pano0": ["pano", os.path.join(IMG, "pano0.565"), "--seed", "91", "--scene",
              "green rolling hills with round trees, dark pine woods, distant blue mountains with a little snow, "
              "pale blue sky"],
    "pano1": ["pano", os.path.join(IMG, "pano1.565"), "--seed", "92", "--scene",
              "red rock desert mesas and buttes, orange canyon walls, a few cactus, pale warm sky"],
    "pano2": ["pano", os.path.join(IMG, "pano2.565"), "--seed", "93", "--scene",
              "high snowy alpine peaks, glaciers, snowy pine forest at the foot, crisp pale blue sky"],
    # music
    "music_menu": ["music", os.path.join(SND, "menu.wav"), "--seconds", "30", "--bpm", "128", "--key", "E major",
                   "--seed", "5", "--tags", "upbeat kart racing game title theme, funky slap bass, bright synth "
                                            "brass, electric guitar riffs, energetic drums, loopable"],
    "music_win": ["music", os.path.join(SND, "win.wav"), "--seconds", "7", "--bpm", "120", "--key", "C major",
                  "--seed", "21", "--tags", "short triumphant victory fanfare, brass, drum roll, cymbal crash"],
    "music_lose": ["music", os.path.join(SND, "finish.wav"), "--seconds", "6", "--bpm", "100", "--key", "G major",
                   "--seed", "22", "--tags", "short friendly race finished jingle, synth, light drums"],
}


def post_road():
    """road.565: the painted asphalt grain + the circuit markings (u across the road, v along it):
    white edge lines, two darker rubbered-in racing lines, a dashed centre line."""
    sys.path.insert(0, os.path.join(ROOT, "tools"))
    import qwen_assets as q
    from PIL import Image
    import numpy as np
    src = os.path.join(PREVIEW, "g_asphalt.png")
    a = np.asarray(Image.open(src).convert("RGB").resize((128, 128), Image.LANCZOS)).astype(np.float32)
    g = a.mean(axis=2, keepdims=True)
    a = a * 0.35 + g * 0.65                                   # desaturate: grey road
    a = (a - a.mean()) * 0.8 + 100                            # mid-grey, gentle grain
    x = np.arange(128)
    band = ((x > 24) & (x < 44)) | ((x > 84) & (x < 104))
    a[:, band] *= 0.88
    a[:, (x <= 6) | (x >= 121)] = (236, 236, 230)
    a[:56, 62:66] = (245, 232, 180)
    q.to565(Image.fromarray(np.clip(a, 0, 255).astype(np.uint8)), os.path.join(IMG, "road.565"))
    print("wrote road.565")


def post_menu():
    """menu.wav -> menu0..5.wav (5 s parts): the game queues them one by one, so the race can take the
    speaker within one part (the OS plays one stream at a time)."""
    import wave
    src = os.path.join(SND, "menu.wav")
    with wave.open(src, "rb") as w:
        rate, ch, sw = w.getframerate(), w.getnchannels(), w.getsampwidth()
        data = w.readframes(w.getnframes())
    part = rate * 5 * ch * sw
    for i in range(6):
        chunk = data[i * part:(i + 1) * part]
        if not chunk:
            break
        with wave.open(os.path.join(SND, "menu%d.wav" % i), "wb") as o:
            o.setnchannels(ch); o.setsampwidth(sw); o.setframerate(rate)
            o.writeframes(chunk)
    os.remove(src)                                            # only the parts ship
    print("wrote menu0..5.wav")


POST = {"road": post_road, "menu_split": post_menu}


def main():
    os.makedirs(IMG, exist_ok=True)
    os.makedirs(SND, exist_ok=True)
    names = sys.argv[1:] or list(STEPS) + list(POST)
    failed = []
    for n in names:
        print("==", n, flush=True)
        if n in POST:
            POST[n]()
        elif subprocess.call(TOOL + STEPS[n]) != 0:
            failed.append(n)
    if failed:
        print("FAILED:", " ".join(failed))
        sys.exit(1)


if __name__ == "__main__":
    main()
