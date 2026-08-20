#!/usr/bin/env python3
"""Fatia um sprite sheet (fundo TRANSPARENTE) em sprites individuais + contato.

Segmenta por COMPONENTES CONECTADOS na máscara de alpha: cada pose é um blob de
pixels opacos separado por transparência. Blobs próximos são unidos (junta
aura/efeitos ao corpo). Gera cell_NN.png (RGBA) + montage.png numerado.

Uso: python scripts/sheet_extract.py <sheet.png> <pasta_saida> [merge_gap]
"""
import os
import sys

from PIL import Image, ImageDraw

ALPHA_TH = 40            # alpha >= isto conta como opaco (foreground)
MIN_W, MIN_H = 10, 16
MIN_AREA = 180
PAD = 1


def build_mask(im):
    w, h = im.size
    fg = bytearray(w * h)
    for i, p in enumerate(im.getdata()):
        fg[i] = 1 if p[3] >= ALPHA_TH else 0
    return fg, w, h


def components(fg, w, h):
    seen = bytearray(w * h)
    boxes = []
    for start in range(w * h):
        if not fg[start] or seen[start]:
            continue
        minx = maxx = start % w
        miny = maxy = start // w
        area = 0
        stack = [start]
        seen[start] = 1
        while stack:
            idx = stack.pop()
            area += 1
            x = idx % w
            y = idx // w
            if x < minx: minx = x
            if x > maxx: maxx = x
            if y < miny: miny = y
            if y > maxy: maxy = y
            for dy in (-1, 0, 1):
                ny = y + dy
                if ny < 0 or ny >= h:
                    continue
                base = ny * w
                for dx in (-1, 0, 1):
                    nx = x + dx
                    if 0 <= nx < w:
                        j = base + nx
                        if fg[j] and not seen[j]:
                            seen[j] = 1
                            stack.append(j)
        if area >= MIN_AREA and (maxx - minx) >= MIN_W and (maxy - miny) >= MIN_H:
            boxes.append([minx, miny, maxx + 1, maxy + 1])
    return boxes


def near(a, b, gap):
    return not (a[0] - gap > b[2] or b[0] - gap > a[2] or
                a[1] - gap > b[3] or b[1] - gap > a[3])


def merge(boxes, gap):
    changed = True
    while changed:
        changed = False
        out = []
        for b in boxes:
            hit = next((o for o in out if near(b, o, gap)), None)
            if hit:
                hit[0] = min(hit[0], b[0]); hit[1] = min(hit[1], b[1])
                hit[2] = max(hit[2], b[2]); hit[3] = max(hit[3], b[3])
                changed = True
            else:
                out.append(b[:])
        boxes = out
    return boxes


def montage(cells, path, cols=12, thumb=70):
    n = len(cells)
    rows = (n + cols - 1) // cols
    cw, ch = thumb + 8, thumb + 18
    img = Image.new("RGBA", (cols * cw, rows * ch), (28, 28, 36, 255))
    d = ImageDraw.Draw(img)
    for i, c in enumerate(cells):
        t = c.copy()
        t.thumbnail((thumb, thumb), Image.LANCZOS)
        gx, gy = (i % cols) * cw, (i // cols) * ch
        img.alpha_composite(t, (gx + (cw - t.width) // 2, gy + 14 + (thumb - t.height) // 2))
        d.text((gx + 3, gy + 2), str(i), fill=(255, 230, 80, 255))
    img.convert("RGB").save(path)


def main():
    if len(sys.argv) < 3:
        print("uso: python scripts/sheet_extract.py <sheet.png> <pasta_saida> [merge_gap]",
              file=sys.stderr)
        return 1
    sheet, outdir = sys.argv[1], sys.argv[2]
    gap = int(sys.argv[3]) if len(sys.argv) > 3 else 2
    os.makedirs(outdir, exist_ok=True)
    im = Image.open(sheet).convert("RGBA")
    fg, w, h = build_mask(im)
    boxes = components(fg, w, h)
    if gap > 0:
        boxes = merge(boxes, gap)   # só se pedido: pode encadear poses densas
    boxes.sort(key=lambda b: (round(b[1] / 40.0), b[0]))
    cells = []
    for i, bb in enumerate(boxes):
        crop = im.crop((max(bb[0] - PAD, 0), max(bb[1] - PAD, 0),
                        min(bb[2] + PAD, w), min(bb[3] + PAD, h)))
        crop.save(os.path.join(outdir, "cell_%02d.png" % i))
        cells.append(crop)
    montage(cells, os.path.join(outdir, "montage.png"))
    print("%d sprites (gap=%d) -> %s" % (len(cells), gap, outdir))
    return 0


if __name__ == "__main__":
    sys.exit(main())
