# os.path for NucleoOS: POSIX semantics as in CPython's posixpath (the common functions), written
# for MicroPython; replaces micropython-lib's minimal os-path (MIT). Frozen by ../manifest.py.
import os

sep = "/"
curdir = "."
pardir = ".."
extsep = "."


def _s(p):
    return p.decode() if isinstance(p, bytes) else p


def normcase(s):
    return s


def isabs(s):
    return _s(s).startswith("/")


def join(a, *p):
    path = a
    for b in p:
        if b.startswith("/"):
            path = b
        elif not path or path.endswith("/"):
            path += b
        else:
            path += "/" + b
    return path


def normpath(path):
    if not path:
        return "."
    initial = path.startswith("/")
    comps = []
    for c in path.split("/"):
        if c in ("", "."):
            continue
        if c != ".." or (not initial and not comps) or (comps and comps[-1] == ".."):
            comps.append(c)
        elif comps:
            comps.pop()
    path = "/".join(comps)
    if initial:
        path = "/" + path
    return path or "."


def abspath(s):
    return normpath(s if isabs(s) else join(os.getcwd(), s))


realpath = abspath


def split(path):
    i = path.rfind("/") + 1
    head, tail = path[:i], path[i:]
    if head and head != "/" * len(head):
        head = head.rstrip("/")
    return head, tail


def dirname(path):
    return split(path)[0]


def basename(path):
    return split(path)[1]


def splitext(path):
    sep_i = path.rfind("/")
    dot_i = path.rfind(".")
    if dot_i > sep_i:
        f = sep_i + 1
        while f < dot_i:
            if path[f] != ".":
                return path[:dot_i], path[dot_i:]
            f += 1
    return path, ""


def splitdrive(p):
    return p[:0], p


def relpath(path, start="."):
    a = [x for x in abspath(start).split("/") if x]
    b = [x for x in abspath(path).split("/") if x]
    n = len(commonprefix([a, b]))
    rest = [pardir] * (len(a) - n) + b[n:]
    return join(*rest) if rest else curdir


def commonprefix(m):
    if not m:
        return ""
    s1, s2 = min(m), max(m)
    for i, c in enumerate(s1):
        if c != s2[i]:
            return s1[:i]
    return s1


def exists(path):
    try:
        os.stat(path)
        return True
    except OSError:
        return False


lexists = exists


def isdir(path):
    try:
        return bool(os.stat(path)[0] & 0x4000)
    except OSError:
        return False


def isfile(path):
    try:
        return bool(os.stat(path)[0] & 0x8000)
    except OSError:
        return False


def islink(path):
    return False


def getsize(path):
    return os.stat(path)[6]


def getmtime(path):
    return os.stat(path)[8]


def getatime(path):
    return os.stat(path)[7]


def expanduser(s):
    # the home folder is the root the Terminal gives programs (/sdcard/home)
    if s == "~" or s.startswith("~/"):
        return "/" + s[2:]
    return s


def expandvars(s):
    return s
