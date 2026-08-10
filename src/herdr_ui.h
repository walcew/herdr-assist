/**
 * @file
 * @brief UI do herdr-assist (LVGL 8.4, 480x320 landscape).
 *
 * Telas: lista de agentes, detalhe com terminal + ações, modal de aprovação,
 * teclado para prompt. Atualiza via lv_timer observando herdr_model_generation().
 * Chamar com o mutex da LVGL tomado (bsp_display_lock).
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

void herdr_ui_init(void);

#ifdef __cplusplus
}
#endif
