#include "herdr_conn.h"

#include <string.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <unistd.h>

#include "cJSON.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "herdr_model.h"
#include "wifi_creds.h"

static const char *TAG = "herdr_conn";

/* Maior mensagem esperada é o pane_content; o resto é bem menor. */
#define RX_BUF_LEN      16384
#define PING_PERIOD_S   20
/* Sem tráfego por este tempo a conexão é dada como morta. Precisa ser bem maior
   que o intervalo de ping para não derrubar uma conexão apenas ociosa. */
#define RX_TIMEOUT_S    50
/* Curto de propósito: o recv precisa devolver o controle com frequência para
   que o ping saia no prazo, já que com push o silêncio é o estado normal. */
#define RECV_TIMEOUT_S  5
#define RECONNECT_MS    3000

static int s_sock = -1;
static SemaphoreHandle_t s_tx_mutex;
static char s_rx[RX_BUF_LEN];
static size_t s_rx_used;

/* ---------- recebimento ---------- */

static void handle_agents(const cJSON *root)
{
    const cJSON *arr = cJSON_GetObjectItem(root, "agents");
    if (!cJSON_IsArray(arr)) {
        return;
    }
    static herdr_agent_t agents[HERDR_MAX_AGENTS];
    memset(agents, 0, sizeof(agents));
    int n = 0;
    const cJSON *item;
    cJSON_ArrayForEach(item, arr) {
        if (n >= HERDR_MAX_AGENTS) {
            break;
        }
        const cJSON *pane = cJSON_GetObjectItem(item, "pane_id");
        if (!cJSON_IsString(pane)) {
            continue;
        }
        herdr_agent_t *a = &agents[n];
        strlcpy(a->pane_id, pane->valuestring, HERDR_ID_LEN);
        const cJSON *f;
        if (cJSON_IsString((f = cJSON_GetObjectItem(item, "agent")))) {
            strlcpy(a->agent, f->valuestring, HERDR_NAME_LEN);
        }
        if (cJSON_IsString((f = cJSON_GetObjectItem(item, "project")))) {
            strlcpy(a->project, f->valuestring, HERDR_NAME_LEN);
        }
        if (cJSON_IsString((f = cJSON_GetObjectItem(item, "status")))) {
            strlcpy(a->status, f->valuestring, HERDR_STATUS_LEN);
        }
        if (cJSON_IsString((f = cJSON_GetObjectItem(item, "workspace_id")))) {
            strlcpy(a->workspace_id, f->valuestring, HERDR_ID_LEN);
        }
        n++;
    }
    herdr_model_set_agents(agents, n);
}

static void handle_blocked(const cJSON *root)
{
    const cJSON *pane = cJSON_GetObjectItem(root, "pane_id");
    if (!cJSON_IsString(pane)) {
        return;
    }
    static herdr_blocked_t b;
    memset(&b, 0, sizeof(b));
    strlcpy(b.pane_id, pane->valuestring, HERDR_ID_LEN);
    const cJSON *prompt = cJSON_GetObjectItem(root, "prompt");
    if (cJSON_IsString(prompt)) {
        strlcpy(b.prompt, prompt->valuestring, HERDR_PROMPT_LEN);
    }
    const cJSON *opts = cJSON_GetObjectItem(root, "options");
    if (cJSON_IsArray(opts)) {
        const cJSON *o;
        cJSON_ArrayForEach(o, opts) {
            if (b.option_count >= HERDR_MAX_OPTIONS) {
                break;
            }
            if (cJSON_IsString(o)) {
                strlcpy(b.options[b.option_count++], o->valuestring, HERDR_OPTION_LEN);
            }
        }
    }
    herdr_model_set_blocked(&b);
}

static void handle_line(char *line, size_t len)
{
    cJSON *root = cJSON_ParseWithLength(line, len);
    if (!root) {
        ESP_LOGW(TAG, "JSON inválido (%u bytes)", (unsigned)len);
        return;
    }
    const cJSON *type = cJSON_GetObjectItem(root, "type");
    if (cJSON_IsString(type)) {
        const char *t = type->valuestring;
        if (strcmp(t, "agents") == 0) {
            handle_agents(root);
        } else if (strcmp(t, "blocked") == 0) {
            handle_blocked(root);
        } else if (strcmp(t, "pane_content") == 0) {
            const cJSON *pane = cJSON_GetObjectItem(root, "pane_id");
            const cJSON *content = cJSON_GetObjectItem(root, "content");
            if (cJSON_IsString(pane) && cJSON_IsString(content)) {
                herdr_model_set_pane_content(pane->valuestring, content->valuestring);
            }
        } else if (strcmp(t, "error") == 0) {
            const cJSON *m = cJSON_GetObjectItem(root, "message");
            ESP_LOGW(TAG, "ponte recusou: %s", cJSON_IsString(m) ? m->valuestring : "?");
        }
        /* "pong" não precisa de ação: já contou como tráfego recebido */
    }
    cJSON_Delete(root);
}

/** Consome do buffer todas as linhas completas recebidas. */
static void drain_lines(void)
{
    size_t start = 0;
    for (size_t i = 0; i < s_rx_used; i++) {
        if (s_rx[i] != '\n') {
            continue;
        }
        if (i > start) {
            handle_line(s_rx + start, i - start);
        }
        start = i + 1;
    }
    if (start > 0) {
        s_rx_used -= start;
        memmove(s_rx, s_rx + start, s_rx_used);
    } else if (s_rx_used == RX_BUF_LEN) {
        /* linha maior que o buffer: descarta para não travar a conexão */
        ESP_LOGW(TAG, "linha excede %d bytes, descartada", RX_BUF_LEN);
        s_rx_used = 0;
    }
}

/* ---------- envio ---------- */

static esp_err_t send_line(const char *json)
{
    esp_err_t err = ESP_FAIL;
    xSemaphoreTake(s_tx_mutex, portMAX_DELAY);
    if (s_sock >= 0) {
        size_t len = strlen(json);
        if (send(s_sock, json, len, 0) == (int)len && send(s_sock, "\n", 1, 0) == 1) {
            err = ESP_OK;
        }
    }
    xSemaphoreGive(s_tx_mutex);
    return err;
}

static esp_err_t send_json(cJSON *root)
{
    char *text = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!text) {
        return ESP_ERR_NO_MEM;
    }
    esp_err_t err = send_line(text);
    cJSON_free(text);
    return err;
}

static esp_err_t send_simple(const char *type, const char *pane_id)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", type);
    cJSON_AddStringToObject(root, "pane_id", pane_id);
    return send_json(root);
}

esp_err_t herdr_conn_read_pane(const char *pane_id, int lines)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "read_pane");
    cJSON_AddStringToObject(root, "pane_id", pane_id);
    cJSON_AddNumberToObject(root, "lines", lines);
    return send_json(root);
}

esp_err_t herdr_conn_send_keys(const char *pane_id, const char *const *keys, int key_count)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "send_keys");
    cJSON_AddStringToObject(root, "pane_id", pane_id);
    cJSON *arr = cJSON_AddArrayToObject(root, "keys");
    for (int i = 0; i < key_count; i++) {
        cJSON_AddItemToArray(arr, cJSON_CreateString(keys[i]));
    }
    return send_json(root);
}

static esp_err_t send_with_text(const char *type, const char *pane_id, const char *text)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", type);
    cJSON_AddStringToObject(root, "pane_id", pane_id);
    cJSON_AddStringToObject(root, "text", text);
    return send_json(root);
}

esp_err_t herdr_conn_send_text(const char *pane_id, const char *text)
{
    return send_with_text("send_text", pane_id, text);
}

esp_err_t herdr_conn_respond(const char *pane_id, const char *text)
{
    return send_with_text("respond", pane_id, text);
}

esp_err_t herdr_conn_focus(const char *pane_id)
{
    return send_simple("focus", pane_id);
}

/* ---------- task de conexão ---------- */

static int connect_bridge(void)
{
    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons(BRIDGE_PORT),
        .sin_addr.s_addr = inet_addr(BRIDGE_HOST),
    };
    int sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock < 0) {
        return -1;
    }
    if (connect(sock, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        close(sock);
        return -1;
    }
    struct timeval tv = { .tv_sec = RECV_TIMEOUT_S, .tv_usec = 0 };
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    int one = 1;
    setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    return sock;
}

static void conn_task(void *arg)
{
    (void)arg;
    while (true) {
        herdr_model_set_conn(HERDR_CONN_CONNECTING);
        ESP_LOGI(TAG, "conectando em %s:%d", BRIDGE_HOST, BRIDGE_PORT);
        int sock = connect_bridge();
        if (sock < 0) {
            vTaskDelay(pdMS_TO_TICKS(RECONNECT_MS));
            continue;
        }
        xSemaphoreTake(s_tx_mutex, portMAX_DELAY);
        s_sock = sock;
        xSemaphoreGive(s_tx_mutex);
        s_rx_used = 0;
        herdr_model_set_conn(HERDR_CONN_ONLINE);
        ESP_LOGI(TAG, "conectado (heap livre: %u)",
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));

        TickType_t last_rx = xTaskGetTickCount();
        TickType_t last_ping = last_rx;
        while (true) {
            int got = recv(sock, s_rx + s_rx_used, RX_BUF_LEN - s_rx_used, 0);
            TickType_t now = xTaskGetTickCount();

            if (got > 0) {
                last_rx = now;
                s_rx_used += got;
                drain_lines();
            } else if (got == 0) {
                ESP_LOGW(TAG, "ponte encerrou a conexão");
                break;
            } else if (errno != EAGAIN && errno != EWOULDBLOCK) {
                ESP_LOGW(TAG, "erro de leitura (errno %d)", errno);
                break;
            }
            /* got < 0 com EAGAIN é só o timeout do recv: nada a fazer aqui */

            if (now - last_rx > pdMS_TO_TICKS(RX_TIMEOUT_S * 1000)) {
                ESP_LOGW(TAG, "sem resposta há %ds, reconectando", RX_TIMEOUT_S);
                break;
            }
            if (now - last_ping > pdMS_TO_TICKS(PING_PERIOD_S * 1000)) {
                last_ping = now;
                if (send_line("{\"type\":\"ping\"}") != ESP_OK) {
                    ESP_LOGW(TAG, "falha ao enviar ping");
                    break;
                }
            }
        }

        xSemaphoreTake(s_tx_mutex, portMAX_DELAY);
        s_sock = -1;
        xSemaphoreGive(s_tx_mutex);
        close(sock);
        herdr_model_set_conn(HERDR_CONN_CONNECTING);
        vTaskDelay(pdMS_TO_TICKS(RECONNECT_MS));
    }
}

esp_err_t herdr_conn_start(void)
{
    s_tx_mutex = xSemaphoreCreateMutex();
    return xTaskCreate(conn_task, "herdr_conn", 6144, NULL, 5, NULL) == pdPASS
           ? ESP_OK : ESP_FAIL;
}
