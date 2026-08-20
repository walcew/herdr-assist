/* Teste de host da função pura goku_form_for_pct (nível de poder do Goku).
 *
 * Sem LVGL/ESP. No CI seria cc -std=c11; localmente no Windows, MSVC.
 * Usa um contador de falhas (sem assert/abort) para rodar headless. */
#include <stdio.h>

#include "../src/goku_level.h"

static int failures = 0;

#define CHECK_EQ(expr, expected)                                       \
    do {                                                               \
        int _got = (expr);                                             \
        int _exp = (expected);                                         \
        if (_got != _exp) {                                            \
            printf("FALHOU: %s => %d (esperado %d)\n", #expr, _got, _exp); \
            failures++;                                                \
        }                                                              \
    } while (0)

int main(void)
{
    /* --- Ascendente, primeira leitura (prev = -1, sem histerese) --- */
    CHECK_EQ(goku_form_for_pct(0,   GOKU_MODE_ASCENDING, -1), GOKU_FORM_CRIANCA);
    CHECK_EQ(goku_form_for_pct(10,  GOKU_MODE_ASCENDING, -1), GOKU_FORM_CRIANCA);
    CHECK_EQ(goku_form_for_pct(20,  GOKU_MODE_ASCENDING, -1), GOKU_FORM_BASE);
    CHECK_EQ(goku_form_for_pct(50,  GOKU_MODE_ASCENDING, -1), GOKU_FORM_SSJ2);
    CHECK_EQ(goku_form_for_pct(83,  GOKU_MODE_ASCENDING, -1), GOKU_FORM_SSJ3);
    CHECK_EQ(goku_form_for_pct(90,  GOKU_MODE_ASCENDING, -1), GOKU_FORM_BLUE);
    CHECK_EQ(goku_form_for_pct(100, GOKU_MODE_ASCENDING, -1), GOKU_FORM_BLUE);

    /* --- Descendente inverte: 0% uso = Blue, 100% uso = Criança --- */
    CHECK_EQ(goku_form_for_pct(0,   GOKU_MODE_DESCENDING, -1), GOKU_FORM_BLUE);
    CHECK_EQ(goku_form_for_pct(100, GOKU_MODE_DESCENDING, -1), GOKU_FORM_CRIANCA);
    CHECK_EQ(goku_form_for_pct(50,  GOKU_MODE_DESCENDING, -1), GOKU_FORM_SSJ2);
    CHECK_EQ(goku_form_for_pct(20,  GOKU_MODE_DESCENDING, -1), GOKU_FORM_SSJ3);

    /* --- Clamp de entrada fora de faixa --- */
    CHECK_EQ(goku_form_for_pct(-5,  GOKU_MODE_ASCENDING, -1), GOKU_FORM_CRIANCA);
    CHECK_EQ(goku_form_for_pct(150, GOKU_MODE_ASCENDING, -1), GOKU_FORM_BLUE);

    /* --- Histerese perto do limiar BASE<->SSJ (33%): precisa de +/-2 --- */
    CHECK_EQ(goku_form_for_pct(34, GOKU_MODE_ASCENDING, GOKU_FORM_BASE), GOKU_FORM_BASE);    /* nao sobe ainda */
    CHECK_EQ(goku_form_for_pct(35, GOKU_MODE_ASCENDING, GOKU_FORM_BASE), GOKU_FORM_SSJ);     /* sobe */
    CHECK_EQ(goku_form_for_pct(15, GOKU_MODE_ASCENDING, GOKU_FORM_BASE), GOKU_FORM_BASE);    /* nao desce ainda (16-2=14) */
    CHECK_EQ(goku_form_for_pct(13, GOKU_MODE_ASCENDING, GOKU_FORM_BASE), GOKU_FORM_CRIANCA); /* desce */

    if (failures) {
        printf("goku_level: %d FALHA(S)\n", failures);
        return 1;
    }
    printf("goku_level: OK (todos os testes passaram)\n");
    return 0;
}
