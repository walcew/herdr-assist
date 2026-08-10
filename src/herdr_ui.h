/**
 * @file
 * @brief UI do herdr-assist (LVGL 8.4, 320x480 portrait).
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
