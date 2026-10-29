#!/usr/bin/env python3
"""The P4's web OS comes from nucleo-webos, pinned in webos.lock.

Since the switch to the common base (phase 4) the web OS files in this repository are NOT edited by hand:
the single source is the nucleo-webos repository (shell, apps, translations, the /api contract), which
builds them for the "p4" profile with tools/build.py (sd/web is the device's /sdcard/web: the shell at its
root, an app's www/ files flat in apps/<id>/). What stays this edition's own: the firmware, the native apps,
tools/, docs/, and the files in sd/web that nucleo-webos does not have (apps.json, the list nv_web serves as
/api/apps; wallpaper.png; i18n/coverage.json...).

    python tools/webos-sync.py --check [--strict]          webos.lock against the tracked files (CI: --strict
                                                           refuses a lock built from uncommitted changes)
    python tools/webos-sync.py update [--webos DIR]        rebuilds from nucleo-webos and rewrites the lock
                                      [--allow-dirty]      (trials only: a build from uncommitted changes)

What nucleo-webos manages here: every file of sd/web/ that its p4 build produces.

Kept by this edition (never removed, listed in the lock as "kept"): files in sd/web/ that nucleo-webos does
not have at all, or that the profile excludes. They are the P4's, as before the switch.

`update` wants nucleo-webos clean: the lock names a commit, and a commit that does not hold those files can
never be found again. Large third-party files outside git (external-resources.json) are fetched by build.py
from the sibling repositories. The .gz twins come with the build (profile "p4": nv_web serves <file>.gz first
from its PSRAM cache, tools/ci/check_web_gz.py); the lock lists sources only.
"""
import fnmatch, hashlib, json, os, re, shutil, subprocess, sys, tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
LOCK = os.path.join(ROOT, "webos.lock")
PROFILE = "p4"
WEB = "sd/web/"
CORE_APP = re.compile(r"^apps/([^/]+)/(manifest\.json|www/(.+))$")


def git(*args, cwd=ROOT):
    r = subprocess.run(["git", "-C", cwd, *args], capture_output=True)
    if r.returncode:
        raise SystemExit("git %s: %s" % (" ".join(args), r.stderr.decode("utf-8", "replace").strip()))
    return r.stdout.decode("utf-8", "replace")


def digest(path):
    with open(path, "rb") as f:
        data = f.read()
    # a Windows working copy has CRLF: git (eol=lf) and the device have LF
    # text as git's text=auto decides it (no NUL), not by extension: .lua, LICENSE, SHA256SUMS check out CRLF too
    if b"\0" not in data[:8000]:
        data = data.replace(b"\r\n", b"\n")
    return hashlib.sha256(data).hexdigest()


def managed(rel):
    return rel.startswith(WEB)


def to_repo(build_rel):
    return WEB + build_rel


def tracked(gz=False):
    """{repository path: full path} of the tracked managed files (sources, or the .gz twins with gz=True)."""
    out = git("ls-files", "-z", "--", WEB.rstrip("/"))
    files = {}
    for p in out.split("\0"):
        if p and managed(p[:-3] if p.endswith(".gz") else p) and p.endswith(".gz") == gz \
                and os.path.isfile(os.path.join(ROOT, p)):
            files[p] = os.path.join(ROOT, p)
    return files


def excluded_by_profile(webos):
    """The profile's exclude globs, as repository paths (fnmatch, ** = any depth)."""
    with open(os.path.join(webos, "profiles", PROFILE + ".json"), encoding="utf-8") as f:
        prof = json.load(f)
    pats = []
    for e in prof.get("exclude") or []:
        g = e["glob"]
        # {a,b} alternatives become one pattern each
        m = re.search(r"\{([^}]*)\}", g)
        for alt in (m.group(1).split(",") if m else [None]):
            pats.append(to_repo(g if alt is None else g[:m.start()] + alt + g[m.end():]) or g)
    return lambda rel: any(fnmatch.fnmatch(rel, p.replace("**/", "*").replace("/**", "/*")) for p in pats)


def core_files(webos):
    """Where everything nucleo-webos tracks under shell/ and apps/<id>/{manifest.json,www/} lands in sd/web/."""
    out = git("ls-files", "-z", "--", "shell", "apps", cwd=webos)
    paths = set()
    for p in out.split(chr(0)):
        m = CORE_APP.match(p)
        if p.startswith("shell/"):
            paths.add(WEB + p[len("shell/"):])
        elif m:
            paths.add(WEB + "apps/%s/%s" % (m.group(1), m.group(3) or "manifest.json"))
    return paths


def check(strict=False):
    if not os.path.isfile(LOCK):
        raise SystemExit("webos.lock missing: python tools/webos-sync.py update")
    with open(LOCK, encoding="utf-8") as f:
        lock = json.load(f)
    want, have = lock["files"], tracked()
    for p in lock.get("kept") or []:
        have.pop(p, None)
    problems = []
    for p in sorted(set(want) - set(have)):
        problems.append("missing: " + p)
    for p in sorted(set(have) - set(want)):
        problems.append("not from nucleo-webos: " + p)
    for p in sorted(set(want) & set(have)):
        if digest(have[p]) != want[p]:
            problems.append("edited by hand: " + p)
    if problems:
        print("webos.lock: this is not the web OS of nucleo-webos %s" % lock["commit"][:10], *problems[:40], sep="\n  ")
        if len(problems) > 40:
            print("  ... and %d more" % (len(problems) - 40))
        print("Web OS fixes go in nucleo-webos, then: python tools/webos-sync.py update")
        return 1
    if lock.get("uncommitted_files") and strict:
        print("webos.lock: built from %d uncommitted files of nucleo-webos: nobody could rebuild it."
              % lock["uncommitted_files"])
        print("Commit in nucleo-webos, then: python tools/webos-sync.py update")
        return 1
    dirty = " (FROM UNCOMMITTED CHANGES: update again before committing)" if lock.get("uncommitted_files") else ""
    print("webos.lock: ok, %d files from nucleo-webos %s%s" % (len(want), lock["commit"][:10], dirty))
    return 0


def listed_apps(versions):
    """sd/web/apps.json is the app list nv_web serves as /api/apps (no versions in it): compare it with the build."""
    with open(os.path.join(ROOT, "sd", "web", "apps.json"), encoding="utf-8") as f:
        d = json.load(f)
    listed = {a.get("id") for a in (d.get("apps") if isinstance(d, dict) else d) or []}
    return sorted(set(versions) - listed), sorted(listed - set(versions))


def update(webos, allow_dirty):
    webos = os.path.abspath(webos)
    build_py = os.path.join(webos, "tools", "build.py")
    if not os.path.isfile(build_py):
        raise SystemExit("cannot find %s (--webos DIR)" % build_py)
    tmp = tempfile.mkdtemp(prefix="webos-sync-")
    out = os.path.join(tmp, "build")
    try:
        r = subprocess.run([sys.executable, build_py, "--profile", PROFILE, "--out", out,
                            "--externals", os.path.dirname(webos)])
        if r.returncode:
            raise SystemExit("nucleo-webos build failed")
        with open(os.path.join(out, "build.json"), encoding="utf-8") as f:
            info = json.load(f)
        if info.get("uncommitted_files") and not allow_dirty:
            raise SystemExit("nucleo-webos has %d uncommitted files: commit there first (or --allow-dirty for a trial)"
                             % info["uncommitted_files"])
        built, twins = {}, {}
        for d, _, names in os.walk(out):
            for n in names:
                full = os.path.join(d, n)
                repo = to_repo(os.path.relpath(full, out).replace(os.sep, "/"))
                if repo is None:
                    continue
                (twins if repo.endswith(".gz") else built)[repo] = full
        # out: only what nucleo-webos has and its build no longer ships to this edition; the rest stays ours
        excluded, core = excluded_by_profile(webos), core_files(webos)
        kept = sorted(p for p in set(tracked()) - set(built) if excluded(p) or p not in core)
        removed = sorted(set(tracked()) - set(built) - set(kept))
        for p in removed:
            os.remove(os.path.join(ROOT, p))
        for rel, full in list(built.items()) + list(twins.items()):
            dst = os.path.join(ROOT, rel)
            os.makedirs(os.path.dirname(dst), exist_ok=True)
            shutil.copy2(full, dst)
        orphans = []
        for top in [WEB.rstrip("/")]:
            for d, _, names in os.walk(os.path.join(ROOT, top)):
                for n in names:
                    full = os.path.join(d, n)
                    rel = os.path.relpath(full, ROOT).replace(os.sep, "/")
                    if n.endswith(".gz") and managed(rel[:-3]) and rel not in twins and rel[:-3] not in kept:
                        os.remove(full)
                        orphans.append(rel)
        missing, extra = listed_apps({a["id"]: a["version"] for a in info["apps"]})
        files = {rel: digest(os.path.join(ROOT, rel)) for rel in sorted(built)}
        lock = {
            "_doc": "Written by tools/webos-sync.py: what the nucleo-webos p4 build produces in sd/web/ comes from there. Do not edit by hand.",
            "format": 1,
            "source": "nucleo-webos",
            "commit": info["commit"],
            "uncommitted_files": info.get("uncommitted_files", 0),
            "profile": PROFILE,
            "profile_sha256": info.get("profile_sha256"),
            "build_hash": info["hash"],
            "webos": info.get("webos"),   # { webos, core, contract }: nucleo-webos docs/versions.md
            "shell_cache": info["shell"]["cache"],
            "apps": {a["id"]: a["version"] for a in info["apps"]},
            "files": files,
            "kept": kept,
        }
        with open(LOCK, "w", encoding="utf-8", newline="\n") as f:
            f.write(json.dumps(lock, indent=1, sort_keys=True, ensure_ascii=False) + "\n")
        print("webos.lock: %d files from nucleo-webos %s%s; removed %d sources and %d .gz twins"
              % (len(files), info["commit"][:10], " (UNCOMMITTED)" if lock["uncommitted_files"] else "",
                 len(removed), len(orphans)))
        for p in removed:
            print("  removed: " + p)
        print("  kept as this edition's own (profile excludes or not in nucleo-webos): %d files" % len(kept))
        if missing:
            print("  built but not in sd/web/apps.json (not shown on the device until listed): " + ", ".join(missing))
        if extra:
            print("  in sd/web/apps.json but not built for this edition: " + ", ".join(extra))
        print("Then: git add -A sd/web webos.lock")
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


def main():
    args = sys.argv[1:]
    if "--check" in args:
        return check("--strict" in args)
    if args and args[0] == "update":
        webos = args[args.index("--webos") + 1] if "--webos" in args else os.path.join(os.path.dirname(ROOT), "nucleo-webos")
        update(webos, "--allow-dirty" in args)
        return 0
    print(__doc__)
    return 2


if __name__ == "__main__":
    sys.exit(main())
