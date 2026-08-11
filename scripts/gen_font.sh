#!/bin/sh
# Regenera src/lv_font_terminal_12.c com a JetBrainsMono Nerd Font INTEIRA.
#
# Diferente do terminal, que rasteriza o TTF em tempo real e usa fallback quando
# falta glifo, a LVGL trabalha com bitmaps gerados na compilação: o que não for
# convertido aqui aparece como retângulo vazio no painel. Por isso convertemos a
# fonte toda (~12k glifos, ~230KB de flash) em vez de escolher ranges à mão —
# escolher ranges já custou acentos faltando no meio de texto em português.
#
# Requer node e python3 (fontTools). Rode a partir da raiz do repo.
set -e

FONT="${FONT:-$HOME/Library/Fonts/JetBrainsMonoNerdFont-Regular.ttf}"
OUT="src/lv_font_terminal_12.c"

[ -f "$FONT" ] || { echo "fonte não encontrada: $FONT"; exit 1; }

# Lê os codepoints do cmap da própria fonte e converte todos, menos U+FEFF —
# glifo sem contorno que faz o FreeType do lv_font_conv abortar com
# "FT_Load_Glyph: -1". O nome do símbolo C vem do nome do arquivo de saída.
python3 - "$FONT" "$OUT" <<'PY'
import subprocess, sys
try:
    from fontTools.ttLib import TTFont
except ImportError:
    sys.exit("instale fontTools: pip install fonttools (ou use uv run)")

font_path, out = sys.argv[1], sys.argv[2]
cps = set()
for t in TTFont(font_path)["cmap"].tables:
    cps |= set(t.cmap.keys())
cps = sorted(c for c in cps if c >= 0x20 and c != 0xFEFF)

ranges, start, prev = [], cps[0], cps[0]
for c in cps[1:]:
    if c != prev + 1:
        ranges.append((start, prev)); start = c
    prev = c
ranges.append((start, prev))

args = ["npx", "--yes", "lv_font_conv@1.5.2", "--font", font_path,
        "--size", "12", "--bpp", "1", "--format", "lvgl", "--no-compress"]
for a, b in ranges:
    args += ["--range", f"0x{a:X}-0x{b:X}" if a != b else f"0x{a:X}"]
args += ["-o", out]

print(f"convertendo {len(cps)} glifos em {len(ranges)} ranges...")
p = subprocess.run(args)
sys.exit(p.returncode)
PY

echo "gerado: $OUT"
echo
echo "Sobram os emojis (nenhuma fonte monoespaçada os tem) e os símbolos que a"
echo "Nerd Font não cobre (⎿ ⏺ ⏵ ⏸ ⏳ ◑ ◼ ✢ ✳ ✻ ✽ ✔ ✅ ⧉ ※) — todos trocados"
echo "por vizinhos visuais na tabela GLYPH_SWAPS de replace_missing_glyphs()"
echo "(src/herdr_model.c). Ao mexer aqui, re-audite a tabela contra o cmap do .c."
