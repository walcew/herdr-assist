/**
 * @file
 * @brief UI do herdr-assist (LVGL 8.4, 320x480; deitada vira 480x320).
 *
 * Abas do dock: Home (relógio, beacon de decisão, mascote, resumo e mapa de
 * calor), Sessões (lista de agentes), Dash e Configurações. A Dash tem duas
 * páginas lado a lado, trocadas arrastando na horizontal: uso de limites dos
 * provedores e feed de atividade dos agentes. Sobrepostas: detalhe com
 * terminal e teclado. Atualiza via lv_timer observando
 * herdr_model_generation(). Chamar com o mutex da LVGL tomado
 * (bsp_display_lock).
 *
 * A orientação vem da config e é aplicada no boot: as telas se montam uma vez
 * já sabendo a proporção. Em paisagem o dock fica em pé à esquerda (UI_DOCK_W)
 * e a home se divide em duas colunas.
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

void herdr_ui_init(void);

/** Abre a aba de configurações (usado no primeiro boot, sem config salva). */
void herdr_ui_show_settings(void);

#ifdef __cplusplus
}
#endif
