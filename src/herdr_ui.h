/**
 * @file
 * @brief UI do herdr-assist (LVGL 8.4, 320x480 portrait).
 *
 * Abas do dock: Home (relógio, mascote, resumo e mapa de calor), Sessões
 * (lista de agentes), Dash (uso de limites dos provedores) e Configurações.
 * Sobrepostas: detalhe com terminal, aprovação pendente e teclado. Atualiza
 * via lv_timer observando herdr_model_generation(). Chamar com o mutex da
 * LVGL tomado (bsp_display_lock).
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
