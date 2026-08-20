#!/usr/bin/env python3
"""Gera sprites ANIMADOS do Goku: aura de ki tremeluzente + leve respirar.

Para cada forma, compõe N frames a partir de scripts/goku_png/<forma>.png:
uma aura (chamas de energia) atrás do Goku que dança/pulsa por frame, na cor e
alcance da forma, mais um bob vertical sutil. Emite src/assets/sprite_goku_<forma>.h
multi-frame no formato RLE do projeto — o avatar_goku.c já cicla os frames.

Uso: python scripts/goku_animate.py
"""
import math
import os
from collections import deque

from PIL import Image

KEY = 0x18C5
N = 6                     # frames por forma
BOT = 4                   # folga na base (Goku fica apoiado embaixo)

GOLD = ((255, 240, 120), (238, 170, 36))
WHITE = ((236, 236, 255), (150, 150, 205))
BLUE = ((150, 220, 255), (60, 120, 240))

# forma -> (cor_da_aura (bright, mid), alcance_px)
FORMS = [
    ("crianca", WHITE, 4),
    ("base",    WHITE, 5),
    ("ssj",     GOLD,  8),
    ("ssj2",    GOLD,  10),
    ("ssj3",    GOLD,  12),
    ("blue",    BLUE,  12),
]

HERE = os.path.dirname(__file__)
PNG = os.path.join(HERE, "goku_png")
ASSETS = os.path.join(HERE, "..", "src", "assets")


def rgb565(r, g, b):
    v = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)
    return (v ^ 0x0001) if v == KEY else v


def load_goku(name):
    im = Image.open(os.path.join(PNG, name + ".png")).convert("RGBA")
    bb = im.getbbox()
    return im.crop(bb) if bb else im


def dist_map(alpha, w, h, maxd):
    """Distância (4-conn) de cada pixel vazio até a silhueta, capada em maxd."""
    INF = 9999
    dist = [INF] * (w * h)
    dq = deque()
    for i in range(w * h):
        if alpha[i]:
            dist[i] = 0
            dq.append(i)
    while dq:
        i = dq.popleft()
        if dist[i] >= maxd:
            continue
        x = i % w
        y = i // w
        for nx, ny in ((x + 1, y), (x - 1, y), (x, y + 1), (x, y - 1)):
            if 0 <= nx < w and 0 <= ny < h:
                j = ny * w + nx
                if dist[i] + 1 < dist[j]:
                    dist[j] = dist[i] + 1
                    dq.append(j)
    return dist


def aura_lit(d, x, y, f, reach, cy):
    """0 = apagado; 1 = interno (bright); 2 = externo (mid). Chamas sobem e dançam."""
    if d <= 0 or d > reach * 1.5:
        return 0
    up = 1.30 if y < cy else 0.85            # chamas sobem
    wob = 0.60 + 0.40 * abs(math.sin(x * 0.5 + f * (2 * math.pi / N) * 1.3))
    limit = reach * up * wob
    if d <= limit:
        return 1 if d <= 2 else 2
    return 0


def build_form(name, aura, reach):
    goku = load_goku(name)
    gw, gh = goku.size
    marg = reach + 3
    W = gw + 2 * marg
    H = gh + (reach + 5) + BOT
    gx0 = marg
    gy0 = H - BOT - gh                       # Goku apoiado na base

    # silhueta do Goku no canvas
    gpx = goku.load()
    alpha = bytearray(W * H)
    goku565 = {}
    for y in range(gh):
        for x in range(gw):
            r, g, b, a = gpx[x, y]
            if a >= 128:
                cx, cy = gx0 + x, gy0 + y
                alpha[cy * W + cx] = 1
                goku565[(cx, cy)] = rgb565(r, g, b)

    dist = dist_map(alpha, W, H, reach * 2)
    cy_sil = gy0 + gh // 2

    (br, bg, bb), (mr, mg, mb) = aura
    bright = rgb565(br, bg, bb)
    mid = rgb565(mr, mg, mb)

    frames = []
    bob_seq = [0, -1, -1, 0, 1, 1]           # respirar sutil
    for f in range(N):
        bob = bob_seq[f % len(bob_seq)]
        grid = [KEY] * (W * H)
        # aura (atrás), só onde não há Goku
        for i in range(W * H):
            if alpha[i]:
                continue
            d = dist[i]
            if d >= 9999:
                continue
            lit = aura_lit(d, i % W, i // W, f, reach, cy_sil)
            if lit:
                grid[i] = bright if lit == 1 else mid
        # Goku por cima, deslocado pelo bob
        for (cx, cy), v in goku565.items():
            ny = cy + bob
            if 0 <= ny < H:
                grid[ny * W + cx] = v
        frames.append(grid)
    return W, H, frames


def rle_frame(grid):
    out = []
    i, n = 0, len(grid)
    while i < n:
        v = grid[i]
        c = 1
        while i + c < n and grid[i + c] == v and c < 0xFFFF:
            c += 1
        out.append(v); out.append(c)
        i += c
    return out


def emit(name, W, H, frames):
    up = ("goku_" + name).upper()
    lo = "goku_" + name
    words = []
    offsets = [0]
    for gr in frames:
        words.extend(rle_frame(gr))
        offsets.append(len(words))
    path = os.path.join(ASSETS, "sprite_%s.h" % lo)
    with open(path, "w", encoding="utf-8") as fh:
        fh.write("#ifndef %s_FRAMES_H\n#define %s_FRAMES_H\n\n" % (up, up))
        fh.write("/* Auto-gerado por scripts/goku_animate.py (aura de ki animada).\n")
        fh.write(" * %d frames, %dx%d. Chave transparente: 0x%04X. */\n\n"
                 % (len(frames), W, H, KEY))
        fh.write("#include <stdint.h>\n\n")
        fh.write("#define %s_WIDTH  %d\n#define %s_HEIGHT %d\n" % (up, W, up, H))
        fh.write("#define %s_FRAME_COUNT %d\n" % (up, len(frames)))
        fh.write("#define %s_TRANSPARENT_KEY 0x%04X\n\n" % (up, KEY))
        fh.write("static const uint32_t %s_frame_offsets[%d] = {\n    %s\n};\n\n"
                 % (lo, len(offsets), ", ".join(str(o) for o in offsets)))
        fh.write("static const uint16_t %s_rle_data[] = {\n" % lo)
        for i in range(0, len(words), 12):
            fh.write("    " + ", ".join("0x%04X" % w for w in words[i:i + 12]) + ",\n")
        fh.write("};\n\n#endif\n")
    return len(words) * 2 / 1024.0


def main():
    for name, aura, reach in FORMS:
        W, H, frames = build_form(name, aura, reach)
        kb = emit(name, W, H, frames)
        print("  %-8s %dx%d, %d frames, %.1f KB flash" % (name, W, H, N, kb))
    print("ok — headers multi-frame gerados")


if __name__ == "__main__":
    main()
