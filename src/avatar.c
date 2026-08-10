/**
 * @file
 * @brief Motor de avatar: seleção do driver, estado corrente e tick de animação.
 */

#include "avatar.h"

#include "nvs.h"

static const avatar_driver_t *s_drivers[] = {
    &avatar_pet_driver,
    &avatar_clawd_driver,
    &avatar_sonic_driver,   /* anexado no fim: não remapeia a seleção na NVS */
};
#define DRIVER_COUNT   ((int)(sizeof(s_drivers) / sizeof(s_drivers[0])))
#define DRIVER_DEFAULT 1   /* Clawd */

#define NVS_NS  "avatar"
#define NVS_KEY "sel"

static lv_obj_t      *s_slot;
static int            s_cur = DRIVER_DEFAULT;
static avatar_state_t s_state = AVATAR_ST_DISCONNECTED;

/* A NVS já foi inicializada no boot pelo panel_cfg_init(). */
static int load_sel(void)
{
    nvs_handle_t h;
    uint8_t v = DRIVER_DEFAULT;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        nvs_get_u8(h, NVS_KEY, &v);
        nvs_close(h);
    }
    return v < DRIVER_COUNT ? v : DRIVER_DEFAULT;
}

static void save_sel(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u8(h, NVS_KEY, (uint8_t)s_cur);
        nvs_commit(h);
        nvs_close(h);
    }
}

static void cycle_cb(lv_event_t *e)
{
    (void)e;
    s_drivers[s_cur]->destroy();
    lv_obj_clean(s_slot);
    s_cur = (s_cur + 1) % DRIVER_COUNT;
    s_drivers[s_cur]->create(s_slot);
    s_drivers[s_cur]->set_state(s_state);
    save_sel();
}

static void tick_cb(lv_timer_t *timer)
{
    (void)timer;
    if (!s_drivers[s_cur]->tick || !lv_obj_is_visible(s_slot)) {
        return;
    }
    s_drivers[s_cur]->tick(lv_tick_get());
}

void avatar_create(lv_obj_t *slot)
{
    s_slot = slot;
    s_cur = load_sel();
    lv_obj_add_flag(slot, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(slot, cycle_cb, LV_EVENT_CLICKED, NULL);
    s_drivers[s_cur]->create(slot);
    s_drivers[s_cur]->set_state(s_state);
    lv_timer_create(tick_cb, 33, NULL);
}

void avatar_set_state(avatar_state_t st)
{
    if (st == s_state) {
        return;
    }
    s_state = st;
    s_drivers[s_cur]->set_state(st);
}
