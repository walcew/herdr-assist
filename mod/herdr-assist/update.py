"""Atualização do mod: descobre a versão publicada, baixa o pacote e troca a pasta.

O release publica a pasta do mod como asset (`herdr-assist-mod.tar.gz`, ver
release.yml), e a versão dela é travada com a tag. O `manifest.json` do painel
continua sendo o de sempre: como o release é um trem único, a versão anunciada
ali É a versão do mod daquela tag, então um GET basta, sem a API do GitHub.

O pacote só é extraído se o sha256 dele bater com o do `SHA256SUMS` da mesma
tag (o release.yml gera os dois); sem a linha ou divergente, não instala.

A troca extrai o pacote ao lado da pasta do mod (mesmo diretório pai, para o
rename ficar no mesmo filesystem), renomeia a pasta antiga para o backup oculto
`.herdr-assist.old`, põe a nova no lugar e só então apaga o backup. Se algo
falha no meio, a antiga volta; se o processo morre entre os dois renames,
recover() restaura o backup na próxima subida do ctl.py ou da Ponte. Tudo que
fica ao lado é oculto (prefixo `.`), porque o app não recarrega pastas ocultas.
Quem reinicia a Ponte é o laço do herdr_bridge.py, que percebe a versão em disco
mudar e chama restart_bridge (= `ctl.py restart`).

Só atualiza a cópia instalada em `<home do app>/mods/herdr-assist`, e não atua
em checkout de desenvolvimento (ver is_dev) nem com auto_update desligado. A
rede e o download entram por parâmetro, para o teste.
"""
from __future__ import annotations

import hashlib
import json
import logging
import os
import re
import shutil
import subprocess
import tarfile
import tempfile
import time
import urllib.error
import urllib.request

log = logging.getLogger("bridge.update")

DIR = os.path.dirname(os.path.abspath(__file__))
# Onde o app procura o mod. É esse caminho, e não o DIR, que diz se a pasta é
# um symlink: o Python resolve symlinks ao importar, então o DIR já é o real.
APP_MOD_DIR = os.path.join(os.environ.get("HERDR_APP_HOME")
                           or os.path.expanduser("~/.config/herdr-app"),
                           "mods", "herdr-assist")
# Fora da pasta do mod, que é substituída inteira a cada atualização.
STATE = os.environ.get("HERDR_ASSIST_STATE_DIR",
                       os.path.expanduser("~/.config/herdr-assist"))

# Versão deste mod, sem o `v`: o release.yml trava a publicação se ela divergir da tag.
VERSION_FILE = os.path.join(DIR, "VERSION")
STATE_FILE = os.path.join(STATE, "update.json")

# O mesmo manifesto que o painel consulta. Vem do ambiente para dar como
# exercitar o caminho inteiro contra outro arquivo, sem publicar um release só
# para testar.
MANIFEST_URL = os.environ.get(
    "UPDATE_MANIFEST_URL",
    "https://github.com/walcew/herdr-assist/releases/latest/download/manifest.json")
ASSET_NAME = "herdr-assist-mod.tar.gz"
ASSET_URL = ("https://github.com/walcew/herdr-assist/releases/download/"
             "v{}/" + ASSET_NAME)
SUMS_URL = "https://github.com/walcew/herdr-assist/releases/download/v{}/SHA256SUMS"

HTTP_TIMEOUT = 10
DOWNLOAD_TIMEOUT = 60
DOWNLOAD_MAX = 8 * 1024 * 1024   # o pacote tem poucas centenas de KB


def normalize(v: str) -> str:
    """"v0.10.0" e "0.10.0" viram a mesma coisa.

    O manifesto do release carrega a tag com o `v`; o arquivo VERSION traz a
    versão sem ele. Comparar sem normalizar acusaria diferença sempre.
    """
    return (v or "").strip().lstrip("vV")


def auto_enabled() -> bool:
    """Switch de atualização automática (AUTO_UPDATE=0 no ambiente).

    Espelha o `no_auto_update` do painel, inclusive no default: quem não disse
    nada quer atualizar. A Ponte aplica o `auto_update` do doc em os.environ.
    """
    return os.environ.get("AUTO_UPDATE", "1").strip().lower() not in ("0", "false", "no")


def installed_version() -> str:
    """Versão lida do arquivo VERSION ao lado deste arquivo; "" se faltar."""
    try:
        with open(VERSION_FILE, encoding="utf-8") as fh:
            return normalize(fh.read())
    except OSError as exc:
        log.warning("could not read %s: %s", VERSION_FILE, exc)
        return ""


def published_version(url: str = "") -> str:
    """Versão do release mais novo; "" se o manifesto não veio ou não parseia."""
    try:
        with urllib.request.urlopen(url or MANIFEST_URL, timeout=HTTP_TIMEOUT) as r:
            data = json.loads(r.read(4096).decode("utf-8", "replace"))
    except (urllib.error.URLError, OSError, ValueError) as exc:
        log.warning("manifest unreachable: %s", exc)
        return ""
    ver = data.get("version") if isinstance(data, dict) else None
    return normalize(ver) if isinstance(ver, str) else ""


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
        log.warning("could not write %s: %s", STATE_FILE, exc)


def should_update(installed: str, published: str) -> tuple:
    """Decide, e diz por quê. Devolve (bool, motivo).

    A comparação é por DIFERENÇA, não por ordem — mesma regra do
    fw_update.c:do_check, pelo mesmo motivo: um release de correção que aponta
    "para trás" também precisa chegar aos hosts.
    """
    if not installed or not published:
        return False, "unknown"
    if installed == published:
        return False, "same"
    return True, "available"


def is_dev(mod_dir: str) -> bool:
    """Checkout de trabalho: não é para trocar a pasta.

    É dev se o caminho do app é um symlink, se a pasta tem `.git`, ou se este
    código não roda da pasta do app (ponte avulsa do checkout: trocar a pasta
    apagaria o trabalho).
    """
    return (os.path.islink(APP_MOD_DIR) or os.path.exists(os.path.join(mod_dir, ".git"))
            or os.path.realpath(mod_dir) != os.path.realpath(APP_MOD_DIR))


def backup_dir(mod_dir: str) -> str:
    """Backup da troca: oculto, ao lado da pasta do mod."""
    parent, name = os.path.split(mod_dir)
    return os.path.join(parent, "." + name + ".old")


def recover(mod_dir: str = APP_MOD_DIR) -> None:
    """Pasta do mod ausente e backup presente: a troca caiu entre os renames."""
    old = backup_dir(mod_dir)
    if not os.path.lexists(mod_dir) and os.path.isdir(old):
        try:
            os.rename(old, mod_dir)
            log.warning("update: restored %s from an interrupted swap", mod_dir)
        except OSError as exc:
            log.warning("update: could not restore %s: %s", mod_dir, exc)


def fetch(url: str, dest: str) -> None:
    """Baixa `url` para o arquivo `dest`; levanta OSError se falhar ou passar do teto."""
    with urllib.request.urlopen(url, timeout=DOWNLOAD_TIMEOUT) as r:
        data = r.read(DOWNLOAD_MAX + 1)
    if len(data) > DOWNLOAD_MAX:
        raise OSError("package larger than %d bytes" % DOWNLOAD_MAX)
    with open(dest, "wb") as fh:
        fh.write(data)


def expected_sha256(sums: str) -> str:
    """sha256 do pacote no texto do SHA256SUMS; "" se não há a linha."""
    for line in sums.splitlines():
        digest, _, name = line.strip().partition(" ")
        if name.strip().lstrip("*") == ASSET_NAME and re.fullmatch(r"[0-9a-f]{64}", digest):
            return digest
    return ""


def verify(archive: str, sums_file: str) -> None:
    """ValueError se o pacote não bate com o SHA256SUMS (ou se ele não o lista)."""
    with open(sums_file, encoding="utf-8", errors="replace") as fh:
        want = expected_sha256(fh.read())
    if not want:
        raise ValueError("SHA256SUMS has no entry for " + ASSET_NAME)
    with open(archive, "rb") as fh:
        got = hashlib.sha256(fh.read()).hexdigest()
    if got != want:
        raise ValueError("sha256 mismatch for %s: %s != %s" % (ASSET_NAME, got, want))


def extract(archive: str, dest: str) -> None:
    """Extrai só `herdr-assist/...` com arquivos e pastas comuns; senão, ValueError."""
    with tarfile.open(archive, "r:gz") as tf:
        for m in tf.getmembers():
            parts = m.name.split("/")
            if not (m.isfile() or m.isdir()) or parts[0] != "herdr-assist" \
                    or m.name.startswith("/") or ".." in parts:
                raise ValueError("unexpected member in the package: " + m.name)
        tf.extractall(dest)


def swap(new: str, mod_dir: str) -> None:
    """Põe `new` no lugar de `mod_dir`; se não conseguir, a antiga fica intacta."""
    old = backup_dir(mod_dir)
    shutil.rmtree(old, ignore_errors=True)    # sobra de uma troca interrompida
    os.rename(mod_dir, old)
    try:
        os.rename(new, mod_dir)
    except OSError:
        os.rename(old, mod_dir)
        raise
    shutil.rmtree(old, ignore_errors=True)


def install(target: str, mod_dir: str, fetch_fn=fetch) -> bool:
    """Baixa o pacote da versão `target` e troca a pasta. Não reinicia a Ponte."""
    work = tempfile.mkdtemp(prefix=".herdr-assist-update-", dir=os.path.dirname(mod_dir))
    try:
        archive = os.path.join(work, "mod.tar.gz")
        sums = os.path.join(work, "SHA256SUMS")
        fetch_fn(ASSET_URL.format(target), archive)
        fetch_fn(SUMS_URL.format(target), sums)
        verify(archive, sums)
        extract(archive, work)
        new = os.path.join(work, "herdr-assist")
        with open(os.path.join(new, "VERSION"), encoding="utf-8") as fh:
            if normalize(fh.read()) != target:
                raise ValueError("the package is not version %s" % target)
        swap(new, mod_dir)
        return True
    except (OSError, ValueError, tarfile.TarError) as exc:
        log.warning("update: %s; keeping the current version", exc)
        return False
    finally:
        shutil.rmtree(work, ignore_errors=True)


def restart_bridge(python_bin: str) -> None:
    """Sobe `ctl.py restart` destacado e devolve o controle.

    O restart mata o terminal da Ponte, que é quem chama aqui; por isso o
    processo precisa estar em outra sessão, ou morreria junto.
    """
    subprocess.Popen([python_bin, os.path.join(DIR, "ctl.py"), "restart"],
                     stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                     stdin=subprocess.DEVNULL, cwd=DIR, start_new_session=True)


def check(mod_dir: str = DIR, fetch_fn=fetch) -> dict:
    """Uma rodada completa. Devolve o estado, para log e para a tela."""
    st = load_state()
    cur = installed_version()
    pub = published_version()
    if pub:
        st["latest"] = pub
        st["checked"] = int(time.time())
    st["installed"] = cur

    go, why = should_update(cur, pub)
    if go and is_dev(mod_dir):
        go, why = False, "dev"
    elif go and not auto_enabled():
        go, why = False, "off"

    if go:
        log.info("update: %s -> %s, swapping the mod folder", cur, pub)
        why = "installed" if install(pub, mod_dir, fetch_fn) else "failed"
    elif why not in ("same", "unknown"):
        log.info("update: installed %s, published %s: %s", cur, pub, why)
    st["state"] = why
    save_state(st)
    return st
