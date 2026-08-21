/**
 * @file
 * @brief Conexões com as pontes herdr-assist (TCP + JSON por linha).
 *
 * Uma task por host habilitado em panel_cfg; cada uma reconecta sozinha.
 * As pontes entregam o estado por push, então não há polling: cada task fica
 * bloqueada na leitura e só acorda quando o Herdr daquele host avisa mudança.
 */

#pragma once

#include <stdbool.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Callback do payload `avatar_repos`, que a ponte envia no handshake com as
 * URLs de repositório de avatar configuradas no plugin.
 *
 * É callback e não chamada direta porque este arquivo é compartilhado com o
 * porte do Cardputer (cardputer/sync_shared.py), que não tem marketplace — lá
 * ninguém registra nada e o payload é ignorado, sem flag de build nem
 * divergência entre os dois firmwares.
 */
typedef void (*herdr_repos_cb_t)(int host, const char *const *urls, int count);

/** Registra o consumidor de `avatar_repos`. Chamar antes de herdr_conn_start(). */
void herdr_conn_set_repos_cb(herdr_repos_cb_t cb);

/** Sobe uma task de conexão por host habilitado (esperam o Wi-Fi sozinhas). */
esp_err_t herdr_conn_start(void);

/* Comandos painel→ponte, roteados pelo índice do host em panel_cfg.
   Retornam ESP_FAIL se o host não estiver conectado. */
/* cols/rows são a geometria da tela do painel: enquanto ele estiver lendo este
   pane, a ponte trava a sessão nesse tamanho (release_pane devolve). */
esp_err_t herdr_conn_read_pane(int host, const char *pane_id, int lines, int cols, int rows);
esp_err_t herdr_conn_release_pane(int host, const char *pane_id);
/* Rolagem nativa: uma roda de mouse no pane, na célula col/row (up = para trás
   no tempo). A posição importa: TUI só rola a região sob o ponteiro. */
esp_err_t herdr_conn_scroll_pane(int host, const char *pane_id, int lines, bool up,
                                 int col, int row);
esp_err_t herdr_conn_send_keys(int host, const char *pane_id, const char *const *keys, int key_count);
esp_err_t herdr_conn_send_text(int host, const char *pane_id, const char *text);
esp_err_t herdr_conn_focus(int host, const char *pane_id);

#ifdef __cplusplus
}
#endif
