"""F5 publishing (only with the user's OK): Releases for the EPUBs, books/ on Pages for the indexes.

GitHub allows about 500 content-creating requests an hour (each asset upload is one), so uploads go one
at a time at a steady pace (UPLOAD_EVERY seconds), and a secondary-rate-limit answer pauses until the
time GitHub gives. A language goes live with the rows whose asset is verified (name, size and GitHub's
sha256 digest equal to the row's s/h) and grows at every checkpoint: after each CHECKPOINT uploads its
shards are rewritten from the verified rows only and books/ is pushed. The app never sees a row whose
file is not downloadable. Releases are never marked Latest, so the firmware channels are untouched.
"""
import datetime
import hashlib
import json
import os
import re
import shutil
import subprocess
import time
from pathlib import Path

from .common import CACHE, MAX_ROWS_PER_SHARD, OUT, load_json, save_json

FROZEN = CACHE / "published"            # the exact file of every published id (hard links)
FROZEN_INDEX = FROZEN / "index.json"    # id -> [size, sha256]

REPO = "indecenti/nucleoos-p4-store"
UPLOAD_EVERY = 8.0          # seconds between uploads: ~450 an hour, under GitHub's ~500/hour
CHECKPOINT = 400            # push the verified rows of a language after this many uploads
NOTES = {
    "it": "Libri liberi (pubblico dominio e licenze libere) per l'app Libri di NucleoOS. Catalogo, licenze e crediti: "
          "https://indecenti.github.io/nucleoos-p4-store/books/",
}
NOTES_DEFAULT = ("Free books (public domain and free licences) for the NucleoOS Books app, language: {lang}. "
                 "Catalog, licences and credits: https://indecenti.github.io/nucleoos-p4-store/books/")


def gh(*args, check=True, log=print):
    """Run gh; a rate-limit answer waits for the time GitHub names and retries (any call, not only uploads)."""
    for _ in range(30):         # secondary-limit penalties can last an hour or more
        r = subprocess.run(["gh", *args], capture_output=True, text=True, encoding="utf-8")
        err = (r.stderr or r.stdout or "")
        if r.returncode != 0 and _limited(err):
            _wait_for_limit(err, log)
            continue
        break
    if check and r.returncode != 0:
        raise RuntimeError("gh %s: %s" % (" ".join(args[:3]), err.strip()[-400:]))
    return r


def _limited(text):
    return "secondary rate limit" in text or "scraping GitHub" in text or "abuse" in text.lower() \
        or "rate limit" in text.lower()


_limit_hits = []


def _wait_for_limit(text, log):
    """Back off 10, 20, then 30 minutes (the time in GitHub's message is the request's, not the reset's;
    for the primary limit the reset time comes from /rate_limit)."""
    now = time.time()
    _limit_hits[:] = [t for t in _limit_hits if now - t < 3 * 3600] + [now]
    wait = min(30, 10 * len(_limit_hits)) * 60
    if "API rate limit exceeded" in text:
        # the primary limit only if it is really used up; otherwise it is the secondary one (keep backing off)
        r = subprocess.run(["gh", "api", "rate_limit", "--jq", ".resources.core | [.remaining, .reset] | @tsv"],
                           capture_output=True, text=True)
        try:
            remaining, reset = (int(x) for x in r.stdout.split())
            if remaining < 50:
                wait = max(60, reset - now + 60)
        except ValueError:
            pass
    log("  GitHub rate limit (%s): pausing %d min" % (datetime.datetime.now().strftime("%H:%M"), wait // 60))
    time.sleep(wait)


def release_assets(tag):
    r = gh("api", "repos/%s/releases/tags/%s" % (REPO, tag), check=False)
    if r.returncode != 0:
        return None
    rid = json.loads(r.stdout)["id"]
    out, page = {}, 1
    while True:
        a = json.loads(gh("api", "repos/%s/releases/%d/assets?per_page=100&page=%d" % (REPO, rid, page)).stdout)
        for x in a:
            out[x["name"]] = {"size": x["size"], "digest": x.get("digest"), "state": x.get("state")}
        if len(a) < 100:
            return out
        page += 1


def ensure_release(tag, lang):
    have = release_assets(tag)
    if have is not None:
        return have
    n = tag.rsplit("-", 1)[-1]
    gh("release", "create", tag, "--repo", REPO, "--title", "Libri %s %s" % (lang, n),
       "--notes", NOTES.get(lang, NOTES_DEFAULT.format(lang=lang)), "--latest=false")
    return {}


def _good(a, r):
    return bool(a) and a["size"] == r["s"] and a.get("state") in (None, "uploaded") and \
        (not a.get("digest") or a["digest"] == "sha256:" + r["h"])


def lang_rows(lang):
    rows = []
    for f in sorted((OUT / "books" / lang).glob("*.json"), key=lambda p: int(p.stem)):
        rows += load_json(f)["books"]
    return rows


def _tag(r):
    return r["u"].split("/download/")[1].split("/")[0]


def verified(rows, cache):
    """Rows whose asset is on GitHub and matches; cache = {tag: assets} (refreshed by the caller)."""
    out = []
    for r in rows:
        tag = _tag(r)
        if tag not in cache:
            cache[tag] = release_assets(tag) or {}
        if _good(cache[tag].get(r["id"] + ".epub"), r):
            out.append(r)
    return out


def upload(r, log):
    """One asset, retried through rate limits. True when GitHub accepted it."""
    f = OUT / "releases" / _tag(r) / (r["id"] + ".epub")
    for attempt in range(6):
        p = gh("release", "upload", _tag(r), "--repo", REPO, "--clobber", str(f), check=False)
        if p.returncode == 0:
            return True
        err = (p.stderr or p.stdout or "")
        if _limited(err):
            _wait_for_limit(err, log)
            continue
        log("  upload %s failed: %s" % (r["id"], err.strip()[-200:]))
        time.sleep(30 * (attempt + 1))
    return False


def head_check(rows, log=print, n=5):
    """HEAD a few public download URLs exactly as the app will use them: 200 after redirects, right size."""
    import random
    import requests
    good = True
    for r in random.Random(len(rows)).sample(rows, min(n, len(rows))):
        try:
            h = requests.head(r["u"], allow_redirects=True, timeout=60)
            size = int(h.headers.get("content-length", -1))
        except Exception as e:
            h, size = None, -1
            log("  HEAD %s: %s" % (r["u"], e))
        if not h or h.status_code != 200 or size != r["s"]:
            log("  HEAD NOT OK %s: %s size %s (want %d)" % (r["u"], h.status_code if h else "-", size, r["s"]))
            good = False
    log("  HEAD check %s on %d urls" % ("ok" if good else "FAILED", min(n, len(rows))))
    return good


PACK = 30   # covers per pack: books/covers/<lang>/<k>-<p>.bin = rows [p*PACK, p*PACK+PACK) of shard k


def cover_pack(items):
    """NVCP v1, little-endian: "NVCP" u8 1 u8 0 u16 n | n x {u8 idlen, id, u32 off, u32 len} | the JPEGs."""
    import struct
    head = b"NVCP" + struct.pack("<BBH", 1, 0, len(items))
    table_len = sum(1 + len(i.encode()) + 8 for i, _ in items)
    off = len(head) + table_len
    table, blobs = b"", b""
    for i, data in items:
        idb = i.encode()
        table += struct.pack("<B", len(idb)) + idb + struct.pack("<II", off, len(data))
        off += len(data)
        blobs += data
    return head + table + blobs


def write_cover_packs(pdir, lang, shards):
    """One pack per PACK rows of each shard (rows without a cover are skipped but keep their place)."""
    if pdir.exists():
        shutil.rmtree(pdir)
    pdir.mkdir(parents=True)
    out = []
    for k, part in enumerate(shards, 1):
        for p in range(0, (len(part) + PACK - 1) // PACK):
            items = [(r["id"], (OUT / "books" / "covers" / (r["id"] + ".jpg")).read_bytes())
                     for r in part[p * PACK:(p + 1) * PACK] if r.get("cv")]
            if not items:
                continue
            f = pdir / ("%d-%d.bin" % (k, p))
            f.write_bytes(cover_pack(items))
            out.append(f)
    return out


def stage_language(store, lang, rows, log=print):
    """Write the verified rows of a language into <store>/books (own shards, index entry, covers)."""
    dst = Path(store) / "books"
    ldir = dst / lang
    if ldir.exists():
        shutil.rmtree(ldir)
    ldir.mkdir(parents=True)
    shards = [rows[i:i + MAX_ROWS_PER_SHARD] for i in range(0, len(rows), MAX_ROWS_PER_SHARD)]
    for k, part in enumerate(shards, 1):
        save_json(ldir / ("%d.json" % k), {"format": 1, "lang": lang, "part": k, "parts": len(shards),
                                            "books": part}, compact=True)
    cdir = dst / "covers"
    cdir.mkdir(exist_ok=True)
    for r in rows:
        if r.get("cv"):
            shutil.copy2(OUT / "books" / "covers" / (r["id"] + ".jpg"), cdir / (r["id"] + ".jpg"))
    packs = write_cover_packs(cdir / lang, lang, shards)
    full = load_json(OUT / "books" / "index-v1.json")
    pub = load_json(dst / "index-v1.json", None) or {"format": 1, "langs": []}
    # rev changes exactly when the language's shards change (the app keys its cache on count/shards/rev)
    h = hashlib.sha256()
    for f in sorted(ldir.glob("*.json")) + packs:
        h.update(f.read_bytes())
    digest = h.hexdigest()
    rev = int(digest[:8], 16)
    langs = [l for l in pub["langs"] if l["code"] != lang] + [{"code": lang, "count": len(rows), "shards": len(shards), "rev": rev}]
    order = ["it", "en", "de", "fr", "es"]
    langs.sort(key=lambda l: (order.index(l["code"]) if l["code"] in order else 99, -l["count"]))
    save_json(dst / "index-v1.json", {"format": 1, "generated": full["generated"], "pack": PACK, "langs": langs,
                                      "cats": full["cats"]}, compact=True)
    shutil.copy2(OUT / "CREDITS.md", dst / "CREDITS.md")
    rmap = load_json(dst / "release-map.json", {}) or {}
    rmap.update(load_json(OUT / "release-map.json", {}) or {})
    save_json(dst / "release-map.json", rmap, compact=True)
    log("staged %s: %d books in %d shards, %d covers" % (lang, len(rows), len(shards), sum(1 for r in rows if r.get("cv"))))


def commit_push(store, message, log=print):
    def git(*a, check=True):
        r = subprocess.run(["git", "-C", str(store), *a], capture_output=True, text=True, encoding="utf-8")
        if check and r.returncode != 0:
            raise RuntimeError("git %s: %s" % (a[0], (r.stderr or r.stdout).strip()[-400:]))
        return r
    git("add", "books")
    if not git("diff", "--cached", "--quiet", check=False).returncode:
        log("nothing to commit")
        return False
    git("commit", "-q", "-m", message)
    for attempt in range(3):
        git("pull", "-q", "--rebase", "origin", "main")
        if git("push", "-q", "origin", "HEAD:main", check=False).returncode == 0:
            log("pushed: %s" % message)
            return True
        time.sleep(20)
    raise RuntimeError("push failed")


def freeze(rows, log=print):
    """Keep the published file of each id forever: later builds reuse it (u/h/s never change for an id)."""
    FROZEN.mkdir(parents=True, exist_ok=True)
    idx = load_json(FROZEN_INDEX, {}) or {}
    n = 0
    for r in rows:
        if r["id"] in idx:
            continue
        src = OUT / "releases" / _tag(r) / (r["id"] + ".epub")
        dst = FROZEN / (r["id"] + ".epub")
        if not dst.exists():
            try:
                os.link(src, dst)
            except OSError:
                shutil.copy2(src, dst)
        idx[r["id"]] = [r["s"], r["h"]]
        n += 1
    save_json(FROZEN_INDEX, idx, compact=True)
    if n:
        log("  frozen %d published files" % n)


def go_live(store, lang, rows, cache, log):
    ok = verified(rows, cache)
    if not ok:
        return 0
    if not head_check(ok, log):
        log("%s: HEAD check failed - not published" % lang)
        return 0
    freeze(ok, log)
    stage_language(store, lang, ok, log)
    commit_push(store, "books: %s catalog (%d books)" % (lang, len(ok)), log)
    return len(ok)


def publish(store, langs, log=print):
    # first make live what is already on GitHub in every language, then upload the rest in order
    for lang in langs:
        if (OUT / "books" / lang).exists():
            rows = lang_rows(lang)
            tags = sorted({_tag(r) for r in rows})
            cache = {t: release_assets(t) or {} for t in tags}
            if any(cache.values()):
                go_live(store, lang, rows, cache, log)
    for lang in langs:
        if not (OUT / "books" / lang).exists():
            log("%s: not in the build, skipped" % lang)
            continue
        rows = lang_rows(lang)
        cache = {}
        for tag in sorted({_tag(r) for r in rows}):
            cache[tag] = ensure_release(tag, lang)
        todo = [r for r in rows if not _good(cache[_tag(r)].get(r["id"] + ".epub"), r)]
        log("%s: %d rows, %d already uploaded, %d to upload" % (lang, len(rows), len(rows) - len(todo), len(todo)))
        live = go_live(store, lang, rows, cache, log) if len(todo) < len(rows) else 0
        done = 0
        for r in todo:
            t0 = time.time()
            upload(r, log)
            done += 1
            if done % CHECKPOINT == 0 or done == len(todo):
                cache = {}                       # re-read the releases: verification is against GitHub
                live = go_live(store, lang, rows, cache, log)
                log("%s: %d/%d uploaded, %d live" % (lang, done, len(todo), live))
            time.sleep(max(0.0, UPLOAD_EVERY - (time.time() - t0)))
        if not todo:
            live = go_live(store, lang, rows, cache, log)
        log("%s: done, %d of %d rows live" % (lang, live, len(rows)))
