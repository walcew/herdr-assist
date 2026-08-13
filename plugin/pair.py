#!/usr/bin/env python3
"""Pareia um painel herdr-assist por linha de comando.

A tela de administração (admin.py) faz isto e mais, mas ela é curses — que não
existe na stdlib do Windows. Aqui é o mesmo protocolo em texto puro: serve o
Windows, serve uma sessão por SSH sem TUI, e serve para script.

Fluxo: o painel entra em modo de pareamento e se anuncia por broadcast UDP na
porta 9376; este script escuta os anúncios, mostra os códigos e entrega ao
escolhido a configuração pronta (nome do host, endereço, porta e token). O
painel grava na NVS e reinicia já conectado — nada é digitado no aparelho.

    python3 pair.py            # escuta, lista e pergunta
    python3 pair.py --wait 60  # janela maior de escuta
    python3 pair.py --id A1B2C3  # pareia direto com esse painel
"""
import argparse
import json
import os
import socket
import sys
import time

PORT = int(os.environ.get("BRIDGE_PORT", "9375"))
PAIR_PORT = 9376
CONF = os.environ.get("HERDR_PLUGIN_CONFIG_DIR",
                      os.path.expanduser("~/.config/herdr-assist"))
TOKEN_FILE = os.path.join(CONF, "token")


def read_token() -> str:
    try:
        with open(TOKEN_FILE, encoding="utf-8") as fh:
            return fh.read().strip()
    except OSError:
        return ""


def host_name() -> str:
    """Nome curto desta máquina — vira o rótulo do host no painel (cabe em 15)."""
    return socket.gethostname().split(".")[0][:15]


def lan_ip(peer: str) -> str:
    """IP desta máquina na rota até o painel (evita chutar a interface errada)."""
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        s.connect((peer, 9))
        return s.getsockname()[0]
    finally:
        s.close()


def scan(seconds: float, parar_no_primeiro: bool = True) -> dict:
    """Escuta anúncios e devolve {id: (ip, porta)}. Imprime cada novidade.

    `seconds` é o teto de espera, não a duração: assim que um painel aparece,
    a escuta segue só mais um instante (GRACA_S) para capturar um segundo
    aparelho e então devolve. Esperar a janela inteira faria o caso comum — um
    painel só, anunciando de imediato — demorar o tempo do pior caso, e o
    modo de pareamento do painel dura apenas 3 minutos.
    """
    GRACA_S = 1.5
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    try:
        s.bind(("", PAIR_PORT))
    except OSError as exc:
        print("nao consegui escutar na porta %d: %s" % (PAIR_PORT, exc),
              file=sys.stderr)
        return {}
    s.settimeout(0.5)
    achados: dict = {}
    fim = time.time() + seconds
    limite_graca = None
    while time.time() < fim and (limite_graca is None or time.time() < limite_graca):
        try:
            data, addr = s.recvfrom(512)
        except socket.timeout:
            continue
        except OSError:
            break
        try:
            msg = json.loads(data)
        except (ValueError, UnicodeDecodeError):
            continue
        if msg.get("t") != "herdr-assist" or not msg.get("id"):
            continue
        pid = msg["id"]
        if pid not in achados:
            print("  painel %s em %s" % (pid, addr[0]), flush=True)
        achados[pid] = (addr[0], int(msg.get("port", PAIR_PORT)))
        if parar_no_primeiro and limite_graca is None:
            limite_graca = time.time() + GRACA_S
    s.close()
    return achados


def send_config(ip: str, port: int, token: str):
    """Manda a config pronta ao painel; (aceitou, erro_de_transporte)."""
    # auto: firmware com descoberta grava o slot sem IP (o broadcast resolve o
    # endereço em runtime, então DHCP trocar o IP não quebra o pareamento)
    payload = {"t": "pair", "name": host_name(), "host": lan_ip(ip),
               "port": PORT, "token": token, "auto": True}
    try:
        s = socket.create_connection((ip, port), timeout=8)
        s.settimeout(8)
        s.sendall((json.dumps(payload) + "\n").encode())
        reply = s.recv(256).decode(errors="replace")
        s.close()
    except OSError as exc:
        return False, str(exc)
    try:
        return bool(json.loads(reply).get("ok")), ""
    except (ValueError, AttributeError):
        return False, ""


def main() -> int:
    ap = argparse.ArgumentParser(description="pareia um painel herdr-assist")
    ap.add_argument("--wait", type=float, default=20,
                    help="segundos de escuta (padrao 20)")
    ap.add_argument("--id", help="parear direto com este painel, sem perguntar")
    args = ap.parse_args()

    token = read_token()
    if not token:
        print("sem token em %s — suba a ponte uma vez antes de parear" % TOKEN_FILE,
              file=sys.stderr)
        return 1

    print("no painel: Settings -> Parear com um host", flush=True)
    print("escutando (ate %gs) na porta %d..." % (args.wait, PAIR_PORT), flush=True)
    # com --id explicito vale ouvir a janela toda: o painel pedido pode
    # nao ser o primeiro a se anunciar
    achados = scan(args.wait, parar_no_primeiro=not args.id)
    if not achados:
        print("nenhum painel se anunciou.\n"
              "  - o painel esta em modo de pareamento? (a janela dura 3 min)\n"
              "  - esta na mesma rede e segmento que esta maquina?\n"
              "  - o firewall do Windows libera UDP %d de entrada?" % PAIR_PORT,
              file=sys.stderr)
        return 1

    if args.id:
        alvo = args.id
    elif len(achados) == 1:
        alvo = next(iter(achados))
        print("um painel encontrado: %s" % alvo)
    else:
        itens = sorted(achados)
        for i, pid in enumerate(itens, 1):
            print("  %d) %s  (%s)" % (i, pid, achados[pid][0]))
        try:
            escolha = int(input("qual? ").strip())
            alvo = itens[escolha - 1]
        except (ValueError, IndexError, EOFError):
            print("escolha invalida", file=sys.stderr)
            return 1

    if alvo not in achados:
        print("painel %s nao esta na lista" % alvo, file=sys.stderr)
        return 1

    ip, porta = achados[alvo]
    ok, erro = send_config(ip, porta, token)
    if ok:
        print("pareado com %s (%s) — o painel reinicia e volta conectado" % (alvo, ip))
        return 0
    print("o painel recusou a configuracao%s" % (": " + erro if erro else ""),
          file=sys.stderr)
    return 1


if __name__ == "__main__":
    sys.exit(main())
