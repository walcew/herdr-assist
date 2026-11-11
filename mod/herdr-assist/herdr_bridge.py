#!/usr/bin/env python3
# /// script
# requires-python = ">=3.9"
# ///
"""Ponte entre o herdr-core (Herdr desktop) e o painel herdr-assist na LAN.

O core expõe sua API pelo `hc connect` (JSON-RPC 2.0, uma mensagem por linha
em stdin/stdout), que um dispositivo na rede não alcança. Esta ponte traduz:
assina os eventos do core (events.subscribe) e serve o mesmo conteúdo por TCP.
Cada mudança de terminal dispara o envio na hora; não há polling.

Sem dependências externas: só a stdlib, compatível com o python3 de fábrica do
macOS (3.9). Para rodar avulsa:

    BRIDGE_TOKEN=... python3 mod/herdr-assist/herdr_bridge.py

Toda conexão exige um handshake `{"type":"hello","token":"..."}` na primeira
linha — sem token não roda: se nenhum existir, um é gerado e salvo (0600).

Variáveis: HC_BIN (hc), HERDR_APP_BIN (herdr-app), BRIDGE_PORT (9375),
BRIDGE_BIND (0.0.0.0), BRIDGE_TOKEN, BRIDGE_MACHINES (máquinas SSH salvas do
Herdr TUI a incluir: vazio, "all" ou lista).
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
import rollout
import transcript
import update
from accounts import read_account_email, default_dir, CONFIG_VAR  # noqa: E402
from proc_env import read_process_env  # noqa: E402

# Binários do Herdr desktop: a variável vence (é o que injeta o hc falso nos
# testes), senão o PATH, senão onde o instalador os põe.
HC_BIN = (os.environ.get("HC_BIN") or shutil.which("hc")
          or os.path.expanduser("~/.local/bin/hc"))
HERDR_APP_BIN = (os.environ.get("HERDR_APP_BIN") or shutil.which("herdr-app")
                 or os.path.expanduser("~/.local/bin/herdr-app"))
PORT = int(os.environ.get("BRIDGE_PORT", "9375"))
BIND = os.environ.get("BRIDGE_BIND", "0.0.0.0")
CONFIG_DIR = os.environ.get("HERDR_PLUGIN_CONFIG_DIR",
                            os.path.expanduser("~/.config/herdr-assist"))
TOKEN_FILE = os.path.join(CONFIG_DIR, "token")
TOKEN = ""  # carregado em main()

# Máquinas SSH salvas no Herdr que a ponte também deve mostrar. O servidor
# local só conhece os próprios panes; o cliente do Herdr junta várias máquinas
# na tela, e é isso que o painel deve espelhar. Vazio = só o servidor local;
# "all" = todas as habilitadas; ou rótulos/ids separados por vírgula.
# Nada é instalado na máquina remota: o próprio Herdr usa o perfil salvo
# (`herdr --machine <id> ...`) para falar com ela.
MACHINES = os.environ.get("BRIDGE_MACHINES", "").strip()
REMOTE_POLL_S = 5.0
remote_agents: list[dict] = []          # última leitura das máquinas remotas
remote_panes: dict = {}                 # "HVM/w7:p1" -> (profile_id, "w7:p1")

# Repositórios de avatar que o painel deve varrer além do padrão de fábrica:
# uma URL base por linha, "#" comenta. Fica no config-dir, ao lado do token —
# o que sob o plugin do Herdr é ~/.config/herdr/plugins/config/herdr-assist/,
# e não o default de baixo (esse só vale para a ponte rodando avulsa).
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
# Texto do pane: PANE_LINES por padrão; o corte (CONTENT_MAX_B) e o teto do
# JSON no fio (WIRE_MAX_B) existem porque o RX do firmware é de tamanho fixo.
PANE_LINES = 40
CONTENT_MAX_B = 12000
WIRE_MAX_B = 20000
# Nome da tecla do painel -> nome do `hc keys`; as demais só mudam a caixa.
HC_KEYS = {"Escape": "esc", "C-c": "ctrl+c", "BSpace": "backspace"}

# CLI do Herdr TUI: só o que ainda não migrou (máquinas SSH, update.check).
HERDR_BIN = (os.environ.get("HERDR_BIN") or shutil.which("herdr")
             or "/opt/homebrew/bin/herdr")

# Eventos do core que podem mudar a lista de agentes; os demais (size, turn,
# title, doc, keep) não mexem em nada que o painel recebe.
REFRESH_EVENTS = {"created", "lost", "restarted", "finished", "removed",
                  "status", "labels", "foreground"}
# Nome do app no status do core -> nome que o firmware conhece ("CC"/"CX").
AGENT_NAMES = {"claude-code": "claude"}

# Uso de limites dos provedores de IA. As credenciais são as que os próprios
# CLIs mantêm renovadas no disco — nada se configura aqui, e nenhum token sai
# desta máquina: só percentuais derivados atravessam a LAN. Arquivo ausente
# significa "CLI não instalado", não erro.
LIMITS_POLL_S = 60.0     # cadência da coleta com painel conectado
LIMITS_STEP_S = 5.0      # passo do laço: painel que conecta espera pouco

# Atualização da ponte (ver update.py). O passo curto relê o manifesto local,
# que é um open + regex; o longo é o que sai na rede atrás do release. Mesma
# divisão do limits_loop, e pelo mesmo motivo: um evento barato não precisa
# esperar a cadência do caro.
UPDATE_DELAY_S = 60.0            # respiro antes da primeira ida à rede
UPDATE_STEP_S = 60.0
UPDATE_POLL_S = 24 * 60 * 60.0   # diária, como a do painel (fw_update.c)
UPDATE_TRIES = 3                 # reinícios antes de desistir e gritar no log
BOOT_VERSION = ""                # versão em disco quando este processo subiu
LIMITS_HTTP_TIMEOUT = 10
LIMITS_MAX_CARDS = 8     # teto espelhado no firmware (HERDR_MAX_PROVIDERS)
HOME = os.path.expanduser("~")  # injetável nos testes


def _cred_path(agent: str, config_dir: str) -> str:
    """Caminho da credencial do agente dentro do config-dir da conta."""
    return os.path.join(config_dir, ".credentials.json" if agent == "claude"
                        else "auth.json")

clients: set[asyncio.StreamWriter] = set()
known_panes: set[str] = set()
terminals: dict = {}     # endereço (t1m) -> Terminal do core, como veio dele
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
# Codex: rollout escolhido por pane, SEPARADO de pane_paths de propósito —
# _session_paths() alimenta a agregação de custo, que só entende transcript de
# Claude; um rollout ali contaria custo errado em silêncio. O cache de headers
# guarda o cwd da 1ª linha de cada candidato (imutável após criado): é ele que
# permite escolher o rollout no event loop sem abrir arquivo nenhum — headers
# novos são lidos no executor e valem a partir do ciclo seguinte.
codex_paths: dict = {}         # pane_id -> rollout path
rollout_meta_cache: dict = {}  # rollout path -> cwd do session_meta

# Custo (ver cost.py): cache por (mtime, size) — só relê um transcript se ele
# cresceu — e snapshot para o dedup do broadcast "cost", nos mesmos moldes de
# tx_cache/last_limits_snapshot.
cost_cache: dict = {}        # path -> (mtime, size, {"total","days"})
last_cost_snapshot = ""      # dedup do payload cost


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

    Chamada só quando a composição de panes (pane_id, agent, pid) muda (ver
    push_agents), não a cada evento. O pid vem do foreground do terminal no
    core, já presente no snapshot. Todo I/O bloqueante (leitura de ambiente
    por pid) roda num executor: o event loop não pode travar enquanto
    ctypes/`/proc`/`ps` respondem.
    """
    global pane_account, account_dirs, acc_email_cache

    targets = [p for p in panes if p.get("agent") in CONFIG_VAR]
    pane_pid_cache.clear()
    pane_pid_cache.update({(p["pane_id"], p.get("agent")): p.get("pid")
                           for p in targets})
    pids = {p["pane_id"]: p.get("pid") for p in targets}

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


# Sem isto cada chamada pisca um console: a ponte roda destacada, sem console
# próprio, e um flash por segundo seria insuportável.
NO_WINDOW = 0x08000000 if os.name == "nt" else 0


async def cli_run(args: list[str], timeout: float = 10, binary: str = "") -> str | None:
    """Roda o herdr (ou `binary`) e devolve o stdout. None em qualquer falha.

    Decodifica aqui, em utf-8: deixar o Python decidir pelo locale (cp1252 no
    Windows) estoura na primeira caixa ou seta que a tela do terminal trouxer,
    e a saída volta vazia sem erro visível.
    """
    try:
        proc = await asyncio.create_subprocess_exec(
            binary or HERDR_BIN, *args,
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


def _session_from_pid(p: dict, acc: tuple):
    """sessionId do registro <config-dir>/sessions/<pid>.json do Claude Code.

    O CC escreve esse arquivo ao subir (pid, sessionId, cwd...) e o remove ao
    sair — pid→sessão determinístico, imune ao desempate por mtime. O pid vem
    do foreground do terminal (pane_pid_cache): se o registro existe para ele,
    foi o claude vivo desse pane que o escreveu, então não precisa de
    validação extra. Falha de forma limpa quando o foreground do pane é um
    FILHO do claude (um tool em execução no instante da resolução do pid) —
    não há registro para o filho e a cascata segue. É formato interno do CC,
    não documentado — mais um motivo para ser a primeira tentativa, nunca a
    única. ~500 bytes de tamanho fixo: leitura
    barata o bastante para o event loop, ao contrário do transcript.
    """
    pid = pane_pid_cache.get((p["pane_id"], p.get("agent")))
    if not pid:
        return None
    try:
        with open(os.path.join(acc[1], "sessions", str(pid) + ".json"),
                  encoding="utf-8") as fh:
            rec = json.load(fh)
    except (OSError, ValueError):
        return None
    return rec.get("sessionId") if isinstance(rec, dict) else None


def _resolve_transcript_path(p: dict, acc: tuple) -> str | None:
    """Transcript do Claude Code para o pane `p`, na conta `acc` (agent, config_dir).

    Pelo pid (registro sessions/ que o próprio CC mantém — ver
    _session_from_pid); senão por UUID (`agent_session`) quando o Herdr o
    expõe; senão pelo cwd, achando o dir de projects/ e pegando o .jsonl mais
    recente por mtime (sessão ativa). O desempate por mtime é o elo fraco dos
    fallbacks: duas sessões no mesmo diretório se confundem, e qualquer jsonl
    recém-gravado ali rouba a vez. Extraída de push_agents porque a mesma
    resolução alimenta também _session_paths()/aggregate_cost — glob e stat
    são baratos o bastante para rodar direto no event loop (nunca abrir/ler o
    jsonl aqui).
    """
    sess = _session_from_pid(p, acc) or (p.get("agent_session") or {}).get("value")
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


def _resolve_rollout_path(p: dict, acc: tuple, to_meta: set, cands_out: set):
    """Rollout do Codex para o pane `p`: candidato de cwd casado ou, sem um,
    o path que o pane já tinha (sessão mais velha que a janela de descoberta).

    Sem I/O de conteúdo aqui: a escolha usa só rollout_meta_cache; candidato
    com header ainda desconhecido entra em `to_meta` para a fase B ler no
    executor e concorre a partir do ciclo seguinte — um ciclo de atraso, só na
    primeira vez que o arquivo aparece.
    """
    cwd = p.get("cwd")
    if not cwd:
        return None
    cands = rollout.recent_paths(acc[1])
    cands_out.update(pth for pth, _ in cands)
    to_meta.update(pth for pth, _ in cands if pth not in rollout_meta_cache)
    return rollout.pick(cands, cwd, rollout_meta_cache) or codex_paths.get(p["pane_id"])


def _session_paths() -> list:
    """Transcripts dos panes claude ativos na última rodada de push_agents.

    Não faz I/O: só devolve o mapa pane→transcript já resolvido (mesma
    resolução de _resolve_transcript_path). Chamada direto do event loop pela
    coleta de custo, antes de agregar (que aí sim vai para o executor).
    """
    return list(pane_paths.values())


def friendly_name(label: str, cwd: str) -> str:
    """Nome mostrado na linha: o rótulo da aba do Herdr, senão o basename do cwd.

    O rótulo é o que a pessoa vê no Herdr ("OMP", "agent-claude", "agent-codex");
    o basename ("tmp", "luispoveda93") não diz nada a quem olha a tela. Aba sem
    nome vem como número ("1") e aí o cwd ainda é melhor que "1".
    """
    label = (label or "").strip()
    if label and not label.isdigit():
        return label
    return os.path.basename(cwd or "")


def machine_code(label: str) -> str:
    """Rótulo curto (3 letras) que caiba na linha do painel: "Hermes VM" -> HVM."""
    words = [w for w in re.split(r"[^A-Za-z0-9]+", label or "") if w]
    if not words:
        return "REM"
    if len(words) == 1:
        return words[0][:3].upper()
    return "".join(w[0] for w in words)[:3].upper()


async def refresh_machines() -> None:
    """Relê os agentes das máquinas salvas e troca o cache de uma vez.

    O id do pane vai com o código da máquina na frente ("HVM/w7:p1") porque
    w7:p1 existe nos dois servidores — e é por ele que a ponte sabe para onde
    mandar focus/send_keys depois.
    """
    global remote_agents, remote_panes
    if not MACHINES:
        return
    out = await cli_run(["machine", "list", "--json"], timeout=8)
    if not out:
        return
    try:
        profiles = json.loads(out)
    except json.JSONDecodeError:
        return
    wanted = {w.strip().lower() for w in MACHINES.split(",") if w.strip()}
    picked = []
    for p in profiles:
        if not p.get("enabled"):
            continue
        if "all" not in wanted:
            names = {(p.get("label") or "").lower(), (p.get("id") or "").lower()}
            if not (names & wanted):
                continue
        picked.append(p)

    agents: list[dict] = []
    pane_map: dict = {}
    ok = 0
    for p in picked:
        code = machine_code(p.get("label") or p.get("id") or "")
        # api snapshot traz agentes e abas numa chamada só (uma ida SSH por
        # máquina por ciclo); o rótulo da aba é o nome amigável do agente
        data = await cli_run(["--machine", p["id"], "api", "snapshot"], timeout=15)
        if not data:
            continue                      # máquina fora do ar: mantém o resto
        try:
            snap = json.loads(data).get("result", {}).get("snapshot", {}) or {}
        except (json.JSONDecodeError, AttributeError):
            continue
        if not snap:
            continue
        ok += 1
        labels = {t.get("tab_id"): (t.get("label") or "")
                  for t in snap.get("tabs", [])}
        for a in snap.get("agents", []):
            raw = a.get("pane_id")
            if not raw:
                continue
            name = friendly_name(labels.get(a.get("tab_id"), ""), a.get("cwd") or "")
            ns = f"{code}/{raw}"
            pane_map[ns] = (p["id"], raw)
            agents.append({
                "pane_id": ns,
                "agent": a.get("agent", ""),
                "status": a.get("agent_status", "unknown"),
                "project": f"{code}/{name}",
                "workspace_id": a.get("workspace_id", ""),
                "machine": p.get("label", ""),
            })
    if picked and ok == 0:
        # nenhuma máquina respondeu: é queda de SSH/rede, não "acabaram os
        # agentes" — manter o último retrato evita a tela esvaziar sozinha
        return
    if agents != remote_agents:
        log.info("máquinas: %d agente(s) de %d máquina(s)", len(agents), len(picked))
    remote_agents = agents
    remote_panes = pane_map


async def machines_loop() -> None:
    while True:
        try:
            await refresh_machines()
        except Exception as exc:          # SSH/rede nunca derruba a ponte
            log.warning("máquinas: %s", exc)
        await asyncio.sleep(REMOTE_POLL_S)


def agent_panes() -> list:
    """Os agentes do core no formato de pane que o resto da ponte já usa.

    Agente = terminal vivo que reporta status. Ficam de fora a própria ponte
    (rótulo herdr-assist=bridge) e os terminais de outras máquinas
    (`machine/t7`), que a v1 não espelha. Space e tab vêm do rótulo `where`
    ("space / tab"), e o pid é o do processo em primeiro plano.
    """
    panes = []
    for addr, t in sorted(terminals.items()):
        st = t.get("status") or {}
        labels = t.get("labels") or {}
        if (t.get("state") != "live" or not st or "/" in addr
                or labels.get("herdr-assist") == "bridge"):
            continue
        space, _, tab = (labels.get("where") or "").partition(" / ")
        fg = t.get("foreground") or {}
        panes.append({"pane_id": addr,
                      "agent": AGENT_NAMES.get(st.get("app"), st.get("app") or ""),
                      "agent_status": st.get("state") or "unknown",
                      "cwd": fg.get("cwd") or t.get("cwd") or "",
                      "space": space, "tab": tab, "pid": fg.get("pid")})
    return panes


async def push_agents() -> None:
    """Monta `agents` a partir do estado do core; envia só o que de fato mudou."""
    global last_snapshot, working_since, pane_paths
    panes = agent_panes()
    # Composição chaveada por (pane_id, agent, pid): é ela que decide troca de
    # conta (login/logout, terminal novo de outra conta, OU o mesmo terminal
    # trocando de agente ou reiniciando o CLI, que muda o pid).
    new_set = {(p["pane_id"], p.get("agent"), p.get("pid")) for p in panes}
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
    codex_path: dict = {}  # pane_id -> rollout path (idem, panes codex)
    to_read: dict = {}     # path -> (mtime, parser) (misses a reler no executor)
    to_meta: set = set()   # rollouts com header fora do cache (ler no executor)
    codex_cands: set = set()   # candidatos do ciclo, p/ poda do cache de headers
    for p in panes:
        pid, status = p["pane_id"], p["agent_status"]
        a = {
            "pane_id": pid,
            "agent": p["agent"],
            "status": status,
            # nome da tab, senão o da space (tab sem nome vem como número)
            "project": friendly_name(p["tab"], p["space"]) or os.path.basename(p["cwd"]),
            "workspace_id": p["space"],
        }
        acc = pane_account.get(pid)
        if acc:
            a["account"] = acc_email_cache.get(acc, "")
            org, corp = accounts.org_and_corp(a["account"])
            if org:
                a["org"], a["corp"] = org, corp
        path, parser, bucket = None, transcript.session_metrics, pane_path
        if p.get("agent") == "claude" and acc:
            path = _resolve_transcript_path(p, acc)
        elif p.get("agent") == "codex" and acc:
            path = _resolve_rollout_path(p, acc, to_meta, codex_cands)
            parser, bucket = rollout.session_metrics, codex_path
        if path:
            try:
                mt = os.path.getmtime(path)
            except OSError:
                mt = None  # arquivo sumiu entre o glob e o stat: ignora este ciclo
            if mt is not None:
                bucket[pid] = path
                cached = tx_cache.get(path)
                if cached is None or cached[0] != mt:
                    to_read[path] = (mt, parser)
        if status == "working":
            # só o primeiro ciclo em working cria o carimbo; os seguintes o herdam
            new_since[pid] = working_since.get(pid, now)
            a["since"] = int(new_since[pid])
        agents.append(a)

    # Fase B (executor): só os transcripts/rollouts que mudaram de mtime e os
    # headers de rollout inéditos, fora do event loop — ler conteúdo é I/O
    # bloqueante, o mesmo cuidado do _read_env_filtered/refresh_accounts do
    # multi-conta. Header ilegível entra como None no cache de propósito:
    # nunca casa com cwd nenhum e não é relido a cada ciclo.
    if to_read or to_meta:
        loop = asyncio.get_running_loop()
        metas, fresh = await loop.run_in_executor(
            None, lambda: ({pth: rollout.head_cwd(pth) for pth in to_meta},
                           {pth: (mt, parser(pth))
                            for pth, (mt, parser) in to_read.items()}))
        rollout_meta_cache.update(metas)
        tx_cache.update(fresh)

    # Fase C (no loop): anexa model/context_pct/effort a partir do cache já
    # quente. model e effort só entram quando existem (effort exige CLI >=
    # 2.1.234; rollout sem turn_context no tail vem sem modelo): mandar chave
    # vazia engordaria o payload de todo mundo.
    for a in agents:
        path = pane_path.get(a["pane_id"]) or codex_path.get(a["pane_id"])
        cached = tx_cache.get(path) if path else None
        m = cached[1] if cached else None
        if m:
            if m["model"]:
                a["model"] = m["model"]
            a["context_pct"] = m["context_pct"]
            if m["effort"]:
                a["effort"] = m["effort"]

    # Poda: só ficam no cache os transcripts/rollouts de panes vivos neste
    # ciclo — sem isso ele cresceria sem limite conforme sessões fecham. O
    # cache de headers mantém também os candidatos do ciclo: podá-los faria a
    # fase B reler os headers dos rollouts vizinhos a cada segundo.
    live_paths = set(pane_path.values()) | set(codex_path.values())
    for k in list(tx_cache):
        if k not in live_paths:
            tx_cache.pop(k, None)
    keep_meta = codex_cands | set(codex_path.values())
    for k in list(rollout_meta_cache):
        if k not in keep_meta:
            rollout_meta_cache.pop(k, None)
    pane_paths = pane_path  # publica o mapa desta rodada para _session_paths()
    codex_paths.clear()     # idem para os rollouts (mutação in-place: sem global)
    codex_paths.update(codex_path)

    # Máquinas SSH salvas (BRIDGE_MACHINES): entram no mesmo snapshot, com o id
    # prefixado pelo código da máquina. O cliente do Herdr mostra várias
    # máquinas juntas; o servidor local não, então é a ponte que junta.
    agents.extend(remote_agents)

    # reconstruir em vez de mutar poda de graça quem saiu de working ou sumiu
    working_since = new_since
    known_panes.clear()
    known_panes.update(a["pane_id"] for a in agents)
    snapshot = json.dumps({"type": "agents", "agents": agents}, separators=(",", ":"))
    if snapshot != last_snapshot:
        last_snapshot = snapshot
        await broadcast_line(snapshot)


class Hc:
    """Uma conexão `hc connect`: requests casados por id, notificações numa fila.

    A fila recebe (method, params) de cada notificação e None quando o
    processo cai, para quem a consome saber que precisa reconectar.
    """

    def __init__(self) -> None:
        self.proc = None
        self.alive = False
        self.pending: dict = {}
        self.notes: asyncio.Queue = asyncio.Queue()
        self.next_id = 0

    async def start(self) -> bool:
        try:
            # limit alto: o snapshot do events.subscribe vem numa linha só
            self.proc = await asyncio.create_subprocess_exec(
                HC_BIN, "connect", stdin=asyncio.subprocess.PIPE,
                stdout=asyncio.subprocess.PIPE, stderr=asyncio.subprocess.DEVNULL,
                limit=1 << 24, creationflags=NO_WINDOW)
        except (OSError, ValueError) as exc:
            log.warning("hc connect: %s", exc)
            return False
        self.alive = True
        asyncio.create_task(self._read())
        return True

    async def _read(self) -> None:
        while True:
            try:
                line = await self.proc.stdout.readline()
            except (ValueError, ConnectionError):   # linha acima do limit
                line = b""
            if not line:
                break
            try:
                msg = json.loads(line)
            except ValueError:
                continue
            if not isinstance(msg, dict):
                continue
            if "id" in msg:
                fut = self.pending.pop(msg["id"], None)
                if fut and not fut.done():
                    fut.set_result(msg)
            elif "method" in msg:
                self.notes.put_nowait((msg["method"], msg.get("params") or {}))
        for fut in self.pending.values():
            if not fut.done():
                fut.set_result({})
        self.alive = False
        self.pending.clear()
        self.notes.put_nowait(None)

    async def request(self, method: str, params: dict | None = None,
                      timeout: float = 10):
        """Result do método, ou None em erro, timeout ou conexão caída."""
        if not self.alive:
            return None
        self.next_id += 1
        rid = self.next_id
        fut = asyncio.get_running_loop().create_future()
        self.pending[rid] = fut
        try:
            self.proc.stdin.write((json.dumps({"jsonrpc": "2.0", "id": rid,
                                               "method": method,
                                               "params": params or {}}) + "\n").encode())
            await self.proc.stdin.drain()
            resp = await asyncio.wait_for(fut, timeout)
        except (OSError, asyncio.TimeoutError):
            self.pending.pop(rid, None)
            return None
        if "error" in resp or "result" not in resp:
            log.warning("hc %s: %s", method, (resp.get("error") or {}).get("message"))
            return None
        return resp.get("result")

    async def close(self) -> None:
        if self.proc and self.proc.returncode is None:
            with contextlib.suppress(ProcessLookupError):
                self.proc.kill()
            await self.proc.wait()


rpc_conn = None   # conexão de requests do hc_loop; None enquanto desconectado


async def hc_call(method: str, params: dict):
    """Request ao core pela conexão do hc_loop; None em erro ou sem conexão."""
    return await rpc_conn.request(method, params) if rpc_conn else None


def trim_text(raw: str, keep: int) -> str:
    """Últimas `keep` linhas do texto, dentro de CONTENT_MAX_B.

    Só as linhas em branco do fim caem; estourando o cap, caem linhas inteiras
    mais antigas (nunca um corte no meio de um caractere UTF-8).
    """
    lines = raw.splitlines()
    while lines and not lines[-1].strip():
        lines.pop()
    lines = lines[-keep:]
    while len(lines) > 1 and len("\n".join(lines).encode()) > CONTENT_MAX_B:
        lines.pop(0)
    return "\n".join(lines)


async def hc_loop() -> None:
    """Mantém `terminals` em dia com o core e os painéis com ele.

    Duas conexões: depois do events.subscribe a conexão só carrega
    notificações, então os requests vão por outra. Cada evento relevante relê
    o terminal com terminal.get em vez de aplicar o `data` do evento: o estado
    lido é sempre o atual, então ordem e eventos repetidos não importam. Um
    `missed` (a ponte ficou para trás) refaz a lista inteira com
    terminal.list. Se qualquer das conexões cair, reconecta.
    """
    while True:
        rpc, sub = Hc(), Hc()
        if await rpc.start() and await sub.start():
            global rpc_conn
            rpc_conn = rpc
            snap = await sub.request("events.subscribe", {})
            if snap is not None:
                terminals.clear()
                terminals.update({t["address"]: t for t in snap.get("terminals", [])})
                log.info("hc: assinado, %d terminal(is)", len(terminals))
                await push_agents()
                while True:
                    note = await sub.notes.get()
                    if note is None:
                        break
                    method, params = note
                    addr = params.get("terminal") or ""
                    if method == "missed":
                        log.warning("hc: eventos perdidos, ressincronizando")
                        res = await rpc.request("terminal.list", {})
                        if res is None:
                            break
                        terminals.clear()
                        terminals.update({t["address"]: t for t in res.get("terminals", [])})
                    elif method == "event" and params.get("type") in REFRESH_EVENTS and addr:
                        t = (await rpc.request("terminal.get", {"terminal": addr})
                             if params.get("type") != "removed" else None)
                        if not rpc.alive:
                            break
                        if t:
                            terminals[addr] = t
                        else:
                            terminals.pop(addr, None)
                    else:
                        continue    # heartbeat e eventos que não mudam agents
                    await push_agents()
        rpc_conn = None
        await rpc.close()
        await sub.close()
        log.warning("hc connect caiu, reconectando em 2s")
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


async def update_loop() -> None:
    """Mantém a ponte na versão publicada e reinicia quando o disco muda.

    Duas coisas, no mesmo laço. A ida à rede é diária e decide se há release
    novo (update.check). A releitura do manifesto local é de minuto em minuto e
    responde a uma pergunta diferente: o código em disco ainda é o que este
    processo está rodando? Quando deixa de ser, reiniciar é a única saída —
    trocar os arquivos nunca trocou o processo, porque a ponte sobe destacada
    e o start.py sai cedo se a porta já responde.

    Isso cobre também o `herdr plugin install` feito na mão, que é o passo cujo
    par (`restart-bridge`) todo mundo esquece: em até um minuto a ponte percebe
    sozinha e sobe de novo, sem depender de ninguém lembrar.
    """
    await asyncio.sleep(UPDATE_DELAY_S)
    loop = asyncio.get_running_loop()
    last = None      # sentinela: a base do monotonic varia por plataforma
    tries = 0
    while True:
        if last is None or time.monotonic() - last >= UPDATE_POLL_S:
            last = time.monotonic()
            await loop.run_in_executor(None, update.check, HERDR_BIN)
        disco = update.installed_version()
        if disco and BOOT_VERSION and disco != BOOT_VERSION and tries <= UPDATE_TRIES:
            tries += 1
            if tries > UPDATE_TRIES:
                # Continuar tentando de minuto em minuto encheria o log sem
                # mudar nada; parar calado esconderia que a ponte está rodando
                # código velho. Um erro, uma vez.
                log.error("a ponte não reiniciou em %d tentativas: rodando %s "
                          "com %s em disco", UPDATE_TRIES, BOOT_VERSION, disco)
            else:
                log.info("versão em disco mudou (%s → %s): reiniciando",
                         BOOT_VERSION, disco)
                update.restart_bridge(sys.executable)
        await asyncio.sleep(UPDATE_STEP_S)


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

    # pane de outra máquina: o socket local não o alcança, só a CLI com
    # --machine. Só focus e send_keys fazem sentido para o pager, e é o que a
    # allowlist de teclas continua valendo.
    remote = remote_panes.get(pane_id) if pane_id else None
    if remote and kind not in ("focus", "send_keys"):
        return await deny("pane de outra máquina: só focus e send_keys")

    if kind in ("send_keys", "send_text"):
        # validação antes de tudo: vale mesmo enquanto o envio não existe
        keys, text = msg.get("keys", []), msg.get("text", "")
        if kind == "send_keys" and not all(k in SAFE_KEYS for k in keys):
            return await deny("tecla fora da allowlist")
        if kind == "send_text" and (not text or len(text) > MAX_TEXT):
            return await deny("texto vazio ou longo demais")

    if remote and kind == "send_keys":
        log.info("send_keys pane=%s keys=%s", pane_id, keys)
        await cli_run(["--machine", remote[0], "pane", "send-keys", remote[1], *keys])

    elif remote and kind == "focus":
        log.info("focus pane=%s", pane_id)
        await cli_run(["--machine", remote[0], "agent", "focus", remote[1]])

    elif kind == "read_pane":
        lines = max(1, min(int(msg.get("lines", PANE_LINES)), 60))
        # cols/rows do painel e a trava de resolução ficam para o ticket #11
        res = await hc_call("terminal.read", {"terminal": pane_id, "lines": lines})
        if res is None:
            return await deny("read failed")
        content = trim_text(res.get("text", ""), lines)

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

    elif kind == "send_text":
        log.info("send_text pane=%s (%d chars)", pane_id, len(text))
        # force: o painel quer o texto já, inclusive num agente bloqueado
        # esperando resposta; sem wait, que o envio só confirma a entrega.
        if await hc_call("terminal.send", {"terminal": pane_id, "text": text,
                                           "force": True}) is None:
            await deny("send failed")

    elif kind == "send_keys":
        log.info("send_keys pane=%s keys=%s", pane_id, keys)
        names = [HC_KEYS.get(k, k.lower()) for k in keys]
        if await hc_call("terminal.keys", {"terminal": pane_id, "keys": names}) is None:
            await deny("keys failed")

    elif kind == "focus":
        log.info("focus pane=%s", pane_id)
        if await cli_run(["do", "agent.focus", "--terminal", pane_id],
                         binary=HERDR_APP_BIN) is None:
            await deny("focus failed")

    elif kind in ("scroll_pane", "release_pane"):
        # shortcut: rolagem e trava de resolução ainda não portadas (ticket #11)
        return await deny("not supported yet")

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
        # Versão da ponte: BOOT_VERSION, e não a do disco — o que interessa ao
        # painel é o que está no ar, que depois de um install pendente de
        # reinício não são a mesma coisa.
        writer.write((json.dumps({"type": "bridge_info", "version": BOOT_VERSION},
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
        log.info("painel desconectado: %s", peer)


class DiscoveryResponder(asyncio.DatagramProtocol):
    """Responde probes herdr-find de painéis com slot em descoberta automática.

    O probe é broadcast e o nonce é público; a resposta prova posse do token
    sem transportá-lo — h = HMAC-SHA256(token, "nonce|ip|port|name") — e
    assina o ip/port que o painel deve adotar (nunca o remetente do datagrama,
    para um vizinho não conseguir redirecionar o painel por relay).
    Só escuta broadcast de verdade com BRIDGE_BIND no default 0.0.0.0.

    E a máquina precisa deixar entrar UDP na BRIDGE_PORT: no Windows com WSL o
    Hyper-V firewall bloqueia entrada por padrão (é outra regra, não o firewall
    comum), e sem ela a ponte sobe, escuta e o painel nunca a encontra — fica em
    OFFLINE sem erro nenhum do lado da ponte.
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
    global TOKEN, BOOT_VERSION
    if not shutil.which(HC_BIN):
        log.error("binário do hc não encontrado (%s)", HC_BIN)
        sys.exit(1)
    TOKEN = load_token()
    BOOT_VERSION = update.installed_version()
    log.info("ponte versão %s", BOOT_VERSION or "?")
    asyncio.create_task(hc_loop())
    asyncio.create_task(limits_loop())
    asyncio.create_task(update_loop())
    if MACHINES:
        # só aqui a ponte toca SSH: as máquinas são lidas pelo próprio Herdr
        log.info("incluindo máquinas salvas: %s", MACHINES)
        asyncio.create_task(machines_loop())
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
