#!/usr/bin/env python3
"""Abre a tela de administração do plugin.

Existe como script porque a ação precisa rodar nos três sistemas e o Windows
não tem `sh -c`. HERDR_BIN_PATH e não "herdr": a ação roda com o PATH do
ambiente do plugin, onde o binário pode não estar (é o caso de um Homebrew não
exportado).
"""
import os
import shutil
import subprocess
import sys

NO_WINDOW = 0x08000000 if os.name == "nt" else 0
binario = os.environ.get("HERDR_BIN_PATH") or shutil.which("herdr")
if not binario:
    print("herdr binary not found (set HERDR_BIN_PATH)", file=sys.stderr)
    sys.exit(1)

sys.exit(subprocess.run(
    [binario, "plugin", "pane", "open", "--plugin", "herdr-assist",
     "--entrypoint", "admin"],
    creationflags=NO_WINDOW).returncode)
