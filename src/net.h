/**
 * @file
 * @brief Wi-Fi station do herdr-assist: conecta na rede definida em wifi_creds.h
 */

#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Inicializa NVS, netif e Wi-Fi STA; bloqueia até obter IP (com retry infinito).
 *
 * @return ESP_OK quando conectado com IP obtido
 */
esp_err_t net_wifi_start(void);

#ifdef __cplusplus
}
#endif
