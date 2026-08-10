#!/usr/bin/env python3
"""Exporta sprites do Sonic (skdisasm, Sonic 3 & Knuckles) para o avatar do firmware.

Decodifica arte 4bpp + mappings + DPLCs + paleta do Mega Drive, compõe cada
animação num canvas comum alinhado pelo hotspot original do jogo e gera:

  - src/assets/sprite_sonic_<anim>.h  (RLE RGB565, via clawd-tank/tools/png2rgb565.py)
  - src/assets/sonic_sequences.h      (tabelas de sequência frame-a-frame)

PNGs intermediários ficam em scripts/sonic_frames/ (inspecionáveis, não usados
pelo build). Uso: python3 scripts/sonic_export.py

Sprites © SEGA — uso pessoal, não redistribuir.
"""
import re
import struct
import subprocess
import sys
from pathlib import Path

from PIL import Image

REPO = Path(__file__).resolve().parents[2]
SONIC = REPO / "skdisasm" / "General" / "Sprites" / "Sonic"
PNG2RGB = REPO / "clawd-tank" / "tools" / "png2rgb565.py"
ASSETS = REPO / "firmware" / "src" / "assets"
FRAMES_DIR = Path(__file__).resolve().parent / "sonic_frames"

# Animações montadas à mão: nome -> frames únicos (ids de mapping) + sequência.
# ko = caído de costas (D7/D8), push = empurrando (anim $22),
# duck = agachado (anim $07, dorme no frame 1).
ANIMS = {
    "run":  {"frames": [0x21, 0x22, 0x23, 0x24], "seq": [0, 1, 2, 3], "loop": 0},
    "push": {"frames": [0x90, 0x91, 0x92],       "seq": [0, 1, 2, 1], "loop": 0},
    "ko":   {"frames": [0xD7, 0xD8],             "seq": [0, 1],       "loop": 0},
    "duck": {"frames": [0xC3, 0xC4],             "seq": [0, 1],       "loop": 1},
}

# Animações copiadas na íntegra do jogo — frames, ordem e ponto de loop saem do
# próprio script de animação. $05 = espera (fica parado, depois bate o pé);
# $13 = levanta a mão e fica balançando o dedo.
GAME_ANIMS = {"idle": 0x05, "cheer": 0x13}

LABEL_RE = re.compile(r"^(\w+):")
DC_RE = re.compile(r"dc\.(b|w)\s+(.*)")


def eval_item(item):
    """Avalia um item de dc.b/dc.w: números, $hex e aritmética simples ($F2-2).
    Retorna None para referências simbólicas (linhas de tabela de offsets)."""
    expr = re.sub(r"\$([0-9A-Fa-f]+)", lambda m: str(int(m.group(1), 16)), item.strip())
    if re.fullmatch(r"[\d+\-\s()]+", expr) and re.search(r"\d", expr):
        return eval(expr)
    return None


def parse_asm(path):
    """Retorna (ordem da tabela de offsets, {label: bytes})."""
    order = []
    blocks = {}
    current = None
    for raw in path.read_text().splitlines():
        line = raw.split(";")[0]
        m = LABEL_RE.match(raw)
        if m:
            current = m.group(1)
            blocks[current] = bytearray()
            line = line[m.end():]
        for ref in re.finditer(r"dc\.w\s+(\w+)\s*-\s*\w+", line):
            order.append(ref.group(1))
        dm = DC_RE.search(line)
        if not dm or current is None:
            continue
        size = dm.group(1)
        for item in dm.group(2).split(","):
            val = eval_item(item)
            if val is None:
                continue
            blocks[current] += struct.pack(">B" if size == "b" else ">H",
                                           val & (0xFF if size == "b" else 0xFFFF))
    return order, blocks


def load_palette(path):
    data = path.read_bytes()
    pal = []
    for i in range(0, 32, 2):
        word = struct.unpack(">H", data[i:i + 2])[0]
        r = (word & 0x00E) * 255 // 14
        g = ((word >> 4) & 0xE) * 255 // 14
        b = ((word >> 8) & 0xE) * 255 // 14
        pal.append((r, g, b, 255))
    pal[0] = (0, 0, 0, 0)
    return pal


def decode_tile(art, index, pal):
    off = index * 32
    tile = []
    data = art[off:off + 32]
    for row in range(8):
        px = []
        for bi in range(4):
            byte = data[row * 4 + bi] if row * 4 + bi < len(data) else 0
            px.append(byte >> 4)
            px.append(byte & 0xF)
        tile.append([pal[p] if p else None for p in px])
    return tile


def build_vram(dplc_bytes, art, pal):
    n = struct.unpack(">H", dplc_bytes[:2])[0]
    tiles = []
    off = 2
    for _ in range(n):
        word = struct.unpack(">H", dplc_bytes[off:off + 2])[0]
        for t in range((word >> 12) + 1):
            tiles.append(decode_tile(art, (word & 0xFFF) + t, pal))
        off += 2
    return tiles


def render_frame(map_bytes, vram):
    """Retorna (imagem RGBA, (x0, y0) relativo ao hotspot do objeto)."""
    count = struct.unpack(">H", map_bytes[:2])[0]
    pieces = []
    off = 2
    for _ in range(count):
        y = struct.unpack(">b", map_bytes[off:off + 1])[0]
        size = map_bytes[off + 1]
        tile_word = struct.unpack(">H", map_bytes[off + 2:off + 4])[0]
        x = struct.unpack(">h", map_bytes[off + 4:off + 6])[0]
        pieces.append((x, y, ((size >> 2) & 3) + 1, (size & 3) + 1, tile_word))
        off += 6
    x0 = min(p[0] for p in pieces)
    y0 = min(p[1] for p in pieces)
    x1 = max(p[0] + p[2] * 8 for p in pieces)
    y1 = max(p[1] + p[3] * 8 for p in pieces)
    img = Image.new("RGBA", (x1 - x0, y1 - y0), (0, 0, 0, 0))
    pix = img.load()
    for x, y, w, h, tile_word in pieces:
        base = tile_word & 0x7FF
        hflip = bool(tile_word & 0x800)
        vflip = bool(tile_word & 0x1000)
        for col in range(w):
            for row in range(h):
                idx = base + col * h + row  # tiles do VDP em column-major
                if idx >= len(vram):
                    continue
                tile = vram[idx]
                dc = (w - 1 - col) if hflip else col
                dr = (h - 1 - row) if vflip else row
                for ty in range(8):
                    sy = 7 - ty if vflip else ty
                    for tx in range(8):
                        c = tile[sy][7 - tx if hflip else tx]
                        if c:
                            pix[x - x0 + dc * 8 + tx, y - y0 + dr * 8 + ty] = c
    return img, (x0, y0)


def anim_from_game(anim_blocks, anim_order, anim_id):
    """Extrai uma animação do jogo (frames únicos + sequência + ponto de loop)."""
    data = bytes(anim_blocks[anim_order[anim_id]])
    entries = []
    loop = 0
    i = 1
    while i < len(data):
        b = data[i]
        if b == 0xFE:                      # loopback: volta N entradas
            loop = len(entries) - data[i + 1]
            break
        if b >= 0xFC:
            break
        entries.append(b)
        i += 1
    uniq = []
    for f in entries:
        if f not in uniq:
            uniq.append(f)
    seq = [uniq.index(f) for f in entries]
    return {"frames": uniq, "seq": seq, "loop": loop}


def main():
    art = (SONIC / "Art" / "Sonic.bin").read_bytes()
    pal = load_palette(SONIC / "Palettes" / "SonicAndTails.bin")
    map_order, map_blocks = parse_asm(SONIC / "Map - Sonic.asm")
    dplc_order, dplc_blocks = parse_asm(SONIC / "DPLC - Sonic.asm")
    anim_order, anim_blocks = parse_asm(SONIC / "Anim - Sonic.asm")

    anims = dict(ANIMS)
    for name, anim_id in GAME_ANIMS.items():
        anims[name] = anim_from_game(anim_blocks, anim_order, anim_id)

    ASSETS.mkdir(exist_ok=True)
    seq_lines = [
        "#ifndef SONIC_SEQUENCES_H",
        "#define SONIC_SEQUENCES_H",
        "",
        "/* Auto-gerado por scripts/sonic_export.py — sequências frame-a-frame",
        "   das animações do Sonic (idle fiel à animação de espera do jogo). */",
        "",
        "#include <stdint.h>",
        "",
    ]

    for name, spec in sorted(anims.items()):
        rendered = {}
        for f in spec["frames"]:
            img, origin = render_frame(bytes(map_blocks[map_order[f]]),
                                       build_vram(bytes(dplc_blocks[dplc_order[f]]), art, pal))
            rendered[f] = (img, origin)

        # canvas comum da animação, alinhado pelo hotspot do jogo
        minx = min(o[0] for _, o in rendered.values())
        miny = min(o[1] for _, o in rendered.values())
        maxx = max(o[0] + im.width for im, o in rendered.values())
        maxy = max(o[1] + im.height for im, o in rendered.values())

        out_dir = FRAMES_DIR / name
        out_dir.mkdir(parents=True, exist_ok=True)
        for old in out_dir.glob("*.png"):
            old.unlink()
        for i, f in enumerate(spec["frames"]):
            im, (x0, y0) = rendered[f]
            canvas = Image.new("RGBA", (maxx - minx, maxy - miny), (0, 0, 0, 0))
            canvas.paste(im, (x0 - minx, y0 - miny), im)
            canvas.save(out_dir / f"{i:02d}.png")

        header = ASSETS / f"sprite_sonic_{name}.h"
        subprocess.run([sys.executable, str(PNG2RGB), str(out_dir), str(header),
                        "--name", f"sonic_{name}"], check=True)

        up = name.upper()
        seq = spec["seq"]
        seq_lines.append(f"#define SONIC_{up}_SEQ_LEN  {len(seq)}")
        seq_lines.append(f"#define SONIC_{up}_SEQ_LOOP {spec['loop']}")
        seq_lines.append(f"static const uint8_t sonic_{name}_seq[{len(seq)}] = {{")
        for i in range(0, len(seq), 16):
            seq_lines.append("    " + ", ".join(str(v) for v in seq[i:i + 16]) + ",")
        seq_lines.append("};")
        seq_lines.append("")
        print(f"{name}: {len(spec['frames'])} frames {maxx - minx}x{maxy - miny}, "
              f"seq {len(seq)} passos (loop em {spec['loop']}), chão em y={maxy}")

    seq_lines.append("#endif // SONIC_SEQUENCES_H")
    (ASSETS / "sonic_sequences.h").write_text("\n".join(seq_lines) + "\n")
    print(f"\nHeaders em {ASSETS}")


if __name__ == "__main__":
    main()
