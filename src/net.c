#include "net.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_sntp.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "net";

static volatile bool s_up;
static volatile bool s_scanning;
static bool s_have_creds;
static bool s_time_started;

/**
 * Sobe o SNTP na primeira vez que houver IP.
 *
 * O relógio da home é a única coisa que depende disso; sem sincronizar, a UI
 * mostra "--:--" em vez de uma hora inventada. O fuso vai fixo em UTC-3 (o
 * Brasil não usa horário de verão desde 2019), no formato POSIX — que é
 * invertido de propósito: "<-03>3" significa 3 horas a subtrair de UTC.
 */
static void time_start(void)
{
    if (s_time_started) {
        return;
    }
    s_time_started = true;
    setenv("TZ", "<-03>3", 1);
    tzset();
    esp_sntp_setoperatingmode(ESP_SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "pool.ntp.org");
    esp_sntp_init();
}

static void event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        s_up = false;
        /* durante o scan a desconexão é provocada por nós; não reagir */
        if (s_have_creds && !s_scanning) {
            ESP_LOGW(TAG, "desconectado do Wi-Fi, tentando de novo...");
            vTaskDelay(pdMS_TO_TICKS(2000));
            esp_wifi_connect();
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)data;
        ESP_LOGI(TAG, "IP obtido: " IPSTR, IP2STR(&event->ip_info.ip));
        s_up = true;
        time_start();
    }
}

esp_err_t net_wifi_init(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &event_handler, NULL));

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());
    return ESP_OK;
}

void net_wifi_connect(const char *ssid, const char *pass)
{
    wifi_config_t wifi_config = { 0 };
    strlcpy((char *)wifi_config.sta.ssid, ssid, sizeof(wifi_config.sta.ssid));
    strlcpy((char *)wifi_config.sta.password, pass, sizeof(wifi_config.sta.password));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    s_have_creds = true;
    ESP_LOGI(TAG, "conectando ao SSID \"%s\"...", ssid);
    esp_wifi_connect();
}

bool net_wifi_is_up(void)
{
    return s_up;
}

bool net_wifi_rssi(int8_t *out_rssi)
{
    /* durante o scan o driver desassocia de propósito (net_wifi_scan), então
       o registro do AP não vale — melhor dizer "não sei" que piscar erro */
    if (!s_up || s_scanning) {
        return false;
    }
    wifi_ap_record_t ap;
    if (esp_wifi_sta_get_ap_info(&ap) != ESP_OK) {
        return false;
    }
    *out_rssi = ap.rssi;
    return true;
}

int net_wifi_scan(net_ap_t *out, int max)
{
    s_scanning = true;
    esp_err_t err = esp_wifi_scan_start(NULL, true);
    if (err == ESP_ERR_WIFI_STATE) {
        /* colidiu com uma tentativa de conexão em andamento */
        esp_wifi_disconnect();
        err = esp_wifi_scan_start(NULL, true);
    }
    int count = -1;
    if (err == ESP_OK) {
        uint16_t n = 0;
        esp_wifi_scan_get_ap_num(&n);
        wifi_ap_record_t *recs = calloc(n, sizeof(wifi_ap_record_t));
        if (recs && esp_wifi_scan_get_ap_records(&n, recs) == ESP_OK) {
            count = 0;
            /* recs já vem ordenado por RSSI; dedup por SSID mantém o mais forte */
            for (int i = 0; i < n && count < max; i++) {
                if (recs[i].ssid[0] == '\0') {
                    continue;      /* rede oculta não dá para escolher na tela */
                }
                bool dup = false;
                for (int j = 0; j < count; j++) {
                    if (strcmp(out[j].ssid, (char *)recs[i].ssid) == 0) {
                        dup = true;
                        break;
                    }
                }
                if (dup) {
                    continue;
                }
                strlcpy(out[count].ssid, (char *)recs[i].ssid, sizeof(out[count].ssid));
                out[count].rssi = recs[i].rssi;
                out[count].secure = recs[i].authmode != WIFI_AUTH_OPEN;
                count++;
            }
        }
        free(recs);
    } else {
        ESP_LOGW(TAG, "scan falhou: %s", esp_err_to_name(err));
    }
    s_scanning = false;
    /* a desconexão forçada acima suprimiu o retry do handler; retoma aqui */
    if (s_have_creds && !s_up) {
        esp_wifi_connect();
    }
    return count;
}

uint32_t net_subnet_broadcast(void)
{
    esp_netif_t *sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    esp_netif_ip_info_t ip;
    if (!sta || esp_netif_get_ip_info(sta, &ip) != ESP_OK || ip.ip.addr == 0) {
        return 0;
    }
    return ip.ip.addr | ~ip.netmask.addr;
}
