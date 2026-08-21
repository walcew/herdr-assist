#!/usr/bin/env python3
"""Remove um fundo xadrez "de mentira" (JPEG sem alpha) e salva PNG transparente.

O xadrez de transparência falsa é feito de dois cinzas CLAROS e quase-neutros.
Removemos por flood-fill a partir das bordas os pixels claros+neutros (o fundo),
sem furar o Goku (pele/cabelo/rabo têm cor). Depois erodimos a franja clara que
a compressão JPEG deixa na borda. Corta no bounding box do que sobrou.

Uso: python scripts/dekey_checker.py <entrada.jpg> <saida.png>
"""
import sys
from collections import deque

from PIL import Image

BRIGHT = 165     # claro
NEUTRAL = 46     # (max-min) pequeno = quase cinza/branco
HALO_PASSES = 2


def is_bg(px):
    r, g, b = px[0], px[1], px[2]
    mx = max(r, g, b)
    mn = min(r, g, b)
    return mx >= BRIGHT and (mx - mn) <= NEUTRAL


def main():
    if len(sys.argv) < 3:
        print("uso: python scripts/dekey_checker.py <entrada> <saida.png>",
              file=sys.stderr)
        return 1
    im = Image.open(sys.argv[1]).convert("RGBA")
    w, h = im.size
    px = im.load()
    # Remoção GLOBAL: o xadrez é claro+neutro e o Goku (pele/cabelo/rabo) tem
    # cor, então dá pra tirar todo pixel de fundo — inclusive bolsões fechados
    # (ex.: dentro da curva do rabo) que o flood-fill de borda não alcança.
    for y in range(h):
        for x in range(w):
            if px[x, y][3] and is_bg(px[x, y]):
                r, g, b, _ = px[x, y]
                px[x, y] = (r, g, b, 0)

    # erode: pixel claro+neutro OPACO encostando em transparente vira transparente
    for _ in range(HALO_PASSES):
        rem = []
        for y in range(h):
            for x in range(w):
                if px[x, y][3] == 0 or not is_bg(px[x, y]):
                    continue
                for dx, dy in ((1, 0), (-1, 0), (0, 1), (0, -1)):
                    nx, ny = x + dx, y + dy
                    if 0 <= nx < w and 0 <= ny < h and px[nx, ny][3] == 0:
                        rem.append((x, y))
                        break
        for x, y in rem:
            r, g, b, _ = px[x, y]
            px[x, y] = (r, g, b, 0)

    bb = im.getbbox()
    if bb:
        im = im.crop(bb)
    im.save(sys.argv[2])
    print("salvo %s (%dx%d)" % (sys.argv[2], im.width, im.height))
    return 0


if __name__ == "__main__":
    sys.exit(main())
