#!/usr/bin/env python3
import json
import sys
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer


SESSION_ID = "mock-session"


def make_response(req, session_header):
    method = req.get("method")
    rid = req.get("id")
    if method == "initialize":
        return {
            "jsonrpc": "2.0",
            "id": rid,
            "result": {
                "protocolVersion": "2024-11-05",
                "capabilities": {"tools": {}},
                "serverInfo": {"name": "mock-http", "version": "1.0"},
            },
        }
    if method == "tools/list":
        return {
            "jsonrpc": "2.0",
            "id": rid,
            "result": {
                "tools": [{
                    "name": "echo",
                    "description": "Echo text over HTTP MCP",
                    "inputSchema": {
                        "type": "object",
                        "properties": {"text": {"type": "string"}},
                    },
                }]
            },
        }
    if method == "tools/call":
        params = req.get("params") or {}
        args = params.get("arguments") or {}
        text = args.get("text", "")
        if session_header:
            text = f"{text}|session:{session_header}"
        return {
            "jsonrpc": "2.0",
            "id": rid,
            "result": {"content": [{"type": "text", "text": text}]},
        }
    return {"jsonrpc": "2.0", "id": rid, "error": {"message": "unknown method"}}


class Handler(BaseHTTPRequestHandler):
    mode = "json"

    def log_message(self, fmt, *args):
        return

    def do_POST(self):
        length = int(self.headers.get("Content-Length", "0"))
        req = json.loads(self.rfile.read(length).decode("utf-8"))
        session_header = self.headers.get("Mcp-Session-Id")
        resp = make_response(req, session_header)
        payload = json.dumps(resp, separators=(",", ":")).encode("utf-8")

        if self.mode in ("sse", "streamable"):
            self.send_response(200)
            self.send_header("Content-Type", "text/event-stream")
            if self.mode == "streamable":
                self.send_header("Mcp-Session-Id", SESSION_ID)
            self.end_headers()
            self.wfile.write(b":heartbeat\n")
            self.wfile.write(b"data: ")
            self.wfile.write(payload)
            self.wfile.write(b"\n\n")
            self.wfile.flush()
            return

        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.end_headers()
        self.wfile.write(payload)


class ReusableHTTPServer(ThreadingHTTPServer):
    allow_reuse_address = True


def main():
    port = int(sys.argv[1])
    Handler.mode = sys.argv[2]
    try:
        server = ReusableHTTPServer(("127.0.0.1", port), Handler)
    except PermissionError as exc:
        print(f"SKIP {exc}", flush=True)
        return
    print("READY", flush=True)
    server.serve_forever()


if __name__ == "__main__":
    main()
