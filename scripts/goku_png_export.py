#!/usr/bin/env python3
"""Converte PNGs reais do Goku nos sprites do firmware (arte drop-in).

Para cada forma, procura scripts/goku_png/<forma>.png e gera
src/assets/sprite_goku_<forma>.h no MESMO formato RLE do placeholder
(consumível pelo rle_decode_tca16_swap). Formas sem PNG são deixadas como
estão (o placeholder anterior permanece), então dá para trocar 1 ou todas.

IMPORTANTE — o formato tem alpha de 1 bit: cada pixel é opaco OU transparente.
Pixels com alpha < LIMIAR viram a chave transparente (0x18C5); os demais ficam
opacos com sua cor. Bordas com meio-alpha ficam opacas (o zoom da LVGL suaviza).
Use PNG com fundo REALMENTE transparente (alpha), não um xadrez "de mentira".

Uso: python scripts/goku_png_export.py [pasta_png]
"""
import os
import sys

from PIL import Image

FORMS = ["crianca", "base", "ssj", "ssj2", "ssj3", "blue"]
KEY = 0x18C5              # fundo transparente (igual ao placeholder/Clawd)
ALPHA_TH = 128           # >= opaco, < transparente
MAX_SIDE = 160           # maior lado do sprite (~tamanho do slot, sem upscale borrado)

HERE = os.path.dirname(__file__)
SRC_DIR = sys.argv[1] if len(sys.argv) > 1 else os.path.join(HERE, "goku_png")
ASSETS = os.path.join(HERE, "..", "src", "assets")


def rgb565(r, g, b):
    v = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)
    return (v ^ 0x0001) if v == KEY else v   # nunca deixa a cor virar a chave


def load_grid(path):
    """PNG -> (W, H, grid[y][x] em RGB565 com KEY nos transparentes)."""
    im = Image.open(path).convert("RGBA")
    bbox = im.getbbox()          # corta margem totalmente transparente
    if bbox:
        im = im.crop(bbox)
    w, h = im.size
    scale = min(MAX_SIDE / max(w, h), 1.0)
    if scale < 1.0:
        im = im.resize((max(1, round(w * scale)), max(1, round(h * scale))),
                       Image.LANCZOS)
    w, h = im.size
    px = im.load()
    grid = [[KEY] * w for _ in range(h)]
    for y in range(h):
        for x in range(w):
            r, g, b, a = px[x, y]
            grid[y][x] = rgb565(r, g, b) if a >= ALPHA_TH else KEY
    return w, h, grid


def rle_encode(w, h, grid):
    flat = []
    for y in range(h):
        flat.extend(grid[y])
    out = []
    i, n = 0, len(flat)
    while i < n:
        v = flat[i]
        c = 1
        while i + c < n and flat[i + c] == v and c < 0xFFFF:
            c += 1
        out.append(v)
        out.append(c)
        i += c
    return out


def emit(name, w, h, words):
    up = ("goku_" + name).upper()
    lo = "goku_" + name
    path = os.path.join(ASSETS, "sprite_%s.h" % lo)
    guard = up + "_FRAMES_H"
    with open(path, "w", encoding="utf-8") as f:
        f.write("#ifndef %s\n#define %s\n\n" % (guard, guard))
        f.write("/* Auto-gerado por scripts/goku_png_export.py a partir de PNG real.\n")
        f.write(" * 1 frame, %dx%d. Chave transparente: 0x%04X. */\n\n" % (w, h, KEY))
        f.write("#include <stdint.h>\n\n")
        f.write("#define %s_WIDTH  %d\n" % (up, w))
        f.write("#define %s_HEIGHT %d\n" % (up, h))
        f.write("#define %s_FRAME_COUNT 1\n" % up)
        f.write("#define %s_TRANSPARENT_KEY 0x%04X\n\n" % (up, KEY))
        f.write("static const uint32_t %s_frame_offsets[2] = {\n    0, %d\n};\n\n"
                % (lo, len(words)))
        f.write("static const uint16_t %s_rle_data[] = {\n" % lo)
        for i in range(0, len(words), 12):
            f.write("    " + ", ".join("0x%04X" % x for x in words[i:i + 12]) + ",\n")
        f.write("};\n\n#endif\n")
    return path, len(words)


def main():
    if not os.path.isdir(SRC_DIR):
        print("pasta de PNGs nao existe: %s" % SRC_DIR, file=sys.stderr)
        print("crie-a e ponha <forma>.png (formas: %s)" % ", ".join(FORMS),
              file=sys.stderr)
        return 1
    done = 0
    for name in FORMS:
        png = None
        for ext in (".png", ".PNG"):
            cand = os.path.join(SRC_DIR, name + ext)
            if os.path.isfile(cand):
                png = cand
                break
        if not png:
            print("  (sem PNG para %-8s — mantem o sprite atual)" % name)
            continue
        w, h, grid = load_grid(png)
        words = rle_encode(w, h, grid)
        path, nwords = emit(name, w, h, words)
        kb = nwords * 2 / 1024.0
        print("  %-8s <- %-24s  %dx%d, %.1f KB flash%s"
              % (name, os.path.basename(png), w, h, kb,
                 "  (!) grande" if kb > 120 else ""))
        done += 1
    print("%d forma(s) convertida(s) de %s" % (done, SRC_DIR))
    return 0 if done else 2


if __name__ == "__main__":
    sys.exit(main())
