"""F5 publishing, one language at a time (only with the user's OK).

For each language: create the missing GitHub Releases books-<lang>-<n> (never marked Latest, so the
firmware channels are untouched), upload the EPUBs that are not there yet, verify every asset's name
and size (and sha256 when GitHub reports a digest), and only then put that language's shards, covers,
the index, CREDITS.md and release-map.json into books/ of a sparse clone of the store repo and push it.
The app therefore never sees a row whose file is not downloadable yet.
"""
import json
import shutil
import subprocess
from pathlib import Path

from .common import OUT, load_json, save_json

REPO = "indecenti/nucleoos-p4-store"
NOTES = {
    "it": "Libri liberi (pubblico dominio e licenze libere) per l'app Libri di NucleoOS. Catalogo, licenze e crediti: "
          "https://indecenti.github.io/nucleoos-p4-store/books/",
}
NOTES_DEFAULT = ("Free books (public domain and free licences) for the NucleoOS Books app, language: {lang}. "
                 "Catalog, licences and credits: https://indecenti.github.io/nucleoos-p4-store/books/")


def gh(*args, check=True):
    r = subprocess.run(["gh", *args], capture_output=True, text=True, encoding="utf-8")
    if check and r.returncode != 0:
        raise RuntimeError("gh %s: %s" % (" ".join(args[:3]), (r.stderr or r.stdout).strip()[-400:]))
    return r


def release_assets(tag):
    r = gh("api", "repos/%s/releases/tags/%s" % (REPO, tag), check=False)
    if r.returncode != 0:
        return None
    js = json.loads(r.stdout)
    out = {}
    page = 1
    while True:
        a = json.loads(gh("api", "repos/%s/releases/%d/assets?per_page=100&page=%d" % (REPO, js["id"], page)).stdout)
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


def upload_language(lang, log=print, batch=20):
    """Upload every EPUB of `lang` that is missing or different; returns (rows, ok)."""
    rows = []
    for f in sorted((OUT / "books" / lang).glob("*.json"), key=lambda p: int(p.stem)):
        rows += load_json(f)["books"]
    by_tag = {}
    for r in rows:
        tag = r["u"].split("/download/")[1].split("/")[0]
        by_tag.setdefault(tag, []).append(r)
    ok = True
    for tag, rs in sorted(by_tag.items()):
        have = ensure_release(tag, lang)
        todo = []
        for r in rs:
            a = have.get(r["id"] + ".epub")
            if not a or a["size"] != r["s"] or a.get("state") not in (None, "uploaded") or \
                    (a.get("digest") and a["digest"] != "sha256:" + r["h"]):
                todo.append(r)
        log("%s: %d assets present, %d to upload" % (tag, len(rs) - len(todo), len(todo)))
        rel = OUT / "releases" / tag
        for i in range(0, len(todo), batch):
            files = [str(rel / (r["id"] + ".epub")) for r in todo[i:i + batch]]
            r = gh("release", "upload", tag, "--repo", REPO, "--clobber", *files, check=False)
            if r.returncode != 0:
                log("  upload error: %s" % (r.stderr or "").strip()[-300:])
            if (i // batch) % 10 == 0:
                log("  %s %d/%d" % (tag, min(i + batch, len(todo)), len(todo)))
        have = release_assets(tag) or {}
        for r in rs:
            a = have.get(r["id"] + ".epub")
            if not a or a["size"] != r["s"] or (a.get("digest") and a["digest"] != "sha256:" + r["h"]):
                log("  NOT OK: %s %s" % (tag, r["id"]))
                ok = False
        ok = head_check(rs, log) and ok
    return rows, ok


def head_check(rows, log=print, n=5):
    """HEAD a few public download URLs exactly as the app will use them: 200 after redirects, right size."""
    import random
    import requests
    good = True
    for r in random.Random(0).sample(rows, min(n, len(rows))):
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


def stage_language(store, lang, rows, log=print):
    """Copy a verified language into <store>/books and update the published index."""
    store = Path(store)
    dst = store / "books"
    ldir = dst / lang
    if ldir.exists():
        shutil.rmtree(ldir)
    ldir.mkdir(parents=True)
    for f in (OUT / "books" / lang).glob("*.json"):
        shutil.copy2(f, ldir / f.name)
    cdir = dst / "covers"
    cdir.mkdir(exist_ok=True)
    for r in rows:
        if r.get("cv"):
            shutil.copy2(OUT / "books" / "covers" / (r["id"] + ".jpg"), cdir / (r["id"] + ".jpg"))
    full = load_json(OUT / "books" / "index-v1.json")
    pub = load_json(dst / "index-v1.json", None) or {"format": 1, "langs": []}
    entry = next(l for l in full["langs"] if l["code"] == lang)
    langs = [l for l in pub["langs"] if l["code"] != lang] + [entry]
    order = [l["code"] for l in full["langs"]]
    langs.sort(key=lambda l: order.index(l["code"]) if l["code"] in order else 999)
    save_json(dst / "index-v1.json", {"format": 1, "generated": full["generated"], "langs": langs,
                                      "cats": full["cats"]}, compact=True)
    shutil.copy2(OUT / "CREDITS.md", dst / "CREDITS.md")
    shutil.copy2(OUT / "release-map.json", dst / "release-map.json")
    log("staged %s: %d books, %d covers" % (lang, len(rows), sum(1 for r in rows if r.get("cv"))))


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
    git("pull", "-q", "--rebase", "origin", "main")
    git("push", "-q", "origin", "HEAD:main")
    log("pushed: %s" % message)
    return True


def publish(store, langs, log=print):
    for lang in langs:
        if not (OUT / "books" / lang).exists():
            log("%s: not in the build, skipped" % lang)
            continue
        rows, ok = upload_language(lang, log)
        if not ok:
            log("%s: some assets failed verification - index NOT published" % lang)
            continue
        stage_language(store, lang, rows, log)
        commit_push(store, "books: %s catalog (%d books)" % (lang, len(rows)), log)
