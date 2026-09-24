#!/usr/bin/env python3
"""Mock NucleoOS board for testing the NucleoCast sender page / helper without hardware.

Serves components/nv_secondscreen/web/cast.html (placeholders filled like the firmware does) and
the /cast WebSocket with the same protocol as ss_cast.cpp: hello -> hi -> ready (or pending when
--ask), binary JPEG tiles composited into a 1024x600 canvas, ack after each frame's last tile.
The canvas is saved to --out after every frame so tests can compare it with the source.

  python mock_board.py --port 7070 --out mock_screen.png
"""
import argparse
import asyncio
import io
import json
import os
import struct
import time

from PIL import Image
import websockets
from websockets.asyncio.server import serve
from websockets.http11 import Response
from websockets.datastructures import Headers

HERE = os.path.dirname(os.path.abspath(__file__))
PAGE = os.path.join(HERE, "..", "..", "components", "nv_secondscreen", "web", "cast.html")
W, H = 1024, 600


def page(host, mode):
    s = open(PAGE, encoding="utf-8").read()
    return (s.replace("{{HOST}}", host).replace("{{TLSHOST}}", host.split(":")[0] + ":7443")
             .replace("{{FW}}", "mock").replace("{{MODE}}", mode))


class Board:
    def __init__(self, a):
        self.a = a
        self.canvas = Image.new("RGB", (W, H))
        self.stats = {"frames": 0, "tiles": 0, "bytes": 0, "t0": time.time()}

    async def handler(self, ws):
        await ws.send(json.dumps({"t": "hello", "v": 1, "w": W, "h": H, "name": "MockBoard", "fw": "mock",
                                  "max": 524288, "ask": self.a.ask}))
        approved = False
        async for msg in ws:
            if isinstance(msg, str):
                m = json.loads(msg)
                if m.get("t") == "hi":
                    print("hi:", m)
                    if self.a.ask and not m.get("token"):
                        await ws.send(json.dumps({"t": "state", "s": "pending"}))
                        await asyncio.sleep(1.5)   # "user taps Allow always"
                        await ws.send(json.dumps({"t": "trust", "token": "0123456789abcdef"}))
                    approved = True
                    await ws.send(json.dumps({"t": "state", "s": "ready"}))
                continue
            if not approved:
                continue
            hdr, jpg = msg[:16], msg[16:]
            mg0, mg1, typ, flags, x, y, w, h, seq = struct.unpack("<BBBBHHHHI", hdr)
            assert (mg0, mg1, typ) == (78, 67, 1), hdr
            tile = Image.open(io.BytesIO(jpg))
            assert tile.format == "JPEG"
            info = tile.info
            if tile.mode != "RGB":
                tile = tile.convert("RGB")
            if w and h and (w, h) != tile.size:
                tile = tile.resize((w, h))
            self.canvas.paste(tile, (x, y))
            self.stats["tiles"] += 1
            self.stats["bytes"] += len(msg)
            if flags & 1:
                self.stats["frames"] += 1
                if not await self.progressive_check(jpg):
                    print("WARNING: progressive JPEG (the board's hardware decoder needs baseline)")
                await ws.send(json.dumps({"t": "ack", "seq": seq, "shown": True, "ms": 5.0}))
                self.canvas.save(self.a.out)
                if self.stats["frames"] % 10 == 1:
                    dt = time.time() - self.stats["t0"]
                    print(f"frame {self.stats['frames']} tiles={self.stats['tiles']} "
                          f"{self.stats['bytes'] / max(dt, 0.001) / 1024:.0f} kB/s last tile {w or tile.size[0]}x{h or tile.size[1]}@{x},{y}")
                if self.a.touch_demo and self.stats["frames"] == 3:
                    await ws.send(json.dumps({"t": "touch", "p": [[1, 512, 300]]}))
                    await ws.send(json.dumps({"t": "touch", "p": []}))

    @staticmethod
    async def progressive_check(jpg):
        i = 2
        while i + 4 <= len(jpg):
            if jpg[i] != 0xFF:
                return True
            marker = jpg[i + 1]
            if marker == 0xC2:
                return False
            if marker in (0xC0, 0xC1, 0xDA):
                return True
            seglen = struct.unpack(">H", jpg[i + 2:i + 4])[0]
            i += 2 + seglen
        return True


async def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=7070)
    ap.add_argument("--out", default="mock_screen.png")
    ap.add_argument("--ask", action="store_true", help="simulate the approval prompt")
    ap.add_argument("--touch-demo", action="store_true", help="send a tap after frame 3")
    a = ap.parse_args()
    b = Board(a)

    async def process_request(conn, req):
        if req.path.startswith("/cast"):
            return None   # WebSocket
        body = page(f"127.0.0.1:{a.port}", "http").encode() if req.path in ("/", "/index.html") else b"not found"
        code = 200 if req.path in ("/", "/index.html") else 404
        return Response(code, "OK", Headers({"Content-Type": "text/html; charset=utf-8",
                                             "Content-Length": str(len(body))}), body)

    async with serve(b.handler, "0.0.0.0", a.port, process_request=process_request, max_size=2 ** 22):
        print(f"mock board on http://127.0.0.1:{a.port}/", flush=True)
        await asyncio.Future()


if __name__ == "__main__":
    asyncio.run(main())
