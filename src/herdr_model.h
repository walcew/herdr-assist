/**
 * @file
 * @brief Modelo de estado do herdr-assist: agentes por host, com acesso thread-safe.
 *
 * As tasks de conexão (uma por host) escrevem; a task da LVGL lê. Um contador
 * de geração permite à UI detectar mudanças sem callback cruzando threads.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "panel_cfg.h"

#ifdef __cplusplus
extern "C" {
#endif

#define HERDR_MAX_AGENTS      16   /* por host */
#define HERDR_MAX_AGENTS_TOTAL (HERDR_MAX_AGENTS * CFG_MAX_HOSTS)
#define HERDR_ID_LEN          24
#define HERDR_NAME_LEN        32
#define HERDR_STATUS_LEN      16
#define HERDR_PROMPT_LEN      512
/* 40 linhas de terminal com SGR (cores) e box-drawing: ~6KB típicos na
 * amostra real; a ponte capa em 12000 antes de enviar. Buffer em PSRAM. */
#define HERDR_CONTENT_LEN     12288
/* O agente monta a lista de opções livremente: as de permissão têm 3, as de
 * escolha aberta passam de 5. A ponte corta o rótulo em 68 bytes. */
#define HERDR_MAX_OPTIONS     8
#define HERDR_OPTION_LEN      72
#define HERDR_MAX_PROVIDERS   4    /* provedores de IA por host (aba Dash) */
#define HERDR_MAX_LIMIT_ROWS  4    /* janelas de limite por provedor */

typedef struct {
    char    pane_id[HERDR_ID_LEN];
    char    agent[HERDR_NAME_LEN];    /* claude, codex, ... */
    char    project[HERDR_NAME_LEN];  /* basename do cwd */
    char    status[HERDR_STATUS_LEN]; /* idle | working | blocked | unknown */
    char    workspace_id[HERDR_ID_LEN];
    uint8_t host;                     /* índice em panel_cfg hosts[] */
    /* Epoch em que a ponte viu o agente entrar em "working"; 0 nos demais
     * status. A API do Herdr não expõe tempo algum, então quem carimba é ela. */
    uint32_t since;
} herdr_agent_t;

typedef struct {
    char    label[HERDR_OPTION_LEN];
    uint8_t num;    /* número que o agente mostra na tela; é o que se responde */
    bool    input;  /* abre campo de texto em vez de decidir na hora */
} herdr_option_t;

typedef struct {
    char           pane_id[HERDR_ID_LEN];
    char           prompt[HERDR_PROMPT_LEN];
    herdr_option_t options[HERDR_MAX_OPTIONS];
    int            option_count;
    bool           active;
    uint8_t        host;
} herdr_blocked_t;

/* Uma janela de limite de uso ("5h", "7d", "7d Fable"...). Os 20 bytes do
 * label acomodam o pior caso da ponte: 16 de texto + elipse de 3 + NUL. */
typedef struct {
    char     label[20];
    uint8_t  pct;        /* 0-100 */
    uint32_t resets_at;  /* epoch em que a janela zera; 0 = desconhecido */
} herdr_limit_row_t;

/* Uso de limites de um provedor de IA, coletado pela ponte (aba Dash).
 * Os textos chegam prontos para exibir; o firmware não interpreta nada. */
typedef struct {
    char     name[12];      /* "Claude", "Codex" */
    char     plan[16];      /* "Max 20x", "Plus" */
    bool     ok;            /* false = coleta falhando, valores são os últimos bons */
    uint32_t stale_since;   /* epoch do último sucesso quando !ok; 0 caso contrário */
    uint8_t  row_count;
    herdr_limit_row_t rows[HERDR_MAX_LIMIT_ROWS];
    uint8_t  host;          /* índice em panel_cfg hosts[] */
} herdr_limits_t;

typedef enum {
    HERDR_CONN_OFFLINE = 0,   /* sem Wi-Fi ou ponte fora */
    HERDR_CONN_CONNECTING,
    HERDR_CONN_ONLINE,
} herdr_conn_state_t;

void herdr_model_init(void);

/* --- Escrita (tasks de conexão; host = índice do slot) --- */
void herdr_model_set_agents(int host, const herdr_agent_t *agents, int count);
void herdr_model_set_blocked(const herdr_blocked_t *blocked);
void herdr_model_clear_blocked(int host, const char *pane_id);
/* content é sanitizado in-place (glifos sem cobertura na fonte) antes de copiar */
void herdr_model_set_pane_content(int host, const char *pane_id, char *content);
void herdr_model_set_conn(int host, herdr_conn_state_t state);
void herdr_model_set_limits(int host, const herdr_limits_t *limits, int count);

/* --- Leitura (task LVGL) --- */
/** Copia os agentes de todos os hosts (agrupados por host); retorna o total. */
int herdr_model_get_agents(herdr_agent_t *out, int max);
/** Copia os limites de todos os hosts (agrupados por host); retorna o total. */
int herdr_model_get_limits(herdr_limits_t *out, int max);
/** Copia o bloqueio ativo de menor host; false se nenhum. */
bool herdr_model_get_blocked(herdr_blocked_t *out);
/**
 * Copia o conteúdo de pane só se a sequência mudou desde *seq_inout (dedup:
 * snapshot repetido não repinta — com full_refresh, cada repintura custa um
 * quadro inteiro no QSPI). Devolve false sem copiar caso contrário.
 */
bool herdr_model_get_pane_content(char *pane_id, size_t id_size, int *host,
                                  char *content, size_t content_size,
                                  uint32_t *seq_inout);
herdr_conn_state_t herdr_model_get_conn(int host);
/** Incrementa a cada escrita; a UI compara para saber se redesenha. */
uint32_t herdr_model_generation(void);

#ifdef __cplusplus
}
#endif
