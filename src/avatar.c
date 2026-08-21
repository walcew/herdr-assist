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

#include <dirent.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs.h"

#include "avatar_store.h"
#include "sd.h"

/* O pacote de fábrica, embutido pelo EMBED_FILES em src/CMakeLists.txt. Fica
   mapeado em flash e é lido no lugar, sem cópia para a PSRAM. */
extern const uint8_t clawd_hav_start[] asm("_binary_clawd_hav_start");
extern const uint8_t clawd_hav_end[]   asm("_binary_clawd_hav_end");

#define TAG "avatar"
#define PAD 4     /* respiro nas bordas do slot */

#define AVATAR_DIR    SD_ROOT "/avatars"
#define AVATAR_EXT    ".hav"
/* Teto da lista. O cartão comporta muito mais, mas a troca é por toque cíclico:
   passar de uma dúzia já torna alcançar o último uma tarefa chata. */
#define MAX_AVATARS   12
#define ID_LEN        24

/* A escolha é gravada pelo ID, não pelo índice: com pacotes entrando e saindo
   do cartão, um índice salvo passaria a apontar para outro avatar a cada
   download. Foi o defeito que já obrigou a aposentar as chaves "sel".."sel4" —
   com marketplace ele deixa de ser contornável. */
#define NVS_NS  "avatar"
#define NVS_KEY "id"


static avatar_pack_t  s_pack;         /* o que está tocando */
static char           s_ids[MAX_AVATARS][ID_LEN];
static int            s_count;        /* sempre >= 1: o de fábrica é o índice 0 */
static int            s_cur;          /* índice em s_ids do avatar tocando */
static int64_t        s_t0;
static bool           s_loading;      /* leitura em curso na task de I/O */
static int            s_req_idx;      /* índice pedido, confirmado ao chegar */
static lv_obj_t      *s_slot;         /* pai do sprite; o spinner nasce aqui */
static lv_obj_t      *s_spin;         /* enquanto lê o cartão; NULL fora disso */
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

/* ---------- lista de avatares ---------- */

static void path_of(const char *id, char *out, size_t size)
{
    snprintf(out, size, AVATAR_DIR "/%s" AVATAR_EXT, id);
}

/* Varre o cartão uma vez, no boot. O índice 0 é sempre o de fábrica, com id
   vazio: ele não vem de arquivo nenhum e não pode ser apagado. */
static void scan_packs(void)
{
    s_ids[0][0] = '\0';
    s_count = 1;
    if (!sd_is_mounted()) {
        return;
    }
    DIR *d = opendir(AVATAR_DIR);
    if (!d) {
        return;   /* diretório ainda não existe: nada instalado */
    }
    const struct dirent *e;
    while ((e = readdir(d)) && s_count < MAX_AVATARS) {
        size_t n = strlen(e->d_name);
        size_t ext = strlen(AVATAR_EXT);
        if (n <= ext || n - ext >= ID_LEN ||
            strcasecmp(e->d_name + n - ext, AVATAR_EXT) != 0) {
            continue;   /* .part de download interrompido cai aqui */
        }
        memcpy(s_ids[s_count], e->d_name, n - ext);
        s_ids[s_count][n - ext] = '\0';
        s_count++;
    }
    closedir(d);
}

static int index_of(const char *id)
{
    for (int i = 0; i < s_count; i++) {
        if (strcmp(s_ids[i], id) == 0) {
            return i;
        }
    }
    return -1;
}

/* A NVS já foi inicializada no boot pelo panel_cfg_init(). */
static void load_choice(char *out, size_t size)
{
    nvs_handle_t h;
    out[0] = '\0';
    if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        size_t len = size;
        if (nvs_get_str(h, NVS_KEY, out, &len) != ESP_OK) {
            out[0] = '\0';
        }
        nvs_close(h);
    }
}

static void save_choice(const char *id)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_str(h, NVS_KEY, id);
        nvs_commit(h);
        nvs_close(h);
    }
}

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

static void adopt(avatar_pack_t *p);
static void show_spinner(bool on);

static void tick_cb(lv_timer_t *timer)
{
    (void)timer;
    /* Colheita do carregamento em segundo plano: a troca é feita aqui porque
       só esta task pode tocar na LVGL. */
    if (s_loading) {
        avatar_pack_t got;
        int r = avatar_store_take_pack(&got);
        if (r == 1) {
            s_loading = false;
            s_cur = s_req_idx;             /* só agora a escolha vale */
            save_choice(s_ids[s_cur]);
            adopt(&got);
            return;
        }
        if (r < 0) {
            s_loading = false;
            /* Pacote ilegível fica no cartão mas sai da lista: mantê-lo faria
               o toque no mascote bater sempre no mesmo erro, sem passar
               adiante. A varredura do próximo boot o traz de volta se ele
               melhorar. */
            ESP_LOGW(TAG, "%s não carregou; tirando da lista", s_ids[s_req_idx]);
            memmove(s_ids[s_req_idx], s_ids[s_req_idx + 1],
                    (size_t)(s_count - s_req_idx - 1) * ID_LEN);
            s_count--;
            if (s_cur > s_req_idx) {
                s_cur--;
            }
            show_spinner(false);
            lv_obj_set_style_img_opa(s_img, LV_OPA_COVER, 0);
        }
    }
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


/* ---------- troca de avatar ---------- */

/* Indicador de leitura do cartão. Criado e destruído junto com a espera, em vez
   de ficar escondido: é o único objeto animado da home fora do próprio avatar,
   e um spinner parado atrás de um LV_OBJ_FLAG_HIDDEN continuaria invalidando. */
static void show_spinner(bool on)
{
    if (on == (s_spin != NULL)) {
        return;
    }
    if (!on) {
        lv_obj_del(s_spin);
        s_spin = NULL;
        return;
    }
    s_spin = lv_spinner_create(s_slot, 900, 60);
    lv_obj_set_size(s_spin, 36, 36);
    lv_obj_clear_flag(s_spin, LV_OBJ_FLAG_CLICKABLE);   /* o toque é do slot */
    lv_obj_center(s_spin);
    lv_obj_set_style_arc_width(s_spin, 4, LV_PART_MAIN);
    lv_obj_set_style_arc_width(s_spin, 4, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(s_spin, UI_PANEL, LV_PART_MAIN);
    lv_obj_set_style_arc_color(s_spin, UI_IDLE, LV_PART_INDICATOR);
}

/* Entrega o pacote recém-lido ao motor: descarta o antigo, redimensiona o
   buffer de frame e recomeça no estado corrente. Só na task da LVGL. */
static void adopt(avatar_pack_t *p)
{
    s_anim = -1;                 /* nada mais aponta para o pacote velho */
    avatar_pack_free(&s_pack);
    s_pack = *p;
    memset(p, 0, sizeof(*p));

    lv_img_set_antialias(s_img, s_pack.antialias);
    uint32_t max_px;
    compute_zoom(&max_px);
    heap_caps_free(s_buf);
    s_buf = heap_caps_malloc(max_px * 3, MALLOC_CAP_SPIRAM);
    if (!s_buf) {
        ESP_LOGW(TAG, "sem PSRAM para o frame buffer (%u bytes)", (unsigned)(max_px * 3));
    }
    show_spinner(false);
    lv_obj_set_style_img_opa(s_img, LV_OPA_COVER, 0);
    s_frame = -1;
    play(role_idx((hav_role_t)s_st));
    ESP_LOGI(TAG, "avatar: %s (%d animações, zoom %d/256) — leitura %ldms",
             s_pack.name, s_pack.count, s_zoom, (long)((esp_timer_get_time() - s_t0) / 1000));
}

int avatar_count(void)
{
    return s_count;
}

const char *avatar_id_at(int idx)
{
    return (idx >= 0 && idx < s_count) ? s_ids[idx] : NULL;
}

const char *avatar_current(void)
{
    return s_ids[s_cur];
}

void avatar_select(const char *id)
{
    int idx = index_of(id);
    if (idx < 0 || idx == s_cur || s_loading || !s_img) {
        return;
    }
    if (!id[0]) {   /* de fábrica: está mapeado em flash, entra na hora */
        avatar_pack_t p;
        if (!avatar_pack_load_mem(clawd_hav_start,
                                  (size_t)(clawd_hav_end - clawd_hav_start), &p)) {
            return;
        }
        s_cur = idx;
        save_choice(id);
        adopt(&p);
        return;
    }
    char path[64];
    path_of(id, path, sizeof(path));
    if (!avatar_store_load_pack(path)) {
        return;   /* task ocupada (baixando, por exemplo): o toque não faz nada */
    }
    /* Esmaecer sozinho não dava sinal suficiente de que algo estava acontecendo:
       o pacote maior leva ~1,3s para ser lido, e o toque parecia ignorado.
       s_cur só muda quando o pacote chega inteiro (ver tick_cb). */
    lv_obj_set_style_img_opa(s_img, LV_OPA_50, 0);
    show_spinner(true);
    s_req_idx = idx;
    s_t0 = esp_timer_get_time();
    s_loading = true;
}

/* Toque no mascote passa para o próximo da lista. */
static void cycle_cb(lv_event_t *e)
{
    (void)e;
    if (s_count > 1) {
        avatar_select(s_ids[(s_cur + 1) % s_count]);
    }
}

void avatar_create(lv_obj_t *slot)
{
    if (!avatar_pack_load_mem(clawd_hav_start,
                              (size_t)(clawd_hav_end - clawd_hav_start), &s_pack)) {
        ESP_LOGE(TAG, "pacote de fábrica inválido");   /* build quebrado */
        return;
    }

    s_slot = slot;
    s_img = lv_img_create(slot);
    lv_obj_clear_flag(s_img, LV_OBJ_FLAG_CLICKABLE);   /* o toque é do slot */
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

    scan_packs();
    if (s_count > 1) {
        lv_obj_add_flag(slot, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(slot, cycle_cb, LV_EVENT_CLICKED, NULL);

        /* O de fábrica já está na tela; se a escolha salva for outra, ela entra
           quando a leitura terminar — o boot não espera pelo cartão. */
        char saved[ID_LEN];
        load_choice(saved, sizeof(saved));
        ESP_LOGI(TAG, "%d pacote(s) no cartão; escolhido: %s", s_count - 1,
                 saved[0] ? saved : "(de fábrica)");
        if (saved[0]) {
            avatar_select(saved);
        }
    }
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
