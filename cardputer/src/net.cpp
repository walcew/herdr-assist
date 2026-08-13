/**
 * @file
 * @brief Wi-Fi do Cardputer implementando o contrato net.h do painel.
 *
 * Mesma API, outro mundo: no painel quem sobe o rádio é o esp_wifi cru da IDF,
 * aqui é a lib WiFi do Arduino (que já cuida do netif, do event loop e do
 * re-connect). O resto do firmware — inclusive o herdr_conn.c compartilhado —
 * não vê diferença.
 */

#include "net.h"

#include "net_extra.h"

#include <string.h>
#include <time.h>

#include <WiFi.h>
#include <esp_wifi.h>

#include "esp_log.h"
#include "esp_sntp.h"

static const char *TAG = "net";

static bool s_have_creds;
static bool s_time_started;

/**
 * Sobe o SNTP na primeira vez que houver IP.
 *
 * O relógio da barra de status e o tempo decorrido das sessões dependem disso:
 * o "since" que a ponte carimba é epoch absoluto, e sem hora certa a conta dá
 * qualquer coisa. Fuso fixo em UTC-3, no formato POSIX (invertido de propósito:
 * "<-03>3" são 3 horas a subtrair de UTC).
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

static void on_wifi_event(WiFiEvent_t event)
{
    switch (event) {
    case ARDUINO_EVENT_WIFI_STA_GOT_IP:
        ESP_LOGI(TAG, "IP obtido: %s", WiFi.localIP().toString().c_str());
        time_start();
        break;
    case ARDUINO_EVENT_WIFI_STA_DISCONNECTED:
        /* o reconnect automático da lib cuida da volta; só registra */
        if (s_have_creds) {
            ESP_LOGW(TAG, "desconectado do Wi-Fi");
        }
        break;
    default:
        break;
    }
}

esp_err_t net_wifi_init(void)
{
    WiFi.persistent(false);          /* as credenciais são nossas, na NVS "panel" */
    WiFi.onEvent(on_wifi_event);
    WiFi.mode(WIFI_STA);
    WiFi.setAutoReconnect(true);
    WiFi.setSleep(false);            /* latência do push da ponte importa mais que mA */
    return ESP_OK;
}

void net_wifi_connect(const char *ssid, const char *pass)
{
    s_have_creds = true;
    ESP_LOGI(TAG, "conectando ao SSID \"%s\"...", ssid);
    WiFi.begin(ssid, pass);
}

bool net_wifi_is_up(void)
{
    return WiFi.status() == WL_CONNECTED && (uint32_t)WiFi.localIP() != 0;
}

const char *net_wifi_status_text(void)
{
    if (!s_have_creds) {
        return "sem rede configurada";
    }
    switch (WiFi.status()) {
    case WL_CONNECTED:       return "conectado";
    case WL_NO_SSID_AVAIL:   return "rede nao encontrada";
    case WL_CONNECT_FAILED:  return "senha recusada";
    case WL_CONNECTION_LOST: return "conexao perdida";
    case WL_DISCONNECTED:    return "conectando...";
    case WL_IDLE_STATUS:     return "ocioso";
    case WL_STOPPED:         return "radio parado";
    default:                 return "indefinido";
    }
}

bool net_wifi_rssi(int8_t *out_rssi)
{
    if (!net_wifi_is_up()) {
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
    int found = WiFi.scanNetworks(false, false);
    if (found < 0) {
        ESP_LOGW(TAG, "scan falhou (%d)", found);
        return -1;
    }
    int count = 0;
    /* a lib já devolve ordenado por RSSI; dedup por SSID mantém o mais forte */
    for (int i = 0; i < found && count < max; i++) {
        String ssid = WiFi.SSID(i);
        if (ssid.isEmpty()) {
            continue;               /* rede oculta não dá para escolher na tela */
        }
        bool dup = false;
        for (int j = 0; j < count; j++) {
            if (ssid == out[j].ssid) {
                dup = true;
                break;
            }
        }
        if (dup) {
            continue;
        }
        strlcpy(out[count].ssid, ssid.c_str(), sizeof(out[count].ssid));
        out[count].rssi = WiFi.RSSI(i);
        out[count].secure = WiFi.encryptionType(i) != WIFI_AUTH_OPEN;
        count++;
    }
    WiFi.scanDelete();
    return count;
}

uint32_t net_subnet_broadcast(void)
{
    if (!net_wifi_is_up()) {
        return 0;
    }
    uint32_t ip = (uint32_t)WiFi.localIP();
    uint32_t mask = (uint32_t)WiFi.subnetMask();
    if (!ip || !mask) {
        return 0;
    }
    return ip | ~mask;   /* já em ordem de rede: é como a IPAddress guarda */
}
