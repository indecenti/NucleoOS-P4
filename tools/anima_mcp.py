"""MCP server for a NucleoOS board: lets any MCP client (Claude Desktop / Claude Code, OpenCode,
OpenClaw, Cursor...) talk to ANIMA and drive the device. No dependencies (stdlib only); it speaks
MCP over stdio and calls the board's HTTP API, so no firmware change is needed.

  claude mcp add nucleo -- python tools/anima_mcp.py --host 192.168.1.50
  opencode / Claude Desktop: command "python", args ["tools/anima_mcp.py", "--host", "<board ip>"]

Tools: anima_ask, anima_mode, anima_models, device_status, ui_state, ui_open, say, fs_list, fs_read,
fs_write (only with --allow-write), logs. Pairing: the token tools/pair.py saved (as tools/anima.py).
--selftest runs the protocol against a fake board and exits (no network needed).
"""
import argparse
import json
import os
import sys
import time
import urllib.error
import urllib.parse
import urllib.request

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

PROTOCOL = "2025-06-18"
SERVER = {"name": "nucleoos-anima", "version": "1.0.0"}
MODES = ["offline", "local", "hybrid", "llm"]
READ_CAP = 64 * 1024


class Board:
    """The board's HTTP API (the same endpoints the web OS uses)."""

    def __init__(self, host, headers=None):
        self.base = "http://" + host
        self.headers = headers or {}

    def request(self, method, path, query=None, body=None, raw=False, timeout=140):
        url = self.base + path + ("?" + urllib.parse.urlencode(query) if query else "")
        data = body if isinstance(body, bytes) else (json.dumps(body).encode() if body is not None else None)
        hdr = dict(self.headers)
        if body is not None and not isinstance(body, bytes):
            hdr["Content-Type"] = "application/json"
        req = urllib.request.Request(url, data=data, method=method, headers=hdr)
        try:
            with urllib.request.urlopen(req, timeout=timeout) as r:
                out = r.read()
        except urllib.error.HTTPError as e:
            if e.code == 401:
                raise RuntimeError("not paired with the board (401): run python tools/pair.py --host " + self.base[7:])
            raise RuntimeError("HTTP %d: %s" % (e.code, e.read().decode("utf-8", "replace")[:200]))
        except (urllib.error.URLError, OSError) as e:
            raise RuntimeError("cannot reach %s: %s" % (self.base, getattr(e, "reason", e)))
        return out if raw else json.loads(out.decode("utf-8", "replace") or "{}")


def S(props, required=()):
    return {"type": "object", "properties": props, "required": list(required)}


STR = {"type": "string"}
TOOLS = [
    ("anima_ask", "Ask ANIMA, the device assistant, in natural language (Italian or English). Commands really run "
     "on the device (open apps, volume, reminders, files). Returns what it understood, did and replied.",
     S({"question": STR, "lang": {"type": "string", "enum": ["it", "en"]},
        "conversation": {"type": "boolean", "description": "use the stored conversation (memory + context)"}}, ["question"])),
    ("anima_mode", "Get or set ANIMA's network mode: offline (nothing leaves the device), local (LAN only, e.g. Ollama), "
     "hybrid (offline first, then web/AI), llm (language model first).",
     S({"mode": {"type": "string", "enum": MODES}})),
    ("anima_models", "List the models the configured AI server offers (Ollama, LM Studio, cloud).", S({})),
    ("device_status", "Device snapshot: battery, Wi-Fi, memory, storage, ANIMA engine status.", S({})),
    ("ui_state", "Which app is in the foreground on the device screen.", S({})),
    ("ui_open", "Open a native app on the device screen by id (e.g. calc, notes, music, settings, files).",
     S({"id": STR}, ["id"])),
    ("say", "Speak a sentence through the device speaker.", S({"text": STR, "lang": {"type": "string", "enum": ["it", "en"]}}, ["text"])),
    ("fs_list", "List a folder on the device SD card (logical paths, e.g. /data/Documents).", S({"path": STR}, ["path"])),
    ("fs_read", "Read a text file from the device SD card (first 64 KB).", S({"path": STR}, ["path"])),
    ("fs_write", "Write a text file on the device SD card (creates or REPLACES it).", S({"path": STR, "content": STR}, ["path", "content"])),
    ("logs", "The device's recent log lines (for diagnosing problems).", S({})),
]


def render(r):
    """The compact text tools/anima.py prints for one answer."""
    if r.get("busy"):
        return "(busy: the device is answering another request, retry in a moment)"
    head = "[%s%s]" % (r.get("intent") or "-", (" " + r["arg"]) if r.get("arg") else "")
    bits = [head] + ["tier=%s" % r["tier"]] * bool(r.get("tier")) + ["conf=%s" % r["conf"]] * (r.get("conf") is not None)
    out = [" ".join(bits), (r.get("reply") or "(no reply)").strip()]
    if "done" in r:
        out.append("└ %s %s" % ("done:" if r["done"] else "NOT done:", r.get("note", "")))
    if r.get("why"):
        out.append("└ why: " + r["why"])
    if r.get("trace"):
        out.append("└ " + r["trace"])
    return "\n".join(out)


class Server:
    def __init__(self, board, allow_write=False):
        self.board = board
        self.allow_write = allow_write
        self.conv = ""

    def tools(self):
        return [{"name": n, "description": d, "inputSchema": s} for n, d, s in TOOLS
                if n != "fs_write" or self.allow_write]

    def call(self, name, a):
        b = self.board
        if name == "anima_ask":
            lang = a.get("lang") or "it"
            if a.get("conversation"):
                r = b.request("POST", "/api/anima/chat", body={"q": a["question"], "conv": self.conv, "lang": lang})
            else:
                r = b.request("GET", "/api/anima", {"q": a["question"], "lang": lang})
            end = time.time() + 900                      # a long agent turn answers "pending": collect it
            while r.get("pending") and r.get("job") and time.time() < end:
                r = b.request("GET", "/api/anima/job", {"id": r["job"], "wait_ms": 1500})
            if a.get("conversation"):
                self.conv = r.get("conv") or self.conv
            return render(r)
        if name == "anima_mode":
            r = b.request("POST", "/api/anima/net", body={"mode": a["mode"]}) if a.get("mode") else b.request("GET", "/api/anima/net")
            return json.dumps(r)
        if name == "anima_models":
            return json.dumps(b.request("GET", "/api/anima/models"), ensure_ascii=False)
        if name == "device_status":
            st = b.request("GET", "/api/status")
            for k, path in (("anima", "/api/anima/caps"), ("wake", "/api/anima/wake")):
                try:
                    st[k] = b.request("GET", path)
                except RuntimeError:
                    pass
            return json.dumps(st, ensure_ascii=False)
        if name == "ui_state":
            return json.dumps(b.request("GET", "/api/ui/state"))
        if name == "ui_open":
            return json.dumps(b.request("GET", "/api/ui/open", {"id": a["id"]}))
        if name == "say":
            return json.dumps(b.request("GET", "/api/say", {"text": a["text"], "lang": a.get("lang") or "it"}))
        if name == "fs_list":
            r = b.request("GET", "/api/fs/list", {"path": a["path"]})
            rows = ["%s%s  %s" % (e.get("name"), "/" if e.get("isDir") or e.get("type") == "dir" else "",
                                  "" if e.get("isDir") else e.get("size", "")) for e in r.get("entries", [])]
            return "\n".join(rows) or "(empty)"
        if name == "fs_read":
            raw = b.request("GET", "/api/fs/read", {"path": a["path"]}, raw=True)
            text = raw[:READ_CAP].decode("utf-8", "replace")
            return text + ("\n…(truncated at 64 KB)" if len(raw) > READ_CAP else "")
        if name == "fs_write":
            if not self.allow_write:
                raise RuntimeError("writes are disabled (start the server with --allow-write)")
            r = b.request("POST", "/api/fs/write", {"path": a["path"]}, body=a["content"].encode("utf-8"))
            return json.dumps(r)
        if name == "logs":
            raw = b.request("GET", "/api/logs", raw=True)
            return raw.decode("utf-8", "replace")[-8000:]
        raise KeyError(name)

    def handle(self, msg):
        """One JSON-RPC message -> the response dict (None for notifications)."""
        mid, method, p = msg.get("id"), msg.get("method"), msg.get("params") or {}
        if mid is None:
            return None                       # notifications (initialized, cancelled): nothing to answer
        if method == "initialize":
            res = {"protocolVersion": p.get("protocolVersion") or PROTOCOL, "capabilities": {"tools": {}},
                   "serverInfo": SERVER,
                   "instructions": "NucleoOS device tools. Use anima_ask for anything the user would say to the device."}
        elif method == "ping":
            res = {}
        elif method == "tools/list":
            res = {"tools": self.tools()}
        elif method == "tools/call":
            name = p.get("name")
            if name not in {t["name"] for t in self.tools()}:
                return {"jsonrpc": "2.0", "id": mid, "error": {"code": -32602, "message": "unknown tool: %s" % name}}
            try:
                res = {"content": [{"type": "text", "text": self.call(name, p.get("arguments") or {})}], "isError": False}
            except (RuntimeError, KeyError, ValueError) as e:
                res = {"content": [{"type": "text", "text": "error: %s" % e}], "isError": True}
        else:
            return {"jsonrpc": "2.0", "id": mid, "error": {"code": -32601, "message": "method not found: %s" % method}}
        return {"jsonrpc": "2.0", "id": mid, "result": res}

    def serve(self, fin=sys.stdin, fout=sys.stdout):
        for line in fin:
            line = line.strip()
            if not line:
                continue
            try:
                msg = json.loads(line)
            except ValueError:
                out = {"jsonrpc": "2.0", "id": None, "error": {"code": -32700, "message": "parse error"}}
            else:
                out = self.handle(msg)
            if out is not None:
                fout.write(json.dumps(out, ensure_ascii=False) + "\n")
                fout.flush()


def selftest():
    """The protocol end to end against an in-process fake board."""
    import threading
    from http.server import BaseHTTPRequestHandler, HTTPServer

    class Fake(BaseHTTPRequestHandler):
        def log_message(self, *a):
            pass

        def _send(self, body, ctype="application/json"):
            data = body if isinstance(body, bytes) else json.dumps(body).encode()
            self.send_response(200)
            self.send_header("Content-Type", ctype)
            self.send_header("Content-Length", str(len(data)))
            self.end_headers()
            self.wfile.write(data)

        def do_GET(self):
            u = urllib.parse.urlparse(self.path)
            q = dict(urllib.parse.parse_qsl(u.query))
            if u.path == "/api/anima":
                self._send({"intent": "set_volume", "arg": "30", "tier": "command", "conf": 95,
                            "reply": "Volume al 30%.", "done": True, "note": "volume 30%", "q": q.get("q")})
            elif u.path == "/api/anima/net":
                self._send({"mode": "hybrid"})
            elif u.path == "/api/fs/list":
                self._send({"entries": [{"name": "nota.txt", "type": "file", "size": 12}, {"name": "Foto", "isDir": True}]})
            elif u.path == "/api/fs/read":
                self._send(b"ciao dal device", "text/plain")
            else:
                self.send_response(404)
                self.end_headers()

        def do_POST(self):
            n = int(self.headers.get("Content-Length") or 0)
            body = self.rfile.read(n)
            self._send({"ok": True, "path": self.path, "bytes": len(body)})

    httpd = HTTPServer(("127.0.0.1", 0), Fake)
    threading.Thread(target=httpd.serve_forever, daemon=True).start()
    srv = Server(Board("127.0.0.1:%d" % httpd.server_port))
    fails = 0

    def rpc(method, params=None, mid=1):
        return srv.handle({"jsonrpc": "2.0", "id": mid, "method": method, "params": params or {}})

    def check(cond, what):
        nonlocal fails
        print(("ok   " if cond else "FAIL ") + what)
        fails += not cond

    init = rpc("initialize", {"protocolVersion": "2025-03-26", "capabilities": {}, "clientInfo": {"name": "t"}})
    check(init["result"]["protocolVersion"] == "2025-03-26" and "tools" in init["result"]["capabilities"], "initialize")
    check(srv.handle({"jsonrpc": "2.0", "method": "notifications/initialized"}) is None, "notification gets no reply")
    names = [t["name"] for t in rpc("tools/list")["result"]["tools"]]
    check("anima_ask" in names and "fs_write" not in names, "tools/list (fs_write hidden without --allow-write)")
    r = rpc("tools/call", {"name": "anima_ask", "arguments": {"question": "volume al 30"}})["result"]
    check(not r["isError"] and "[set_volume 30]" in r["content"][0]["text"] and "done: volume 30%" in r["content"][0]["text"], "anima_ask")
    r = rpc("tools/call", {"name": "anima_mode", "arguments": {}})["result"]
    check('"hybrid"' in r["content"][0]["text"], "anima_mode get")
    r = rpc("tools/call", {"name": "fs_list", "arguments": {"path": "/data"}})["result"]
    check("nota.txt" in r["content"][0]["text"] and "Foto/" in r["content"][0]["text"], "fs_list")
    r = rpc("tools/call", {"name": "fs_read", "arguments": {"path": "/data/nota.txt"}})["result"]
    check(r["content"][0]["text"] == "ciao dal device", "fs_read")
    check("error" in rpc("tools/call", {"name": "fs_write", "arguments": {"path": "/x", "content": "y"}}), "fs_write refused when disabled")
    srv.allow_write = True
    r = rpc("tools/call", {"name": "fs_write", "arguments": {"path": "/data/x.txt", "content": "abc"}})["result"]
    check('"bytes": 3' in r["content"][0]["text"], "fs_write with --allow-write")
    r = rpc("tools/call", {"name": "ui_state", "arguments": {}})["result"]
    check(r["isError"] and "HTTP 404" in r["content"][0]["text"], "device errors come back as isError")
    check(rpc("nope")["error"]["code"] == -32601, "unknown method")
    # stdio framing
    import io
    out = io.StringIO()
    srv.serve(io.StringIO('{"jsonrpc":"2.0","id":7,"method":"ping"}\nnot json\n'), out)
    lines = out.getvalue().splitlines()
    check(json.loads(lines[0]) == {"jsonrpc": "2.0", "id": 7, "result": {}} and json.loads(lines[1])["error"]["code"] == -32700, "stdio framing")
    httpd.shutdown()
    print("%s: %d failed" % ("SELFTEST OK" if not fails else "SELFTEST FAILED", fails))
    return 1 if fails else 0


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--host", default=os.environ.get("NV_BOARD_IP") or os.environ.get("NUCLEO_HOST") or "nucleov2.local")
    ap.add_argument("--allow-write", action="store_true", help="expose fs_write (files on the SD can be replaced)")
    ap.add_argument("--selftest", action="store_true")
    a = ap.parse_args()
    if a.selftest:
        return selftest()
    try:
        from nvtoken import auth_headers
        headers = auth_headers()
    except Exception:  # noqa: BLE001 - an unpaired board still answers /api/anima reads
        headers = {}
    Server(Board(a.host, headers), a.allow_write).serve()
    return 0


if __name__ == "__main__":
    sys.exit(main())
