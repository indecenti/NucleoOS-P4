# os for NucleoOS: the built-in module plus os.path and os.walk (micropython-lib's os package,
# MIT, extended). Frozen into app.wasm by ../manifest.py.
from uos import *

from . import path

sep = "/"
curdir = "."
pardir = ".."
linesep = "\n"


def walk(top, topdown=True):
    """CPython's os.walk: yields (dirpath, dirnames, filenames)."""
    dirs, files = [], []
    try:
        entries = list(ilistdir(top))
    except OSError:
        return
    for e in entries:
        (dirs if e[1] & 0x4000 else files).append(e[0])
    if topdown:
        yield top, dirs, files
    for d in dirs:
        yield from walk(path.join(top, d), topdown)
    if not topdown:
        yield top, dirs, files


def makedirs(name, mode=0o777, exist_ok=False):
    head = ""
    for part in name.split("/"):
        head = head + "/" + part if head or name.startswith("/") else part
        if not part or path.isdir(head):
            continue
        mkdir(head)
    if not exist_ok and not head:
        raise OSError(17)


def getcwdb():
    return getcwd().encode()
