#!/usr/bin/env python3
"""`hc` falso para os testes da ponte: só o subcomando `connect`.

Fala o JSONL do `hc connect` (JSON-RPC 2.0, uma mensagem por linha) a partir
de um cenário que o teste controla em FAKE_HC_DIR:

- terminals.json: a verdade do core (lista de Terminal), relida a cada request;
- notify.jsonl: notificações que o teste anexa; quem assinou events.subscribe
  as repassa em ordem (`{"method": "event"|"missed"|..., "params": ...}`);
- calls.jsonl: cada request recebido, registrado para o teste conferir;
- read.txt: o texto que terminal.read devolve (terminal.send/keys só confirmam).
  Esses três métodos falham com not_found se o terminal não está em terminals.json.
"""
import json
import os
import sys
import threading
import time

DIR = os.environ["FAKE_HC_DIR"]
lock = threading.Lock()


def out(msg):
    with lock:
        sys.stdout.write(json.dumps(msg) + "\n")
        sys.stdout.flush()


def terminals():
    with open(os.path.join(DIR, "terminals.json")) as fh:
        return json.load(fh)


def tail_notifications():
    path = os.path.join(DIR, "notify.jsonl")
    pos = os.path.getsize(path) if os.path.exists(path) else 0
    while True:
        time.sleep(0.02)
        try:
            with open(path) as fh:
                fh.seek(pos)
                chunk = fh.read()
        except OSError:
            continue
        # só linhas completas: o teste pode estar no meio de uma escrita
        done = chunk[:chunk.rfind("\n") + 1]
        pos += len(done.encode())
        for line in done.splitlines():
            if line.strip():
                out(dict(json.loads(line), jsonrpc="2.0"))


def handle(req):
    method, params = req.get("method"), req.get("params") or {}
    with open(os.path.join(DIR, "calls.jsonl"), "a") as fh:
        fh.write(json.dumps({"method": method, "params": params}) + "\n")
    if method == "hello":
        return {"generation": 1, "version": "fake", "framed": False,
                "core": {"id": "fake", "name": "fake"}, "methods": [], "features": []}
    if method == "terminal.list":
        return {"terminals": [t for t in terminals() if t.get("state") == "live"],
                "hidden_finished": 0, "machines": []}
    if method == "terminal.get":
        for t in terminals():
            if t["address"] == params.get("terminal"):
                return t
        raise LookupError("not_found")
    if method in ("terminal.read", "terminal.send", "terminal.keys"):
        if params.get("terminal") not in [t["address"] for t in terminals()]:
            raise LookupError("not_found")
        if method == "terminal.read":
            path = os.path.join(DIR, "read.txt")
            text = open(path).read() if os.path.exists(path) else ""
            return {"text": text, "complete": True, "how": "screen"}
        return {"turn": 1, "status": None}
    if method == "events.subscribe":
        threading.Thread(target=tail_notifications, daemon=True).start()
        return {"channel": None, "seq": 1, "terminals": terminals()}
    raise LookupError("method_not_found")


def main():
    if sys.argv[1:2] != ["connect"]:
        sys.exit("fake hc: only `connect`")
    for line in sys.stdin:
        req = json.loads(line)
        try:
            out({"jsonrpc": "2.0", "id": req["id"], "result": handle(req)})
        except LookupError as exc:
            out({"jsonrpc": "2.0", "id": req["id"],
                 "error": {"code": -32000, "message": str(exc),
                           "data": {"kind": str(exc)}}})


if __name__ == "__main__":
    main()
