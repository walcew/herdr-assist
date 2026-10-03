/**
 * @file
 * @brief Efeitos da fita de LED de status: estado + tempo -> cor de cada LED.
 *
 * C11 puro, sem ESP-IDF nem LVGL, para rodar no teste de host
 * (scripts/led_fx_test.c). Quem fala com a fita é o led_status.c.
 */

#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** LEDs na fita ligada ao P3. */
#define LED_FX_COUNT 7

/* Teto de brilho por canal (~78% de 255). Os 7 LEDs saem do mesmo regulador de
   3.3V do ESP32 e do display (~150 mA, mais os picos do Wi-Fi). Nenhum estado
   acende branco na fita inteira: o pior é o ciano cheio, ~160 mA neste teto.
   Acima disso o ganho visível é pequeno (o olho percebe brilho em curva) e a
   folga do regulador some. Calibre aqui se a fita ficar fraca ou forte demais. */
#define LED_FX_MAX 200

/**
 * Estado que a fita reflete. Mesmos valores e ordem de avatar_state_t — o
 * led_status.c confere em tempo de compilação; mexer numa lista exige
 * mexer na outra.
 */
typedef enum {
    LED_FX_DISCONNECTED = 0,
    LED_FX_IDLE,
    LED_FX_DONE,
    LED_FX_WORKING,
    LED_FX_BLOCKED,
} led_fx_state_t;

typedef struct {
    uint8_t r, g, b;
} led_rgb_t;

/**
 * Preenche `out` com a cor de cada LED para o estado no instante `t_ms`, no
 * nível do slider `pct` (0..100 do teto LED_FX_MAX; 0 apaga tudo).
 */
void led_fx_render(led_fx_state_t st, uint32_t t_ms, uint8_t pct,
                   led_rgb_t out[LED_FX_COUNT]);

#ifdef __cplusplus
}
#endif
