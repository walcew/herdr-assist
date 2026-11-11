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

Modo framed (hello com framed:true, como o attach da ponte usa): depois da
resposta do hello tudo vira frame `<IBI` (canal, kind, len) + payload, com o
JSON-RPC no canal 0. terminal.attach devolve um canal novo e despeja uma
repintura nele (com mouse tracking ligado, salvo "mouse": false no terminal); os frames recebidos nos canais de attach vão para calls.jsonl
como frame.data (text), frame.credit (n), frame.resize (cols, rows),
frame.close, e o EOF do stdin com attach aberto vira frame.eof (channels).
"""
import itertools
import json
import os
import struct
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


def record(method, params):
    with open(os.path.join(DIR, "calls.jsonl"), "a") as fh:
        fh.write(json.dumps({"method": method, "params": params}) + "\n")


def frame(ch, kind, payload):
    with lock:
        sys.stdout.buffer.write(struct.pack("<IBI", ch, kind, len(payload)) + payload)
        sys.stdout.buffer.flush()


channels = set()   # attaches abertos nesta conexão
next_channel = itertools.count(1)


def handle(req):
    method, params = req.get("method"), req.get("params") or {}
    record(method, params)
    if method == "hello":
        return {"generation": 1, "version": "fake", "framed": bool(params.get("framed")),
                "core": {"id": "fake", "name": "fake"}, "methods": [], "features": []}
    if method == "terminal.list":
        return {"terminals": [t for t in terminals() if t.get("state") == "live"],
                "hidden_finished": 0, "machines": []}
    if method == "terminal.get":
        for t in terminals():
            if t["address"] == params.get("terminal"):
                return t
        raise LookupError("not_found")
    if method in ("terminal.read", "terminal.send", "terminal.keys", "terminal.attach"):
        if params.get("terminal") not in [t["address"] for t in terminals()]:
            raise LookupError("not_found")
        if method == "terminal.attach":
            ch = next(next_channel)
            channels.add(ch)
            return {"channel": ch}
        if method == "terminal.read":
            path = os.path.join(DIR, "read.txt")
            text = open(path).read() if os.path.exists(path) else ""
            return {"text": text, "complete": True, "how": "screen"}
        return {"turn": 1, "status": None}
    if method == "events.subscribe":
        threading.Thread(target=tail_notifications, daemon=True).start()
        return {"channel": None, "seq": 1, "terminals": terminals()}
    raise LookupError("method_not_found")


def repaint(req):
    """Como o core: zera os modos e religa os ativos; "mouse": false desliga a roda."""
    t = next(t for t in terminals() if t["address"] == req["params"]["terminal"])
    on = b"\x1b[?1000h\x1b[?1006h" if t.get("mouse", True) else b""
    return b"\x1b[?1000l\x1b[?1006l\x1b[2J" + on + b"repaint"


def reply(req):
    try:
        return {"jsonrpc": "2.0", "id": req["id"], "result": handle(req)}
    except LookupError as exc:
        return {"jsonrpc": "2.0", "id": req["id"],
                "error": {"code": -32000, "message": str(exc),
                          "data": {"kind": str(exc)}}}


def framed():
    inp = sys.stdin.buffer
    while True:
        head = inp.read(9)
        if len(head) < 9:
            break
        ch, kind, n = struct.unpack("<IBI", head)
        payload = inp.read(n)
        if ch == 0:
            resp = reply(json.loads(payload))
            frame(0, 0, json.dumps(resp).encode())
            if "channel" in resp.get("result", {}):
                frame(resp["result"]["channel"], 0, repaint(json.loads(payload)))
        elif kind == 0:
            record("frame.data", {"channel": ch, "text": payload.decode()})
        elif kind == 1:
            record("frame.credit", {"channel": ch, "n": struct.unpack("<I", payload)[0]})
        elif kind == 2:
            channels.discard(ch)
            record("frame.close", {"channel": ch})
        elif kind == 3:
            cols, rows = struct.unpack("<HH", payload)
            record("frame.resize", {"channel": ch, "cols": cols, "rows": rows})
    if channels:
        record("frame.eof", {"channels": sorted(channels)})


def main():
    if sys.argv[1:2] != ["connect"]:
        sys.exit("fake hc: only `connect`")
    while True:
        line = sys.stdin.buffer.readline()
        if not line:
            break
        req = json.loads(line)
        out(reply(req))
        if req.get("method") == "hello" and (req.get("params") or {}).get("framed"):
            return framed()


if __name__ == "__main__":
    main()
