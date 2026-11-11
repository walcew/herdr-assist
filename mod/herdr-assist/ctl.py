#!/usr/bin/env python3
"""Operações do Mod de controle; cada subcomando imprime um JSON numa linha.

O main.js só desenha e chama este script por `exec`, então a lógica (quem
escuta a porta, se é a Ponte do mod, subir e reiniciar) fica testável aqui.

    ctl.py status            Ponte, versão, porta, painéis, IP, quem escuta, idioma
    ctl.py port-holder       quem escuta a porta da ponte
    ctl.py kill-holder <pid> encerra esse pid se ele ainda for o dono da porta
                             e não for a Ponte do mod
    ctl.py ensure            sobe a Ponte se não há uma viva e a porta está livre
    ctl.py restart           encerra a Ponte do mod e sobe outra
    ctl.py token             token atual, lido do doc
    ctl.py rotate-token      grava um token novo no doc; a Ponte aplica pelo evento doc
    ctl.py discover          painéis em modo de pareamento na LAN (escuta ~2,5 s)
    ctl.py pair <ip> <porta> manda ao painel a configuração pronta (host, porta, token)

A Ponte do mod é o terminal do core com o rótulo herdr-assist=bridge. Sem os
rótulos herdr.space/herdr.tab ela fica fora das telas do app.
"""
from __future__ import annotations

import json
import os
import re
import secrets
import shutil
import signal
import socket
import subprocess
import sys
import time

import update

DIR = os.path.dirname(os.path.abspath(__file__))
HC_BIN = (os.environ.get("HC_BIN") or shutil.which("hc")
          or os.path.expanduser("~/.local/bin/hc"))
LABEL = "herdr-assist=bridge"
DOC_NAME = os.environ.get("HERDR_ASSIST_DOC", "herdr-assist")   # como a Ponte
DEFAULT_PORT = 9375
PAIR_PORT = 9376   # UDP dos anúncios do painel em modo de pareamento


def run(args: list, stdin: str | None = None, timeout: float = 5) -> str:
    """stdout do comando; "" em qualquer falha (sem lsof, hc fora do ar)."""
    try:
        return subprocess.run(args, input=stdin, capture_output=True, text=True,
                              timeout=timeout).stdout
    except (OSError, subprocess.SubprocessError):
        return ""


def hc_json(args: list):
    try:
        return json.loads(run([HC_BIN, *args, "--json"]) or "null")
    except ValueError:
        return None


def rpc(method: str, params: dict) -> tuple:
    """(result, error) de um request por um `hc connect` curto."""
    req = {"jsonrpc": "2.0", "id": 1, "method": method, "params": params}
    for line in run([HC_BIN, "connect"], stdin=json.dumps(req) + "\n").splitlines():
        try:
            msg = json.loads(line)
        except ValueError:
            continue
        if msg.get("id") == 1:
            return msg.get("result"), msg.get("error")
    return None, {"message": "hc connect failed"}


def doc_read() -> tuple:
    """(configuração, versão) do doc do core; ({}, None) se o core falhou."""
    res, err = rpc("doc.get", {"name": DOC_NAME})
    if err or not isinstance(res, dict):
        return {}, None
    value = res.get("value")
    return (value if isinstance(value, dict) else {}), res.get("version", 0)


def doc_get() -> dict:
    """Configuração da ponte, lida do doc do core."""
    return doc_read()[0]


def rotate_token() -> dict:
    """Grava um token novo no doc, mantendo o resto da configuração.

    Em conflito de versão (outro escritor entre o get e o set) relê e tenta
    mais uma vez, como a Ponte faz.
    """
    new = secrets.token_hex(16)
    err = {"message": "hc connect failed"}
    for _ in range(2):
        doc, version = doc_read()
        if version is None:
            break
        _, err = rpc("doc.set", {"name": DOC_NAME, "value": {**doc, "token": new},
                                 "version": version})
        if not err:
            return {"ok": True, "token": new}
    return {"ok": False, "error": "doc.set: " + str(err.get("message", err))}


def bridge_port() -> int:
    """Mesma precedência da ponte: BRIDGE_PORT, depois o doc, depois 9375."""
    env = os.environ.get("BRIDGE_PORT", "")
    if env.isdigit():
        return int(env)
    port = doc_get().get("port")
    return port if isinstance(port, int) and 0 < port < 65536 else DEFAULT_PORT


def bridge_terminals() -> list:
    """Endereços dos terminais vivos com o rótulo da Ponte."""
    data = hc_json(["ls", "--label", LABEL]) or {}
    return [t["address"] for t in data.get("terminals", []) if t.get("state") == "live"]


def bridge_pids(addresses: list) -> set:
    """pid do terminal e do programa em primeiro plano de cada Ponte."""
    pids = set()
    for addr in addresses:
        info = hc_json(["get", addr]) or {}
        pids.add(info.get("pid"))
        pids.add((info.get("foreground") or {}).get("pid"))
    pids.discard(None)
    return pids


def listeners(port: int) -> list:
    """(pid, comando) de quem escuta a porta em TCP, pelo lsof (macOS/Linux)."""
    out = run(["lsof", "-nP", f"-iTCP:{port}", "-sTCP:LISTEN", "-Fpc"])
    found, pid = [], None
    for line in out.splitlines():
        if line.startswith("p"):
            pid = int(line[1:])
        elif line.startswith("c") and pid is not None:
            found.append((pid, line[1:]))
            pid = None
    return found


def port_holder(port: int, listen=listeners, pids=None) -> dict | None:
    """Quem escuta a porta e se é a Ponte do mod; None se a porta está livre.

    `listen` e `pids` são injetáveis para o teste.
    """
    found = listen(port)
    if not found:
        return None
    ours = bridge_pids(bridge_terminals()) if pids is None else pids
    # shortcut: compara só o pid direto; uma Ponte que rodasse num filho do
    # processo do terminal passaria por "outra". Hoje o hc roda o python direto.
    for pid, cmd in found:
        if pid in ours:
            return {"pid": pid, "command": cmd, "is_bridge": True}
    pid, cmd = found[0]
    return {"pid": pid, "command": cmd, "is_bridge": False}


def kill_holder(pid: int, port: int, holder=port_holder, kill=os.kill) -> dict:
    """Encerra `pid` só se ele ainda é o dono da porta e não é a Ponte do mod."""
    h = holder(port)
    if not h or h["pid"] != pid:
        return {"ok": False, "error": "pid is not listening on the port anymore"}
    if h["is_bridge"]:
        return {"ok": False, "error": "refusing to kill the mod's own bridge"}
    try:
        kill(pid, signal.SIGTERM)
    except OSError as exc:
        return {"ok": False, "error": str(exc)}
    return {"ok": True}


def panels(port: int) -> list:
    """IPs dos painéis conectados agora (a porta é o lado local da conexão)."""
    out = run(["lsof", "-nP", f"-iTCP:{port}", "-sTCP:ESTABLISHED"])
    found = []
    for m in re.finditer(rf":{port}->([0-9.]+):", out):
        if m.group(1) not in found:
            found.append(m.group(1))
    return found


def host_name() -> str:
    """Nome curto desta máquina; vira o rótulo do host no painel (cabe em 15)."""
    return socket.gethostname().split(".")[0][:15]


def route_ip(peer: str) -> str:
    """IP desta máquina na rota até `peer`; UDP connect não manda pacote."""
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        s.connect((peer, 9))
        return s.getsockname()[0]
    except OSError:
        return ""
    finally:
        s.close()


def lan_ip() -> str:
    """IP desta máquina na rota padrão."""
    return route_ip("192.0.2.1")


def discover(seconds: float = 2.5, port: int = PAIR_PORT) -> dict:
    """Painéis que se anunciaram em `seconds`; o painel manda um beacon por segundo."""
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    try:
        s.bind(("", port))
    except OSError as exc:
        s.close()
        return {"panels": [], "error": "udp %d: %s" % (port, exc)}
    found = {}
    end = time.time() + seconds
    try:
        while (left := end - time.time()) > 0:
            s.settimeout(left)
            try:
                data, addr = s.recvfrom(512)
                msg = json.loads(data)
                if msg.get("t") == "herdr-assist" and msg.get("id"):
                    found[str(msg["id"])] = {"id": str(msg["id"]), "ip": addr[0],
                                             "port": int(msg.get("port", PAIR_PORT))}
            except socket.timeout:
                break
            except (ValueError, TypeError, AttributeError):   # lixo da LAN
                continue
    finally:
        s.close()
    return {"panels": [found[k] for k in sorted(found)]}


def send_config(ip: str, port: int, token: str, bridge: int) -> dict:
    """Manda a configuração pronta ao painel, que grava na NVS e reinicia."""
    # auto: o painel grava o slot sem IP e acha a Ponte por broadcast, então
    # o DHCP trocar o IP do host não quebra o pareamento
    payload = {"t": "pair", "name": host_name(), "host": route_ip(ip),
               "port": bridge, "token": token, "auto": True}
    try:
        with socket.create_connection((ip, port), timeout=8) as s:
            s.settimeout(8)
            s.sendall((json.dumps(payload) + "\n").encode())
            reply = s.recv(256).decode(errors="replace")
    except OSError as exc:
        return {"ok": False, "error": str(exc)}
    try:
        if json.loads(reply).get("ok"):
            return {"ok": True}
    except (ValueError, AttributeError):
        pass
    return {"ok": False, "error": "the panel refused the configuration"}


def pair(ip: str, port: int) -> dict:
    token = doc_get().get("token")
    if not isinstance(token, str) or not token.strip():
        return {"ok": False, "error": "no token in the doc: start the bridge before pairing"}
    return send_config(ip, port, token, bridge_port())


def lang() -> str:
    """"pt" ou "en": o ambiente primeiro; no macOS, o idioma do sistema."""
    raw = (os.environ.get("HERDR_ASSIST_LANG") or os.environ.get("LC_ALL")
           or os.environ.get("LC_MESSAGES") or os.environ.get("LANG") or "")
    if not raw and sys.platform == "darwin":
        raw = run(["defaults", "read", "-g", "AppleLocale"])
    return "pt" if raw.strip().lower().startswith("pt") else "en"


def start_bridge() -> str | None:
    """Sobe a Ponte como terminal do core; devolve o endereço."""
    data = hc_json(["run", "--bg", "--no-parent", "--cwd", DIR, "--label", LABEL,
                    "--", sys.executable, os.path.join(DIR, "herdr_bridge.py")])
    return (data or {}).get("address")


def ensure(port: int) -> dict:
    alive = bridge_terminals()
    if alive:
        return {"started": None, "terminal": alive[0]}
    holder = port_holder(port, pids=set())
    if holder:   # outra ponte na porta: só avisa, quem decide é a pessoa
        return {"started": None, "terminal": None, "holder": holder}
    addr = start_bridge()
    return {"started": addr, "terminal": addr}


def restart(port: int) -> dict:
    for addr in bridge_terminals():
        run([HC_BIN, "kill", addr])
        # rm só aceita terminal já encerrado: espera até 3 s
        for _ in range(15):
            if addr not in bridge_terminals():
                break
            time.sleep(0.2)
        run([HC_BIN, "rm", addr])
    for _ in range(15):   # a porta leva um instante para soltar
        if not listeners(port):
            break
        time.sleep(0.2)
    return ensure(port)


def status() -> dict:
    alive = bridge_terminals()
    port = bridge_port()
    return {"alive": bool(alive), "terminal": alive[0] if alive else None,
            "pids": sorted(bridge_pids(alive)), "version": update.installed_version(),
            "port": port, "panels": panels(port), "ip": lan_ip(),
            "holder": port_holder(port), "lang": lang()}


def main(argv: list) -> int:
    update.recover()   # queda entre os renames de uma atualização
    cmd = argv[0] if argv else ""
    if cmd == "status":
        out = status()
    elif cmd == "port-holder":
        out = {"holder": port_holder(bridge_port())}
    elif cmd == "kill-holder" and len(argv) == 2 and argv[1].isdigit():
        out = kill_holder(int(argv[1]), bridge_port())
    elif cmd == "ensure":
        out = ensure(bridge_port())
    elif cmd == "restart":
        out = restart(bridge_port())
    elif cmd == "token":
        out = {"token": doc_get().get("token", "")}
    elif cmd == "rotate-token":
        out = rotate_token()
    elif cmd == "discover":
        out = discover()
    elif cmd == "pair" and len(argv) == 3 and argv[2].isdigit():
        out = pair(argv[1], int(argv[2]))
    else:
        print(json.dumps({"error": "usage: ctl.py status|port-holder|kill-holder <pid>|ensure"
                                   "|restart|token|rotate-token|discover|pair <ip> <port>"}))
        return 2
    print(json.dumps(out))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
