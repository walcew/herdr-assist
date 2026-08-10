#!/bin/sh
# Regenera src/lv_font_terminal_12.c a partir de uma Nerd Font monoespaçada.
#
# As fontes que acompanham a LVGL (Montserrat, unscii) cobrem apenas ASCII
# 0x20-0x7F, então a saída dos agentes — box-drawing, spinners braille — sai
# como retângulos vazios. Esta fonte cobre o que eles realmente usam.
#
# Requer node (npx baixa o lv_font_conv sozinho). Rode a partir da raiz do repo.
set -e

FONT="${FONT:-$HOME/Library/Fonts/JetBrainsMonoNerdFont-Regular.ttf}"
OUT="src/lv_font_terminal_12.c"

[ -f "$FONT" ] || { echo "fonte não encontrada: $FONT"; exit 1; }

npx --yes lv_font_conv@1.5.2 \
  --font "$FONT" \
  --size 12 --bpp 1 --format lvgl --no-compress \
  --range 0x20-0x7E \
  --range 0x00A0,0x00B7,0x00BB \
  --range 0x2022,0x2026,0x2190,0x2192,0x2713,0x2717 \
  --range 0x2500-0x259F \
  --range 0x2800-0x28FF \
  -o "$OUT"

echo "gerado: $OUT"
echo "Glifos fora desses ranges viram retângulo vazio — U+2733 (✳), usado pelo"
echo "Claude, é trocado por '*' em replace_missing_glyphs() (src/herdr_model.c)."
