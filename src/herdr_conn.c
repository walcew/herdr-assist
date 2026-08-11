#include "herdr_conn.h"

#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <netdb.h>
#include <unistd.h>

#include "cJSON.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "herdr_model.h"
#include "net.h"
#include "panel_cfg.h"

static const char *TAG = "herdr_conn";

/* Maior mensagem esperada é o pane_content (com SGR: a ponte capa a linha
   JSON em 20000); o resto é bem menor. */
#define RX_BUF_LEN      24576
#define PING_PERIOD_S   20
/* Sem tráfego por este tempo a conexão é dada como morta. Precisa ser bem maior
   que o intervalo de ping para não derrubar uma conexão apenas ociosa. */
#define RX_TIMEOUT_S    50
/* Curto de propósito: o recv precisa devolver o controle com frequência para
   que o ping saia no prazo, já que com push o silêncio é o estado normal. */
#define RECV_TIMEOUT_S  5
#define RECONNECT_MS    3000

/* Uma conexão independente por host habilitado. Os buffers de parse vivem no
   slot (não na stack) porque as tasks rodam em paralelo — um scratch static
   compartilhado seria corrida entre hosts. */
typedef struct {
    int idx;                   /* índice em panel_cfg hosts[] */
    panel_host_t cfg;
    const char *label;         /* nome (ou host) para logs */
    int sock;
    SemaphoreHandle_t tx_mutex;
    char *rx;
    size_t rx_used;
    herdr_agent_t parse_agents[HERDR_MAX_AGENTS];
    herdr_limits_t parse_limits[HERDR_MAX_PROVIDERS];
    bool used;
} conn_slot_t;

static conn_slot_t s_slots[CFG_MAX_HOSTS];

static conn_slot_t *slot_for(int host)
{
    if (host < 0 || host >= CFG_MAX_HOSTS || !s_slots[host].used) {
        return NULL;
    }
    return &s_slots[host];
}

/* ---------- recebimento ---------- */

static void handle_agents(conn_slot_t *s, const cJSON *root)
{
    const cJSON *arr = cJSON_GetObjectItem(root, "agents");
    if (!cJSON_IsArray(arr)) {
        return;
    }
    memset(s->parse_agents, 0, sizeof(s->parse_agents));
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
        herdr_agent_t *a = &s->parse_agents[n];
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
        /* valuedouble, não valueint: o epoch já está perto do teto de int */
        if (cJSON_IsNumber((f = cJSON_GetObjectItem(item, "since")))) {
            a->since = (uint32_t)f->valuedouble;
        }
        n++;
    }
    herdr_model_set_agents(s->idx, s->parse_agents, n);
}

static void handle_limits(conn_slot_t *s, const cJSON *root)
{
    const cJSON *arr = cJSON_GetObjectItem(root, "providers");
    if (!cJSON_IsArray(arr)) {
        return;
    }
    /* o memset também zera padding e linhas não usadas — o memcmp do modelo
       depende disso para comparar structs inteiras */
    memset(s->parse_limits, 0, sizeof(s->parse_limits));
    int n = 0;
    const cJSON *item;
    cJSON_ArrayForEach(item, arr) {
        if (n >= HERDR_MAX_PROVIDERS) {
            break;
        }
        const cJSON *name = cJSON_GetObjectItem(item, "name");
        if (!cJSON_IsString(name)) {
            continue;
        }
        herdr_limits_t *l = &s->parse_limits[n];
        strlcpy(l->name, name->valuestring, sizeof(l->name));
        const cJSON *f;
        if (cJSON_IsString((f = cJSON_GetObjectItem(item, "plan")))) {
            strlcpy(l->plan, f->valuestring, sizeof(l->plan));
        }
        l->ok = cJSON_IsTrue(cJSON_GetObjectItem(item, "ok"));
        /* valuedouble, não valueint: o epoch já está perto do teto de int */
        if (cJSON_IsNumber((f = cJSON_GetObjectItem(item, "stale_since")))) {
            l->stale_since = (uint32_t)f->valuedouble;
        }
        const cJSON *rows = cJSON_GetObjectItem(item, "limits");
        if (cJSON_IsArray(rows)) {
            const cJSON *r;
            cJSON_ArrayForEach(r, rows) {
                if (l->row_count >= HERDR_MAX_LIMIT_ROWS) {
                    break;
                }
                const cJSON *label = cJSON_GetObjectItem(r, "label");
                const cJSON *pct = cJSON_GetObjectItem(r, "pct");
                if (!cJSON_IsString(label) || !cJSON_IsNumber(pct)) {
                    continue;
                }
                herdr_limit_row_t *row = &l->rows[l->row_count++];
                strlcpy(row->label, label->valuestring, sizeof(row->label));
                int p = pct->valueint;
                row->pct = (uint8_t)(p < 0 ? 0 : (p > 100 ? 100 : p));
                if (cJSON_IsNumber((f = cJSON_GetObjectItem(r, "resets_at")))) {
                    row->resets_at = (uint32_t)f->valuedouble;
                }
            }
        }
        n++;
    }
    herdr_model_set_limits(s->idx, s->parse_limits, n);
}

static void handle_line(conn_slot_t *s, char *line, size_t len)
{
    cJSON *root = cJSON_ParseWithLength(line, len);
    if (!root) {
        ESP_LOGW(TAG, "[%s] JSON inválido (%u bytes)", s->label, (unsigned)len);
        return;
    }
    const cJSON *type = cJSON_GetObjectItem(root, "type");
    if (cJSON_IsString(type)) {
        const char *t = type->valuestring;
        if (strcmp(t, "agents") == 0) {
            handle_agents(s, root);
        } else if (strcmp(t, "pane_content") == 0) {
            const cJSON *pane = cJSON_GetObjectItem(root, "pane_id");
            cJSON *content = cJSON_GetObjectItem(root, "content");
            if (cJSON_IsString(pane) && cJSON_IsString(content)) {
                /* valuestring é mutável: o model sanitiza glifos in-place */
                herdr_model_set_pane_content(s->idx, pane->valuestring, content->valuestring);
            }
        } else if (strcmp(t, "limits") == 0) {
            handle_limits(s, root);
        } else if (strcmp(t, "error") == 0) {
            const cJSON *m = cJSON_GetObjectItem(root, "message");
            ESP_LOGW(TAG, "[%s] ponte recusou: %s", s->label,
                     cJSON_IsString(m) ? m->valuestring : "?");
        }
        /* "pong" não precisa de ação: já contou como tráfego recebido */
    }
    cJSON_Delete(root);
}

/** Consome do buffer todas as linhas completas recebidas. */
static void drain_lines(conn_slot_t *s)
{
    size_t start = 0;
    for (size_t i = 0; i < s->rx_used; i++) {
        if (s->rx[i] != '\n') {
            continue;
        }
        if (i > start) {
            handle_line(s, s->rx + start, i - start);
        }
        start = i + 1;
    }
    if (start > 0) {
        s->rx_used -= start;
        memmove(s->rx, s->rx + start, s->rx_used);
    } else if (s->rx_used == RX_BUF_LEN) {
        /* linha maior que o buffer: descarta para não travar a conexão */
        ESP_LOGW(TAG, "[%s] linha excede %d bytes, descartada", s->label, RX_BUF_LEN);
        s->rx_used = 0;
    }
}

/* ---------- envio ---------- */

static esp_err_t send_line(conn_slot_t *s, const char *json)
{
    esp_err_t err = ESP_FAIL;
    xSemaphoreTake(s->tx_mutex, portMAX_DELAY);
    if (s->sock >= 0) {
        size_t len = strlen(json);
        if (send(s->sock, json, len, 0) == (int)len && send(s->sock, "\n", 1, 0) == 1) {
            err = ESP_OK;
        }
    }
    xSemaphoreGive(s->tx_mutex);
    return err;
}

static esp_err_t send_json(conn_slot_t *s, cJSON *root)
{
    char *text = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!text) {
        return ESP_ERR_NO_MEM;
    }
    esp_err_t err = send_line(s, text);
    cJSON_free(text);
    return err;
}

static esp_err_t send_simple(int host, const char *type, const char *pane_id)
{
    conn_slot_t *s = slot_for(host);
    if (!s) {
        return ESP_FAIL;
    }
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", type);
    cJSON_AddStringToObject(root, "pane_id", pane_id);
    return send_json(s, root);
}

esp_err_t herdr_conn_read_pane(int host, const char *pane_id, int lines, int cols, int rows)
{
    conn_slot_t *s = slot_for(host);
    if (!s) {
        return ESP_FAIL;
    }
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "read_pane");
    cJSON_AddStringToObject(root, "pane_id", pane_id);
    cJSON_AddNumberToObject(root, "lines", lines);
    cJSON_AddNumberToObject(root, "cols", cols);
    cJSON_AddNumberToObject(root, "rows", rows);
    return send_json(s, root);
}

esp_err_t herdr_conn_release_pane(int host, const char *pane_id)
{
    return send_simple(host, "release_pane", pane_id);
}

esp_err_t herdr_conn_scroll_pane(int host, const char *pane_id, int lines, bool up,
                                 int col, int row)
{
    conn_slot_t *s = slot_for(host);
    if (!s) {
        return ESP_FAIL;
    }
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "scroll_pane");
    cJSON_AddStringToObject(root, "pane_id", pane_id);
    cJSON_AddStringToObject(root, "dir", up ? "up" : "down");
    cJSON_AddNumberToObject(root, "lines", lines);
    cJSON_AddNumberToObject(root, "col", col);
    cJSON_AddNumberToObject(root, "row", row);
    return send_json(s, root);
}

esp_err_t herdr_conn_send_keys(int host, const char *pane_id, const char *const *keys, int key_count)
{
    conn_slot_t *s = slot_for(host);
    if (!s) {
        return ESP_FAIL;
    }
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "send_keys");
    cJSON_AddStringToObject(root, "pane_id", pane_id);
    cJSON *arr = cJSON_AddArrayToObject(root, "keys");
    for (int i = 0; i < key_count; i++) {
        cJSON_AddItemToArray(arr, cJSON_CreateString(keys[i]));
    }
    return send_json(s, root);
}

static esp_err_t send_with_text(int host, const char *type, const char *pane_id, const char *text)
{
    conn_slot_t *s = slot_for(host);
    if (!s) {
        return ESP_FAIL;
    }
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", type);
    cJSON_AddStringToObject(root, "pane_id", pane_id);
    cJSON_AddStringToObject(root, "text", text);
    return send_json(s, root);
}

esp_err_t herdr_conn_send_text(int host, const char *pane_id, const char *text)
{
    return send_with_text(host, "send_text", pane_id, text);
}

esp_err_t herdr_conn_focus(int host, const char *pane_id)
{
    return send_simple(host, "focus", pane_id);
}

/* ---------- task de conexão (uma por host) ---------- */

static int connect_bridge(const panel_host_t *hc)
{
    char port[8];
    snprintf(port, sizeof(port), "%u", hc->port);
    struct addrinfo hints = {
        .ai_family = AF_INET,
        .ai_socktype = SOCK_STREAM,
    };
    struct addrinfo *res = NULL;
    if (getaddrinfo(hc->host, port, &hints, &res) != 0 || !res) {
        return -1;
    }
    int sock = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (sock >= 0 && connect(sock, res->ai_addr, res->ai_addrlen) != 0) {
        close(sock);
        sock = -1;
    }
    freeaddrinfo(res);
    if (sock < 0) {
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
    conn_slot_t *s = arg;
    while (true) {
        while (!net_wifi_is_up()) {
            herdr_model_set_conn(s->idx, HERDR_CONN_OFFLINE);
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
        herdr_model_set_conn(s->idx, HERDR_CONN_CONNECTING);
        ESP_LOGI(TAG, "[%s] conectando em %s:%u", s->label, s->cfg.host, s->cfg.port);
        int sock = connect_bridge(&s->cfg);
        if (sock < 0) {
            vTaskDelay(pdMS_TO_TICKS(RECONNECT_MS));
            continue;
        }
        xSemaphoreTake(s->tx_mutex, portMAX_DELAY);
        s->sock = sock;
        xSemaphoreGive(s->tx_mutex);
        s->rx_used = 0;

        /* A ponte exige hello com token na primeira linha; sem ele, derruba.
           cJSON escapa o token caso o usuário digite algo fora do hex. */
        cJSON *hello = cJSON_CreateObject();
        cJSON_AddStringToObject(hello, "type", "hello");
        cJSON_AddStringToObject(hello, "token", s->cfg.token);
        if (send_json(s, hello) != ESP_OK) {
            ESP_LOGW(TAG, "[%s] falha ao enviar hello", s->label);
            xSemaphoreTake(s->tx_mutex, portMAX_DELAY);
            s->sock = -1;
            xSemaphoreGive(s->tx_mutex);
            close(sock);
            vTaskDelay(pdMS_TO_TICKS(RECONNECT_MS));
            continue;
        }

        herdr_model_set_conn(s->idx, HERDR_CONN_ONLINE);
        ESP_LOGI(TAG, "[%s] conectado (heap livre: %u)", s->label,
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));

        TickType_t last_rx = xTaskGetTickCount();
        TickType_t last_ping = last_rx;
        while (true) {
            int got = recv(sock, s->rx + s->rx_used, RX_BUF_LEN - s->rx_used, 0);
            TickType_t now = xTaskGetTickCount();

            if (got > 0) {
                last_rx = now;
                s->rx_used += got;
                drain_lines(s);
            } else if (got == 0) {
                ESP_LOGW(TAG, "[%s] ponte encerrou a conexão", s->label);
                break;
            } else if (errno != EAGAIN && errno != EWOULDBLOCK) {
                ESP_LOGW(TAG, "[%s] erro de leitura (errno %d)", s->label, errno);
                break;
            }
            /* got < 0 com EAGAIN é só o timeout do recv: nada a fazer aqui */

            if (now - last_rx > pdMS_TO_TICKS(RX_TIMEOUT_S * 1000)) {
                ESP_LOGW(TAG, "[%s] sem resposta há %ds, reconectando", s->label, RX_TIMEOUT_S);
                break;
            }
            if (now - last_ping > pdMS_TO_TICKS(PING_PERIOD_S * 1000)) {
                last_ping = now;
                if (send_line(s, "{\"type\":\"ping\"}") != ESP_OK) {
                    ESP_LOGW(TAG, "[%s] falha ao enviar ping", s->label);
                    break;
                }
            }
        }

        xSemaphoreTake(s->tx_mutex, portMAX_DELAY);
        s->sock = -1;
        xSemaphoreGive(s->tx_mutex);
        close(sock);
        herdr_model_set_conn(s->idx, HERDR_CONN_CONNECTING);
        vTaskDelay(pdMS_TO_TICKS(RECONNECT_MS));
    }
}

esp_err_t herdr_conn_start(void)
{
    const panel_cfg_t *cfg = panel_cfg_get();
    int started = 0;
    for (int i = 0; i < CFG_MAX_HOSTS; i++) {
        if (!cfg->hosts[i].enabled) {
            continue;
        }
        conn_slot_t *s = &s_slots[i];
        s->idx = i;
        s->cfg = cfg->hosts[i];
        s->label = s->cfg.name[0] ? s->cfg.name : s->cfg.host;
        s->sock = -1;
        s->tx_mutex = xSemaphoreCreateMutex();
        /* buffer grande e sem pressa: PSRAM, poupando a SRAM interna */
        s->rx = heap_caps_malloc(RX_BUF_LEN, MALLOC_CAP_SPIRAM);
        if (!s->rx) {
            s->rx = malloc(RX_BUF_LEN);
        }
        if (!s->rx || !s->tx_mutex) {
            ESP_LOGE(TAG, "[%s] sem memória para o slot", s->label);
            continue;
        }
        char name[16];
        snprintf(name, sizeof(name), "herdr_c%d", i);
        if (xTaskCreate(conn_task, name, 6144, s, 5, NULL) != pdPASS) {
            ESP_LOGE(TAG, "[%s] falha ao criar task", s->label);
            continue;
        }
        s->used = true;
        started++;
    }
    ESP_LOGI(TAG, "%d conexão(ões) de ponte iniciada(s)", started);
    return started > 0 ? ESP_OK : ESP_FAIL;
}
