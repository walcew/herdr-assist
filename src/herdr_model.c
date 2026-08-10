#include "herdr_model.h"

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static struct {
    herdr_agent_t agents[CFG_MAX_HOSTS][HERDR_MAX_AGENTS];
    int agent_count[CFG_MAX_HOSTS];
    herdr_blocked_t blocked[CFG_MAX_HOSTS];
    herdr_pane_content_t pane_content;
    herdr_conn_state_t conn[CFG_MAX_HOSTS];
    uint32_t generation;
    SemaphoreHandle_t mutex;
} s_model;

static void lock(void)   { xSemaphoreTake(s_model.mutex, portMAX_DELAY); }
static void unlock(void) { xSemaphoreGive(s_model.mutex); }
static void bump(void)   { s_model.generation++; }

static bool host_ok(int host)
{
    return host >= 0 && host < CFG_MAX_HOSTS;
}

void herdr_model_init(void)
{
    memset(&s_model, 0, sizeof(s_model));
    s_model.mutex = xSemaphoreCreateMutex();
}

void herdr_model_set_agents(int host, const herdr_agent_t *agents, int count)
{
    if (!host_ok(host)) {
        return;
    }
    if (count > HERDR_MAX_AGENTS) {
        count = HERDR_MAX_AGENTS;
    }
    lock();
    /* a ponte pode reenviar snapshot idêntico; só bumpa se mudou. O campo host
       é carimbado aqui, então a comparação precisa carimbá-lo antes. */
    bool changed = (count != s_model.agent_count[host]);
    for (int i = 0; i < count && !changed; i++) {
        herdr_agent_t tmp = agents[i];
        tmp.host = (uint8_t)host;
        changed = memcmp(&s_model.agents[host][i], &tmp, sizeof(tmp)) != 0;
    }
    memcpy(s_model.agents[host], agents, count * sizeof(herdr_agent_t));
    s_model.agent_count[host] = count;
    for (int i = 0; i < count; i++) {
        s_model.agents[host][i].host = (uint8_t)host;
    }
    /* blocked deixa de valer se o pane sumiu ou se o agente destravou */
    herdr_blocked_t *b = &s_model.blocked[host];
    if (b->active) {
        bool still_blocked = false;
        for (int i = 0; i < count; i++) {
            if (strcmp(s_model.agents[host][i].pane_id, b->pane_id) == 0) {
                still_blocked = strcmp(s_model.agents[host][i].status, "blocked") == 0;
                break;
            }
        }
        if (!still_blocked) {
            b->active = false;
            changed = true;
        }
    }
    if (changed) {
        bump();
    }
    unlock();
}

void herdr_model_set_blocked(const herdr_blocked_t *blocked)
{
    if (!host_ok(blocked->host)) {
        return;
    }
    lock();
    s_model.blocked[blocked->host] = *blocked;
    s_model.blocked[blocked->host].active = true;
    bump();
    unlock();
}

void herdr_model_clear_blocked(int host, const char *pane_id)
{
    if (!host_ok(host)) {
        return;
    }
    lock();
    herdr_blocked_t *b = &s_model.blocked[host];
    if (b->active && strcmp(b->pane_id, pane_id) == 0) {
        b->active = false;
        bump();
    }
    unlock();
}

/**
 * Copia texto truncando só em fronteira de caractere UTF-8.
 *
 * A saída do terminal é cheia de box-drawing e emoji, então um corte cego no
 * meio de uma sequência multibyte deixa bytes de continuação órfãos — e a LVGL,
 * configurada com LV_TXT_ENC_UTF8, trava ao decodificar isso.
 */
static void copy_utf8_safe(char *dst, const char *src, size_t dst_size)
{
    size_t len = strlen(src);
    if (len >= dst_size) {
        len = dst_size - 1;
        /* Recua enquanto o corte cair sobre um byte de continuação (10xxxxxx).
           Ao parar, src[len] é ASCII ou líder de sequência — e como ele fica de
           fora da cópia, tudo que sobra são caracteres completos. */
        while (len > 0 && ((unsigned char)src[len] & 0xC0) == 0x80) {
            len--;
        }
    }
    memcpy(dst, src, len);
    dst[len] = '\0';
}

/**
 * Substitui o que a fonte do terminal não desenha, evitando retângulos vazios.
 *
 * A fonte traz a JetBrainsMono Nerd inteira, então sobra pouco: o ⎿ (U+23BF) da
 * árvore de tool calls, que a fonte não tem e vira └ (mesmo sentido, mesmos 3
 * bytes), e os emojis, que nenhuma fonte monoespaçada cobre.
 *
 * Emoji e ícone Nerd são ambos 4 bytes em UTF-8 e só se distinguem pelo primeiro:
 * 0xF0 abre os planos 1–3 (onde vivem os emojis) e 0xF3 abre a área privada
 * U+F0000+ (ícones Material Design, que a fonte tem). Só os primeiros são trocados.
 */
static void replace_missing_glyphs(char *s)
{
    unsigned char *w = (unsigned char *)s;
    const unsigned char *r = (const unsigned char *)s;
    while (*r) {
        if (r[0] == 0xE2 && r[1] == 0x8E && r[2] == 0xBF) {
            *w++ = 0xE2;            /* ⎿ -> └ */
            *w++ = 0x94;
            *w++ = 0x94;
            r += 3;
        } else if (r[0] == 0xF0 && r[1] == 0x9F && r[2] && r[3]) {
            *w++ = '*';             /* emoji -> marcador de uma coluna */
            r += 4;
        } else {
            *w++ = *r++;
        }
    }
    *w = '\0';
}

void herdr_model_set_pane_content(int host, const char *pane_id, const char *content)
{
    if (!host_ok(host)) {
        return;
    }
    lock();
    s_model.pane_content.host = (uint8_t)host;
    strlcpy(s_model.pane_content.pane_id, pane_id, HERDR_ID_LEN);
    copy_utf8_safe(s_model.pane_content.content, content, HERDR_CONTENT_LEN);
    replace_missing_glyphs(s_model.pane_content.content);
    bump();
    unlock();
}

void herdr_model_set_conn(int host, herdr_conn_state_t state)
{
    if (!host_ok(host)) {
        return;
    }
    lock();
    if (s_model.conn[host] != state) {
        s_model.conn[host] = state;
        bump();
    }
    unlock();
}

int herdr_model_get_agents(herdr_agent_t *out, int max)
{
    lock();
    int n = 0;
    for (int h = 0; h < CFG_MAX_HOSTS && n < max; h++) {
        for (int i = 0; i < s_model.agent_count[h] && n < max; i++) {
            out[n++] = s_model.agents[h][i];
        }
    }
    unlock();
    return n;
}

bool herdr_model_get_blocked(herdr_blocked_t *out)
{
    lock();
    bool found = false;
    for (int h = 0; h < CFG_MAX_HOSTS; h++) {
        if (s_model.blocked[h].active) {
            *out = s_model.blocked[h];
            found = true;
            break;
        }
    }
    unlock();
    return found;
}

bool herdr_model_get_pane_content(herdr_pane_content_t *out)
{
    lock();
    bool has = s_model.pane_content.pane_id[0] != '\0';
    if (has) {
        *out = s_model.pane_content;
    }
    unlock();
    return has;
}

herdr_conn_state_t herdr_model_get_conn(int host)
{
    if (!host_ok(host)) {
        return HERDR_CONN_OFFLINE;
    }
    lock();
    herdr_conn_state_t c = s_model.conn[host];
    unlock();
    return c;
}

uint32_t herdr_model_generation(void)
{
    lock();
    uint32_t g = s_model.generation;
    unlock();
    return g;
}
