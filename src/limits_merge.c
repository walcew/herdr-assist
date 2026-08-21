#include "limits_merge.h"

#include <string.h>

/* A janela real mais curta é de 1h; diferença menor que isto entre dois hosts
   é jitter do arredondamento ao minuto que a ponte faz, não virada de janela. */
#define RESET_SKEW_S 300

/* row_count vem capado em 4 pelo parser, mas o módulo é puro e um teste pode
   alimentar lixo — clampar aqui evita ler fora do array. */
static int row_n(const herdr_limits_t *l)
{
    return l->row_count > HERDR_MAX_LIMIT_ROWS ? HERDR_MAX_LIMIT_ROWS
                                               : l->row_count;
}

/* Fim da janela mais distante: é o que denuncia quem já viu o limite zerar. */
static uint32_t last_reset(const herdr_limits_t *l)
{
    uint32_t max = 0;
    for (int i = 0; i < row_n(l); i++) {
        if (l->rows[i].resets_at > max) {
            max = l->rows[i].resets_at;
        }
    }
    return max;
}

static int pct_sum(const herdr_limits_t *l)
{
    int sum = 0;
    for (int i = 0; i < row_n(l); i++) {
        sum += l->rows[i].pct;
    }
    return sum;
}

/* Conta desconhecida nunca casa: mostrar um card a mais é barato, fundir duas
   contas distintas e sumir com o uso de uma delas é caro. O provedor entra na
   chave para "Claude" jamais fundir com "Codex". */
static bool same_account(const herdr_limits_t *a, const herdr_limits_t *b)
{
    return a->account[0] != '\0' &&
           strcmp(a->account, b->account) == 0 &&
           strcmp(a->name, b->name) == 0;
}

/* Ordem total entre duas amostras da mesma conta: true se as linhas de `src`
   devem substituir as de `dst`. As pontes polam a cada 60s de forma
   independente, então elas divergem — sem um critério determinístico o card
   pisca entre dois valores a cada geração. */
static bool src_wins(const herdr_limits_t *dst, const herdr_limits_t *src)
{
    /* amostra !ok são "últimos valores bons" de idade arbitrária: quem está
       coletando ganha sempre */
    if (dst->ok != src->ok) {
        return src->ok;
    }
    /* mais janelas conhecidas ganha, e sem isso os critérios abaixo comparariam
       somas de conjuntos de janelas diferentes */
    if (dst->row_count != src->row_count) {
        return src->row_count > dst->row_count;
    }
    /* virada de janela: quem já repolou depois do reset tem o fim mais à frente
       e ganha na hora, senão o pct velho ficaria de pé por até um ciclo */
    uint32_t rd = last_reset(dst), rs = last_reset(src);
    if (rs > rd && rs - rd > RESET_SKEW_S) {
        return true;
    }
    if (rd > rs && rd - rs > RESET_SKEW_S) {
        return false;
    }
    /* dentro da mesma janela o consumo só cresce: maior soma = leitura mais nova */
    int pd = pct_sum(dst), ps = pct_sum(src);
    if (pd != ps) {
        return ps > pd;
    }
    /* desempate final, para o resultado não depender da posição no array */
    return src->host < dst->host;
}

static uint8_t add_sat(uint8_t a, uint8_t b)
{
    unsigned sum = (unsigned)a + (unsigned)b;
    return sum > 255u ? 255u : (uint8_t)sum;
}

static void absorb(herdr_limits_t *dst, const herdr_limits_t *src)
{
    /* a amostra vencedora entra INTEIRA: unir linha a linha exigiria casar
       rótulos de exibição e poderia passar de HERDR_MAX_LIMIT_ROWS, e a altura
       do card é fórmula fixa — sobraria sobreposição, não reflow */
    if (src_wins(dst, src)) {
        memcpy(dst->rows, src->rows, sizeof(dst->rows));
        dst->row_count = src->row_count;
    }
    /* saúde da coleta é por host: basta um vivo para o card não ser "velho" */
    if (src->ok) {
        dst->ok = true;
    }
    dst->stale_since = dst->ok ? 0u
                     : (src->stale_since > dst->stale_since ? src->stale_since
                                                            : dst->stale_since);
    /* plan e org NÃO seguem o vencedor: o tier detalhado do Claude vem de
       perfil local, as máquinas podem discordar ("Max" vs "Max 20x") e o texto
       trocaria a cada ciclo. Campo-array inteiro no memcpy — strlcpy sobre um
       campo já populado deixaria cauda do valor anterior. */
    if (!dst->plan[0]) {
        memcpy(dst->plan, src->plan, sizeof(dst->plan));
    }
    if (!dst->org[0]) {
        memcpy(dst->org, src->org, sizeof(dst->org));
        dst->corp = src->corp;   /* o par vem do mesmo doador */
    }
    /* contagens de agentes são por host */
    dst->agents = add_sat(dst->agents, src->agents);
    dst->agents_working = add_sat(dst->agents_working, src->agents_working);
    if (src->host != dst->host) {
        dst->shared = true;
        if (src->host < dst->host) {
            dst->host = src->host;
        }
    }
}

int limits_merge_accounts(herdr_limits_t *list, int n)
{
    if (n <= 0) {
        return 0;
    }
    int out = 0;
    for (int i = 0; i < n; i++) {
        int j = 0;
        while (j < out && !same_account(&list[j], &list[i])) {
            j++;
        }
        if (j < out) {
            absorb(&list[j], &list[i]);
        } else {
            if (out != i) {
                list[out] = list[i];
            }
            out++;
        }
    }
    return out;
}
