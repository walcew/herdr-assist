/**
 * @file
 * @brief Driver "clawd": o mascote do Claude animado por sprites do clawd-tank.
 *
 * Assets MIT do projeto clawd-tank (© 2026 Marcio Granzotto Rodrigues — ver
 * assets/LICENSE-clawd-tank). Um único buffer em PSRAM recebe o decode do
 * frame corrente e o lv_img referencia esse buffer direto (zero-copy).
 *
 * O Clawd tem o mesmo tamanho em todos os sprites — o que muda de um para
 * outro é o cenário em volta (o "zZ" do sono, o teclado, o "!"). Por isso a
 * escala é única para todas as animações: encaixar cada sprite no slot faria
 * o Clawd mudar de tamanho a cada troca de estado.
 */

#include "avatar.h"
#include "rle_sprite.h"
#include "ui_theme.h"

#include "esp_heap_caps.h"
#include "esp_log.h"

/* Cada sprite_*.h tem arrays `static const`: incluir SOMENTE neste .c,
   senão os dados duplicam em flash. */
#include "assets/sprite_alert.h"
#include "assets/sprite_dizzy.h"
#include "assets/sprite_idle.h"
#include "assets/sprite_sleeping.h"
#include "assets/sprite_typing.h"

#define TAG             "avatar"
#define TRANSPARENT_KEY 0x18C5                 /* fundo dos sprites */
#define SLEEP_AFTER_MS  (3u * 60u * 1000u)     /* idle contínuo até dormir */
#define PAD             4                      /* respiro nas bordas do slot */

typedef struct {
    const uint16_t *rle_data;
    const uint32_t *frame_offsets;   /* em words; frame_count+1 entradas */
    uint16_t frame_count;
    uint16_t frame_ms;
    uint16_t width;
    uint16_t height;
} clawd_anim_t;

enum {
    ANIM_DISCONNECTED = 0,
    ANIM_IDLE,
    ANIM_SLEEPING,
    ANIM_TYPING,
    ANIM_ALERT,
    ANIM_COUNT,
};

/* campos: rle_data, frame_offsets, frame_count, frame_ms, width, height */
#define ANIM_DEF(lo, UP, ms) \
    { lo##_rle_data, lo##_frame_offsets, UP##_FRAME_COUNT, ms, UP##_WIDTH, UP##_HEIGHT }

static const clawd_anim_t s_anims[ANIM_COUNT] = {
    /* dizzy no lugar do "disconnected" do clawd-tank, que é um ícone de
       Bluetooth — aqui a ponte é Wi-Fi/TCP */
    [ANIM_DISCONNECTED] = ANIM_DEF(dizzy,    DIZZY,    125),  /* 8 fps */
    [ANIM_IDLE]         = ANIM_DEF(idle,     IDLE,     167),  /* 6 fps */
    [ANIM_SLEEPING]     = ANIM_DEF(sleeping, SLEEPING, 167),  /* 6 fps */
    [ANIM_TYPING]       = ANIM_DEF(typing,   TYPING,   125),  /* 8 fps */
    [ANIM_ALERT]        = ANIM_DEF(alert,    ALERT,    100),  /* 10 fps */
};

static lv_obj_t      *s_img;
static lv_img_dsc_t   s_dsc;
static uint8_t       *s_buf;         /* PSRAM, cabe o maior frame */
static uint16_t       s_zoom;        /* escala única de todas as animações */
static int            s_anim = -1;
static int            s_frame;
static uint32_t       s_last_tick;
static uint32_t       s_idle_since;
static avatar_state_t s_st;

static void decode_frame(const clawd_anim_t *a, int idx)
{
    rle_decode_tca16_swap(&a->rle_data[a->frame_offsets[idx]], s_buf,
                          a->width * a->height, TRANSPARENT_KEY);
}

static void play(int anim)
{
    s_anim = anim;
    s_frame = 0;
    s_last_tick = lv_tick_get();
    if (!s_buf || !s_img) {
        return;
    }

    const clawd_anim_t *a = &s_anims[anim];
    decode_frame(a, 0);
    s_dsc.header.cf = LV_IMG_CF_TRUE_COLOR_ALPHA;
    s_dsc.header.always_zero = 0;
    s_dsc.header.w = a->width;
    s_dsc.header.h = a->height;
    s_dsc.data_size = (uint32_t)a->width * a->height * 3;
    s_dsc.data = s_buf;
    lv_img_set_src(s_img, &s_dsc);   /* refaz pivot/tamanho e invalida */
    lv_img_set_zoom(s_img, s_zoom);

    /* O sprite é desenhado com zoom em torno do centro do objeto; deslocar
       para cima metade do que ele cresce deixa o Clawd apoiado na base do
       slot, na mesma altura em todas as animações. */
    lv_coord_t grow = a->height * s_zoom / 256 - a->height;
    lv_obj_align(s_img, LV_ALIGN_BOTTOM_MID, 0, -(PAD + grow / 2));
}

static void clawd_create(lv_obj_t *parent)
{
    s_img = lv_img_create(parent);
    lv_obj_clear_flag(s_img, LV_OBJ_FLAG_CLICKABLE);

    /* escala única (a maior que faz todos os sprites caberem) e buffer do
       maior frame — ambos derivados da tabela, sem número mágico */
    uint32_t max_px = 0;
    s_zoom = 0xFFFF;
    for (int i = 0; i < ANIM_COUNT; i++) {
        const clawd_anim_t *a = &s_anims[i];
        uint16_t z = LV_MIN(256 * (AVATAR_SLOT_W - 2 * PAD) / a->width,
                            256 * (AVATAR_SLOT_H - 2 * PAD) / a->height);
        s_zoom = LV_MIN(s_zoom, z);
        max_px = LV_MAX(max_px, (uint32_t)a->width * a->height);
    }

    s_buf = heap_caps_malloc(max_px * 3, MALLOC_CAP_SPIRAM);
    if (!s_buf) {
        ESP_LOGW(TAG, "sem PSRAM para o frame buffer (%u bytes)", (unsigned)(max_px * 3));
    }
    s_anim = -1;
}

static void clawd_destroy(void)
{
    /* widgets morrem no lv_obj_clean do motor; aqui só a memória própria */
    heap_caps_free(s_buf);
    s_buf = NULL;
    s_img = NULL;
    s_anim = -1;
}

static void clawd_set_state(avatar_state_t st)
{
    s_st = st;
    switch (st) {
    case AVATAR_ST_DISCONNECTED: play(ANIM_DISCONNECTED); break;
    case AVATAR_ST_WORKING:      play(ANIM_TYPING);       break;
    case AVATAR_ST_BLOCKED:      play(ANIM_ALERT);        break;
    case AVATAR_ST_IDLE:
    default:
        s_idle_since = lv_tick_get();
        play(ANIM_IDLE);
        break;
    }
}

static void clawd_tick(uint32_t now)
{
    if (s_anim < 0 || !s_buf) {
        return;
    }
    if (s_st == AVATAR_ST_IDLE && s_anim == ANIM_IDLE &&
        now - s_idle_since >= SLEEP_AFTER_MS) {
        play(ANIM_SLEEPING);
        return;
    }

    const clawd_anim_t *a = &s_anims[s_anim];
    if (now - s_last_tick < a->frame_ms) {
        return;
    }
    s_last_tick = now;
    s_frame = (s_frame + 1) % a->frame_count;
    decode_frame(a, s_frame);
    /* mesmas dimensões: mutar o buffer + invalidar redesenha (decoder zero-copy) */
    lv_obj_invalidate(s_img);
}

const avatar_driver_t avatar_clawd_driver = {
    .name      = "clawd",
    .create    = clawd_create,
    .destroy   = clawd_destroy,
    .set_state = clawd_set_state,
    .tick      = clawd_tick,
};
