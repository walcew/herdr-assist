/**
 * @file
 * @brief Nível de poder do avatar Goku: mapeia % de uso semanal em forma.
 *
 * Função pura, sem LVGL/ESP — testável no host (scripts/goku_level_test.c).
 * O driver avatar_goku.c a chama a cada tick para escolher a transformação
 * corrente a partir do uso semanal (7d) do Claude e do modo configurado.
 */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/* Escada de formas, da mais fraca (piso, o "zero") à mais forte. */
typedef enum {
    GOKU_FORM_CRIANCA = 0,   /* Goku criança — piso */
    GOKU_FORM_BASE,
    GOKU_FORM_SSJ,
    GOKU_FORM_SSJ2,
    GOKU_FORM_SSJ3,
    GOKU_FORM_BLUE,          /* SSJ Blue — topo */
    GOKU_FORM_COUNT,
} goku_form_t;

/* Direção escolhida nas Configurações (persistida na NVS). */
typedef enum {
    GOKU_MODE_ASCENDING = 0,   /* mais uso -> mais forte (Criança -> Blue) */
    GOKU_MODE_DESCENDING = 1,  /* mais uso -> mais fraco (Blue -> Criança) */
    GOKU_MODE_COUNT,
} goku_mode_t;

/**
 * Forma para um % de uso semanal (0-100) num dado modo.
 *
 * @param pct        uso semanal 0-100 (valores fora da faixa são clampados).
 * @param mode       GOKU_MODE_ASCENDING ou GOKU_MODE_DESCENDING.
 * @param prev_form  forma corrente, para histerese (evita piscar no limiar);
 *                   passe -1 na primeira leitura (sem histerese).
 * @return           forma 0..GOKU_FORM_COUNT-1.
 */
int goku_form_for_pct(int pct, int mode, int prev_form);

#ifdef __cplusplus
}
#endif
