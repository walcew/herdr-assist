#include "led_status.h"

#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <led_strip.h>
#include <nvs.h>

#include "led_fx.h"

static const char *TAG = "led_status";

#define LED_GPIO     17
#define FRAME_MS     20   /* ~50 Hz: suave para as animações, folgado para o RMT */

/* led_fx indexa as animações pelo mesmo número de avatar_state_t */
_Static_assert(LED_FX_DISCONNECTED == (int)AVATAR_ST_DISCONNECTED &&
               LED_FX_IDLE == (int)AVATAR_ST_IDLE &&
               LED_FX_DONE == (int)AVATAR_ST_DONE &&
               LED_FX_WORKING == (int)AVATAR_ST_WORKING &&
               LED_FX_BLOCKED == (int)AVATAR_ST_BLOCKED,
               "led_fx_state_t e avatar_state_t divergiram");

#define NVS_NS  "ledstrip"
#define KEY_OFF "off"   /* negado: chave ausente = ligada, o padrão de fábrica */

static led_strip_handle_t s_strip;
/* escrito pela task da LVGL (toggle), lido pela da fita */
static volatile bool s_enabled = true;
/* escrito pela task da LVGL, lido pela da fita: um int é atômico no Xtensa */
static volatile int s_state = AVATAR_ST_DISCONNECTED;

/* Autoteste do boot: R, G e B na fita inteira. Confirma que os 7 LEDs estão
   vivos e que a ordem de cor está certa — vermelho saindo verde = fita RGB,
   não GRB (trocar color_component_format abaixo). */
static void self_test(void)
{
    static const uint8_t rgb[3][3] = {
        {LED_FX_MAX, 0, 0}, {0, LED_FX_MAX, 0}, {0, 0, LED_FX_MAX},
    };
    for (int c = 0; c < 3; c++) {
        for (int i = 0; i < LED_FX_COUNT; i++) {
            led_strip_set_pixel(s_strip, i, rgb[c][0], rgb[c][1], rgb[c][2]);
        }
        led_strip_refresh(s_strip);
        vTaskDelay(pdMS_TO_TICKS(200));
    }
}

static void led_task(void *arg)
{
    (void)arg;
    if (s_enabled) {
        self_test();
    }
    led_rgb_t px[LED_FX_COUNT];
    for (;;) {
        if (!s_enabled) {
            led_strip_clear(s_strip);
            vTaskDelay(pdMS_TO_TICKS(FRAME_MS * 5));
            continue;
        }
        uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
        led_fx_render((led_fx_state_t)s_state, now, px);
        for (int i = 0; i < LED_FX_COUNT; i++) {
            led_strip_set_pixel(s_strip, i, px[i].r, px[i].g, px[i].b);
        }
        led_strip_refresh(s_strip);
        vTaskDelay(pdMS_TO_TICKS(FRAME_MS));
    }
}

void led_status_init(void)
{
    /* A NVS já foi inicializada no boot pelo panel_cfg_init(). */
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        uint8_t off = 0;
        nvs_get_u8(h, KEY_OFF, &off);
        s_enabled = off == 0;
        nvs_close(h);
    }

    const led_strip_config_t cfg = {
        .strip_gpio_num = LED_GPIO,
        .max_leds = LED_FX_COUNT,
        .led_model = LED_MODEL_WS2812,
        .color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB,
    };
    const led_strip_rmt_config_t rmt = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = 10 * 1000 * 1000,
    };
    esp_err_t err = led_strip_new_rmt_device(&cfg, &rmt, &s_strip);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "fita sem RMT: %s", esp_err_to_name(err));
        return;
    }
    /* Criada cedo no boot pelo mesmo motivo da avatar_store: depois do display,
       da UI e do Wi-Fi a RAM interna não comporta mais uma pilha. */
    if (xTaskCreate(led_task, "led_status", 3072, NULL, 2, NULL) != pdPASS) {
        ESP_LOGE(TAG, "sem RAM para a task (%u livres)",
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    }
}

void led_status_set_state(avatar_state_t st)
{
    s_state = st;
}

bool led_status_enabled(void)
{
    return s_enabled;
}

void led_status_set_enabled(bool enabled)
{
    s_enabled = enabled;
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u8(h, KEY_OFF, enabled ? 0 : 1);
        nvs_commit(h);
        nvs_close(h);
    }
}
