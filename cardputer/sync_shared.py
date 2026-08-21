"""Copia para src/shared os módulos que este firmware divide com o painel.

Tudo que é protocolo e estado (config na NVS, conexão com a ponte, descoberta,
pareamento, parse de SGR) não tem uma linha de LVGL e compila igual no Arduino:
é exatamente a parte que não pode divergir entre os dois firmwares — um bug no
parse de SGR ou uma mudança no HMAC da descoberta precisa valer nos dois. Em vez
de duplicar os arquivos no repositório (que divergem em silêncio) ou de apontar
o build_src_filter para fora do projeto (que o PlatformIO trata mal), o build
copia os originais de ../src a cada compilação.

O que muda de mundo fica aqui: net.cpp (Wi-Fi do Arduino implementando o
net.h do painel) e toda a UI.

src/shared é gerado: está no .gitignore e qualquer edição ali se perde no
próximo build. Edite ../src.
"""

import os
import shutil
import subprocess

Import("env")  # noqa: F821  (injetado pelo PlatformIO/SCons)

PANEL_SRC = os.path.normpath(os.path.join(env.subst("$PROJECT_DIR"), "..", "src"))
DEST = os.path.join(env.subst("$PROJECT_SRC_DIR"), "shared")
FILES = (
    # protocolo e estado, iguais nos dois firmwares
    "term_parse.c", "term_parse.h",
    "discovery.c", "discovery.h",
    "panel_cfg.c", "panel_cfg.h",
    "herdr_model.c", "herdr_model.h",
    # dependência do herdr_model: a leitura de limites colapsa por conta
    "limits_merge.c", "limits_merge.h",
    # dependência do herdr_model: o diff de status alimenta o log de atividade
    "activity_log.c", "activity_log.h",
    "herdr_conn.c", "herdr_conn.h",
    "pairing.c", "pairing.h",
    # só o contrato: o Wi-Fi do Cardputer é a lib do Arduino (src/net.cpp)
    "net.h",
)

os.makedirs(DEST, exist_ok=True)

missing = [f for f in FILES if not os.path.isfile(os.path.join(PANEL_SRC, f))]
if missing:
    raise SystemExit(
        "sync_shared: nao achei %s em %s — este projeto vive dentro do "
        "repositorio do herdr-assist e reaproveita o firmware do painel"
        % (", ".join(missing), PANEL_SRC)
    )

for name in FILES:
    src = os.path.join(PANEL_SRC, name)
    dst = os.path.join(DEST, name)
    # copia só quando muda: mtime igual evita recompilar o mundo a cada build
    if not os.path.isfile(dst) or os.path.getmtime(src) > os.path.getmtime(dst):
        shutil.copy2(src, dst)
        print("sync_shared: %s atualizado" % name)

# módulo que saiu da lista não pode continuar compilando pela cópia velha
for name in os.listdir(DEST):
    if name not in FILES:
        os.remove(os.path.join(DEST, name))
        print("sync_shared: %s removido (fora da lista)" % name)


def _version():
    """Mesma resolução do painel: o version.txt que o CI grava na raiz (== tag)
    vence; num clone de desenvolvimento vale o git describe; sem git, "dev".
    É o que a tela Sobre e o splash exibem — antes era um literal no
    platformio.ini, que divergiria da tag de qualquer release."""
    try:
        with open(os.path.join(PANEL_SRC, "..", "version.txt"),
                  encoding="utf-8") as fh:
            v = fh.read().strip()
        if v:
            return v
    except OSError:
        pass
    try:
        return subprocess.run(
            ["git", "describe", "--tags", "--always", "--dirty"],
            cwd=PANEL_SRC, capture_output=True, text=True,
            check=True).stdout.strip() or "dev"
    except (OSError, subprocess.CalledProcessError):
        return "dev"


# este é o único extra_script do projeto, então o carimbo de versão mora aqui
env.Append(CPPDEFINES=[("HERDR_ASSIST_VERSION", '\\"%s\\"' % _version())])
