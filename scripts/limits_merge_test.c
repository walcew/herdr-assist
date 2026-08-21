/* Teste de host de limits_merge_accounts (colapso de cards por conta).
 *
 * Sem LVGL/ESP:  cc -std=c11 -Wall -Wextra -Isrc -Iscripts/hoststub -o /tmp/lmt \
 *                   scripts/limits_merge_test.c src/limits_merge.c && /tmp/lmt
 * Usa contador de falhas (sem assert/abort) para rodar headless. */
#include <stdio.h>
#include <string.h>

#include "../src/limits_merge.h"

static int failures = 0;

#define CHECK(cond, ...)                                    \
    do {                                                    \
        if (!(cond)) {                                      \
            printf("FALHA linha %d: ", __LINE__);           \
            printf(__VA_ARGS__);                            \
            printf("\n");                                   \
            failures++;                                     \
        }                                                   \
    } while (0)

/* Toda origem nasce zerada, como o scratch do parser em herdr_conn.c. */
static herdr_limits_t mk(const char *name, const char *account, uint8_t host,
                         bool ok)
{
    herdr_limits_t l;
    memset(&l, 0, sizeof(l));
    snprintf(l.name, sizeof(l.name), "%s", name);
    snprintf(l.account, sizeof(l.account), "%s", account);
    l.host = host;
    l.ok = ok;
    return l;
}

static void row(herdr_limits_t *l, const char *label, uint8_t pct,
                uint32_t resets_at)
{
    int i = l->row_count++;
    snprintf(l->rows[i].label, sizeof(l->rows[i].label), "%s", label);
    l->rows[i].pct = pct;
    l->rows[i].resets_at = resets_at;
}

#define T0 1755684000u   /* epoch base dos testes */

int main(void)
{
    /* 1. vazio e passagem única */
    {
        CHECK(limits_merge_accounts(NULL, 0) == 0, "n=0 devia devolver 0");

        herdr_limits_t l[1] = {mk("Claude", "a@x.com", 0, true)};
        row(&l[0], "5h", 12, T0);
        herdr_limits_t before = l[0];
        CHECK(limits_merge_accounts(l, 1) == 1, "entrada única vira 1");
        CHECK(memcmp(&before, &l[0], sizeof(before)) == 0,
              "entrada única foi alterada");
    }

    /* 2. mesma conta em 2 hosts: colapsa, host = menor, shared, agents somam */
    {
        herdr_limits_t l[2] = {mk("Claude", "a@x.com", 1, true),
                               mk("Claude", "a@x.com", 0, true)};
        row(&l[0], "5h", 40, T0);
        row(&l[1], "5h", 40, T0);
        l[0].agents = 3; l[0].agents_working = 1;
        l[1].agents = 2; l[1].agents_working = 2;

        CHECK(limits_merge_accounts(l, 2) == 1, "mesma conta devia colapsar");
        CHECK(l[0].host == 0, "host devia ser o menor, veio %u", l[0].host);
        CHECK(l[0].shared, "shared devia estar marcado");
        CHECK(l[0].agents == 5, "agents devia somar 5, veio %u", l[0].agents);
        CHECK(l[0].agents_working == 3, "agents_working devia somar 3, veio %u",
              l[0].agents_working);
    }

    /* 3. contas diferentes do mesmo provedor: não colapsa, ordem preservada */
    {
        herdr_limits_t l[2] = {mk("Claude", "a@x.com", 0, true),
                               mk("Claude", "b@x.com", 1, true)};
        CHECK(limits_merge_accounts(l, 2) == 2, "contas distintas não colapsam");
        CHECK(strcmp(l[0].account, "a@x.com") == 0, "ordem trocou");
        CHECK(!l[0].shared && !l[1].shared, "shared não devia estar marcado");
    }

    /* 4. conta desconhecida nos dois: NUNCA casa (anti-regressão) */
    {
        herdr_limits_t l[2] = {mk("Claude", "", 0, true),
                               mk("Claude", "", 1, true)};
        CHECK(limits_merge_accounts(l, 2) == 2,
              "conta vazia jamais pode fundir com conta vazia");
    }

    /* 5. conta vazia de um lado só */
    {
        herdr_limits_t l[2] = {mk("Claude", "", 0, true),
                               mk("Claude", "a@x.com", 1, true)};
        CHECK(limits_merge_accounts(l, 2) == 2, "vazia não casa com preenchida");
    }

    /* 6. mesmo e-mail em provedores diferentes */
    {
        herdr_limits_t l[2] = {mk("Claude", "a@x.com", 0, true),
                               mk("Codex", "a@x.com", 0, true)};
        CHECK(limits_merge_accounts(l, 2) == 2, "provedores distintos não fundem");
    }

    /* 7. vencedor por ok: quem coleta ganha das linhas velhas */
    {
        herdr_limits_t l[2] = {mk("Claude", "a@x.com", 0, false),
                               mk("Claude", "a@x.com", 1, true)};
        row(&l[0], "5h", 90, T0);
        row(&l[1], "5h", 10, T0);
        l[0].stale_since = T0 - 3600;

        CHECK(limits_merge_accounts(l, 2) == 1, "devia colapsar");
        CHECK(l[0].rows[0].pct == 10, "linhas deviam vir do host ok, veio %u",
              l[0].rows[0].pct);
        CHECK(l[0].ok, "ok devia ser OR");
        CHECK(l[0].stale_since == 0, "stale_since devia zerar quando ok");
    }

    /* 8. todos !ok: stale_since fica com o sucesso mais recente */
    {
        herdr_limits_t l[2] = {mk("Claude", "a@x.com", 0, false),
                               mk("Claude", "a@x.com", 1, false)};
        l[0].stale_since = T0 - 3600;
        l[1].stale_since = T0 - 600;

        CHECK(limits_merge_accounts(l, 2) == 1, "devia colapsar");
        CHECK(!l[0].ok, "ok devia continuar falso");
        CHECK(l[0].stale_since == T0 - 600,
              "stale_since devia ser o MAIOR, veio %u", l[0].stale_since);
    }

    /* 9. vencedor por row_count: mais janelas conhecidas ganha */
    {
        herdr_limits_t l[2] = {mk("Claude", "a@x.com", 0, true),
                               mk("Claude", "a@x.com", 1, true)};
        row(&l[0], "5h", 50, T0);
        row(&l[1], "5h", 20, T0);
        row(&l[1], "7d", 30, T0 + 604800);

        CHECK(limits_merge_accounts(l, 2) == 1, "devia colapsar");
        CHECK(l[0].row_count == 2, "devia ficar com 2 janelas, veio %u",
              l[0].row_count);
        CHECK(l[0].rows[0].pct == 20, "linhas deviam vir da amostra de 2 janelas");
    }

    /* 10. virada de janela: 92% velho perde para 3% já resetado (anti-piscada) */
    {
        herdr_limits_t l[2] = {mk("Claude", "a@x.com", 0, true),
                               mk("Claude", "a@x.com", 1, true)};
        row(&l[0], "5h", 92, T0);
        row(&l[1], "5h", 3, T0 + 18000);

        CHECK(limits_merge_accounts(l, 2) == 1, "devia colapsar");
        CHECK(l[0].rows[0].pct == 3,
              "amostra pós-reset devia ganhar, veio %u", l[0].rows[0].pct);
    }

    /* 11. dentro do skew vence o maior pct — e a ordem do array não importa */
    {
        herdr_limits_t a[2] = {mk("Claude", "a@x.com", 0, true),
                               mk("Claude", "a@x.com", 1, true)};
        row(&a[0], "5h", 41, T0);
        row(&a[1], "5h", 44, T0 + 60);   /* 60s < RESET_SKEW_S: é jitter */
        herdr_limits_t b[2] = {a[1], a[0]};

        CHECK(limits_merge_accounts(a, 2) == 1, "devia colapsar");
        CHECK(limits_merge_accounts(b, 2) == 1, "devia colapsar (invertido)");
        CHECK(a[0].rows[0].pct == 44, "maior pct devia ganhar, veio %u",
              a[0].rows[0].pct);
        CHECK(b[0].rows[0].pct == 44,
              "resultado devia independer da ordem, veio %u", b[0].rows[0].pct);
        CHECK(a[0].host == 0 && b[0].host == 0, "host devia ser o menor nos dois");
    }

    /* 12. ordem da primeira ocorrência, mesmo quando o vencedor muda */
    {
        herdr_limits_t l[4] = {mk("Claude", "a@x.com", 0, true),
                               mk("Codex", "b@x.com", 0, true),
                               mk("Claude", "a@x.com", 1, true),
                               mk("Claude", "c@x.com", 1, true)};
        row(&l[0], "5h", 10, T0);
        row(&l[2], "5h", 80, T0);   /* host 1 vence por pct */

        CHECK(limits_merge_accounts(l, 4) == 3, "devia sobrar 3 cards");
        CHECK(strcmp(l[0].account, "a@x.com") == 0 &&
              strcmp(l[0].name, "Claude") == 0, "A devia continuar na posição 0");
        CHECK(strcmp(l[1].account, "b@x.com") == 0, "B devia continuar na posição 1");
        CHECK(strcmp(l[2].account, "c@x.com") == 0, "C devia continuar na posição 2");
        CHECK(l[0].rows[0].pct == 80, "A devia ter as linhas do vencedor");
    }

    /* 13. agents é uint8_t: soma satura em vez de dar a volta */
    {
        herdr_limits_t l[2] = {mk("Claude", "a@x.com", 0, true),
                               mk("Claude", "a@x.com", 1, true)};
        l[0].agents = 200; l[1].agents = 200;
        CHECK(limits_merge_accounts(l, 2) == 1, "devia colapsar");
        CHECK(l[0].agents == 255, "devia saturar em 255, veio %u", l[0].agents);
    }

    /* 14. três hosts na mesma conta */
    {
        herdr_limits_t l[3] = {mk("Claude", "a@x.com", 2, true),
                               mk("Claude", "a@x.com", 0, true),
                               mk("Claude", "a@x.com", 1, true)};
        l[0].agents = 1; l[1].agents = 2; l[2].agents = 4;
        CHECK(limits_merge_accounts(l, 3) == 1, "3 hosts viram 1 card");
        CHECK(l[0].agents == 7, "agents devia somar 7, veio %u", l[0].agents);
        CHECK(l[0].host == 0, "host devia ser o menor, veio %u", l[0].host);
        CHECK(l[0].shared, "shared devia estar marcado");
    }

    /* 15. plan/org vêm do primeiro doador não-vazio, e nenhum byte vaza:
           o struto inteiro tem de bater com o esperado montado do zero */
    {
        herdr_limits_t l[2] = {mk("Claude", "a@x.com", 0, true),
                               mk("Claude", "a@x.com", 1, true)};
        row(&l[0], "5h", 40, T0);
        row(&l[1], "5h", 40, T0);
        snprintf(l[1].plan, sizeof(l[1].plan), "%s", "Max 20x");
        snprintf(l[1].org, sizeof(l[1].org), "%s", "x");
        l[1].corp = true;

        CHECK(limits_merge_accounts(l, 2) == 1, "devia colapsar");

        herdr_limits_t want;
        memset(&want, 0, sizeof(want));
        snprintf(want.name, sizeof(want.name), "%s", "Claude");
        snprintf(want.account, sizeof(want.account), "%s", "a@x.com");
        snprintf(want.plan, sizeof(want.plan), "%s", "Max 20x");
        snprintf(want.org, sizeof(want.org), "%s", "x");
        want.corp = true;
        want.shared = true;
        want.ok = true;
        want.host = 0;
        row(&want, "5h", 40, T0);

        CHECK(memcmp(&want, &l[0], sizeof(want)) == 0,
              "card mesclado difere byte a byte do esperado");
    }

    /* 16. idempotente: rodar de novo não soma as contagens outra vez */
    {
        herdr_limits_t l[3] = {mk("Claude", "a@x.com", 0, true),
                               mk("Claude", "a@x.com", 1, true),
                               mk("Codex", "b@x.com", 0, true)};
        l[0].agents = 2; l[1].agents = 3;

        int n1 = limits_merge_accounts(l, 3);
        herdr_limits_t snap[3];
        memcpy(snap, l, sizeof(snap));
        int n2 = limits_merge_accounts(l, n1);

        CHECK(n1 == 2 && n2 == 2, "contagem devia ser estável (%d, %d)", n1, n2);
        CHECK(memcmp(snap, l, (size_t)n1 * sizeof(l[0])) == 0,
              "segunda passada alterou o resultado");
        CHECK(l[0].agents == 5, "agents somou de novo: %u", l[0].agents);
    }

    if (failures == 0) {
        printf("limits_merge_test: OK (sizeof(herdr_limits_t) = %zu)\n",
               sizeof(herdr_limits_t));
    }
    return failures != 0;
}
