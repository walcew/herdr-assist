/**
 * @file
 * @brief Driver "sf": Ryu vs Ken (Street Fighter Alpha) lutando na home.
 *
 * Sprites © Capcom, da Street Fighter Wiki — uso pessoal, não redistribuir.
 * Mesmo RLE dos outros avatares (flash -> um frame em PSRAM, zero-copy). Cada
 * frame já é uma CENA com os dois lutadores compostos (não há composição em
 * runtime); as cenas vêm de GIFs recortados por scripts/sf_export.py.
 *
 * Estado -> animação: WORKING os dois trocam golpes (a briga); IDLE em guarda
 * se encarando; BLOCKED travados (clinch); DONE toque de punho; DISCONNECTED
 * Ryu abatido. Todas em loop (sem transição). Como a cena é larga, o zoom pode
 * REDUZIR para caber no slot (os outros avatares só ampliam) — daí o antialias
 * ligado, para a redução não serrilhar.
 */
#include "avatar.h"
#include "rle_sprite.h"

#include "esp_heap_caps.h"
#include "esp_log.h"

/* Arrays static const: incluir SOMENTE aqui, senão duplicam em flash. */
#include "assets/sprite_sf_luta.h"
#include "assets/sprite_sf_encarando.h"
#include "assets/sprite_sf_guarda.h"
#include "assets/sprite_sf_vitoria.h"
#include "assets/sprite_sf_caidos.h"

#define TAG             "avatar_sf"
#define TRANSPARENT_KEY 0x18C5
#define PAD             4

typedef struct {
    const uint16_t *rle_data;
    const uint32_t *frame_offsets;
    uint16_t frame_count;
    uint16_t frame_ms;
    uint16_t width;
    uint16_t height;
} sf_anim_t;

enum {
    ANIM_LUTA = 0,      /* working */
    ANIM_ENCARANDO,     /* idle */
    ANIM_GUARDA,        /* blocked */
    ANIM_VITORIA,       /* done */
    ANIM_CAIDOS,        /* disconnected */
    ANIM_COUNT,
};

/* campos: rle_data, offsets, frame_count, frame_ms, width, height */
#define ANIM_DEF(lo, UP, ms) \
    { sf_##lo##_rle_data, sf_##lo##_frame_offsets, \
      SF_##UP##_FRAME_COUNT, ms, SF_##UP##_WIDTH, SF_##UP##_HEIGHT }

static const sf_anim_t s_anims[ANIM_COUNT] = {
    [ANIM_LUTA]      = ANIM_DEF(luta,      LUTA,       68),
    [ANIM_ENCARANDO] = ANIM_DEF(encarando, ENCARANDO, 120),
    [ANIM_GUARDA]    = ANIM_DEF(guarda,    GUARDA,    130),
    [ANIM_VITORIA]   = ANIM_DEF(vitoria,   VITORIA,   150),
    [ANIM_CAIDOS]    = ANIM_DEF(caidos,    CAIDOS,    180),
};

static lv_obj_t      *s_img;
static lv_img_dsc_t   s_dsc;
static uint8_t       *s_buf;
static uint16_t       s_zoom;
static int            s_anim = -1;
static int            s_frame;
static uint32_t       s_last_tick;

static int target_anim(avatar_state_t st)
{
    switch (st) {
    case AVATAR_ST_WORKING:      return ANIM_LUTA;
    case AVATAR_ST_BLOCKED:      return ANIM_GUARDA;
    case AVATAR_ST_DONE:         return ANIM_VITORIA;
    case AVATAR_ST_DISCONNECTED: return ANIM_CAIDOS;
    case AVATAR_ST_IDLE:
    default:                     return ANIM_ENCARANDO;
    }
}

static void show_frame(const sf_anim_t *a)
{
    rle_decode_tca16_swap(&a->rle_data[a->frame_offsets[s_frame]], s_buf,
                          a->width * a->height, TRANSPARENT_KEY);
    lv_obj_invalidate(s_img);
}

static void play(int anim)
{
    s_anim = anim;
    s_frame = 0;
    s_last_tick = lv_tick_get();
    if (!s_buf || !s_img) {
        return;
    }
    const sf_anim_t *a = &s_anims[anim];
    s_dsc.header.cf = LV_IMG_CF_TRUE_COLOR_ALPHA;
    s_dsc.header.always_zero = 0;
    s_dsc.header.w = a->width;
    s_dsc.header.h = a->height;
    s_dsc.data_size = (uint32_t)a->width * a->height * 3;
    s_dsc.data = s_buf;
    show_frame(a);
    lv_img_set_src(s_img, &s_dsc);   /* refaz pivot/tamanho e invalida */
    lv_img_set_zoom(s_img, s_zoom);

    lv_coord_t grow = a->height * s_zoom / 256 - a->height;
    avatar_place(s_img, PAD, grow);
}

static void sf_create(lv_obj_t *parent)
{
    s_img = lv_img_create(parent);
    lv_obj_clear_flag(s_img, LV_OBJ_FLAG_CLICKABLE);
    lv_img_set_antialias(s_img, true);   /* a cena é reduzida para caber */

    uint32_t max_px = 0;
    s_zoom = 0xFFFF;
    for (int i = 0; i < ANIM_COUNT; i++) {
        const sf_anim_t *a = &s_anims[i];
        uint16_t z = LV_MIN(256 * (avatar_slot_w() - 2 * PAD) / a->width,
                            256 * (avatar_slot_h() - 2 * PAD) / a->height);
        s_zoom = LV_MIN(s_zoom, z);
        max_px = LV_MAX(max_px, (uint32_t)a->width * a->height);
    }
    /* Diferente dos outros: nunca AMPLIA além do nativo (evita borrão/cabeça
       gigante nos estados de 1 lutador), mas PODE reduzir para caber. */
    s_zoom = LV_MIN(s_zoom, 256);

    s_buf = heap_caps_malloc(max_px * 3, MALLOC_CAP_SPIRAM);
    if (!s_buf) {
        ESP_LOGW(TAG, "sem PSRAM para o frame buffer (%u bytes)", (unsigned)(max_px * 3));
    }
    s_anim = -1;
}

static void sf_destroy(void)
{
    heap_caps_free(s_buf);
    s_buf = NULL;
    s_img = NULL;
    s_anim = -1;
}

static void sf_set_state(avatar_state_t st)
{
    play(target_anim(st));
}

static void sf_tick(uint32_t now)
{
    if (s_anim < 0 || !s_buf) {
        return;
    }
    const sf_anim_t *a = &s_anims[s_anim];
    if (now - s_last_tick < a->frame_ms) {
        return;
    }
    s_last_tick = now;
    s_frame++;
    if (s_frame >= a->frame_count) {
        s_frame = 0;
    }
    show_frame(a);
}

const avatar_driver_t avatar_sf_driver = {
    .name      = "sf",
    .create    = sf_create,
    .destroy   = sf_destroy,
    .set_state = sf_set_state,
    .tick      = sf_tick,
};
