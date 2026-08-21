#!/usr/bin/env python3
"""Monta a LUTA (estado WORKING) do avatar Ryu vs Ken a partir do spritesheet
SF Alpha do Ryu (scripts/sf_src/ryu_sheet.png, © Capcom / The Spriters Resource,
uso pessoal). Ken = Ryu ESPELHADO + gi recolorido de vermelho (o Ken clássico é
palette-swap do Ryu). Compõe uma cena simétrica: Ryu (esq, virado à direita) e
Ken (dir, virado à esq) executam a mesma sequência de golpes reais de Street
Fighter — trocando socos, chutes, voadora, hadouken e shoryuken. Emite
src/assets/sprite_sf_luta.h + um preview.

Uso: python scripts/sf_fight.py [preview.png]
"""
import colorsys
import os
import sys
from collections import deque

from PIL import Image, ImageDraw, ImageSequence

KEY = 0x18C5
HERE = os.path.dirname(__file__)
SHEET = os.path.join(HERE, "sf_src", "ryu_sheet.png")
ASSETS = os.path.join(HERE, "..", "src", "assets")

DIST = 96          # distância entre os centros dos dois lutadores (px do sheet)

# Sequência de golpes (índices de célula na fatiação abaixo). Cada índice = 1
# frame da cena. Repetimos a guarda entre golpes pra dar ritmo de luta.
STANCE = [26, 27, 28, 29]              # guarda em pé, quicando
WALK   = [146, 147, 148, 149]          # caminhada pra frente (aproxima)
JAB    = [56, 55, 60, 55, 56]          # guarda -> jab -> soco forte -> jab -> guarda
KICK   = [96, 97, 98, 99, 100]         # chute alto (roundhouse)
HADO   = [152, 151, 152, 153, 153, 154]  # recolhe -> empurra as palmas (hadouken)

SEQ = (STANCE + WALK + JAB + STANCE[:2] + KICK + STANCE[:2] +
       HADO + STANCE[:2])

# Frames do both_act.gif (SFA3, dois lutadores já compostos) que TRAZEM o que a
# composição espelhada não faz: agarrão -> projeção -> VOADORA (chute aéreo).
# São concatenados na luta pra "manter os eventos anteriores".
BOTHACT = [6, 10, 14, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31,
           32, 34, 36]
BOTHACT_GIF = os.path.join(HERE, "sf_src", "both_act.gif")

FIREBALL = 210     # projétil do hadouken (bola redonda com rastro)

SCALE_OUT = 0.82   # reduz a canvas comum pra caber no flash (o driver reescala)


def rgb565(r, g, b):
    v = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)
    return (v ^ 0x0001) if v == KEY else v


def keyed(im):
    """Remove o fundo teal/azul do sheet do Ryu -> alpha 0."""
    im = im.convert("RGBA")
    px = im.load()
    for y in range(im.height):
        for x in range(im.width):
            r, g, b, a = px[x, y]
            bg = ((abs(r) < 50 and abs(g - 85) < 50 and abs(b - 127) < 55) or
                  (abs(r - 85) < 50 and abs(g - 170) < 50 and abs(b - 255) < 40))
            if bg or a < 8:
                px[x, y] = (0, 0, 0, 0)
    return im


def components(im, minw=8, minh=14):
    W, H = im.size
    al = im.getchannel("A").load()
    seen = bytearray(W * H)
    boxes = []
    for sy in range(H):
        for sx in range(W):
            if al[sx, sy] > 16 and not seen[sy * W + sx]:
                q = deque([(sx, sy)])
                seen[sy * W + sx] = 1
                x0 = x1 = sx
                y0 = y1 = sy
                while q:
                    x, y = q.popleft()
                    x0 = min(x0, x); x1 = max(x1, x)
                    y0 = min(y0, y); y1 = max(y1, y)
                    for dx, dy in ((1, 0), (-1, 0), (0, 1), (0, -1),
                                   (1, 1), (1, -1), (-1, 1), (-1, -1)):
                        nx, ny = x + dx, y + dy
                        if 0 <= nx < W and 0 <= ny < H and not seen[ny * W + nx] \
                                and al[nx, ny] > 16:
                            seen[ny * W + nx] = 1
                            q.append((nx, ny))
                if x1 - x0 + 1 >= minw and y1 - y0 + 1 >= minh:
                    boxes.append((x0, y0, x1 + 1, y1 + 1))
    boxes.sort(key=lambda b: (round(b[1] / 40), b[0]))
    return boxes


def recolor_ken(im):
    """Gi branco/cinza -> vermelho (mantém sombreado). Pele e cabelo ficam."""
    im = im.copy()
    px = im.load()
    for y in range(im.height):
        for x in range(im.width):
            r, g, b, a = px[x, y]
            if a < 8:
                continue
            h, s, v = colorsys.rgb_to_hsv(r / 255, g / 255, b / 255)
            if s < 0.28 and v > 0.50:          # gi branco/cinza
                nr, ng, nb = colorsys.hsv_to_rgb(0.0, 0.72, v)
                px[x, y] = (int(nr * 255), int(ng * 255), int(nb * 255), a)
    return im


def raster(im):
    """RGBA -> grid RGB565 (alpha<128 = KEY transparente)."""
    W, H = im.size
    grid = [KEY] * (W * H)
    px = im.load()
    for y in range(H):
        for x in range(W):
            r, g, b, a = px[x, y]
            if a >= 128:
                grid[y * W + x] = rgb565(r, g, b)
    return grid


def compose_strike(cr, ck, W, H, cxR):
    """Cena da composição: Ryu (esq, cxR) + Ken (dir, cxR+DIST), base no chão."""
    im = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    im.alpha_composite(ck, (int(cxR + DIST - ck.width / 2), H - ck.height))
    im.alpha_composite(cr, (int(cxR - cr.width / 2), H - cr.height))
    return im


def load_bothact(idxs):
    """Frames do both_act.gif recortados por bbox de união (preserva a coreografia)."""
    im = Image.open(BOTHACT_GIF)
    allf = [fr.convert("RGBA").copy() for fr in ImageSequence.Iterator(im)]
    sel = [allf[i] for i in idxs if i < len(allf)]
    box = None
    for f in sel:
        bb = f.getchannel("A").getbbox()
        if bb:
            box = bb if box is None else (min(box[0], bb[0]), min(box[1], bb[1]),
                                          max(box[2], bb[2]), max(box[3], bb[3]))
    return [f.crop(box) for f in sel]


def onto(f, W, H):
    im = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    im.alpha_composite(f, ((W - f.width) // 2, H - f.height))
    return im


def build():
    sheet = keyed(Image.open(SHEET))
    boxes = components(sheet)
    cells = [sheet.crop(b) for b in boxes]

    # Ryu (esq) encara a direita = espelhado; Ken (dir) encara a esq = original.
    def R(i):
        return cells[i].transpose(Image.FLIP_LEFT_RIGHT)

    def K(i):
        return recolor_ken(cells[i])

    used = set(STANCE + WALK + JAB + KICK + [151, 152, 153, 154, FIREBALL])
    mw = max(cells[i].width for i in used)
    mh = max(cells[i].height for i in used)
    cxR = mw / 2 + 2
    Wc = int(cxR + DIST + mw / 2 + 2)
    Hc = mh

    def beat(idxs):
        return [compose_strike(R(i), K(i), Wc, Hc, cxR) for i in idxs]

    def hadouken():
        """Recolhe -> empurra -> as DUAS bolas viajam pro centro e colidem."""
        fr = beat([151, 152, 153])
        hold_r, hold_k = R(154), K(154)
        fb_r = cells[FIREBALL].transpose(Image.FLIP_LEFT_RIGHT)  # Ryu: viaja p/ direita
        fb_k = cells[FIREBALL]                                   # Ken: viaja p/ esquerda
        fh = max(hold_r.height, hold_k.height)
        hy = Hc - fh + int(fh * 0.36)          # altura das mãos
        sR, sK = cxR + 20, cxR + DIST - 20
        mid = cxR + DIST / 2
        n = 6
        for t in range(1, n + 1):
            im = compose_strike(hold_r, hold_k, Wc, Hc, cxR)
            xr = sR + (mid - sR) * t / n
            xk = sK + (mid - sK) * t / n
            im.alpha_composite(fb_r, (int(xr - fb_r.width / 2), int(hy - fb_r.height / 2)))
            im.alpha_composite(fb_k, (int(xk - fb_k.width / 2), int(hy - fb_k.height / 2)))
            fr.append(im)
        return fr

    strikes = (beat(STANCE) + beat(WALK) + beat(JAB) + beat(STANCE[:2]) +
               beat(KICK) + beat(STANCE[:2]) + hadouken() + beat(STANCE[:2]))

    both = load_bothact(BOTHACT)
    W = max(Wc, max(f.width for f in both))
    H = max(Hc, max(f.height for f in both))
    # loop: eventos do both_act (agarrão/projeção/voadora) e depois os golpes.
    scenes = [onto(f, W, H) for f in both] + [onto(s, W, H) for s in strikes]

    if SCALE_OUT != 1.0:
        W, H = int(W * SCALE_OUT), int(H * SCALE_OUT)
        scenes = [s.resize((W, H), Image.LANCZOS) for s in scenes]

    return W, H, [raster(s) for s in scenes]


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


def emit(W, H, frames):
    up, lo = "SF_LUTA", "sf_luta"
    words, offsets = [], [0]
    for gr in frames:
        words.extend(rle_frame(gr))
        offsets.append(len(words))
    path = os.path.join(ASSETS, "sprite_sf_luta.h")
    with open(path, "w", encoding="utf-8") as f:
        f.write("#ifndef %s_FRAMES_H\n#define %s_FRAMES_H\n\n" % (up, up))
        f.write("/* Auto-gerado por scripts/sf_fight.py. %d frames, %dx%d. */\n\n"
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


def preview(W, H, frames, path, cols=10, scale=2):
    rows = (len(frames) + cols - 1) // cols
    cw, ch = W * scale + 4, H * scale + 16
    cv = Image.new("RGBA", (cols * cw + 4, rows * ch + 4), (30, 30, 40, 255))
    d = ImageDraw.Draw(cv)
    for k, g in enumerate(frames):
        r, c = divmod(k, cols)
        im = grid_img(g, W, H).resize((W * scale, H * scale), Image.NEAREST)
        cv.alpha_composite(im, (4 + c * cw, 4 + r * ch + 12))
        d.text((4 + c * cw, 2 + r * ch), str(k), fill=(255, 230, 90, 255))
    cv.convert("RGB").save(path)


def main():
    W, H, frames = build()
    kb = emit(W, H, frames)
    print("luta %dx%d, %d frames, %.1f KB" % (W, H, len(frames), kb))
    if len(sys.argv) > 1:
        preview(W, H, frames, sys.argv[1])
        print("preview:", sys.argv[1])


if __name__ == "__main__":
    main()
