/**
 * @file
 * @brief Driver "sonic": o Sonic clássico com sprites de Sonic 3 & Knuckles.
 *
 * Sprites © SEGA, extraídos do disassembly da comunidade (skdisasm) pelo
 * scripts/sonic_export.py — uso pessoal, não redistribuir. Mesmo esquema do
 * driver clawd (RLE em flash, um buffer de frame em PSRAM, zero-copy), com
 * duas diferenças:
 *
 *  - cada animação toca uma SEQUÊNCIA de frames com ponto de loop, não um
 *    ciclo 0..N-1: a espera do jogo fica parada ~5 s e só então entra no
 *    ciclo impaciente de bater o pé, voltando sempre para ele;
 *  - o zoom é arredondado para múltiplo inteiro e o antialias desligado,
 *    para a pixel art ficar nítida em vez de borrada.
 */

#include "avatar.h"
#include "rle_sprite.h"

#include "esp_heap_caps.h"
#include "esp_log.h"

/* Arrays `static const`: incluir SOMENTE neste .c, senão duplicam em flash. */
#include "assets/sonic_sequences.h"
#include "assets/sprite_sonic_cheer.h"
#include "assets/sprite_sonic_sleep.h"
#include "assets/sprite_sonic_idle.h"
#include "assets/sprite_sonic_ko.h"
#include "assets/sprite_sonic_push.h"
#include "assets/sprite_sonic_run.h"

#define TAG             "avatar"
#define TRANSPARENT_KEY 0x18C5
#define SLEEP_AFTER_MS  (3u * 60u * 1000u)   /* idle contínuo até dormir */
#define PAD             4

typedef struct {
    const uint16_t *rle_data;
    const uint32_t *frame_offsets;   /* em words; frame_count+1 entradas */
    const uint8_t  *seq;             /* índices em rle_data por passo */
    uint16_t seq_len;
    uint16_t seq_loop;               /* passo para onde o loop retorna */
    uint16_t frame_ms;
    uint16_t width;
    uint16_t height;
} sonic_anim_t;

enum {
    ANIM_KO = 0,      /* caído de costas (desconectado) */
    ANIM_IDLE,        /* espera do jogo: parado -> impaciente batendo o pé */
    ANIM_SLEEP,       /* agacha e fica agachado (ocioso tempo demais) */
    ANIM_RUN,         /* correndo (trabalhando) */
    ANIM_CHEER,       /* levanta a mão e balança o dedo (terminou) */
    ANIM_PUSH,        /* empurrando parede invisível (bloqueado) */
    ANIM_COUNT,
};

/* campos: rle_data, offsets, seq, seq_len, seq_loop, frame_ms, width, height */
#define ANIM_DEF(lo, UP, ms) \
    { sonic_##lo##_rle_data, sonic_##lo##_frame_offsets, sonic_##lo##_seq, \
      SONIC_##UP##_SEQ_LEN, SONIC_##UP##_SEQ_LOOP, ms, \
      SONIC_##UP##_WIDTH, SONIC_##UP##_HEIGHT }

static const sonic_anim_t s_anims[ANIM_COUNT] = {
    [ANIM_KO]    = ANIM_DEF(ko,    KO,    600),
    /* 100 ms = a duração real da animação de espera no jogo (6 ticks de 60 Hz) */
    [ANIM_IDLE]  = ANIM_DEF(idle,  IDLE,  100),
    /* 167 ms = a duração do jogo (10 ticks): agacha e fica agachado uns 2 s
       antes de o ciclo recomeçar, então nunca congela num frame */
    [ANIM_SLEEP] = ANIM_DEF(sleep, SLEEP, 167),
    [ANIM_RUN]   = ANIM_DEF(run,   RUN,   60),
    [ANIM_CHEER] = ANIM_DEF(cheer, CHEER, 133),
    [ANIM_PUSH]  = ANIM_DEF(push,  PUSH,  200),
};

static lv_obj_t      *s_img;
static lv_img_dsc_t   s_dsc;
static uint8_t       *s_buf;         /* PSRAM, cabe o maior frame */
static uint16_t       s_zoom;        /* escala única, múltiplo inteiro */
static int            s_anim = -1;
static int            s_step;        /* posição na sequência */
static int            s_frame = -1;  /* frame decodificado no buffer */
static uint32_t       s_last_tick;
static uint32_t       s_idle_since;
static avatar_state_t s_st;

static void show_step(const sonic_anim_t *a)
{
    int f = a->seq[s_step];
    if (f == s_frame) {
        return;   /* a espera repete o mesmo frame por vários passos */
    }
    s_frame = f;
    rle_decode_tca16_swap(&a->rle_data[a->frame_offsets[f]], s_buf,
                          a->width * a->height, TRANSPARENT_KEY);
    lv_obj_invalidate(s_img);
}

static void play(int anim)
{
    s_anim = anim;
    s_step = 0;
    s_frame = -1;
    s_last_tick = lv_tick_get();
    if (!s_buf || !s_img) {
        return;
    }

    const sonic_anim_t *a = &s_anims[anim];
    s_dsc.header.cf = LV_IMG_CF_TRUE_COLOR_ALPHA;
    s_dsc.header.always_zero = 0;
    s_dsc.header.w = a->width;
    s_dsc.header.h = a->height;
    s_dsc.data_size = (uint32_t)a->width * a->height * 3;
    s_dsc.data = s_buf;
    show_step(a);
    lv_img_set_src(s_img, &s_dsc);   /* refaz pivot/tamanho e invalida */
    lv_img_set_zoom(s_img, s_zoom);

    /* zoom cresce em torno do centro; compensar metade do crescimento deixa
       o Sonic apoiado na base do slot em todas as animações */
    lv_coord_t grow = a->height * s_zoom / 256 - a->height;
    lv_obj_align(s_img, LV_ALIGN_BOTTOM_MID, 0, -(PAD + grow / 2));
}

static void sonic_create(lv_obj_t *parent)
{
    s_img = lv_img_create(parent);
    lv_obj_clear_flag(s_img, LV_OBJ_FLAG_CLICKABLE);
    lv_img_set_antialias(s_img, false);   /* pixel art nítida */

    /* maior escala que faz todos os sprites caberem, arredondada para
       múltiplo inteiro (pixel-perfect), e buffer do maior frame */
    uint32_t max_px = 0;
    s_zoom = 0xFFFF;
    for (int i = 0; i < ANIM_COUNT; i++) {
        const sonic_anim_t *a = &s_anims[i];
        uint16_t z = LV_MIN(256 * (AVATAR_SLOT_W - 2 * PAD) / a->width,
                            256 * (AVATAR_SLOT_H - 2 * PAD) / a->height);
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

static void sonic_destroy(void)
{
    /* widgets morrem no lv_obj_clean do motor; aqui só a memória própria */
    heap_caps_free(s_buf);
    s_buf = NULL;
    s_img = NULL;
    s_anim = -1;
}

static void sonic_set_state(avatar_state_t st)
{
    s_st = st;
    switch (st) {
    case AVATAR_ST_DISCONNECTED: play(ANIM_KO);    break;
    case AVATAR_ST_WORKING:      play(ANIM_RUN);   break;
    case AVATAR_ST_DONE:         play(ANIM_CHEER); break;
    case AVATAR_ST_BLOCKED:      play(ANIM_PUSH);  break;
    case AVATAR_ST_IDLE:
    default:
        s_idle_since = lv_tick_get();
        play(ANIM_IDLE);
        break;
    }
}

static void sonic_tick(uint32_t now)
{
    if (s_anim < 0 || !s_buf) {
        return;
    }
    if (s_st == AVATAR_ST_IDLE && s_anim == ANIM_IDLE &&
        now - s_idle_since >= SLEEP_AFTER_MS) {
        play(ANIM_SLEEP);
        return;
    }

    const sonic_anim_t *a = &s_anims[s_anim];
    if (now - s_last_tick < a->frame_ms) {
        return;
    }
    s_last_tick = now;
    s_step++;
    if (s_step >= a->seq_len) {
        s_step = a->seq_loop;
    }
    show_step(a);
}

const avatar_driver_t avatar_sonic_driver = {
    .name      = "sonic",
    .create    = sonic_create,
    .destroy   = sonic_destroy,
    .set_state = sonic_set_state,
    .tick      = sonic_tick,
};
