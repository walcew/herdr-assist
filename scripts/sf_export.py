#!/usr/bin/env python3
"""Exporta o avatar "Ryu vs Ken" (Street Fighter Alpha) para sprites RLE.

As fontes (scripts/sf_src/*.gif) já são CENAS com os dois lutadores compostos,
tiradas da Street Fighter Wiki (© Capcom — uso pessoal, não redistribuir). Cada
estado é uma FAIXA de frames de uma dessas cenas; recortamos por BBOX DE UNIÃO
(o mesmo retângulo para todos os frames do estado) para preservar a coreografia
— os lutadores se movem dentro do quadro, não são recentrados. Emite
src/assets/sprite_sf_<estado>.h no mesmo RLE dos outros avatares + um preview.

Uso: python scripts/sf_export.py [preview.png]
"""
import os
import sys

from PIL import Image, ImageDraw, ImageSequence

KEY = 0x18C5

HERE = os.path.dirname(__file__)
SRC = os.path.join(HERE, "sf_src")
ASSETS = os.path.join(HERE, "..", "src", "assets")

# estado -> (arquivo fonte, índices de frame). O driver mapeia:
#   luta=WORKING, encarando=IDLE, guarda=BLOCKED, vitoria=DONE, caidos=DISCONNECTED
STATES = [
    # a briga COMPLETA (38 frames contíguos, como o artista animou): agarrão ->
    # projeção/pulo -> chute -> volta pra guarda. Contígua = luta fluida.
    ("luta",      "both_act.gif",   list(range(38))),
    ("encarando", "both_act.gif",   [32, 33, 34, 35, 36, 37]),   # guarda quicando
    ("guarda",    "both_act.gif",   [6, 8, 10, 12, 14]),          # travados (clinch)
    ("vitoria",   "both_intro.gif", [0, 1, 2, 3, 4]),             # toque de punho
    ("caidos",    "ryu_lose.gif",   [0, 1, 2, 3]),                # Ryu abatido
]


def rgb565(r, g, b):
    v = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)
    return (v ^ 0x0001) if v == KEY else v


def load_frames(path):
    """Coalesce sequencial (Pillow monta o disposal ao iterar em ordem)."""
    im = Image.open(path)
    return [fr.convert("RGBA").copy() for fr in ImageSequence.Iterator(im)]


def union_bbox(frames):
    box = None
    for im in frames:
        bb = im.getchannel("A").getbbox()
        if bb is None:
            continue
        box = bb if box is None else (min(box[0], bb[0]), min(box[1], bb[1]),
                                      max(box[2], bb[2]), max(box[3], bb[3]))
    return box


def build():
    anims = {}
    for name, src, idxs in STATES:
        allf = load_frames(os.path.join(SRC, src))
        sel = [allf[i] for i in idxs if i < len(allf)]
        box = union_bbox(sel)
        if box is None:
            continue
        sel = [im.crop(box) for im in sel]
        W = box[2] - box[0]
        H = box[3] - box[1]
        frames = []
        for im in sel:
            grid = [KEY] * (W * H)
            px = im.load()
            for y in range(H):
                for x in range(W):
                    r, g, b, a = px[x, y]
                    if a >= 128:
                        grid[y * W + x] = rgb565(r, g, b)
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
        out.append(v)
        out.append(c)
        i += c
    return out


def emit(name, W, H, frames):
    up = ("sf_" + name).upper()
    lo = "sf_" + name
    words, offsets = [], [0]
    for gr in frames:
        words.extend(rle_frame(gr))
        offsets.append(len(words))
    path = os.path.join(ASSETS, "sprite_%s.h" % lo)
    with open(path, "w", encoding="utf-8") as f:
        f.write("#ifndef %s_FRAMES_H\n#define %s_FRAMES_H\n\n" % (up, up))
        f.write("/* Auto-gerado por scripts/sf_export.py. %d frames, %dx%d. */\n\n"
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
    anims = build()
    total = 0
    for name, (W, H, frames) in anims.items():
        kb = emit(name, W, H, frames)
        total += kb
        print("  %-9s %dx%d, %d frames, %.1f KB" % (name, W, H, len(frames), kb))
    print("total ~%.0f KB flash" % total)
    if len(sys.argv) > 1:
        preview(anims, sys.argv[1])
        print("preview:", sys.argv[1])


if __name__ == "__main__":
    main()
