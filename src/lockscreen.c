#include "lockscreen.h"

#include <string.h>

#include <lvgl.h>

#include "nvs.h"

#include "i18n.h"
#include "ui_theme.h"

#define NVS_NS      "lock"
#define KEY_ENABLED "en"
#define KEY_TIMEOUT "tmo"
#define KEY_PATTERN "pat"

#define PAT_MAX     9
#define PAT_MIN     4

/* Grade de 3x88 px centrada na área útil: célula folgada o bastante para o
   dedo, com o raio de captura menor que a metade dela (senão pontos vizinhos
   disputariam o mesmo toque). */
#define CELL        88
#define GRID        (3 * CELL)
#define HIT_R       28
#define DOT_R       8
#define DOT_SEL_R   16
#define LINE_W      4

#define PAD_TOP     (UI_TOPBAR_H + 40)   /* topbar + linha de status */
#define FLASH_MS    600                  /* erro em vermelho antes de limpar */
#define SAVED_MS    500                  /* confirmação em verde antes de sair */

typedef enum {
    MODE_NONE,
    MODE_UNLOCK,
    MODE_VERIFY,
    MODE_CAPTURE,
} lock_mode_t;

typedef enum {
    PHASE_NORMAL,
    PHASE_ERROR,
    PHASE_OK,
} phase_t;

/* --- config persistida --- */
static bool    s_enabled;
static uint8_t s_timeout_min;
static char    s_pattern[PAT_MAX + 1];

/* --- prazo de tolerância (só em RAM: reiniciar volta bloqueado) --- */
static uint32_t s_grace_tick;
static bool     s_grace_armed;

/* --- overlay --- */
static lv_obj_t   *s_overlay;
static lv_obj_t   *s_title;
static lv_obj_t   *s_status;
static lv_obj_t   *s_pad;
static lv_timer_t *s_timer;

/* --- desenho em curso --- */
static lock_mode_t  s_mode;
static phase_t s_phase;
static uint8_t s_seq[PAT_MAX];
static uint8_t s_seq_len;
static bool    s_tracking;
static lv_point_t s_finger;
static char    s_first[PAT_MAX + 1];   /* 1º desenho da captura */

static void (*s_on_success)(void);
static void (*s_on_done)(bool saved);

/* ---------- NVS ---------- */

/* A NVS já foi inicializada no boot pelo panel_cfg_init(). */
static void cfg_load(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) {
        return;   /* namespace ainda não existe: ficam os zeros */
    }
    uint8_t v = 0;
    if (nvs_get_u8(h, KEY_ENABLED, &v) == ESP_OK) {
        s_enabled = v != 0;
    }
    v = 0;
    if (nvs_get_u8(h, KEY_TIMEOUT, &v) == ESP_OK) {
        s_timeout_min = v;
    }
    size_t len = sizeof(s_pattern);
    if (nvs_get_str(h, KEY_PATTERN, s_pattern, &len) != ESP_OK) {
        s_pattern[0] = '\0';
    }
    nvs_close(h);
}

static void cfg_save_u8(const char *key, uint8_t value)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u8(h, key, value);
        nvs_commit(h);
        nvs_close(h);
    }
}

static void cfg_save_pattern(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_str(h, KEY_PATTERN, s_pattern);
        nvs_commit(h);
        nvs_close(h);
    }
}

/* ---------- sequência ---------- */

static bool seq_has(int idx)
{
    for (int i = 0; i < s_seq_len; i++) {
        if (s_seq[i] == idx) return true;
    }
    return false;
}

/* Regra do Android: passar por cima de um ponto ainda não usado o inclui, então
   ligar 0 a 2 registra 0-1-2. Sem isso, um padrão desenhado no celular não
   casaria aqui. */
static void seq_add(int idx)
{
    if (s_seq_len >= PAT_MAX || seq_has(idx)) return;
    if (s_seq_len > 0) {
        int a = s_seq[s_seq_len - 1];
        int dr = idx / 3 - a / 3;
        int dc = idx % 3 - a % 3;
        if (dr % 2 == 0 && dc % 2 == 0) {
            int mid = (a / 3 + dr / 2) * 3 + (a % 3 + dc / 2);
            if (mid != a && !seq_has(mid)) {
                s_seq[s_seq_len++] = (uint8_t)mid;
            }
        }
    }
    s_seq[s_seq_len++] = (uint8_t)idx;
}

static void seq_to_str(char *out)
{
    for (int i = 0; i < s_seq_len; i++) {
        out[i] = (char)('0' + s_seq[i]);
    }
    out[s_seq_len] = '\0';
}

/* ---------- geometria ---------- */

/* Centros dos 9 pontos em coordenadas absolutas. O widget ocupa a área útil
   inteira (a grade é só o miolo dele) para que a linha até o dedo não seja
   cortada pelo clip do próprio objeto. */
static void dot_centers(lv_point_t *c)
{
    lv_area_t a;
    lv_obj_get_coords(s_pad, &a);
    lv_coord_t gx = a.x1 + (lv_area_get_width(&a) - GRID) / 2;
    lv_coord_t gy = a.y1 + (lv_area_get_height(&a) - GRID) / 2;
    for (int i = 0; i < 9; i++) {
        c[i].x = gx + CELL / 2 + CELL * (i % 3);
        c[i].y = gy + CELL / 2 + CELL * (i / 3);
    }
}

static int dot_at(lv_coord_t x, lv_coord_t y)
{
    lv_point_t c[9];
    dot_centers(c);
    for (int i = 0; i < 9; i++) {
        lv_coord_t dx = x - c[i].x;
        lv_coord_t dy = y - c[i].y;
        if (dx * dx + dy * dy <= HIT_R * HIT_R) return i;
    }
    return -1;
}

/* ---------- estado visual ---------- */

static const char *default_status(void)
{
    if (s_mode != MODE_CAPTURE) return T(STR_LOCK_DRAW);
    return s_first[0] ? T(STR_LOCK_CONFIRM) : T(STR_LOCK_DRAW_NEW);
}

static void set_status(const char *text, lv_color_t color)
{
    lv_label_set_text(s_status, text);
    lv_obj_set_style_text_color(s_status, color, 0);
}

static lv_color_t phase_color(void)
{
    switch (s_phase) {
    case PHASE_ERROR: return UI_BLOCKED;
    case PHASE_OK:    return UI_IDLE;
    default:          return UI_TEXT;
    }
}

static void timer_clear(void)
{
    if (s_timer) {
        lv_timer_del(s_timer);
        s_timer = NULL;
    }
}

static void hide(void)
{
    timer_clear();
    s_mode = MODE_NONE;
    s_phase = PHASE_NORMAL;
    s_tracking = false;
    s_seq_len = 0;
    s_on_success = NULL;
    s_on_done = NULL;
    lv_obj_add_flag(s_overlay, LV_OBJ_FLAG_HIDDEN);
}

static void error_done_cb(lv_timer_t *t)
{
    (void)t;
    s_timer = NULL;   /* one-shot: a LVGL apaga sozinha ao voltar */
    s_phase = PHASE_NORMAL;
    s_seq_len = 0;
    set_status(default_status(), UI_MUTED);
    lv_obj_invalidate(s_pad);
}

static void flash_error(const char *message)
{
    timer_clear();
    s_phase = PHASE_ERROR;
    set_status(message, UI_BLOCKED);
    lv_obj_invalidate(s_pad);
    s_timer = lv_timer_create(error_done_cb, FLASH_MS, NULL);
    lv_timer_set_repeat_count(s_timer, 1);
}

static void saved_done_cb(lv_timer_t *t)
{
    (void)t;
    s_timer = NULL;
    void (*done)(bool) = s_on_done;
    hide();
    if (done) done(true);
}

/* ---------- avaliação do desenho ---------- */

static void evaluate(void)
{
    char drawn[PAT_MAX + 1];
    seq_to_str(drawn);

    if (s_mode == MODE_UNLOCK || s_mode == MODE_VERIFY) {
        if (strcmp(drawn, s_pattern) != 0) {
            flash_error(T(STR_LOCK_WRONG));   /* tentativas ilimitadas */
            return;
        }
        if (s_mode == MODE_UNLOCK && s_timeout_min > 0) {
            s_grace_tick = lv_tick_get();
            s_grace_armed = true;
        }
        void (*success)(void) = s_on_success;
        hide();
        if (success) success();
        return;
    }

    /* captura: dois desenhos iguais */
    if (s_seq_len < PAT_MIN) {
        flash_error(T(STR_LOCK_MIN_DOTS));
        return;
    }
    if (s_first[0] == '\0') {
        strlcpy(s_first, drawn, sizeof(s_first));
        s_seq_len = 0;
        set_status(T(STR_LOCK_CONFIRM), UI_MUTED);
        lv_obj_invalidate(s_pad);
        return;
    }
    if (strcmp(s_first, drawn) != 0) {
        s_first[0] = '\0';
        flash_error(T(STR_LOCK_MISMATCH));
        return;
    }
    strlcpy(s_pattern, drawn, sizeof(s_pattern));
    cfg_save_pattern();
    s_phase = PHASE_OK;
    set_status(T(STR_LOCK_SAVED), UI_IDLE);
    lv_obj_invalidate(s_pad);
    timer_clear();
    s_timer = lv_timer_create(saved_done_cb, SAVED_MS, NULL);
    lv_timer_set_repeat_count(s_timer, 1);
}

/* ---------- widget do padrão ---------- */

static void pad_draw_cb(lv_event_t *e)
{
    lv_draw_ctx_t *dc = lv_event_get_draw_ctx(e);
    lv_point_t c[9];
    dot_centers(c);
    lv_color_t accent = phase_color();

    lv_draw_line_dsc_t ld;
    lv_draw_line_dsc_init(&ld);
    ld.color = accent;
    ld.width = LINE_W;
    ld.opa = LV_OPA_COVER;
    ld.round_start = 1;
    ld.round_end = 1;
    for (int i = 1; i < s_seq_len; i++) {
        lv_draw_line(dc, &ld, &c[s_seq[i - 1]], &c[s_seq[i]]);
    }
    if (s_tracking && s_seq_len > 0) {
        lv_draw_line(dc, &ld, &c[s_seq[s_seq_len - 1]], &s_finger);
    }

    lv_draw_rect_dsc_t rd;
    lv_draw_rect_dsc_init(&rd);
    rd.radius = LV_RADIUS_CIRCLE;
    rd.bg_opa = LV_OPA_COVER;
    for (int i = 0; i < 9; i++) {
        bool on = seq_has(i);
        lv_coord_t r = on ? DOT_SEL_R : DOT_R;
        rd.bg_color = on ? accent : UI_PANEL;
        rd.border_width = on ? 0 : 1;
        rd.border_color = UI_BORDER;
        lv_area_t a = { c[i].x - r, c[i].y - r, c[i].x + r, c[i].y + r };
        lv_draw_rect(dc, &rd, &a);
    }
}

static void pad_event_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);

    if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
        if (!s_tracking) return;
        s_tracking = false;
        if (s_seq_len > 0) {
            evaluate();
        }
        lv_obj_invalidate(s_pad);
        return;
    }

    if (code == LV_EVENT_PRESSED) {
        if (s_phase == PHASE_OK) return;   /* já salvo, fechando: ignora */
        timer_clear();                     /* corta o flash de erro pendente */
        s_phase = PHASE_NORMAL;
        set_status(default_status(), UI_MUTED);
        s_seq_len = 0;
        s_tracking = true;
    } else if (code != LV_EVENT_PRESSING || !s_tracking) {
        return;
    }

    lv_indev_t *indev = lv_indev_get_act();
    if (!indev) return;
    lv_indev_get_point(indev, &s_finger);
    int idx = dot_at(s_finger.x, s_finger.y);
    if (idx >= 0) {
        seq_add(idx);
    }
    lv_obj_invalidate(s_pad);
}

/* ---------- overlay ---------- */

static void cancel_cb(lv_event_t *e)
{
    (void)e;
    lock_mode_t mode = s_mode;
    void (*done)(bool) = s_on_done;
    hide();
    if (mode == MODE_CAPTURE && done) {
        done(false);
    }
}

static void build_overlay(void)
{
    /* layer_sys fica acima da tela ativa e da layer_top (onde mora o toast de
       firmware, que se repromove sozinho a cada 5s) e é a primeira camada
       consultada no hit-test — escondido, não intercepta nada. */
    s_overlay = ui_plain(lv_layer_sys());
    lv_obj_set_size(s_overlay, LV_HOR_RES, LV_VER_RES);
    lv_obj_align(s_overlay, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_bg_opa(s_overlay, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(s_overlay, UI_BG, 0);
    lv_obj_add_flag(s_overlay, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t *bar = ui_topbar(s_overlay, NULL, NULL);
    ui_icon_btn(bar, LV_SYMBOL_LEFT, cancel_cb, NULL);
    s_title = lv_label_create(bar);
    lv_label_set_text(s_title, T(STR_LOCK_UNLOCK));
    lv_obj_set_style_text_font(s_title, &lv_font_ui_bold_20, 0);
    lv_obj_set_style_text_color(s_title, UI_TEXT, 0);
    lv_label_set_long_mode(s_title, LV_LABEL_LONG_DOT);
    lv_obj_set_flex_grow(s_title, 1);

    s_status = lv_label_create(s_overlay);
    lv_label_set_text(s_status, T(STR_LOCK_DRAW));
    lv_obj_set_style_text_font(s_status, &lv_font_ui_14, 0);
    lv_obj_set_style_text_color(s_status, UI_MUTED, 0);
    lv_obj_set_width(s_status, LV_HOR_RES);
    lv_obj_set_style_text_align(s_status, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s_status, LV_ALIGN_TOP_MID, 0, UI_TOPBAR_H + 4);

    s_pad = lv_obj_create(s_overlay);
    lv_obj_remove_style_all(s_pad);   /* obj cru: o desenho é todo do draw_cb */
    lv_obj_clear_flag(s_pad, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(s_pad, 0, PAD_TOP);
    lv_obj_set_size(s_pad, LV_HOR_RES, LV_VER_RES - PAD_TOP);
    lv_obj_add_event_cb(s_pad, pad_draw_cb, LV_EVENT_DRAW_MAIN, NULL);
    lv_obj_add_event_cb(s_pad, pad_event_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(s_pad, pad_event_cb, LV_EVENT_PRESSING, NULL);
    lv_obj_add_event_cb(s_pad, pad_event_cb, LV_EVENT_RELEASED, NULL);
    lv_obj_add_event_cb(s_pad, pad_event_cb, LV_EVENT_PRESS_LOST, NULL);
}

static void show(lock_mode_t mode, const char *title)
{
    timer_clear();
    s_mode = mode;
    s_phase = PHASE_NORMAL;
    s_seq_len = 0;
    s_tracking = false;
    s_first[0] = '\0';
    lv_label_set_text(s_title, title);
    set_status(default_status(), UI_MUTED);
    lv_obj_invalidate(s_pad);
    lv_obj_clear_flag(s_overlay, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_overlay);
}

/* ---------- API ---------- */

void lockscreen_init(void)
{
    cfg_load();
    build_overlay();
}

bool lockscreen_enabled(void)
{
    return s_enabled;
}

void lockscreen_set_enabled(bool enabled)
{
    s_enabled = enabled;
    s_grace_armed = false;
    cfg_save_u8(KEY_ENABLED, enabled ? 1 : 0);
}

uint8_t lockscreen_timeout_min(void)
{
    return s_timeout_min;
}

void lockscreen_set_timeout_min(uint8_t minutes)
{
    s_timeout_min = minutes;
    cfg_save_u8(KEY_TIMEOUT, minutes);
}

bool lockscreen_has_pattern(void)
{
    return s_pattern[0] != '\0';
}

bool lockscreen_is_locked(void)
{
    if (!s_enabled || s_pattern[0] == '\0') return false;
    if (s_timeout_min == 0) return true;   /* sem tolerância: pede sempre */
    if (!s_grace_armed) return true;
    return lv_tick_elaps(s_grace_tick) >= (uint32_t)s_timeout_min * 60000u;
}

void lockscreen_request_unlock(void (*on_success)(void))
{
    if (!lockscreen_is_locked()) {
        if (on_success) on_success();
        return;
    }
    s_on_success = on_success;
    s_on_done = NULL;
    show(MODE_UNLOCK, T(STR_LOCK_UNLOCK));
}

void lockscreen_request_verify(void (*on_success)(void))
{
    if (!s_enabled || s_pattern[0] == '\0') {
        if (on_success) on_success();
        return;
    }
    s_on_success = on_success;
    s_on_done = NULL;
    show(MODE_VERIFY, T(STR_LOCK_UNLOCK));
}

void lockscreen_request_capture(void (*on_done)(bool saved))
{
    s_on_success = NULL;
    s_on_done = on_done;
    show(MODE_CAPTURE, lockscreen_has_pattern() ? T(STR_LOCK_CHANGE_PAT) : T(STR_LOCK_SET_PAT));
}
