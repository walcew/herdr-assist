/**
 * @file
 * @brief OTA via GitHub Releases: checagem diária do manifest e aplicação do
 *        -update.bin publicado pelo workflow de release.
 *
 * Task própria, sem nenhuma chamada LVGL: a UI faz poll de
 * fw_update_get_status() dentro de um lv_timer (mesmo padrão do pareamento).
 * O download nunca começa sozinho — fw_update_start() é sempre um toque do
 * usuário; a checagem automática só muda o estado para AVAILABLE.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    FW_UPDATE_IDLE = 0,      /* nunca checou (sem rede ainda) */
    FW_UPDATE_CHECKING,
    FW_UPDATE_UP_TO_DATE,
    FW_UPDATE_AVAILABLE,
    FW_UPDATE_DOWNLOADING,   /* pct válido */
    FW_UPDATE_DONE,          /* gravado; a task reinicia sozinha em ~1,5s */
    FW_UPDATE_ERROR,         /* err válido */
} fw_update_state_t;

typedef enum {
    FW_ERR_NONE = 0,
    FW_ERR_NET,              /* manifest inacessível (rede, TLS, 404) */
    FW_ERR_MANIFEST,         /* manifest veio, mas não parseia */
    FW_ERR_DOWNLOAD,         /* download/gravação da imagem falhou */
} fw_update_err_t;

typedef struct {
    fw_update_state_t state;
    fw_update_err_t   err;
    uint8_t           pct;         /* 0-100, só em DOWNLOADING */
    char              latest[32];  /* versão do manifest, quando conhecida */
} fw_update_status_t;

/** Confirma o boot (rollback) e sobe a task. Chamar uma vez, no fim do setup. */
void fw_update_init(void);

/** Versão embarcada (esp_app_desc); no CI é a tag do release, via version.txt. */
const char *fw_update_current_version(void);

/** Cópia consistente do estado corrente (livre de tearing entre tasks). */
void fw_update_get_status(fw_update_status_t *out);

/** Agenda uma checagem imediata; false se já checando ou baixando. */
bool fw_update_check_now(void);

/** Inicia o download da versão anunciada; false se o estado não é AVAILABLE. */
bool fw_update_start(void);

#ifdef __cplusplus
}
#endif
