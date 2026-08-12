#!/usr/bin/env python3
# /// script
# requires-python = ">=3.9"
# ///
"""Tela de administração da ponte, aberta como popup do Herdr.

TUI curses no espírito da TUI do próprio Herdr: tudo é clicável (o ghostty
embutido repassa o mouse reporting a quem o pedir) e tudo tem tecla — os
atalhos p/r/x/k/q de sempre, mais setas+enter. O status atualiza sozinho,
coisa que o loop de input() da versão anterior impedia.

Reúne o que antes exigia arqueologia na CLI: token, estado da ponte, painéis
conectados — e, principalmente, o pareamento. Digitar 32 caracteres
hexadecimais num touch de 3,5" é inviável, então o painel entra em modo de
pareamento, se anuncia por broadcast, e é daqui que a configuração sai pronta.

Só stdlib (curses incluso); roda no python3 de fábrica do macOS.
"""
from __future__ import annotations

import curses
import json
import locale
import os
import re
import shutil
import socket
import subprocess
import time

from i18n import t

PORT = int(os.environ.get("BRIDGE_PORT", "9375"))
PAIR_PORT = 9376
CONF = os.environ.get("HERDR_PLUGIN_CONFIG_DIR",
                      os.path.expanduser("~/.config/herdr-assist"))
STATE = os.environ.get("HERDR_PLUGIN_STATE_DIR", CONF)
ROOT = os.environ.get("HERDR_PLUGIN_ROOT", os.path.dirname(os.path.abspath(__file__)))
TOKEN_FILE = os.path.join(CONF, "token")


# --- estado da ponte e do host ----------------------------------------------

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


def poll_status() -> dict:
    return {"tok": read_token(), "pid": bridge_pid(), "conn": panels()}


def bridge_restart() -> str:
    r = subprocess.run([os.path.join(ROOT, "start.sh"), "--restart"],
                       capture_output=True, text=True)
    return (r.stdout + r.stderr).strip()


def token_rotate() -> str:
    """Gera e grava um token novo e recicla a ponte; devolve o token."""
    import secrets
    new = secrets.token_hex(16)
    os.makedirs(CONF, exist_ok=True)
    with open(TOKEN_FILE, "w") as f:
        f.write(new + "\n")
    os.chmod(TOKEN_FILE, 0o600)
    bridge_restart()
    return new


# --- pareamento ---------------------------------------------------------------

class PairScan:
    """Escuta os anúncios de pareamento sem bloquear o loop da TUI."""

    def __init__(self):
        self.devices: dict = {}
        self.error = ""
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        try:
            self.sock.bind(("", PAIR_PORT))
            self.sock.setblocking(False)
        except OSError as e:
            self.error = t("pair.listen_er", port=PAIR_PORT, err=e)
            self.sock.close()
            self.sock = None

    def pump(self) -> None:
        """Drena os datagramas já recebidos; volta na hora se não houver."""
        while self.sock:
            try:
                data, addr = self.sock.recvfrom(512)
            except OSError:
                return
            try:
                msg = json.loads(data)
            except (ValueError, UnicodeDecodeError):
                continue
            if msg.get("t") == "herdr-assist" and msg.get("id"):
                self.devices[msg["id"]] = (addr[0], int(msg.get("port", PAIR_PORT)))

    def listed(self) -> list:
        return [(i, ip, p) for i, (ip, p) in sorted(self.devices.items())]

    def close(self) -> None:
        if self.sock:
            self.sock.close()
            self.sock = None


def send_config(ip: str, port: int, token: str) -> "tuple[bool, str]":
    """Manda a config pronta ao painel; (aceitou, erro_de_transporte)."""
    # auto: firmware com descoberta grava o slot sem IP (broadcast resolve);
    # firmware antigo ignora o campo e usa o host, como sempre
    payload = {"t": "pair", "name": host_name(), "host": lan_ip(ip),
               "port": PORT, "token": token, "auto": True}
    try:
        s = socket.create_connection((ip, port), timeout=8)
        s.settimeout(8)
        s.sendall((json.dumps(payload) + "\n").encode())
        reply = s.recv(256).decode(errors="replace")
        s.close()
    except OSError as e:
        return False, str(e)
    try:
        return bool(json.loads(reply).get("ok")), ""
    except (ValueError, AttributeError):
        return False, ""


# --- atalho de teclado no config do usuário -----------------------------------

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


def herdr_config_path() -> str:
    return os.path.join(os.environ.get("XDG_CONFIG_HOME",
                                       os.path.expanduser("~/.config")),
                        "herdr", "config.toml")


def read_herdr_config() -> str:
    try:
        with open(herdr_config_path()) as f:
            return f.read()
    except OSError:
        return ""


def keybind_state(text: str) -> str:
    """O que o config do usuário já diz sobre o nosso atalho."""
    if "herdr-assist.admin" in text:
        return "installed"
    # busca textual proposital: pega a tecla até em comentário e aborta — na
    # dúvida, melhor mandar para o caminho manual do que criar bind duplicado
    if '"prefix+a"' in text:
        return "conflict"
    return "free"


def keybind_apply() -> "tuple[bool, str, bool]":
    """Acrescenta o bloco ao config com backup; (ok, detalhe_do_erro, reloaded)."""
    cfg = herdr_config_path()
    current = read_herdr_config()
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
        return False, str(e), False
    if r.returncode != 0:
        undo()
        return False, (r.stderr or r.stdout).strip(), False
    try:
        r = subprocess.run([herdr_bin(), "server", "reload-config"],
                           capture_output=True, text=True, timeout=15)
        reloaded = r.returncode == 0
    except (OSError, subprocess.SubprocessError):
        reloaded = False
    return True, "", reloaded


# --- TUI ------------------------------------------------------------------------
# Cores por papel, os mesmos da versão ANSI; "dim" é A_DIM, sem par próprio.
C_OK, C_WARN, C_ERR = 1, 2, 3

MENU = (
    ("p", "act.pair"),
    ("r", "act.rotate"),
    ("x", "act.restart"),
    ("k", "act.keybind"),
    ("q", "act.quit"),
)


def colors_init() -> None:
    curses.use_default_colors()
    curses.init_pair(C_OK, curses.COLOR_GREEN, -1)
    curses.init_pair(C_WARN, curses.COLOR_YELLOW, -1)
    curses.init_pair(C_ERR, curses.COLOR_RED, -1)


def put(scr, y: int, x: int, text: str, attr: int = 0) -> None:
    """addstr que não estoura a janela (fora dela o curses lança erro)."""
    h, w = scr.getmaxyx()
    if 0 <= y < h and x < w:
        try:
            scr.addstr(y, x, text[: max(0, w - x - 1)], attr)
        except curses.error:
            pass


class Hits:
    """Regiões clicáveis do frame atual; remontada a cada render."""

    def __init__(self):
        self.zones: list = []

    def add(self, y: int, x0: int, x1: int, value) -> None:
        self.zones.append((y, x0, x1, value))

    def at(self, y: int, x: int):
        for zy, x0, x1, value in self.zones:
            if y == zy and x0 <= x <= x1:
                return value
        return None


def read_click() -> "tuple[int, int] | None":
    """(linha, coluna) do botão 1, ou None. Com mouseinterval(0) o clique
    chega como PRESSED+RELEASED separados; agir no PRESSED evita disparo duplo."""
    try:
        _, mx, my, _, bstate = curses.getmouse()
    except curses.error:
        return None
    if bstate & (curses.BUTTON1_PRESSED | curses.BUTTON1_CLICKED):
        return my, mx
    return None


def wait_key(scr) -> None:
    """Qualquer tecla (ou clique) volta; solta o RELEASED perdido do mouse."""
    scr.timeout(-1)
    while True:
        ch = scr.getch()
        if ch == curses.KEY_RESIZE:
            continue
        if ch == curses.KEY_MOUSE:
            if read_click() is None:
                continue
        return


def draw_status(scr, y: int, st: dict) -> int:
    """Bloco de status; devolve a linha seguinte. Valores na coluna 13 para
    ficarem alinhados nos dois idiomas (rótulo mais largo: 'Painéis')."""
    def row(yy: int, key: str, text: str, attr: int = 0) -> None:
        put(scr, yy, 2, t(key), curses.A_DIM)
        put(scr, yy, 13, text, attr)

    row(y, "host", host_name())
    if st["pid"]:
        row(y + 1, "bridge",
            t("bridge.up") + " (" + t("bridge.where", pid=st["pid"], port=PORT) + ")",
            curses.color_pair(C_OK))
    else:
        row(y + 1, "bridge", t("bridge.down") + " — " + t("bridge.hint"),
            curses.color_pair(C_ERR))
    row(y + 2, "token", st["tok"] or t("token.missing"),
        0 if st["tok"] else curses.color_pair(C_ERR))
    if st["conn"]:
        row(y + 3, "panels",
            t("panels.some", n=len(st["conn"])) + " — " + ", ".join(st["conn"]),
            curses.color_pair(C_OK))
    else:
        row(y + 3, "panels", t("panels.none"), curses.A_DIM)
    return y + 4


def screen_pair(scr) -> None:
    token = read_token()
    if not token:
        scr.erase()
        put(scr, 1, 2, t("pair.title"), curses.A_BOLD)
        put(scr, 3, 2, t("pair.no_token"), curses.color_pair(C_ERR))
        put(scr, 5, 2, t("hint.back"), curses.A_DIM)
        scr.refresh()
        wait_key(scr)
        return
    scan = PairScan()
    sel = 0
    scr.timeout(250)
    try:
        while True:
            scan.pump()
            devices = scan.listed()
            sel = min(sel, max(0, len(devices) - 1))
            hits = Hits()
            scr.erase()
            put(scr, 1, 2, t("pair.title"), curses.A_BOLD)
            put(scr, 3, 2, t("pair.where"), curses.A_DIM)
            if scan.error:
                put(scr, 5, 2, scan.error, curses.color_pair(C_ERR))
                put(scr, 7, 2, t("hint.back"), curses.A_DIM)
                scr.refresh()
                wait_key(scr)
                return
            put(scr, 5, 2, t("pair.listening", n=len(devices)),
                curses.color_pair(C_WARN))
            y = 7
            for i, (dev_id, ip, _p) in enumerate(devices):
                attr = curses.A_REVERSE if i == sel else 0
                text = f" {i + 1}) {dev_id}   {ip} "
                put(scr, y + i, 3, text, attr)
                hits.add(y + i, 3, 3 + len(text) - 1, i)
            if devices:
                put(scr, y + len(devices) + 1, 2, t("pair.check"), curses.A_DIM)
            scr.refresh()

            ch = scr.getch()
            pick = None
            if ch == -1 or ch == curses.KEY_RESIZE:
                continue
            if ch == curses.KEY_MOUSE:
                pos = read_click()
                v = hits.at(*pos) if pos else None
                if v is None:
                    continue
                sel, pick = v, v
            elif ch == 27:
                return
            elif ch == curses.KEY_UP and devices:
                sel = (sel - 1) % len(devices)
                continue
            elif ch == curses.KEY_DOWN and devices:
                sel = (sel + 1) % len(devices)
                continue
            elif ch in (curses.KEY_ENTER, 10, 13) and devices:
                pick = sel
            elif ord("1") <= ch <= ord("9") and ch - ord("1") < len(devices):
                pick = ch - ord("1")
            if pick is None:
                continue

            dev_id, ip, port = devices[pick]
            yy = y + len(devices) + 3
            put(scr, yy, 2, t("pair.sending", dev=dev_id))
            scr.refresh()
            ok, err = send_config(ip, port, token)  # bloqueia ≤8 s, com aviso na tela
            if ok:
                put(scr, yy + 1, 2,
                    t("pair.ok") + " " + t("pair.ok_hint", host=host_name()),
                    curses.color_pair(C_OK))
            elif err:
                put(scr, yy + 1, 2, t("pair.send_fail", err=err),
                    curses.color_pair(C_ERR))
            else:
                put(scr, yy + 1, 2, t("pair.refused"), curses.color_pair(C_ERR))
            put(scr, yy + 3, 2, t("hint.back"), curses.A_DIM)
            scr.refresh()
            wait_key(scr)
            return
    finally:
        scan.close()


def screen_rotate(scr) -> None:
    scr.erase()
    put(scr, 1, 2, t("rotate.title"), curses.A_BOLD)
    put(scr, 3, 2, t("rotate.warn1"), curses.color_pair(C_WARN))
    put(scr, 4, 2, t("rotate.warn2"), curses.color_pair(C_WARN))
    word = t("rotate.confirm")
    prompt = t("rotate.prompt", word=word)
    put(scr, 6, 2, prompt)
    scr.refresh()
    # a fricção de digitar SIM/YES fica: rotação derruba todos os painéis pareados
    curses.echo()
    curses.curs_set(1)
    scr.timeout(-1)
    try:
        typed = scr.getstr(6, 2 + len(prompt), 12).decode("utf-8", "replace").strip()
    except curses.error:
        typed = ""
    finally:
        curses.noecho()
        curses.curs_set(0)
    if typed != word:
        put(scr, 8, 2, t("rotate.cancel"), curses.A_DIM)
    else:
        new = token_rotate()
        put(scr, 8, 2, t("rotate.new") + new, curses.A_BOLD)
        put(scr, 9, 2, t("rotate.done"), curses.color_pair(C_OK))
    put(scr, 11, 2, t("hint.back"), curses.A_DIM)
    scr.refresh()
    wait_key(scr)


def screen_keybind(scr) -> None:
    state = keybind_state(read_herdr_config())
    scr.erase()
    put(scr, 1, 2, t("kb.title"), curses.A_BOLD)
    if state == "installed":
        put(scr, 3, 2, t("kb.already"), curses.color_pair(C_OK))
        put(scr, 5, 2, t("hint.back"), curses.A_DIM)
        scr.refresh()
        wait_key(scr)
        return
    if state == "conflict":
        put(scr, 3, 2, t("kb.conflict"), curses.color_pair(C_WARN))
        put(scr, 4, 2, t("kb.conflict_hint"), curses.A_DIM)
        put(scr, 6, 2, t("hint.back"), curses.A_DIM)
        scr.refresh()
        wait_key(scr)
        return

    shown = herdr_config_path().replace(os.path.expanduser("~"), "~")
    put(scr, 3, 2, t("kb.what"))
    put(scr, 4, 2, t("kb.where", path=shown))
    hits = Hits()
    b1 = f"[ {t('btn.install')} ]"
    b2 = f"[ {t('btn.cancel')} ]"
    x2 = 4 + len(b1) + 3
    put(scr, 6, 4, b1, curses.A_BOLD)
    hits.add(6, 4, 4 + len(b1) - 1, "install")
    put(scr, 6, x2, b2)
    hits.add(6, x2, x2 + len(b2) - 1, "cancel")
    put(scr, 8, 2, t("hint.confirm"), curses.A_DIM)
    scr.refresh()

    scr.timeout(-1)
    while True:
        ch = scr.getch()
        choice = None
        if ch == curses.KEY_MOUSE:
            pos = read_click()
            choice = hits.at(*pos) if pos else None
        elif ch in (curses.KEY_ENTER, 10, 13):
            choice = "install"
        elif ch == 27:
            choice = "cancel"
        if choice == "cancel":
            return
        if choice == "install":
            break

    ok, detail, reloaded = keybind_apply()
    y = 10
    if ok:
        put(scr, y, 2, t("kb.done"), curses.color_pair(C_OK))
        if not reloaded:
            y += 1
            put(scr, y, 2, t("kb.reload_warn"), curses.color_pair(C_WARN))
    else:
        put(scr, y, 2, t("kb.check_fail"), curses.color_pair(C_ERR))
        y += 1
        put(scr, y, 2, detail[:200], curses.A_DIM)
    put(scr, y + 2, 2, t("hint.back"), curses.A_DIM)
    scr.refresh()
    wait_key(scr)


def main_screen(scr) -> None:
    sel = 0
    note = ("", 0.0)  # mensagem transitória do restart: (texto, expira_em)
    st, st_at = poll_status(), time.time()
    while True:
        scr.timeout(250)
        if time.time() - st_at > 2:  # lsof a cada ~2 s, não a cada tick
            st, st_at = poll_status(), time.time()
        hits = Hits()
        scr.erase()
        put(scr, 1, 2, t("title"), curses.A_BOLD)
        y = draw_status(scr, 3, st) + 1
        for i, (key, name) in enumerate(MENU):
            attr = curses.A_REVERSE if i == sel else 0
            text = f" [{key}] {t(name)} "
            put(scr, y + i, 3, text, attr)
            hits.add(y + i, 3, 3 + len(text) - 1, i)
        hy = y + len(MENU) + 1
        if note[0] and time.time() < note[1]:
            put(scr, hy, 2, note[0], curses.color_pair(C_WARN))
        else:
            put(scr, hy, 2, t("hint.main"), curses.A_DIM)
        scr.refresh()

        ch = scr.getch()
        act = None
        if ch == -1 or ch == curses.KEY_RESIZE:
            continue
        if ch == curses.KEY_MOUSE:
            pos = read_click()
            v = hits.at(*pos) if pos else None
            if v is None:
                continue
            sel, act = v, MENU[v][0]
        elif ch == curses.KEY_UP:
            sel = (sel - 1) % len(MENU)
            continue
        elif ch == curses.KEY_DOWN:
            sel = (sel + 1) % len(MENU)
            continue
        elif ch in (curses.KEY_ENTER, 10, 13):
            act = MENU[sel][0]
        elif ch == 27:
            act = "q"
        elif 0 <= ch < 256 and chr(ch).lower() in "prxkq":
            act = chr(ch).lower()
        if act is None:
            continue

        if act == "q":
            return
        if act == "p":
            screen_pair(scr)
        elif act == "r":
            screen_rotate(scr)
        elif act == "k":
            screen_keybind(scr)
        elif act == "x":
            put(scr, hy, 2, " " * 70)
            put(scr, hy, 2, t("restarting"), curses.color_pair(C_WARN))
            scr.refresh()
            out = bridge_restart()  # bloqueia ~1,5 s (start.sh dorme 1 s)
            note = (out, time.time() + 5)
        st_at = 0.0  # força releitura do status ao voltar


def tui(scr) -> None:
    try:
        curses.curs_set(0)
    except curses.error:
        pass
    colors_init()
    scr.keypad(True)
    curses.mousemask(curses.ALL_MOUSE_EVENTS)
    curses.mouseinterval(0)
    if hasattr(curses, "set_escdelay"):
        curses.set_escdelay(25)  # Esc responde na hora (padrão é ~1 s)
    main_screen(scr)


def main() -> None:
    try:
        locale.setlocale(locale.LC_ALL, "")  # UTF-8 nos addstr (acentos do pt_BR)
    except locale.Error:
        pass  # env sem LANG válido: segue com o locale C, só perde acento
    try:
        curses.wrapper(tui)
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
