#!/bin/sh
# Regenera as fontes da interface (src/lv_font_ui_*.c).
#
# As fontes Montserrat que acompanham a LVGL cobrem só ASCII, então "Sessões",
# "Configurações" e "Endereço" apareceriam com quadrados no lugar dos acentos.
# Aqui a Montserrat é convertida com o Latin-1 inteiro e mesclada com a mesma
# FontAwesome que a LVGL usa, para que os LV_SYMBOL_* continuem valendo.
#
# Requer node e python3. Rode a partir da raiz do repo.
set -e

CACHE="scripts/.fontcache"
MONT="$CACHE/Montserrat-Medium.ttf"
BOLD="$CACHE/Montserrat-Bold.ttf"
FA="$CACHE/FontAwesome5.woff"
DEJAVU="$CACHE/DejaVuSans.ttf"
LVGL_RAW="https://raw.githubusercontent.com/lvgl/lvgl/release/v8.3/scripts/built_in_font"
# a LVGL só distribui o peso Medium; o negrito vem do projeto Montserrat (OFL)
MONT_RAW="https://github.com/JulietaUla/Montserrat/raw/master/fonts/ttf"
DEJAVU_ZIP="https://github.com/dejavu-fonts/dejavu-fonts/releases/download/version_2_37/dejavu-fonts-ttf-2.37.zip"

mkdir -p "$CACHE"
[ -f "$MONT" ] || curl -sfL -o "$MONT" "$LVGL_RAW/Montserrat-Medium.ttf"
[ -f "$BOLD" ] || curl -sfL -o "$BOLD" "$MONT_RAW/Montserrat-Bold.ttf"
[ -f "$FA" ]   || curl -sfL -o "$FA" "$LVGL_RAW/FontAwesome5-Solid+Brands+Regular.woff"
[ -f "$DEJAVU" ] || {
    curl -sfL -o "$CACHE/dejavu.zip" "$DEJAVU_ZIP"
    unzip -jo "$CACHE/dejavu.zip" "dejavu-fonts-ttf-2.37/ttf/DejaVuSans.ttf" -d "$CACHE" >/dev/null
    rm -f "$CACHE/dejavu.zip"
}

python3 - "$MONT" "$BOLD" "$FA" "$DEJAVU" <<'PY'
import subprocess, sys

mont, bold, fa, dejavu = sys.argv[1], sys.argv[2], sys.argv[3], sys.argv[4]

# ASCII imprimível + Latin-1 Supplement (acentos do português e o · dos
# rótulos) + pontuação tipográfica que aparece em texto vindo dos agentes
# (aspas curvas, travessões, bullet e a elipse que o clip() da ponte anexa;
# 0x2010-0x2027 cobre tudo isso) + setas (Montserrat tem as quatro)
LATIN = ["0x20-0x7E", "0xA0-0xFF", "0x2010-0x2027", "0x2190-0x2193"]

# Símbolos de texto que a Montserrat não tem, tirados da DejaVu Sans (livre):
# ✓ ✗ ⚠ ● — usados em rótulos de status e em texto vindo do terminal.
DEJAVU_EXTRA = [0x2713, 0x2717, 0x25CF, 0x26A0]

# Símbolos usados pela UI e pelos widgets da LVGL (teclado, textarea).
# Os cinco sem LV_SYMBOL_ estão em ui_theme.h: os dois pares de ícones de
# agrupar e ordenar da tela de sessões e o velocímetro da aba Dash. O 0xF0F3
# (bell) é o beacon de decisão pendente da home; o 0xF019 (download) é o aviso
# de firmware novo.
# Ao acrescentar um codepoint aqui, confira no .c gerado que ele entrou: o
# lv_font_conv descarta em silêncio o que a face não tem (dos pedidos acima de
# 0x2010, 9 caem fora porque a Montserrat não os desenha).
SYMBOLS = [0xF00B, 0xF00C, 0xF00D, 0xF013, 0xF015, 0xF019, 0xF053, 0xF054,
           0xF067, 0xF06E, 0xF071, 0xF077, 0xF078, 0xF0C7, 0xF0F3, 0xF11C,
           0xF1EB, 0xF2ED, 0xF55A, 0xF8A2, 0xF5FD, 0xF0C9, 0xF0DC, 0xF160,
           0xF3FD]

# (arquivo, peso, tamanho, ranges do texto, inclui símbolos)
FONTS = [
    ("src/lv_font_ui_12.c", mont, 12, LATIN, True),
    ("src/lv_font_ui_14.c", mont, 14, LATIN, True),
    ("src/lv_font_ui_16.c", mont, 16, LATIN, True),
    # 20px é a escala das topbars: ícones dos botões e títulos
    ("src/lv_font_ui_20.c", mont, 20, LATIN, True),
    ("src/lv_font_ui_bold_16.c", bold, 16, LATIN, True),
    ("src/lv_font_ui_bold_20.c", bold, 20, LATIN, True),
    ("src/lv_font_ui_clock_44.c", mont, 44, ["0x30-0x3A"], False), # relógio da home
]

for out, face, size, ranges, with_symbols in FONTS:
    args = ["npx", "--yes", "lv_font_conv@1.5.2",
            "--size", str(size), "--bpp", "4", "--format", "lvgl", "--no-compress",
            "--font", face]
    for r in ranges:
        args += ["--range", r]
    if with_symbols:
        args += ["--font", fa]
        for cp in SYMBOLS:
            args += ["--range", f"0x{cp:X}"]
        args += ["--font", dejavu]
        for cp in DEJAVU_EXTRA:
            args += ["--range", f"0x{cp:X}"]
    args += ["-o", out]
    print(f"gerando {out} ({size}px)...", flush=True)
    p = subprocess.run(args)
    if p.returncode != 0:
        sys.exit(p.returncode)
PY

echo
echo "gerado: src/lv_font_ui_*.c"
