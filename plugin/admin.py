#!/usr/bin/env python3
# /// script
# requires-python = ">=3.9"
# ///
"""Tela de administração da ponte, aberta como pane do Herdr.

Reúne o que antes exigia arqueologia na CLI: token, estado da ponte, painéis
conectados — e, principalmente, o pareamento. Digitar 32 caracteres
hexadecimais num touch de 3,5" é inviável, então o painel entra em modo de
pareamento, se anuncia por broadcast, e é daqui que a configuração sai pronta.

Só stdlib; roda no python3 de fábrica do macOS.
"""
from __future__ import annotations

import json
import os
import re
import shutil
import socket
import subprocess
import sys
import time

from i18n import t

PORT = int(os.environ.get("BRIDGE_PORT", "9375"))
PAIR_PORT = 9376
CONF = os.environ.get("HERDR_PLUGIN_CONFIG_DIR",
                      os.path.expanduser("~/.config/herdr-assist"))
STATE = os.environ.get("HERDR_PLUGIN_STATE_DIR", CONF)
ROOT = os.environ.get("HERDR_PLUGIN_ROOT", os.path.dirname(os.path.abspath(__file__)))
TOKEN_FILE = os.path.join(CONF, "token")
SCAN_SECONDS = 5

BOLD, DIM, GREEN, AMBER, RED, OFF = (
    "\033[1m", "\033[2m", "\033[32m", "\033[33m", "\033[31m", "\033[0m")


def read_token() -> str:
    try:
        with open(TOKEN_FILE) as f:
            return f.read().strip()
    except OSError:
        return ""


def bridge_pid() -> str:
    """PID de quem escuta a porta da ponte, ou vazio."""
    try:
        out = subprocess.run(["lsof", "-nP", f"-iTCP:{PORT}", "-sTCP:LISTEN", "-t"],
                             capture_output=True, text=True, timeout=5).stdout
        return out.split()[0] if out.strip() else ""
    except (OSError, subprocess.SubprocessError):
        return ""


def panels() -> list:
    """IPs conectados à ponte agora."""
    try:
        out = subprocess.run(["lsof", "-nP", f"-iTCP:{PORT}"],
                             capture_output=True, text=True, timeout=5).stdout
    except (OSError, subprocess.SubprocessError):
        return []
    found = []
    for line in out.splitlines():
        if "ESTABLISHED" not in line:
            continue
        m = re.search(r"->([0-9.]+):", line)
        if m and m.group(1) not in found:
            found.append(m.group(1))
    return found


def host_name() -> str:
    """Nome curto desta máquina — vira o rótulo do host no painel (cabe em 15)."""
    return socket.gethostname().split(".")[0][:15]


def lan_ip(peer: str) -> str:
    """IP desta máquina na rota até o painel (evita chutar a interface certa)."""
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        s.connect((peer, 9))
        return s.getsockname()[0]
    finally:
        s.close()


def clear() -> None:
    sys.stdout.write("\033[2J\033[H")


def row(key: str, value: str) -> None:
    """Linha rotulada do status, com os valores na mesma coluna nos dois idiomas."""
    label = t(key)
    print(f"  {DIM}{label}{OFF}{' ' * max(2, 9 - len(label))}{value}")


def status_screen() -> None:
    clear()
    tok, pid = read_token(), bridge_pid()
    conn = panels()
    print(f"{BOLD}{t('title')}{OFF}\n")
    row("host", host_name())
    if pid:
        row("bridge", f"{GREEN}{t('bridge.up')}{OFF} "
                      f"({t('bridge.where', pid=pid, port=PORT)})")
    else:
        row("bridge", f"{RED}{t('bridge.down')}{OFF} — {t('bridge.hint')}")
    row("token", tok or RED + t("token.missing") + OFF)
    if conn:
        row("panels", f"{GREEN}{t('panels.some', n=len(conn))}{OFF}"
                      f" — {', '.join(conn)}")
    else:
        row("panels", t("panels.none"))
    # as teclas ficam em negrito dentro da própria string traduzida
    menu = t("menu").replace("[", BOLD + "[").replace("]", "]" + OFF)
    print(f"\n  {menu}")


def discover() -> list:
    """Escuta os anúncios dos painéis em modo de pareamento."""
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    try:
        s.bind(("", PAIR_PORT))
    except OSError as e:
        print(f"  {RED}{t('pair.listen_er', port=PAIR_PORT, err=e)}{OFF}")
        return []
    s.settimeout(0.5)
    found, deadline = {}, time.time() + SCAN_SECONDS
    while time.time() < deadline:
        try:
            data, addr = s.recvfrom(512)
        except socket.timeout:
            continue
        try:
            msg = json.loads(data)
        except (ValueError, UnicodeDecodeError):
            continue
        if msg.get("t") == "herdr-assist" and msg.get("id"):
            found[msg["id"]] = (addr[0], int(msg.get("port", PAIR_PORT)))
        left = int(deadline - time.time())
        sys.stdout.write("\r  " + t("pair.searching", left=left, n=len(found)) + "   ")
        sys.stdout.flush()
    s.close()
    print()
    return [(i, ip, p) for i, (ip, p) in sorted(found.items())]


def send_config(ip: str, port: int, token: str) -> bool:
    payload = {"t": "pair", "name": host_name(), "host": lan_ip(ip),
               "port": PORT, "token": token}
    try:
        s = socket.create_connection((ip, port), timeout=8)
        s.settimeout(8)
        s.sendall((json.dumps(payload) + "\n").encode())
        reply = s.recv(256).decode(errors="replace")
        s.close()
    except OSError as e:
        print(f"  {RED}{t('pair.send_fail', err=e)}{OFF}")
        return False
    try:
        return bool(json.loads(reply).get("ok"))
    except (ValueError, AttributeError):
        return False


def pair_flow() -> None:
    clear()
    token = read_token()
    if not token:
        print(f"  {RED}{t('pair.no_token')}{OFF}")
        return
    print(f"{BOLD}{t('pair.title')}{OFF}\n")
    print(f"  {DIM}{t('pair.where')}{OFF}\n")
    devices = discover()
    if not devices:
        print(f"\n  {AMBER}{t('pair.none')}{OFF}")
        print(f"  {DIM}{t('pair.none_hint')}{OFF}")
        return
    print()
    for n, (dev_id, ip, _) in enumerate(devices, 1):
        print(f"  {BOLD}{n}{OFF}) {dev_id}   {DIM}{ip}{OFF}")
    print(f"\n  {DIM}{t('pair.check')}{OFF}")
    try:
        choice = input("\n  " + t("pair.prompt")).strip()
    except (EOFError, KeyboardInterrupt):
        return
    if not choice.isdigit() or not (1 <= int(choice) <= len(devices)):
        return
    dev_id, ip, port = devices[int(choice) - 1]
    print("\n  " + t("pair.sending", dev=dev_id))
    if send_config(ip, port, token):
        print(f"  {GREEN}{t('pair.ok')}{OFF} {t('pair.ok_hint', host=host_name())}")
    else:
        print(f"  {RED}{t('pair.refused')}{OFF}")


def rotate_token() -> None:
    clear()
    print(f"{BOLD}{t('rotate.title')}{OFF}\n")
    print(f"  {AMBER}{t('rotate.warn1')}{OFF}")
    print(f"  {AMBER}{t('rotate.warn2')}{OFF}")
    word = t("rotate.confirm")
    try:
        if input("\n  " + t("rotate.prompt", word=word)).strip() != word:
            print("  " + t("rotate.cancel"))
            return
    except (EOFError, KeyboardInterrupt):
        return
    import secrets
    new = secrets.token_hex(16)
    os.makedirs(CONF, exist_ok=True)
    with open(TOKEN_FILE, "w") as f:
        f.write(new + "\n")
    os.chmod(TOKEN_FILE, 0o600)
    print(f"\n  {t('rotate.new')}{BOLD}{new}{OFF}")
    restart_bridge(quiet=True)
    print(f"  {GREEN}{t('rotate.done')}{OFF}")


# O Herdr não deixa plugin embarcar keybind (decisão de design: as teclas são
# do config do usuário), então o caminho suportado é escrever no config POR ele,
# com consentimento — o mesmo idioma do `herdr config reset-keys`, que também
# edita o arquivo guardando backup. prefix+a está livre nos defaults do 0.8.0.
KEYBIND_BLOCK = """
# herdr-assist: open the admin screen (installed by the plugin)
[[keys.command]]
key = "prefix+a"
type = "plugin_action"
command = "herdr-assist.admin"
description = "herdr-assist admin"
"""


def herdr_bin() -> str:
    """Binário do herdr — o PATH do ambiente do plugin pode não o conter."""
    return os.environ.get("HERDR_BIN_PATH", "herdr")


def install_keybind() -> None:
    clear()
    print(f"{BOLD}{t('kb.title')}{OFF}\n")
    cfg = os.path.join(os.environ.get("XDG_CONFIG_HOME",
                                      os.path.expanduser("~/.config")),
                       "herdr", "config.toml")
    try:
        with open(cfg) as f:
            current = f.read()
    except OSError:
        current = ""
    if "herdr-assist.admin" in current:
        print(f"  {GREEN}{t('kb.already')}{OFF}")
        return
    # busca textual proposital: pega a tecla até em comentário e aborta — na
    # dúvida, melhor mandar para o caminho manual do que criar bind duplicado
    if '"prefix+a"' in current:
        print(f"  {AMBER}{t('kb.conflict')}{OFF}")
        print(f"  {DIM}{t('kb.conflict_hint')}{OFF}")
        return
    print(f"  {t('kb.what')}")
    print(f"  {t('kb.where', path=cfg.replace(os.path.expanduser('~'), '~'))}")
    try:
        if input("\n  " + t("kb.prompt")).strip():
            return
    except (EOFError, KeyboardInterrupt):
        return
    backup = f"{cfg}.bak.{int(time.time())}" if current else ""
    os.makedirs(os.path.dirname(cfg), exist_ok=True)
    if backup:
        shutil.copy2(cfg, backup)

    def undo() -> None:
        if backup:
            shutil.move(backup, cfg)
        elif os.path.exists(cfg):
            os.remove(cfg)

    with open(cfg, "a") as f:
        f.write(KEYBIND_BLOCK)
    try:
        r = subprocess.run([herdr_bin(), "config", "check"],
                           capture_output=True, text=True, timeout=15)
    except (OSError, subprocess.SubprocessError) as e:
        undo()
        print(f"\n  {RED}{t('kb.check_fail')}{OFF}\n  {e}")
        return
    if r.returncode != 0:
        undo()
        print(f"\n  {RED}{t('kb.check_fail')}{OFF}")
        print("  " + (r.stderr or r.stdout).strip())
        return
    try:
        r = subprocess.run([herdr_bin(), "server", "reload-config"],
                           capture_output=True, text=True, timeout=15)
        reloaded = r.returncode == 0
    except (OSError, subprocess.SubprocessError):
        reloaded = False
    print(f"\n  {GREEN}{t('kb.done')}{OFF}")
    if not reloaded:
        print(f"  {AMBER}{t('kb.reload_warn')}{OFF}")


def restart_bridge(quiet: bool = False) -> None:
    if not quiet:
        clear()
        print(f"{BOLD}{t('restart.title')}{OFF}\n")
    r = subprocess.run([os.path.join(ROOT, "start.sh"), "--restart"],
                       capture_output=True, text=True)
    out = (r.stdout + r.stderr).strip()
    if not quiet:
        print(f"  {out}")


def main() -> None:
    while True:
        status_screen()
        try:
            key = input("\n  > ").strip().lower()
        except (EOFError, KeyboardInterrupt):
            return
        if key == "q":
            return
        if key == "p":
            pair_flow()
        elif key == "r":
            rotate_token()
        elif key == "x":
            restart_bridge()
        elif key == "k":
            install_keybind()
        else:
            continue
        try:
            input(f"\n  {DIM}{t('back')}{OFF}")
        except (EOFError, KeyboardInterrupt):
            return


if __name__ == "__main__":
    main()
