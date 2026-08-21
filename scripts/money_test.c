/* Teste de host de money_fmt (formatação de US$ por idioma).
 *
 * Sem LVGL/ESP:  cc -std=c11 -Wall -Wextra -Isrc -o /tmp/mt \
 *                   scripts/money_test.c src/money.c && /tmp/mt
 * Usa contador de falhas (sem assert/abort) para rodar headless. */
#include <stdio.h>
#include <string.h>

#include "../src/money.h"

static int failures = 0;

#define CHECK_STR(cents, br, expected)                                      \
    do {                                                                    \
        char _b[16];                                                        \
        money_fmt(_b, sizeof(_b), (cents), (br));                           \
        if (strcmp(_b, (expected)) != 0) {                                  \
            printf("FALHA linha %d: %u cents %s -> \"%s\", esperado \"%s\"\n", \
                   __LINE__, (unsigned)(cents), (br) ? "pt" : "en",         \
                   _b, (expected));                                         \
            failures++;                                                     \
        }                                                                   \
    } while (0)

int main(void)
{
    /* pt-BR: vírgula decimal, ponto de milhar, símbolo separado */
    CHECK_STR(0, true, "US$ 0,00");
    CHECK_STR(5, true, "US$ 0,05");
    CHECK_STR(420, true, "US$ 4,20");
    CHECK_STR(9999, true, "US$ 99,99");

    /* en-US: ponto decimal, vírgula de milhar, símbolo colado */
    CHECK_STR(0, false, "$0.00");
    CHECK_STR(420, false, "$4.20");
    CHECK_STR(9999, false, "$99.99");

    /* de US$ 100 para cima os centavos somem, nos dois idiomas */
    CHECK_STR(10000, true, "US$ 100");
    CHECK_STR(10000, false, "$100");
    CHECK_STR(10099, true, "US$ 100");

    /* separador de milhar entra a partir de 1.000 */
    CHECK_STR(124000, true, "US$ 1.240");
    CHECK_STR(124000, false, "$1,240");
    CHECK_STR(123456789, true, "US$ 1.234.567");
    CHECK_STR(123456789, false, "$1,234,567");

    /* buffer curto não pode vazar: devolve string vazia e terminada */
    char small[4];
    money_fmt(small, sizeof(small), 124000, true);
    if (small[0] != '\0') {
        printf("FALHA: buffer curto devia virar \"\", veio \"%s\"\n", small);
        failures++;
    }

    if (failures) {
        printf("money: %d teste(s) falharam\n", failures);
        return 1;
    }
    printf("money: OK (todos os testes passaram)\n");
    return 0;
}
