/**
 * @file
 * @brief Tela de configurações: rede Wi-Fi e hosts Herdr, com persistência em NVS.
 *
 * Edita uma cópia da config; "salvar" grava na NVS e reinicia o painel.
 * herdr_ui_init() chama o init; a engrenagem (ou o primeiro boot sem config)
 * chama o open. Chamar com o mutex da LVGL tomado.
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

void herdr_ui_settings_init(void);
void herdr_ui_settings_open(void);

#ifdef __cplusplus
}
#endif
