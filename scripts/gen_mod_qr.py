#!/usr/bin/env python3
"""Gera os QR do manual de pareamento que a aba Parear do Mod de controle mostra.

O gpui-kit só desenha imagem de arquivo, sem gerador de QR, então os SVG ficam
versionados em mod/herdr-assist/qr-<idioma>.svg. A matriz vem do qr_matrix do
gen_docs_mockups.py (o mesmo qrcodegen do firmware) e as URLs são as do
PAIR_MANUAL_URL de src/i18n.h. Rode de novo se o manual mudar de lugar.

Uso, a partir da raiz do repo:  python3 scripts/gen_mod_qr.py
Requer Pillow (import do gen_docs_mockups) e cc.
"""
import os

from gen_docs_mockups import ROOT, qr_matrix

BASE = "https://github.com/walcew/herdr-assist/blob/main/docs/"
URLS = {"en": BASE + "pairing.md", "pt": BASE + "pairing.pt-BR.md"}
QUIET = 4   # margem branca exigida pelos leitores

for lang, url in URLS.items():
    size, rows = qr_matrix(url, 160)
    n = size + 2 * QUIET
    dots = "".join("M%d %dh1v1h-1z" % (x + QUIET, y + QUIET)
                   for y, row in enumerate(rows) for x, c in enumerate(row) if c == "1")
    svg = ('<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 %d %d" '
           'shape-rendering="crispEdges"><rect width="%d" height="%d" fill="#fff"/>'
           '<path d="%s" fill="#000"/></svg>\n' % (n, n, n, n, dots))
    with open(os.path.join(ROOT, "mod", "herdr-assist", "qr-%s.svg" % lang), "w") as fh:
        fh.write(svg)
    print(lang, url, size)
