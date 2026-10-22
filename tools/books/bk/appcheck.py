"""Gate every EPUB through the reader app's own C engine (ports/books/test/book_check.c).

`bk_check -q FILE` opens the book and lays out every chapter: exit 0 = "OK"/"WARN", exit 1 = "FAIL".
Built in WSL from the app's committed sources (see README); runs in parallel, results cached by
path+size+mtime in .cache/appcheck.json. A book that FAILs, crashes (sanitizer report) or was never
checked does not enter the catalog. Very large books that time out under the sanitizers are retried
with an optimised build without them.
"""
import os
import subprocess

from .common import CACHE, load_json, save_json
from .src_pg import _wsl_path

CACHE_FILE = CACHE / "appcheck.json"
CHECK = os.environ.get("NV_BOOKS_BK_CHECK", "/tmp/bkcat_check")
CHECK_FAST = os.environ.get("NV_BOOKS_BK_CHECK_FAST", "/tmp/bkcat_check_fast")
TIMEOUT = 240
TIMEOUT_FAST = 900

RUNNER = r'''
f="$1"
out=$(UBSAN_OPTIONS=halt_on_error=1 timeout %d %s -q "$f" 2>&1); rc=$?
if [ $rc -eq 124 ] && [ -x %s ]; then out=$(timeout %d %s -q "$f" 2>&1); rc=$?; out="(fast) $out"; fi
if echo "$out" | grep -qE "ERROR: AddressSanitizer|runtime error"; then rc=99; fi
last=$(echo "$out" | grep -E "^(OK|WARN|FAIL)" | tail -1 | tr '\t\n' '  ' | cut -c1-300)
[ -z "$last" ] && last=$(echo "$out" | tail -1 | tr '\t\n' '  ' | cut -c1-300)
printf 'R\t%%d\t%%s\t%%s\n' "$rc" "$f" "$last"
''' % (TIMEOUT, CHECK, CHECK_FAST, TIMEOUT_FAST, CHECK_FAST)


def _key(path):
    st = os.stat(path)
    return "%s|%d|%d" % (path, st.st_size, int(st.st_mtime))


def run(paths, workers=8, log=print):
    cache = load_json(CACHE_FILE, {}) or {}
    todo = [p for p in paths if os.path.exists(p) and _key(p) not in cache]
    if not todo:
        return cache
    log("appcheck: %d files with %d workers" % (len(todo), workers))
    wsl_of = {_wsl_path(p): p for p in todo}
    lst = CACHE / "appcheck-list.txt"
    with open(lst, "w", newline="\n", encoding="utf-8") as f:
        for w in wsl_of:
            f.write(w + "\n")
    runner = CACHE / "appcheck-run.sh"
    with open(runner, "w", newline="\n", encoding="utf-8") as f:
        f.write(RUNNER)
    cmd = ["wsl", "-e", "bash", "-c", "xargs -d '\\n' -P %d -n 1 bash %s < %s"
           % (workers, _wsl_path(runner), _wsl_path(lst))]
    p = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True, encoding="utf-8",
                         errors="replace", env=dict(os.environ, MSYS_NO_PATHCONV="1"))
    n = 0
    for line in p.stdout:
        parts = line.rstrip("\n").split("\t")
        if len(parts) < 4 or parts[0] != "R":
            continue
        rc, wpath, last = int(parts[1]), parts[2], parts[3]
        win = wsl_of.get(wpath)
        if not win:
            continue
        status = "ok" if rc == 0 else ("crash" if rc == 99 else ("timeout" if rc == 124 else "fail"))
        cache[_key(win)] = {"status": status, "line": last}
        n += 1
        if n % 200 == 0:
            save_json(CACHE_FILE, cache, compact=True)
            log("  appcheck %d/%d" % (n, len(todo)))
    p.wait()
    save_json(CACHE_FILE, cache, compact=True)
    return cache


def result(path, cache):
    try:
        return cache.get(_key(path))
    except OSError:
        return None
