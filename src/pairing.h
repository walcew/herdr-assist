/**
 * @file
 * @brief Modo de pareamento: o host descobre o painel e entrega a configuração.
 *
 * Digitar 32 caracteres hexadecimais num touch de 3,5" é inviável, então o
 * sentido da configuração se inverte: com o modo ligado, o painel anuncia sua
 * presença por broadcast UDP e aceita, por TCP, uma configuração vinda de um
 * host Herdr (nome, endereço, porta e token). O host é quem já tem teclado.
 *
 * A janela é curta e só abre por toque na tela — quem não estiver na LAN
 * naquele intervalo não encontra nada aberto.
 */

#pragma once

#include <stdbool.h>

#include "esp_err.h"
#include "panel_cfg.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PAIRING_PORT       9376
#define PAIRING_TIMEOUT_S  180

typedef enum {
    PAIRING_OFF = 0,
    PAIRING_WAITING,   /* anunciando e aguardando um host */
    PAIRING_DONE,      /* recebeu configuração válida; ver pairing_result() */
} pairing_state_t;

/** Liga o modo de pareamento (desliga sozinho após PAIRING_TIMEOUT_S). */
esp_err_t pairing_start(void);

/** Desliga o modo de pareamento. Seguro chamar mesmo se já estiver desligado. */
void pairing_stop(void);

pairing_state_t pairing_state(void);

/** Segundos restantes da janela; 0 fora do modo de pareamento. */
int pairing_seconds_left(void);

/** Identificador curto do painel (3 últimos bytes do MAC), mostrado na tela. */
const char *pairing_device_id(void);

/**
 * Configuração recebida, válida quando o estado é PAIRING_DONE.
 * Quem consome grava na NVS (a task da LVGL), não esta. auto_mode true
 * quando o host pediu descoberta automática: o consumidor zera o endereço
 * antes de gravar (o campo host recebido segue preenchido — ele valida o
 * payload e serve de fallback para painéis antigos).
 */
bool pairing_result(panel_host_t *out, bool *auto_mode);

#ifdef __cplusplus
}
#endif
