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
macOS (3.9). Roda como plugin do Herdr (ver start.py), que fornece as variáveis
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
import glob
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

import accounts
import claude_cli
import cost
import transcript
from accounts import read_account_email, default_dir, CONFIG_VAR  # noqa: E402
from proc_env import read_process_env  # noqa: E402

SOCK = (os.environ.get("HERDR_SOCK")
        or os.environ.get("HERDR_SOCKET_PATH")  # fornecida pelo plugin do Herdr
        or os.path.expanduser("~/.config/herdr/herdr.sock"))
PORT = int(os.environ.get("BRIDGE_PORT", "9375"))
BIND = os.environ.get("BRIDGE_BIND", "0.0.0.0")
CONFIG_DIR = os.environ.get("HERDR_PLUGIN_CONFIG_DIR",
                            os.path.expanduser("~/.config/herdr-assist"))
TOKEN_FILE = os.path.join(CONFIG_DIR, "token")
TOKEN = ""  # carregado em main()

# Repositórios de avatar que o painel deve varrer além do padrão de fábrica:
# uma URL base por linha, "#" comenta. Mesma convenção do arquivo token ao lado.
# Lido a cada conexão, não no boot — editar o arquivo e reconectar o painel
# basta, sem reiniciar a ponte.
AVATAR_REPOS_FILE = os.path.join(CONFIG_DIR, "avatar_repos")
AVATAR_REPOS_MAX = 2   # teto espelhado no firmware (STORE_BRIDGE_REPOS)

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
LIMITS_MAX_CARDS = 8     # teto espelhado no firmware (HERDR_MAX_PROVIDERS)
HOME = os.path.expanduser("~")  # injetável nos testes


def _cred_path(agent: str, config_dir: str) -> str:
    """Caminho da credencial do agente dentro do config-dir da conta."""
    return os.path.join(config_dir, ".credentials.json" if agent == "claude"
                        else "auth.json")

clients: set[asyncio.StreamWriter] = set()
known_panes: set[str] = set()
# Nem o Herdr nem sua API guardam quando o status mudou, então o instante em que
# um agente entra em "working" é carimbado aqui — é o que o painel usa para
# cronometrar. Vai como epoch absoluto (ver push_agents).
working_since: dict[str, float] = {}
last_snapshot = ""       # último {"type":"agents"} serializado que foi ao ar
last_limits_snapshot = ""  # último {"type":"limits"} serializado que foi ao ar
# Última coleta boa por provedor ({"data": ..., "at": epoch}): quando a coleta
# falha, o painel recebe esses valores com ok=false e stale_since imutável —
# um stale_since que andasse a cada falha quebraria o dedup do snapshot.
limits_last_good: dict[tuple[str, str], dict] = {}
limits_ok: dict[tuple[str, str], bool] = {}  # só para logar transições, não cada ciclo
# Controller de resolução: no máximo um pane travado por vez (o painel só abre
# uma sessão de cada vez). proc é o `herdr terminal session control` vivo.
ctl: dict = {"pane": None, "size": None, "proc": None}
_req_id = 0

# Descoberta de contas por pane (ver accounts.py): qual config-dir cada pane
# usa e o conjunto de contas em uso, para taguear agents[] e alimentar a
# coleta de limites por conta (collect_limits). Semeado com os defaults logo
# abaixo para a primeira coleta nunca rodar vazia.
pane_account: dict = {}          # pane_id -> (agent, config_dir)
account_dirs: set = {(a, default_dir(a, HOME)) for a in CONFIG_VAR}
acc_email_cache: dict = {}       # (agent, config_dir) -> e-mail (clipado)
# Chave (pane_id, agent), não só pane_id: um agente trocado NO MESMO pane
# (reinicia com outro CLI) precisa invalidar o pid cacheado — o pid do SO
# mudou mesmo que o pane continue vivo.
pane_pid_cache: dict = {}        # (pane_id, agent) -> pid do processo em 1º plano
pid_env_cache: dict = {}         # pid -> env (só os config-dirs, já filtrado)
_prev_agent_panes = [set()]      # {(pane_id, agent)} na última refresh_accounts

# Métricas (model, context_pct) por transcript, uma entrada por CAMINHO (não
# por pane nem clear() a cada miss): dois+ panes claude concorrentes não se
# expulsam do cache. push_agents poda as entradas cujo path saiu da rodada.
tx_cache: dict = {}   # path -> (mtime, {"model","context_pct"} | None)
# Mapa pane→transcript da última rodada de push_agents (mesma resolução por
# UUID/cwd usada para tx_cache): é o que _session_paths() devolve para a
# agregação de custo "agora" sem precisar chamar o Herdr de novo.
pane_paths: dict = {}  # pane_id -> transcript path

# Custo (ver cost.py): cache por (mtime, size) — só relê um transcript se ele
# cresceu — e snapshot para o dedup do broadcast "cost", nos mesmos moldes de
# tx_cache/last_limits_snapshot.
cost_cache: dict = {}        # path -> (mtime, size, {"total","days"})
last_cost_snapshot = ""      # dedup do payload cost


async def pane_pid(pane_id: str):
    """Pid do processo em primeiro plano do pane, via `pane.process_info`."""
    info = await herdr_request("pane.process_info", {"pane_id": pane_id})
    if not info:
        return None
    procs = (info.get("process_info") or {}).get("foreground_processes") or []
    return procs[0].get("pid") if procs else None


def _read_env_filtered(pid):
    """Ambiente do processo `pid`, já filtrado só às chaves de CONFIG_VAR.

    Bloqueante (ctypes no Windows, /proc no Linux, `ps` com timeout no macOS —
    até ~5s no pior caso): só chamar de dentro de um executor, nunca direto do
    event loop. Guarda só o config-dir — nada mais do ambiente fica em memória.
    """
    env = read_process_env(pid)
    return {k: env[k] for k in CONFIG_VAR.values() if k in env}


def _cached_env(pid):
    """Ambiente já cacheado de um pid — leitura pura, sem I/O.

    Depende de refresh_accounts ter pré-populado pid_env_cache no executor
    antes de chamar accounts.discover(); por isso não dispara `read_process_env`
    aqui, ao contrário de antes (isso bloquearia o event loop).
    """
    return pid_env_cache.get(pid, {})


async def refresh_accounts(panes) -> None:
    """Recalcula pane_account/account_dirs/acc_email_cache.

    Chamada só quando a composição de panes (pane_id, agent) muda (ver
    push_agents) — não a cada ciclo de reconciliação de 1s. Todo I/O
    bloqueante (leitura de ambiente por pid) roda num executor: o event loop
    não pode travar enquanto ctypes/`/proc`/`ps` respondem.
    """
    global pane_account, account_dirs, acc_email_cache

    live_keys = {(p["pane_id"], p.get("agent")) for p in panes}
    for k in list(pane_pid_cache):   # poda pid cache de panes/agentes que sumiram
        if k not in live_keys:
            pane_pid_cache.pop(k, None)

    # resolve os pids que faltam, concorrente (cada um já é I/O não-bloqueante
    # via cli_run/subprocess assíncrono, então gather em vez de await sequencial)
    targets = [p for p in panes if p.get("agent") in CONFIG_VAR]
    to_resolve = [p for p in targets
                 if (p["pane_id"], p.get("agent")) not in pane_pid_cache]
    if to_resolve:
        resolved = await asyncio.gather(*(pane_pid(p["pane_id"]) for p in to_resolve))
        for p, pid in zip(to_resolve, resolved):
            pane_pid_cache[(p["pane_id"], p.get("agent"))] = pid
    pids = {p["pane_id"]: pane_pid_cache.get((p["pane_id"], p.get("agent")))
           for p in targets}

    # leitura de ambiente é bloqueante — todas de uma vez, fora do event loop
    new_pids = {pid for pid in pids.values() if pid and pid not in pid_env_cache}
    if new_pids:
        loop = asyncio.get_running_loop()
        envs = await loop.run_in_executor(
            None, lambda ps=new_pids: {p: _read_env_filtered(p) for p in ps})
        pid_env_cache.update(envs)

    # discover() é síncrona; a essa altura pid_env_cache já tem tudo que ela
    # precisa, então _cached_env não faz I/O nenhum aqui dentro
    pane_account, account_dirs = accounts.discover(
        panes, lambda pane: pids.get(pane), _cached_env, HOME)
    acc_email_cache = {(a, d): clip(read_account_email(a, d, HOME), 32)
                       for (a, d) in account_dirs}

    live_pids = {pid for pid in pids.values() if pid}  # poda env de pids sumidos
    for k in list(pid_env_cache):
        if k not in live_pids:
            pid_env_cache.pop(k, None)


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


# No Windows o socket do Herdr é um named pipe cujo NOME é o caminho inteiro
# (\\.\pipe\C:\Users\...\herdr.sock): open() normaliza o caminho e tropeça no
# "C:" do meio, CreateFileW devolve erro 123, e o CPython nem expõe AF_UNIX ali.
# A CLI devolve o mesmo JSON, é o caminho que a documentação do Herdr indica
# para automação, e custa ~19ms por chamada — barato o bastante para o ritmo
# desta ponte. Só o transporte muda: o protocolo com o painel é idêntico.
USE_CLI = os.name == "nt" or not hasattr(socket, "AF_UNIX")
# Sem isto cada chamada pisca um console: a ponte roda destacada, sem console
# próprio, e um flash por segundo seria insuportável.
NO_WINDOW = 0x08000000 if os.name == "nt" else 0


async def cli_run(args: list[str], timeout: float = 10) -> str | None:
    """Roda o herdr e devolve o stdout. None em qualquer falha.

    Decodifica aqui, em utf-8: deixar o Python decidir pelo locale (cp1252 no
    Windows) estoura na primeira caixa ou seta que a tela do terminal trouxer,
    e a saída volta vazia sem erro visível.
    """
    try:
        proc = await asyncio.create_subprocess_exec(
            HERDR_BIN, *args,
            stdout=asyncio.subprocess.PIPE,
            stderr=asyncio.subprocess.DEVNULL,
            creationflags=NO_WINDOW)
    except (OSError, ValueError) as exc:
        log.warning("herdr %s: %s", args[0] if args else "?", exc)
        return None
    try:
        out, _ = await asyncio.wait_for(proc.communicate(), timeout)
    except asyncio.TimeoutError:
        with contextlib.suppress(ProcessLookupError):
            proc.kill()
        return None
    if proc.returncode != 0:
        return None
    return out.decode("utf-8", "replace")


async def cli_request(method: str, params: dict) -> dict | None:
    """Traduz um método da API para a CLI, devolvendo a MESMA forma de result.

    Quem chama não sabe qual transporte está em uso — é isso que mantém o resto
    da ponte com um caminho só.
    """
    pane = params.get("pane_id", "")
    if method == "pane.list":
        out = await cli_run(["pane", "list"])
        if not out:
            return None
        try:
            return json.loads(out).get("result")
        except (ValueError, AttributeError):
            return None
    if method == "pane.read":
        # a CLI devolve o ANSI cru no stdout, não JSON
        args = ["pane", "read", pane,
                "--source", params.get("source", "visible"),
                "--format", params.get("format", "ansi")]
        if params.get("lines"):
            args += ["--lines", str(params["lines"])]
        out = await cli_run(args)
        return None if out is None else {"read": {"text": out}}
    if method == "pane.send_keys":
        keys = params.get("keys", [])
        out = await cli_run(["pane", "send-keys", pane, *keys])
        return None if out is None else {}
    if method == "pane.send_text":
        out = await cli_run(["pane", "send-text", pane, params.get("text", "")])
        return None if out is None else {}
    if method == "pane.focus":
        # a CLI separa: `pane focus` é por direção; focar por id é `agent focus`
        out = await cli_run(["agent", "focus", pane])
        return None if out is None else {}
    if method == "pane.process_info":
        out = await cli_run(["pane", "process-info", "--pane", pane])
        if not out:
            return None
        try:
            return json.loads(out).get("result")
        except (ValueError, AttributeError):
            return None
    log.warning("metodo sem equivalente na CLI: %s", method)
    return None


async def herdr_request(method: str, params: dict | None = None) -> dict | None:
    """Um request ao Herdr, pelo transporte que a plataforma permite."""
    if USE_CLI:
        return await cli_request(method, params or {})
    return await sock_request(method, params)


async def sock_request(method: str, params: dict | None = None) -> dict | None:
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


async def broadcast_line(line: str) -> None:
    data = (line + "\n").encode()
    for w in list(clients):
        try:
            w.write(data)
            await w.drain()
        except (ConnectionResetError, BrokenPipeError):
            clients.discard(w)


async def read_pane_ansi(pane_id: str, lines: int = PANE_LINES) -> str:
    """Tela fiel, com SGR (cores/estilos), para o pane_content do painel.

    O emulador interno do Herdr é o motor do Ghostty; format:"ansi" re-emite o
    grid já emulado com os estilos por célula — é a única leitura que a ponte
    faz, porque o painel mostra a sessão como ela é em vez de interpretá-la.

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


def _resolve_transcript_path(p: dict, acc: tuple) -> str | None:
    """Transcript do Claude Code para o pane `p`, na conta `acc` (agent, config_dir).

    Por UUID (`agent_session`) quando o Herdr o expõe; senão pelo cwd, achando
    o dir de projects/ e pegando o .jsonl mais recente por mtime (sessão
    ativa). Extraída de push_agents porque a mesma resolução alimenta agora
    também _session_paths()/aggregate_cost — glob e stat são baratos o
    bastante para rodar direto no event loop (nunca abrir/ler o jsonl aqui).
    """
    sess = (p.get("agent_session") or {}).get("value")
    if sess:
        hits = glob.glob(os.path.join(acc[1], "projects", "*", sess + ".jsonl"))
        return hits[0] if hits else None
    cwd = p.get("cwd")
    if not cwd:
        return None
    enc = transcript.encode_cwd(cwd)
    hits = glob.glob(os.path.join(acc[1], "projects", enc, "*.jsonl"))
    if not hits:
        return None
    try:
        return max(hits, key=os.path.getmtime)
    except OSError:
        return None  # arquivo sumiu entre o glob e o stat: ignora este ciclo


def _session_paths() -> list:
    """Transcripts dos panes claude ativos na última rodada de push_agents.

    Não faz I/O: só devolve o mapa pane→transcript já resolvido (mesma
    resolução de _resolve_transcript_path). Chamada direto do event loop pela
    coleta de custo, antes de agregar (que aí sim vai para o executor).
    """
    return list(pane_paths.values())


async def push_agents() -> set[str] | None:
    """Reconcilia com o Herdr; envia aos painéis só o que de fato mudou.

    Retorna os pane_ids de TODOS os panes (com e sem agente) para o event_loop
    saber quando reassinar, ou None se o Herdr não respondeu — que não é motivo
    para reassinar.
    """
    global last_snapshot, working_since, pane_paths
    result = await herdr_request("pane.list")
    if not result:
        return None
    panes = result.get("panes", [])
    # Composição de panes (todos, não só os com agente) chaveada por
    # (pane_id, agent): é ela que decide troca de conta (login/logout, pane
    # novo de outra conta, OU o mesmo pane trocando de agente — o pid do SO
    # muda mesmo com o pane_id estável), então o refresh roda sobre `panes`
    # inteiro, não sobre a lista já filtrada de agentes.
    new_set = {(p["pane_id"], p.get("agent")) for p in panes}
    if new_set != _prev_agent_panes[0]:
        _prev_agent_panes[0] = new_set
        await refresh_accounts(panes)
    # O carimbo vai como epoch absoluto, e não como duração: o broadcast só sai
    # quando o snapshot serializado muda (adiante), então uma duração mudaria a
    # cada segundo e faria a ponte transmitir sem parar — e o painel reconstruir
    # a lista inteira junto. Absoluto, fica imutável enquanto o agente trabalha.
    now = time.time()
    new_since = {}
    agents = []
    # Fase A (no loop, barato): monta os agentes e, para cada pane claude
    # elegível, resolve o path do transcript por glob e faz só o stat
    # (getmtime é barato — abrir e ler o jsonl inteiro é que não pode rodar
    # aqui). pane_path guarda o candidato deste ciclo; to_read só os misses
    # (path ausente do cache ou com mtime diferente).
    pane_path: dict = {}   # pane_id -> transcript path (candidatos deste ciclo)
    to_read: dict = {}     # path -> mtime (misses a reler no executor)
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
        acc = pane_account.get(pid)
        if acc:
            a["account"] = acc_email_cache.get(acc, "")
            org, corp = accounts.org_and_corp(a["account"])
            if org:
                a["org"], a["corp"] = org, corp
        path = None
        if p.get("agent") == "claude" and acc:
            path = _resolve_transcript_path(p, acc)
        if path:
            try:
                mt = os.path.getmtime(path)
            except OSError:
                mt = None  # arquivo sumiu entre o glob e o stat: ignora este ciclo
            if mt is not None:
                pane_path[pid] = path
                cached = tx_cache.get(path)
                if cached is None or cached[0] != mt:
                    to_read[path] = mt
        if status == "working":
            # só o primeiro ciclo em working cria o carimbo; os seguintes o herdam
            new_since[pid] = working_since.get(pid, now)
            a["since"] = int(new_since[pid])
        agents.append(a)

    # Fase B (executor): só os transcripts que mudaram de mtime, fora do event
    # loop — ler um jsonl inteiro é I/O bloqueante, o mesmo cuidado do
    # _read_env_filtered/refresh_accounts do multi-conta.
    if to_read:
        loop = asyncio.get_running_loop()
        fresh = await loop.run_in_executor(
            None, lambda: {pth: (mt, transcript.session_metrics(pth))
                           for pth, mt in to_read.items()})
        tx_cache.update(fresh)

    # Fase C (no loop): anexa model/context_pct/effort a partir do cache já
    # quente. O effort só entra quando existe (CLI >= 2.1.234): mandar a chave
    # vazia engordaria o payload de todo mundo que roda CLI anterior.
    for a in agents:
        path = pane_path.get(a["pane_id"])
        cached = tx_cache.get(path) if path else None
        m = cached[1] if cached else None
        if m:
            a["model"] = m["model"]
            a["context_pct"] = m["context_pct"]
            if m["effort"]:
                a["effort"] = m["effort"]

    # Poda: só ficam no cache os transcripts de panes vivos neste ciclo —
    # sem isso ele cresceria sem limite conforme sessões (panes) fecham.
    live_paths = set(pane_path.values())
    for k in list(tx_cache):
        if k not in live_paths:
            tx_cache.pop(k, None)
    pane_paths = pane_path  # publica o mapa desta rodada para _session_paths()

    # reconstruir em vez de mutar poda de graça quem saiu de working ou sumiu
    working_since = new_since
    known_panes.clear()
    known_panes.update(a["pane_id"] for a in agents)
    snapshot = json.dumps({"type": "agents", "agents": agents}, separators=(",", ":"))
    if snapshot != last_snapshot:
        last_snapshot = snapshot
        await broadcast_line(snapshot)
    return {p["pane_id"] for p in panes}


async def poll_loop() -> None:
    """Reconciliação pura, para quando o transporte é a CLI (ver USE_CLI).

    A CLI não assina eventos, então aqui não há o disparo imediato do
    event_loop — só o piso de latência do `pane.list`. Na prática a diferença é
    menor do que parece: o Herdr 0.8.0 já não emite evento nas transições de
    agent_status (é derivado do conteúdo do terminal), então mesmo com socket
    quem garante o teto é esta mesma reconciliação. O que se perde é a reação
    instantânea a pane criado/fechado.
    """
    while True:
        await push_agents()
        await asyncio.sleep(RECONCILE_BUSY_S if clients else RECONCILE_IDLE_S)


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


def collect_claude(config_dir: str) -> dict:
    """Uso do Claude Code perguntando ao próprio CLI (ver claude_cli).

    Antes isto lia `.credentials.json` e batia no endpoint OAuth. O CLI guarda
    a credencial onde cada SO manda e renova sozinho: no macOS passou a usar o
    Keychain e o arquivo congelou expirado, derrubando a coleta sem aviso.
    Perguntar ao CLI vale em todo sistema, não replica autenticação nenhuma e
    acompanha a sessão que o usuário já mantém viva dentro do Herdr.

    Duas chamadas por conta (~3s no total, bloqueantes — collect_limits roda no
    executor): `auth status` dá login/e-mail/plano, `/usage` dá as janelas.
    Conta sem assinatura logada levanta NotLoggedIn e some do painel.
    """
    st = claude_cli.auth_status(config_dir)
    if not st.get("loggedIn"):
        raise claude_cli.NotLoggedIn(config_dir)
    rows = claude_cli.usage_rows(config_dir)
    # tier detalhado do perfil ("Max 20x"); sem ele, o tipo cru do CLI ("Max")
    plan = (accounts.plan_label(accounts.read_account_tier(config_dir, HOME))
            or (st.get("subscriptionType") or "").capitalize())
    account = st.get("email") or read_account_email("claude", config_dir, HOME)
    return {"name": "Claude", "plan": plan, "limits": rows,
            "account": clip(account, 32)}


def collect_codex(config_dir: str) -> dict:
    """Uso do Codex pelo backend do ChatGPT, com o token que o CLI renova.

    Atenção ao reset_after_seconds: muda a cada segundo e por isso NÃO entra
    na saída — só o reset_at (epoch fixo), senão o dedup do snapshot morre.
    `config_dir` é o CODEX_HOME da conta (default ou não).
    """
    with open(os.path.join(config_dir, "auth.json")) as f:
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
                     "resets_at": round_min(win.get("reset_at") or 0),
                     "window_s": int(secs)})
    return {"name": "Codex", "plan": (data.get("plan_type") or "").capitalize(),
            "limits": rows,
            "account": clip(read_account_email("codex", config_dir, HOME), 32)}


def collect_limits(account_dirs) -> list:
    """Monta a lista de provedores, um por conta, degradando com honestidade.

    `account_dirs` é um iterável de (agent, config_dir) — as contas descobertas
    nos panes ativos (ver accounts.discover). Três estados por provedor:
    credencial ausente → omitido (CLI não instalado/conta sem login nesta
    máquina); falha com sucesso anterior → últimos valores com ok=false e
    stale_since do último sucesso; falha sem histórico → omitido. A ordenação
    por (agent, config_dir) é o que garante determinismo — a serialização
    precisa ser estável para o dedup do snapshot funcionar. Corta em
    LIMITS_MAX_CARDS: o firmware tem um teto fixo de cards por painel.
    """
    collectors = {"claude": collect_claude, "codex": collect_codex}
    providers = []
    for agent, cdir in sorted(account_dirs):
        key = (agent, cdir)
        # o claude não tem arquivo para checar: quem responde se a conta está
        # logada é o CLI, e a resposta chega como NotLoggedIn abaixo
        if agent != "claude" and not os.path.exists(_cred_path(agent, cdir)):
            limits_last_good.pop(key, None)
            limits_ok.pop(key, None)
            continue
        try:
            cur = collectors[agent](cdir)
            cur.update(ok=True, stale_since=0)
            limits_last_good[key] = {"data": {k: cur[k] for k in
                                     ("name", "plan", "limits", "account")},
                                     "at": int(time.time())}
            # _config_dir é interno: viaja com o provedor só para o limits_loop
            # casar com pane_account (agent kind + config_dir) e chamar
            # enrich_provider; nunca vai para o payload (pop antes do broadcast).
            cur["_config_dir"] = cdir
            providers.append(cur)
            if limits_ok.get(key) is not True:
                log.info("limites %s [%s]: ok (%d janelas)",
                         agent, cur.get("account") or os.path.basename(cdir),
                         len(cur["limits"]))
            limits_ok[key] = True
        except claude_cli.NotLoggedIn:
            limits_last_good.pop(key, None)
            limits_ok.pop(key, None)
            continue
        except Exception as e:  # endpoint interno: qualquer surpresa é falha, não crash
            reason = ("HTTP %d" % e.code if isinstance(e, urllib.error.HTTPError)
                      else str(e) or type(e).__name__)
            good = limits_last_good.get(key)
            if good:
                stale = dict(good["data"])
                stale.update(ok=False, stale_since=good["at"], _config_dir=cdir)
                providers.append(stale)
            if limits_ok.get(key) is not False:
                log.warning("limites %s [%s]: %s", agent, os.path.basename(cdir), reason)
            limits_ok[key] = False
    if len(providers) > LIMITS_MAX_CARDS:
        log.warning("cards de uso acima do teto (%d>%d): cortando",
                    len(providers), LIMITS_MAX_CARDS)
        providers = providers[:LIMITS_MAX_CARDS]
    return providers


# name do provedor (collect_claude/collect_codex) -> kind usado em pane_account
_PROVIDER_KIND = {"Claude": "claude", "Codex": "codex"}


def enrich_provider(prov, agents, working):
    """Anexa org/corp (do account) e contagem de agentes ao provedor."""
    org, corp = accounts.org_and_corp(prov.get("account", ""))
    if org:
        prov["org"], prov["corp"] = org, corp
    prov["agents"], prov["agents_working"] = int(agents), int(working)
    return prov


def _file_cost_cached(path):
    """Custo de um transcript, cacheado por (mtime, size) — só relê se cresceu.

    Bloqueante (abre e lê o jsonl inteiro via cost.file_cost): só chamar de
    dentro do executor, nunca do event loop — o mesmo cuidado dos demais
    caches de I/O desta ponte (tx_cache, pid_env_cache).
    """
    try:
        st = os.stat(path)
    except OSError:
        return None
    key = (st.st_mtime, st.st_size)
    ent = cost_cache.get(path)
    if not ent or (ent[0], ent[1]) != key:
        m = cost.file_cost(path)
        cost_cache[path] = (key[0], key[1], m or {"total": 0.0, "days": {}})
    return cost_cache[path][2]


def aggregate_cost(account_dirs, session_paths) -> dict:
    """Custo agora/semana/vitalício, formatados (ver cost.fmt_usd).

    Bloqueante (varredura de todos os transcripts das contas claude para o
    vitalício): só rodar no executor, nunca no event loop. `now` soma só os
    transcripts de `session_paths` — os panes claude ativos agora, via
    _session_paths(); `week` soma os dias da janela [hoje-6..hoje] de TODOS os
    transcripts das contas claude descobertas; `life` é o total de todos eles.
    """
    seen = set()
    life = 0.0
    week = 0.0
    today = datetime.date.today()
    window = {(today - datetime.timedelta(days=i)).isoformat() for i in range(7)}
    for agent, cdir in account_dirs:
        if agent != "claude":
            continue
        for p in glob.glob(os.path.join(cdir, "projects", "*", "*.jsonl")):
            if p in seen:
                continue
            seen.add(p)
            m = _file_cost_cached(p)
            if not m:
                continue
            life += m["total"]
            for day, c in m["days"].items():
                if day in window:
                    week += c
    now = 0.0
    for p in session_paths:                 # transcripts dos panes claude ativos
        m = _file_cost_cached(p)
        if m:
            now += m["total"]
    # Centavos são o valor de verdade: quem formata é o painel, que conhece o
    # idioma configurado (a ponte não). As strings continuam indo junto para os
    # painéis anteriores a este campo, que só sabem exibir texto pronto.
    return {"now": cost.fmt_usd(now), "week": cost.fmt_usd(week),
            "life": cost.fmt_usd(life),
            "now_cents": cost.to_cents(now), "week_cents": cost.to_cents(week),
            "life_cents": cost.to_cents(life)}


async def limits_loop() -> None:
    """Coleta uso de limites e custo, difundindo cada um com seu próprio dedup.

    Passo curto com contabilização própria em vez de dormir o ciclo inteiro:
    um painel que conecta depois de ociosidade espera LIMITS_STEP_S pelo
    primeiro dado, não LIMITS_POLL_S. Sem painel conectado, nada é coletado.
    Custo entra no mesmo ciclo de limites (mesma cadência LIMITS_POLL_S): não
    há motivo para um laço próprio só para variar o passo.
    """
    global last_limits_snapshot, last_cost_snapshot
    last = None   # sentinela: a base do monotonic varia por plataforma
    while True:
        if clients and (last is None or time.monotonic() - last >= LIMITS_POLL_S):
            last = time.monotonic()
            loop = asyncio.get_running_loop()
            providers = await loop.run_in_executor(
                None, collect_limits, set(account_dirs))
            # org/corp + contagem de agentes por conta: casa cada provedor com
            # os panes de pane_account cujo (agent kind, config_dir) bate com
            # o de onde o provedor coletou (_config_dir, ver collect_limits).
            for prov in providers:
                kind = _PROVIDER_KIND.get(prov.get("name"), "")
                cdir = prov.pop("_config_dir", None)
                acc_key = (kind, cdir)
                agents = sum(1 for acc in pane_account.values() if acc == acc_key)
                working = sum(1 for pid, acc in pane_account.items()
                              if acc == acc_key and pid in working_since)
                enrich_provider(prov, agents, working)
            snapshot = json.dumps({"type": "limits", "providers": providers},
                                  separators=(",", ":"))
            if snapshot != last_limits_snapshot:
                last_limits_snapshot = snapshot
                await broadcast_line(snapshot)

            costs = await loop.run_in_executor(
                None, aggregate_cost, set(account_dirs), _session_paths())
            cost_snapshot = json.dumps({"type": "cost", **costs},
                                       separators=(",", ":"))
            if cost_snapshot != last_cost_snapshot:
                last_cost_snapshot = cost_snapshot
                await broadcast_line(cost_snapshot)
        await asyncio.sleep(LIMITS_STEP_S)


async def handle_command(msg: dict, writer: asyncio.StreamWriter) -> None:
    kind = msg.get("type")
    pane_id = msg.get("pane_id", "")

    async def deny(reason: str) -> None:
        writer.write((json.dumps({"type": "error", "message": reason}) + "\n").encode())
        await writer.drain()

    if kind in ("read_pane", "send_keys", "send_text", "focus",
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


def avatar_repos() -> list:
    """URLs de repositório de avatar do config-dir; [] se o arquivo não existe.

    Ausência é o caminho normal: só quem publica avatares cria este arquivo.
    """
    try:
        with open(AVATAR_REPOS_FILE, encoding="utf-8") as fh:
            urls = [ln.strip() for ln in fh]
    except OSError:
        return []
    return [u for u in urls if u and not u.startswith("#")][:AVATAR_REPOS_MAX]


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
        if last_cost_snapshot:    # custo: mesmo trio cache/push/entrega
            writer.write((last_cost_snapshot + "\n").encode())
            await writer.drain()
        # Repositórios de avatar: sempre enviados, inclusive vazios — lista
        # vazia é a ponte dizendo "não tenho nenhum", e o painel precisa disso
        # para esquecer os da conexão anterior.
        writer.write((json.dumps({"type": "avatar_repos", "repos": avatar_repos()},
                                 separators=(",", ":")) + "\n").encode())
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


class DiscoveryResponder(asyncio.DatagramProtocol):
    """Responde probes herdr-find de painéis com slot em descoberta automática.

    O probe é broadcast e o nonce é público; a resposta prova posse do token
    sem transportá-lo — h = HMAC-SHA256(token, "nonce|ip|port|name") — e
    assina o ip/port que o painel deve adotar (nunca o remetente do datagrama,
    para um vizinho não conseguir redirecionar o painel por relay).
    Só escuta broadcast de verdade com BRIDGE_BIND no default 0.0.0.0.
    """

    RATE_MAX = 10       # respostas por janela de 1s (higiene anti-amplificação)
    DEDUPE_S = 0.3      # probe duplicado (255.255.255.255 + subnet) responde 1x;
                        # o reenvio legítimo do painel chega a ~400ms e passa

    def __init__(self) -> None:
        self.transport: asyncio.DatagramTransport | None = None
        self.seen: dict[tuple[str, str], float] = {}
        self.window = 0.0
        self.count = 0
        self.limited = False    # anti-spam: warning só na transição
        self.replied: set[tuple[str, str]] = set()  # (id, ip) já logados

    def connection_made(self, transport: asyncio.BaseTransport) -> None:
        self.transport = transport  # type: ignore[assignment]

    def datagram_received(self, data: bytes, addr: tuple[str, int]) -> None:
        try:
            msg = json.loads(data)
        except (json.JSONDecodeError, UnicodeDecodeError):
            return  # broadcast é tráfego público: lixo não merece log
        if not (isinstance(msg, dict) and msg.get("t") == "herdr-find"):
            return
        nonce = str(msg.get("n", ""))
        panel = str(msg.get("id", "?"))
        if not nonce:
            return
        now = time.monotonic()
        key = (panel, nonce)
        if now - self.seen.get(key, 0.0) < self.DEDUPE_S:
            return
        self.seen[key] = now
        if len(self.seen) > 64:  # aparar entradas velhas de vez em quando
            self.seen = {k: t for k, t in self.seen.items() if now - t < 2.0}
        if now - self.window >= 1.0:
            self.window, self.count = now, 0
            self.limited = False
        self.count += 1
        if self.count > self.RATE_MAX:
            if not self.limited:
                self.limited = True
                log.warning("descoberta: limite de respostas atingido, silenciando")
            return
        # IP desta máquina na rota até o painel (mesmo truque do admin.py);
        # é o campo que o painel adota, então precisa ser o da interface certa
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        try:
            s.connect((addr[0], 9))
            my_ip = s.getsockname()[0]
        except OSError:
            return
        finally:
            s.close()
        name = socket.gethostname().split(".")[0][:15]
        canon = "%s|%s|%d|%s" % (nonce, my_ip, PORT, name)
        h = hmac.new(TOKEN.encode(), canon.encode(), "sha256").hexdigest()
        reply = json.dumps({"t": "herdr-here", "name": name, "ip": my_ip,
                            "port": PORT, "h": h}, separators=(",", ":"))
        if self.transport is not None:
            self.transport.sendto(reply.encode(), addr)
        if (panel, addr[0]) not in self.replied:
            self.replied.add((panel, addr[0]))
            log.info("descoberta: sonda do painel %s respondida para %s", panel, addr[0])


async def main() -> None:
    global TOKEN
    if USE_CLI:
        if not HERDR_BIN or not shutil.which(HERDR_BIN) and not os.path.isfile(HERDR_BIN):
            log.error("binário do herdr não encontrado (%s)", HERDR_BIN)
            sys.exit(1)
        log.info("falando com o Herdr pela CLI (%s)", HERDR_BIN)
    elif not os.path.exists(SOCK):
        log.error("socket do Herdr não encontrado: %s", SOCK)
        sys.exit(1)
    TOKEN = load_token()
    asyncio.create_task(poll_loop() if USE_CLI else event_loop())
    asyncio.create_task(limits_loop())
    # porta UDP ocupada não pode derrubar a ponte: sem descoberta ela ainda
    # serve os painéis que já sabem o endereço
    try:
        await asyncio.get_running_loop().create_datagram_endpoint(
            DiscoveryResponder, local_addr=(BIND, PORT))
        log.info("descoberta udp em %s:%d", BIND, PORT)
    except OSError as exc:
        log.warning("descoberta indisponível (udp %s:%d: %s)", BIND, PORT, exc)
    server = await asyncio.start_server(handle_client, BIND, PORT)
    log.info("ponte ouvindo em %s:%d", BIND, PORT)
    async with server:
        await server.serve_forever()


if __name__ == "__main__":
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        pass
