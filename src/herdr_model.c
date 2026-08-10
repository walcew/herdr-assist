#include "herdr_model.h"

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static struct {
    herdr_agent_t agents[HERDR_MAX_AGENTS];
    int agent_count;
    herdr_blocked_t blocked;
    herdr_pane_content_t pane_content;
    herdr_conn_state_t conn;
    uint32_t generation;
    SemaphoreHandle_t mutex;
} s_model;

static void lock(void)   { xSemaphoreTake(s_model.mutex, portMAX_DELAY); }
static void unlock(void) { xSemaphoreGive(s_model.mutex); }
static void bump(void)   { s_model.generation++; }

void herdr_model_init(void)
{
    memset(&s_model, 0, sizeof(s_model));
    s_model.mutex = xSemaphoreCreateMutex();
}

void herdr_model_set_agents(const herdr_agent_t *agents, int count)
{
    if (count > HERDR_MAX_AGENTS) {
        count = HERDR_MAX_AGENTS;
    }
    lock();
    /* relay retransmite a cada 2s mesmo sem mudança; só bumpa se mudou */
    bool changed = (count != s_model.agent_count) ||
                   (memcmp(s_model.agents, agents, count * sizeof(herdr_agent_t)) != 0);
    memcpy(s_model.agents, agents, count * sizeof(herdr_agent_t));
    s_model.agent_count = count;
    /* blocked deixa de valer se o pane sumiu ou se o agente destravou */
    if (s_model.blocked.active) {
        bool still_blocked = false;
        for (int i = 0; i < count; i++) {
            if (strcmp(s_model.agents[i].pane_id, s_model.blocked.pane_id) == 0) {
                still_blocked = strcmp(s_model.agents[i].status, "blocked") == 0;
                break;
            }
        }
        if (!still_blocked) {
            s_model.blocked.active = false;
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
    lock();
    s_model.blocked = *blocked;
    s_model.blocked.active = true;
    bump();
    unlock();
}

void herdr_model_clear_blocked(const char *pane_id)
{
    lock();
    if (s_model.blocked.active && strcmp(s_model.blocked.pane_id, pane_id) == 0) {
        s_model.blocked.active = false;
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
 * Troca por '*' o U+2733 (✳), que o Claude usa nos títulos mas a JetBrainsMono
 * Nerd não tem — sem isso vira retângulo vazio na tela. Substituição in-place:
 * E2 9C B3 (3 bytes) vira '*' e o resto da string desliza para trás.
 */
static void replace_missing_glyphs(char *s)
{
    char *w = s;
    for (const char *r = s; *r; ) {
        if ((unsigned char)r[0] == 0xE2 && (unsigned char)r[1] == 0x9C &&
            (unsigned char)r[2] == 0xB3) {
            *w++ = '*';
            r += 3;
        } else {
            *w++ = *r++;
        }
    }
    *w = '\0';
}

void herdr_model_set_pane_content(const char *pane_id, const char *content)
{
    lock();
    strlcpy(s_model.pane_content.pane_id, pane_id, HERDR_ID_LEN);
    copy_utf8_safe(s_model.pane_content.content, content, HERDR_CONTENT_LEN);
    replace_missing_glyphs(s_model.pane_content.content);
    bump();
    unlock();
}

void herdr_model_set_conn(herdr_conn_state_t state)
{
    lock();
    s_model.conn = state;
    bump();
    unlock();
}

int herdr_model_get_agents(herdr_agent_t *out, int max)
{
    lock();
    int n = s_model.agent_count < max ? s_model.agent_count : max;
    memcpy(out, s_model.agents, n * sizeof(herdr_agent_t));
    unlock();
    return n;
}

bool herdr_model_get_blocked(herdr_blocked_t *out)
{
    lock();
    bool active = s_model.blocked.active;
    if (active) {
        *out = s_model.blocked;
    }
    unlock();
    return active;
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

herdr_conn_state_t herdr_model_get_conn(void)
{
    lock();
    herdr_conn_state_t c = s_model.conn;
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
