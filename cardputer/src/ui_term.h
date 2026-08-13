/**
 * @file
 * @brief Terminal colorido do Cardputer: parseia SGR e desenha célula a célula.
 *
 * O snapshot chega do host já emulado pelo Ghostty embutido no Herdr (texto +
 * SGR), e o term_parse compartilhado com o painel transforma isso num grid de
 * runs estilizados. Aqui só sobra desenhar — e reduzir a ASCII o que a fonte
 * de 6x8 não tem (box-drawing, ✓, acentos), sempre um caractere por coluna
 * para o alinhamento com o host não se perder.
 *
 * O grid custa ~20KB e o Cardputer não tem PSRAM, então ele só existe enquanto
 * a tela de terminal está aberta: term_alloc() ao entrar, term_free() ao sair.
 */

#pragma once

#include <stdbool.h>

/** Aloca o grid. tiny troca a fonte de 6x8 pela de 4x6 (60 colunas). */
bool term_alloc(bool tiny);
void term_free(void);

int term_cols(void);
int term_cell_h(void);
/** Quantas linhas de texto cabem em h pixels. */
int term_rows_for(int h);

/** Substitui o conteúdo (folding para ASCII in-place + parse). */
void term_set_content(char *ansi);
/** Esvazia e mostra um recado no lugar ("conectando...", "sem conteúdo"). */
void term_set_message(const char *msg);

/**
 * Rola a visão dentro do snapshot recebido; positivo é para trás no tempo.
 *
 * Devolve false quando não há para onde ir — e esse false é informação, não
 * falha: significa que o histórico está no host, não aqui, e quem chama deve
 * pedir a rolagem nativa. Como a ponte lê o viewport, o caso comum é o
 * snapshot ter exatamente as linhas visíveis e esta função nunca sair do lugar.
 */
bool term_scroll(int lines, int rows_visible);
/** true quando a visão está ancorada no fim (comportamento de terminal). */
bool term_at_bottom(void);
/** Linhas recebidas no último snapshot. */
int term_line_count(void);

/** Desenha as `rows` linhas visíveis a partir de y. */
void term_draw(int y, int rows);
