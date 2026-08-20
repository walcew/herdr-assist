/**
 * @file
 * @brief Log de atividade: eventos de transição dos agentes (aba Atividade).
 *
 * Um buffer em anel dos últimos eventos ("começou", "bloqueou", "terminou"),
 * alimentado pelo diff de status no herdr_model_set_agents (task de conexão) e
 * lido pela UI (task LVGL). Acesso protegido por spinlock, como o resto do
 * modelo. `activity_classify` é pura (sem lock) — testável no host.
 */
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ACTIVITY_CAP        64     /* eventos guardados (em anel) */
#define ACTIVITY_AGENT_LEN  16
#define ACTIVITY_PROJ_LEN   24

typedef enum {
    ACT_NONE = 0,   /* transição sem evento relevante */
    ACT_STARTED,    /* começou a trabalhar */
    ACT_BLOCKED,    /* ficou bloqueado (precisa decisão) */
    ACT_DONE,       /* terminou (estava working/blocked, voltou a idle ou sumiu) */
} activity_type_t;

typedef struct {
    uint32_t epoch;                 /* time() do evento */
    uint32_t dur;                   /* segundos (só ACT_DONE); 0 nos demais */
    uint8_t  type;                  /* activity_type_t */
    uint8_t  host;                  /* índice do host em panel_cfg */
    char     agent[ACTIVITY_AGENT_LEN];
    char     project[ACTIVITY_PROJ_LEN];
} activity_event_t;

void activity_log_init(void);

/**
 * Classifica a transição de status (strings "idle"/"working"/"blocked"/
 * "unknown"; "" = agente ausente). Pura, sem efeitos. NULL é tratado como "".
 */
activity_type_t activity_classify(const char *old_status, const char *new_status);

/** Registra um evento no anel (descarta o mais antigo se cheio). */
void activity_log_note(activity_type_t type, uint8_t host, const char *agent,
                       const char *project, uint32_t dur, uint32_t epoch);

/** Copia até `max` eventos, DO MAIS RECENTE ao mais antigo. Retorna quantos. */
int activity_log_get(activity_event_t *out, int max);

typedef struct {
    uint16_t started;
    uint16_t blocked;
    uint16_t done;
    uint32_t active_secs;   /* soma das durações dos "terminou" */
} activity_summary_t;

/** Agrega os eventos com epoch >= `since` (ex.: início do dia). Pura. */
activity_summary_t activity_summarize(const activity_event_t *ev, int n, uint32_t since);

#ifdef __cplusplus
}
#endif
