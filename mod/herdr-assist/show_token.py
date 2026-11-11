#!/usr/bin/env python3
"""Mostra o token da ponte.

O `cat` do antigo `sh -c` não existe no Windows. O token é gerado pela ponte na
primeira subida; se ainda não houver arquivo, o que falta é subir a ponte uma
vez — dizer isso é mais útil que um erro de arquivo ausente.
"""
import os
import sys

conf = os.environ.get("HERDR_PLUGIN_CONFIG_DIR") or os.path.expanduser("~/.config/herdr-assist")
path = os.path.join(conf, "token")
try:
    with open(path, encoding="utf-8") as fh:
        print(fh.read().strip())
except OSError:
    print("no token yet — start the bridge once (action: restart-bridge)",
          file=sys.stderr)
    sys.exit(1)
