/**
 * @file
 * @brief Conexões com as pontes herdr-assist (TCP + JSON por linha).
 *
 * Uma task por host habilitado em panel_cfg; cada uma reconecta sozinha.
 * As pontes entregam o estado por push, então não há polling: cada task fica
 * bloqueada na leitura e só acorda quando o Herdr daquele host avisa mudança.
 */

#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Sobe uma task de conexão por host habilitado (esperam o Wi-Fi sozinhas). */
esp_err_t herdr_conn_start(void);

/* Comandos painel→ponte, roteados pelo índice do host em panel_cfg.
   Retornam ESP_FAIL se o host não estiver conectado. */
esp_err_t herdr_conn_read_pane(int host, const char *pane_id, int lines);
esp_err_t herdr_conn_send_keys(int host, const char *pane_id, const char *const *keys, int key_count);
esp_err_t herdr_conn_send_text(int host, const char *pane_id, const char *text);
esp_err_t herdr_conn_respond(int host, const char *pane_id, int choice, const char *label);
esp_err_t herdr_conn_focus(int host, const char *pane_id);

#ifdef __cplusplus
}
#endif
