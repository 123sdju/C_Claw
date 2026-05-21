#!/usr/bin/env python3
import json
import os
import sys


def write(obj):
    sys.stdout.write(json.dumps(obj, separators=(",", ":")) + "\n")
    sys.stdout.flush()


for line in sys.stdin:
    req = json.loads(line)
    method = req.get("method")
    rid = req.get("id")
    if method == "initialize":
        write({
            "jsonrpc": "2.0",
            "id": rid,
            "result": {
                "protocolVersion": "2024-11-05",
                "capabilities": {"tools": {}},
                "serverInfo": {"name": "mock", "version": "1.0"}
            }
        })
    elif method == "tools/list":
        write({
            "jsonrpc": "2.0",
            "id": rid,
            "result": {
                "tools": [{
                    "name": "echo",
                    "description": "Echo text",
                    "inputSchema": {
                        "type": "object",
                        "properties": {"text": {"type": "string"}}
                    }
                }]
            }
        })
    elif method == "tools/call":
        params = req.get("params") or {}
        args = params.get("arguments") or {}
        text = f"{args.get('text', '')}|pid:{os.getpid()}"
        write({
            "jsonrpc": "2.0",
            "id": rid,
            "result": {"content": [{"type": "text", "text": text}]}
        })
    else:
        write({"jsonrpc": "2.0", "id": rid, "error": {"message": "unknown method"}})
