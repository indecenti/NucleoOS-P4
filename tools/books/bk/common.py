"""Shared paths, HTTP and hashing helpers for the books catalog pipeline."""
import hashlib
import json
import os
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent          # tools/books
CACHE = Path(os.environ.get("NV_BOOKS_CACHE", ROOT / ".cache"))
OUT = Path(os.environ.get("NV_BOOKS_OUT", ROOT / "out"))
DATA = ROOT / "data"

UA = "NucleoOS-books-catalog/1.0 (+https://github.com/indecenti/nucleoos-p4-store)"
RELEASE_BASE = "https://github.com/indecenti/nucleoos-p4-store/releases/download"
MAX_ROWS_PER_SHARD = 500
MAX_ASSETS_PER_RELEASE = 1000

# Languages whose scripts our fonts cover (Latin, Greek, Cyrillic). Dead languages are left out on
# purpose (the user wants living languages); CJK, Arabic, Hebrew, Devanagari need fonts we do not ship.
LIVING_LANGS = {
    "it", "en", "de", "fr", "es", "pt", "nl", "fi", "sv", "da", "no", "nb", "nn", "is", "hu", "pl",
    "cs", "sk", "sl", "hr", "sr", "bg", "ru", "uk", "be", "ro", "ca", "gl", "eu", "oc", "el", "eo",
    "cy", "ga", "gd", "fy", "af", "et", "lv", "lt", "tl", "ilo", "ceb", "fur", "rm", "lb", "mt", "sq",
    "tr", "id", "ms", "sw", "vi", "br", "ia", "io", "vo",
}
DEAD_LANGS = {"la", "grc", "ang", "enm", "fro", "frm", "gmh", "goh", "non", "sa", "pi", "got", "cu"}
PRIORITY_LANGS = ["it", "en", "de", "fr", "es"]


def load_json(p, default=None):
    p = Path(p)
    if not p.exists():
        return default
    with open(p, encoding="utf-8") as f:
        return json.load(f)


def save_json(p, obj, compact=False):
    p = Path(p)
    p.parent.mkdir(parents=True, exist_ok=True)
    tmp = p.with_suffix(p.suffix + ".tmp")
    with open(tmp, "w", encoding="utf-8", newline="\n") as f:
        if compact:
            json.dump(obj, f, ensure_ascii=False, separators=(",", ":"))
        else:
            json.dump(obj, f, ensure_ascii=False, indent=1)
        f.write("\n")
    os.replace(tmp, p)


def sha256_file(p):
    h = hashlib.sha256()
    with open(p, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


class Http:
    """requests session with a fixed User-Agent and a minimum delay between requests per host."""

    def __init__(self, delay=2.0):
        import requests
        self.s = requests.Session()
        self.s.headers["User-Agent"] = UA
        self.delay = delay
        self.last = {}

    def _wait(self, url):
        host = url.split("/")[2]
        d = self.last.get(host)
        if d is not None:
            left = self.delay - (time.time() - d)
            if left > 0:
                time.sleep(left)
        self.last[host] = time.time()

    def get(self, url, **kw):
        self._wait(url)
        kw.setdefault("timeout", 60)
        for attempt in range(4):
            try:
                r = self.s.get(url, **kw)
                if r.status_code in (429, 502, 503, 504):
                    time.sleep(10 * (attempt + 1))
                    continue
                return r
            except Exception:
                if attempt == 3:
                    raise
                time.sleep(5 * (attempt + 1))
        return r

    def download(self, url, dest, **kw):
        dest = Path(dest)
        dest.parent.mkdir(parents=True, exist_ok=True)
        r = self.get(url, stream=True, **kw)
        r.raise_for_status()
        tmp = dest.with_suffix(dest.suffix + ".part")
        with open(tmp, "wb") as f:
            for c in r.iter_content(1 << 16):
                f.write(c)
        os.replace(tmp, dest)
        return dest
