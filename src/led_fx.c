#include "led_fx.h"

/* Cores saturadas no mesmo tom da paleta da tela (ui_theme.h): a dessaturada
   sai esbranquiçada no WS2812. */
static const led_rgb_t RED   = {255, 0, 0};
static const led_rgb_t AMBER = {255, 144, 0};
static const led_rgb_t GREEN = {0, 255, 0};
static const led_rgb_t CYAN  = {0, 180, 255};
static const led_rgb_t WHITE = {255, 255, 255};

/* Cor em `level` (0..255), já com o teto de brilho. */
static led_rgb_t dim(led_rgb_t c, uint32_t level)
{
    return (led_rgb_t){
        (uint8_t)(c.r * level * LED_FX_MAX / (255 * 255)),
        (uint8_t)(c.g * level * LED_FX_MAX / (255 * 255)),
        (uint8_t)(c.b * level * LED_FX_MAX / (255 * 255)),
    };
}

/* Respiração: triângulo ao quadrado (o olho percebe brilho em curva), com piso
   para a fita não apagar de todo no vale. */
static uint32_t breathe(uint32_t t_ms, uint32_t period)
{
    uint32_t x = (t_ms % period) * 510 / period;
    uint32_t tri = x <= 255 ? x : 510 - x;
    return 24 + (tri * tri / 255) * (255 - 24) / 255;
}

static void fill(led_rgb_t out[LED_FX_COUNT], led_rgb_t c)
{
    for (int i = 0; i < LED_FX_COUNT; i++) {
        out[i] = c;
    }
}

void led_fx_render(led_fx_state_t st, uint32_t t_ms, led_rgb_t out[LED_FX_COUNT])
{
    static const led_rgb_t off = {0, 0, 0};
    fill(out, off);

    switch (st) {
    case LED_FX_BLOCKED: {
        /* pulsa-pulsa-pausa, no ritmo do sino da home */
        uint32_t p = t_ms % 1500;
        if (p < 200 || (p >= 350 && p < 550)) {
            fill(out, dim(RED, 255));
        }
        break;
    }
    case LED_FX_DONE:
        fill(out, dim(CYAN, breathe(t_ms, 2000)));
        break;
    case LED_FX_WORKING: {
        /* cometa: cabeça cheia e rastro que some, uma volta por segundo */
        static const uint8_t tail[] = {255, 80, 20};
        int head = (int)((t_ms % 1000) * LED_FX_COUNT / 1000);
        for (int k = 0; k < (int)sizeof(tail) && head - k >= 0; k++) {
            out[head - k] = dim(AMBER, tail[k]);
        }
        break;
    }
    case LED_FX_IDLE:
        fill(out, dim(GREEN, breathe(t_ms, 6000)));
        break;
    case LED_FX_DISCONNECTED:
        /* só sinal de vida: um ponto fraco a cada 3 s */
        if (t_ms % 3000 < 150) {
            out[0] = dim(WHITE, 64);
        }
        break;
    }
}
