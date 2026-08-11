#!/usr/bin/env python3
"""Gera os headers dos logos dos provedores a partir dos SVGs do Claude Design.

Renderiza 4x com rsvg-convert e reduz com LANCZOS (antialias melhor que
rasterizar direto no tamanho final), depois emite um lv_img_dsc_t em
LV_IMG_CF_TRUE_COLOR_ALPHA — 3 bytes por pixel [cor_hi, cor_lo, alpha], a
mesma ordem que rle_sprite.h usa (LV_COLOR_16_SWAP 1).
"""
import subprocess
import sys
from pathlib import Path

from PIL import Image

HERE = Path(__file__).resolve().parent / "logos"  # SVGs exportados do Claude Design
OUT = Path(__file__).resolve().parent.parent / "src" / "assets"
SCALE = 4
LOGOS = [("claude", 15), ("codex", 17)]


def rgb565(r, g, b):
    return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)


for name, size in LOGOS:
    png = HERE / f"{name}.png"
    subprocess.run(["rsvg-convert", "-w", str(size * SCALE), "-h", str(size * SCALE),
                    "-o", str(png), str(HERE / f"{name}.svg")], check=True)
    img = Image.open(png).convert("RGBA").resize((size, size), Image.LANCZOS)

    body = []
    for y in range(size):
        row = []
        for x in range(size):
            r, g, b, a = img.getpixel((x, y))
            v = rgb565(r, g, b)
            row.append("0x%02X,0x%02X,0x%02X," % (v >> 8, v & 0xFF, a))
        body.append("    " + "".join(row))

    guard = f"LOGO_{name.upper()}_H"
    hdr = f"""#ifndef {guard}
#define {guard}

/**
 * @file
 * @brief Logo do provedor {name}, para os cards da aba Dash.
 *
 * Gerado por scripts/gen_logos.py a partir do SVG da tela "Dashboards" no
 * projeto herdr-assist do Claude Design. Não editar à mão.
 *
 * {size}x{size} em LV_IMG_CF_TRUE_COLOR_ALPHA: 3 bytes por pixel
 * [cor_hi, cor_lo, alpha], com a cor byte-swapped (LV_COLOR_16_SWAP 1).
 *
 * A marca pertence ao seu titular; aqui ela só identifica a origem do dado.
 */

#include <lvgl.h>

static const uint8_t logo_{name}_map[] = {{
{chr(10).join(body)}
}};

static const lv_img_dsc_t logo_{name} = {{
    .header = {{
        .cf = LV_IMG_CF_TRUE_COLOR_ALPHA,
        .always_zero = 0,
        .reserved = 0,
        .w = {size},
        .h = {size},
    }},
    .data_size = sizeof(logo_{name}_map),
    .data = logo_{name}_map,
}};

#endif /* {guard} */
"""
    dest = OUT / f"logo_{name}.h"
    dest.write_text(hdr)
    print("%s  %dx%d  %d bytes de pixel" % (dest.name, size, size, size * size * 3))
