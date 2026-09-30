"""Minimal fake Home Assistant for testing apps/casa and the ABI v12 ha_* imports. Stdlib only.

    python tools/ha_mock.py [--port 8123] [--token mock-token]

Implements what the Casa app uses: GET /api/, POST /api/template (ignores the template text and
renders the fixed demo entities below in the app's line format), POST /api/services/<domain>/<svc>
(updates the demo state). Every call must carry "Authorization: Bearer <token>" — like the real one.
Point the panel at it: POST /api/home {"ha_url":"http://<pc-ip>:8123","ha_token":"mock-token"}.
"""
import argparse
import json
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

ENTS = [  # domain, entity_id, state, name, area, brightness, unit, target, current
    ["light", "light.soggiorno", "on", "Luce soggiorno", "Soggiorno", 180, "", "", ""],
    ["light", "light.cucina", "off", "Luce cucina", "Cucina", 0, "", "", ""],
    ["switch", "switch.macchina_caffe", "off", "Macchina caffè", "Cucina", 0, "", "", ""],
    ["cover", "cover.tapparella_sala", "open", "Tapparella sala", "Soggiorno", 0, "", "", ""],
    ["climate", "climate.termostato", "heat", "Termostato", "Soggiorno", 0, "", "21.5", "20.8"],
    ["sensor", "sensor.temp_camera", "19.4", "Temperatura camera", "Camera", 0, "°C", "", ""],
    ["sensor", "sensor.consumo", "412", "Consumo casa", "", 0, "W", "", ""],
    ["binary_sensor", "binary_sensor.porta", "off", "Porta ingresso", "Ingresso", 0, "", "", ""],
    ["lock", "lock.portone", "locked", "Portone", "Ingresso", 0, "", "", ""],
    ["scene", "scene.cinema", "scening", "Scena cinema", "Soggiorno", 0, "", "", ""],
    ["light", "light.camera", "off", "Abat-jour camera", "Camera", 0, "", "", ""],
    ["fan", "fan.ventilatore", "off", "Ventilatore", "Camera", 0, "", "", ""],
    ["input_boolean", "input_boolean.ospiti", "off", "Modalità ospiti", "", 0, "", "", ""],
]


class H(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def _send(self, code, body, ctype="application/json"):
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def _auth(self):
        if self.headers.get("Authorization") != "Bearer " + TOKEN:
            self._send(401, b'{"message":"401: Unauthorized"}')
            return False
        return True

    def do_GET(self):
        if not self._auth():
            return
        if self.path == "/api/":
            return self._send(200, b'{"message":"API running."}')
        self._send(404, b"{}")

    def do_POST(self):
        n = int(self.headers.get("Content-Length", "0") or 0)
        body = self.rfile.read(n)
        if not self._auth():
            return
        if self.path == "/api/template":
            txt = "".join("|".join(str(f) for f in e) + "\n" for e in ENTS)
            return self._send(200, txt.encode("utf-8"), "text/plain; charset=utf-8")
        if self.path.startswith("/api/services/"):
            _, _, _, dom, svc = self.path.split("/")[:5]
            data = json.loads(body or b"{}")
            eid = data.get("entity_id")
            print("  service %s.%s %s" % (dom, svc, json.dumps(data)))
            for e in ENTS:
                if e[1] != eid:
                    continue
                if svc == "toggle":
                    e[2] = {"on": "off", "off": "on", "open": "closed", "closed": "open"}.get(e[2], e[2])
                elif svc == "turn_on":
                    e[2] = "on" if dom != "scene" else e[2]
                    if "brightness_pct" in data:
                        e[5] = int(data["brightness_pct"] * 255 / 100)
                    elif dom == "light" and not e[5]:
                        e[5] = 255
                elif svc == "turn_off":
                    e[2] = "off"
                elif svc in ("open_cover", "close_cover"):
                    e[2] = "open" if svc == "open_cover" else "closed"
                elif svc in ("lock", "unlock"):
                    e[2] = "locked" if svc == "lock" else "unlocked"
                elif svc == "set_temperature":
                    e[7] = str(data.get("temperature"))
            return self._send(200, b"[]")
        self._send(404, b"{}")


def main():
    global TOKEN
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=8123)
    ap.add_argument("--token", default="mock-token")
    a = ap.parse_args()
    TOKEN = a.token
    print("fake Home Assistant on :%d (token %s)" % (a.port, TOKEN))
    ThreadingHTTPServer(("0.0.0.0", a.port), H).serve_forever()


if __name__ == "__main__":
    main()
