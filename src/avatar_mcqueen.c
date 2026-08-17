/**
 * @file
 * @brief Driver "mcqueen": Lightning McQueen renderizado do modelo do jogo de NDS.
 *
 * Sprites © Disney/Pixar, renderizados do rip do modelo 3D pelo
 * scripts/mcqueen_export.py --fw — uso pessoal, não redistribuir. Mesmo esquema
 * RLE do driver sonic (rle_sprite.h: flash -> um frame em PSRAM, zero-copy),
 * mas sem tabelas de sequência: cada animação é um ciclo direto 0..N-1, os
 * "passos parados" já vêm assados nos frames (o export controla o ritmo).
 *
 * Estado -> animação (mesmo mapa do driver sonic, avatar.h):
 *   DISCONNECTED -> apagado, olhos fechados   WORKING -> vibra, olhar atento
 *   DONE         -> comemora girando          BLOCKED -> fala procurando o usuário
 *   IDLE         -> sorri de leve (+ dormir após SLEEP_AFTER_MS)
 */

#include "avatar.h"
#include "rle_sprite.h"

#include "esp_heap_caps.h"
#include "esp_log.h"

/* Arrays `static const`: incluir SOMENTE neste .c, senão duplicam em flash. */
#include "assets/sprite_mcqueen_idle.h"
#include "assets/sprite_mcqueen_working.h"
#include "assets/sprite_mcqueen_done.h"
#include "assets/sprite_mcqueen_blocked.h"
#include "assets/sprite_mcqueen_disconnected.h"
#include "assets/sprite_mcqueen_sleep.h"

#define TAG             "avatar"
#define TRANSPARENT_KEY 0x18C5
#define SLEEP_AFTER_MS  (3u * 60u * 1000u)   /* idle contínuo até dormir */
#define PAD             4

typedef struct {
    const uint16_t *rle_data;
    const uint32_t *frame_offsets;   /* em words; frame_count+1 entradas */
    uint16_t frame_count;
    uint16_t frame_ms;
    uint16_t width;
    uint16_t height;
} mcqueen_anim_t;

enum {
    ANIM_DISCONNECTED = 0,  /* apagado: olhos fechados, caído */
    ANIM_IDLE,              /* sorri de leve e desfaz, olhar passeia */
    ANIM_SLEEP,             /* pálpebras pesadas, cabeceia (ocioso demais) */
    ANIM_WORKING,           /* vibra de leve, olhar atento, arrancadas */
    ANIM_DONE,              /* comemora: giro completo com pulos, sorrindo */
    ANIM_BLOCKED,           /* preocupado, fala procurando o usuário */
    ANIM_COUNT,
};

/* campos: rle_data, offsets, frame_count, frame_ms, width, height */
#define ANIM_DEF(lo, UP, ms) \
    { mcqueen_##lo##_rle_data, mcqueen_##lo##_frame_offsets, \
      MCQUEEN_##UP##_FRAME_COUNT, ms, MCQUEEN_##UP##_WIDTH, MCQUEEN_##UP##_HEIGHT }

/* frame_ms = o mesmo ritmo aprovado na página de preview (mcqueen_anims) */
static const mcqueen_anim_t s_anims[ANIM_COUNT] = {
    [ANIM_DISCONNECTED] = ANIM_DEF(disconnected, DISCONNECTED, 160),
    [ANIM_IDLE]         = ANIM_DEF(idle,         IDLE,          90),
    [ANIM_SLEEP]        = ANIM_DEF(sleep,        SLEEP,        150),
    [ANIM_WORKING]      = ANIM_DEF(working,      WORKING,       60),
    [ANIM_DONE]         = ANIM_DEF(done,         DONE,          70),
    [ANIM_BLOCKED]      = ANIM_DEF(blocked,      BLOCKED,      100),
};

static lv_obj_t      *s_img;
static lv_img_dsc_t   s_dsc;
static uint8_t       *s_buf;         /* PSRAM, cabe o maior frame */
static uint16_t       s_zoom;
static int            s_anim = -1;
static int            s_frame;
static uint32_t       s_last_tick;
static uint32_t       s_idle_since;
static avatar_state_t s_st;

static void show_frame(const mcqueen_anim_t *a)
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

    const mcqueen_anim_t *a = &s_anims[anim];
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

static void mcqueen_create(lv_obj_t *parent)
{
    s_img = lv_img_create(parent);
    lv_obj_clear_flag(s_img, LV_OBJ_FLAG_CLICKABLE);
    lv_img_set_antialias(s_img, false);   /* paleta chapada nítida */

    /* maior escala que faz todos os frames caberem, arredondada para múltiplo
       inteiro (com 128px no slot de 160 isso dá 1:1), e buffer do maior frame */
    uint32_t max_px = 0;
    s_zoom = 0xFFFF;
    for (int i = 0; i < ANIM_COUNT; i++) {
        const mcqueen_anim_t *a = &s_anims[i];
        uint16_t z = LV_MIN(256 * (avatar_slot_w() - 2 * PAD) / a->width,
                            256 * (avatar_slot_h() - 2 * PAD) / a->height);
        s_zoom = LV_MIN(s_zoom, z);
        max_px = LV_MAX(max_px, (uint32_t)a->width * a->height);
    }
    s_zoom = LV_MAX(256, s_zoom / 256 * 256);

    s_buf = heap_caps_malloc(max_px * 3, MALLOC_CAP_SPIRAM);
    if (!s_buf) {
        ESP_LOGW(TAG, "sem PSRAM para o frame buffer (%u bytes)", (unsigned)(max_px * 3));
    }
    s_anim = -1;
}

static void mcqueen_destroy(void)
{
    /* widgets morrem no lv_obj_clean do motor; aqui só a memória própria */
    heap_caps_free(s_buf);
    s_buf = NULL;
    s_img = NULL;
    s_anim = -1;
}

static void mcqueen_set_state(avatar_state_t st)
{
    s_st = st;
    switch (st) {
    case AVATAR_ST_DISCONNECTED: play(ANIM_DISCONNECTED); break;
    case AVATAR_ST_WORKING:      play(ANIM_WORKING);      break;
    case AVATAR_ST_DONE:         play(ANIM_DONE);         break;
    case AVATAR_ST_BLOCKED:      play(ANIM_BLOCKED);      break;
    case AVATAR_ST_IDLE:
    default:
        s_idle_since = lv_tick_get();
        play(ANIM_IDLE);
        break;
    }
}

static void mcqueen_tick(uint32_t now)
{
    if (s_anim < 0 || !s_buf) {
        return;
    }
    if (s_st == AVATAR_ST_IDLE && s_anim == ANIM_IDLE &&
        now - s_idle_since >= SLEEP_AFTER_MS) {
        play(ANIM_SLEEP);
        return;
    }

    const mcqueen_anim_t *a = &s_anims[s_anim];
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

const avatar_driver_t avatar_mcqueen_driver = {
    .name      = "mcqueen",
    .create    = mcqueen_create,
    .destroy   = mcqueen_destroy,
    .set_state = mcqueen_set_state,
    .tick      = mcqueen_tick,
};
