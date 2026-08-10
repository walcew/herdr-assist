/**
 * @file
 * @brief Decoder dos sprites RLE do Clawd, no formato de imagem do LVGL 8.
 *
 * Formato dos dados (portado do projeto clawd-tank, MIT, © 2026 Marcio
 * Granzotto Rodrigues — ver assets/LICENSE-clawd-tank): cada frame é uma
 * sequência de pares (valor RGB565, contagem) em uint16_t; frame_offsets[i]
 * dá o offset EM WORDS do frame i dentro de rle_data, com sentinela em
 * frame_offsets[frame_count].
 */

#pragma once

#include <stdint.h>

/**
 * Decodifica um frame RLE para LV_IMG_CF_TRUE_COLOR_ALPHA em 16 bpp com
 * LV_COLOR_16_SWAP 1: 3 bytes por pixel = [cor_hi, cor_lo, alpha], com a cor
 * byte-swapped como a LVGL espera. Pixels iguais à chave saem transparentes e
 * pretos: o antialias do zoom interpola a cor dos vizinhos, e preto se dilui
 * no fundo escuro da home (a cor original da chave deixaria halo azulado).
 *
 * @param rle          pares (valor, contagem) do frame
 * @param out          buffer de saída (pixel_count * 3 bytes)
 * @param pixel_count  width * height
 * @param key          valor RGB565 tratado como transparente
 */
static inline void rle_decode_tca16_swap(const uint16_t *rle, uint8_t *out,
                                         int pixel_count, uint16_t key)
{
    int pos = 0;
    while (pos < pixel_count) {
        uint16_t v = *rle++;
        uint16_t n = *rle++;
        uint8_t a  = (v == key) ? 0x00 : 0xFF;
        uint8_t hi = a ? (v >> 8) : 0x00;
        uint8_t lo = a ? (v & 0xFF) : 0x00;
        for (uint16_t i = 0; i < n && pos < pixel_count; i++, pos++) {
            out[pos * 3 + 0] = hi;
            out[pos * 3 + 1] = lo;
            out[pos * 3 + 2] = a;
        }
    }
}
