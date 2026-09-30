import os
import sys
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "..", "tools"))
import ace_music as a

OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "snd") + "/"
BASE = "instrumental, 1990s arcade video game music, 16-bit era, catchy melody, "
jobs = [
    ("lake0", BASE + "bright alpine morning, acoustic guitar, flute, light synth, cheerful", 110, "G major", 91),
    ("lake1", BASE + "warm sunset marsh, slow funk groove, electric piano, mellow sax, relaxed", 92, "F major", 92),
    ("lake2", BASE + "mysterious night dam, dark synth pads, pulsing bass, tense but cool", 100, "D minor", 93),
    ("lake3", BASE + "desert red canyon, western twang guitar, driving drums, adventurous", 118, "E minor", 94),
    ("lake4", BASE + "melancholic autumn lake, piano and strings, gentle beat, nostalgic", 96, "A minor", 95),
    ("lake5", BASE + "epic final stage, orchestral brass, heroic synth lead, big drums, grand", 128, "C major", 96),
    ("record", "instrumental, short triumphant arcade new record fanfare, brass, bells, cymbal, 1990s Sega style", 140, "D major", 97),
]
for name, tags, bpm, key, seed in jobs:
    secs = 7 if name == "record" else 16
    data, rate = a.generate(tags, secs, bpm, key, seed)
    a.to_device_wav(data, rate, OUT + name + ".wav")
    print(name, "ok", flush=True)
