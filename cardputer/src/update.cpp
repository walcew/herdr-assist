#include "update.h"

#include <stdio.h>
#include <string.h>

#include <HTTPClient.h>
#include <NetworkClientSecure.h>
#include <SD.h>

#include "cJSON.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "mbedtls/md.h"

#include "net.h"
#include "sd_cfg.h"

static const char *TAG = "update";

/* releases/latest/download resolve sempre para a última release publicada, sem
   precisar consultar a API do GitHub (que tem limite por IP para quem não
   autentica). O CI grava este arquivo a cada tag. */
#define MANIFEST_URL \
    "https://github.com/walcew/herdr-assist/releases/latest/download/manifest-cardputer.json"

#define CHUNK 4096          /* bloco de download; 4KB é o tamanho de setor do SD */
#define HTTP_TIMEOUT_MS 15000

static struct {
    update_state_t state;
    char latest[48];
    char url[256];
    char sha[65];
    char file[64];
    char err[64];
    int  size;
    int  done;              /* bytes escritos */
    SemaphoreHandle_t mutex;
} s;

static void lock(void)   { if (s.mutex) xSemaphoreTake(s.mutex, portMAX_DELAY); }
static void unlock(void) { if (s.mutex) xSemaphoreGive(s.mutex); }

static void fail(const char *msg)
{
    lock();
    strlcpy(s.err, msg, sizeof(s.err));
    s.state = UPD_ERROR;
    unlock();
    ESP_LOGW(TAG, "%s", msg);
}

/* ---------- comparação de versão ---------- */

/**
 * Extrai o vX.Y.Z do começo da string; false se não houver.
 *
 * A versão local pode ter cauda de build ("v0.6.0-9-gabc1234-nimbcorp") e a
 * publicada é a tag limpa com a marca ("v0.7.0-nimbcorp"). Comparar as strings
 * inteiras diria "diferente" para toda build de desenvolvimento; comparar os
 * três números diz o que interessa.
 */
static bool parse_ver(const char *v, int out[3])
{
    while (*v == 'v' || *v == 'V') {
        v++;
    }
    return sscanf(v, "%d.%d.%d", &out[0], &out[1], &out[2]) == 3;
}

/** true se `pub` é mais nova que `cur`. Sem número legível, assume que não. */
static bool is_newer(const char *pub, const char *cur)
{
    int a[3], b[3];
    if (!parse_ver(pub, a) || !parse_ver(cur, b)) {
        return false;
    }
    for (int i = 0; i < 3; i++) {
        if (a[i] != b[i]) {
            return a[i] > b[i];
        }
    }
    return false;
}

/* ---------- HTTPS ---------- */

/**
 * Prepara o cliente TLS.
 *
 * setInsecure() — sem validar a cadeia do GitHub. Não é descuido: a Arduino não
 * embarca o bundle de CAs por padrão, carregá-lo custaria flash e manutenção de
 * validade, e o que realmente protege o binário aqui é o SHA-256 do manifest,
 * conferido byte a byte na escrita. O que se perde é a garantia de que o
 * manifest veio mesmo do GitHub — um atacante na rota poderia anunciar outro
 * arquivo com o hash dele. Aceitável para trazer um .bin ao cartão, que ainda
 * exige uma ação sua no Launcher para virar firmware; NÃO seria aceitável se o
 * aparelho se regravasse sozinho com isso.
 */
static void tls_setup(NetworkClientSecure &c)
{
    c.setInsecure();
    c.setTimeout(HTTP_TIMEOUT_MS / 1000);
}

/* ---------- checagem ---------- */

static void parse_manifest(const String &body)
{
    cJSON *root = cJSON_ParseWithLength(body.c_str(), body.length());
    if (!root) {
        fail("manifest invalido");
        return;
    }
    const cJSON *ver = cJSON_GetObjectItem(root, "version");
    const cJSON *url = cJSON_GetObjectItem(root, "url");
    const cJSON *sha = cJSON_GetObjectItem(root, "sha256");
    const cJSON *sz  = cJSON_GetObjectItem(root, "size");
    if (!cJSON_IsString(ver) || !cJSON_IsString(url)) {
        cJSON_Delete(root);
        fail("manifest sem versao/url");
        return;
    }
    lock();
    strlcpy(s.latest, ver->valuestring, sizeof(s.latest));
    strlcpy(s.url, url->valuestring, sizeof(s.url));
    s.sha[0] = '\0';
    if (cJSON_IsString(sha) && strlen(sha->valuestring) == 64) {
        strlcpy(s.sha, sha->valuestring, sizeof(s.sha));
    }
    s.size = cJSON_IsNumber(sz) ? sz->valueint : 0;
    s.state = is_newer(s.latest, HERDR_ASSIST_VERSION) ? UPD_AVAILABLE : UPD_UPTODATE;
    unlock();
    cJSON_Delete(root);
    ESP_LOGI(TAG, "publicada %s (rodando %s)", s.latest, HERDR_ASSIST_VERSION);
}

static void check_task(void *arg)
{
    (void)arg;
    if (!net_wifi_is_up()) {
        fail("sem Wi-Fi");
        vTaskDelete(NULL);
        return;
    }
    NetworkClientSecure client;
    tls_setup(client);
    HTTPClient http;
    http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);   /* o GitHub redireciona */
    http.setTimeout(HTTP_TIMEOUT_MS);
    if (!http.begin(client, MANIFEST_URL)) {
        fail("nao consegui abrir a conexao");
        vTaskDelete(NULL);
        return;
    }
    int code = http.GET();
    if (code == HTTP_CODE_OK) {
        parse_manifest(http.getString());
    } else {
        char m[64];
        snprintf(m, sizeof(m), "GitHub respondeu %d", code);
        fail(m);
    }
    http.end();
    vTaskDelete(NULL);
}

/* ---------- download ---------- */

static void hex32(const uint8_t *in, char *out)
{
    static const char d[] = "0123456789abcdef";
    for (int i = 0; i < 32; i++) {
        out[2 * i]     = d[in[i] >> 4];
        out[2 * i + 1] = d[in[i] & 0xf];
    }
    out[64] = '\0';
}

static void download_task(void *arg)
{
    (void)arg;
    if (!sd_mount()) {
        fail("sem cartao");
        vTaskDelete(NULL);
        return;
    }

    /* o nome segue a convenção do cartão: uma versão por arquivo, para a
       anterior continuar ali como volta atrás */
    char destino[80];
    snprintf(destino, sizeof(destino), "/herdr-assist-%s.bin", s.latest);
    char parcial[80];
    snprintf(parcial, sizeof(parcial), "%s.parcial", destino);

    NetworkClientSecure client;
    tls_setup(client);
    HTTPClient http;
    http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
    http.setTimeout(HTTP_TIMEOUT_MS);
    if (!http.begin(client, s.url)) {
        fail("nao consegui abrir a conexao");
        vTaskDelete(NULL);
        return;
    }
    int code = http.GET();
    if (code != HTTP_CODE_OK) {
        char m[64];
        snprintf(m, sizeof(m), "download respondeu %d", code);
        http.end();
        fail(m);
        vTaskDelete(NULL);
        return;
    }

    int total = http.getSize();
    lock();
    if (total > 0) {
        s.size = total;
    }
    s.done = 0;
    s.state = UPD_DOWNLOADING;
    unlock();

    /* grava num .parcial e só renomeia no fim: um arquivo com o nome final e
       conteúdo pela metade seria oferecido pelo Launcher como se prestasse */
    SD.remove(parcial);
    File f = SD.open(parcial, FILE_WRITE);
    if (!f) {
        http.end();
        fail("nao consegui escrever no cartao");
        vTaskDelete(NULL);
        return;
    }

    mbedtls_md_context_t md;
    mbedtls_md_init(&md);
    mbedtls_md_setup(&md, mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), 0);
    mbedtls_md_starts(&md);

    uint8_t *buf = (uint8_t *)malloc(CHUNK);
    NetworkClient *stream = http.getStreamPtr();
    bool ok = buf != NULL;
    int escrito = 0;
    while (ok && http.connected() && (total <= 0 || escrito < total)) {
        size_t disponivel = stream->available();
        if (!disponivel) {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        int n = stream->readBytes(buf, disponivel > CHUNK ? CHUNK : disponivel);
        if (n <= 0) {
            break;
        }
        if (f.write(buf, n) != (size_t)n) {
            ok = false;
            fail("cartao cheio ou com falha de escrita");
            break;
        }
        mbedtls_md_update(&md, buf, n);
        escrito += n;
        lock();
        s.done = escrito;
        unlock();
    }
    uint8_t digest[32];
    mbedtls_md_finish(&md, digest);
    mbedtls_md_free(&md);
    free(buf);
    f.close();
    http.end();

    if (!ok) {
        SD.remove(parcial);
        vTaskDelete(NULL);
        return;
    }
    if (total > 0 && escrito != total) {
        SD.remove(parcial);
        fail("download incompleto");
        vTaskDelete(NULL);
        return;
    }
    if (s.sha[0]) {
        char got[65];
        hex32(digest, got);
        if (strcasecmp(got, s.sha) != 0) {
            SD.remove(parcial);
            fail("sha256 nao confere");
            vTaskDelete(NULL);
            return;
        }
    }

    SD.remove(destino);
    if (!SD.rename(parcial, destino)) {
        SD.remove(parcial);
        fail("nao consegui renomear no cartao");
        vTaskDelete(NULL);
        return;
    }

    lock();
    strlcpy(s.file, destino + 1, sizeof(s.file));   /* sem a barra, para a tela */
    s.state = UPD_READY;
    unlock();
    ESP_LOGI(TAG, "gravado %s (%d bytes)", destino, escrito);
    vTaskDelete(NULL);
}

/* ---------- API ---------- */

static void ensure_init(void)
{
    if (!s.mutex) {
        s.mutex = xSemaphoreCreateMutex();
    }
}

void update_check(void)
{
    ensure_init();
    if (s.state == UPD_CHECKING || s.state == UPD_DOWNLOADING) {
        return;
    }
    lock();
    s.state = UPD_CHECKING;
    s.err[0] = '\0';
    unlock();
    /* 12KB: o handshake TLS do mbedtls é o que manda no tamanho desta pilha */
    xTaskCreate(check_task, "upd_check", 12288, NULL, 4, NULL);
}

void update_download(void)
{
    ensure_init();
    if (s.state != UPD_AVAILABLE) {
        return;
    }
    lock();
    s.state = UPD_DOWNLOADING;
    s.err[0] = '\0';
    s.done = 0;
    unlock();
    xTaskCreate(download_task, "upd_get", 12288, NULL, 4, NULL);
}

update_state_t update_state(void) { return s.state; }
const char *update_latest(void)   { return s.latest; }
const char *update_file(void)     { return s.file; }
const char *update_error(void)    { return s.err; }
bool update_available(void)       { return s.state == UPD_AVAILABLE; }

int update_progress(void)
{
    if (s.size <= 0) {
        return 0;
    }
    long p = (long)s.done * 100 / s.size;
    return p > 100 ? 100 : (int)p;
}
