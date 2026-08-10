#include "herdr_ws.h"

#include <string.h>

#include "cJSON.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_websocket_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "herdr_model.h"
#include "wifi_creds.h"

static const char *TAG = "herdr_ws";

#define WS_URI_LEN    64
#define WS_RX_BUF_LEN (32 * 1024)   /* frame maior esperado: pane_content */

/* Se a reconexão automática não voltar nesse tempo, reinicia o cliente na marra */
#define WS_STUCK_TIMEOUT_US (30 * 1000 * 1000)
#define WS_SUPERVISOR_PERIOD_MS 5000

static esp_websocket_client_handle_t s_client;
/* Reassemblagem de frames fragmentados (payload_offset/len) */
static char s_rx_buf[WS_RX_BUF_LEN];
/* Instante do último tráfego recebido; base para detectar conexão travada */
static volatile int64_t s_last_rx_us;

static void handle_agents(const cJSON *root)
{
    const cJSON *arr = cJSON_GetObjectItem(root, "agents");
    if (!cJSON_IsArray(arr)) {
        return;
    }
    herdr_agent_t agents[HERDR_MAX_AGENTS] = {0};
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
    herdr_blocked_t b = {0};
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

static void handle_pane_content(const cJSON *root)
{
    const cJSON *pane = cJSON_GetObjectItem(root, "pane_id");
    const cJSON *content = cJSON_GetObjectItem(root, "content");
    if (cJSON_IsString(pane) && cJSON_IsString(content)) {
        herdr_model_set_pane_content(pane->valuestring, content->valuestring);
    }
}

static void handle_message(const char *data, size_t len)
{
    cJSON *root = cJSON_ParseWithLength(data, len);
    if (!root) {
        ESP_LOGW(TAG, "JSON inválido (%d bytes)", (int)len);
        return;
    }
    const cJSON *type = cJSON_GetObjectItem(root, "type");
    if (cJSON_IsString(type)) {
        if (strcmp(type->valuestring, "agents") == 0) {
            handle_agents(root);
        } else if (strcmp(type->valuestring, "blocked") == 0) {
            handle_blocked(root);
        } else if (strcmp(type->valuestring, "pane_content") == 0) {
            handle_pane_content(root);
        } else if (strcmp(type->valuestring, "error") == 0) {
            const cJSON *m = cJSON_GetObjectItem(root, "message");
            ESP_LOGW(TAG, "erro do relay: %s", cJSON_IsString(m) ? m->valuestring : "?");
        }
        /* agent_update/command_result/push_*: ignorados na v1 */
    }
    cJSON_Delete(root);
}

static void ws_event_handler(void *arg, esp_event_base_t base, int32_t event_id, void *event_data)
{
    esp_websocket_event_data_t *ev = (esp_websocket_event_data_t *)event_data;
    switch (event_id) {
    case WEBSOCKET_EVENT_CONNECTED:
        ESP_LOGI(TAG, "conectado ao relay");
        s_last_rx_us = esp_timer_get_time();
        herdr_model_set_conn(HERDR_CONN_ONLINE);
        break;
    case WEBSOCKET_EVENT_DISCONNECTED:
    case WEBSOCKET_EVENT_CLOSED:
        ESP_LOGW(TAG, "desconectado do relay");
        herdr_model_set_conn(HERDR_CONN_CONNECTING);
        break;
    case WEBSOCKET_EVENT_DATA:
        s_last_rx_us = esp_timer_get_time();
        if (ev->op_code != 0x01 && ev->op_code != 0x00) {
            break;  /* só text/continuation; ignora ping/pong/close */
        }
        if (ev->payload_len > WS_RX_BUF_LEN) {
            ESP_LOGW(TAG, "frame de %d bytes excede buffer, descartado", ev->payload_len);
            break;
        }
        memcpy(s_rx_buf + ev->payload_offset, ev->data_ptr, ev->data_len);
        if (ev->payload_offset + ev->data_len >= ev->payload_len) {
            handle_message(s_rx_buf, ev->payload_len);
        }
        break;
    default:
        break;
    }
}

/**
 * O relay faz broadcast a cada 2s, então silêncio prolongado significa conexão
 * morta — inclusive quando a reconexão automática do cliente não se recupera
 * (observado: 4 min parado em "conectando"). Aí reiniciamos o cliente na marra.
 */
static void supervisor_task(void *arg)
{
    (void)arg;
    int ticks = 0;
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(WS_SUPERVISOR_PERIOD_MS));
        if (++ticks % 12 == 0) {  /* a cada ~60s: acompanha heap p/ detectar vazamento */
            ESP_LOGI(TAG, "vivo: conectado=%d heap=%u",
                     esp_websocket_client_is_connected(s_client),
                     (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
        }
        int64_t silent_us = esp_timer_get_time() - s_last_rx_us;
        if (silent_us > WS_STUCK_TIMEOUT_US) {
            ESP_LOGW(TAG, "sem tráfego há %llds, reiniciando cliente (heap livre: %u)",
                     silent_us / 1000000, (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
            herdr_model_set_conn(HERDR_CONN_CONNECTING);
            esp_websocket_client_stop(s_client);
            esp_websocket_client_start(s_client);
            s_last_rx_us = esp_timer_get_time();  /* dá uma janela nova antes de tentar de novo */
        }
    }
}

esp_err_t herdr_ws_start(void)
{
    char uri[WS_URI_LEN];
    snprintf(uri, sizeof(uri), "ws://%s:%d", RELAY_HOST, RELAY_PORT);

    const esp_websocket_client_config_t cfg = {
        .uri = uri,
        .buffer_size = 4096,
        .reconnect_timeout_ms = 3000,
        .network_timeout_ms = 10000,
    };
    s_client = esp_websocket_client_init(&cfg);
    if (!s_client) {
        return ESP_FAIL;
    }
    ESP_ERROR_CHECK(esp_websocket_register_events(s_client, WEBSOCKET_EVENT_ANY, ws_event_handler, NULL));
    herdr_model_set_conn(HERDR_CONN_CONNECTING);
    ESP_LOGI(TAG, "conectando em %s", uri);
    s_last_rx_us = esp_timer_get_time();
    esp_err_t err = esp_websocket_client_start(s_client);
    xTaskCreate(supervisor_task, "ws_supervisor", 3072, NULL, 4, NULL);
    return err;
}

static esp_err_t send_json(cJSON *root)
{
    esp_err_t err = ESP_FAIL;
    char *text = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!text) {
        return ESP_ERR_NO_MEM;
    }
    if (s_client && esp_websocket_client_is_connected(s_client)) {
        int sent = esp_websocket_client_send_text(s_client, text, strlen(text), pdMS_TO_TICKS(5000));
        err = sent >= 0 ? ESP_OK : ESP_FAIL;
    }
    cJSON_free(text);
    return err;
}

esp_err_t herdr_ws_read_pane(const char *pane_id, int lines)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "read_pane");
    cJSON_AddStringToObject(root, "pane_id", pane_id);
    cJSON_AddNumberToObject(root, "lines", lines);
    return send_json(root);
}

esp_err_t herdr_ws_send_keys(const char *pane_id, const char *const *keys, int key_count)
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

esp_err_t herdr_ws_send_text(const char *pane_id, const char *text)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "send_text");
    cJSON_AddStringToObject(root, "pane_id", pane_id);
    cJSON_AddStringToObject(root, "text", text);
    return send_json(root);
}

esp_err_t herdr_ws_respond(const char *pane_id, const char *text)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "respond");
    cJSON_AddStringToObject(root, "pane_id", pane_id);
    cJSON_AddStringToObject(root, "text", text);
    return send_json(root);
}

esp_err_t herdr_ws_focus(const char *pane_id)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "focus");
    cJSON_AddStringToObject(root, "pane_id", pane_id);
    return send_json(root);
}
