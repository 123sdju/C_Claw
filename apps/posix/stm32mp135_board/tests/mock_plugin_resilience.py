#!/usr/bin/env python3
import json
import os
import sys
import time


def read_request():
    line = sys.stdin.readline()
    if not line:
        sys.exit(1)
    return json.loads(line)


def write_response(request, result):
    sys.stdout.write(json.dumps({
        "jsonrpc": "2.0",
        "id": request.get("id", "1"),
        "result": result,
    }) + "\n")
    sys.stdout.flush()


def main():
    mode = sys.argv[1] if len(sys.argv) > 1 else "ok"
    request = read_request()
    if mode == "slow":
        time.sleep(2.0)
        write_response(request, {"mode": "slow"})
        return
    if mode == "flaky":
        state_path = sys.argv[2]
        if not os.path.exists(state_path):
            with open(state_path, "w", encoding="utf-8") as f:
                f.write("crashed-once")
            sys.exit(2)
        write_response(request, {"mode": "restarted", "pid": os.getpid()})
        return
    write_response(request, {"mode": "ok"})


if __name__ == "__main__":
    main()
