/**
 * @file
 * @brief Modelo de estado do herdr-assist: agentes vindos do relay, com acesso thread-safe.
 *
 * A task do WebSocket escreve; a task da LVGL lê. Um contador de geração
 * permite à UI detectar mudanças sem callback cruzando threads.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define HERDR_MAX_AGENTS      16
#define HERDR_ID_LEN          24
#define HERDR_NAME_LEN        32
#define HERDR_STATUS_LEN      16
#define HERDR_PROMPT_LEN      512
/* 40 linhas de terminal com box-drawing/emoji passam de 5.8KB em UTF-8 */
#define HERDR_CONTENT_LEN     8192
#define HERDR_MAX_OPTIONS     3
#define HERDR_OPTION_LEN      40

typedef struct {
    char pane_id[HERDR_ID_LEN];
    char agent[HERDR_NAME_LEN];      /* claude, codex, ... */
    char project[HERDR_NAME_LEN];    /* basename do cwd */
    char status[HERDR_STATUS_LEN];   /* idle | working | blocked | unknown */
    char workspace_id[HERDR_ID_LEN];
} herdr_agent_t;

typedef struct {
    char pane_id[HERDR_ID_LEN];
    char prompt[HERDR_PROMPT_LEN];
    char options[HERDR_MAX_OPTIONS][HERDR_OPTION_LEN];
    int  option_count;
    bool active;
} herdr_blocked_t;

/* Conteúdo de pane pedido via read_pane */
typedef struct {
    char pane_id[HERDR_ID_LEN];
    char content[HERDR_CONTENT_LEN];
} herdr_pane_content_t;

typedef enum {
    HERDR_CONN_OFFLINE = 0,   /* sem Wi-Fi ou relay fora */
    HERDR_CONN_CONNECTING,
    HERDR_CONN_ONLINE,
} herdr_conn_state_t;

void herdr_model_init(void);

/* --- Escrita (task WS) --- */
void herdr_model_set_agents(const herdr_agent_t *agents, int count);
void herdr_model_set_blocked(const herdr_blocked_t *blocked);
void herdr_model_clear_blocked(const char *pane_id);
void herdr_model_set_pane_content(const char *pane_id, const char *content);
void herdr_model_set_conn(herdr_conn_state_t state);

/* --- Leitura (task LVGL) --- */
/** Copia o estado atual; retorna o número de agentes. */
int herdr_model_get_agents(herdr_agent_t *out, int max);
bool herdr_model_get_blocked(herdr_blocked_t *out);
bool herdr_model_get_pane_content(herdr_pane_content_t *out);
herdr_conn_state_t herdr_model_get_conn(void);
/** Incrementa a cada escrita; a UI compara para saber se redesenha. */
uint32_t herdr_model_generation(void);

#ifdef __cplusplus
}
#endif
