#include "pairing.h"

#include <string.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>

#include "cJSON.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "pairing";

/* O anúncio cabe folgado nisto; a configuração recebida é um pouco maior. */
#define RX_LEN        512
#define BEACON_MS     1000

static SemaphoreHandle_t s_mutex;
static volatile pairing_state_t s_state;
static volatile bool s_stop;
static TickType_t s_deadline;
static char s_id[7];
static panel_host_t s_result;
static bool s_auto;             /* host pediu descoberta automática */

static void set_state(pairing_state_t st)
{
    s_state = st;
}

/* ---------- recebimento da configuração ---------- */

/**
 * Interpreta a configuração enviada pelo host.
 *
 * Rejeita o que não dá para usar depois: sem endereço, porta ou token, o
 * painel ficaria com um host que nunca conecta — pior que não parear.
 */
static bool parse_pair(const char *json, size_t len, panel_host_t *out, bool *auto_out)
{
    cJSON *root = cJSON_ParseWithLength(json, len);
    if (!root) {
        return false;
    }
    bool ok = false;
    const cJSON *type = cJSON_GetObjectItem(root, "t");
    const cJSON *host = cJSON_GetObjectItem(root, "host");
    const cJSON *port = cJSON_GetObjectItem(root, "port");
    const cJSON *token = cJSON_GetObjectItem(root, "token");
    const cJSON *name = cJSON_GetObjectItem(root, "name");

    if (cJSON_IsString(type) && strcmp(type->valuestring, "pair") == 0 &&
        cJSON_IsString(host) && host->valuestring[0] &&
        cJSON_IsNumber(port) && port->valueint > 0 && port->valueint <= 65535 &&
        cJSON_IsString(token) && token->valuestring[0]) {
        memset(out, 0, sizeof(*out));
        strlcpy(out->host, host->valuestring, CFG_HOST_LEN);
        strlcpy(out->token, token->valuestring, CFG_TOKEN_LEN);
        /* sem nome, o endereço serve de rótulo */
        strlcpy(out->name, cJSON_IsString(name) && name->valuestring[0]
                           ? name->valuestring : host->valuestring, CFG_NAME_LEN);
        out->port = (uint16_t)port->valueint;
        out->enabled = true;
        /* o host continua preenchido mesmo com auto: valida o payload e é o
           que um firmware sem descoberta gravaria — quem zera é o pair_apply */
        *auto_out = cJSON_IsTrue(cJSON_GetObjectItem(root, "auto"));
        ok = true;
    }
    cJSON_Delete(root);
    return ok;
}

/** Lê uma linha da conexão e responde. true se pareou. */
static bool serve_client(int fd)
{
    struct timeval tv = { .tv_sec = 5, .tv_usec = 0 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    char buf[RX_LEN];
    size_t used = 0;
    while (used < sizeof(buf) - 1) {
        int got = recv(fd, buf + used, sizeof(buf) - 1 - used, 0);
        if (got <= 0) {
            break;
        }
        used += got;
        buf[used] = '\0';
        if (memchr(buf, '\n', used)) {
            break;
        }
    }

    panel_host_t cfg;
    bool auto_mode = false;
    bool ok = used > 0 && parse_pair(buf, used, &cfg, &auto_mode);
    if (ok) {
        xSemaphoreTake(s_mutex, portMAX_DELAY);
        s_result = cfg;
        s_auto = auto_mode;
        xSemaphoreGive(s_mutex);
        ESP_LOGI(TAG, "pareado com \"%s\" (%s:%u%s)", cfg.name, cfg.host, cfg.port,
                 auto_mode ? ", auto" : "");
        send(fd, "{\"t\":\"paired\",\"ok\":true}\n", 25, 0);
    } else {
        ESP_LOGW(TAG, "configuração inválida recebida");
        send(fd, "{\"t\":\"paired\",\"ok\":false}\n", 26, 0);
    }
    return ok;
}

/* ---------- task ---------- */

/**
 * Anuncia o painel na rede.
 *
 * Vai para 255.255.255.255 e também para o broadcast da própria sub-rede: o
 * primeiro é o caminho comum, mas nem toda pilha o encaminha, e o segundo é o
 * que roteadores domésticos costumam entregar sem reclamar.
 */
static void send_beacon(int udp, const char *msg)
{
    size_t len = strlen(msg);
    struct sockaddr_in to = {
        .sin_family = AF_INET,
        .sin_port = htons(PAIRING_PORT),
        .sin_addr.s_addr = htonl(INADDR_BROADCAST),
    };
    sendto(udp, msg, len, 0, (struct sockaddr *)&to, sizeof(to));

    esp_netif_ip_info_t ip;
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (netif && esp_netif_get_ip_info(netif, &ip) == ESP_OK && ip.ip.addr) {
        to.sin_addr.s_addr = ip.ip.addr | ~ip.netmask.addr;
        sendto(udp, msg, len, 0, (struct sockaddr *)&to, sizeof(to));
    }
}

static void pairing_task(void *arg)
{
    (void)arg;

    int udp = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    int one = 1;
    if (udp >= 0) {
        setsockopt(udp, SOL_SOCKET, SO_BROADCAST, &one, sizeof(one));
    }

    int srv = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons(PAIRING_PORT),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    if (srv < 0 || bind(srv, (struct sockaddr *)&addr, sizeof(addr)) != 0 ||
        listen(srv, 1) != 0) {
        ESP_LOGE(TAG, "não foi possível abrir a porta %d", PAIRING_PORT);
        if (srv >= 0) close(srv);
        if (udp >= 0) close(udp);
        set_state(PAIRING_OFF);
        vTaskDelete(NULL);
        return;
    }
    /* accept devolve o controle a cada segundo para o anúncio sair no ritmo */
    struct timeval tv = { .tv_sec = 1, .tv_usec = 0 };
    setsockopt(srv, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    char beacon[96];
    snprintf(beacon, sizeof(beacon),
             "{\"t\":\"herdr-assist\",\"id\":\"%s\",\"port\":%d}", s_id, PAIRING_PORT);

    ESP_LOGI(TAG, "modo de pareamento ligado (id %s, porta %d)", s_id, PAIRING_PORT);
    while (!s_stop && xTaskGetTickCount() < s_deadline) {
        send_beacon(udp, beacon);
        int fd = accept(srv, NULL, NULL);
        if (fd < 0) {
            continue;               /* timeout do accept: só reanuncia */
        }
        bool paired = serve_client(fd);
        close(fd);
        if (paired) {
            set_state(PAIRING_DONE);
            break;
        }
    }

    close(srv);
    if (udp >= 0) {
        close(udp);
    }
    if (s_state != PAIRING_DONE) {
        ESP_LOGI(TAG, "modo de pareamento encerrado sem parear");
        set_state(PAIRING_OFF);
    }
    vTaskDelete(NULL);
}

/* ---------- API ---------- */

esp_err_t pairing_start(void)
{
    if (s_state == PAIRING_WAITING) {
        return ESP_OK;
    }
    if (!s_mutex) {
        s_mutex = xSemaphoreCreateMutex();
    }
    if (!s_id[0]) {
        uint8_t mac[6] = { 0 };
        esp_read_mac(mac, ESP_MAC_WIFI_STA);
        snprintf(s_id, sizeof(s_id), "%02X%02X%02X", mac[3], mac[4], mac[5]);
    }
    memset(&s_result, 0, sizeof(s_result));
    s_auto = false;
    s_stop = false;
    s_deadline = xTaskGetTickCount() + pdMS_TO_TICKS(PAIRING_TIMEOUT_S * 1000);
    set_state(PAIRING_WAITING);
    if (xTaskCreate(pairing_task, "pairing", 4096, NULL, 5, NULL) != pdPASS) {
        set_state(PAIRING_OFF);
        return ESP_FAIL;
    }
    return ESP_OK;
}

void pairing_stop(void)
{
    s_stop = true;
}

pairing_state_t pairing_state(void)
{
    return s_state;
}

int pairing_seconds_left(void)
{
    if (s_state != PAIRING_WAITING) {
        return 0;
    }
    TickType_t now = xTaskGetTickCount();
    if (now >= s_deadline) {
        return 0;
    }
    return (int)((s_deadline - now) / configTICK_RATE_HZ);
}

const char *pairing_device_id(void)
{
    return s_id;
}

bool pairing_result(panel_host_t *out, bool *auto_mode)
{
    if (s_state != PAIRING_DONE) {
        return false;
    }
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    *out = s_result;
    *auto_mode = s_auto;
    xSemaphoreGive(s_mutex);
    return out->host[0] != '\0';
}
