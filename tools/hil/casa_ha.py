#!/usr/bin/env python3
"""HIL check: the Casa app drives a real Home Assistant, and Home Assistant drives the app.

Needs a Home Assistant you can break (the `demo:` integration is ideal: it makes lights, switches,
covers, thermostats, locks, media players and vacuums) and the board paired (tools/pair.py).
The script taps the board's screen, then reads the state back from Home Assistant:

  setup   rooms (Living Room, Kitchen, Bedroom, Garage) on a few demo entities, a name with a '|'
          in it (must not shift the fields of the line), known starting states
  1  a light toggles                       light.bed_light off -> on
  2  the brightness bar sets 25 %          light.ceiling_lights
  3  a switch toggles                      switch.ac
  4  a window cover opens with one tap     cover.kitchen_window
  5  a garage does NOT open with one tap: a confirmation comes up; Cancel keeps it closed,
     the confirm button opens it           cover.garage_door
  6  a lock does NOT unlock with one tap; confirm unlocks; one tap locks again   lock.front_door
  7  the thermostat's + raises the target by 0.5          climate.heatpump
  8  a media player pauses                 media_player.living_room
  9  a vacuum starts                       vacuum.demo_vacuum_0_ground_floor
  10 live: a light switched in Home Assistant changes colour on the panel within 3 s (no tap)
  11 the board did not reboot

  python tools/hil/casa_ha.py --ha http://localhost:8123 --token-file ha.json \\
      --configure --board-ha http://<pc-ip>:8123 [--mqtt] [--cleanup]

--token-file: a long-lived access token, plain text or JSON {"token": ...}; keep it outside the
repo. --configure writes the address and token into the board (Settings > Home); --cleanup
forgets them at the end. Exit code: 0 pass, 1 fail, 2 board or Home Assistant unreachable.
Python 3.8+ with Pillow (screen colours); everything else is the standard library.
"""
import argparse
import base64
import io
import json
import os
import re
import socket
import struct
import sys
import time
import urllib.parse
import urllib.request

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
import nsh  # noqa: E402  (host discovery)
from nvtoken import auth_headers  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
W = 1024
TW, TH, GX, GY, GAP, PER = 236, 128, 19, 148, 14, 12     # apps/casa/main.c layout
AMBER = (255, 186, 73)
PANEL = (30, 34, 46)


# ---- Home Assistant -----------------------------------------------------------------------------
class HA:
    def __init__(self, base, token):
        self.base, self.token = base.rstrip("/"), token

    def req(self, method, path, body=None, timeout=15):
        data = None if body is None else (body if isinstance(body, bytes) else json.dumps(body).encode())
        r = urllib.request.Request(self.base + path, data, method=method,
                                   headers={"Authorization": "Bearer " + self.token, "Content-Type": "application/json"})
        with urllib.request.urlopen(r, timeout=timeout) as resp:
            raw = resp.read().decode("utf-8", "replace")
        try:
            return json.loads(raw)
        except ValueError:
            return raw

    def state(self, eid):
        return self.req("GET", "/api/states/" + eid)

    def call(self, domain, service, **data):
        return self.req("POST", "/api/services/%s/%s" % (domain, service), data)

    def ws(self, msgs, extra=0):
        """Runs messages on the WebSocket API; their results, in order, then `extra` more messages
        (subscription events). Minimal RFC 6455 client."""
        u = urllib.parse.urlparse(self.base)
        s = socket.create_connection((u.hostname, u.port or 80), 15)
        key = base64.b64encode(os.urandom(16)).decode()
        s.sendall(("GET /api/websocket HTTP/1.1\r\nHost: %s\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                   "Sec-WebSocket-Key: %s\r\nSec-WebSocket-Version: 13\r\n\r\n" % (u.netloc, key)).encode())
        buf = b""
        while b"\r\n\r\n" not in buf:
            buf += s.recv(4096)
        rest = buf.split(b"\r\n\r\n", 1)[1]

        def recv_exact(n):
            nonlocal rest
            while len(rest) < n:
                chunk = s.recv(65536)
                if not chunk:
                    raise ConnectionError("websocket closed")
                rest += chunk
            out, rest = rest[:n], rest[n:]
            return out

        def recv():
            payload = b""
            while True:
                b0, b1 = recv_exact(2)
                n = b1 & 0x7F
                if n == 126:
                    n = struct.unpack(">H", recv_exact(2))[0]
                elif n == 127:
                    n = struct.unpack(">Q", recv_exact(8))[0]
                payload += recv_exact(n)
                if b0 & 0x80:
                    return json.loads(payload.decode())

        def send(obj):
            data = json.dumps(obj).encode()
            mask = os.urandom(4)
            hdr = bytes([0x81])
            n = len(data)
            hdr += bytes([0x80 | n]) if n < 126 else bytes([0x80 | 126]) + struct.pack(">H", n) if n < 65536 \
                else bytes([0x80 | 127]) + struct.pack(">Q", n)
            s.sendall(hdr + mask + bytes(c ^ mask[i % 4] for i, c in enumerate(data)))

        recv()                                                  # auth_required
        send({"type": "auth", "access_token": self.token})
        if recv().get("type") != "auth_ok":
            raise PermissionError("Home Assistant refused the token")
        out = []
        for i, m in enumerate(msgs, 1):
            send(dict(m, id=i))
            while True:
                r = recv()
                if r.get("id") == i and r.get("type") == "result":
                    out.append(r)
                    break
        for _ in range(extra):
            out.append(recv())
        s.close()
        return out


def casa_template():
    """The template apps/casa/main.c sends, as the JSON body of POST /api/template."""
    src = open(os.path.join(ROOT, "apps", "casa", "main.c"), encoding="utf-8").read()
    m = re.search(r"#define TPL \\\n((?:[^\n]*\\\n)*[^\n]*)", src)    # the macro's lines
    parts = re.findall(r'"((?:[^"\\]|\\.)*)"', m.group(1))
    c = "".join(parts)
    return c.replace("\\\\", "\x00").replace('\\"', '"').replace("\x00", "\\")


def tile_order(ha):
    """Entity ids in the order the app lays them out (room filter: All)."""
    body = '{"template":"%s"}' % casa_template()
    text = ha.req("POST", "/api/template", body.encode())
    ids = []
    for line in str(text).split("\n"):
        f = line.split("|")
        if len(f) >= 11 and re.fullmatch(r"[a-z0-9_]+\.[a-z0-9_]+", f[0]) and len(ids) < 72:
            ids.append(f[0])
    return ids


# ---- board --------------------------------------------------------------------------------------
class Board:
    def __init__(self, host):
        self.host = host

    def raw(self, method, path, body=None, timeout=10):
        data = None if body is None else json.dumps(body).encode()
        h = dict(auth_headers())
        if data:
            h["Content-Type"] = "application/json"
        r = urllib.request.Request("http://%s%s" % (self.host, path), data, method=method, headers=h)
        with urllib.request.urlopen(r, timeout=timeout) as resp:
            return resp.read()

    def get(self, path):
        return json.loads(self.raw("GET", path).decode("utf-8", "replace") or "{}")

    def tap(self, x, y):
        self.raw("GET", "/api/ui/tap?x=%d&y=%d" % (x, y))
        time.sleep(0.25)

    def screen(self):
        from PIL import Image
        return Image.open(io.BytesIO(self.raw("GET", "/api/screen", timeout=15))).convert("RGB")

    def open_casa(self):
        self.raw("GET", "/api/ui/home")
        time.sleep(1.2)
        self.raw("GET", "/api/ui/open?id=casa")
        time.sleep(6.0)                                     # first render + the live subscription


def tile_xy(pos):
    return GX + (pos % 4) * (TW + GAP), GY + (pos // 4) * (TH + GAP)


def goto(board, order, eid):
    """Opens Casa fresh and pages to eid's tile; its top-left corner."""
    i = order.index(eid)
    board.open_casa()
    for _ in range(i // PER):
        board.tap(W - 24 - 24, 110)                        # the pager's next arrow
        time.sleep(0.4)
    return tile_xy(i % PER)


def wait(ha, eid, pred, timeout=8.0):
    t0 = time.time()
    while time.time() - t0 < timeout:
        st = ha.state(eid)
        if pred(st):
            return st, time.time() - t0
        time.sleep(0.3)
    return ha.state(eid), None


def near(c, ref, tol=40):
    return all(abs(a - b) <= tol for a, b in zip(c, ref))


def mqtt_checks(ha, board, check):
    """The board as a Home Assistant device: its entities exist, and commands from HA reach it."""
    node = board.get("/api/home").get("node", "")
    pre = node.replace("nucleo_", "nucleoos_")
    ents = {s["entity_id"]: s for s in ha.req("GET", "/api/states") if s["entity_id"].split(".", 1)[1].startswith(pre + "_")}
    need = ["light.%s_screen", "number.%s_volume", "switch.%s_mute", "switch.%s_do_not_disturb", "select.%s_app",
            "notify.%s_notification", "sensor.%s_chip_temperature", "binary_sensor.%s_locked"]
    missing = [n % pre for n in need if n % pre not in ents]
    check("mqtt: the board's entities are in Home Assistant", not missing, "%d found%s" % (
        len(ents), ", missing " + " ".join(missing) if missing else ""))
    if missing:
        return
    vol = "number.%s_volume" % pre
    v0 = float(ents[vol]["state"])
    v1 = 40.0 if v0 != 40.0 else 50.0
    ha.call("number", "set_value", entity_id=vol, value=v1)
    st, dt = wait(ha, vol, lambda s: s["state"] not in ("unknown", "unavailable") and float(s["state"]) == v1, 10)
    check("mqtt: volume set from HA comes back from the board", dt is not None, "%s -> %s" % (v0, st["state"]))
    ha.call("number", "set_value", entity_id=vol, value=v0)
    mute = "switch.%s_mute" % pre
    ha.call("switch", "turn_on", entity_id=mute)
    st, dt = wait(ha, mute, lambda s: s["state"] == "on", 10)
    check("mqtt: mute from HA", dt is not None, st["state"])
    ha.call("switch", "turn_off", entity_id=mute)
    wait(ha, mute, lambda s: s["state"] == "off", 10)
    app = "select.%s_app" % pre
    ha.call("select", "select_option", entity_id=app, option="home")
    t0, ok = time.time(), False
    while time.time() - t0 < 8 and not ok:
        ok = board.get("/api/ui/state").get("app", "x") == ""
        time.sleep(0.5)
    check("mqtt: 'App = home' from HA closes the app on the board", ok)
    tag = "casa_ha %d" % int(time.time())
    ha.call("notify", "send_message", entity_id="notify.%s_notification" % pre, message=tag)
    t0, ok = time.time(), False
    while time.time() - t0 < 8 and not ok:                  # nv_notify logs every post: "post [n] <title>: <text>"
        ok = tag in board.raw("GET", "/api/logs").decode("utf-8", "replace")
        time.sleep(0.5)
    check("mqtt: a notification from HA is posted on the board", ok)


# ---- run ----------------------------------------------------------------------------------------
def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--ha", default="http://localhost:8123", help="Home Assistant as seen from this PC")
    ap.add_argument("--token-file", required=True)
    ap.add_argument("--board-ha", help="Home Assistant as seen from the board (for --configure)")
    ap.add_argument("--configure", action="store_true", help="write address + token into the board")
    ap.add_argument("--cleanup", action="store_true", help="forget them on the board at the end")
    ap.add_argument("--host")
    ap.add_argument("--mqtt", action="store_true",
                    help="also the board's own entities (MQTT discovery; board and HA on one broker)")
    a = ap.parse_args()

    raw = open(a.token_file, encoding="utf-8").read().strip()
    token = json.loads(raw)["token"] if raw.startswith("{") else raw
    ha = HA(a.ha, token)
    board = Board(a.host or nsh.pick_host())
    try:
        ha.req("GET", "/api/")
        up0 = board.get("/api/info").get("uptime_s", 0)
    except Exception as e:
        print("unreachable:", e)
        return 2
    if a.configure:
        if not a.board_ha:
            print("--configure needs --board-ha")
            return 2
        board.raw("POST", "/api/home", {"ha_url": a.board_ha, "ha_token": token})

    # setup: rooms, a hostile name, known states
    areas = {r["name"]: r["area_id"] for r in ha.ws([{"type": "config/area_registry/list"}])[0]["result"]}
    want = {"Living Room": ["light.ceiling_lights", "media_player.living_room", "climate.heatpump"],
            "Kitchen": ["light.kitchen_lights", "cover.kitchen_window"],
            "Bedroom": ["light.bed_light", "switch.ac"], "Garage": ["cover.garage_door", "lock.front_door"]}
    msgs = [{"type": "config/area_registry/create", "name": n} for n in want if n not in areas]
    for r in ha.ws(msgs) if msgs else []:
        areas[r["result"]["name"]] = r["result"]["area_id"]
    upd = [{"type": "config/entity_registry/update", "entity_id": e, "area_id": areas[n]} for n, es in want.items() for e in es]
    upd.append({"type": "config/entity_registry/update", "entity_id": "switch.decorative_lights", "name": "Deco | lights"})
    ha.ws(upd)
    ha.call("light", "turn_off", entity_id="light.bed_light")
    ha.call("light", "turn_on", entity_id="light.ceiling_lights", brightness_pct=80)
    ha.call("light", "turn_on", entity_id="light.kitchen_lights")
    ha.call("switch", "turn_off", entity_id="switch.ac")
    ha.call("cover", "close_cover", entity_id="cover.kitchen_window")
    ha.call("cover", "close_cover", entity_id="cover.garage_door")
    ha.call("lock", "lock", entity_id="lock.front_door")
    ha.call("climate", "set_temperature", entity_id="climate.heatpump", temperature=20)
    ha.call("media_player", "media_play", entity_id="media_player.living_room")
    ha.call("vacuum", "return_to_base", entity_id="vacuum.demo_vacuum_0_ground_floor")
    for e, s in (("light.bed_light", "off"), ("light.kitchen_lights", "on"), ("switch.ac", "off"),
                 ("cover.kitchen_window", "closed"), ("cover.garage_door", "closed"), ("lock.front_door", "locked"),
                 ("media_player.living_room", "playing")):
        wait(ha, e, lambda st, s=s: st["state"] == s, 20)
    order = tile_order(ha)
    print("tiles:", len(order))

    results = []

    def check(name, ok, detail=""):
        results.append(ok)
        print("%s  %s%s" % ("PASS" if ok else "FAIL", name, ("  " + detail) if detail else ""))

    # 1 light toggle
    x, y = goto(board, order, "light.bed_light")
    board.tap(x + TW // 2, y + 40)
    st, dt = wait(ha, "light.bed_light", lambda s: s["state"] == "on")
    check("light toggles", dt is not None, "%.1f s" % dt if dt else st["state"])

    # 2 brightness bar at 25 %
    x, y = goto(board, order, "light.ceiling_lights")
    board.tap(x + 16 + (TW - 32) // 4, y + TH - 10)
    st, dt = wait(ha, "light.ceiling_lights", lambda s: abs((s["attributes"].get("brightness") or 0) - 64) <= 13)
    check("brightness bar sets 25 %", dt is not None, "brightness %s" % st["attributes"].get("brightness"))

    # 3 switch
    x, y = goto(board, order, "switch.ac")
    board.tap(x + TW // 2, y + 40)
    st, dt = wait(ha, "switch.ac", lambda s: s["state"] == "on")
    check("switch toggles", dt is not None, st["state"])

    # 4 window cover: one tap opens
    x, y = goto(board, order, "cover.kitchen_window")
    board.tap(x + TW // 2, y + 40)
    st, dt = wait(ha, "cover.kitchen_window", lambda s: s["state"] in ("opening", "open"))
    check("window cover opens with one tap", dt is not None, st["state"])

    # 5 garage: confirmation
    x, y = goto(board, order, "cover.garage_door")
    board.tap(x + TW // 2, y + 40)
    time.sleep(2.0)
    s1 = ha.state("cover.garage_door")["state"]
    board.tap(264 + 118, 340 + 28)                          # Cancel
    time.sleep(1.5)
    s2 = ha.state("cover.garage_door")["state"]
    check("garage: one tap does not open", s1 == "closed" and s2 == "closed", "%s, after Cancel %s" % (s1, s2))
    board.tap(x + TW // 2, y + 40)
    time.sleep(0.8)
    board.tap(524 + 118, 340 + 28)                          # Open
    st, dt = wait(ha, "cover.garage_door", lambda s: s["state"] in ("opening", "open"))
    check("garage: confirm opens", dt is not None, st["state"])

    # 6 lock: confirmation to unlock, one tap to lock
    x, y = goto(board, order, "lock.front_door")
    board.tap(x + TW // 2, y + 40)
    time.sleep(2.0)
    s1 = ha.state("lock.front_door")["state"]
    check("lock: one tap does not unlock", s1 == "locked", s1)
    board.tap(524 + 118, 340 + 28)                          # Unlock
    st, dt = wait(ha, "lock.front_door", lambda s: s["state"] == "unlocked")
    check("lock: confirm unlocks", dt is not None, st["state"])
    time.sleep(1.5)
    board.tap(x + TW // 2, y + 40)
    st, dt = wait(ha, "lock.front_door", lambda s: s["state"] == "locked")
    check("lock: one tap locks", dt is not None, st["state"])

    # 7 thermostat +
    x, y = goto(board, order, "climate.heatpump")
    board.tap(x + TW - 34, y + 104)                         # the + button
    st, dt = wait(ha, "climate.heatpump", lambda s: s["attributes"].get("temperature") == 20.5)
    check("thermostat + raises 0.5", dt is not None, "target %s" % st["attributes"].get("temperature"))

    # 8 media player
    x, y = goto(board, order, "media_player.living_room")
    board.tap(x + TW // 2, y + 40)
    st, dt = wait(ha, "media_player.living_room", lambda s: s["state"] == "paused")
    check("media player pauses", dt is not None, st["state"])

    # 9 vacuum
    x, y = goto(board, order, "vacuum.demo_vacuum_0_ground_floor")
    board.tap(x + TW // 2, y + 40)
    st, dt = wait(ha, "vacuum.demo_vacuum_0_ground_floor", lambda s: s["state"] == "cleaning")
    check("vacuum starts", dt is not None, st["state"])

    # 10 live: Home Assistant -> panel, no tap
    x, y = goto(board, order, "light.kitchen_lights")
    px = (x + TW - 30, y + 8)                               # top padding of the tile: tile colour only
    c0 = board.screen().getpixel(px)
    ha.call("light", "turn_off", entity_id="light.kitchen_lights")
    # A capture takes 1-4 s over Wi-Fi, so one capture 2.5 s after the change: the 5 s HTTP poll
    # could only make it by luck, so the header's "Live" dot (green) must be on too.
    time.sleep(2.5)
    img = board.screen()
    c = img.getpixel(px)
    dot = any(near(img.getpixel((xx, 41)), (76, 204, 146), 50) for xx in range(900, 1000, 2))
    check("live: HA change reaches the panel within 2.5 s", near(c0, AMBER) and near(c, PANEL) and dot,
          "before %s, after %s, live dot %s" % (c0, c, "on" if dot else "off"))

    if a.mqtt:
        mqtt_checks(ha, board, check)

    up1 = board.get("/api/info").get("uptime_s", 0)
    check("no reboot", up1 >= up0, "uptime %s -> %s" % (up0, up1))
    board.raw("GET", "/api/ui/home")
    if a.cleanup:
        board.raw("POST", "/api/home", {"ha_url": None, "ha_token": None})
    print("%d/%d passed" % (sum(results), len(results)))
    return 0 if all(results) else 1


if __name__ == "__main__":
    sys.exit(main())
