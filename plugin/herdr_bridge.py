#!/usr/bin/env python3
# /// script
# requires-python = ">=3.9"
# ///
"""Ponte entre o socket do Herdr e o painel herdr-assist na LAN.

O Herdr expõe sua API num unix socket, que um dispositivo na rede não alcança.
Esta ponte traduz: fala o protocolo nativo do Herdr (JSON por linha) e serve o
mesmo conteúdo por TCP. Os eventos do Herdr disparam o envio na hora, mas não
bastam: o agent_status é derivado por regra sobre o conteúdo do terminal e o
Herdr 0.8.0 não emite pane.agent_status_changed nessas transições (verificado
ao vivo). Quem garante o teto de latência é a reconciliação por timeout — um
pane.list local por segundo enquanto houver painel conectado, com broadcast
só quando o estado de fato mudou.

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
import contextlib
import datetime
import hmac
import json
import logging
import os
import re
import secrets
import shutil
import socket
import sys
import time
import urllib.error
import urllib.request

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

# pane_content vai com SGR (cores) e sem filtro de conteúdo; os caps casam com
# os buffers do firmware (HERDR_CONTENT_LEN 12288, RX_BUF_LEN 24576) e cortam
# linhas inteiras — nunca no meio de um escape.
PANE_LINES = 40
CONTENT_MAX_B = 12000
WIRE_MAX_B = 20000
SGR_RE = re.compile(r"\x1b\[[0-9;:]*m")

# Enquanto a sessão está aberta no painel, o pane é travado na resolução da tela
# do dispositivo (ver ctl_acquire). A API JSON do Herdr não tem tamanho de
# terminal — pane.resize é proporção de split —, então quem faz isso é a CLI.
HERDR_BIN = (os.environ.get("HERDR_BIN") or shutil.which("herdr")
             or "/opt/homebrew/bin/herdr")
CTL_SETTLE_S = 0.4       # tempo para a TUI reflowar antes da primeira leitura

# Ruído de terminal que não ajuda em nada numa tela de 3.5": spinners, barras,
# dicas de atalho. Some para sobrar espaço ao que importa.
CHROME_RE = re.compile(
    r"^[\s─━═_—│|◔◑◕●\s]+$"
    r"|(?i:esc to cancel)"
    r"|type to queue"
    r"|^\s*[◔◑◕●]\s+(Shell|Bash)"
)

# Opções de aprovação que o agente oferece quando bloqueia — viram botões na
# tela. Não existe API para elas: o agent_status do Herdr é derivado por regra
# sobre o texto do terminal e para por aí, então quem lê a lista é este parser.
# O formato é uma linha numerada por opção, com o cursor ❯ na selecionada; as
# descrições vêm indentadas embaixo e, por não serem numeradas, já ficam fora.
OPTION_RE = re.compile(r"^[\s│|]*(❯)?\s*(\d{1,2})\.\s+(\S.*?)\s*$")
FORM_FOOTER_RE = re.compile(r"enter to (select|confirm)", re.I)
RULE_RE = re.compile(r"^\s*[─━═_-]{10,}\s*$")
# Opções que abrem um campo de texto em vez de decidir na hora: com esta marca
# o painel já abre o teclado depois de escolher.
INPUT_OPTION_RE = re.compile(r"type something|chat about this|tell claude|tab to edit", re.I)
OPTION_MAX_B = 68        # duas linhas do botão na tela, dentro do limite do firmware
QUESTION_MAX_B = 300
PROMPT_MAX_B = 480       # cabe em HERDR_PROMPT_LEN (512) com o terminador
NAV_SETTLE_S = 0.2       # tempo para o agente redesenhar depois de uma seta

# Eventos sem pane_id na assinatura; sinalizam mudança na composição dos panes.
GENERIC_EVENTS = ("pane.updated", "pane.created", "pane.closed",
                  "pane.agent_detected", "pane.focused", "pane.exited")
DEBOUNCE_S = 0.25        # silêncio que fecha uma rajada de eventos
BURST_CAP_S = 0.5        # rajada contínua não adia o envio além disto
RECONCILE_BUSY_S = 1.0   # painel conectado: teto de latência do status
RECONCILE_IDLE_S = 5.0   # sem painel: só manter o cache morno

# Uso de limites dos provedores de IA. As credenciais são as que os próprios
# CLIs mantêm renovadas no disco — nada se configura aqui, e nenhum token sai
# desta máquina: só percentuais derivados atravessam a LAN. Arquivo ausente
# significa "CLI não instalado", não erro.
LIMITS_POLL_S = 60.0     # cadência da coleta com painel conectado
LIMITS_STEP_S = 5.0      # passo do laço: painel que conecta espera pouco
LIMITS_HTTP_TIMEOUT = 10
CLAUDE_CRED = os.path.expanduser("~/.claude/.credentials.json")
CODEX_AUTH = os.path.expanduser("~/.codex/auth.json")

clients: set[asyncio.StreamWriter] = set()
known_panes: set[str] = set()
last_status: dict[str, str] = {}
# Nem o Herdr nem sua API guardam quando o status mudou, então o instante em que
# um agente entra em "working" é carimbado aqui — é o que o painel usa para
# cronometrar. Vai como epoch absoluto (ver push_agents).
working_since: dict[str, float] = {}
last_snapshot = ""       # último {"type":"agents"} serializado que foi ao ar
last_agents: list = []   # o mesmo, ainda como objeto: quem conecta precisa dele
last_limits_snapshot = ""  # último {"type":"limits"} serializado que foi ao ar
# Última coleta boa por provedor ({"data": ..., "at": epoch}): quando a coleta
# falha, o painel recebe esses valores com ok=false e stale_since imutável —
# um stale_since que andasse a cada falha quebraria o dedup do snapshot.
limits_last_good: dict[str, dict] = {}
limits_ok: dict[str, bool] = {}  # só para logar transições, não cada ciclo
# Controller de resolução: no máximo um pane travado por vez (o painel só abre
# uma sessão de cada vez). proc é o `herdr terminal session control` vivo.
ctl: dict = {"pane": None, "size": None, "proc": None}
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


def clip(text: str, limit: int) -> str:
    """Corta em `limit` bytes sem partir caractere UTF-8.

    O firmware guarda estes campos em buffers de tamanho fixo e trunca por
    byte: cortar aqui, no lugar certo, evita meio caractere acentuado virar
    lixo na tela.
    """
    raw = text.encode()
    if len(raw) <= limit:
        return text
    return raw[:limit].decode(errors="ignore").rstrip() + "…"


def strip_box(line: str) -> str:
    """Tira a moldura: o prompt de permissão vem dentro de uma caixa."""
    return line.strip().strip("│|╭╮╰╯├┤┌┐└┘").strip()


def extract_question(lines: list[str], first_option: int) -> str:
    """O bloco de texto logo acima da lista é a pergunta em si.

    Uma linha só de moldura conta como vazia — é ela que separa a pergunta do
    comando ou do diff que o agente mostrou acima.
    """
    def blank(i: int) -> bool:
        return not strip_box(lines[i]) or bool(RULE_RE.match(lines[i]))

    i = first_option - 1
    while i >= 0 and blank(i):
        i -= 1
    out = []
    while i >= 0 and not blank(i) and len(out) < 4:
        out.append(strip_box(lines[i]))
        i -= 1
    out.reverse()
    return clip(" ".join(out), QUESTION_MAX_B)


def parse_form(raw: str) -> dict | None:
    """Lê o formulário de escolha que está na tela do agente.

    Devolve a pergunta, as opções (número, rótulo, se pedem texto) e em qual
    delas está o cursor — ou None se o que está na tela não é um formulário
    navegável, caso em que o painel mostra o texto e nenhum botão.
    """
    lines = raw.splitlines()
    if not any(FORM_FOOTER_RE.search(l) for l in lines[-6:]):
        return None

    hits = []
    for i, line in enumerate(lines):
        m = OPTION_RE.match(line)
        if m:
            hits.append((i, int(m.group(2)), strip_box(m.group(3)), m.group(1) is not None))

    # Sobe do último item encaixando a numeração: assim réguas e linhas em
    # branco no meio da lista não a quebram, e qualquer lista numerada que o
    # agente tenha escrito acima fica de fora.
    block: list = []
    for hit in reversed(hits):
        if not block or hit[1] == block[-1][1] - 1:
            block.append(hit)
        if block[-1][1] == 1:
            break
    block.reverse()
    if len(block) < 2 or block[0][1] != 1:
        return None

    return {
        "question": extract_question(lines, block[0][0]),
        "options": [{"n": n, "label": clip(label, OPTION_MAX_B),
                     "input": bool(INPUT_OPTION_RE.search(label))}
                    for _, n, label, _ in block],
        "cursor": next((k for k, h in enumerate(block) if h[3]), None),
    }


async def broadcast_line(line: str) -> None:
    data = (line + "\n").encode()
    for w in list(clients):
        try:
            w.write(data)
            await w.drain()
        except (ConnectionResetError, BrokenPipeError):
            clients.discard(w)


async def broadcast(msg: dict) -> None:
    await broadcast_line(json.dumps(msg, separators=(",", ":")))


async def read_pane_raw(pane_id: str, lines: int = 50) -> str:
    result = await herdr_request("pane.read", {"pane_id": pane_id, "lines": lines,
                                               "source": "recent"})
    if not result:
        return ""
    return result.get("read", {}).get("text", "")


async def read_pane(pane_id: str, lines: int = 40) -> str:
    return clean_output(await read_pane_raw(pane_id, lines))


async def read_pane_ansi(pane_id: str, lines: int = PANE_LINES) -> str:
    """Tela fiel, com SGR (cores/estilos), para o pane_content do painel.

    O emulador interno do Herdr é o motor do Ghostty; format:"ansi" re-emite o
    grid já emulado com os estilos por célula. O caminho de texto puro
    (read_pane/clean_output) continua existindo para o respond/blocked, que
    fazem match por regex sobre o texto.

    source "visible" (e não "recent") porque é o viewport: é ele que a rolagem
    move, seja o app rolando o próprio conteúdo ou o Herdr rolando o scrollback.
    Com "recent" o painel ficaria preso no fim, sem enxergar o que o usuário
    rolou. Nesse modo o Herdr ignora `lines` — quem manda é o tamanho do pane.
    """
    result = await herdr_request("pane.read", {"pane_id": pane_id, "lines": lines,
                                               "source": "visible", "format": "ansi",
                                               "strip_ansi": False})
    if not result:
        return ""
    return result.get("read", {}).get("text", "")


async def ctl_release() -> None:
    """Solta a trava de resolução: o pane volta ao tamanho da UI do Herdr."""
    proc = ctl["proc"]
    pane = ctl["pane"]
    ctl.update(pane=None, size=None, proc=None)
    if not proc or proc.returncode is not None:
        return
    try:
        proc.stdin.write(b'{"type":"terminal.release"}\n')
        await proc.stdin.drain()
        await asyncio.wait_for(proc.wait(), 2)
    except (OSError, asyncio.TimeoutError):
        with contextlib.suppress(ProcessLookupError):
            proc.kill()
    log.info("pane %s solto", pane)


async def ctl_acquire(pane_id: str, cols: int, rows: int) -> bool:
    """Trava o pane em cols x rows. True se o controller acabou de subir.

    O Herdr redimensiona o pty de verdade (TIOCSWINSZ) e trava o tamanho
    enquanto este cliente viver, devolvendo tudo quando ele cai. O stdin em pipe
    é o cinto de segurança: se esta ponte morrer de qualquer jeito, o filho vê
    EOF, manda `Detach` sozinho e a sessão volta ao normal.
    """
    if (ctl["pane"] == pane_id and ctl["size"] == (cols, rows)
            and ctl["proc"] and ctl["proc"].returncode is None):
        return False
    await ctl_release()
    try:
        proc = await asyncio.create_subprocess_exec(
            HERDR_BIN, "terminal", "session", "control", pane_id,
            "--cols", str(cols), "--rows", str(rows),
            stdin=asyncio.subprocess.PIPE,
            stdout=asyncio.subprocess.DEVNULL,   # os frames do controller não servem aqui
            stderr=asyncio.subprocess.DEVNULL)
    except OSError as err:
        log.warning("resolução não travada em %s: %s", pane_id, err)
        return False
    ctl.update(pane=pane_id, size=(cols, rows), proc=proc)
    log.info("pane %s travado em %dx%d", pane_id, cols, rows)
    return True


async def ctl_scroll(pane_id: str, up: bool, lines: int, col: int, row: int) -> bool:
    """Rolagem nativa: manda uma roda de mouse ao pane travado.

    Quem decide o destino é o Herdr (server/headless.rs): app com mouse
    tracking (é o caso do Claude Code) recebe o evento e rola o próprio
    conteúdo; senão o scrollback do emulador é que anda. A posição não é
    detalhe: com o ponteiro fora da área de transcript o Claude Code ignora a
    roda (medido), por isso vem a célula sob o dedo, não o centro da tela.
    """
    proc = ctl["proc"]
    if ctl["pane"] != pane_id or not proc or proc.returncode is not None:
        return False
    cols, rows = ctl["size"]
    cmd = {"type": "terminal.scroll", "direction": "up" if up else "down",
           "lines": lines, "source": "wheel",
           "column": max(0, min(col, cols - 1)), "row": max(0, min(row, rows - 1))}
    try:
        proc.stdin.write((json.dumps(cmd) + "\n").encode())
        await proc.stdin.drain()
    except OSError:
        return False
    return True


def trim_ansi(raw: str, keep: int) -> str:
    """Últimas `keep` linhas da tela autêntica, dentro de CONTENT_MAX_B.

    Sem CHROME_RE aqui: spinners e réguas agora renderizam direito no painel.
    Só as linhas em branco do fim caem; estourando o cap, caem linhas inteiras
    mais antigas (nunca um corte no meio de escape/UTF-8).
    """
    lines = raw.splitlines()
    while lines and not SGR_RE.sub("", lines[-1]).strip():
        lines.pop()
    lines = lines[-keep:]
    while len(lines) > 1 and len("\n".join(lines).encode()) > CONTENT_MAX_B:
        lines.pop(0)
    return "\n".join(lines)


async def blocked_message(agent: dict) -> dict:
    """Monta o alerta lendo a tela na hora.

    Sem formulário reconhecível vão a cauda da tela e nenhuma opção: melhor o
    painel ficar sem botão do que com botão que não corresponde à pergunta.
    """
    raw = await read_pane_raw(agent["pane_id"])
    form = parse_form(raw)
    return {"type": "blocked", "pane_id": agent["pane_id"], "agent": agent["agent"],
            "project": agent["project"],
            "prompt": (form["question"] if form
                       else clip(clean_output(raw)[-500:], PROMPT_MAX_B)),
            "options": form["options"] if form else []}


async def answer_form(pane_id: str, choice: int, label: str) -> bool:
    """Move o cursor até a opção pedida e só então confirma.

    Mandar o texto da opção não escolhe nada: num seletor as letras são
    ignoradas e o Enter confirma o que estiver marcado — sempre a primeira.
    Daí navegar por setas e reler a tela antes de confirmar: se o formulário
    mudou entre o toque no painel e a chegada do comando, nada é enviado.
    """
    form = parse_form(await read_pane_raw(pane_id))
    if not form or form["cursor"] is None:
        log.warning("respond pane=%s: nenhum formulário navegável na tela", pane_id)
        return False
    target = next((k for k, o in enumerate(form["options"]) if o["n"] == choice), None)
    if target is None or form["options"][target]["label"] != label:
        log.warning("respond pane=%s: opção %d não confere com %r", pane_id, choice, label)
        return False

    delta = target - form["cursor"]
    if delta:
        key = "Down" if delta > 0 else "Up"
        await herdr_request("pane.send_keys", {"pane_id": pane_id, "keys": [key] * abs(delta)})
        await asyncio.sleep(NAV_SETTLE_S)
        form = parse_form(await read_pane_raw(pane_id))
        if (not form or form["cursor"] != target or target >= len(form["options"])
                or form["options"][target]["n"] != choice):
            log.warning("respond pane=%s: cursor não parou na opção %d", pane_id, choice)
            return False

    log.info("respond pane=%s opção=%d %r", pane_id, choice, label)
    await herdr_request("pane.send_keys", {"pane_id": pane_id, "keys": ["Enter"]})
    return True


async def push_agents() -> set[str] | None:
    """Reconcilia com o Herdr; envia aos painéis só o que de fato mudou.

    Retorna os pane_ids de TODOS os panes (com e sem agente) para o event_loop
    saber quando reassinar, ou None se o Herdr não respondeu — que não é motivo
    para reassinar.
    """
    global last_snapshot, last_agents, working_since
    result = await herdr_request("pane.list")
    if not result:
        return None
    panes = result.get("panes", [])
    # O carimbo vai como epoch absoluto, e não como duração: o broadcast só sai
    # quando o snapshot serializado muda (adiante), então uma duração mudaria a
    # cada segundo e faria a ponte transmitir sem parar — e o painel reconstruir
    # a lista inteira junto. Absoluto, fica imutável enquanto o agente trabalha.
    now = time.time()
    new_since = {}
    agents = []
    for p in panes:
        if not p.get("agent"):
            continue
        pid, status = p["pane_id"], p.get("agent_status", "unknown")
        a = {
            "pane_id": pid,
            "agent": p.get("agent", ""),
            "status": status,
            "project": os.path.basename(p.get("cwd", "")),
            "workspace_id": p.get("workspace_id", ""),
        }
        if status == "working":
            # só o primeiro ciclo em working cria o carimbo; os seguintes o herdam
            new_since[pid] = working_since.get(pid, now)
            a["since"] = int(new_since[pid])
        agents.append(a)
    # reconstruir em vez de mutar poda de graça quem saiu de working ou sumiu
    working_since = new_since
    known_panes.clear()
    known_panes.update(a["pane_id"] for a in agents)
    last_agents = agents
    snapshot = json.dumps({"type": "agents", "agents": agents}, separators=(",", ":"))
    if snapshot != last_snapshot:
        last_snapshot = snapshot
        await broadcast_line(snapshot)

    for a in agents:
        pid, status = a["pane_id"], a["status"]
        if status == "blocked" and last_status.get(pid) != "blocked":
            await broadcast(await blocked_message(a))
        last_status[pid] = status
    return {p["pane_id"] for p in panes}


async def event_loop() -> None:
    """Mantém os painéis em dia combinando eventos e reconciliação.

    Os eventos genéricos e o pane.agent_status_changed (assinado por pane, o
    único jeito que a API aceita) disparam envio imediato; o timeout do
    readline faz o resto — fecha rajadas (DEBOUNCE_S) e, no silêncio, vira a
    reconciliação que garante o teto de latência do status. Quando o conjunto
    de panes muda, a conexão é refeita para reassinar os novos.
    """
    while True:
        try:
            reader, writer = await asyncio.open_unix_connection(SOCK)
        except (FileNotFoundError, ConnectionRefusedError):
            log.warning("Herdr indisponível, tentando em 5s")
            await asyncio.sleep(5)
            continue

        subscribed = await push_agents()   # estado inicial + panes a assinar
        if subscribed is None:
            writer.close()
            await asyncio.sleep(2)
            continue
        subs = [{"type": t} for t in GENERIC_EVENTS]
        subs += [{"type": "pane.agent_status_changed", "pane_id": pid}
                 for pid in sorted(subscribed)]
        writer.write((json.dumps({"id": "sub", "method": "events.subscribe",
                                  "params": {"subscriptions": subs}}) + "\n").encode())
        await writer.drain()
        resubscribe = False
        pending_since = None
        got_response = False
        try:
            while True:
                if pending_since:
                    timeout = DEBOUNCE_S
                else:
                    timeout = RECONCILE_BUSY_S if clients else RECONCILE_IDLE_S
                try:
                    line = await asyncio.wait_for(reader.readline(), timeout)
                except asyncio.TimeoutError:
                    pending_since = None
                    ids = await push_agents()
                    if ids is not None and ids != subscribed:
                        resubscribe = True
                        break
                    continue
                if not line:
                    break
                try:
                    msg = json.loads(line)
                except json.JSONDecodeError:
                    continue
                if not got_response:   # 1ª linha é a resposta do subscribe
                    got_response = True
                    if "error" in msg:
                        log.warning("subscribe recusado: %s",
                                    msg["error"].get("message"))
                    else:
                        log.info("assinado: %d genéricos + %d panes",
                                 len(GENERIC_EVENTS), len(subscribed))
                    continue
                ev = msg.get("event", "")
                now = time.monotonic()
                if (ev == "pane_agent_status_changed" or
                        (pending_since and now - pending_since > BURST_CAP_S)):
                    pending_since = None
                    ids = await push_agents()
                    if ids is not None and ids != subscribed:
                        resubscribe = True
                        break
                elif pending_since is None:
                    pending_since = now
        except (ConnectionResetError, BrokenPipeError):
            pass
        finally:
            writer.close()
        if resubscribe:
            log.info("conjunto de panes mudou, reassinando")
            continue
        log.warning("conexão de eventos caiu, reassinando em 2s")
        await asyncio.sleep(2)


def fetch_json(url: str, headers: dict) -> dict:
    """GET + parse, síncrona — é ela que bloqueia, por isso a coleta roda no
    executor. Isolada também para o teste trocar por fixtures."""
    req = urllib.request.Request(url, headers=headers)
    with urllib.request.urlopen(req, timeout=LIMITS_HTTP_TIMEOUT) as resp:
        return json.loads(resp.read().decode())


def round_min(epoch: float) -> int:
    """Arredonda um epoch ao minuto cheio.

    O servidor recalcula o instante de reset a cada resposta e ele treme ±1s
    entre chamadas — sem arredondar, quase todo ciclo geraria um snapshot
    "novo" e broadcast à toa. O painel só mostra HH:MM, nada se perde.
    """
    return int(round(epoch / 60) * 60)


def iso_epoch(s: str) -> int:
    """ISO 8601 → epoch ao minuto. O fromisoformat do 3.9 não aceita 'Z'."""
    return round_min(datetime.datetime.fromisoformat(s.replace("Z", "+00:00")).timestamp())


def collect_claude() -> dict:
    """Uso do Claude Code pelo endpoint OAuth, com o token que o CLI renova.

    É o mesmo endpoint interno que alimenta o /usage do CLI; formato pode mudar
    sem aviso, e qualquer surpresa aqui vira falha do provedor, nunca crash
    (collect_limits embrulha tudo). O array limits[] já vem normalizado —
    session (5h), weekly_all (7d) e weekly_scoped por modelo.
    """
    with open(CLAUDE_CRED) as f:
        cred = json.load(f)["claudeAiOauth"]
    if cred.get("expiresAt", 0) / 1000 <= time.time():
        # o CLI renova no próximo uso; request agora seria um 401 garantido
        raise RuntimeError("token expirado")
    data = fetch_json("https://api.anthropic.com/api/oauth/usage",
                      {"Authorization": "Bearer " + cred["accessToken"],
                       "anthropic-beta": "oauth-2025-04-20"})
    rows = []
    for lim in data.get("limits", [])[:4]:      # teto espelhado no firmware
        kind = lim.get("kind", "")
        if kind == "session":
            label = "5h"
        elif kind == "weekly_all":
            label = "7d"
        elif kind == "weekly_scoped":
            model = ((lim.get("scope") or {}).get("model") or {}).get("display_name") or "?"
            label = clip("7d " + model, 16)     # cabe em label[20] mesmo truncado
        else:
            label = clip(kind, 16)              # kind novo aparece cru, não some
        rows.append({"label": label, "pct": int(round(lim.get("percent") or 0)),
                     "resets_at": iso_epoch(lim["resets_at"]) if lim.get("resets_at") else 0})
    # o plano vem do próprio arquivo de credencial: default_claude_max_20x → Max 20x
    tier = cred.get("rateLimitTier", "")
    plan = (tier.split("claude_")[-1].replace("_", " ").capitalize()
            if tier else cred.get("subscriptionType", "").capitalize())
    return {"name": "Claude", "plan": plan, "limits": rows}


def collect_codex() -> dict:
    """Uso do Codex pelo backend do ChatGPT, com o token que o CLI renova.

    Atenção ao reset_after_seconds: muda a cada segundo e por isso NÃO entra
    na saída — só o reset_at (epoch fixo), senão o dedup do snapshot morre.
    """
    with open(CODEX_AUTH) as f:
        tok = json.load(f)["tokens"]
    data = fetch_json("https://chatgpt.com/backend-api/wham/usage",
                      {"Authorization": "Bearer " + tok["access_token"],
                       "chatgpt-account-id": tok["account_id"]})
    rl = data.get("rate_limit") or {}
    rows = []
    for win in (rl.get("primary_window"), rl.get("secondary_window")):
        if not win:
            continue
        secs = win.get("limit_window_seconds") or 0
        label = ("%dd" % round(secs / 86400) if secs >= 86400
                 else "%dh" % max(1, round(secs / 3600)))
        rows.append({"label": label, "pct": int(round(win.get("used_percent") or 0)),
                     "resets_at": round_min(win.get("reset_at") or 0)})
    return {"name": "Codex", "plan": (data.get("plan_type") or "").capitalize(),
            "limits": rows}


def collect_limits() -> list:
    """Monta a lista de provedores, degradando com honestidade.

    Três estados por provedor: credencial ausente → omitido (CLI não instalado
    nesta máquina); falha com sucesso anterior → últimos valores com ok=false e
    stale_since do último sucesso; falha sem histórico → omitido. Ordem fixa
    dos provedores: a serialização precisa ser determinística para o dedup.
    """
    providers = []
    for key, path, collect in (("claude", CLAUDE_CRED, collect_claude),
                               ("codex", CODEX_AUTH, collect_codex)):
        if not os.path.exists(path):
            limits_last_good.pop(key, None)
            limits_ok.pop(key, None)
            continue
        try:
            cur = collect()
            cur.update(ok=True, stale_since=0)
            limits_last_good[key] = {"data": {k: cur[k] for k in ("name", "plan", "limits")},
                                     "at": int(time.time())}
            providers.append(cur)
            if limits_ok.get(key) is not True:
                log.info("limites %s: ok (%d janelas)", key, len(cur["limits"]))
            limits_ok[key] = True
        except Exception as e:  # endpoint interno: qualquer surpresa é falha, não crash
            reason = ("HTTP %d" % e.code if isinstance(e, urllib.error.HTTPError)
                      else str(e) or type(e).__name__)
            good = limits_last_good.get(key)
            if good:
                stale = dict(good["data"])
                stale.update(ok=False, stale_since=good["at"])
                providers.append(stale)
            if limits_ok.get(key) is not False:
                log.warning("limites %s: %s", key, reason)
            limits_ok[key] = False
    return providers


async def limits_loop() -> None:
    """Coleta o uso de limites e difunde com o mesmo dedup de agents.

    Passo curto com contabilização própria em vez de dormir o ciclo inteiro:
    um painel que conecta depois de ociosidade espera LIMITS_STEP_S pelo
    primeiro dado, não LIMITS_POLL_S. Sem painel conectado, nada é coletado.
    """
    global last_limits_snapshot
    last = None   # sentinela: a base do monotonic varia por plataforma
    while True:
        if clients and (last is None or time.monotonic() - last >= LIMITS_POLL_S):
            last = time.monotonic()
            providers = await asyncio.get_running_loop().run_in_executor(None, collect_limits)
            snapshot = json.dumps({"type": "limits", "providers": providers},
                                  separators=(",", ":"))
            if snapshot != last_limits_snapshot:
                last_limits_snapshot = snapshot
                await broadcast_line(snapshot)
        await asyncio.sleep(LIMITS_STEP_S)


async def handle_command(msg: dict, writer: asyncio.StreamWriter) -> None:
    kind = msg.get("type")
    pane_id = msg.get("pane_id", "")

    async def deny(reason: str) -> None:
        writer.write((json.dumps({"type": "error", "message": reason}) + "\n").encode())
        await writer.drain()

    if kind in ("read_pane", "send_keys", "send_text", "respond", "focus",
                "release_pane", "scroll_pane"):
        if pane_id not in known_panes:
            return await deny("pane desconhecido")

    if kind == "read_pane":
        lines = max(1, min(int(msg.get("lines", PANE_LINES)), 60))
        # O painel manda a geometria da própria tela: enquanto ele estiver lendo
        # este pane, a sessão fica nesse tamanho e o conteúdo cabe sem arrastar.
        cols, rows = int(msg.get("cols", 0)), int(msg.get("rows", 0))
        if 4 <= cols <= 400 and 2 <= rows <= 200 and await ctl_acquire(pane_id, cols, rows):
            await asyncio.sleep(CTL_SETTLE_S)
        content = trim_ansi(await read_pane_ansi(pane_id, lines), lines)

        def encode_payload(c: str) -> bytes:
            # ensure_ascii=False: box-drawing cru pesa ~1.2x no JSON (a forma
            # \uXXXX passa de 1.4x e estouraria o RX do firmware à toa)
            return json.dumps({"type": "pane_content", "pane_id": pane_id,
                               "content": c}, separators=(",", ":"),
                              ensure_ascii=False).encode()

        payload = encode_payload(content)
        while len(payload) > WIRE_MAX_B and "\n" in content:
            content = content.split("\n", 1)[1]   # densidade patológica: derruba as antigas
            payload = encode_payload(content)
        writer.write(payload + b"\n")
        await writer.drain()

    elif kind == "send_keys":
        keys = msg.get("keys", [])
        if not all(k in SAFE_KEYS for k in keys):
            return await deny("tecla fora da allowlist")
        log.info("send_keys pane=%s keys=%s", pane_id, keys)
        await herdr_request("pane.send_keys", {"pane_id": pane_id, "keys": keys})

    elif kind == "send_text":
        text = msg.get("text", "")
        if not text or len(text) > MAX_TEXT:
            return await deny("texto vazio ou longo demais")
        log.info("send_text pane=%s len=%d", pane_id, len(text))
        await herdr_request("pane.send_text", {"pane_id": pane_id, "text": text})

    elif kind == "respond":
        choice = msg.get("choice")
        if not isinstance(choice, int) or not 1 <= choice <= 20:
            return await deny("respond exige o número da opção")
        if not await answer_form(pane_id, choice, msg.get("label", "")):
            return await deny("a tela mudou; nada foi enviado")

    elif kind == "focus":
        log.info("focus pane=%s", pane_id)
        await herdr_request("pane.focus", {"pane_id": pane_id})

    elif kind == "scroll_pane":
        lines = max(1, min(int(msg.get("lines", 1)), 20))
        await ctl_scroll(pane_id, msg.get("dir") != "down", lines,
                         int(msg.get("col", 0)), int(msg.get("row", 0)))

    elif kind == "release_pane":
        if ctl["pane"] == pane_id:
            await ctl_release()

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
        await push_agents()      # atualiza o cache (e broadcasta se mudou)
        if last_snapshot:        # entrega direta: o broadcast acima pode ter
            writer.write((last_snapshot + "\n").encode())  # sido suprimido
            await writer.drain()
        if last_limits_snapshot:  # limites: mesmo trio cache/push/entrega
            writer.write((last_limits_snapshot + "\n").encode())
            await writer.drain()
        # Um bloqueio que já estava de pé não gera transição: sem isto, o painel
        # que reconecta fica sem o alerta até algum outro agente parar.
        for a in last_agents:
            if a["status"] == "blocked":
                msg = json.dumps(await blocked_message(a), separators=(",", ":"))
                writer.write((msg + "\n").encode())
                await writer.drain()
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
        if not clients:   # ninguém mais olhando: devolve a sessão ao tamanho normal
            await ctl_release()
        log.info("painel desconectado: %s", peer)


async def main() -> None:
    global TOKEN
    if not os.path.exists(SOCK):
        log.error("socket do Herdr não encontrado: %s", SOCK)
        sys.exit(1)
    TOKEN = load_token()
    asyncio.create_task(event_loop())
    asyncio.create_task(limits_loop())
    server = await asyncio.start_server(handle_client, BIND, PORT)
    log.info("ponte ouvindo em %s:%d", BIND, PORT)
    async with server:
        await server.serve_forever()


if __name__ == "__main__":
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        pass
