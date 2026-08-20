#include "goku_level.h"

/* Margem (em pontos percentuais) para trocar de forma perto do limiar: evita
   piscar quando o uso oscila em torno da fronteira entre duas formas. */
#define GOKU_HYSTERESIS 2

/* Topo (exclusivo) da forma f, em % do "valor": (f+1)*100/COUNT.
   O piso da forma f é band_top(f-1). Divisão inteira: para COUNT=6 dá os
   limiares 16, 33, 50, 66, 83. */
static int band_top(int f)
{
    return (f + 1) * 100 / GOKU_FORM_COUNT;
}

int goku_form_for_pct(int pct, int mode, int prev_form)
{
    if (pct < 0) {
        pct = 0;
    } else if (pct > 100) {
        pct = 100;
    }

    /* Descendente inverte: quanto mais uso, menos "valor" -> forma mais fraca. */
    int value = (mode == GOKU_MODE_DESCENDING) ? (100 - pct) : pct;

    int nominal = value * GOKU_FORM_COUNT / 100;
    if (nominal > GOKU_FORM_COUNT - 1) {
        nominal = GOKU_FORM_COUNT - 1;
    }

    /* Primeira leitura (ou prev inválido): sem histerese. */
    if (prev_form < 0 || prev_form >= GOKU_FORM_COUNT) {
        return nominal;
    }

    int f = prev_form;
    /* Sobe enquanto ultrapassar o topo da forma atual mais a margem. */
    while (f < GOKU_FORM_COUNT - 1 && value >= band_top(f) + GOKU_HYSTERESIS) {
        f++;
    }
    /* Desce enquanto cair abaixo do piso da forma atual menos a margem. */
    while (f > 0 && value < band_top(f - 1) - GOKU_HYSTERESIS) {
        f--;
    }
    return f;
}
