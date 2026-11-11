#!/usr/bin/env python3
"""Sobe a ponte herdr-assist destacada e sai.

O startup de plugin do Herdr roda uma vez por subida da sessão e não
supervisiona daemons: quem é chamado precisa destacar o processo e terminar.

Substitui o antigo start.sh porque o Windows não tem `sh` nem `nohup`, e a
ponte agora roda nos três sistemas (no Windows ela fala com o Herdr pela CLI —
ver USE_CLI em herdr_bridge.py). O interpretador é o `sys.executable`, então
vale o mesmo Python que o Herdr usou para chamar este script.

Idempotente: se a porta já responde, não sobe uma segunda ponte (o startup
re-executa em live handoff do Herdr). Com --restart, derruba a atual antes.

Config opcional em $HERDR_PLUGIN_CONFIG_DIR/env (KEY=VALUE por linha):
BRIDGE_PORT, BRIDGE_BIND. O token vive em $HERDR_PLUGIN_CONFIG_DIR/token
(gerado pela ponte na primeira subida).

As mensagens saem em inglês (fixo): a tela de administração as ecoa ao
reiniciar a ponte, e ali o idioma é o do locale do host.
"""
import os
import shutil
import signal
import socket
import subprocess
import sys
import time

DIR = os.path.dirname(os.path.abspath(__file__))
CONF = os.environ.get("HERDR_PLUGIN_CONFIG_DIR") or os.path.expanduser("~/.config/herdr-assist")
STATE = os.environ.get("HERDR_PLUGIN_STATE_DIR") or CONF


def load_env() -> None:
    """Aplica o arquivo env do config-dir ao ambiente deste processo.

    O filho herda, que é como BRIDGE_BIND e afins chegam à ponte.
    """
    path = os.path.join(CONF, "env")
    if not os.path.isfile(path):
        return
    with open(path, encoding="utf-8", errors="replace") as fh:
        for line in fh:
            line = line.strip()
            if not line or line.startswith("#") or "=" not in line:
                continue
            key, _, val = line.partition("=")
            os.environ.setdefault(key.strip(), val.strip().strip('"').strip("'"))


def port_free(port: int) -> bool:
    """Porta livre = não há ponte de pé. É o teste que torna isto idempotente."""
    s = socket.socket()
    try:
        s.bind(("127.0.0.1", port))
        return True
    except OSError:
        return False
    finally:
        s.close()


def listener_pid(port: int) -> int:
    """Quem está escutando a porta, ou 0.

    O pidfile é a via rápida, mas não é confiável: ele mora no state-dir, que
    varia com quem chama (o Herdr passa um HERDR_PLUGIN_STATE_DIR próprio), e
    uma ponte subida por outro caminho não deixa arquivo nenhum. A porta é a
    única fonte de verdade sobre o que está no ar.
    """
    try:
        if os.name == "nt":
            out = subprocess.run(["netstat", "-ano", "-p", "TCP"],
                                 capture_output=True, text=True, timeout=5,
                                 creationflags=0x08000000).stdout
            for line in out.splitlines():
                f = line.split()
                if len(f) >= 5 and f[3].upper() == "LISTENING" \
                        and f[1].rsplit(":", 1)[-1] == str(port):
                    return int(f[4])
            return 0
        out = subprocess.run(["lsof", "-nP", "-iTCP:%d" % port, "-sTCP:LISTEN", "-t"],
                             capture_output=True, text=True, timeout=5).stdout
        return int(out.split()[0]) if out.strip() else 0
    except (OSError, ValueError, IndexError, subprocess.SubprocessError):
        return 0


def kill_pid(pid: int) -> None:
    try:
        if os.name == "nt":
            # sem SIGTERM no Windows; taskkill resolve sem trazer dependência
            subprocess.run(["taskkill", "/PID", str(pid), "/F"],
                           capture_output=True, creationflags=0x08000000)
        else:
            os.kill(pid, signal.SIGTERM)
    except (OSError, ValueError):
        pass


def kill_previous(pidfile: str, port: int) -> None:
    """Derruba a ponte que estiver de pé, pelo pidfile ou pela porta."""
    pid = 0
    if os.path.isfile(pidfile):
        try:
            with open(pidfile) as fh:
                pid = int(fh.read().strip())
        except (ValueError, OSError):
            pid = 0
        try:
            os.remove(pidfile)
        except OSError:
            pass
    if pid:
        kill_pid(pid)
        time.sleep(1)
    if port_free(port):
        return
    # O pidfile não existia, apontava para outro processo, ou o sinal não pegou:
    # sem isto o --restart terminaria dizendo "already running", como se tivesse
    # dado certo — e quem depende dele (o auto-update) nunca trocaria de versão.
    other = listener_pid(port)
    if other:
        print("bridge on port %d is pid %d, not the one in the pidfile" % (port, other))
        kill_pid(other)
        time.sleep(1)


def interpretador() -> str:
    """Qual Python roda a ponte.

    Normalmente é o mesmo que rodou este script. A exceção é o Windows com o
    Python da Microsoft Store: `python3` costuma resolver para o alias em
    WindowsApps, e o firewall trata cada binário separadamente — se o usuário
    já negou um prompt para aquele executável, fica uma regra de BLOQUEIO de
    entrada, e bloqueio vence permissão. A ponte precisa aceitar conexão da
    LAN, então quando existe um Python instalado fora da Store, ele é a aposta
    melhor. Localhost não revela o problema: só o painel, na rede, é que fica
    sem conseguir conectar.
    """
    atual = sys.executable
    if os.name != "nt" or "windowsapps" not in atual.lower():
        return atual
    outro = shutil.which("python")
    if outro and "windowsapps" not in outro.lower():
        return outro
    return atual


def spawn(port: int, log_path: str) -> int:
    """Sobe a ponte desligada deste processo e devolve o pid."""
    env = dict(os.environ, BRIDGE_PORT=str(port))
    log = open(log_path, "ab")
    kwargs = {"stdout": log, "stderr": subprocess.STDOUT,
              "stdin": subprocess.DEVNULL, "cwd": DIR, "env": env}
    if os.name == "nt":
        # DETACHED_PROCESS: sobrevive ao fechamento do console que nos chamou.
        # CREATE_NO_WINDOW: sem janela piscando a cada subida.
        kwargs["creationflags"] = 0x00000008 | 0x08000000
    else:
        kwargs["start_new_session"] = True
    proc = subprocess.Popen([interpretador(), os.path.join(DIR, "herdr_bridge.py")], **kwargs)
    return proc.pid


def main() -> int:
    os.makedirs(CONF, exist_ok=True)
    os.makedirs(STATE, exist_ok=True)
    load_env()
    port = int(os.environ.get("BRIDGE_PORT", "9375"))
    pidfile = os.path.join(STATE, "bridge.pid")
    log_path = os.path.join(STATE, "bridge.log")

    if "--restart" in sys.argv:
        kill_previous(pidfile, port)
        if not port_free(port):
            # Falhar alto: reiniciar e não reiniciar precisam ser distinguíveis
            # por quem chamou, e antes isto saía como sucesso.
            print("could not free port %d; bridge NOT restarted" % port)
            return 1

    if not port_free(port):
        print("bridge already running on port %d" % port)
        return 0

    pid = spawn(port, log_path)
    with open(pidfile, "w") as fh:
        fh.write(str(pid))
    print("bridge started (pid %d, port %d, log at %s)" % (pid, port, log_path))
    return 0


if __name__ == "__main__":
    sys.exit(main())
