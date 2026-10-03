#include "backlight.h"

#include <nvs.h>

#include "display.h"

#define NVS_NS    "backlight"
#define KEY_LEVEL "level"   /* ausente = 100, o padrão de fábrica */

static uint8_t s_level = 100;

void backlight_init(void)
{
    /* A NVS já foi inicializada no boot pelo panel_cfg_init(). */
    nvs_handle_t h;
    uint8_t level = 100;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        nvs_get_u8(h, KEY_LEVEL, &level);
        nvs_close(h);
    }
    backlight_set_level(level, false);
}

uint8_t backlight_level(void)
{
    return s_level;
}

void backlight_set_level(uint8_t pct, bool save)
{
    s_level = pct < BACKLIGHT_MIN ? BACKLIGHT_MIN : pct > 100 ? 100 : pct;
    bsp_display_brightness_set(s_level);
    nvs_handle_t h;
    if (save && nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u8(h, KEY_LEVEL, s_level);
        nvs_commit(h);
        nvs_close(h);
    }
}
