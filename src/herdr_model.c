#include "herdr_model.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <esp_heap_caps.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "activity_log.h"
#include "limits_merge.h"

static struct {
    herdr_agent_t agents[CFG_MAX_HOSTS][HERDR_MAX_AGENTS];
    int agent_count[CFG_MAX_HOSTS];
    struct {
        char     pane_id[HERDR_ID_LEN];
        uint8_t  host;
        char    *content;   /* HERDR_CONTENT_LEN, alocado 1x em PSRAM no init */
        uint32_t seq;       /* incrementa a cada conteúdo diferente */
    } pane_content;
    herdr_conn_state_t conn[CFG_MAX_HOSTS];
    herdr_limits_t limits[CFG_MAX_HOSTS][HERDR_MAX_PROVIDERS];
    int limit_count[CFG_MAX_HOSTS];
    herdr_cost_t cost[CFG_MAX_HOSTS];
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
    activity_log_init();
    /* grande e sem pressa: PSRAM, poupando a SRAM interna */
    s_model.pane_content.content = heap_caps_malloc(HERDR_CONTENT_LEN, MALLOC_CAP_SPIRAM);
    if (!s_model.pane_content.content) {
        s_model.pane_content.content = malloc(HERDR_CONTENT_LEN);
    }
    if (s_model.pane_content.content) {
        s_model.pane_content.content[0] = '\0';
    }
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

    /* Log de atividade: diff por pane_id contra o snapshot anterior (antes de
       sobrescrevê-lo) para registrar começou/bloqueou/terminou. */
    {
        uint32_t now = (uint32_t)time(NULL);
        const herdr_agent_t *old = s_model.agents[host];
        int old_count = s_model.agent_count[host];
        for (int i = 0; i < count; i++) {
            const char *os = "";
            uint32_t osince = 0;
            for (int j = 0; j < old_count; j++) {
                if (strcmp(old[j].pane_id, agents[i].pane_id) == 0) {
                    os = old[j].status;
                    osince = old[j].since;
                    break;
                }
            }
            activity_type_t t = activity_classify(os, agents[i].status);
            if (t != ACT_NONE) {
                uint32_t dur = (t == ACT_DONE && osince && now > osince) ? now - osince : 0;
                activity_log_note(t, (uint8_t)host, agents[i].agent,
                                  agents[i].project, dur, now);
            }
        }
        /* agentes que sumiram do snapshot: se estavam ativos, "terminou" */
        for (int j = 0; j < old_count; j++) {
            bool present = false;
            for (int i = 0; i < count; i++) {
                if (strcmp(old[j].pane_id, agents[i].pane_id) == 0) {
                    present = true;
                    break;
                }
            }
            if (present) {
                continue;
            }
            activity_type_t t = activity_classify(old[j].status, "");
            if (t != ACT_NONE) {
                uint32_t dur = (old[j].since && now > old[j].since) ? now - old[j].since : 0;
                activity_log_note(t, (uint8_t)host, old[j].agent, old[j].project, dur, now);
            }
        }
    }

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
    if (changed) {
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
 * Trocas de codepoint que a JetBrainsMono Nerd não cobre por vizinhos visuais
 * que ela tem (auditado contra o cmap do .c gerado — ver gen_font.sh). Tudo
 * 1 coluna -> 1 coluna e mesmo tamanho em bytes ou menor, para a troca in-place.
 * Os quatro asteriscos do spinner do Claude Code colapsam no ✶, que é o único
 * frame que a fonte tem. Entradas com "" removem o caractere (variation
 * selectors: invisíveis no host, mas contariam coluna e virariam tofu aqui).
 */
static const struct { const char *from; const char *to; } GLYPH_SWAPS[] = {
    { "\xE2\x8E\xBF", "\xE2\x94\x94" },  /* ⎿ -> └ */
    { "\xE2\x8F\xBA", "\xE2\x97\x8F" },  /* ⏺ -> ● */
    { "\xE2\x8F\xB5", "\xE2\x96\xB6" },  /* ⏵ -> ▶ */
    { "\xE2\x8F\xB8", "\xE2\x80\x96" },  /* ⏸ -> ‖ */
    { "\xE2\x8F\xB3", "\xE2\x97\x8B" },  /* ⏳ -> ○ */
    { "\xE2\x97\x91", "\xE2\x97\x95" },  /* ◑ -> ◕ */
    { "\xE2\x97\xBC", "\xE2\x96\xA0" },  /* ◼ -> ■ */
    { "\xE2\x9C\xA2", "\xE2\x9C\xB6" },  /* ✢ -> ✶ */
    { "\xE2\x9C\xB3", "\xE2\x9C\xB6" },  /* ✳ -> ✶ */
    { "\xE2\x9C\xBB", "\xE2\x9C\xB6" },  /* ✻ -> ✶ */
    { "\xE2\x9C\xBD", "\xE2\x9C\xB6" },  /* ✽ -> ✶ */
    { "\xE2\x9C\x94", "\xE2\x9C\x93" },  /* ✔ -> ✓ */
    { "\xE2\x9C\x85", "\xE2\x9C\x93" },  /* ✅ -> ✓ */
    { "\xE2\xA7\x89", "\xE2\x8A\x9E" },  /* ⧉ -> ⊞ */
    { "\xE2\x80\xBB", "*" },             /* ※ -> * */
    { "\xEF\xB8\x8E", "" },              /* VS15 (U+FE0E) some */
    { "\xEF\xB8\x8F", "" },              /* VS16 (U+FE0F) some */
};

/**
 * Substitui o que a fonte do terminal não desenha, evitando retângulos vazios.
 *
 * A fonte traz a JetBrainsMono Nerd inteira, então sobra pouco: a tabela
 * GLYPH_SWAPS acima e os emojis, que nenhuma fonte monoespaçada cobre.
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
        if (r[0] == 0xF0 && r[1] == 0x9F && r[2] && r[3]) {
            *w++ = '*';             /* emoji ocupa 2 células no host: 2 chars */
            *w++ = ' ';             /* preservam o alinhamento das colunas */
            r += 4;
            continue;
        }
        size_t i;
        for (i = 0; i < sizeof(GLYPH_SWAPS) / sizeof(GLYPH_SWAPS[0]); i++) {
            size_t flen = strlen(GLYPH_SWAPS[i].from);
            /* strncmp para no NUL: nunca lê além do fim da string */
            if (strncmp((const char *)r, GLYPH_SWAPS[i].from, flen) == 0) {
                size_t tlen = strlen(GLYPH_SWAPS[i].to);
                memcpy(w, GLYPH_SWAPS[i].to, tlen);
                w += tlen;
                r += flen;
                break;
            }
        }
        if (i == sizeof(GLYPH_SWAPS) / sizeof(GLYPH_SWAPS[0])) {
            *w++ = *r++;
        }
    }
    *w = '\0';
}

void herdr_model_set_pane_content(int host, const char *pane_id, char *content)
{
    if (!host_ok(host) || !s_model.pane_content.content) {
        return;
    }
    /* in-place e fora do lock: só encolhe, e 0x1B (escapes SGR) é ASCII,
       intocado pelas trocas de glifo */
    replace_missing_glyphs(content);
    lock();
    /* a ponte reenvia o mesmo snapshot a cada polling; só bumpa se mudou —
       com full_refresh=1, cada repintura custa um quadro inteiro no QSPI */
    bool changed = s_model.pane_content.host != (uint8_t)host ||
                   strcmp(s_model.pane_content.pane_id, pane_id) != 0 ||
                   strcmp(s_model.pane_content.content, content) != 0;
    if (changed) {
        s_model.pane_content.host = (uint8_t)host;
        strlcpy(s_model.pane_content.pane_id, pane_id, HERDR_ID_LEN);
        copy_utf8_safe(s_model.pane_content.content, content, HERDR_CONTENT_LEN);
        s_model.pane_content.seq++;
        bump();
    }
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

void herdr_model_set_limits(int host, const herdr_limits_t *limits, int count)
{
    if (!host_ok(host)) {
        return;
    }
    if (count > HERDR_MAX_PROVIDERS) {
        count = HERDR_MAX_PROVIDERS;
    }
    lock();
    /* mesmo contrato de set_agents: a ponte deduplica, mas um snapshot idêntico
       ainda pode chegar (reconexão) — só bumpa se mudou, com o host carimbado
       antes da comparação. */
    bool changed = (count != s_model.limit_count[host]);
    for (int i = 0; i < count && !changed; i++) {
        herdr_limits_t tmp = limits[i];
        tmp.host = (uint8_t)host;
        changed = memcmp(&s_model.limits[host][i], &tmp, sizeof(tmp)) != 0;
    }
    memcpy(s_model.limits[host], limits, count * sizeof(herdr_limits_t));
    s_model.limit_count[host] = count;
    for (int i = 0; i < count; i++) {
        s_model.limits[host][i].host = (uint8_t)host;
    }
    if (changed) {
        bump();
    }
    unlock();
}

void herdr_model_set_cost(int host, const herdr_cost_t *cost)
{
    if (!host_ok(host)) {
        return;
    }
    lock();
    s_model.cost[host] = *cost;
    s_model.cost[host].valid = true;
    bump();
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

int herdr_model_get_limits(herdr_limits_t *out, int max)
{
    lock();
    int n = 0;
    for (int h = 0; h < CFG_MAX_HOSTS && n < max; h++) {
        for (int i = 0; i < s_model.limit_count[h] && n < max; i++) {
            out[n++] = s_model.limits[h][i];
        }
    }
    unlock();
    /* Uma conta é a mesma conta em qualquer máquina: duas pontes logadas no
       mesmo login mandam o mesmo teto de uso. Fora do lock de propósito — daqui
       para baixo só se mexe na cópia do chamador. */
    return limits_merge_accounts(out, n);
}

/* Soma saturando: o total de várias máquinas não pode dar a volta no uint32. */
static uint32_t cents_add(uint32_t a, uint32_t b)
{
    return (a > UINT32_MAX - b) ? UINT32_MAX : a + b;
}

bool herdr_model_get_cost(herdr_cost_t *out)
{
    memset(out, 0, sizeof(*out));
    lock();
    /* O custo sai de transcripts LOCAIS de cada máquina, então cada ponte
       reporta um pedaço do gasto e o total é a soma. Máquina desligada segue
       contando com o último valor conhecido: o gasto dela aconteceu. */
    const herdr_cost_t *legacy = NULL;
    for (int h = 0; h < CFG_MAX_HOSTS; h++) {
        const herdr_cost_t *c = &s_model.cost[h];
        if (!c->valid) {
            continue;
        }
        out->valid = true;
        if (c->has_cents) {
            out->has_cents = true;
            out->now_cents = cents_add(out->now_cents, c->now_cents);
            out->week_cents = cents_add(out->week_cents, c->week_cents);
            out->life_cents = cents_add(out->life_cents, c->life_cents);
        } else if (!legacy) {
            legacy = c;   /* ponte antiga: strings prontas, que não somam */
        }
    }
    /* Só vale o caminho legado quando NENHUMA ponte mandou centavos. Ponte
       antiga misturada com nova subnotifica até ser atualizada — as strings não
       são somáveis, e exibir a de uma máquina só também seria parcial. */
    if (legacy && !out->has_cents) {
        memcpy(out->now, legacy->now, sizeof(out->now));
        memcpy(out->week, legacy->week, sizeof(out->week));
        memcpy(out->life, legacy->life, sizeof(out->life));
    }
    bool valid = out->valid;
    unlock();
    return valid;
}

bool herdr_model_get_pane_content(char *pane_id, size_t id_size, int *host,
                                  char *content, size_t content_size,
                                  uint32_t *seq_inout)
{
    if (!s_model.pane_content.content) {
        return false;
    }
    lock();
    bool fresh = s_model.pane_content.pane_id[0] != '\0' &&
                 s_model.pane_content.seq != *seq_inout;
    if (fresh) {
        strlcpy(pane_id, s_model.pane_content.pane_id, id_size);
        *host = s_model.pane_content.host;
        copy_utf8_safe(content, s_model.pane_content.content, content_size);
        *seq_inout = s_model.pane_content.seq;
    }
    unlock();
    return fresh;
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
