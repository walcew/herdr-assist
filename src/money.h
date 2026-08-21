/**
 * @file
 * @brief Formatação de dinheiro (US$) conforme o idioma do painel.
 *
 * Antes a ponte mandava a string pronta em pt-BR ("~US$ 4,20") e o painel só
 * exibia — com a tela em inglês o número saía com vírgula decimal mesmo assim,
 * porque a ponte não conhece o idioma configurado. Agora ela manda centavos e
 * quem formata é aqui.
 *
 * Função pura, sem LVGL/ESP — testável no host (scripts/money_test.c).
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Escreve `cents` como dinheiro em `buf`.
 *
 * pt-BR: "US$ 4,20" e "US$ 1.240"   (vírgula decimal, ponto de milhar)
 * en-US: "$4.20"    e "$1,240"      (ponto decimal, vírgula de milhar)
 *
 * A partir de US$ 100 os centavos somem: no card cabe pouco, e nessa ordem de
 * grandeza eles não informam nada.
 *
 * @param buf   destino; recebe "" se não couber (nunca fica sem terminador).
 * @param size  tamanho de `buf`; 16 bytes cobrem qualquer valor plausível.
 * @param cents valor em centavos de dólar.
 * @param br    true = pt-BR, false = en-US.
 */
void money_fmt(char *buf, size_t size, uint32_t cents, bool br);

#ifdef __cplusplus
}
#endif
