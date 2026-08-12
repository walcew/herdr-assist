#include "fw_update.h"

#include <string.h>

#include "cJSON.h"
#include "esp_app_desc.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_https_ota.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"

#include "net.h"

static const char *TAG = "fw_update";

/* URL estável: o GitHub redireciona para o asset do release mais novo, então
   uma checagem é um único GET — sem tocar na API (rate limit) nem parsear
   release notes. O manifest é publicado pelo workflow junto com os .bin. */
#define MANIFEST_URL \
    "https://github.com/walcew/herdr-assist/releases/latest/download/manifest.json"

/* A URL assinada do redirect (objects.githubusercontent.com/...?X-Amz-...)
   passa de 512 bytes: precisa caber no buffer de resposta (header Location)
   e no de envio (request line do retry). O default de 512 não basta. */
#define HTTP_BUF_LEN    2048
#define MANIFEST_MAX    512
#define CHECK_PERIOD    pdMS_TO_TICKS(24 * 60 * 60 * 1000)  /* checagem diária */
#define BOOT_DELAY_MS   30000   /* não competir com o boot (UI, conexões) */

#define EV_CHECK  BIT0
#define EV_START  BIT1

static EventGroupHandle_t s_events;
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static fw_update_status_t s_status;   /* escrita só pela task, leitura sob s_mux */

/* Alvo do download, válido após uma checagem que achou versão diferente. */
static char s_url[256];
static uint32_t s_size;

static void set_status(fw_update_state_t state, fw_update_err_t err, uint8_t pct)
{
    taskENTER_CRITICAL(&s_mux);
    s_status.state = state;
    s_status.err = err;
    s_status.pct = pct;
    taskEXIT_CRITICAL(&s_mux);
}

const char *fw_update_current_version(void)
{
    return esp_app_get_description()->version;
}

void fw_update_get_status(fw_update_status_t *out)
{
    taskENTER_CRITICAL(&s_mux);
    *out = s_status;
    taskEXIT_CRITICAL(&s_mux);
}

bool fw_update_check_now(void)
{
    fw_update_state_t st = s_status.state;
    if (st == FW_UPDATE_CHECKING || st == FW_UPDATE_DOWNLOADING) {
        return false;
    }
    xEventGroupSetBits(s_events, EV_CHECK);
    return true;
}

bool fw_update_start(void)
{
    if (s_status.state != FW_UPDATE_AVAILABLE) {
        return false;
    }
    xEventGroupSetBits(s_events, EV_START);
    return true;
}

/* ---------- checagem ---------- */

typedef struct {
    char buf[MANIFEST_MAX + 1];
    size_t len;
} body_buf_t;

/* Acumula só o corpo da resposta final: os 302 do caminho até o asset também
   geram eventos, e não podem poluir o buffer. */
static esp_err_t manifest_ev(esp_http_client_event_t *ev)
{
    if (ev->event_id == HTTP_EVENT_ON_DATA &&
        esp_http_client_get_status_code(ev->client) == 200) {
        body_buf_t *b = ev->user_data;
        size_t n = ev->data_len;
        if (n > sizeof(b->buf) - 1 - b->len) {
            n = sizeof(b->buf) - 1 - b->len;
        }
        memcpy(b->buf + b->len, ev->data, n);
        b->len += n;
    }
    return ESP_OK;
}

static void do_check(void)
{
    set_status(FW_UPDATE_CHECKING, FW_ERR_NONE, 0);

    static body_buf_t body;            /* grande demais para a stack da task */
    body.len = 0;
    esp_http_client_config_t cfg = {
        .url = MANIFEST_URL,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .buffer_size = HTTP_BUF_LEN,
        .buffer_size_tx = HTTP_BUF_LEN,
        .timeout_ms = 10000,
        .event_handler = manifest_ev,
        .user_data = &body,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        set_status(FW_UPDATE_ERROR, FW_ERR_NET, 0);
        return;
    }
    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    if (err != ESP_OK || status != 200 || body.len == 0) {
        ESP_LOGW(TAG, "manifest inacessível: %s (HTTP %d)", esp_err_to_name(err), status);
        set_status(FW_UPDATE_ERROR, FW_ERR_NET, 0);
        return;
    }

    body.buf[body.len] = '\0';
    cJSON *root = cJSON_ParseWithLength(body.buf, body.len);
    const cJSON *ver = cJSON_GetObjectItem(root, "version");
    const cJSON *url = cJSON_GetObjectItem(root, "url");
    const cJSON *size = cJSON_GetObjectItem(root, "size");
    if (!cJSON_IsString(ver) || !cJSON_IsString(url) ||
        !cJSON_IsNumber(size) || size->valuedouble <= 0 ||
        strlen(url->valuestring) >= sizeof(s_url)) {
        ESP_LOGW(TAG, "manifest inválido (%u bytes)", (unsigned)body.len);
        cJSON_Delete(root);
        set_status(FW_UPDATE_ERROR, FW_ERR_MANIFEST, 0);
        return;
    }

    /* Diferença, não ordem: um release de correção "para trás" também conta.
       Builds locais (git describe) sempre diferem do release — inofensivo. */
    bool same = strcmp(ver->valuestring, fw_update_current_version()) == 0;
    strlcpy(s_url, url->valuestring, sizeof(s_url));
    s_size = (uint32_t)size->valuedouble;
    taskENTER_CRITICAL(&s_mux);
    strlcpy(s_status.latest, ver->valuestring, sizeof(s_status.latest));
    taskEXIT_CRITICAL(&s_mux);
    ESP_LOGI(TAG, "instalada %s, publicada %s", fw_update_current_version(),
             ver->valuestring);
    cJSON_Delete(root);
    set_status(same ? FW_UPDATE_UP_TO_DATE : FW_UPDATE_AVAILABLE, FW_ERR_NONE, 0);
}

/* ---------- download ---------- */

static void do_download(void)
{
    set_status(FW_UPDATE_DOWNLOADING, FW_ERR_NONE, 0);

    esp_http_client_config_t http = {
        .url = s_url,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .buffer_size = HTTP_BUF_LEN,
        .buffer_size_tx = HTTP_BUF_LEN,
        .timeout_ms = 30000,
        .keep_alive_enable = true,
    };
    esp_https_ota_config_t ota = { .http_config = &http };
    esp_https_ota_handle_t handle = NULL;
    esp_err_t err = esp_https_ota_begin(&ota, &handle);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "ota_begin: %s", esp_err_to_name(err));
        set_status(FW_UPDATE_ERROR, FW_ERR_DOWNLOAD, 0);
        return;
    }
    while ((err = esp_https_ota_perform(handle)) == ESP_ERR_HTTPS_OTA_IN_PROGRESS) {
        uint32_t read = (uint32_t)esp_https_ota_get_image_len_read(handle);
        uint8_t pct = (uint8_t)(read >= s_size ? 100 : read * 100ULL / s_size);
        set_status(FW_UPDATE_DOWNLOADING, FW_ERR_NONE, pct);
    }
    if (err != ESP_OK || !esp_https_ota_is_complete_data_received(handle)) {
        ESP_LOGW(TAG, "download falhou: %s", esp_err_to_name(err));
        esp_https_ota_abort(handle);
        set_status(FW_UPDATE_ERROR, FW_ERR_DOWNLOAD, 0);
        return;
    }
    /* finish valida a imagem (SHA-256 embutido) e marca o slot para o boot */
    err = esp_https_ota_finish(handle);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "imagem rejeitada: %s", esp_err_to_name(err));
        set_status(FW_UPDATE_ERROR, FW_ERR_DOWNLOAD, 0);
        return;
    }
    ESP_LOGI(TAG, "gravado %s, reiniciando", s_status.latest);
    set_status(FW_UPDATE_DONE, FW_ERR_NONE, 100);
    vTaskDelay(pdMS_TO_TICKS(1500));   /* deixa a UI pintar o aviso */
    esp_restart();
}

/* ---------- task ---------- */

static void fw_update_task(void *arg)
{
    (void)arg;
    while (!net_wifi_is_up()) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
    vTaskDelay(pdMS_TO_TICKS(BOOT_DELAY_MS));
    do_check();
    for (;;) {
        EventBits_t bits = xEventGroupWaitBits(s_events, EV_CHECK | EV_START,
                                               pdTRUE, pdFALSE, CHECK_PERIOD);
        if (bits & EV_START) {
            if (s_status.state == FW_UPDATE_AVAILABLE) {
                do_download();          /* só retorna em erro */
            }
        } else if (bits & EV_CHECK) {
            do_check();                 /* manual: falha vira feedback na tela */
        } else if (net_wifi_is_up()) {
            do_check();                 /* diária: sem rede, fica para amanhã */
        }
    }
}

void fw_update_init(void)
{
#if CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE
    /* Chegar aqui = config carregou e display subiu: o boot é são. Sem esta
       confirmação o bootloader voltaria ao slot anterior no próximo reset. */
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t st;
    if (esp_ota_get_state_partition(running, &st) == ESP_OK &&
        st == ESP_OTA_IMG_PENDING_VERIFY) {
        esp_ota_mark_app_valid_cancel_rollback();
        ESP_LOGI(TAG, "boot de %s confirmado, rollback cancelado",
                 fw_update_current_version());
    }
#endif
    s_events = xEventGroupCreate();
    xTaskCreate(fw_update_task, "fw_update", 8192, NULL, 5, NULL);
}
