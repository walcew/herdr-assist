#include "avatar_store.h"

#include <dirent.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"

#include "avatar_pack.h"
#include "nvs.h"
#include "panel_cfg.h"
#include "sd.h"

static const char *TAG = "avatar_store";

/* Repositório padrão. É uma URL base servindo arquivos estáticos — trocar de
   hospedagem é trocar esta linha, sem nada no protocolo mudar. */
#define DEFAULT_REPO "https://raw.githubusercontent.com/walcew/herdr-avatars/main/"

#define AVATAR_DIR   SD_ROOT "/avatars"
/* O redirect assinado do GitHub (objects.githubusercontent.com/...?X-Amz-...)
   passa de 512 bytes e precisa caber no buffer de resposta. Mesma conta do
   fw_update.c. */
#define HTTP_BUF     2048
#define INDEX_MAX    2048
#define DL_CHUNK     4096
#define MAX_REDIRECT 3
#define URL_LEN      160
/* Teto de repositórios por refresh: 1 padrão + 2 do usuário + repos.txt + o
   que as pontes empurrarem. Passar disso é lido e ignorado, com log. */
#define MAX_REPOS    8
#define NVS_NS       "avatar"

#define EV_REFRESH  BIT0
#define EV_INSTALL  BIT1
#define EV_PACK     BIT2

static EventGroupHandle_t s_events;
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static avatar_store_status_t s_status;   /* escrita só pela task */

/* Catálogo: escrito pela task, lido pela LVGL. A contagem é publicada por
   último, então a UI nunca enxerga uma entrada pela metade. */
static avatar_entry_t s_cat[STORE_MAX];
static volatile int s_cat_count;

static char s_want[STORE_ID_LEN];        /* id pedido para instalar */

/* Repositórios da rodada corrente, montados no refresh. s_cat[i].repo indexa
   aqui, para o download saber de onde veio cada avatar. */
static char s_repos[MAX_REPOS][STORE_REPO_LEN];
static int  s_repo_count;

/* Empurrados pela ponte; ver avatar_store_set_bridge_repos. */
static char s_bridge[CFG_MAX_HOSTS][STORE_BRIDGE_REPOS][STORE_REPO_LEN];

/* Leitura de pacote a pedido do motor. s_pack_state é a barreira: a task só a
   move para pronto depois de s_pack estar inteira. */
static char s_pack_path[64];
static avatar_pack_t s_pack;
static volatile int s_pack_state;        /* 0 ocioso/colhido, 1 lendo, 2 pronto, -1 falhou */

static void set_status(store_state_t state, store_err_t err, uint8_t pct)
{
    taskENTER_CRITICAL(&s_mux);
    s_status.state = state;
    s_status.err = err;
    s_status.pct = pct;
    taskEXIT_CRITICAL(&s_mux);
}

void avatar_store_get_status(avatar_store_status_t *out)
{
    taskENTER_CRITICAL(&s_mux);
    *out = s_status;
    taskEXIT_CRITICAL(&s_mux);
}

/**
 * O id vira nome de arquivo em /sd/avatars, e vem de um JSON que qualquer um
 * publica. Sem esta peneira, um id como "../../config" escreveria fora do
 * diretório. Só minúsculas, dígitos, hífen e sublinhado.
 */
static bool id_ok(const char *id)
{
    size_t n = strlen(id);
    if (n == 0 || n >= STORE_ID_LEN) {
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        char c = id[i];
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
              c == '-' || c == '_')) {
            return false;
        }
    }
    return true;
}

/* Junta base + folha com uma única barra, venha a base com ou sem ela. */
static int join_url(char *out, size_t size, const char *base, const char *leaf)
{
    size_t n = strlen(base);
    return snprintf(out, size, "%s%s%s", base,
                    (n && base[n - 1] == '/') ? "" : "/", leaf);
}

static void path_of(const char *id, const char *suffix, char *out, size_t size)
{
    snprintf(out, size, AVATAR_DIR "/%s.hav%s", id, suffix);
}

static bool installed(const char *id)
{
    char path[64];
    struct stat st;
    path_of(id, "", path, sizeof(path));
    return stat(path, &st) == 0;
}

/* ---------- catálogo ---------- */

/* Acumula só o corpo da resposta final: os 302 do caminho também geram
   eventos e não podem poluir o buffer. */
typedef struct { char buf[INDEX_MAX + 1]; size_t len; } body_t;

static esp_err_t index_ev(esp_http_client_event_t *ev)
{
    if (ev->event_id == HTTP_EVENT_ON_DATA &&
        esp_http_client_get_status_code(ev->client) == 200) {
        body_t *b = ev->user_data;
        size_t n = ev->data_len;
        if (n > sizeof(b->buf) - 1 - b->len) {
            n = sizeof(b->buf) - 1 - b->len;
        }
        memcpy(b->buf + b->len, ev->data, n);
        b->len += n;
    }
    return ESP_OK;
}

/* Acrescenta ao catálogo o que este repositório anuncia. Id repetido é
   ignorado: vence quem apareceu primeiro, e o padrão é o primeiro a ser lido. */
static bool read_repo(const char *base, int repo_idx)
{
    static body_t body;               /* grande demais para a stack da task */
    body.len = 0;

    char url[URL_LEN];
    int n = join_url(url, sizeof(url), base, "index.json");
    if (n < 0 || n >= (int)sizeof(url)) {
        ESP_LOGW(TAG, "URL longa demais: %s", base);
        return false;
    }

    esp_http_client_config_t cfg = {
        .url = url,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .buffer_size = HTTP_BUF,
        .buffer_size_tx = HTTP_BUF,
        .timeout_ms = 10000,
        .event_handler = index_ev,
        .user_data = &body,
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) {
        return false;
    }
    esp_err_t err = esp_http_client_perform(c);
    int status = esp_http_client_get_status_code(c);
    esp_http_client_cleanup(c);
    if (err != ESP_OK || status != 200 || body.len == 0) {
        ESP_LOGW(TAG, "%s: %s (HTTP %d)", url, esp_err_to_name(err), status);
        return false;
    }

    body.buf[body.len] = '\0';
    cJSON *root = cJSON_ParseWithLength(body.buf, body.len);
    const cJSON *list = cJSON_GetObjectItem(root, "avatars");
    if (!cJSON_IsArray(list)) {
        ESP_LOGW(TAG, "%s não traz a lista de avatares", url);
        cJSON_Delete(root);
        return false;
    }

    const cJSON *item;
    cJSON_ArrayForEach(item, list) {
        if (s_cat_count >= STORE_MAX) {
            ESP_LOGW(TAG, "catálogo cheio em %d; o resto de %s ficou de fora",
                     STORE_MAX, base);
            break;
        }
        const cJSON *id = cJSON_GetObjectItem(item, "id");
        const cJSON *nm = cJSON_GetObjectItem(item, "name");
        const cJSON *sz = cJSON_GetObjectItem(item, "size");
        if (!cJSON_IsString(id) || !id_ok(id->valuestring)) {
            continue;   /* entrada torta não derruba o resto do repositório */
        }
        bool dup = false;
        for (int i = 0; i < s_cat_count && !dup; i++) {
            dup = strcmp(s_cat[i].id, id->valuestring) == 0;
        }
        if (dup) {
            continue;
        }
        avatar_entry_t *e = &s_cat[s_cat_count];
        memset(e, 0, sizeof(*e));
        strlcpy(e->id, id->valuestring, sizeof(e->id));
        e->repo = (uint8_t)repo_idx;
        strlcpy(e->name, cJSON_IsString(nm) ? nm->valuestring : id->valuestring,
                sizeof(e->name));
        e->size = cJSON_IsNumber(sz) && sz->valuedouble > 0
                  ? (uint32_t)sz->valuedouble : 0;
        e->installed = installed(e->id);
        s_cat_count++;
    }
    cJSON_Delete(root);
    return true;
}

/* Pacotes que estão no cartão mas nenhum repositório anuncia — copiados à mão,
   ou de um repositório que saiu da lista. Continuam utilizáveis, e a tela
   precisa mostrá-los para dar como apagar. */
static void add_local(void)
{
    DIR *d = opendir(AVATAR_DIR);
    if (!d) {
        return;
    }
    const struct dirent *e;
    while ((e = readdir(d)) && s_cat_count < STORE_MAX) {
        size_t n = strlen(e->d_name);
        if (n <= 4 || strcasecmp(e->d_name + n - 4, ".hav") != 0 ||
            n - 4 >= STORE_ID_LEN) {
            continue;
        }
        char id[STORE_ID_LEN];
        memcpy(id, e->d_name, n - 4);
        id[n - 4] = '\0';
        bool known = false;
        for (int i = 0; i < s_cat_count && !known; i++) {
            known = strcmp(s_cat[i].id, id) == 0;
        }
        if (known) {
            continue;
        }
        avatar_entry_t *ent = &s_cat[s_cat_count];
        memset(ent, 0, sizeof(*ent));
        strlcpy(ent->id, id, sizeof(ent->id));
        strlcpy(ent->name, id, sizeof(ent->name));
        ent->installed = true;
        ent->repo = 255;      /* copiado à mão, ou de repositório que saiu */
        s_cat_count++;
    }
    closedir(d);
}

void avatar_store_get_repo(int idx, char *out, size_t size)
{
    nvs_handle_t h;
    char key[8];
    out[0] = '\0';
    if (idx < 0 || idx >= STORE_USER_REPOS) {
        return;
    }
    snprintf(key, sizeof(key), "repo%d", idx);
    if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        size_t len = size;
        if (nvs_get_str(h, key, out, &len) != ESP_OK) {
            out[0] = '\0';
        }
        nvs_close(h);
    }
}

void avatar_store_set_repo(int idx, const char *url)
{
    nvs_handle_t h;
    char key[8];
    if (idx < 0 || idx >= STORE_USER_REPOS) {
        return;
    }
    snprintf(key, sizeof(key), "repo%d", idx);
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        if (url && url[0]) {
            nvs_set_str(h, key, url);
        } else {
            nvs_erase_key(h, key);   /* campo esvaziado = repositório removido */
        }
        nvs_commit(h);
        nvs_close(h);
    }
}

void avatar_store_set_bridge_repos(int host, const char *const *urls, int count)
{
    if (host < 0 || host >= CFG_MAX_HOSTS) {
        return;
    }
    memset(s_bridge[host], 0, sizeof(s_bridge[host]));
    for (int i = 0; i < count && i < STORE_BRIDGE_REPOS; i++) {
        strlcpy(s_bridge[host][i], urls[i], STORE_REPO_LEN);
    }
}

/* Acrescenta à lista da rodada, sem repetir URL. */
static void add_repo(const char *url)
{
    if (!url || !url[0] || strlen(url) >= STORE_REPO_LEN) {
        return;
    }
    for (int i = 0; i < s_repo_count; i++) {
        if (strcmp(s_repos[i], url) == 0) {
            return;
        }
    }
    if (s_repo_count >= MAX_REPOS) {
        ESP_LOGW(TAG, "mais de %d repositórios; %s ficou de fora", MAX_REPOS, url);
        return;
    }
    strlcpy(s_repos[s_repo_count++], url, STORE_REPO_LEN);
}

/* Uma URL por linha; '#' comenta e linha vazia passa. Para quem publica o
   próprio repositório e prefere escrever no cartão a digitar na tela. */
static void add_repos_from_card(void)
{
    FILE *f = fopen(AVATAR_DIR "/repos.txt", "r");
    if (!f) {
        return;
    }
    char line[STORE_REPO_LEN + 8];
    while (fgets(line, sizeof(line), f)) {
        char *p = line;
        while (*p == ' ' || *p == '\t') {
            p++;
        }
        char *end = p + strlen(p);
        while (end > p && (end[-1] == '\n' || end[-1] == '\r' ||
                           end[-1] == ' ' || end[-1] == '\t')) {
            *--end = '\0';
        }
        if (*p && *p != '#') {
            add_repo(p);
        }
    }
    fclose(f);
}

/* Ordem importa: o padrão primeiro, porque id repetido fica com o primeiro que
   apareceu — um repositório de terceiro não sequestra o nome de um avatar
   oficial só por anunciá-lo também. */
static void collect_repos(void)
{
    s_repo_count = 0;
    add_repo(DEFAULT_REPO);
    for (int i = 0; i < STORE_USER_REPOS; i++) {
        char url[STORE_REPO_LEN];
        avatar_store_get_repo(i, url, sizeof(url));
        add_repo(url);
    }
    add_repos_from_card();
    for (int h = 0; h < CFG_MAX_HOSTS; h++) {
        for (int i = 0; i < STORE_BRIDGE_REPOS; i++) {
            add_repo(s_bridge[h][i]);
        }
    }
}

static void do_refresh(void)
{
    set_status(STORE_REFRESHING, STORE_ERR_NONE, 0);

    /* O de fábrica encabeça sempre, e não vem de repositório nenhum. */
    s_cat_count = 0;
    avatar_entry_t *f = &s_cat[0];
    memset(f, 0, sizeof(*f));
    strlcpy(f->name, "Clawd", sizeof(f->name));
    f->installed = true;
    f->builtin = true;
    f->repo = 255;
    s_cat_count = 1;

    collect_repos();
    bool any = false;
    for (int i = 0; i < s_repo_count; i++) {
        any |= read_repo(s_repos[i], i);
    }
    add_local();
    set_status(STORE_READY, any ? STORE_ERR_NONE : STORE_ERR_NET, 0);
}

/* ---------- download ---------- */

/* Segue os 301/302 na mão porque o corpo é lido em pedaços (open/read), e não
   pelo perform() que trata redirect sozinho. O GitHub dá dois saltos até o
   objeto assinado. */
static bool open_following(esp_http_client_handle_t c, int64_t *out_len)
{
    for (int hop = 0; hop <= MAX_REDIRECT; hop++) {
        if (esp_http_client_open(c, 0) != ESP_OK) {
            return false;
        }
        *out_len = esp_http_client_fetch_headers(c);
        int status = esp_http_client_get_status_code(c);
        if (status == 200) {
            return true;
        }
        if (status != 301 && status != 302 && status != 307 && status != 308) {
            ESP_LOGW(TAG, "HTTP %d", status);
            return false;
        }
        esp_http_client_set_redirection(c);
        esp_http_client_close(c);
    }
    ESP_LOGW(TAG, "redirects demais");
    return false;
}

static void do_install(void)
{
    set_status(STORE_DOWNLOADING, STORE_ERR_NONE, 0);
    if (!sd_is_mounted()) {
        set_status(STORE_ERROR, STORE_ERR_NO_SD, 0);
        return;
    }

    uint32_t want = 0;
    int repo = -1;
    for (int i = 0; i < s_cat_count; i++) {
        if (strcmp(s_cat[i].id, s_want) == 0) {
            want = s_cat[i].size;
            repo = s_cat[i].repo < s_repo_count ? s_cat[i].repo : -1;
        }
    }
    if (repo < 0) {
        /* só existe no cartão: não há de onde baixar */
        set_status(STORE_ERROR, STORE_ERR_NET, 0);
        return;
    }
    if (want && sd_free_bytes() < (uint64_t)want + 64 * 1024) {
        set_status(STORE_ERROR, STORE_ERR_SPACE, 0);
        return;
    }

    char leaf[STORE_ID_LEN + 8], url[URL_LEN];
    snprintf(leaf, sizeof(leaf), "%s.hav", s_want);
    int n = join_url(url, sizeof(url), s_repos[repo], leaf);
    if (n < 0 || n >= (int)sizeof(url)) {
        set_status(STORE_ERROR, STORE_ERR_DOWNLOAD, 0);
        return;
    }

    /* Baixa para .part e só renomeia no fim: um download interrompido não
       deixa meio pacote com nome de pacote bom (scan_packs ignora .part). */
    char part[64], final[64];
    path_of(s_want, ".part", part, sizeof(part));
    path_of(s_want, "", final, sizeof(final));
    mkdir(AVATAR_DIR, 0777);

    esp_http_client_config_t cfg = {
        .url = url,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .buffer_size = HTTP_BUF,
        .buffer_size_tx = HTTP_BUF,
        .timeout_ms = 30000,
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) {
        set_status(STORE_ERROR, STORE_ERR_NET, 0);
        return;
    }
    int64_t total = 0;
    if (!open_following(c, &total)) {
        esp_http_client_cleanup(c);
        set_status(STORE_ERROR, STORE_ERR_NET, 0);
        return;
    }
    if (total > (int64_t)HAV_MAX_BYTES) {
        ESP_LOGW(TAG, "%s tem %lld bytes (teto %u)", s_want, total, HAV_MAX_BYTES);
        esp_http_client_cleanup(c);
        set_status(STORE_ERROR, STORE_ERR_SPACE, 0);
        return;
    }

    FILE *f = fopen(part, "wb");
    if (!f) {
        esp_http_client_cleanup(c);
        set_status(STORE_ERROR, STORE_ERR_DOWNLOAD, 0);
        return;
    }
    static uint8_t chunk[DL_CHUNK];   /* grande demais para a stack da task */
    uint32_t got = 0;
    bool ok = true;
    for (;;) {
        int r = esp_http_client_read(c, (char *)chunk, sizeof(chunk));
        if (r < 0) {
            ok = false;
            break;
        }
        if (r == 0) {
            break;   /* fim do corpo */
        }
        if (fwrite(chunk, 1, (size_t)r, f) != (size_t)r) {
            ok = false;      /* cartão cheio ou removido no meio */
            break;
        }
        got += (uint32_t)r;
        if (got > HAV_MAX_BYTES) {
            ok = false;      /* servidor mentiu no Content-Length */
            break;
        }
        if (total > 0) {
            set_status(STORE_DOWNLOADING, STORE_ERR_NONE,
                       (uint8_t)(got >= total ? 100 : got * 100 / total));
        }
    }
    fclose(f);
    /* completude tem que ser lida ANTES do cleanup: sem Content-Length (corpo
       em chunked) é a única forma de distinguir fim de corpo de conexão caída */
    bool complete = esp_http_client_is_complete_data_received(c);
    esp_http_client_cleanup(c);
    if (!ok || got == 0 || !complete || (total > 0 && got != (uint32_t)total)) {
        ESP_LOGW(TAG, "%s: %u de %lld bytes", s_want, (unsigned)got, total);
        unlink(part);
        set_status(STORE_ERROR, STORE_ERR_DOWNLOAD, 0);
        return;
    }

    /* Só vira pacote instalado depois de passar pela mesma validação que o
       motor usa: nunca deixar no cartão um arquivo que vá falhar na troca. */
    avatar_pack_t probe;
    if (!avatar_pack_load_file(part, &probe)) {
        unlink(part);
        set_status(STORE_ERROR, STORE_ERR_PACK, 0);
        return;
    }
    avatar_pack_free(&probe);

    unlink(final);
    if (rename(part, final) != 0) {
        unlink(part);
        set_status(STORE_ERROR, STORE_ERR_DOWNLOAD, 0);
        return;
    }
    for (int i = 0; i < s_cat_count; i++) {
        if (strcmp(s_cat[i].id, s_want) == 0) {
            s_cat[i].installed = true;
        }
    }
    ESP_LOGI(TAG, "%s instalado (%u bytes)", s_want, (unsigned)got);
    set_status(STORE_READY, STORE_ERR_NONE, 100);
}

/* ---------- API ---------- */

bool avatar_store_refresh(void)
{
    store_state_t st = s_status.state;
    if (st == STORE_REFRESHING || st == STORE_DOWNLOADING) {
        return false;
    }
    xEventGroupSetBits(s_events, EV_REFRESH);
    return true;
}

bool avatar_store_install(const char *id)
{
    store_state_t st = s_status.state;
    if (st == STORE_REFRESHING || st == STORE_DOWNLOADING || !id_ok(id)) {
        return false;
    }
    taskENTER_CRITICAL(&s_mux);
    strlcpy(s_status.id, id, sizeof(s_status.id));
    taskEXIT_CRITICAL(&s_mux);
    strlcpy(s_want, id, sizeof(s_want));
    xEventGroupSetBits(s_events, EV_INSTALL);
    return true;
}

bool avatar_store_remove(const char *id)
{
    if (!id_ok(id)) {
        return false;
    }
    char path[64];
    path_of(id, "", path, sizeof(path));
    if (unlink(path) != 0) {
        return false;
    }
    for (int i = 0; i < s_cat_count; i++) {
        if (strcmp(s_cat[i].id, id) == 0) {
            s_cat[i].installed = false;
        }
    }
    return true;
}

int avatar_store_list(avatar_entry_t *out, int max)
{
    int n = s_cat_count < max ? s_cat_count : max;
    memcpy(out, s_cat, (size_t)n * sizeof(*out));
    return n;
}

bool avatar_store_load_pack(const char *path)
{
    if (s_pack_state == 1) {
        return false;
    }
    strlcpy(s_pack_path, path, sizeof(s_pack_path));
    s_pack_state = 1;
    xEventGroupSetBits(s_events, EV_PACK);
    return true;
}

int avatar_store_take_pack(avatar_pack_t *out)
{
    int st = s_pack_state;
    if (st == 2) {
        *out = s_pack;
        memset(&s_pack, 0, sizeof(s_pack));
        s_pack_state = 0;
        return 1;
    }
    if (st == -1) {
        s_pack_state = 0;
        return -1;
    }
    return 0;
}

static void store_task(void *arg)
{
    (void)arg;
    for (;;) {
        EventBits_t bits = xEventGroupWaitBits(s_events, EV_REFRESH | EV_INSTALL | EV_PACK,
                                               pdTRUE, pdFALSE, portMAX_DELAY);
        if (bits & EV_PACK) {
            s_pack_state = avatar_pack_load_file(s_pack_path, &s_pack) ? 2 : -1;
        }
        if (bits & EV_INSTALL) {
            do_install();
        } else if (bits & EV_REFRESH) {
            do_refresh();
        }
    }
}

void avatar_store_init(void)
{
    s_events = xEventGroupCreate();
    /* 8192 porque o handshake TLS é o que manda no consumo (mesma da task de
       OTA). Criada cedo no boot de propósito: a RAM interna deste painel cai
       de 225KB para ~11KB entre o display, a UI, o Wi-Fi e as pontes — criar
       esta task no fim do setup falhava, e falhar calado deixaria o
       marketplace morto sem pista nenhuma. */
    if (xTaskCreate(store_task, "avatar_store", 8192, NULL, 4, NULL) != pdPASS) {
        ESP_LOGE(TAG, "sem RAM para a task (%u livres)",
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    }
}
