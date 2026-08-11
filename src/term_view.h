/**
 * @file
 * @brief Visão de terminal colorida: renderiza o grid do term_parse.
 *
 * Widget custom (lv_obj cru + LV_EVENT_DRAW_MAIN) filho de um container
 * scrollável. O tamanho acompanha o conteúdo (largura real do pane no host,
 * célula 7x19 da lv_font_terminal_12); o pan/scroll fica no pai. A cada
 * snapshot novo o pan horizontal do usuário é preservado e o vertical
 * ancora no fim, como um terminal de verdade.
 */

#pragma once

#include <lvgl.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Cria a visão dentro do container scrollável (grid vai para a PSRAM). */
lv_obj_t *term_view_create(lv_obj_t *scroll_parent);

/** Parseia o snapshot ANSI/SGR e repinta (autoscroll + pan preservado). */
void term_view_set_ansi(lv_obj_t *tv, const char *ansi);

/** Esvazia e mostra um placeholder (troca de pane); zera o pan. */
void term_view_clear(lv_obj_t *tv, const char *placeholder);

/** Quantas colunas x linhas cabem na área visível (é o que a ponte trava). */
void term_view_fit(lv_obj_t *tv, int *cols, int *rows);

/**
 * @brief Registra quem recebe o arraste vertical como rolagem do terminal.
 *
 * Só é chamado quando o conteúdo cabe na tela — se sobra o que rolar aqui
 * dentro, o gesto continua sendo do LVGL. `lines` positivo é para trás no
 * tempo (dedo descendo), negativo é para frente; `col`/`row` são a célula sob
 * o dedo, porque a TUI só rola a região onde o ponteiro está.
 */
void term_view_set_scroll_cb(lv_obj_t *tv, void (*cb)(int lines, int col, int row));

#ifdef __cplusplus
}
#endif
