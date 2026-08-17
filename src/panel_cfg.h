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
#define CFG_TOKEN_LEN  33   /* token da ponte: 32 hex + NUL */

typedef struct {
    char     name[CFG_NAME_LEN];
    char     host[CFG_HOST_LEN];
    char     token[CFG_TOKEN_LEN];
    uint16_t port;
    bool     enabled;
} panel_host_t;

/* Host vazio distingue dois estados pelo token: com token é um slot em
   descoberta automática (endereço resolvido por broadcast, só em RAM);
   sem token é um slot livre. Todo teste de "vazio" deve usar estes
   helpers — comparar só o host confunde auto com livre. */
static inline bool panel_host_is_auto(const panel_host_t *h)
{
    return h->host[0] == '\0' && h->token[0] != '\0';
}

static inline bool panel_host_is_free(const panel_host_t *h)
{
    return h->host[0] == '\0' && h->token[0] == '\0';
}

/** Orientação da tela; 0 (retrato) é o padrão de fábrica. Só o painel usa. */
typedef enum {
    CFG_ORIENT_PORTRAIT = 0,
    CFG_ORIENT_LANDSCAPE,
    CFG_ORIENT_COUNT,
} cfg_orient_t;

typedef struct {
    char         wifi_ssid[CFG_SSID_LEN];
    char         wifi_pass[CFG_PASS_LEN];
    panel_host_t hosts[CFG_MAX_HOSTS];
    uint8_t      lang;                  /* ui_lang_t; 0 (en_US) é o de fábrica */
    uint8_t      orient;                /* cfg_orient_t; 0 (retrato) é o de fábrica */
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
