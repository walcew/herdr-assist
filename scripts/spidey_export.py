#!/usr/bin/env python3
"""Exporta animações do Spider-Man (células fatiadas do sheet) para sprites RLE.

De um diretório de células (cell_NN.png transparentes, já sem o green screen),
monta cada animação a partir de uma lista de índices, com ESCALA GLOBAL (o
personagem tem o mesmo tamanho em todas as animações) e canvas por animação
(todos os frames do mesmo tamanho, alinhados na base ao centro). Emite
src/assets/sprite_spidey_<anim>.h multi-frame + um preview grid.

Uso: python scripts/spidey_export.py <cells_dir> [preview.png]
"""
import os
import sys

from PIL import Image, ImageDraw

KEY = 0x18C5
TARGET_H = 112       # altura do personagem (escala global)

# anim -> lista de índices de células (sequência real do sheet)
ANIMS = [
    ("idle",  [400, 401, 402, 403, 404, 405]),
    ("crawl", [0, 1, 2, 3, 4, 5, 6, 7, 8, 9]),        # working
    ("cheer", [406, 407, 408, 409]),                   # done
    ("web",   [329, 330, 331]),                        # blocked (maça de teia)
    ("down",  [396, 397, 398]),                        # disconnected
    # transição (one-shot): cambalhota aérea contígua (379-391) + salto com teia
    ("flip",  [383, 385, 387, 389, 391, 276]),
]

HERE = os.path.dirname(__file__)
ASSETS = os.path.join(HERE, "..", "src", "assets")


def rgb565(r, g, b):
    v = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)
    return (v ^ 0x0001) if v == KEY else v


def cell(cells_dir, i):
    p = os.path.join(cells_dir, "cell_%02d.png" % i)
    if not os.path.exists(p):
        return None
    im = Image.open(p).convert("RGBA")
    bb = im.getbbox()
    return im.crop(bb) if bb else im


def build(cells_dir):
    # escala global a partir da maior célula usada
    used = {}
    maxh = 1
    for name, idxs in ANIMS:
        used[name] = []
        for i in idxs:
            im = cell(cells_dir, i)
            if im is not None:
                used[name].append(im)
                maxh = max(maxh, im.height)
    scale = TARGET_H / maxh

    anims = {}
    for name, _ in ANIMS:
        ims = [im.resize((max(1, round(im.width * scale)),
                          max(1, round(im.height * scale))), Image.LANCZOS)
               for im in used[name]]
        if not ims:
            continue
        W = max(i.width for i in ims)
        H = max(i.height for i in ims)
        frames = []
        for im in ims:
            grid = [KEY] * (W * H)
            ox = (W - im.width) // 2
            oy = H - im.height                     # base
            px = im.load()
            for y in range(im.height):
                for x in range(im.width):
                    r, g, b, a = px[x, y]
                    if a >= 128:
                        grid[(oy + y) * W + (ox + x)] = rgb565(r, g, b)
            frames.append(grid)
        anims[name] = (W, H, frames)
    return anims


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
    up = ("spidey_" + name).upper()
    lo = "spidey_" + name
    words, offsets = [], [0]
    for gr in frames:
        words.extend(rle_frame(gr))
        offsets.append(len(words))
    path = os.path.join(ASSETS, "sprite_%s.h" % lo)
    with open(path, "w", encoding="utf-8") as f:
        f.write("#ifndef %s_FRAMES_H\n#define %s_FRAMES_H\n\n" % (up, up))
        f.write("/* Auto-gerado por scripts/spidey_export.py. %d frames, %dx%d. */\n\n"
                % (len(frames), W, H))
        f.write("#include <stdint.h>\n\n")
        f.write("#define %s_WIDTH  %d\n#define %s_HEIGHT %d\n" % (up, W, up, H))
        f.write("#define %s_FRAME_COUNT %d\n" % (up, len(frames)))
        f.write("#define %s_TRANSPARENT_KEY 0x%04X\n\n" % (up, KEY))
        f.write("static const uint32_t %s_frame_offsets[%d] = {\n    %s\n};\n\n"
                % (lo, len(offsets), ", ".join(str(o) for o in offsets)))
        f.write("static const uint16_t %s_rle_data[] = {\n" % lo)
        for i in range(0, len(words), 12):
            f.write("    " + ", ".join("0x%04X" % w for w in words[i:i + 12]) + ",\n")
        f.write("};\n\n#endif\n")
    return len(words) * 2 / 1024.0


def grid_img(grid, W, H):
    im = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    px = im.load()
    for i, v in enumerate(grid):
        if v != KEY:
            px[i % W, i // W] = (((v >> 11) & 0x1F) << 3, ((v >> 5) & 0x3F) << 2,
                                 (v & 0x1F) << 3, 255)
    return im


def preview(anims, path, scale=2):
    ncol = max(len(fr) for _, _, fr in anims.values())
    cellh = max(H for _, H, _ in anims.values()) * scale + 6
    cw = max(W for W, _, _ in anims.values()) * scale + 6
    lab = 90
    canvas = Image.new("RGBA", (lab + ncol * cw, len(anims) * cellh), (28, 28, 36, 255))
    d = ImageDraw.Draw(canvas)
    for r, (name, (W, H, fr)) in enumerate(anims.items()):
        gy = r * cellh
        d.text((4, gy + cellh // 2), name, fill=(255, 230, 80, 255))
        for k, gr in enumerate(fr):
            im = grid_img(gr, W, H).resize((W * scale, H * scale), Image.NEAREST)
            canvas.alpha_composite(im, (lab + k * cw, gy + 3 + (cellh - 6 - im.height)))
    canvas.convert("RGB").save(path)


def main():
    cells_dir = sys.argv[1]
    anims = build(cells_dir)
    total = 0
    for name, (W, H, frames) in anims.items():
        kb = emit(name, W, H, frames)
        total += kb
        print("  %-6s %dx%d, %d frames, %.1f KB" % (name, W, H, len(frames), kb))
    print("total ~%.0f KB flash" % total)
    if len(sys.argv) > 2:
        preview(anims, sys.argv[2])
        print("preview:", sys.argv[2])


if __name__ == "__main__":
    main()
