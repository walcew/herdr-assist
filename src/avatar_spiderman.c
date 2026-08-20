/**
 * @file
 * @brief Driver "spiderman": Spider-Man com sprites do Marvel Super Heroes (CPS2).
 *
 * Sprites © Marvel/Capcom, do sheet riggado por Alvin-Earthworm — uso pessoal,
 * não redistribuir. Mesmo esquema RLE dos outros (flash -> um frame em PSRAM,
 * zero-copy). A diferença é uma ANIMAÇÃO DE TRANSIÇÃO: ao trocar de estado, o
 * Spidey dá uma cambalhota e lança teia (one-shot) e só então entra no loop do
 * novo estado — o extra pedido para este avatar.
 *
 * Estado -> animação: IDLE em pé; WORKING rastejando; DONE comemora;
 * BLOCKED lança a maça de teia; DISCONNECTED ajoelhado.
 */
#include "avatar.h"
#include "rle_sprite.h"

#include "esp_heap_caps.h"
#include "esp_log.h"

/* Arrays static const: incluir SOMENTE aqui, senão duplicam em flash. */
#include "assets/sprite_spidey_cheer.h"
#include "assets/sprite_spidey_crawl.h"
#include "assets/sprite_spidey_down.h"
#include "assets/sprite_spidey_flip.h"
#include "assets/sprite_spidey_idle.h"
#include "assets/sprite_spidey_web.h"

#define TAG             "avatar_spidey"
#define TRANSPARENT_KEY 0x18C5
#define PAD             4

typedef struct {
    const uint16_t *rle_data;
    const uint32_t *frame_offsets;
    uint16_t frame_count;
    uint16_t frame_ms;
    uint16_t width;
    uint16_t height;
    uint8_t  loop;                   /* 1 = cicla; 0 = one-shot (encadeia no alvo) */
} spidey_anim_t;

enum {
    ANIM_IDLE = 0,
    ANIM_CRAWL,        /* working */
    ANIM_CHEER,        /* done */
    ANIM_WEB,          /* blocked (lança teia) */
    ANIM_DOWN,         /* disconnected */
    ANIM_FLIP,         /* transição (one-shot): cambalhota + teia */
    ANIM_COUNT,
};

/* campos: rle_data, offsets, frame_count, frame_ms, width, height, loop */
#define ANIM_DEF(lo, UP, ms, loop) \
    { spidey_##lo##_rle_data, spidey_##lo##_frame_offsets, \
      SPIDEY_##UP##_FRAME_COUNT, ms, SPIDEY_##UP##_WIDTH, SPIDEY_##UP##_HEIGHT, loop }

static const spidey_anim_t s_anims[ANIM_COUNT] = {
    [ANIM_IDLE]  = ANIM_DEF(idle,  IDLE,  130, 1),
    [ANIM_CRAWL] = ANIM_DEF(crawl, CRAWL,  90, 1),
    [ANIM_CHEER] = ANIM_DEF(cheer, CHEER, 110, 1),
    [ANIM_WEB]   = ANIM_DEF(web,   WEB,   120, 1),
    [ANIM_DOWN]  = ANIM_DEF(down,  DOWN,  150, 1),
    [ANIM_FLIP]  = ANIM_DEF(flip,  FLIP,   90, 0),   /* one-shot */
};

static lv_obj_t      *s_img;
static lv_img_dsc_t   s_dsc;
static uint8_t       *s_buf;
static uint16_t       s_zoom;
static int            s_anim = -1;
static int            s_target = ANIM_IDLE;   /* loop a entrar após a transição */
static int            s_frame;
static uint32_t       s_last_tick;
static avatar_state_t s_st;

static int target_anim(avatar_state_t st)
{
    switch (st) {
    case AVATAR_ST_WORKING:      return ANIM_CRAWL;
    case AVATAR_ST_DONE:         return ANIM_CHEER;
    case AVATAR_ST_BLOCKED:      return ANIM_WEB;
    case AVATAR_ST_DISCONNECTED: return ANIM_DOWN;
    case AVATAR_ST_IDLE:
    default:                     return ANIM_IDLE;
    }
}

static void show_frame(const spidey_anim_t *a)
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
    const spidey_anim_t *a = &s_anims[anim];
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

static void spidey_create(lv_obj_t *parent)
{
    s_img = lv_img_create(parent);
    lv_obj_clear_flag(s_img, LV_OBJ_FLAG_CLICKABLE);
    lv_img_set_antialias(s_img, false);   /* pixel art nítida */

    uint32_t max_px = 0;
    s_zoom = 0xFFFF;
    for (int i = 0; i < ANIM_COUNT; i++) {
        const spidey_anim_t *a = &s_anims[i];
        uint16_t z = LV_MIN(256 * (avatar_slot_w() - 2 * PAD) / a->width,
                            256 * (avatar_slot_h() - 2 * PAD) / a->height);
        s_zoom = LV_MIN(s_zoom, z);
        max_px = LV_MAX(max_px, (uint32_t)a->width * a->height);
    }
    s_zoom = LV_MAX(256, s_zoom / 256 * 256);   /* múltiplo inteiro */

    s_buf = heap_caps_malloc(max_px * 3, MALLOC_CAP_SPIRAM);
    if (!s_buf) {
        ESP_LOGW(TAG, "sem PSRAM para o frame buffer (%u bytes)", (unsigned)(max_px * 3));
    }
    s_anim = -1;
}

static void spidey_destroy(void)
{
    heap_caps_free(s_buf);
    s_buf = NULL;
    s_img = NULL;
    s_anim = -1;
}

static void spidey_set_state(avatar_state_t st)
{
    s_st = st;
    int t = target_anim(st);
    if (s_anim < 0) {
        play(t);                 /* primeira montagem: sem transição */
    } else {
        s_target = t;            /* troca de estado: cambalhota + teia, depois o alvo */
        play(ANIM_FLIP);
    }
}

static void spidey_tick(uint32_t now)
{
    if (s_anim < 0 || !s_buf) {
        return;
    }
    const spidey_anim_t *a = &s_anims[s_anim];
    if (now - s_last_tick < a->frame_ms) {
        return;
    }
    s_last_tick = now;
    s_frame++;
    if (s_frame >= a->frame_count) {
        if (!a->loop) {
            play(s_target);      /* fim da transição: entra no loop do estado */
            return;
        }
        s_frame = 0;
    }
    show_frame(a);
}

const avatar_driver_t avatar_spiderman_driver = {
    .name      = "spiderman",
    .create    = spidey_create,
    .destroy   = spidey_destroy,
    .set_state = spidey_set_state,
    .tick      = spidey_tick,
};
