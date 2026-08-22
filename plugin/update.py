#!/usr/bin/env python3
# /// script
# requires-python = ">=3.9"
# ///
"""Atualização da ponte: descobre a versão publicada, decide e reinstala.

O painel se atualiza sozinho desde sempre (src/fw_update.c); a ponte, que roda
no host e muda muito mais, não tinha nada. Isto fecha a lacuna com as mesmas
peças: um GET no `manifest.json` que o release já publica, comparação por
diferença, e um switch para desligar.

Por que o `manifest.json` do painel serve para o plugin: o release é um trem
único — a mesma tag `vX.Y.Z` builda painel e Cardputer —, então a versão
anunciada ali É a versão do plugin daquela tag. Não há asset novo a publicar,
nem chamada à API do GitHub (rate limit), nem release notes a parsear.

O `herdr` não tem `plugin update`: atualizar é repetir o `plugin install`, e
trocar os arquivos não troca a ponte em execução (o start.py sai cedo se a
porta responde, e o processo é destacado). Daí o par install + restart.

As funções de decisão não fazem I/O de rede nem subprocess — é o que deixa a
lógica testável sem cortar release de teste (ver tests/test_update.py).
"""
from __future__ import annotations

import json
import logging
import os
import re
import subprocess
import time
import urllib.error
import urllib.request

log = logging.getLogger("bridge.update")

DIR = os.environ.get("HERDR_PLUGIN_ROOT", os.path.dirname(os.path.abspath(__file__)))
CONF = os.environ.get("HERDR_PLUGIN_CONFIG_DIR",
                      os.path.expanduser("~/.config/herdr-assist"))
STATE = os.environ.get("HERDR_PLUGIN_STATE_DIR", CONF)

PLUGIN_ID = "herdr-assist"
MANIFEST = os.path.join(DIR, "herdr-plugin.toml")
STATE_FILE = os.path.join(STATE, "update.json")

# O mesmo manifesto que o painel consulta. Vem do ambiente para dar como
# exercitar o caminho inteiro contra outro arquivo, sem publicar um release só
# para testar.
MANIFEST_URL = os.environ.get(
    "UPDATE_MANIFEST_URL",
    "https://github.com/walcew/herdr-assist/releases/latest/download/manifest.json")

# Alvo do reinstall quando o Herdr não disser de onde o plugin veio; é o
# comando que o README manda rodar na instalação.
DEFAULT_REPO = "walcew/herdr-assist/plugin"

HTTP_TIMEOUT = 10
CLI_TIMEOUT = 180        # clonar o repo num host lento passa fácil de 30s

_VERSION_RE = re.compile(r'^\s*version\s*=\s*"([^"]+)"', re.MULTILINE)


def normalize(v: str) -> str:
    """"v0.10.0" e "0.10.0" viram a mesma coisa.

    O manifesto do release carrega a tag com o `v`; o herdr-plugin.toml declara
    a versão sem ele. Comparar sem normalizar acusaria diferença sempre.
    """
    return (v or "").strip().lstrip("vV")


def auto_enabled() -> bool:
    """Switch de atualização automática (AUTO_UPDATE=0 no `env` do config-dir).

    Espelha o `no_auto_update` do painel, inclusive no default: quem não disse
    nada quer atualizar. O start.py carrega o arquivo `env` e a ponte herda.
    """
    return os.environ.get("AUTO_UPDATE", "1").strip().lower() not in ("0", "false", "no")


def installed_version() -> str:
    """Versão declarada no herdr-plugin.toml ao lado deste arquivo.

    Regex e não `tomllib`: aquele é 3.11+ e a ponte roda no python3 de fábrica
    do macOS (3.9). A primeira chave `version` do arquivo é a do plugin — as
    outras seções (`[[startup]]`, `[[panes]]`) não têm uma.
    """
    try:
        with open(MANIFEST, encoding="utf-8") as fh:
            m = _VERSION_RE.search(fh.read())
    except OSError as exc:
        log.warning("não li %s: %s", MANIFEST, exc)
        return ""
    return normalize(m.group(1)) if m else ""


def published_version(url: str = "") -> str:
    """Versão do release mais novo; "" se o manifesto não veio ou não parseia."""
    try:
        with urllib.request.urlopen(url or MANIFEST_URL, timeout=HTTP_TIMEOUT) as r:
            data = json.loads(r.read(4096).decode("utf-8", "replace"))
    except (urllib.error.URLError, OSError, ValueError) as exc:
        log.warning("manifesto inacessível: %s", exc)
        return ""
    ver = data.get("version") if isinstance(data, dict) else None
    return normalize(ver) if isinstance(ver, str) else ""


def plugin_source(herdr_bin: str) -> dict:
    """Como o Herdr registrou este plugin: {"kind": ..., "repo": ...}.

    O `kind` é o que impede o estrago: com "local" o plugin está LINKADO a um
    checkout de trabalho, e reinstalar por cima o trocaria por um checkout
    gerenciado — apagando o ambiente de quem desenvolve.

    O formato do `source` para plugin vindo do GitHub não pôde ser conferido
    aqui (esta máquina é `local`), então o repo é lido com tolerância e o que
    vier fica no log; sem um palpite bom, vale o DEFAULT_REPO.
    """
    out = _run([herdr_bin, "plugin", "list", "--json", "--plugin", PLUGIN_ID], 15)
    if out is None:
        return {}
    try:
        plugins = json.loads(out).get("result", {}).get("plugins", [])
    except ValueError:
        return {}
    if not plugins:
        return {}
    src = plugins[0].get("source") or {}
    kind = str(src.get("kind", ""))
    repo = ""
    for key in ("repo", "slug", "spec", "path"):     # o nome real sai no log
        if isinstance(src.get(key), str) and "/" in src[key]:
            repo = src[key]
            break
    if kind and kind != "local" and not repo:
        log.info("source do plugin sem repo reconhecível: %s", src)
    return {"kind": kind, "repo": repo}


def load_state() -> dict:
    try:
        with open(STATE_FILE, encoding="utf-8") as fh:
            st = json.load(fh)
    except (OSError, ValueError):
        return {}
    return st if isinstance(st, dict) else {}


def save_state(st: dict) -> None:
    try:
        os.makedirs(STATE, exist_ok=True)
        with open(STATE_FILE, "w", encoding="utf-8") as fh:
            json.dump(st, fh)
    except OSError as exc:
        log.warning("não gravei %s: %s", STATE_FILE, exc)


def should_update(installed: str, published: str, kind: str, tried: str) -> tuple:
    """Decide, e diz por quê. Devolve (bool, motivo).

    A comparação é por DIFERENÇA, não por ordem — mesma regra do
    fw_update.c:do_check, pelo mesmo motivo: um release de correção que aponta
    "para trás" também precisa chegar aos hosts.
    """
    if not installed or not published:
        return False, "unknown"
    if installed == published:
        return False, "same"
    if kind == "local":
        return False, "local"
    if tried and normalize(tried) == published:
        # A rodada anterior já tentou esta versão e o plugin continua na antiga:
        # o install não pegou. Repetir seria ciclar install→restart→install até
        # sair um release novo.
        return False, "tried"
    return True, "available"


def install(target: str, repo: str, herdr_bin: str) -> bool:
    """Reinstala o plugin na tag `target`. Não reinicia a ponte."""
    out = _run([herdr_bin, "plugin", "install", repo or DEFAULT_REPO,
                "--ref", "v" + target, "--yes"], CLI_TIMEOUT)
    return out is not None


def restart_bridge(python_bin: str) -> None:
    """Sobe o start.py --restart destacado e devolve o controle.

    Quem mata a ponte é ele, pelo pidfile — que é o nosso próprio pid. Por isso
    o processo precisa estar destacado: se fosse filho comum, morreria junto com
    quem o gerou. É o mesmo caminho da ação `restart-bridge`, e não um segundo
    jeito de reiniciar.
    """
    kwargs = {"stdout": subprocess.DEVNULL, "stderr": subprocess.DEVNULL,
              "stdin": subprocess.DEVNULL, "cwd": DIR}
    if os.name == "nt":
        kwargs["creationflags"] = 0x00000008 | 0x08000000   # DETACHED | NO_WINDOW
    else:
        kwargs["start_new_session"] = True
    subprocess.Popen([python_bin, os.path.join(DIR, "start.py"), "--restart"], **kwargs)


def check(herdr_bin: str, force: bool = False) -> dict:
    """Uma rodada completa. Devolve o estado, para log e para a tela.

    `force` ignora só o switch de automáticas — é o gesto do "Verificar agora"
    do painel, e da tecla `u` da administração. Não ignora a guarda de checkout
    local: ali reinstalar destruiria o ambiente de trabalho de qualquer forma.
    """
    st = load_state()
    cur = installed_version()
    pub = published_version()
    if pub:
        st["latest"] = pub
        st["checked"] = int(time.time())
    st["installed"] = cur

    src = plugin_source(herdr_bin)
    go, why = should_update(cur, pub, src.get("kind", ""), st.get("tried", ""))
    if go and not (force or auto_enabled()):
        go, why = False, "off"
    st["state"] = why
    save_state(st)

    if not go:
        if why not in ("same", "unknown"):
            log.info("update: instalada %s, publicada %s — %s", cur, pub, why)
        return st

    log.info("update: %s → %s, reinstalando", cur, pub)
    st["tried"] = pub
    st["tried_at"] = int(time.time())
    save_state(st)          # antes do install: uma queda no meio não pode
                            # apagar o registro da tentativa
    if not install(pub, src.get("repo", ""), herdr_bin):
        log.warning("update: o install falhou; ficando na %s", cur)
        st["state"] = "failed"
        save_state(st)
        return st
    st["state"] = "installed"
    save_state(st)
    return st


def _run(args: list, timeout: float) -> "str | None":
    """Roda um comando e devolve o stdout; None em qualquer falha."""
    kwargs = {"capture_output": True, "text": True, "timeout": timeout}
    if os.name == "nt":
        kwargs["creationflags"] = 0x08000000    # CREATE_NO_WINDOW
    try:
        r = subprocess.run(args, **kwargs)
    except (OSError, subprocess.SubprocessError) as exc:
        log.warning("%s: %s", args[1] if len(args) > 1 else args[0], exc)
        return None
    if r.returncode != 0:
        log.warning("%s saiu %d: %s", " ".join(args[1:3]), r.returncode,
                    (r.stderr or "").strip()[:200])
        return None
    return r.stdout
