/**
 * @file
 * @brief Motor de avatar: carrega um pacote .hav e o anima conforme o estado.
 *
 * Um caminho de código só, para o avatar de fábrica e para os do cartão. Ele é
 * a união do que os cinco drivers antigos faziam separado — ciclo direto de
 * frames, sequência com ponto de laço, transição one-shot entre estados, sono
 * após ociosidade e os três modos de escala. Nada disso é opção nova: cada
 * peça existe porque um avatar real depende dela (ver avatar_pack.h).
 *
 * O frame corrente é decodificado num buffer em PSRAM que o lv_img referencia
 * direto (zero-copy): mutar o buffer e invalidar já redesenha.
 */

#include "avatar.h"

#include "avatar_pack.h"
#include "rle_sprite.h"

#include "esp_heap_caps.h"
#include "esp_log.h"

/* O pacote de fábrica, embutido pelo EMBED_FILES em src/CMakeLists.txt. Fica
   mapeado em flash e é lido no lugar, sem cópia para a PSRAM. */
extern const uint8_t clawd_hav_start[] asm("_binary_clawd_hav_start");
extern const uint8_t clawd_hav_end[]   asm("_binary_clawd_hav_end");

#define TAG "avatar"
#define PAD 4     /* respiro nas bordas do slot */

static avatar_pack_t  s_pack;
static lv_obj_t      *s_img;
static lv_img_dsc_t   s_dsc;
static uint8_t       *s_buf;         /* PSRAM, cabe o maior frame do pacote */
static uint16_t       s_zoom;        /* escala única de todas as animações */
static int            s_anim = -1;   /* índice em s_pack.anims */
static int            s_step;        /* posição na sequência (ou o próprio frame) */
static int            s_frame = -1;  /* frame já decodificado no buffer */
static int            s_target;      /* animação a entrar após uma transição */
static uint32_t       s_last_tick;
static uint32_t       s_idle_since;
static avatar_state_t s_st = AVATAR_ST_DISCONNECTED;

/**
 * Posiciona o sprite no slot; `grow` é o quanto ele cresce por causa do zoom
 * (que a LVGL aplica em torno do centro do objeto, sem mudar o tamanho dele).
 *
 * Em retrato quem limita o zoom é a altura: o mascote preenche a faixa e fica
 * apoiado na base, na mesma altura em todas as animações. Em paisagem o slot é
 * estreito e alto, quem limita passa a ser a largura e sobra altura — apoiar na
 * base deixaria o mascote caído num canto, então ele centra e fica na mesma
 * linha dos cards de host ao lado.
 */
static void place(lv_obj_t *img, lv_coord_t grow)
{
    if (ui_landscape()) {
        lv_obj_align(img, LV_ALIGN_CENTER, 0, 0);
    } else {
        lv_obj_align(img, LV_ALIGN_BOTTOM_MID, 0, -(PAD + grow / 2));
    }
}

/* Frame que o passo corrente aponta: pela sequência, quando há; senão o passo
   é o próprio índice do frame. */
static int step_frame(const avatar_anim_t *a)
{
    return a->seq ? a->seq[s_step] : s_step;
}

static void show_step(const avatar_anim_t *a)
{
    int f = step_frame(a);
    if (f == s_frame) {
        return;   /* sequência que repete o mesmo frame não redecodifica */
    }
    s_frame = f;
    rle_decode_tca16_swap(&a->rle[a->offsets[f]], a->rle_end, s_buf,
                          a->width * a->height, s_pack.key);
    lv_obj_invalidate(s_img);
}

static void play(int idx)
{
    s_anim = idx;
    s_step = 0;
    s_frame = -1;
    s_last_tick = lv_tick_get();
    if (!s_buf || !s_img) {
        return;
    }

    const avatar_anim_t *a = &s_pack.anims[idx];
    s_dsc.header.cf = LV_IMG_CF_TRUE_COLOR_ALPHA;
    s_dsc.header.always_zero = 0;
    s_dsc.header.w = a->width;
    s_dsc.header.h = a->height;
    s_dsc.data_size = (uint32_t)a->width * a->height * 3;
    s_dsc.data = s_buf;
    show_step(a);
    lv_img_set_src(s_img, &s_dsc);   /* refaz pivot/tamanho e invalida */
    lv_img_set_zoom(s_img, s_zoom);

    lv_coord_t grow = a->height * s_zoom / 256 - a->height;
    place(s_img, grow);
}

/* Índice da animação de um papel; cai na ociosidade quando o pacote não traz
   aquele papel (o parse garante que a ociosidade existe). */
static int role_idx(hav_role_t role)
{
    int8_t i = s_pack.by_role[role];
    return i >= 0 ? i : s_pack.by_role[HAV_ROLE_IDLE];
}

/**
 * Escala única para todas as animações do pacote: a maior que faz TODAS
 * caberem. Única de propósito — encaixar cada sprite no slot faria o
 * personagem mudar de tamanho a cada troca de estado, já que o que muda de um
 * sprite para outro costuma ser o cenário em volta, não ele.
 */
static void compute_zoom(uint32_t *max_px)
{
    *max_px = 0;
    s_zoom = 0xFFFF;
    for (int i = 0; i < s_pack.count; i++) {
        const avatar_anim_t *a = &s_pack.anims[i];
        uint16_t z = LV_MIN(256 * (avatar_slot_w() - 2 * PAD) / a->width,
                            256 * (avatar_slot_h() - 2 * PAD) / a->height);
        s_zoom = LV_MIN(s_zoom, z);
        *max_px = LV_MAX(*max_px, (uint32_t)a->width * a->height);
    }
    switch (s_pack.zoom) {
    case HAV_ZOOM_INTEGER:
        s_zoom = LV_MAX(256, s_zoom / 256 * 256);   /* pixel-perfect */
        break;
    case HAV_ZOOM_SHRINK:
        s_zoom = LV_MIN(s_zoom, 256);               /* nunca além do nativo */
        break;
    case HAV_ZOOM_FIT:
    default:
        break;
    }
}

static void tick_cb(lv_timer_t *timer)
{
    (void)timer;
    if (s_anim < 0 || !s_buf || !lv_obj_is_visible(s_img)) {
        return;
    }
    uint32_t now = lv_tick_get();

    /* Ociosidade longa vira sono, quando o pacote traz a animação. */
    if (s_st == AVATAR_ST_IDLE && s_pack.sleep_s &&
        s_pack.by_role[HAV_ROLE_SLEEP] >= 0 &&
        s_anim == s_pack.by_role[HAV_ROLE_IDLE] &&
        now - s_idle_since >= s_pack.sleep_s * 1000u) {
        play(s_pack.by_role[HAV_ROLE_SLEEP]);
        return;
    }

    const avatar_anim_t *a = &s_pack.anims[s_anim];
    if (now - s_last_tick < a->frame_ms) {
        return;
    }
    s_last_tick = now;

    s_step++;
    int len = a->seq_len ? a->seq_len : a->frames;
    if (s_step >= len) {
        if (!a->loop) {
            play(s_target);   /* fim da transição: entra no laço do estado */
            return;
        }
        s_step = a->seq_len ? a->seq_loop : 0;
    }
    show_step(a);
}

void avatar_create(lv_obj_t *slot)
{
    if (!avatar_pack_load_mem(clawd_hav_start,
                              (size_t)(clawd_hav_end - clawd_hav_start), &s_pack)) {
        ESP_LOGE(TAG, "pacote de fábrica inválido");   /* build quebrado */
        return;
    }

    s_img = lv_img_create(slot);
    lv_obj_clear_flag(s_img, LV_OBJ_FLAG_CLICKABLE);
    lv_img_set_antialias(s_img, s_pack.antialias);

    uint32_t max_px;
    compute_zoom(&max_px);
    s_buf = heap_caps_malloc(max_px * 3, MALLOC_CAP_SPIRAM);
    if (!s_buf) {
        ESP_LOGW(TAG, "sem PSRAM para o frame buffer (%u bytes)", (unsigned)(max_px * 3));
    }

    s_anim = -1;
    s_idle_since = lv_tick_get();
    lv_timer_create(tick_cb, 33, NULL);
    play(role_idx((hav_role_t)s_st));
}

void avatar_set_state(avatar_state_t st)
{
    if (st == s_st || s_anim < 0) {
        return;
    }
    s_st = st;
    if (st == AVATAR_ST_IDLE) {
        s_idle_since = lv_tick_get();
    }

    /* Pacote com transição encadeia: toca o one-shot e só então entra no laço
       do estado novo. Sem transição, entra direto. */
    int target = role_idx((hav_role_t)st);
    int trans = s_pack.by_role[HAV_ROLE_TRANSITION];
    if (trans >= 0) {
        s_target = target;
        play(trans);
    } else {
        play(target);
    }
}
