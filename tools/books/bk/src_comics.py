"""Free comics: Pepper&Carrot by David Revoy (CC BY 4.0), translated by volunteers into dozens of languages.

Index: https://www.peppercarrot.com/0_sources/episodes-v1.json and langs.json (official JSON).
Pages: the published "low-res" JPEGs, fetched politely (1 request/s), resized to 1000 px wide
(the board screen is 1024x600; CC BY allows adaptation, noted on the credits page) and packed
10 episodes per EPUB volume, each episode a chapter, one image per page, the authors' credit pages kept.
"""
import io
from collections import Counter, defaultdict

from . import epubbuild
from .common import CACHE, LIVING_LANGS, Http, load_json, save_json

BASE = "https://www.peppercarrot.com/0_sources/"
DIR = CACHE / "comics"
EPUB_DIR = DIR / "epub"
CAND = CACHE / "candidates-comics.json"
PER_VOLUME = 10
MIN_EPISODES = 30
WIDTH = 1000
POP = 800        # curated items carry a fixed popularity (no download counts at the source)

TXT = {
    "it": ("Pepper&Carrot, volume %d (episodi %d-%d)", "Episodio %d", "Fumetto libero di David Revoy: le avventure "
           "della giovane strega Pepper e del suo gatto Carrot. Volume %d, episodi %d-%d."),
    "en": ("Pepper&Carrot, volume %d (episodes %d-%d)", "Episode %d", "Free webcomic by David Revoy: the adventures of "
           "the young witch Pepper and her cat Carrot. Volume %d, episodes %d-%d."),
    "de": ("Pepper&Carrot, Band %d (Episoden %d-%d)", "Episode %d", "Freier Webcomic von David Revoy: die Abenteuer der "
           "jungen Hexe Pepper und ihres Katers Carrot. Band %d, Episoden %d-%d."),
    "fr": ("Pepper&Carrot, tome %d (épisodes %d-%d)", "Épisode %d", "Bande dessinée libre de David Revoy : les aventures "
           "de la jeune sorcière Pepper et de son chat Carrot. Tome %d, épisodes %d-%d."),
    "es": ("Pepper&Carrot, volumen %d (episodios %d-%d)", "Episodio %d", "Cómic libre de David Revoy: las aventuras de la "
           "joven bruja Pepper y su gato Carrot. Volumen %d, episodios %d-%d."),
}


def _txt(lang):
    return TXT.get(lang, ("Pepper&Carrot %d (%d-%d)", "%d", ""))


def _jpeg(data):
    from PIL import Image
    im = Image.open(io.BytesIO(data)).convert("RGB")
    if im.width > WIDTH:
        im = im.resize((WIDTH, round(im.height * WIDTH / im.width)), Image.LANCZOS)
    out = io.BytesIO()
    im.save(out, "JPEG", quality=82, optimize=True, progressive=False)
    return out.getvalue()


def build(langs=None, log=print):
    http = Http(delay=1.0)
    eps = http.get(BASE + "episodes-v1.json").json()
    langinfo = http.get(BASE + "langs.json").json()
    recs, excl = [], defaultdict(Counter)
    for code, li in sorted(langinfo.items()):
        iso = {"nb": "no"}.get(li.get("iso_code"), li.get("iso_code"))
        if not iso or iso not in LIVING_LANGS or code in ("mx", "ls", "ga", "go", "gb", "kh"):
            continue
        if langs and iso not in langs:
            continue
        mine = [e for e in eps if code in e["translated_languages"]]
        if len(mine) < MIN_EPISODES:
            excl[iso]["comics:incomplete_translation"] += 1
            continue
        for v in range(0, len(mine), PER_VOLUME):
            vol = v // PER_VOLUME + 1
            group = mine[v:v + PER_VOLUME]
            first = int(group[0]["name"][2:4])
            last = int(group[-1]["name"][2:4])
            rid = "mn-pepper-%s-%d" % (code, vol)
            dest = EPUB_DIR / (rid + ".epub")
            tt, et, dt = _txt(iso)
            title = tt % (vol, first, last)
            if not dest.exists():
                images, chapters = {}, []
                for e in group:
                    n = int(e["name"][2:4])
                    pages = e["pages"]
                    order = ["title"] + sorted([k for k in pages if k.isdigit()], key=int) + ["credits"]
                    body = []
                    for k in order:
                        fn = pages.get(k)
                        if not fn:
                            continue
                        url = "%s%s/low-res/%s_%s" % (BASE, e["name"], code, fn)
                        cache = DIR / "img" / code / ("%s_%s" % (code, fn))
                        if not cache.exists():
                            r = http.get(url)
                            if r.status_code != 200:
                                log("comics: missing %s" % url)
                                continue
                            cache.parent.mkdir(parents=True, exist_ok=True)
                            cache.write_bytes(_jpeg(r.content))
                        path = "img/%s" % fn
                        images[path] = cache.read_bytes()
                        body.append('<div class="center"><img src="%s" alt="%s"/></div>' % (path, epubbuild._esc(et % n)))
                    chapters.append((et % n, "".join(body)))
                tr = ", ".join(li.get("translators", []))
                lic = ("<h2>Pepper&amp;Carrot</h2><p>Pepper&amp;Carrot © David Revoy, https://www.peppercarrot.com - "
                       "Creative Commons Attribution 4.0 International (CC BY 4.0, "
                       "https://creativecommons.org/licenses/by/4.0/).</p><p>%s: %s.</p><p>Pages resized to %d px for "
                       "NucleoOS Books; the credit page of each episode is kept.</p>"
                       % (epubbuild._esc(li.get("local_name", iso)), epubbuild._esc(tr), WIDTH))
                epubbuild.build(dest, title, ["David Revoy"], iso, chapters, lic, identifier=rid,
                                description=dt % (vol, first, last) if dt else "", source_url="https://www.peppercarrot.com",
                                images=images)
                log("comics %s ok (%d pages)" % (rid, len(images)))
            recs.append({"id": rid, "lang": iso, "t": title, "a": "David Revoy", "ad": "", "c": ["comics", "children"],
                         "src": "mn", "lic": "cc-by", "pop": POP - vol,
                         "d": (dt % (vol, first, last)) if dt else "", "u2": "https://www.peppercarrot.com/%s/" % code,
                         "_file": str(dest),
                         "_attrib": "Pepper&Carrot © David Revoy (CC BY 4.0), translation: %s" % ", ".join(li.get("translators", []))})
    prev = load_json(CAND, {}) or {}
    keep = [r for r in prev.get("records", []) if langs and r["lang"] not in langs]
    save_json(CAND, {"records": keep + recs, "excluded": excl})
    log("comics candidates: %d" % len(recs))
    return recs
