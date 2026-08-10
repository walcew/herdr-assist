#!/usr/bin/env python3
# /// script
# requires-python = ">=3.9"
# ///
"""Ponte entre o socket do Herdr e o painel herdr-assist na LAN.

O Herdr expõe sua API num unix socket, que um dispositivo na rede não alcança.
Esta ponte traduz: fala o protocolo nativo do Herdr (JSON por linha) e serve o
mesmo conteúdo por TCP, com push real — o Herdr avisa quando algo muda, então
não há polling.

Sem dependências externas: só a stdlib, compatível com o python3 de fábrica do
macOS (3.9). Roda como plugin do Herdr (ver start.sh), que fornece as variáveis
HERDR_SOCKET_PATH e HERDR_PLUGIN_CONFIG_DIR; para rodar avulsa:

    BRIDGE_TOKEN=... python3 plugin/herdr_bridge.py

Toda conexão exige um handshake `{"type":"hello","token":"..."}` na primeira
linha — sem token não roda: se nenhum existir, um é gerado e salvo (0600).

Variáveis: HERDR_SOCK, BRIDGE_PORT (9375), BRIDGE_BIND (0.0.0.0), BRIDGE_TOKEN.
"""
from __future__ import annotations

import asyncio
import hmac
import json
import logging
import os
import re
import secrets
import socket
import sys

SOCK = (os.environ.get("HERDR_SOCK")
        or os.environ.get("HERDR_SOCKET_PATH")  # fornecida pelo plugin do Herdr
        or os.path.expanduser("~/.config/herdr/herdr.sock"))
PORT = int(os.environ.get("BRIDGE_PORT", "9375"))
BIND = os.environ.get("BRIDGE_BIND", "0.0.0.0")
TOKEN_FILE = os.path.join(
    os.environ.get("HERDR_PLUGIN_CONFIG_DIR", os.path.expanduser("~/.config/herdr-assist")),
    "token")
TOKEN = ""  # carregado em main()

logging.basicConfig(level=logging.INFO, format="%(asctime)s %(levelname)s %(message)s",
                    datefmt="%H:%M:%S")
log = logging.getLogger("bridge")

# Só estas teclas podem ser enviadas a um agente. O painel fica numa mesa e
# qualquer um na LAN alcança esta porta: sem allowlist, um toque acidental (ou
# um curioso na rede) vira execução de comando arbitrário num projeto real.
SAFE_KEYS = {"y", "n", "a", "Enter", "Tab", "Escape", "C-c", "Up", "Down",
             "Left", "Right", "BSpace"} | {str(n) for n in range(10)}
MAX_TEXT = 1000

# Ruído de terminal que não ajuda em nada numa tela de 3.5": spinners, barras,
# dicas de atalho. Some para sobrar espaço ao que importa.
CHROME_RE = re.compile(
    r"^[\s─━═_—│|◔◑◕●\s]+$"
    r"|esc to cancel"
    r"|type to queue"
    r"|^\s*[◔◑◕●]\s+(Shell|Bash)"
)

# Opções de aprovação que o Claude oferece quando bloqueia — viram botões na tela.
TOOL_OPTIONS = ["yes, single permission", "trust, always allow", "no (tab to edit)"]
SUBAGENT_OPTIONS = ["approve all pending", "configure individually", "exit (cancel subagents)"]

clients: set[asyncio.StreamWriter] = set()
known_panes: set[str] = set()
last_status: dict[str, str] = {}
_req_id = 0


def load_token() -> str:
    """Token da ponte, nesta ordem: env, arquivo do plugin, ou gera um novo.

    Fail-closed de propósito: não existe modo sem autenticação. O arquivo fica
    no config-dir do plugin (editável pelo usuário; sobrescreva para igualar o
    token entre máquinas).
    """
    tok = os.environ.get("BRIDGE_TOKEN", "").strip()
    if tok:
        return tok
    try:
        with open(TOKEN_FILE) as f:
            tok = f.read().strip()
        if tok:
            return tok
    except OSError:
        pass
    tok = secrets.token_hex(16)
    os.makedirs(os.path.dirname(TOKEN_FILE), exist_ok=True)
    with open(TOKEN_FILE, "w") as f:
        f.write(tok + "\n")
    os.chmod(TOKEN_FILE, 0o600)
    log.info("token gerado em %s — cadastre-o no painel", TOKEN_FILE)
    return tok


async def herdr_request(method: str, params: dict | None = None) -> dict | None:
    """Um request ao Herdr. O socket atende um por conexão, então abre e fecha."""
    global _req_id
    _req_id += 1
    try:
        reader, writer = await asyncio.open_unix_connection(SOCK)
    except (FileNotFoundError, ConnectionRefusedError):
        log.warning("Herdr não está rodando (%s)", SOCK)
        return None
    try:
        writer.write((json.dumps({"id": f"b{_req_id}", "method": method,
                                  "params": params or {}}) + "\n").encode())
        await writer.drain()
        line = await asyncio.wait_for(reader.readline(), timeout=10)
    except (asyncio.TimeoutError, ConnectionResetError, BrokenPipeError):
        return None
    finally:
        writer.close()
    if not line:
        return None
    resp = json.loads(line)
    if "error" in resp:
        log.warning("herdr %s: %s", method, resp["error"].get("message"))
        return None
    return resp.get("result")


def clean_output(raw: str, keep: int = 20) -> str:
    lines = [l for l in raw.splitlines() if l.strip() and not CHROME_RE.search(l)]
    return "\n".join(lines[-keep:])


def detect_options(text: str) -> list[str]:
    lower = text.lower()
    if "approve all pending" in lower:
        return SUBAGENT_OPTIONS
    return TOOL_OPTIONS


async def broadcast(msg: dict) -> None:
    data = (json.dumps(msg, separators=(",", ":")) + "\n").encode()
    for w in list(clients):
        try:
            w.write(data)
            await w.drain()
        except (ConnectionResetError, BrokenPipeError):
            clients.discard(w)


async def read_pane(pane_id: str, lines: int = 40) -> str:
    result = await herdr_request("pane.read", {"pane_id": pane_id, "lines": lines,
                                               "source": "recent"})
    if not result:
        return ""
    return clean_output(result.get("read", {}).get("text", ""))


async def push_agents() -> list[dict]:
    """Envia o estado atual de todos os agentes e avisa sobre quem bloqueou."""
    result = await herdr_request("pane.list")
    if not result:
        return []
    agents = [
        {
            "pane_id": p["pane_id"],
            "agent": p.get("agent", ""),
            "status": p.get("agent_status", "unknown"),
            "project": os.path.basename(p.get("cwd", "")),
            "workspace_id": p.get("workspace_id", ""),
        }
        for p in result.get("panes", []) if p.get("agent")
    ]
    known_panes.clear()
    known_panes.update(a["pane_id"] for a in agents)
    await broadcast({"type": "agents", "agents": agents})

    for a in agents:
        pid, status = a["pane_id"], a["status"]
        if status == "blocked" and last_status.get(pid) != "blocked":
            content = await read_pane(pid)
            await broadcast({"type": "blocked", "pane_id": pid, "agent": a["agent"],
                             "project": a["project"], "prompt": content[:500],
                             "options": detect_options(content)})
        last_status[pid] = status
    return agents


async def event_loop() -> None:
    """Assina os eventos do Herdr e reemite o estado quando algo muda."""
    while True:
        try:
            reader, writer = await asyncio.open_unix_connection(SOCK)
        except (FileNotFoundError, ConnectionRefusedError):
            log.warning("Herdr indisponível, tentando em 5s")
            await asyncio.sleep(5)
            continue
        subs = [{"type": t} for t in ("pane.updated", "pane.created", "pane.closed",
                                      "pane.agent_detected", "pane.focused", "pane.exited")]
        writer.write((json.dumps({"id": "sub", "method": "events.subscribe",
                                  "params": {"subscriptions": subs}}) + "\n").encode())
        await writer.drain()
        log.info("assinado aos eventos do Herdr")
        await push_agents()
        try:
            while True:
                line = await reader.readline()
                if not line:
                    break
                # Vários eventos chegam em rajada; um pequeno atraso agrupa a
                # rajada inteira num único envio ao painel.
                await asyncio.sleep(0.3)
                while not reader.at_eof():
                    try:
                        await asyncio.wait_for(reader.readline(), timeout=0.05)
                    except asyncio.TimeoutError:
                        break
                await push_agents()
        except (ConnectionResetError, BrokenPipeError):
            pass
        finally:
            writer.close()
        log.warning("conexão de eventos caiu, reassinando em 2s")
        await asyncio.sleep(2)


async def handle_command(msg: dict, writer: asyncio.StreamWriter) -> None:
    kind = msg.get("type")
    pane_id = msg.get("pane_id", "")

    async def deny(reason: str) -> None:
        writer.write((json.dumps({"type": "error", "message": reason}) + "\n").encode())
        await writer.drain()

    if kind in ("read_pane", "send_keys", "send_text", "respond", "focus"):
        if pane_id not in known_panes:
            return await deny("pane desconhecido")

    if kind == "read_pane":
        content = await read_pane(pane_id, int(msg.get("lines", 40)))
        writer.write((json.dumps({"type": "pane_content", "pane_id": pane_id,
                                  "content": content}, separators=(",", ":")) + "\n").encode())
        await writer.drain()

    elif kind == "send_keys":
        keys = msg.get("keys", [])
        if not all(k in SAFE_KEYS for k in keys):
            return await deny("tecla fora da allowlist")
        log.info("send_keys pane=%s keys=%s", pane_id, keys)
        await herdr_request("pane.send_keys", {"pane_id": pane_id, "keys": keys})

    elif kind in ("send_text", "respond"):
        text = msg.get("text", "")
        if not text or len(text) > MAX_TEXT:
            return await deny("texto vazio ou longo demais")
        log.info("%s pane=%s len=%d", kind, pane_id, len(text))
        if kind == "respond":
            text += "\n"
        await herdr_request("pane.send_text", {"pane_id": pane_id, "text": text})

    elif kind == "focus":
        log.info("focus pane=%s", pane_id)
        await herdr_request("pane.focus", {"pane_id": pane_id})

    elif kind == "ping":
        # Com push, ficar em silêncio é normal — o painel usa isto para saber
        # que a conexão continua viva em vez de esperar dados que não vêm.
        writer.write(b'{"type":"pong"}\n')
        await writer.drain()

    else:
        await deny("comando desconhecido")


async def handle_client(reader: asyncio.StreamReader, writer: asyncio.StreamWriter) -> None:
    peer = writer.get_extra_info("peername")
    sock = writer.get_extra_info("socket")
    if sock is not None:  # detecta queda de link sem FIN (Wi-Fi some, painel desliga)
        sock.setsockopt(socket.SOL_SOCKET, socket.SO_KEEPALIVE, 1)

    # Handshake obrigatório: a primeira linha precisa ser um hello com o token.
    # Qualquer um na LAN alcança esta porta; sem isso, alcançar = controlar.
    try:
        line = await asyncio.wait_for(reader.readline(), timeout=5)
        hello = json.loads(line) if line else {}
    except (asyncio.TimeoutError, json.JSONDecodeError, ConnectionResetError):
        hello = {}
    if not (isinstance(hello, dict) and hello.get("type") == "hello" and
            hmac.compare_digest(str(hello.get("token", "")), TOKEN)):
        log.warning("hello inválido de %s — desconectando", peer)
        try:
            writer.write(b'{"type":"error","message":"token invalido"}\n')
            await writer.drain()
        except (ConnectionResetError, BrokenPipeError):
            pass
        writer.close()
        return

    log.info("painel conectado: %s", peer)
    clients.add(writer)
    try:
        await push_agents()  # estado inicial, sem esperar o próximo evento
        while True:
            line = await reader.readline()
            if not line:
                break
            try:
                msg = json.loads(line)
            except json.JSONDecodeError:
                continue
            await handle_command(msg, writer)
    except (ConnectionResetError, BrokenPipeError):
        pass
    finally:
        clients.discard(writer)
        writer.close()
        log.info("painel desconectado: %s", peer)


async def main() -> None:
    global TOKEN
    if not os.path.exists(SOCK):
        log.error("socket do Herdr não encontrado: %s", SOCK)
        sys.exit(1)
    TOKEN = load_token()
    asyncio.create_task(event_loop())
    server = await asyncio.start_server(handle_client, BIND, PORT)
    log.info("ponte ouvindo em %s:%d", BIND, PORT)
    async with server:
        await server.serve_forever()


if __name__ == "__main__":
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        pass
