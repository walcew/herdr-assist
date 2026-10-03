/* Teste de host do render da fita de LED (estado + tempo -> 7 cores).
 *
 * Sem LVGL/ESP:  cc -std=c11 -Wall -Wextra -Isrc -o /tmp/lft \
 *                   scripts/led_fx_test.c src/led_fx.c && /tmp/lft
 * Usa contador de falhas (sem assert/abort) para rodar headless. */
#include <stdio.h>

#include "../src/led_fx.h"

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

static led_rgb_t px[LED_FX_COUNT];

static void render(led_fx_state_t st, uint32_t t)
{
    led_fx_render(st, t, px);
}

static int lit(const led_rgb_t *p)
{
    return p->r || p->g || p->b;
}

static int count_lit(void)
{
    int n = 0;
    for (int i = 0; i < LED_FX_COUNT; i++) {
        n += lit(&px[i]);
    }
    return n;
}

static int brightest(void)
{
    int best = 0, sum = -1;
    for (int i = 0; i < LED_FX_COUNT; i++) {
        int s = px[i].r + px[i].g + px[i].b;
        if (s > sum) {
            sum = s;
            best = i;
        }
    }
    return best;
}

int main(void)
{
    static const led_fx_state_t all[] = {
        LED_FX_DISCONNECTED, LED_FX_IDLE, LED_FX_DONE, LED_FX_WORKING, LED_FX_BLOCKED,
    };

    /* 1. teto de brilho: nenhum canal passa de LED_FX_MAX em nenhum instante */
    for (unsigned s = 0; s < sizeof(all) / sizeof(all[0]); s++) {
        for (uint32_t t = 0; t < 12000; t += 7) {
            render(all[s], t);
            for (int i = 0; i < LED_FX_COUNT; i++) {
                CHECK(px[i].r <= LED_FX_MAX && px[i].g <= LED_FX_MAX && px[i].b <= LED_FX_MAX,
                      "estado %d t=%u led %d passou do teto", all[s], t, i);
            }
        }
    }

    /* 2. periodicidade: t e t + período dão a mesma saída */
    {
        static const struct { led_fx_state_t st; uint32_t period; } per[] = {
            {LED_FX_DISCONNECTED, 3000}, {LED_FX_IDLE, 6000}, {LED_FX_DONE, 2000},
            {LED_FX_WORKING, 1000},      {LED_FX_BLOCKED, 1500},
        };
        for (unsigned s = 0; s < sizeof(per) / sizeof(per[0]); s++) {
            for (uint32_t t = 0; t < per[s].period; t += 37) {
                led_rgb_t a[LED_FX_COUNT];
                led_fx_render(per[s].st, t, a);
                render(per[s].st, t + per[s].period * 5);
                for (int i = 0; i < LED_FX_COUNT; i++) {
                    CHECK(a[i].r == px[i].r && a[i].g == px[i].g && a[i].b == px[i].b,
                          "estado %d não repete no período (t=%u led %d)", per[s].st, t, i);
                }
            }
        }
    }

    /* 3. BLOCKED: fita inteira vermelha no pulso, apagada na pausa */
    {
        render(LED_FX_BLOCKED, 100);
        CHECK(count_lit() == LED_FX_COUNT, "BLOCKED no 1º pulso devia acender os 7");
        for (int i = 0; i < LED_FX_COUNT; i++) {
            CHECK(px[i].r > 0 && px[i].g == 0 && px[i].b == 0, "BLOCKED devia ser vermelho puro");
        }
        render(LED_FX_BLOCKED, 450);
        CHECK(count_lit() == LED_FX_COUNT, "BLOCKED no 2º pulso devia acender os 7");
        render(LED_FX_BLOCKED, 275);
        CHECK(count_lit() == 0, "BLOCKED entre os pulsos devia apagar");
        render(LED_FX_BLOCKED, 1100);
        CHECK(count_lit() == 0, "BLOCKED na pausa devia apagar");
    }

    /* 4. DISCONNECTED: no máximo um LED aceso, branco, e a maior parte do tempo apagado */
    {
        int on = 0;
        for (uint32_t t = 0; t < 3000; t += 10) {
            render(LED_FX_DISCONNECTED, t);
            CHECK(count_lit() <= 1, "DISCONNECTED acendeu mais de um LED (t=%u)", t);
            if (count_lit()) {
                on++;
                CHECK(px[0].r == px[0].g && px[0].g == px[0].b, "DISCONNECTED devia ser branco");
            }
        }
        CHECK(on > 0, "DISCONNECTED nunca piscou");
        CHECK(on < 30, "DISCONNECTED devia ficar apagado a maior parte do ciclo");
    }

    /* 5. WORKING: cometa âmbar com a cabeça avançando no tempo */
    {
        int prev = -1;
        for (uint32_t k = 0; k < LED_FX_COUNT; k++) {
            uint32_t t = k * 1000 / LED_FX_COUNT;
            render(LED_FX_WORKING, t + 10);
            int h = brightest();
            CHECK(h > prev, "WORKING: cabeça não avançou (t=%u: %d após %d)", t, h, prev);
            CHECK(px[h].r > px[h].g && px[h].g > 0 && px[h].b == 0, "WORKING devia ser âmbar");
            CHECK(count_lit() < LED_FX_COUNT, "WORKING é um cometa, não a fita inteira");
            prev = h;
        }
    }

    /* 6. DONE e IDLE: fita inteira respirando na cor certa */
    {
        render(LED_FX_DONE, 1000);
        CHECK(count_lit() == LED_FX_COUNT, "DONE no pico devia acender os 7");
        CHECK(px[0].b > px[0].r && px[0].g > px[0].r, "DONE devia ser ciano");
        led_rgb_t peak = px[0];
        render(LED_FX_DONE, 0);
        CHECK(px[0].b < peak.b, "DONE devia respirar (vale mais fraco que o pico)");

        render(LED_FX_IDLE, 3000);
        CHECK(count_lit() == LED_FX_COUNT, "IDLE no pico devia acender os 7");
        CHECK(px[0].g > px[0].r && px[0].g > px[0].b, "IDLE devia ser verde");
        peak = px[0];
        render(LED_FX_IDLE, 0);
        CHECK(px[0].g < peak.g, "IDLE devia respirar (vale mais fraco que o pico)");
    }

    if (failures) {
        printf("%d falha(s)\n", failures);
        return 1;
    }
    printf("led_fx_test: ok\n");
    return 0;
}
