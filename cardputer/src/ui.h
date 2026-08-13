/**
 * @file
 * @brief Telas do herdr-assist no Cardputer.
 *
 * Uma tela por vez, tudo desenhado na mão sobre o M5GFX: com 240x135 e um
 * teclado no lugar do toque, a lista de sessões, o terminal e as configurações
 * cabem num punhado de linhas de texto — não há árvore de widgets a manter.
 */

#pragma once

#include <stdint.h>

/**
 * Pinta a logo de boot. Chamar antes da inicialização pesada: o Wi-Fi e a
 * montagem do cartão levam quase um segundo, e a logo cobre essa espera em vez
 * de somar a ela.
 */
void ui_splash(void);

/** Garante que a logo tenha ficado pelo menos min_ms na tela. */
void ui_splash_hold(uint32_t min_ms);

/** Monta a primeira tela (a config já precisa estar carregada). */
void ui_init(void);

/** Passo do laço principal: teclado, dados novos da ponte e repintura. */
void ui_tick(void);
