#!/usr/bin/env python3
"""Gera as 6 formas do Goku recolorindo UMA pose SSJ3 (cabelo) + escalando.

De uma pose base (sprite SSJ3 com fundo transparente), produz
scripts/goku_png/<forma>.png para cada forma: recolore o cabelo dourado
(preto na Criança/Base, dourado nas SSJ, azul no Blue) preservando o sombreado,
e escala o sprite (mais forte = maior — o motor alinha na base, então o Goku
"cresce" ao evoluir). Depois rode goku_png_export.py para gerar os headers.

Uso: python scripts/goku_recolor.py <pose.png>
"""
import os
import sys

from PIL import Image

HERE = os.path.dirname(__file__)
OUT = os.path.join(HERE, "goku_png")

# cabelo dourado do SSJ3: amarelo (G alto vs R do gi laranja) INCLUINDO os
# realces quase-brancos-amarelados. Distingue do gi laranja (G/R ~0.5) e da pele.
HAIR_MIN, HAIR_MAX = 120, 240


def is_hair(r, g, b):
    if g < 90 or g < 0.62 * r:          # gi laranja tem G/R baixo -> fora
        return False
    if b <= 0.66 * g:                   # amarelo normal do cabelo
        return True
    if r >= 205 and g >= 200 and b < r - 6 and b <= 210:  # realce claro amarelado
        return True
    return False


def recolor(im, ramp):
    """Recolore o cabelo posterizando em 3 tons chapados da rampa (pixel-art)."""
    im = im.convert("RGBA")
    px = im.load()
    for y in range(im.height):
        for x in range(im.width):
            r, g, b, a = px[x, y]
            if a < 40 or not is_hair(r, g, b):
                continue
            lum = (r + g + b) / 3.0
            t = (lum - HAIR_MIN) / (HAIR_MAX - HAIR_MIN)
            band = 0 if t < 0.40 else (1 if t < 0.75 else 2)
            c = ramp[band]
            px[x, y] = (c[0], c[1], c[2], a)
    return im


# 3 tons chapados por forma: sombra, meio, realce. Preto sólido e escuro (sem
# cinza-claro, que virava chuvisco); azul mais limpo/teal (menos neon).
BLACK = ((9, 9, 13), (19, 19, 25), (36, 36, 46))
BLUE = ((15, 34, 82), (38, 86, 156), (82, 150, 224))
KEEP = None   # mantém o dourado original

# forma -> (recolor_do_cabelo, altura_alvo_px)
FORMS = [
    ("crianca", BLACK, 96),
    ("base",    BLACK, 110),
    ("ssj",     KEEP,  122),
    ("ssj2",    KEEP,  134),
    ("ssj3",    KEEP,  146),
    ("blue",    BLUE,  154),
]


def main():
    if len(sys.argv) < 2:
        print("uso: python scripts/goku_recolor.py <pose.png>", file=sys.stderr)
        return 1
    base = Image.open(sys.argv[1]).convert("RGBA")
    bb = base.getbbox()
    if bb:
        base = base.crop(bb)
    os.makedirs(OUT, exist_ok=True)
    for name, ramp, target_h in FORMS:
        im = base.copy()
        if ramp is not None:
            im = recolor(im, ramp)
        s = target_h / im.height
        im = im.resize((max(1, round(im.width * s)), target_h), Image.LANCZOS)
        path = os.path.join(OUT, name + ".png")
        im.save(path)
        print("  %-8s %dx%d -> %s" % (name, im.width, im.height, os.path.basename(path)))
    print("ok — agora: python scripts/goku_png_export.py")
    return 0


if __name__ == "__main__":
    sys.exit(main())
