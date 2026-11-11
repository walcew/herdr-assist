"""O que a Ponte (herdr_bridge.py) e o ctl.py precisam dizer igual.

Só constantes e funções puras: importar não abre conexão nem lê arquivo.
"""
from __future__ import annotations

import os
import shutil
import socket

# Binário do herdr-core: a variável vence (é o que injeta o hc falso nos
# testes), senão o PATH, senão onde o instalador o põe.
HC_BIN = (os.environ.get("HC_BIN") or shutil.which("hc")
          or os.path.expanduser("~/.local/bin/hc"))
# Doc do core com a configuração da Ponte; outro nome só nos testes.
DOC_NAME = os.environ.get("HERDR_ASSIST_DOC", "herdr-assist")
DEFAULT_PORT = 9375


def valid_port(p) -> bool:
    """Porta TCP válida; bool fica de fora (True é int em Python)."""
    return isinstance(p, int) and not isinstance(p, bool) and 0 < p < 65536


def host_name() -> str:
    """Nome curto desta máquina; vira o rótulo do host no painel (cabe em 15)."""
    return socket.gethostname().split(".")[0][:15]


def route_ip(peer: str) -> str:
    """IP desta máquina na rota até `peer`; "" sem rota. UDP connect não manda pacote."""
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        s.connect((peer, 9))
        return s.getsockname()[0]
    except OSError:
        return ""
    finally:
        s.close()
