/**
 * @file
 * @brief Driver "goku": mascote que transforma conforme o uso semanal de IA.
 *
 * A forma (Criança -> Base -> SSJ -> SSJ2 -> SSJ3 -> Blue) vem do % de uso
 * semanal (7d) do Claude, lido do modelo a cada ~1s e mapeado por
 * goku_form_for_pct() com o modo (ascendente/descendente) escolhido nas
 * Configurações. Ao subir de forma, um clarão rápido. O estado global
 * (working/blocked/done/disconnected) vira um leve tingimento por recolor da
 * imagem — o sino de "blocked" do home continua à parte.
 *
 * Sprites PLACEHOLDER gerados por scripts/goku_export.py (mesmo formato RLE do
 * Clawd); trocar pela arte real é drop-in. Todas as formas têm o mesmo tamanho,
 * então a escala é única, como no Clawd.
 */
#include "avatar.h"
#include "goku_level.h"
#include "herdr_model.h"
#include "panel_cfg.h"
#include "rle_sprite.h"

#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"

/* Cada sprite_*.h tem arrays static const: incluir SOMENTE aqui, senão os
   dados duplicam em flash. */
#include "assets/sprite_goku_base.h"
#include "assets/sprite_goku_blue.h"
#include "assets/sprite_goku_crianca.h"
#include "assets/sprite_goku_ssj.h"
#include "assets/sprite_goku_ssj2.h"
#include "assets/sprite_goku_ssj3.h"

#define TAG             "avatar_goku"
#define TRANSPARENT_KEY 0x18C5
#define PAD             4
#define FRAME_MS        140     /* pulso da aura (~7 fps) */
#define POLL_MS         1000    /* cadência de releitura do uso */
#define FLARE_MS        450     /* clarão branco ao subir de forma */

typedef struct {
    const uint16_t *rle_data;
    const uint32_t *frame_offsets;   /* em words; frame_count+1 entradas */
    uint16_t frame_count;
    uint16_t width;
    uint16_t height;
} goku_anim_t;

/* campos: rle_data, frame_offsets, frame_count, width, height */
#define ANIM_DEF(lo, UP) \
    { lo##_rle_data, lo##_frame_offsets, UP##_FRAME_COUNT, UP##_WIDTH, UP##_HEIGHT }

/* Ordem obrigatoriamente igual a goku_form_t. */
static const goku_anim_t s_forms[GOKU_FORM_COUNT] = {
    [GOKU_FORM_CRIANCA] = ANIM_DEF(goku_crianca, GOKU_CRIANCA),
    [GOKU_FORM_BASE]    = ANIM_DEF(goku_base,    GOKU_BASE),
    [GOKU_FORM_SSJ]     = ANIM_DEF(goku_ssj,     GOKU_SSJ),
    [GOKU_FORM_SSJ2]    = ANIM_DEF(goku_ssj2,    GOKU_SSJ2),
    [GOKU_FORM_SSJ3]    = ANIM_DEF(goku_ssj3,    GOKU_SSJ3),
    [GOKU_FORM_BLUE]    = ANIM_DEF(goku_blue,    GOKU_BLUE),
};

static lv_obj_t      *s_img;
static lv_img_dsc_t   s_dsc;
static uint8_t       *s_buf;          /* PSRAM: cabe o maior frame */
static uint16_t       s_zoom;         /* escala única de todas as formas */
static int            s_form = -1;
static int            s_frame;
static uint32_t       s_last_tick;
static uint32_t       s_last_poll;
static uint32_t       s_flare_until;
static avatar_state_t s_st;

static void decode_frame(const goku_anim_t *a, int idx)
{
    rle_decode_tca16_swap(&a->rle_data[a->frame_offsets[idx]], s_buf,
                          a->width * a->height, TRANSPARENT_KEY);
}

/* Estado global e clarão viram um tingimento leve por cima do sprite, sem
   mexer nos pixels (a LVGL recolore e ajusta a opacidade da própria imagem). */
static void apply_recolor(void)
{
    if (!s_img) {
        return;
    }
    lv_color_t col = lv_color_black();
    lv_opa_t recolor = LV_OPA_TRANSP;
    lv_opa_t img_opa = LV_OPA_COVER;

    if (lv_tick_get() < s_flare_until) {
        col = lv_color_white();
        recolor = LV_OPA_70;
    } else {
        switch (s_st) {
        case AVATAR_ST_BLOCKED:
            col = lv_palette_main(LV_PALETTE_RED);
            recolor = LV_OPA_50;
            break;
        case AVATAR_ST_DONE:
            col = lv_palette_main(LV_PALETTE_GREEN);
            recolor = LV_OPA_30;
            break;
        case AVATAR_ST_DISCONNECTED:
            img_opa = LV_OPA_40;
            break;
        default:
            break;
        }
    }
    lv_obj_set_style_img_recolor(s_img, col, 0);
    lv_obj_set_style_img_recolor_opa(s_img, recolor, 0);
    lv_obj_set_style_img_opa(s_img, img_opa, 0);
}

static void play(int form)
{
    s_form = form;
    s_frame = 0;
    s_last_tick = lv_tick_get();
    if (!s_buf || !s_img) {
        return;
    }

    const goku_anim_t *a = &s_forms[form];
    decode_frame(a, 0);
    s_dsc.header.cf = LV_IMG_CF_TRUE_COLOR_ALPHA;
    s_dsc.header.always_zero = 0;
    s_dsc.header.w = a->width;
    s_dsc.header.h = a->height;
    s_dsc.data_size = (uint32_t)a->width * a->height * 3;
    s_dsc.data = s_buf;
    lv_img_set_src(s_img, &s_dsc);   /* refaz pivot/tamanho e invalida */
    lv_img_set_zoom(s_img, s_zoom);

    lv_coord_t grow = a->height * s_zoom / 256 - a->height;
    avatar_place(s_img, PAD, grow);
    apply_recolor();
}

/* % de uso semanal do Claude para dirigir a forma. Prefere a janela "7d"
   (todos os modelos); na falta dela, a maior janela vista. -1 se não há Claude. */
static int claude_weekly_pct(void)
{
    static herdr_limits_t lim[HERDR_MAX_PROVIDERS * CFG_MAX_HOSTS];
    int n = herdr_model_get_limits(lim, (int)(sizeof(lim) / sizeof(lim[0])));
    int best = -1;
    for (int i = 0; i < n; i++) {
        if (strcmp(lim[i].name, "Claude") != 0) {
            continue;
        }
        for (int r = 0; r < lim[i].row_count; r++) {
            int pct = lim[i].rows[r].pct;
            if (strcmp(lim[i].rows[r].label, "7d") == 0) {
                return pct;
            }
            if (pct > best) {
                best = pct;
            }
        }
    }
    return best;
}

static void poll_form(uint32_t now)
{
    if (s_form >= 0 && now - s_last_poll < POLL_MS) {
        return;
    }
    s_last_poll = now;

    int pct = claude_weekly_pct();
    if (pct < 0) {
        pct = 0;   /* sem dado do Claude ainda -> forma mais fraca */
    }
    int mode = panel_cfg_get()->goku_mode;
    int nf = goku_form_for_pct(pct, mode, s_form);
    if (nf == s_form) {
        return;
    }
    int leveled_up = (s_form >= 0 && nf > s_form);
    play(nf);
    if (leveled_up) {
        s_flare_until = now + FLARE_MS;
        apply_recolor();
    }
}

static void goku_create(lv_obj_t *parent)
{
    s_img = lv_img_create(parent);
    lv_obj_clear_flag(s_img, LV_OBJ_FLAG_CLICKABLE);

    /* escala única (a maior que faz todas as formas caberem) e buffer do maior
       frame — como no Clawd, derivados da tabela, sem número mágico */
    uint32_t max_px = 0;
    s_zoom = 0xFFFF;
    for (int i = 0; i < GOKU_FORM_COUNT; i++) {
        const goku_anim_t *a = &s_forms[i];
        uint16_t z = LV_MIN(256 * (avatar_slot_w() - 2 * PAD) / a->width,
                            256 * (avatar_slot_h() - 2 * PAD) / a->height);
        s_zoom = LV_MIN(s_zoom, z);
        max_px = LV_MAX(max_px, (uint32_t)a->width * a->height);
    }

    s_buf = heap_caps_malloc(max_px * 3, MALLOC_CAP_SPIRAM);
    if (!s_buf) {
        ESP_LOGW(TAG, "sem PSRAM para o frame buffer (%u bytes)", (unsigned)(max_px * 3));
    }
    s_form = -1;
    s_last_poll = 0;
    s_flare_until = 0;
    poll_form(lv_tick_get());   /* mostra uma forma já no create (sem flash vazio) */
}

static void goku_destroy(void)
{
    heap_caps_free(s_buf);
    s_buf = NULL;
    s_img = NULL;
    s_form = -1;
}

static void goku_set_state(avatar_state_t st)
{
    s_st = st;
    apply_recolor();
}

static void goku_tick(uint32_t now)
{
    if (!s_buf) {
        return;
    }
    poll_form(now);
    if (s_form < 0) {
        return;
    }
    if (s_flare_until && now >= s_flare_until) {
        s_flare_until = 0;
        apply_recolor();
    }

    const goku_anim_t *a = &s_forms[s_form];
    if (now - s_last_tick < FRAME_MS) {
        return;
    }
    s_last_tick = now;
    s_frame = (s_frame + 1) % a->frame_count;
    decode_frame(a, s_frame);
    lv_obj_invalidate(s_img);   /* mesmas dimensões: mutar o buffer + invalidar */
}

const avatar_driver_t avatar_goku_driver = {
    .name      = "goku",
    .create    = goku_create,
    .destroy   = goku_destroy,
    .set_state = goku_set_state,
    .tick      = goku_tick,
};
