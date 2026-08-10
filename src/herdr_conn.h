/**
 * @file
 * @brief Conexão com a ponte herdr-assist (TCP + JSON por linha).
 *
 * A ponte entrega o estado por push, então não há polling: a task de conexão
 * fica bloqueada na leitura e só acorda quando o Herdr avisa que algo mudou.
 */

#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Sobe a task de conexão (reconecta sozinha). Chamar depois de ter IP. */
esp_err_t herdr_conn_start(void);

/* Comandos painel→ponte. Retornam ESP_FAIL se não houver conexão. */
esp_err_t herdr_conn_read_pane(const char *pane_id, int lines);
esp_err_t herdr_conn_send_keys(const char *pane_id, const char *const *keys, int key_count);
esp_err_t herdr_conn_send_text(const char *pane_id, const char *text);
esp_err_t herdr_conn_respond(const char *pane_id, const char *text);
esp_err_t herdr_conn_focus(const char *pane_id);

#ifdef __cplusplus
}
#endif
