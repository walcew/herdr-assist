#include "money.h"

#include <stdio.h>
#include <string.h>

/* Escreve a parte inteira com separador de milhar, do fim para o começo.
   Devolve quantos bytes usou (sem o NUL). */
static size_t group(char *out, size_t size, uint32_t units, char sep)
{
    char raw[12];
    int n = snprintf(raw, sizeof(raw), "%lu", (unsigned long)units);
    if (n <= 0) {
        return 0;
    }
    size_t need = (size_t)n + (size_t)((n - 1) / 3);
    if (need + 1 > size) {
        return 0;
    }
    size_t w = 0;
    for (int i = 0; i < n; i++) {
        if (i && (n - i) % 3 == 0) {
            out[w++] = sep;
        }
        out[w++] = raw[i];
    }
    out[w] = '\0';
    return w;
}

void money_fmt(char *buf, size_t size, uint32_t cents, bool br)
{
    if (!buf || size == 0) {
        return;
    }
    buf[0] = '\0';

    char intpart[16];
    size_t n = group(intpart, sizeof(intpart), cents / 100u, br ? '.' : ',');
    if (n == 0) {
        return;
    }

    /* de US$ 100 para cima, sem centavos */
    int w;
    if (cents >= 10000u) {
        w = snprintf(buf, size, br ? "US$ %s" : "$%s", intpart);
    } else {
        w = snprintf(buf, size, br ? "US$ %s,%02u" : "$%s.%02u",
                     intpart, (unsigned)(cents % 100u));
    }
    /* truncado é pior que ausente: "US$ 1.2" mente sobre o valor */
    if (w < 0 || (size_t)w >= size) {
        buf[0] = '\0';
    }
}
