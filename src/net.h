/**
 * @file
 * @brief Wi-Fi station do herdr-assist, com credenciais vindas da NVS.
 *
 * Fluxo: net_wifi_init() sobe o driver sem conectar (permite scan mesmo sem
 * config); net_wifi_connect() conecta e re-conecta sozinho; a UI consulta
 * net_wifi_is_up() e usa net_wifi_scan() na tela de configurações.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    char   ssid[33];
    int8_t rssi;
    bool   secure;
} net_ap_t;

/** Sobe netif + driver Wi-Fi em modo STA, sem conectar. */
esp_err_t net_wifi_init(void);

/** Conecta na rede (assíncrono; re-tenta sozinho ao cair). */
void net_wifi_connect(const char *ssid, const char *pass);

/** true enquanto houver IP. */
bool net_wifi_is_up(void);

/**
 * Varre redes 2.4GHz (bloqueia ~2s) e devolve até max redes, mais forte
 * primeiro, sem SSIDs repetidos. Retorna a quantidade, ou -1 em erro.
 */
int net_wifi_scan(net_ap_t *out, int max);

/**
 * Broadcast IPv4 da sub-rede atual (ip | ~mask), em ordem de rede; 0 sem IP.
 * Usado pela descoberta de pontes em herdr_conn.
 */
uint32_t net_subnet_broadcast(void);

#ifdef __cplusplus
}
#endif
