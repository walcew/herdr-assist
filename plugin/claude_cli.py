#!/usr/bin/env python3
"""Uso e identidade da conta Claude pelo próprio CLI do Claude Code (stdlib).

O Claude Code guarda a credencial onde cada sistema manda — Keychain no macOS,
arquivo no Windows/Linux — e a renova sozinho enquanto o usuário trabalha. Ler
esse armazenamento daqui seria um backend por SO e refém de um formato interno:
foi exatamente o que quebrou quando o macOS passou a renovar no Keychain e o
`.credentials.json` congelou com o token expirado. Como todo mundo usa o Claude
pelo CLI dentro do Herdr, delegar a ele vale em todo SO e não toca em token
nenhum.

`claude -p "/usage"` não gasta tokens (num_turns 0, custo 0), não deixa
transcript no config-dir e respeita CLAUDE_CONFIG_DIR — o multi-conta continua
valendo. Chamadas são bloqueantes (subprocess): só de dentro do executor.

parse_usage/parse_reset são puras — testáveis sem invocar o CLI.
"""

import datetime
import json
import os
import re
import shutil
import subprocess

import accounts

HOME = os.path.expanduser("~")  # injetável nos testes

AUTH_TIMEOUT_S = 20
USAGE_TIMEOUT_S = 45

# Onde procurar o CLI quando ele não está no PATH do processo. A ponte sobe
# como plugin do Herdr, que pode ter herdado um PATH curto (uma sessão ssh não
# interativa traz só /usr/bin:/bin:/usr/sbin:/sbin), e o instalador do Claude
# Code costuma pôr o binário em ~/.local/bin — fora dele. Sem esta busca a
# coleta morre com "claude não está no PATH" numa máquina onde o CLI funciona.
_FALLBACK_BINS = (
    "~/.local/bin/claude",
    "~/.claude/local/claude",
    "/opt/homebrew/bin/claude",
    "/usr/local/bin/claude",
)
_exe_cache = []

# "Current session: 52% used · resets Aug 21 at 12:29am (America/Sao_Paulo)"
# "Current week (all models): 2% used · resets Aug 27 at 9:59pm (...)"
# "Current week (Fable): 0% used"                      <- sem reset anunciado
_USAGE_RE = re.compile(
    r"^Current (?:session|week \((?P<scope>[^)]+)\)): (?P<pct>\d+)% used"
    r"(?:\s*\S\s*resets (?P<reset>.+?))?\s*$")

# "Aug 21 at 12:29am (America/Sao_Paulo)" — o nome do fuso é ignorado de
# propósito: o CLI imprime no fuso local da máquina e a ponte roda na mesma
# máquina, então basta interpretar como hora local. Depender de zoneinfo
# custaria o pacote tzdata no Windows, e a ponte é só stdlib.
_RESET_RE = re.compile(
    r"^(?P<mon>[A-Za-z]{3})\s+(?P<day>\d{1,2})\s+at\s+"
    r"(?P<hour>\d{1,2}):(?P<min>\d{2})\s*(?P<ampm>[ap]m)")

WINDOW_SESSION_S = 18000    # 5h
WINDOW_WEEK_S = 604800      # 7d


class ClaudeCliError(RuntimeError):
    """O CLI não pôde ser executado ou respondeu algo inesperado."""


class NotLoggedIn(ClaudeCliError):
    """Conta sem login nesta máquina — provedor some do painel, sem alarde."""


def parse_reset(text: str, ref: datetime.datetime) -> int:
    """Epoch do "resets ..." anunciado pelo /usage; 0 se não der para ler.

    O texto não traz o ano. Como toda janela reseta no futuro próximo (5h ou
    7d), o ano é o de `ref` — e o seguinte quando isso cairia no passado, que é
    o caso da virada de ano.
    """
    m = _RESET_RE.match(text.strip())
    if not m:
        return 0
    try:
        mon = datetime.datetime.strptime(m.group("mon"), "%b").month
    except ValueError:
        return 0
    hour = int(m.group("hour")) % 12
    if m.group("ampm") == "pm":
        hour += 12
    for year in (ref.year, ref.year + 1):
        try:
            dt = datetime.datetime(year, mon, int(m.group("day")), hour,
                                   int(m.group("min")))
        except ValueError:      # 29/02 num ano não bissexto
            continue
        if dt >= ref - datetime.timedelta(days=1):
            return int(dt.timestamp())
    return 0


def parse_usage(text: str, ref: datetime.datetime = None) -> list:
    """Linhas de limite do texto do /usage, no formato que o painel consome.

    Devolve [] quando não há nenhuma linha "Current ..." — é o que o CLI
    imprime para uma conta sem assinatura logada (cai num resumo de custo).
    """
    ref = ref or datetime.datetime.now()
    rows = []
    for line in text.splitlines():
        m = _USAGE_RE.match(line.strip())
        if not m:
            continue
        scope = m.group("scope")
        if scope is None:
            label, window = "5h", WINDOW_SESSION_S
        elif scope == "all models":
            label, window = "7d", WINDOW_WEEK_S
        else:
            label, window = ("7d " + scope)[:16], WINDOW_WEEK_S
        rows.append({"label": label, "pct": int(m.group("pct")),
                     "resets_at": parse_reset(m.group("reset") or "", ref),
                     "window_s": window})
    return rows


def _find_claude() -> str:
    """Caminho do CLI: PATH primeiro, depois os lugares usuais de instalação.

    Cacheado porque roda a cada coleta (duas vezes por conta) e o binário não
    muda de lugar durante a execução.
    """
    if _exe_cache:
        return _exe_cache[0]
    exe = shutil.which("claude")
    if not exe:
        for cand in _FALLBACK_BINS:
            cand = os.path.expanduser(cand)
            if os.path.isfile(cand) and os.access(cand, os.X_OK):
                exe = cand
                break
    if exe:
        _exe_cache.append(exe)
    return exe or ""


def is_default_dir(config_dir: str, home: str) -> bool:
    """A conta é a default (a que não declara CLAUDE_CONFIG_DIR)?"""
    return (os.path.normpath(os.path.abspath(config_dir))
            == accounts.default_dir("claude", home))


def _run(args, config_dir: str, timeout: int) -> str:
    """Roda o CLI na conta de `config_dir`. Bloqueante: só no executor.

    A variável CLAUDE_CONFIG_DIR é o que seleciona a conta — mas só pode ir
    para as contas que realmente a usam. Com ela presente o CLI procura a
    credencial apenas dentro do diretório e ignora o cofre do sistema, então
    apontá-la para o próprio caminho default faz o CLI se declarar deslogado no
    macOS (a credencial da conta default mora no Keychain). Para a default, a
    variável é removida do ambiente — inclusive a que a ponte possa ter
    herdado do pane onde subiu.
    """
    exe = _find_claude()
    if not exe:
        raise ClaudeCliError("claude não encontrado (PATH nem caminhos usuais)")
    env = dict(os.environ)
    if is_default_dir(config_dir, HOME):
        env.pop("CLAUDE_CONFIG_DIR", None)
    else:
        env["CLAUDE_CONFIG_DIR"] = config_dir
    try:
        p = subprocess.run([exe] + args, capture_output=True, text=True,
                           timeout=timeout, env=env)
    except (subprocess.TimeoutExpired, OSError) as e:
        raise ClaudeCliError(str(e) or type(e).__name__)
    if p.returncode != 0:
        raise ClaudeCliError("claude %s: rc=%d %s"
                             % (args[0], p.returncode, (p.stderr or "")[:120]))
    return p.stdout


def auth_status(config_dir: str) -> dict:
    """{loggedIn, email, subscriptionType, ...} da conta. ~0,2s."""
    try:
        return json.loads(_run(["auth", "status", "--json"], config_dir,
                               AUTH_TIMEOUT_S))
    except ValueError as e:
        raise ClaudeCliError("auth status não é JSON: %s" % e)


def usage_rows(config_dir: str) -> list:
    """Limites da conta pelo /usage. NotLoggedIn quando não há assinatura."""
    out = _run(["-p", "/usage", "--output-format", "json"], config_dir,
               USAGE_TIMEOUT_S)
    try:
        result = json.loads(out).get("result") or ""
    except ValueError as e:
        raise ClaudeCliError("/usage não é JSON: %s" % e)
    rows = parse_usage(result)
    if not rows:
        raise NotLoggedIn(config_dir)
    return rows
