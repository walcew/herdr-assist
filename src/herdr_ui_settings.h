/**
 * @file
 * @brief Aba de configurações: rede Wi-Fi e hosts Herdr, com persistência em NVS.
 *
 * Edita uma cópia da config; o disquete da topbar grava na NVS e reinicia o
 * painel. As subtelas (redes, senha, host) têm voltar — que descarta — e o
 * mesmo disquete, que ali só aplica os campos e volta.
 * Chamar com o mutex da LVGL tomado.
 */

#pragma once

#include <lvgl.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Monta a tela. dock_cb recebe a ui_tab_t clicada, como nas outras abas. */
void herdr_ui_settings_init(lv_event_cb_t dock_cb);

void herdr_ui_settings_show(void);
void herdr_ui_settings_hide(void);

#ifdef __cplusplus
}
#endif
