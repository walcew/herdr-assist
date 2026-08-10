/**
 * @file
 * @brief Cliente WebSocket do relay herdr-remote.
 *
 * Conecta em ws://RELAY_HOST:RELAY_PORT (wifi_creds.h), alimenta o
 * herdr_model com as mensagens do relay e expõe helpers de comando.
 */

#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Inicia o cliente (reconexão automática). Chamar após ter IP. */
esp_err_t herdr_ws_start(void);

/* Comandos device→relay (retornam ESP_FAIL se desconectado) */
esp_err_t herdr_ws_read_pane(const char *pane_id, int lines);
esp_err_t herdr_ws_send_keys(const char *pane_id, const char *const *keys, int key_count);
esp_err_t herdr_ws_send_text(const char *pane_id, const char *text);
esp_err_t herdr_ws_respond(const char *pane_id, const char *text);
esp_err_t herdr_ws_focus(const char *pane_id);

#ifdef __cplusplus
}
#endif
