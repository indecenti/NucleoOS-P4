#!/usr/bin/env python3
"""Real bridge answers for the DMD menu's host simulator (tests/host/sim_dmd): run on the cabinet PC, it
asks the bridge's own code for the lists, a few tables' cards and their pictures exactly as the board
would get them, and writes them as files (one per answer) into OUT. Copy OUT to tests/host/sim_dmd/data.

    python sim_data.py OUT [--tables 14]
"""
import json
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import nucleo_bridge as nb   # noqa: E402


class Capture(nb.Link):
    """A Link that writes what it would send to the board into files instead of the serial port."""

    def __init__(self, popper, out):
        super().__init__(None, popper)
        self.out = out
        self.name = None

    def send(self, t, payload):
        if isinstance(payload, str):
            payload = payload.encode("utf-8")
        with open(os.path.join(self.out, self.name), "wb") as f:
            f.write(t.encode() + payload)
        return True


def main():
    out = sys.argv[1]
    n = int(sys.argv[sys.argv.index("--tables") + 1]) if "--tables" in sys.argv else 14
    os.makedirs(out, exist_ok=True)
    popper = nb.Popper(nb.find_popper(None), 80)
    link = Capture(popper, out)
    link.name = "hello"
    link.hello()
    lists = {}
    for which in ("recent", "new", "top", "fav", "all"):
        items = popper.tables(which, 1000)
        lists[which] = items
        link.name = "list_%s" % which
        link.send_json("L", {"list": which, "items": items})
    # tables: the first of "recent", plus some of each emulator from "all"
    ids = [it["id"] for it in lists["recent"][:8]]
    seen_emu = {}
    for it in lists["all"]:
        e = popper.emu_of(it["id"])
        if seen_emu.get(e, 0) < 2:
            seen_emu[e] = seen_emu.get(e, 0) + 1
            ids.append(it["id"])
    ids = list(dict.fromkeys(ids))[:n]
    for gid in ids:
        t = popper.table(gid)
        link.name = "table_%d" % gid
        link.send_json("T", t)
        for kind, w, h, alpha in (("wheel", 136, 136, 1), ("wheel", 280, 280, 1), ("backglass", 512, 300, 0),
                                  ("gamebg", 512, 300, 0), ("help", 1024, 1824, 0)):
            link.name = "img_%s_%d_%d" % (kind, gid, w)
            if not popper.media_for(gid, {"wheel": "Wheel", "help": "GameHelp"}.get(kind, "BackGlass")):
                continue
            link.image({"id": gid, "kind": kind, "w": w, "h": h, "bg": "0", "alpha": alpha, "rot": 90 if kind == "help" else 0})
        sc = popper.showcase(gid)
        link.name = "show_%d" % gid
        link.send_json("S", sc)
        for screen in sc["media"]:
            link.name = "img_media_%d_%s" % (gid, screen)
            link.image({"id": gid, "kind": "media", "screen": screen, "w": 1024, "h": 600, "bg": "0", "box": [32, 18, 960, 480]})
    # side wheels of the bar: every table of "recent" and the first ones of "all"
    for it in lists["recent"][:40] + lists["all"][:60] + lists["new"][:20]:
        name = "img_wheel_%d_136" % it["id"]
        if not os.path.exists(os.path.join(out, name)) and popper.media_for(it["id"], "Wheel"):
            link.name = name
            link.image({"id": it["id"], "kind": "wheel", "w": 136, "h": 136, "bg": "0", "alpha": 1})
    print("tables:", ids)
    print("files:", len(os.listdir(out)))


if __name__ == "__main__":
    nb.log = lambda *a: None
    main()
