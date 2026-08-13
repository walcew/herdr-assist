#!/usr/bin/env python3
"""Converte a logo de boot num bitmap em tons de cinza no tamanho da tela.

    python3 scripts/gen_logo.py assets/logo.png src/assets/logo_boot.h

A arte nasce em 93x64 de dois tons — pequena demais para os 240x135 do
Cardputer. Guardá-la crua e deixar o firmware ampliar por inteiro dava escala 1
(a altura útil, 122, cabe só 1,9 vezes os 64 da fonte), então a logo aparecia
com 39% da largura da tela: um selo perdido no meio do fundo.

Aqui a ampliação sai da compilação, não do firmware. O traço é reamostrado com
supersampling e corte em meio-tom, o que troca a escada de pixels por curvas, e
o resultado é gravado já no tamanho final em tons de cinza — o firmware só
copia linha a linha, sem escalar nada.

Custa 8 bits por pixel (~20KB) contra 1 bit do formato antigo (768 bytes). É o
preço do anti-alias: sem meio-tom não há curva, e a mesma arte em RGB565 custaria
o dobro sem acrescentar nada, porque a arte é cinza puro.

Regenerar só é preciso quando a arte muda; o .h vai versionado.
"""
import argparse
import os
import sys

import cv2
import numpy as np
from PIL import Image

LIMIAR = 128        # abaixo disto, na fonte, é traço
SUPER = 8           # fator de supersampling antes do corte
SUAVIZA = 0.6       # sigma do borrão, em pixels do alvo; acima de ~0.8 o rosto vira mancha
ALTURA_PADRAO = 122  # 135 da tela menos os 13 reservados para a linha da versão


def rasteriza(ink: np.ndarray, larg: int, alt: int) -> np.ndarray:
    """Amplia a máscara de traço e devolve tons de cinza (0 = tinta, 255 = papel).

    Ampliar e borrar sozinho deixaria a arte cinzenta e mole; borrar e cortar em
    meio-tom recoloca a borda num lugar sub-pixel, e só então a média de área
    transforma essa borda em tons. É isso que vira curva onde havia degrau.
    """
    grande = cv2.resize(
        ink * 255.0, (larg * SUPER, alt * SUPER), interpolation=cv2.INTER_NEAREST
    )
    grande = cv2.GaussianBlur(grande, (0, 0), SUPER * SUAVIZA)
    grande = (grande > 127.5).astype(np.float32) * 255.0
    tinta = cv2.resize(grande, (larg, alt), interpolation=cv2.INTER_AREA)
    return np.clip(255.0 - tinta, 0, 255).astype(np.uint8)


def main() -> int:
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("entrada")
    p.add_argument("saida")
    p.add_argument("--altura", type=int, default=ALTURA_PADRAO,
                   help="altura final em pixels (padrão %d)" % ALTURA_PADRAO)
    args = p.parse_args()

    im = np.array(Image.open(args.entrada).convert("L"))
    ink = (im < LIMIAR).astype(np.uint8)
    ys, xs = np.nonzero(ink)
    if not len(ys):
        print("erro: a arte não tem traço nenhum", file=sys.stderr)
        return 1

    # Recortar até o traço: a moldura branca da fonte só roubaria tamanho na tela.
    ink = ink[ys.min():ys.max() + 1, xs.min():xs.max() + 1]
    h0, w0 = ink.shape
    alt = args.altura
    larg = int(round(w0 * alt / h0))

    cinza = rasteriza(ink, larg, alt)
    dados = cinza.reshape(-1)

    nome = os.path.splitext(os.path.basename(args.saida))[0]
    with open(args.saida, "w", encoding="utf-8") as fh:
        fh.write("/* Gerado por scripts/gen_logo.py a partir de %s — não editar. */\n"
                 % os.path.basename(args.entrada))
        fh.write("#pragma once\n\n#include <stdint.h>\n\n")
        fh.write("#define LOGO_BOOT_W %d\n#define LOGO_BOOT_H %d\n\n" % (larg, alt))
        fh.write("/* Um byte por pixel: 0 = tinta preta, 255 = papel branco.\n"
                 "   Já está no tamanho final; o firmware não escala. */\n")
        fh.write("static const uint8_t %s[%d] = {\n" % (nome, len(dados)))
        for i in range(0, len(dados), 16):
            fh.write("    " + " ".join("0x%02x," % b for b in dados[i:i + 16]) + "\n")
        fh.write("};\n")

    tons = len(np.unique(cinza))
    print("%s: fonte %dx%d -> %dx%d, %d bytes, %d tons"
          % (args.saida, w0, h0, larg, alt, len(dados), tons))
    return 0


if __name__ == "__main__":
    sys.exit(main())
