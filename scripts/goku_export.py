#!/usr/bin/env python3
"""Gera os sprites PLACEHOLDER do avatar Goku (6 formas) no formato RLE do projeto.

Cada forma vira src/assets/sprite_goku_<forma>.h com o mesmo layout dos sprites
do Clawd (pares uint16 (RGB565, contagem), frame_offsets em words), consumível
pelo rle_decode_tca16_swap. É PURO stdlib (sem PIL): desenha uma figura
estilizada por primitivas e anima só a aura.

Trocar pela arte real depois é drop-in: basta reemitir os mesmos headers com o
mesmo nome/dimensões a partir das PNGs verdadeiras.

Uso: python scripts/goku_export.py
"""
import os

W, H = 64, 80
KEY = 0x18C5                     # fundo transparente (igual ao Clawd)
FRAMES = 4                       # frames de pulso da aura
ASSETS = os.path.join(os.path.dirname(__file__), "..", "src", "assets")


def rgb565(r, g, b):
    return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)


# paleta
SKIN = rgb565(0xF0, 0xC0, 0x90)
BLACK = rgb565(0x20, 0x20, 0x28)
GOLD = rgb565(0xF8, 0xE0, 0x40)
BLUEH = rgb565(0x50, 0xB8, 0xF8)
GI_O = rgb565(0xF0, 0x80, 0x20)
GI_B = rgb565(0x20, 0x58, 0xC8)
A_WHITE = rgb565(0xE8, 0xE8, 0xF0)
A_GOLD = rgb565(0xF8, 0xE0, 0x40)
A_BLUE = rgb565(0x70, 0xC8, 0xFF)
TAIL = rgb565(0x90, 0x60, 0x30)

# forma -> (hair, gi, aura, tail?, long_hair?, sparks?, child?)
FORMS = {
    "crianca": dict(hair=BLACK, gi=None,  aura=A_WHITE, tail=True,  longh=False, spark=False, child=True),
    "base":    dict(hair=BLACK, gi=GI_O,  aura=A_WHITE, tail=False, longh=False, spark=False, child=False),
    "ssj":     dict(hair=GOLD,  gi=GI_O,  aura=A_GOLD,  tail=False, longh=False, spark=False, child=False),
    "ssj2":    dict(hair=GOLD,  gi=GI_O,  aura=A_GOLD,  tail=False, longh=False, spark=True,  child=False),
    "ssj3":    dict(hair=GOLD,  gi=GI_O,  aura=A_GOLD,  tail=False, longh=True,  spark=True,  child=False),
    "blue":    dict(hair=BLUEH, gi=GI_B,  aura=A_BLUE,  tail=False, longh=False, spark=True,  child=False),
}


def new_grid():
    return [[KEY] * W for _ in range(H)]


def put(g, x, y, c):
    if 0 <= x < W and 0 <= y < H:
        g[y][x] = c


def fill_rect(g, x0, y0, x1, y1, c):
    for y in range(int(y0), int(y1) + 1):
        for x in range(int(x0), int(x1) + 1):
            put(g, x, y, c)


def fill_circle(g, cx, cy, r, c):
    for y in range(cy - r, cy + r + 1):
        for x in range(cx - r, cx + r + 1):
            if (x - cx) ** 2 + (y - cy) ** 2 <= r * r:
                put(g, x, y, c)


def spike(g, ax, ay, half, height, c, up=True):
    """Triângulo (cabelo espetado). up=True aponta para cima."""
    for i in range(height + 1):
        frac = i / max(height, 1)
        w = int(half * (1 - frac))
        y = ay - i if up else ay + i
        for x in range(ax - w, ax + w + 1):
            put(g, x, y, c)


def ellipse_ring(g, cx, cy, rx, ry, c, thick=2):
    for y in range(cy - ry, cy + ry + 1):
        for x in range(cx - rx, cx + rx + 1):
            v = ((x - cx) / rx) ** 2 + ((y - cy) / ry) ** 2 if rx and ry else 2
            if 1.0 - 0.10 * thick <= v <= 1.0:
                put(g, x, y, c)


def draw_form(spec, frame):
    g = new_grid()
    cx = W // 2
    scale = 0.72 if spec["child"] else 1.0
    head_r = int(9 * scale)
    head_y = int(26 * scale) + (6 if spec["child"] else 0)

    # aura (animada): anel pulsando atrás da figura
    puls = [0, 2, 4, 2][frame % 4]
    rx = int(20 * scale) + puls
    ry = int(30 * scale) + puls
    thick = 2 if not spec["child"] else 1
    ellipse_ring(g, cx, head_y + int(14 * scale), rx, ry, spec["aura"], thick)
    if spec["spark"] and frame % 2 == 0:
        for sx, sy in ((cx - rx, head_y), (cx + rx, head_y + 8), (cx, head_y - ry + 2)):
            fill_circle(g, sx, sy, 1, spec["aura"])

    # cabelo longo (SSJ3): desce pelas costas antes do corpo
    if spec["longh"]:
        fill_rect(g, cx - 8, head_y, cx + 8, head_y + 34, spec["hair"])

    # corpo / gi
    if spec["gi"] is not None:
        fill_rect(g, cx - 10, head_y + head_r, cx + 10, head_y + int(30 * scale), spec["gi"])
        # pernas
        fill_rect(g, cx - 9, head_y + int(30 * scale), cx - 2, head_y + int(40 * scale), spec["gi"])
        fill_rect(g, cx + 2, head_y + int(30 * scale), cx + 9, head_y + int(40 * scale), spec["gi"])
    else:
        # criança: corpinho de pele encolhido
        fill_circle(g, cx, head_y + int(16 * scale), int(10 * scale), SKIN)

    # cabeça
    fill_circle(g, cx, head_y, head_r, SKIN)

    # cabelo espetado (coroa de spikes)
    n = 5
    for i in range(n):
        ax = cx - head_r + int((2 * head_r) * i / (n - 1))
        up = True
        h = int((14 if not spec["child"] else 10) * scale)
        spike(g, ax, head_y - head_r + 2, 4, h, spec["hair"], up)
    # franja lateral
    spike(g, cx - head_r, head_y - 1, 3, int(10 * scale), spec["hair"], up=True)
    spike(g, cx + head_r, head_y - 1, 3, int(10 * scale), spec["hair"], up=True)

    # rabo (criança)
    if spec["tail"]:
        ty = head_y + int(20 * scale)
        for i in range(18):
            put(g, cx + 8 + i // 2, ty + i, TAIL)
            put(g, cx + 9 + i // 2, ty + i, TAIL)

    return g


def rle_encode(g):
    """Achata row-major e devolve lista de words (val, count, val, count, ...)."""
    flat = []
    for y in range(H):
        flat.extend(g[y])
    out = []
    i = 0
    n = len(flat)
    while i < n:
        v = flat[i]
        c = 1
        while i + c < n and flat[i + c] == v and c < 0xFFFF:
            c += 1
        out.append(v)
        out.append(c)
        i += c
    return out


def emit(name, frames_words, offsets):
    up = ("goku_" + name).upper()
    lo = "goku_" + name
    path = os.path.join(ASSETS, "sprite_%s.h" % lo)
    guard = up + "_FRAMES_H"
    with open(path, "w", encoding="utf-8") as f:
        f.write("#ifndef %s\n#define %s\n\n" % (guard, guard))
        f.write("/* Auto-gerado por scripts/goku_export.py (placeholder RLE).\n")
        f.write(" * %d frame(s), %dx%d. Chave transparente: 0x%04X.\n" %
                (len(offsets) - 1, W, H, KEY))
        f.write(" * Trocar pela arte real = reemitir com mesmo nome/dimensoes. */\n\n")
        f.write("#include <stdint.h>\n\n")
        f.write("#define %s_WIDTH  %d\n" % (up, W))
        f.write("#define %s_HEIGHT %d\n" % (up, H))
        f.write("#define %s_FRAME_COUNT %d\n" % (up, len(offsets) - 1))
        f.write("#define %s_TRANSPARENT_KEY 0x%04X\n\n" % (up, KEY))
        f.write("static const uint32_t %s_frame_offsets[%d] = {\n    " % (lo, len(offsets)))
        f.write(", ".join(str(o) for o in offsets))
        f.write("\n};\n\n")
        f.write("static const uint16_t %s_rle_data[] = {\n" % lo)
        for i in range(0, len(frames_words), 12):
            f.write("    " + ", ".join("0x%04X" % w for w in frames_words[i:i + 12]) + ",\n")
        f.write("};\n\n#endif\n")
    return path


def main():
    os.makedirs(ASSETS, exist_ok=True)
    for name, spec in FORMS.items():
        words = []
        offsets = [0]
        for fr in range(FRAMES):
            g = draw_form(spec, fr)
            enc = rle_encode(g)
            words.extend(enc)
            offsets.append(len(words))
        path = emit(name, words, offsets)
        print("gerado %-28s %d frames, %d words" %
              (os.path.basename(path), FRAMES, len(words)))


if __name__ == "__main__":
    main()
