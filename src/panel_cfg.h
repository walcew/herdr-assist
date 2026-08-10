/**
 * @file
 * @brief Configuração persistente do painel (NVS): Wi-Fi + hosts Herdr.
 *
 * A config é carregada uma vez no boot e fica imutável durante a execução:
 * a tela de configurações edita uma cópia, salva e reinicia o painel. Isso
 * dispensa teardown de tasks/sockets ao trocar de rede ou de hosts.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define CFG_MAX_HOSTS  4
#define CFG_SSID_LEN   33   /* SSID tem no máximo 32 bytes */
#define CFG_PASS_LEN   65   /* senha WPA tem no máximo 64 */
#define CFG_HOST_LEN   48   /* IP ou hostname da ponte */
#define CFG_NAME_LEN   16   /* rótulo curto exibido na UI */

typedef struct {
    char     name[CFG_NAME_LEN];
    char     host[CFG_HOST_LEN];
    uint16_t port;
    bool     enabled;
} panel_host_t;

typedef struct {
    char         wifi_ssid[CFG_SSID_LEN];
    char         wifi_pass[CFG_PASS_LEN];
    panel_host_t hosts[CFG_MAX_HOSTS];
} panel_cfg_t;

/** Inicializa a NVS e carrega a config salva (zerada se não houver). */
esp_err_t panel_cfg_init(void);

/** Config carregada no boot; válida e imutável até reiniciar. */
const panel_cfg_t *panel_cfg_get(void);

/** Persiste a config na NVS (não altera a cópia em uso; reinicie depois). */
esp_err_t panel_cfg_save(const panel_cfg_t *cfg);

/** true se há um SSID configurado. */
bool panel_cfg_wifi_ok(void);

#ifdef __cplusplus
}
#endif
