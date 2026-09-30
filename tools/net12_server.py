"""Test server for the ABI v12 network imports (apps/net12). Standard library only.

    python tools/net12_server.py [--port 8766]

  GET  /ping      -> "pong"
  POST /echo      -> JSON {"body": <request body>, "x_test": <X-Test header>}
  GET  /redirect  -> 302 to http://1.1.1.1/ (the device must NOT follow it)
  GET  /ws        -> WebSocket echo (text and binary)

Then write "<this PC's LAN IP>:<port>" into /sdcard/apps/net12/server and run the app.
"""
import argparse
import base64
import hashlib
import json
import socketserver
import struct
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

WS_GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"


def ws_recv(rfile):
    head = rfile.read(2)
    if len(head) < 2:
        return None, None
    op, ln = head[0] & 0x0F, head[1] & 0x7F
    masked = head[1] & 0x80
    if ln == 126:
        ln = struct.unpack(">H", rfile.read(2))[0]
    elif ln == 127:
        ln = struct.unpack(">Q", rfile.read(8))[0]
    mask = rfile.read(4) if masked else b"\0\0\0\0"
    data = bytearray(rfile.read(ln))
    for i in range(len(data)):
        data[i] ^= mask[i % 4]
    return op, bytes(data)


def ws_send(wfile, op, data):
    n = len(data)
    if n < 126:
        hdr = struct.pack(">BB", 0x80 | op, n)
    elif n < 65536:
        hdr = struct.pack(">BBH", 0x80 | op, 126, n)
    else:
        hdr = struct.pack(">BBQ", 0x80 | op, 127, n)
    wfile.write(hdr + data)
    wfile.flush()


class H(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def _send(self, code, body, ctype="text/plain", extra=None):
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        for k, v in (extra or {}).items():
            self.send_header(k, v)
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        if self.path == "/ping":
            return self._send(200, b"pong")
        if self.path == "/redirect":
            return self._send(302, b"", extra={"Location": "http://1.1.1.1/"})
        if self.path == "/ws" and self.headers.get("Upgrade", "").lower() == "websocket":
            key = self.headers["Sec-WebSocket-Key"]
            acc = base64.b64encode(hashlib.sha1((key + WS_GUID).encode()).digest()).decode()
            self.send_response(101)
            self.send_header("Upgrade", "websocket")
            self.send_header("Connection", "Upgrade")
            self.send_header("Sec-WebSocket-Accept", acc)
            self.end_headers()
            self.wfile.flush()
            while True:
                op, data = ws_recv(self.rfile)
                if op is None or op == 8:
                    break
                if op in (1, 2):
                    print("  ws echo %r" % data[:60])
                    ws_send(self.wfile, op, data)
                elif op == 9:
                    ws_send(self.wfile, 10, data)
            self.close_connection = True
            return
        self._send(404, b"not found")

    def do_POST(self):
        n = int(self.headers.get("Content-Length", "0") or 0)
        body = self.rfile.read(n).decode("utf-8", "replace")
        if self.path == "/echo":
            out = json.dumps({"body": body, "x_test": self.headers.get("X-Test")}).encode()
            return self._send(200, out, "application/json")
        self._send(404, b"not found")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=8766)
    a = ap.parse_args()
    socketserver.TCPServer.allow_reuse_address = True
    print("net12 test server on :%d" % a.port)
    ThreadingHTTPServer(("0.0.0.0", a.port), H).serve_forever()


if __name__ == "__main__":
    main()
