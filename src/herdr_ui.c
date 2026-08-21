#include "herdr_ui.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <lvgl.h>

#include <esp_heap_caps.h>

#include "assets/logo_claude.h"
#include "assets/logo_codex.h"
#include "activity_log.h"
#include "avatar.h"
#include "i18n.h"
#include "herdr_model.h"
#include "money.h"
#include "herdr_conn.h"
#include "herdr_kb.h"
#include "herdr_ui_settings.h"
#include "lockscreen.h"
#include "panel_cfg.h"
#include "term_view.h"
#include "ui_theme.h"

#define DETAIL_POLL_TICKS 20   /* 20 x 150ms = 3s entre read_pane */
#define SCROLL_POLL_TICKS 2    /* rolando, a tela nova vem em 300ms */
#define HEAT_CELLS        12   /* células do mapa de calor por host */
#define ACTION_BAR_H      48
/* Medidas da home: as duas primeiras entram no cálculo do slot do avatar
   (avatar_slot_h(), em avatar.h), então mexer aqui pede conferir lá. */
#define HOME_PAD_TOP      20
#define HOME_HERO_H       62   /* faixa do relógio, do sino e do cadeado */
#define HOME_PAD_X        12
#define HOME_GAP          12   /* respiro entre os blocos da home */
#define HOST_CARD_H       60
#define HOST_CARD_GAP     6

/* --- telas --- */
static lv_obj_t *s_home;
static lv_obj_t *s_sessions;
static lv_obj_t *s_dash;
static lv_obj_t *s_detail;
static lv_obj_t *s_kb_overlay;

/* --- home --- */
static lv_obj_t *s_clock;
static lv_obj_t *s_date;
static lv_obj_t *s_beacon;      /* sino: alguma sessão espera decisão */
static lv_obj_t *s_beacon_lbl;
static lv_obj_t *s_lock_icon;   /* cadeado: terminal exige o padrão */
static bool s_lock_shown;
static lv_obj_t *s_host_area;

typedef struct {
    int       host;
    lv_obj_t *status;
    lv_obj_t *cells[HEAT_CELLS];
} host_widget_t;
static host_widget_t s_hw[CFG_MAX_HOSTS];
static int s_hw_count;

/* --- sessões --- */
static lv_obj_t *s_sess_list;
static lv_obj_t *s_lbl_group;   /* ícone do botão de agrupamento */
static lv_obj_t *s_lbl_sort;    /* ícone do botão de ordenação */
static bool s_group_by_host = true;
static bool s_sort_priority;

/* Cronômetros das sessões em "working". O since fica junto do ponteiro em vez
   de um índice para s_ui_agents: aquele array é recopiado a cada geração nova,
   mas a lista só se reconstrói com a aba aberta, então um índice guardado
   passaria a apontar para outro agente. */
typedef struct {
    lv_obj_t *label;
    uint32_t  since;
} sess_timer_t;
static sess_timer_t s_sess_timers[HERDR_MAX_AGENTS_TOTAL];
static int          s_sess_timer_count;
static time_t       s_last_timer_sec;

/* --- dash --- */
static lv_obj_t *s_dash_list;
static herdr_limits_t s_ui_limits[HERDR_MAX_PROVIDERS * CFG_MAX_HOSTS];
static int s_ui_limit_count;

/* --- detalhe --- */
static lv_obj_t *s_detail_title;
static lv_obj_t *s_detail_dot;
static lv_obj_t *s_term_cont;
static lv_obj_t *s_term_view;

/* --- aba Atividade --- */
static lv_obj_t *s_activity;
static lv_obj_t *s_activity_list;

/* --- teclado --- */
static lv_obj_t *s_kb_ta;
static lv_obj_t *s_keyboard;

/* --- estado --- */
static ui_tab_t s_tab = UI_TAB_HOME;
static herdr_agent_t s_ui_agents[HERDR_MAX_AGENTS_TOTAL];
static int s_ui_agent_count;
static char s_detail_pane[HERDR_ID_LEN];
static int s_detail_host = -1;
static bool s_detail_open;
/* alvo do teclado; guardado à parte porque o detalhe pode fechar antes do envio */
static char s_kb_pane[HERDR_ID_LEN];
static int s_kb_host = -1;
static uint32_t s_last_generation = UINT32_MAX;
static int s_poll_tick;
static int s_last_minute = -1;

static const char *host_label(int host)
{
    if (host < 0 || host >= CFG_MAX_HOSTS) {
        return "?";
    }
    const panel_host_t *h = &panel_cfg_get()->hosts[host];
    return h->name[0] ? h->name : (h->host[0] ? h->host : "auto");
}

/** Ordem de urgência para o modo prioridade: bloqueado primeiro. */
/* Ordem de quem pede atenção primeiro: "done" (terminou, ninguém abriu ainda)
   vem antes de "working", que não espera nada de quem olha o painel. */
static int status_rank(const char *status)
{
    if (strcmp(status, "blocked") == 0) return 0;
    if (strcmp(status, "done") == 0)    return 1;
    if (strcmp(status, "working") == 0) return 2;
    if (strcmp(status, "idle") == 0)    return 3;
    return 4;
}

/* ---------- navegação ---------- */

static void rebuild_session_rows(void);
static void rebuild_dash_cards(void);
static void rebuild_activity(void);
static bool fmt_when(char *buf, size_t size, time_t now, uint32_t t);

static void show_tab(ui_tab_t tab)
{
    s_tab = tab;
    lv_obj_add_flag(s_home, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_sessions, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_dash, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_activity, LV_OBJ_FLAG_HIDDEN);
    herdr_ui_settings_hide();
    switch (tab) {
    case UI_TAB_SESSIONS:
        /* a lista só se reconstrói com a aba aberta, então o que chegou
           enquanto ela estava escondida precisa ser montado agora */
        rebuild_session_rows();
        lv_obj_clear_flag(s_sessions, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(s_sessions);
        break;
    case UI_TAB_DASH:
        rebuild_dash_cards();   /* mesmo motivo da lista de sessões */
        lv_obj_clear_flag(s_dash, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(s_dash);
        break;
    case UI_TAB_ACTIVITY:
        rebuild_activity();     /* mesmo motivo das outras: monta o que chegou escondido */
        lv_obj_clear_flag(s_activity, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(s_activity);
        break;
    case UI_TAB_SETTINGS:
        herdr_ui_settings_show();
        break;
    default:
        lv_obj_clear_flag(s_home, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(s_home);
        break;
    }
}

static void dock_cb(lv_event_t *e)
{
    show_tab((ui_tab_t)(intptr_t)lv_event_get_user_data(e));
}

void herdr_ui_show_settings(void)
{
    show_tab(UI_TAB_SETTINGS);
}

static void do_open_detail(const herdr_agent_t *agent)
{
    strlcpy(s_detail_pane, agent->pane_id, HERDR_ID_LEN);
    s_detail_host = agent->host;
    s_detail_open = true;
    s_poll_tick = DETAIL_POLL_TICKS;  /* força read_pane no próximo tick */
    lv_label_set_text_fmt(s_detail_title, "%s / %s \xC2\xB7 %s",
                          host_label(agent->host), agent->project, agent->agent);
    lv_obj_set_style_bg_color(s_detail_dot, ui_status_color(agent->status), 0);
    term_view_clear(s_term_view, T(STR_LOADING));
    lv_obj_clear_flag(s_detail, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_detail);
}

/* Sessão que espera o desbloqueio. Guardada por valor porque s_ui_agents é
   recopiado a cada geração do modelo enquanto o padrão está sendo desenhado. */
static herdr_agent_t s_pending_agent;
static bool s_pending_valid;

static void unlock_then_open_cb(void)
{
    if (!s_pending_valid) {
        return;
    }
    s_pending_valid = false;
    for (int i = 0; i < s_ui_agent_count; i++) {
        if (s_ui_agents[i].host == s_pending_agent.host &&
            strcmp(s_ui_agents[i].pane_id, s_pending_agent.pane_id) == 0) {
            do_open_detail(&s_ui_agents[i]);   /* título e status frescos */
            return;
        }
    }
    /* a sessão morreu durante o desbloqueio: destrava e fica onde está */
}

/* Único caminho para o terminal, e por isso o ponto onde o bloqueio de tela
   entra: bloqueado, pede o padrão e abre a sessão tocada no acerto. */
static void open_detail(const herdr_agent_t *agent)
{
    if (lockscreen_is_locked()) {
        s_pending_agent = *agent;
        s_pending_valid = true;
        lockscreen_request_unlock(unlock_then_open_cb);
        return;
    }
    do_open_detail(agent);
}

static void close_detail(void)
{
    herdr_conn_release_pane(s_detail_host, s_detail_pane);  /* devolve a resolução */
    s_detail_open = false;
    s_detail_pane[0] = '\0';
    s_detail_host = -1;
    lv_obj_add_flag(s_detail, LV_OBJ_FLAG_HIDDEN);
}

/* ---------- callbacks ---------- */

static void row_clicked_cb(lv_event_t *e)
{
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    if (idx < s_ui_agent_count) {
        open_detail(&s_ui_agents[idx]);
    }
}

static void back_clicked_cb(lv_event_t *e)
{
    (void)e;
    close_detail();
}

/* Sino da home: abre a primeira sessão que está esperando decisão. Resolvida
   ela, o status muda e o beacon passa a apontar para a seguinte. */
static void beacon_clicked_cb(lv_event_t *e)
{
    (void)e;
    for (int i = 0; i < s_ui_agent_count; i++) {
        if (strcmp(s_ui_agents[i].status, "blocked") == 0) {
            open_detail(&s_ui_agents[i]);
            return;
        }
    }
}

/* Cadeado da home: desbloquear por aqui só arma o prazo de tolerância; sem
   prazo configurado, o padrão volta a ser pedido na próxima sessão aberta. */
static void lock_icon_cb(lv_event_t *e)
{
    (void)e;
    lockscreen_request_unlock(NULL);
}

/* Arraste vertical no terminal: rola a sessão no host (o app decide o que
   fazer com a roda) e antecipa a próxima leitura para o gesto ter resposta. */
static void term_scrolled_cb(int lines, int col, int row)
{
    if (!s_detail_open) {
        return;
    }
    herdr_conn_scroll_pane(s_detail_host, s_detail_pane,
                           lines > 0 ? lines : -lines, lines > 0, col, row);
    if (s_poll_tick < DETAIL_POLL_TICKS - SCROLL_POLL_TICKS) {
        s_poll_tick = DETAIL_POLL_TICKS - SCROLL_POLL_TICKS;
    }
}

static void action_key_cb(lv_event_t *e)
{
    const char *key = lv_event_get_user_data(e);
    if (s_detail_pane[0]) {
        herdr_conn_send_keys(s_detail_host, s_detail_pane, &key, 1);
    }
}

static void action_focus_cb(lv_event_t *e)
{
    (void)e;
    if (s_detail_pane[0]) {
        herdr_conn_focus(s_detail_host, s_detail_pane);
    }
}

static void open_keyboard(int host, const char *pane_id)
{
    s_kb_host = host;
    strlcpy(s_kb_pane, pane_id, HERDR_ID_LEN);
    lv_textarea_set_text(s_kb_ta, "");
    /* sempre reabre na página de letras, como o teclado de celular */
    lv_keyboard_set_mode(s_keyboard, LV_KEYBOARD_MODE_TEXT_LOWER);
    lv_obj_clear_flag(s_kb_overlay, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_kb_overlay);
}

static void action_prompt_cb(lv_event_t *e)
{
    (void)e;
    if (s_detail_pane[0]) {
        open_keyboard(s_detail_host, s_detail_pane);
    }
}

static void kb_event_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_READY) {          /* checkmark: envia */
        const char *text = lv_textarea_get_text(s_kb_ta);
        if (s_kb_pane[0] && text[0]) {
            herdr_conn_send_text(s_kb_host, s_kb_pane, text);
            static const char *enter = "Enter";
            herdr_conn_send_keys(s_kb_host, s_kb_pane, &enter, 1);
        }
        lv_obj_add_flag(s_kb_overlay, LV_OBJ_FLAG_HIDDEN);
    } else if (code == LV_EVENT_CANCEL) {  /* teclado fechado: cancela */
        lv_obj_add_flag(s_kb_overlay, LV_OBJ_FLAG_HIDDEN);
    }
}

/** O ícone de cada botão mostra o modo em vigor, não o que o toque faria. */
static void update_toolbar_icons(void)
{
    lv_label_set_text(s_lbl_group, s_group_by_host ? UI_ICON_GROUPED : UI_ICON_FLAT);
    lv_label_set_text(s_lbl_sort, s_sort_priority ? UI_ICON_SORT_PRI : UI_ICON_SORT_NAT);
}

static void toggle_group_cb(lv_event_t *e)
{
    (void)e;
    s_group_by_host = !s_group_by_host;
    update_toolbar_icons();
    rebuild_session_rows();
}

static void toggle_sort_cb(lv_event_t *e)
{
    (void)e;
    s_sort_priority = !s_sort_priority;
    update_toolbar_icons();
    rebuild_session_rows();
}

/* ---------- home ---------- */

#define BEACON_SWING_PX 4

/**
 * Balanço do sino.
 *
 * A fase percorre dois períodos de seno por rajada, então o repouso cai
 * exatamente em zero — com uma rampa simples de -4 a 4, a pausa entre as
 * rajadas deixaria o ícone parado torto.
 */
static void beacon_swing_cb(void *obj, int32_t phase)
{
    lv_obj_set_style_translate_x(
        obj, lv_trigo_sin((int16_t)phase) * BEACON_SWING_PX / LV_TRIGO_SIN_MAX, 0);
}

/** Mostra o sino enquanto houver sessão esperando decisão; `n` vai no rótulo. */
static void update_beacon(int n)
{
    if (n == 0) {
        lv_anim_del(s_beacon, beacon_swing_cb);
        lv_obj_set_style_translate_x(s_beacon, 0, 0);
        lv_obj_add_flag(s_beacon, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    if (n > 1) {
        lv_label_set_text_fmt(s_beacon_lbl, LV_SYMBOL_BELL " %d", n);
    } else {
        lv_label_set_text(s_beacon_lbl, LV_SYMBOL_BELL);
    }
    if (!lv_obj_has_flag(s_beacon, LV_OBJ_FLAG_HIDDEN)) {
        return;   /* já tocando: só o rótulo podia ter mudado */
    }
    lv_obj_clear_flag(s_beacon, LV_OBJ_FLAG_HIDDEN);
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, s_beacon);
    lv_anim_set_exec_cb(&a, beacon_swing_cb);
    lv_anim_set_values(&a, 0, 720);
    lv_anim_set_time(&a, 700);
    lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_repeat_delay(&a, 800);   /* toca, pausa, toca */
    lv_anim_start(&a);
}

/**
 * Cadeado da home: aparece enquanto o terminal exigir o padrão e some quando o
 * prazo de tolerância está valendo. Com ele visível o sino anda para o lado.
 */
static void refresh_lock_icon(void)
{
    bool locked = lockscreen_is_locked();
    if (locked == s_lock_shown) {
        return;
    }
    s_lock_shown = locked;
    if (locked) {
        lv_obj_clear_flag(s_lock_icon, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_lock_icon, LV_OBJ_FLAG_HIDDEN);
    }
    lv_obj_align(s_beacon, LV_ALIGN_RIGHT_MID, locked ? -(UI_ICON_BTN + 10) : 0, -8);
}

/**
 * Monta os cards de host (nome, status e mapa de calor) numa faixa de largura w.
 *
 * A largura entra resolvida em pixels — e não como LV_PCT(100) — porque o passo
 * das células do mapa de calor sai dela, e o layout ainda não foi calculado
 * quando os cards nascem.
 */
static void build_host_cards(lv_obj_t *parent, lv_coord_t w)
{
    const panel_cfg_t *cfg = panel_cfg_get();
    /* Clampado no passo de projeto: em retrato sobra espaço e o tamanho de hoje
       é o desejado; em paisagem a coluna é estreita e as células encolhem. */
    lv_coord_t step = LV_MIN(21, (w - 24) / HEAT_CELLS);
    lv_coord_t cell = step - 4;

    lv_coord_t area_h = ui_landscape() ? avatar_slot_h() : 152;
    int enabled = 0;
    for (int h = 0; h < CFG_MAX_HOSTS; h++) {
        enabled += cfg->hosts[h].enabled;
    }
    /* Só em paisagem, onde a coluna é alta e os cards colados no topo destoariam
       do mascote ao lado; centrados, os dois blocos ficam na mesma linha. Em
       retrato a faixa é justa e a pilha segue do topo, como sempre foi. E com a
       coluna cheia volta ao topo nas duas: centrar o que não cabe empurraria o
       primeiro card para fora e a rolagem começaria no meio da lista. */
    bool center = ui_landscape() &&
                  enabled * HOST_CARD_H + (enabled - 1) * HOST_CARD_GAP <= area_h;

    s_host_area = ui_plain(parent);
    lv_obj_set_size(s_host_area, w, area_h);
    lv_obj_set_flex_flow(s_host_area, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_host_area, center ? LV_FLEX_ALIGN_CENTER : LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(s_host_area, HOST_CARD_GAP, 0);
    lv_obj_add_flag(s_host_area, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(s_host_area, LV_DIR_VER);

    for (int h = 0; h < CFG_MAX_HOSTS; h++) {
        if (!cfg->hosts[h].enabled) {
            continue;
        }
        host_widget_t *hw = &s_hw[s_hw_count++];
        hw->host = h;

        lv_obj_t *card = ui_card(s_host_area, 8);
        lv_obj_set_size(card, LV_PCT(100), HOST_CARD_H);
        lv_obj_set_style_pad_left(card, 12, 0);
        lv_obj_set_style_pad_right(card, 12, 0);
        lv_obj_set_style_pad_top(card, 10, 0);

        lv_obj_t *name = lv_label_create(card);
        lv_label_set_text(name, host_label(h));
        lv_obj_set_style_text_font(name, &lv_font_ui_12, 0);
        lv_obj_set_style_text_color(name, UI_TEXT, 0);
        lv_obj_align(name, LV_ALIGN_TOP_LEFT, 0, 0);

        hw->status = lv_label_create(card);
        lv_label_set_text(hw->status, "");
        lv_obj_set_style_text_font(hw->status, &lv_font_ui_12, 0);
        lv_obj_set_style_text_color(hw->status, UI_MUTED, 0);
        lv_obj_align(hw->status, LV_ALIGN_TOP_RIGHT, 0, 0);

        for (int c = 0; c < HEAT_CELLS; c++) {
            hw->cells[c] = lv_obj_create(card);
            lv_obj_set_size(hw->cells[c], cell, cell);
            lv_obj_set_style_radius(hw->cells[c], 4, 0);
            lv_obj_set_style_border_width(hw->cells[c], 0, 0);
            lv_obj_set_style_bg_color(hw->cells[c], UI_CELL, 0);
            lv_obj_align(hw->cells[c], LV_ALIGN_TOP_LEFT, c * step, 23);
            lv_obj_clear_flag(hw->cells[c], LV_OBJ_FLAG_SCROLLABLE);
        }
    }

    if (s_hw_count == 0) {
        lv_obj_t *empty = lv_label_create(s_host_area);
        lv_label_set_text_fmt(empty, "%s\n%s", T(STR_NO_HOSTS), T(STR_TAP_TO_ADD));
        lv_obj_set_style_text_font(empty, &lv_font_ui_12, 0);
        lv_obj_set_style_text_color(empty, UI_MUTED, 0);
    }
}

static void build_home(void)
{
    /* A tela crua é o pai do dock, e por isso fica sem padding: lv_obj_align
       resolve contra a área de conteúdo do pai, então um padding aqui empurraria
       o próprio dock junto. As margens vivem na coluna, como nas outras telas. */
    s_home = ui_screen();

    lv_obj_t *col = ui_plain(s_home);
    lv_obj_set_size(col, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_pad_top(col, HOME_PAD_TOP, 0);
    lv_obj_set_style_pad_left(col, HOME_PAD_X + UI_DOCK_W, 0);
    lv_obj_set_style_pad_right(col, HOME_PAD_X, 0);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(col, HOME_GAP, 0);

    lv_obj_t *hero = ui_plain(col);
    lv_obj_set_size(hero, LV_PCT(100), HOME_HERO_H);

    s_clock = lv_label_create(hero);
    lv_label_set_text(s_clock, "--:--");
    lv_obj_set_style_text_font(s_clock, &lv_font_ui_clock_44, 0);
    lv_obj_set_style_text_color(s_clock, UI_TEXT, 0);
    lv_obj_align(s_clock, LV_ALIGN_LEFT_MID, 0, -8);

    s_date = lv_label_create(hero);
    lv_label_set_text(s_date, T(STR_SYNCING));
    lv_obj_set_style_text_font(s_date, &lv_font_ui_12, 0);
    lv_obj_set_style_text_color(s_date, UI_MUTED, 0);
    lv_obj_align(s_date, LV_ALIGN_LEFT_MID, 2, 20);

    /* do lado oposto ao relógio: é o alerta de decisão pendente */
    s_beacon = ui_icon_btn(hero, LV_SYMBOL_BELL, beacon_clicked_cb, NULL);
    lv_obj_align(s_beacon, LV_ALIGN_RIGHT_MID, 0, -8);
    s_beacon_lbl = lv_obj_get_child(s_beacon, 0);
    lv_obj_set_style_text_color(s_beacon_lbl, UI_BLOCKED, 0);
    lv_obj_add_flag(s_beacon, LV_OBJ_FLAG_HIDDEN);

    /* mesmo canto: quando os dois aparecem, o cadeado fica na ponta */
    s_lock_icon = ui_icon_btn(hero, UI_ICON_LOCK, lock_icon_cb, NULL);
    lv_obj_align(s_lock_icon, LV_ALIGN_RIGHT_MID, 0, -8);
    lv_obj_set_style_text_color(lv_obj_get_child(s_lock_icon, 0), UI_MUTED, 0);
    lv_obj_add_flag(s_lock_icon, LV_OBJ_FLAG_HIDDEN);

    /* Retrato: avatar e cards de host empilham na coluna da própria home.
       Paisagem: o hero continua ocupando a largura toda e os dois blocos
       dividem a faixa abaixo dele, lado a lado. */
    lv_coord_t usable = LV_HOR_RES - UI_DOCK_W - 2 * HOME_PAD_X;
    lv_obj_t *body = col;
    if (ui_landscape()) {
        body = ui_plain(col);
        lv_obj_set_size(body, LV_PCT(100), avatar_slot_h());
        lv_obj_set_flex_flow(body, LV_FLEX_FLOW_ROW);
        lv_obj_set_style_pad_column(body, HOME_GAP, 0);
    }

    lv_obj_t *avatar_slot = ui_plain(body);
    lv_obj_set_size(avatar_slot, avatar_slot_w(), avatar_slot_h());
    avatar_create(avatar_slot);

    build_host_cards(body, ui_landscape() ? usable - HOME_GAP - avatar_slot_w()
                                          : usable);
    ui_dock(s_home, UI_TAB_HOME, dock_cb);
}

static void refresh_clock(void)
{
    time_t now;
    time(&now);
    struct tm tm;
    localtime_r(&now, &tm);
    /* antes do SNTP responder o relógio começa em 1970; não mostrar hora falsa */
    if (tm.tm_year < 120) {
        s_last_minute = -1;
        return;
    }
    if (tm.tm_min == s_last_minute) {
        return;
    }
    s_last_minute = tm.tm_min;

    lv_label_set_text_fmt(s_clock, "%02d:%02d", tm.tm_hour, tm.tm_min);
    char date[24];
    i18n_format_date(date, sizeof(date), &tm);
    lv_label_set_text(s_date, date);
}

static void refresh_home(void)
{
    int working = 0;
    int blocked = 0;
    int done = 0;
    for (int i = 0; i < s_ui_agent_count; i++) {
        switch (status_rank(s_ui_agents[i].status)) {
        case 0: blocked++; break;
        case 1: done++;    break;
        case 2: working++; break;
        default: break;
        }
    }

    /* estado do avatar: sem host online = desconectado; senão o pior manda */
    const panel_cfg_t *cfg = panel_cfg_get();
    bool online = false;
    for (int h = 0; h < CFG_MAX_HOSTS; h++) {
        if (cfg->hosts[h].enabled && herdr_model_get_conn(h) == HERDR_CONN_ONLINE) {
            online = true;
            break;
        }
    }
    avatar_set_state(!online ? AVATAR_ST_DISCONNECTED :
                     blocked ? AVATAR_ST_BLOCKED :
                     done    ? AVATAR_ST_DONE :
                     working ? AVATAR_ST_WORKING : AVATAR_ST_IDLE);
    update_beacon(blocked);

    for (int w = 0; w < s_hw_count; w++) {
        host_widget_t *hw = &s_hw[w];
        herdr_conn_state_t conn = herdr_model_get_conn(hw->host);
        int n = 0;
        for (int i = 0; i < s_ui_agent_count && n < HEAT_CELLS; i++) {
            if (s_ui_agents[i].host == hw->host) {
                lv_obj_set_style_bg_color(hw->cells[n++],
                                          ui_status_color(s_ui_agents[i].status), 0);
            }
        }
        lv_color_t empty = conn == HERDR_CONN_ONLINE ? UI_CELL : UI_CELL_OFF;
        for (int c = n; c < HEAT_CELLS; c++) {
            lv_obj_set_style_bg_color(hw->cells[c], empty, 0);
        }
        if (conn == HERDR_CONN_ONLINE) {
            lv_label_set_text_fmt(hw->status, "%d %s", n,
                                  n == 1 ? T(STR_AGENT_ONE) : T(STR_AGENT_MANY));
            lv_obj_set_style_text_color(hw->status, UI_IDLE, 0);
        } else if (conn == HERDR_CONN_CONNECTING) {
            lv_label_set_text(hw->status, T(STR_CONNECTING));
            lv_obj_set_style_text_color(hw->status, UI_WORKING, 0);
        } else {
            lv_label_set_text(hw->status, T(STR_OFFLINE));
            lv_obj_set_style_text_color(hw->status, UI_BLOCKED, 0);
        }
    }
}

/* ---------- sessões ---------- */

static void build_sessions(void)
{
    s_sessions = ui_screen();

    lv_obj_t *bar = ui_topbar(s_sessions, T(STR_TAB_SESSIONS), NULL);
    /* em paisagem o dock fica em pé à esquerda: título e lista saem de trás dele */
    lv_obj_set_style_pad_left(bar, UI_TOPBAR_PAD + UI_DOCK_W, 0);
    s_lbl_group = lv_obj_get_child(ui_icon_btn(bar, "", toggle_group_cb, NULL), 0);
    s_lbl_sort = lv_obj_get_child(ui_icon_btn(bar, "", toggle_sort_cb, NULL), 0);
    update_toolbar_icons();

    s_sess_list = ui_plain(s_sessions);
    lv_obj_set_size(s_sess_list, LV_HOR_RES, LV_VER_RES - UI_TOPBAR_H);
    lv_obj_align(s_sess_list, LV_ALIGN_TOP_MID, 0, UI_TOPBAR_H);
    lv_obj_set_flex_flow(s_sess_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s_sess_list, 4, 0);
    lv_obj_set_style_pad_left(s_sess_list, UI_PAD + UI_DOCK_W, 0);
    lv_obj_set_style_pad_right(s_sess_list, UI_PAD, 0);
    lv_obj_set_style_pad_bottom(s_sess_list, UI_DOCK_H, 0);
    lv_obj_add_flag(s_sess_list, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(s_sess_list, LV_DIR_VER);

    ui_dock(s_sessions, UI_TAB_SESSIONS, dock_cb);
}

/** Escreve o tempo decorrido como mm:ss (h:mm:ss acima de uma hora). */
static void set_elapsed_text(lv_obj_t *label, time_t now, uint32_t since)
{
    /* antes do SNTP responder o relógio está em 1970 e a conta não vale nada;
       a mesma guarda cobre um since no futuro por desvio entre os relógios */
    if (now < (time_t)since) {
        lv_label_set_text(label, "");
        return;
    }
    uint32_t s = (uint32_t)(now - (time_t)since);
    /* %02u nos minutos: a Montserrat é proporcional e a largura fixa evita que
       o texto salte a cada mudança de dezena. Os casts são porque no xtensa
       uint32_t é unsigned long e o -Werror=format reclama. */
    if (s >= 3600) {
        lv_label_set_text_fmt(label, "%u:%02u:%02u", (unsigned)(s / 3600),
                              (unsigned)((s / 60) % 60), (unsigned)(s % 60));
    } else {
        lv_label_set_text_fmt(label, "%02u:%02u", (unsigned)(s / 60),
                              (unsigned)(s % 60));
    }
}

/* Cor do medidor de contexto por preenchimento (aviso de compactação). */
static lv_color_t ctx_color(uint8_t pct)
{
    if (pct >= 90) return UI_BLOCKED;   /* vai compactar */
    if (pct >= 70) return UI_WORKING;
    return UI_IDLE;
}

/* Largura útil da linha (coordenadas locais: já descontam o dock e as
 * margens do container-lista, como em DASH_BAR_W). */
#define SESS_ROW_W (LV_HOR_RES - UI_DOCK_W - 2 * UI_PAD)

static void add_session_row(int flat_idx, bool multi_acct)
{
    const herdr_agent_t *a = &s_ui_agents[flat_idx];
    lv_obj_t *row = lv_btn_create(s_sess_list);
    lv_obj_set_size(row, LV_PCT(100), UI_ROW_H);
    lv_obj_set_style_bg_color(row, UI_PANEL, 0);
    lv_obj_set_style_radius(row, 6, 0);
    lv_obj_set_style_shadow_width(row, 0, 0);
    lv_obj_add_event_cb(row, row_clicked_cb, LV_EVENT_CLICKED, (void *)(intptr_t)flat_idx);

    lv_obj_t *dot = lv_obj_create(row);
    lv_obj_set_size(dot, 14, 14);
    lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(dot, 0, 0);
    lv_obj_set_style_bg_color(dot, ui_status_color(a->status), 0);
    lv_obj_align(dot, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_clear_flag(dot, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *name = lv_label_create(row);
    lv_label_set_text(name, a->project);
    lv_obj_set_style_text_font(name, &lv_font_ui_16, 0);
    lv_obj_set_style_text_color(name, UI_TEXT, 0);
    /* corta o nome longo em vez de deixá-lo passar por baixo do cronômetro */
    lv_obj_set_width(name, LV_PCT(72));
    lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
    lv_obj_align(name, LV_ALIGN_LEFT_MID, 26, -8);

    /* Selo corp/pess (pill): fundo suave arredondado, só quando há mais de
       uma conta entre os agentes visíveis e esta linha conhece a sua org.
       Medido depois de montado (lv_obj_update_layout) para o sub encostar
       logo à direita dele, em vez de um offset de texto estimado à mão. */
    lv_coord_t sub_x = 26;
    if (multi_acct && a->account[0] && a->org[0]) {
        lv_obj_t *chip = lv_label_create(row);
        lv_label_set_text(chip, T(a->corp ? STR_ACCT_CORP : STR_ACCT_PERSONAL));
        lv_obj_set_style_text_font(chip, &lv_font_ui_12, 0);
        lv_obj_set_style_text_color(chip, a->corp ? UI_CORP : UI_MUTED, 0);
        lv_obj_set_style_bg_color(chip, a->corp ? UI_CORP : UI_MUTED, 0);
        lv_obj_set_style_bg_opa(chip, LV_OPA_20, 0);   /* fundo suave, pill discreto */
        lv_obj_set_style_radius(chip, 8, 0);
        lv_obj_set_style_pad_hor(chip, 5, 0);
        lv_obj_set_style_pad_ver(chip, 1, 0);
        lv_obj_align(chip, LV_ALIGN_LEFT_MID, 26, 11);
        lv_obj_update_layout(chip);
        sub_x = 26 + lv_obj_get_width(chip) + 6;
    }

    /* Sub-linha: conta multi-acct mostra org · modelo (selo corp/pess cobre o
       status via cor do dot); conta única cai no modelo, e sem modelo volta
       ao par agente/host de sempre — sem host quando a lista já está
       agrupada por ele. Largura reservada pro medidor de contexto (ou só a
       margem, sem medidor) à direita — nunca deixa o texto passar por baixo. */
    lv_obj_t *sub = lv_label_create(row);
    if (multi_acct && a->account[0]) {
        lv_label_set_text_fmt(sub, "%s \xC2\xB7 %s",
                              a->org[0] ? a->org : "conta",
                              a->model[0] ? a->model : a->agent);
    } else if (a->model[0]) {
        lv_label_set_text_fmt(sub, "%s \xC2\xB7 %s", a->model, i18n_status(a->status));
    } else if (s_group_by_host) {
        lv_label_set_text_fmt(sub, "%s \xC2\xB7 %s", a->agent, i18n_status(a->status));
    } else {
        lv_label_set_text_fmt(sub, "%s \xC2\xB7 %s \xC2\xB7 %s",
                              host_label(a->host), a->agent, i18n_status(a->status));
    }
    lv_obj_set_style_text_font(sub, &lv_font_ui_12, 0);
    lv_obj_set_style_text_color(sub, UI_MUTED, 0);
    lv_coord_t reserve_right = (a->context_pct != 255) ? 72 : UI_PAD;
    lv_obj_set_width(sub, SESS_ROW_W - sub_x - reserve_right);
    lv_label_set_long_mode(sub, LV_LABEL_LONG_DOT);
    lv_obj_align(sub, LV_ALIGN_LEFT_MID, sub_x, 11);

    /* Medidor de contexto: substitui o e-mail à direita (a org já foi para a
       sub-linha). Só aparece quando a ponte reportou um valor. */
    if (a->context_pct != 255) {
        lv_obj_t *ctxlab = lv_label_create(row);
        lv_label_set_text_fmt(ctxlab, "ctx %u%%", (unsigned)a->context_pct);
        lv_obj_set_style_text_font(ctxlab, &lv_font_ui_12, 0);
        lv_obj_set_style_text_color(ctxlab, UI_MUTED, 0);
        lv_obj_align(ctxlab, LV_ALIGN_RIGHT_MID, -4, 4);

        lv_obj_t *cbar = lv_bar_create(row);
        lv_obj_set_size(cbar, 60, 5);
        lv_obj_align(cbar, LV_ALIGN_RIGHT_MID, -4, 16);
        lv_obj_set_style_bg_color(cbar, UI_BORDER, LV_PART_MAIN);
        lv_obj_set_style_bg_opa(cbar, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_radius(cbar, 2, LV_PART_MAIN);
        lv_obj_set_style_bg_color(cbar, ctx_color(a->context_pct), LV_PART_INDICATOR);
        lv_obj_set_style_radius(cbar, 2, LV_PART_INDICATOR);
        lv_bar_set_range(cbar, 0, 100);
        lv_bar_set_value(cbar, a->context_pct, LV_ANIM_OFF);
    }

    /* Só quem está working ganha cronômetro. Mudar de status muda a geração e
       reconstrói a lista, então uma linha registrada aqui continua working até
       ser destruída — não há caso de virar working sem passar por aqui. */
    if (strcmp(a->status, "working") == 0 && a->since != 0 &&
        s_sess_timer_count < HERDR_MAX_AGENTS_TOTAL) {
        lv_obj_t *t = lv_label_create(row);
        set_elapsed_text(t, time(NULL), a->since);   /* já nasce com o valor certo */
        lv_obj_set_style_text_font(t, &lv_font_ui_12, 0);
        lv_obj_set_style_text_color(t, UI_MUTED, 0);
        lv_obj_align(t, LV_ALIGN_RIGHT_MID, -4, -8);   /* na altura do nome */
        s_sess_timers[s_sess_timer_count].label = t;
        s_sess_timers[s_sess_timer_count].since = a->since;
        s_sess_timer_count++;
    }
}

/**
 * Faz os cronômetros correrem sem depender da geração do modelo.
 *
 * O tick da UI é de 150ms, mas o texto só muda quando vira o segundo.
 */
static void refresh_session_timers(void)
{
    if (s_tab != UI_TAB_SESSIONS || s_detail_open || s_sess_timer_count == 0) {
        return;
    }
    time_t now = time(NULL);
    if (now == s_last_timer_sec) {
        return;
    }
    s_last_timer_sec = now;
    for (int i = 0; i < s_sess_timer_count; i++) {
        set_elapsed_text(s_sess_timers[i].label, now, s_sess_timers[i].since);
    }
}

/**
 * Preenche `out` com os índices dos agentes a exibir.
 *
 * host < 0 pega todos. No modo prioridade os bloqueados sobem, mantendo a
 * ordem original entre iguais (ordenação estável por seleção).
 */
static int collect_agents(int host, int *out, int max)
{
    int n = 0;
    for (int i = 0; i < s_ui_agent_count && n < max; i++) {
        if (host < 0 || s_ui_agents[i].host == host) {
            out[n++] = i;
        }
    }
    if (s_sort_priority) {
        for (int i = 1; i < n; i++) {
            int key = out[i];
            int rank = status_rank(s_ui_agents[key].status);
            int j = i - 1;
            while (j >= 0 && status_rank(s_ui_agents[out[j]].status) > rank) {
                out[j + 1] = out[j];
                j--;
            }
            out[j + 1] = key;
        }
    }
    return n;
}

static void add_section_label(const char *text, const char *right, lv_color_t right_color)
{
    lv_obj_t *sec = ui_plain(s_sess_list);
    lv_obj_set_size(sec, LV_PCT(100), 22);

    lv_obj_t *l = lv_label_create(sec);
    lv_label_set_text(l, text);
    lv_obj_set_style_text_font(l, &lv_font_ui_12, 0);
    lv_obj_set_style_text_color(l, UI_MUTED, 0);
    lv_obj_align(l, LV_ALIGN_LEFT_MID, 4, 0);

    if (right) {
        lv_obj_t *r = lv_label_create(sec);
        lv_label_set_text(r, right);
        lv_obj_set_style_text_font(r, &lv_font_ui_12, 0);
        lv_obj_set_style_text_color(r, right_color, 0);
        lv_obj_align(r, LV_ALIGN_RIGHT_MID, -4, 0);
    }
}

static void add_muted_line(const char *text)
{
    lv_obj_t *l = lv_label_create(s_sess_list);
    lv_label_set_text(l, text);
    lv_obj_set_style_text_font(l, &lv_font_ui_12, 0);
    lv_obj_set_style_text_color(l, UI_MUTED, 0);
}

static void rebuild_session_rows(void)
{
    lv_obj_clean(s_sess_list);
    /* os labels foram destruídos junto com as linhas; add_session_row reabastece */
    s_sess_timer_count = 0;
    s_last_timer_sec = 0;
    const panel_cfg_t *cfg = panel_cfg_get();
    int order[HERDR_MAX_AGENTS_TOTAL];

    /* multi_acct: há >1 conta distinta entre os agentes visíveis? calculado
       uma vez ao reconstruir a lista, análogo ao multi da Dash. Contas vazias
       (desconhecidas) não contam para a comparação. */
    bool multi_acct = false;
    {
        const char *first = NULL;
        for (int i = 0; i < s_ui_agent_count; i++) {
            if (!s_ui_agents[i].account[0]) {
                continue;
            }
            if (!first) {
                first = s_ui_agents[i].account;
            } else if (strcmp(s_ui_agents[i].account, first) != 0) {
                multi_acct = true;
                break;
            }
        }
    }

    int enabled = 0;
    for (int h = 0; h < CFG_MAX_HOSTS; h++) {
        enabled += cfg->hosts[h].enabled;
    }
    if (enabled == 0) {
        add_muted_line(T(STR_NO_HOSTS));
        add_muted_line(T(STR_TAP_TO_ADD));
        return;
    }

    if (!s_group_by_host) {
        int n = collect_agents(-1, order, HERDR_MAX_AGENTS_TOTAL);
        if (n == 0) {
            add_muted_line(T(STR_NO_SESSIONS));
        }
        for (int i = 0; i < n; i++) {
            add_session_row(order[i], multi_acct);
        }
        return;
    }

    for (int h = 0; h < CFG_MAX_HOSTS; h++) {
        if (!cfg->hosts[h].enabled) {
            continue;
        }
        herdr_conn_state_t conn = herdr_model_get_conn(h);
        add_section_label(host_label(h),
                          conn == HERDR_CONN_ONLINE ? T(STR_ONLINE) :
                          conn == HERDR_CONN_CONNECTING ? T(STR_CONNECTING) : T(STR_OFFLINE),
                          conn == HERDR_CONN_ONLINE ? UI_IDLE :
                          conn == HERDR_CONN_CONNECTING ? UI_WORKING : UI_BLOCKED);
        int n = collect_agents(h, order, HERDR_MAX_AGENTS_TOTAL);
        for (int i = 0; i < n; i++) {
            add_session_row(order[i], multi_acct);
        }
        if (n == 0) {
            add_muted_line(conn == HERDR_CONN_ONLINE ? T(STR_NO_SESSIONS)
                                                     : T(STR_WAITING_BRIDGE));
        }
    }
}

/* ---------- dash ---------- */

/* Medidas da tela "Dashboards" no Claude Design (projeto herdr-assist). */
#define DASH_PAD_TOP    10  /* padding superior do card */
#define DASH_PAD_X      12  /* padding lateral do card */
#define DASH_PAD_BOTTOM 12
/* Slot quadrado do logo: cobre as DUAS linhas do cabeçalho (título em ui_14,
   line_height 18, + 3 de respiro + e-mail em ui_12, line_height 16). Era 26,
   da época em que o cabeçalho tinha uma linha só. */
#define DASH_LOGO       37
#define DASH_TXT_H      17  /* altura da linha de texto de um limite */
#define DASH_BAR_H      12
/* margem 11 + texto + respiro 6 + barra: passo de um bloco de limite */
#define DASH_ROW_BLOCK  (11 + DASH_TXT_H + 6 + DASH_BAR_H)
/* onde o primeiro bloco começa, logo abaixo do cabeçalho do card */
#define DASH_ROWS_Y     (DASH_PAD_TOP + DASH_LOGO)
/* altura de uma linha do card de custo: valor em bold_16 (19px) + respiro */
#define COST_ROW_H      22
#define DASH_BAR_W      (LV_HOR_RES - UI_DOCK_W - 2 * UI_PAD - 2 * DASH_PAD_X)
/* largura do título (nome/host/conta) até o slot do plano, para o e-mail
   longo truncar em vez de invadir o canto direito do card */
#define DASH_NAME_MAX_W (LV_HOR_RES - DASH_LOGO - 4 * DASH_PAD_X - 60)
/* Linha do e-mail: a coluna do texto inteira, já que o plano fica na linha de
   cima. Derivada como DASH_BAR_W (conta o dock e o pad da lista), ao contrário
   de DASH_NAME_MAX_W, que é do tempo do cabeçalho de uma linha só. Dá 246px em
   retrato — "wallacysferreira@gmail.com" ocupa 171. */
#define DASH_ACCT_MAX_W (LV_HOR_RES - UI_DOCK_W - 2 * UI_PAD - 2 * DASH_PAD_X \
                         - DASH_LOGO - 8)
/* Título: a coluna do texto menos o plano à direita ("Max 20x" ocupa 50px na
   ui_12) e o respiro. Sem esse teto o label passaria por baixo do plano. */
#define DASH_TITLE_MAX_W (LV_HOR_RES - UI_DOCK_W - 2 * UI_PAD - 2 * DASH_PAD_X \
                          - DASH_LOGO - 8 - 58)

static void build_dash(void)
{
    s_dash = ui_screen();
    lv_obj_t *bar = ui_topbar(s_dash, T(STR_TAB_DASH), NULL);
    lv_obj_set_style_pad_left(bar, UI_TOPBAR_PAD + UI_DOCK_W, 0);

    s_dash_list = ui_plain(s_dash);
    lv_obj_set_size(s_dash_list, LV_HOR_RES, LV_VER_RES - UI_TOPBAR_H);
    lv_obj_align(s_dash_list, LV_ALIGN_TOP_MID, 0, UI_TOPBAR_H);
    lv_obj_set_flex_flow(s_dash_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s_dash_list, 8, 0);
    lv_obj_set_style_pad_left(s_dash_list, UI_PAD + UI_DOCK_W, 0);
    lv_obj_set_style_pad_right(s_dash_list, UI_PAD, 0);
    lv_obj_set_style_pad_bottom(s_dash_list, UI_DOCK_H, 0);
    lv_obj_add_flag(s_dash_list, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(s_dash_list, LV_DIR_VER);

    ui_dock(s_dash, UI_TAB_DASH, dock_cb);
}

/* ---------- aba Atividade ---------- */

static void build_activity(void)
{
    s_activity = ui_screen();
    lv_obj_t *bar = ui_topbar(s_activity, T(STR_TAB_ACTIVITY), NULL);
    lv_obj_set_style_pad_left(bar, UI_TOPBAR_PAD + UI_DOCK_W, 0);

    s_activity_list = ui_plain(s_activity);
    lv_obj_set_size(s_activity_list, LV_HOR_RES, LV_VER_RES - UI_TOPBAR_H);
    lv_obj_align(s_activity_list, LV_ALIGN_TOP_MID, 0, UI_TOPBAR_H);
    lv_obj_set_flex_flow(s_activity_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s_activity_list, 6, 0);
    lv_obj_set_style_pad_left(s_activity_list, UI_PAD + UI_DOCK_W, 0);
    lv_obj_set_style_pad_right(s_activity_list, UI_PAD, 0);
    lv_obj_set_style_pad_top(s_activity_list, 6, 0);
    lv_obj_set_style_pad_bottom(s_activity_list, UI_DOCK_H, 0);
    lv_obj_add_flag(s_activity_list, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(s_activity_list, LV_DIR_VER);

    ui_dock(s_activity, UI_TAB_ACTIVITY, dock_cb);
}

static void fmt_dur(char *buf, size_t size, uint32_t sec)
{
    if (sec == 0) {
        buf[0] = '\0';
    } else if (sec < 60) {
        snprintf(buf, size, " (%us)", (unsigned)sec);
    } else if (sec < 3600) {
        snprintf(buf, size, " (%umin)", (unsigned)(sec / 60));
    } else {
        snprintf(buf, size, " (%uh%02u)", (unsigned)(sec / 3600),
                 (unsigned)((sec % 3600) / 60));
    }
}

static void rebuild_activity(void)
{
    lv_obj_clean(s_activity_list);

    /* estático: só a task da LVGL mexe, e 64 eventos são ~3 KB fora da stack */
    static activity_event_t ev[ACTIVITY_CAP];
    int n = activity_log_get(ev, ACTIVITY_CAP);
    if (n == 0) {
        lv_obj_t *empty = lv_label_create(s_activity_list);
        lv_label_set_text(empty, T(STR_NO_ACTIVITY));
        lv_obj_set_style_text_font(empty, &lv_font_ui_14, 0);
        lv_obj_set_style_text_color(empty, UI_MUTED, 0);
        return;
    }

    time_t now = time(NULL);

    /* cabeçalho: totais do dia (desde a meia-noite local) */
    struct tm tmd;
    localtime_r(&now, &tmd);
    tmd.tm_hour = 0;
    tmd.tm_min = 0;
    tmd.tm_sec = 0;
    uint32_t day_start = (uint32_t)mktime(&tmd);
    activity_summary_t sm = activity_summarize(ev, n, day_start);
    char act[24];
    if (sm.active_secs == 0) {
        act[0] = '\0';
    } else if (sm.active_secs < 3600) {
        snprintf(act, sizeof(act), "  \xC2\xB7  %umin", (unsigned)(sm.active_secs / 60));
    } else {
        snprintf(act, sizeof(act), "  \xC2\xB7  %uh%02u", (unsigned)(sm.active_secs / 3600),
                 (unsigned)((sm.active_secs % 3600) / 60));
    }
    lv_obj_t *sum = lv_label_create(s_activity_list);
    lv_label_set_text_fmt(sum, "%s   \xE2\x97\x8F%u   " LV_SYMBOL_BELL "%u   " LV_SYMBOL_OK "%u%s",
                          T(STR_TODAY), sm.started, sm.blocked, sm.done, act);
    lv_obj_set_style_text_font(sum, &lv_font_ui_bold_16, 0);
    lv_obj_set_style_text_color(sum, UI_TEXT, 0);
    lv_obj_set_style_pad_bottom(sum, 4, 0);

    for (int i = 0; i < n; i++) {
        const activity_event_t *e = &ev[i];
        const char *icon;
        const char *verb;
        lv_color_t color;
        switch (e->type) {
        case ACT_BLOCKED:
            icon = LV_SYMBOL_BELL; color = UI_BLOCKED; verb = T(STR_EV_BLOCKED);
            break;
        case ACT_DONE:
            icon = LV_SYMBOL_OK;   color = UI_IDLE;    verb = T(STR_EV_DONE);
            break;
        default:
            icon = "\xE2\x97\x8F"; color = UI_WORKING; verb = T(STR_EV_STARTED);
            break;
        }
        char when[16];
        char dur[16];
        fmt_when(when, sizeof(when), now, e->epoch);
        fmt_dur(dur, sizeof(dur), e->dur);

        lv_obj_t *row = lv_label_create(s_activity_list);
        lv_label_set_text_fmt(row, "%s  %s  %s \xC2\xB7 %s %s%s",
                              icon, when, e->agent, e->project, verb, dur);
        lv_obj_set_style_text_font(row, &lv_font_ui_14, 0);
        lv_obj_set_style_text_color(row, color, 0);
        lv_obj_set_width(row, lv_pct(100));
        lv_label_set_long_mode(row, LV_LABEL_LONG_DOT);
    }
}

/** Cor da barra por nível de uso: verde, âmbar, laranja, vermelho. */
static lv_color_t limit_color(uint8_t pct)
{
    if (pct < 50) return UI_IDLE;
    if (pct < 65) return UI_WORKING;
    if (pct < 80) return UI_LIMIT_HIGH;
    return UI_BLOCKED;
}

/**
 * Escreve um instante de forma curta: hora se a menos de 24h, senão o dia da
 * semana. Devolve false (e buf vazio) sem SNTP sincronizado ou sem instante —
 * melhor omitir do que mostrar hora calculada sobre relógio de 1970.
 */
static bool fmt_when(char *buf, size_t size, time_t now, uint32_t t)
{
    struct tm tm;
    localtime_r(&now, &tm);
    if (tm.tm_year < 120 || t == 0) {
        buf[0] = '\0';
        return false;
    }
    time_t tt = (time_t)t;
    struct tm wtm;
    localtime_r(&tt, &wtm);
    long diff = (long)(tt > now ? tt - now : now - tt);
    if (diff < 24 * 3600) {
        snprintf(buf, size, "%02d:%02d", wtm.tm_hour, wtm.tm_min);
    } else {
        snprintf(buf, size, "%s", i18n_weekday(wtm.tm_wday));
    }
    return true;
}

/** Logo do provedor, ou NULL para um nome que ainda não tem arte. */
static const lv_img_dsc_t *provider_logo(const char *name)
{
    if (strcmp(name, "Claude") == 0) return &logo_claude;
    if (strcmp(name, "Codex") == 0)  return &logo_codex;
    return NULL;
}

static void add_limits_card(const herdr_limits_t *l, bool show_host)
{
    time_t now = time(NULL);
    int n = l->row_count;
    /* Cabeçalho de duas linhas quando a conta precisa aparecer: título e
       contagem em cima, e-mail e plano embaixo. O e-mail inteiro cabe nessa
       linha própria, que é o que o pill + org nunca conseguiram na linha do
       título — com o host no nome o orçamento ficava negativo. */
    bool two_line = l->account[0] != '\0';
    /* altura fixa por fórmula: LV_SIZE_CONTENT com filhos alinhados é armadilha
       no 8.4. A linha de "sem atualizar" cobra a margem de 8 mais a própria
       altura de texto. */
    lv_coord_t h = (lv_coord_t)(DASH_ROWS_Y + n * DASH_ROW_BLOCK +
                                (l->ok ? 0 : 8 + 14) + DASH_PAD_BOTTOM);

    lv_obj_t *card = ui_card(s_dash_list, 10);
    lv_obj_set_size(card, LV_PCT(100), h);

    /* slot do logo: mesmo fundo da tela, para o ícone ficar num recorte */
    lv_obj_t *slot = lv_obj_create(card);
    lv_obj_set_size(slot, DASH_LOGO, DASH_LOGO);
    lv_obj_set_style_radius(slot, 7, 0);
    lv_obj_set_style_bg_color(slot, UI_BG, 0);
    lv_obj_set_style_border_width(slot, 1, 0);
    lv_obj_set_style_border_color(slot, UI_BORDER, 0);
    lv_obj_set_style_shadow_width(slot, 0, 0);
    lv_obj_set_style_pad_all(slot, 0, 0);
    lv_obj_clear_flag(slot, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(slot, LV_ALIGN_TOP_LEFT, DASH_PAD_X, DASH_PAD_TOP);

    const lv_img_dsc_t *logo = provider_logo(l->name);
    if (logo) {
        lv_obj_t *img = lv_img_create(slot);
        lv_img_set_src(img, logo);
        lv_obj_center(img);
    }

    lv_obj_t *name = lv_label_create(card);
    if (show_host) {
        lv_label_set_text_fmt(name, "%s \xC2\xB7 %s", host_label(l->host), l->name);
    } else {
        lv_label_set_text(name, l->name);
    }
    lv_obj_set_style_text_font(name, &lv_font_ui_14, 0);
    lv_obj_set_style_text_color(name, UI_TEXT, 0);
    lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
    lv_obj_set_width(name, DASH_TITLE_MAX_W);
    /* com duas linhas o título sobe para o topo do slot; com uma, fica
       centralizado nele como sempre */
    lv_obj_align_to(name, slot,
                    two_line ? LV_ALIGN_OUT_RIGHT_TOP : LV_ALIGN_OUT_RIGHT_MID,
                    8, 0);

    if (two_line) {
        lv_obj_t *acct = lv_label_create(card);
        lv_label_set_text(acct, l->account);
        lv_obj_set_style_text_font(acct, &lv_font_ui_12, 0);
        lv_obj_set_style_text_color(acct, UI_MUTED, 0);
        /* e-mail fora do comum ainda trunca no fim, mas na linha inteira isso
           só acontece passando de ~34 caracteres */
        lv_label_set_long_mode(acct, LV_LABEL_LONG_DOT);
        lv_obj_set_width(acct, DASH_ACCT_MAX_W);
        lv_obj_align_to(acct, name, LV_ALIGN_OUT_BOTTOM_LEFT, 0, 3);
    }

    if (l->plan[0]) {
        lv_obj_t *plan = lv_label_create(card);
        lv_label_set_text(plan, l->plan);
        lv_obj_set_style_text_font(plan, &lv_font_ui_12, 0);
        lv_obj_set_style_text_color(plan, UI_MUTED, 0);
        /* na linha do título: com duas linhas acompanha o topo, com uma fica
           centralizado na altura do slot (align-items:center do design) */
        lv_obj_align(plan, LV_ALIGN_TOP_RIGHT, -DASH_PAD_X,
                     two_line ? DASH_PAD_TOP + 2
                              : DASH_PAD_TOP + (DASH_LOGO - 14) / 2);
    }

    for (int i = 0; i < n; i++) {
        const herdr_limit_row_t *r = &l->rows[i];
        lv_coord_t y = (lv_coord_t)(DASH_ROWS_Y + 11 + i * DASH_ROW_BLOCK);

        /* consumo esperado: onde a barra estaria no ritmo linear da janela */
        bool has_window = (r->window_s > 0 && (time_t)r->resets_at > now);
        int expected = 0;
        if (has_window) {
            uint32_t remaining = (uint32_t)((time_t)r->resets_at - now);
            if (remaining > r->window_s) remaining = r->window_s;
            expected = (int)(((uint32_t)(r->window_s - remaining) * 100u) / r->window_s);
        }

        lv_obj_t *lab = lv_label_create(card);
        lv_label_set_text(lab, r->label);
        lv_obj_set_style_text_font(lab, &lv_font_ui_14, 0);
        lv_obj_set_style_text_color(lab, UI_TEXT, 0);
        lv_obj_align(lab, LV_ALIGN_TOP_LEFT, DASH_PAD_X, y);

        /* o reset ancora na direita e o percentual encosta nele: são fontes e
           cores diferentes, então não cabem num label só */
        char when[12];
        lv_obj_t *rst = NULL;
        if (fmt_when(when, sizeof(when), now, r->resets_at) && (time_t)r->resets_at > now) {
            rst = lv_label_create(card);
            lv_label_set_text_fmt(rst, LV_SYMBOL_RIGHT " %s", when);
            lv_obj_set_style_text_font(rst, &lv_font_ui_12, 0);
            lv_obj_set_style_text_color(rst, UI_MUTED, 0);
            lv_obj_align(rst, LV_ALIGN_TOP_RIGHT, -DASH_PAD_X, y + 2);
        }
        lv_obj_t *val = lv_label_create(card);
        lv_label_set_text_fmt(val, "%u%%", (unsigned)r->pct);
        lv_obj_set_style_text_font(val, &lv_font_ui_14, 0);
        lv_obj_set_style_text_color(val, UI_TEXT, 0);
        if (rst) {
            lv_obj_align_to(val, rst, LV_ALIGN_OUT_LEFT_MID, -5, -1);
        } else {
            lv_obj_align(val, LV_ALIGN_TOP_RIGHT, -DASH_PAD_X, y);
        }

        /* A cor é o CALOR: quanto do teto já foi. O ritmo é a sombra atrás,
           então a cor não precisa mais dizer as duas coisas — antes ela ficava
           verde a 58% só porque a janela ainda tinha folga, escondendo que o
           consumo já passava da metade. */
        lv_color_t fill = limit_color(r->pct);
        lv_coord_t bar_y = (lv_coord_t)(y + DASH_TXT_H + 6);

        /* Duas barras sobrepostas. A de trás desenha a trilha e, no mesmo tom
           da da frente porém esmaecido, até onde o consumo estaria no ritmo
           linear da janela — a sombra do esperado. A da frente é o consumo
           real, com a trilha transparente para não tapar a sombra.
           Um filho do bar não serviria: a LVGL desenha os filhos depois de
           todas as partes, então ele ficaria POR CIMA do indicador. */
        lv_obj_t *ghost = lv_bar_create(card);
        lv_obj_set_size(ghost, DASH_BAR_W, DASH_BAR_H);
        lv_obj_align(ghost, LV_ALIGN_TOP_MID, 0, bar_y);
        lv_obj_set_style_bg_color(ghost, UI_BORDER, LV_PART_MAIN);
        lv_obj_set_style_bg_opa(ghost, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_radius(ghost, DASH_BAR_H / 2, LV_PART_MAIN);
        lv_obj_set_style_bg_color(ghost, fill, LV_PART_INDICATOR);
        lv_obj_set_style_bg_opa(ghost, LV_OPA_40, LV_PART_INDICATOR);
        lv_obj_set_style_radius(ghost, DASH_BAR_H / 2, LV_PART_INDICATOR);
        lv_bar_set_range(ghost, 0, 100);
        /* sem janela conhecida não há esperado: fica só a trilha, como antes */
        lv_bar_set_value(ghost, has_window ? expected : 0, LV_ANIM_OFF);

        lv_obj_t *bar = lv_bar_create(card);
        lv_obj_set_size(bar, DASH_BAR_W, DASH_BAR_H);
        lv_obj_align(bar, LV_ALIGN_TOP_MID, 0, bar_y);
        lv_obj_set_style_bg_opa(bar, LV_OPA_TRANSP, LV_PART_MAIN);
        /* sem sobrescrever, o tema default pinta o indicador na cor primária
           e anima cada mudança de valor */
        lv_obj_set_style_bg_color(bar, fill, LV_PART_INDICATOR);
        lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_INDICATOR);
        lv_obj_set_style_radius(bar, DASH_BAR_H / 2, LV_PART_INDICATOR);
        lv_bar_set_range(bar, 0, 100);
        lv_bar_set_value(bar, r->pct, LV_ANIM_OFF);
    }

    if (!l->ok) {
        /* âmbar, não muted: o estado degradado é justamente o que precisa
           aparecer */
        char when[12];
        lv_obj_t *st = lv_label_create(card);
        if (fmt_when(when, sizeof(when), now, l->stale_since)) {
            lv_label_set_text_fmt(st, T(STR_STALE_SINCE), when);
        } else {
            lv_label_set_text(st, T(STR_STALE));
        }
        lv_obj_set_style_text_font(st, &lv_font_ui_12, 0);
        lv_obj_set_style_text_color(st, UI_WORKING, 0);
        lv_obj_align(st, LV_ALIGN_TOP_LEFT, DASH_PAD_X,
                     (lv_coord_t)(DASH_ROWS_Y + n * DASH_ROW_BLOCK + 8));
    }
}

static void rebuild_dash_cards(void)
{
    lv_obj_clean(s_dash_list);

    /* card de custo: só aparece depois do primeiro payload "cost" da ponte
       (versões antigas da ponte nunca mandam, então o card some de vez) */
    herdr_cost_t cost;
    if (herdr_model_get_cost(&cost)) {
        /* mesma anatomia dos cards de conta: slot de logo à esquerda, título na
           mesma linha, e as linhas de valor começando abaixo do slot */
        lv_obj_t *cc = ui_card(s_dash_list, 10);
        lv_obj_set_size(cc, LV_PCT(100), (lv_coord_t)(DASH_ROWS_Y + 3 * COST_ROW_H
                                                      + DASH_PAD_BOTTOM));

        lv_obj_t *slot = lv_obj_create(cc);
        lv_obj_set_size(slot, DASH_LOGO, DASH_LOGO);
        lv_obj_set_style_radius(slot, 7, 0);
        lv_obj_set_style_bg_color(slot, UI_BG, 0);
        lv_obj_set_style_border_width(slot, 1, 0);
        lv_obj_set_style_border_color(slot, UI_BORDER, 0);
        lv_obj_set_style_shadow_width(slot, 0, 0);
        lv_obj_set_style_pad_all(slot, 0, 0);
        lv_obj_clear_flag(slot, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_align(slot, LV_ALIGN_TOP_LEFT, DASH_PAD_X, DASH_PAD_TOP);
        /* não há logo de custo: o cifrão é o símbolo, e sai da própria fonte */
        lv_obj_t *sym = lv_label_create(slot);
        lv_label_set_text(sym, "$");
        lv_obj_set_style_text_font(sym, &lv_font_ui_bold_20, 0);
        lv_obj_set_style_text_color(sym, UI_MUTED, 0);
        lv_obj_center(sym);

        lv_obj_t *title = lv_label_create(cc);
        lv_label_set_text(title, T(STR_COST_TITLE));   /* "Custo · estimativa" */
        lv_obj_set_style_text_font(title, &lv_font_ui_14, 0);
        lv_obj_set_style_text_color(title, UI_TEXT, 0);
        lv_obj_align_to(title, slot, LV_ALIGN_OUT_RIGHT_MID, 8, 0);

        const char *labs[3] = {T(STR_COST_NOW), T(STR_COST_WEEK), T(STR_COST_LIFE)};
        const uint32_t cents[3] = {cost.now_cents, cost.week_cents, cost.life_cents};
        const char *legacy[3] = {cost.now, cost.week, cost.life};
        for (int i = 0; i < 3; i++) {
            lv_obj_t *l = lv_label_create(cc);
            lv_label_set_text(l, labs[i]);
            lv_obj_set_style_text_font(l, &lv_font_ui_12, 0);
            lv_obj_set_style_text_color(l, UI_MUTED, 0);
            lv_obj_align(l, LV_ALIGN_TOP_LEFT, DASH_PAD_X,
                         (lv_coord_t)(DASH_ROWS_Y + i * COST_ROW_H + 4));

            /* centavos da ponte nova: formatar aqui, no idioma do painel.
               Sem eles, exibir a string que a ponte antiga já mandou pronta. */
            char money[16];
            if (cost.has_cents) {
                money_fmt(money, sizeof(money), cents[i], i18n_lang() == LANG_PT);
            } else {
                snprintf(money, sizeof(money), "%s", legacy[i]);
            }
            lv_obj_t *v = lv_label_create(cc);
            lv_label_set_text(v, money[0] ? money : "—");
            /* o valor é o conteúdo da linha: peso maior que o do rótulo */
            lv_obj_set_style_text_font(v, &lv_font_ui_bold_16, 0);
            lv_obj_set_style_text_color(v, UI_TEXT, 0);
            lv_obj_align(v, LV_ALIGN_TOP_RIGHT, -DASH_PAD_X,
                         (lv_coord_t)(DASH_ROWS_Y + i * COST_ROW_H));
        }
    }

    s_ui_limit_count = herdr_model_get_limits(
        s_ui_limits, HERDR_MAX_PROVIDERS * CFG_MAX_HOSTS);
    if (s_ui_limit_count == 0) {
        lv_obj_t *l = lv_label_create(s_dash_list);
        lv_label_set_text(l, T(STR_NO_USAGE));
        lv_obj_set_style_text_font(l, &lv_font_ui_12, 0);
        lv_obj_set_style_text_color(l, UI_MUTED, 0);
        return;
    }
    /* com um host os cards falam por si; com mais, o título ganha o host.
       Começa no 0 porque o próprio primeiro card pode ser mesclado — sem o
       shared, Claude em duas máquinas e Codex em uma reportariam todos host 0
       e o Codex perderia o prefixo que merece. */
    bool multi = false;
    for (int i = 0; i < s_ui_limit_count; i++) {
        multi |= s_ui_limits[i].shared ||
                 s_ui_limits[i].host != s_ui_limits[0].host;
    }
    for (int i = 0; i < s_ui_limit_count; i++) {
        /* A conta vai em todo card que a conhece: com linha própria o e-mail
           não disputa espaço com o título, então não há por que escondê-lo
           quando só existe uma conta — ele diz de quem é o consumo.
           Card mesclado é da CONTA, não de uma máquina: sem prefixo de host. */
        add_limits_card(&s_ui_limits[i], multi && !s_ui_limits[i].shared);
    }
}

/* ---------- detalhe ---------- */

static lv_obj_t *make_action_btn(lv_obj_t *parent, const char *label,
                                 lv_event_cb_t cb, void *user_data)
{
    lv_obj_t *btn = lv_btn_create(parent);
    lv_obj_set_flex_grow(btn, 1);
    lv_obj_set_height(btn, ACTION_BAR_H - 8);
    lv_obj_set_style_bg_color(btn, UI_BG, 0);
    lv_obj_set_style_radius(btn, 6, 0);
    lv_obj_set_style_shadow_width(btn, 0, 0);
    lv_obj_set_style_pad_all(btn, 0, 0);
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, user_data);
    lv_obj_t *l = lv_label_create(btn);
    lv_label_set_text(l, label);
    lv_obj_set_style_text_font(l, &lv_font_ui_14, 0);
    lv_obj_set_style_text_color(l, UI_TEXT, 0);
    lv_obj_center(l);
    return btn;
}

static void build_detail(void)
{
    s_detail = ui_screen();

    lv_obj_t *bar = ui_topbar(s_detail, NULL, NULL);
    ui_icon_btn(bar, LV_SYMBOL_LEFT, back_clicked_cb, NULL);

    s_detail_title = lv_label_create(bar);
    lv_label_set_text(s_detail_title, "");
    lv_obj_set_style_text_font(s_detail_title, &lv_font_ui_bold_20, 0);
    lv_obj_set_style_text_color(s_detail_title, UI_TEXT, 0);
    lv_label_set_long_mode(s_detail_title, LV_LABEL_LONG_DOT);
    lv_obj_set_flex_grow(s_detail_title, 1);

    s_detail_dot = lv_obj_create(bar);
    lv_obj_set_size(s_detail_dot, 12, 12);
    lv_obj_set_style_radius(s_detail_dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(s_detail_dot, 0, 0);
    lv_obj_clear_flag(s_detail_dot, LV_OBJ_FLAG_SCROLLABLE);

    s_term_cont = lv_obj_create(s_detail);
    lv_obj_set_size(s_term_cont, LV_HOR_RES, LV_VER_RES - UI_TOPBAR_H - ACTION_BAR_H);
    lv_obj_align(s_term_cont, LV_ALIGN_TOP_MID, 0, UI_TOPBAR_H);
    lv_obj_set_style_bg_color(s_term_cont, UI_TERM_BG, 0);
    lv_obj_set_style_border_width(s_term_cont, 0, 0);
    lv_obj_set_style_radius(s_term_cont, 0, 0);
    lv_obj_set_style_pad_all(s_term_cont, 8, 0);

    s_term_view = term_view_create(s_term_cont);
    term_view_set_scroll_cb(s_term_view, term_scrolled_cb);

    lv_obj_t *actions = ui_plain(s_detail);
    lv_obj_set_size(actions, LV_HOR_RES, ACTION_BAR_H);
    lv_obj_align(actions, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_opa(actions, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(actions, UI_PANEL, 0);
    lv_obj_set_flex_flow(actions, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_all(actions, 4, 0);
    lv_obj_set_style_pad_column(actions, 4, 0);

    make_action_btn(actions, "Esc", action_key_cb, "Escape");
    make_action_btn(actions, LV_SYMBOL_UP, action_key_cb, "Up");
    make_action_btn(actions, LV_SYMBOL_DOWN, action_key_cb, "Down");
    make_action_btn(actions, LV_SYMBOL_NEW_LINE, action_key_cb, "Enter");
    make_action_btn(actions, "C-c", action_key_cb, "C-c");
    make_action_btn(actions, LV_SYMBOL_EYE_OPEN, action_focus_cb, NULL);
    make_action_btn(actions, LV_SYMBOL_KEYBOARD, action_prompt_cb, NULL);
}

static void refresh_detail(void)
{
    if (!s_detail_open) {
        return;
    }
    for (int i = 0; i < s_ui_agent_count; i++) {
        if (s_ui_agents[i].host == s_detail_host &&
            strcmp(s_ui_agents[i].pane_id, s_detail_pane) == 0) {
            lv_obj_set_style_bg_color(s_detail_dot, ui_status_color(s_ui_agents[i].status), 0);
            break;
        }
    }
    /* buffer grande fora da pilha (task da LVGL tem 8KB): PSRAM, alocado 1x.
       Seguro porque esta função roda exclusivamente na task da LVGL. */
    static char *raw;
    if (!raw) {
        raw = heap_caps_malloc(HERDR_CONTENT_LEN, MALLOC_CAP_SPIRAM);
        if (!raw) {
            raw = malloc(HERDR_CONTENT_LEN);
        }
        if (!raw) {
            return;
        }
    }
    static uint32_t seq;
    char pane_id[HERDR_ID_LEN];
    int host;
    /* o dedup mora no model (seq): snapshot repetido nem chega aqui */
    if (herdr_model_get_pane_content(pane_id, sizeof(pane_id), &host,
                                     raw, HERDR_CONTENT_LEN, &seq) &&
        host == s_detail_host &&
        strcmp(pane_id, s_detail_pane) == 0) {
        term_view_set_ansi(s_term_view, raw);
    }
}

/* ---------- teclado ---------- */

static void build_keyboard_overlay(void)
{
    s_kb_overlay = ui_screen();

    s_kb_ta = lv_textarea_create(s_kb_overlay);
    lv_obj_set_size(s_kb_ta, LV_HOR_RES - 16, 76);
    lv_obj_align(s_kb_ta, LV_ALIGN_TOP_MID, 0, 8);
    lv_textarea_set_placeholder_text(s_kb_ta, T(STR_PROMPT_PH));
    lv_obj_set_style_text_font(s_kb_ta, &lv_font_ui_14, 0);
    lv_obj_set_style_bg_color(s_kb_ta, UI_PANEL, 0);
    lv_obj_set_style_border_color(s_kb_ta, UI_BORDER, 0);

    s_keyboard = lv_keyboard_create(s_kb_overlay);
    lv_obj_set_size(s_keyboard, LV_HOR_RES, LV_VER_RES / 2);
    lv_obj_set_style_text_font(s_keyboard, &lv_font_ui_16, 0);
    herdr_kb_setup(s_keyboard);
    lv_keyboard_set_textarea(s_keyboard, s_kb_ta);
    lv_obj_add_event_cb(s_keyboard, kb_event_cb, LV_EVENT_READY, NULL);
    lv_obj_add_event_cb(s_keyboard, kb_event_cb, LV_EVENT_CANCEL, NULL);
}

/* ---------- laço da UI ---------- */

static void ui_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    refresh_clock();
    /* antes do early-out por geração: o cronômetro corre com o relógio, não
       com as mudanças de estado, e esse return é o caminho da maioria dos ticks */
    refresh_session_timers();
    refresh_lock_icon();   /* o prazo vence pelo tempo, não pelo modelo */

    if (s_detail_open && ++s_poll_tick >= DETAIL_POLL_TICKS) {
        s_poll_tick = 0;
        /* a geometria vai junto: a ponte trava a sessão no tamanho da tela, e
           as 40 linhas lidas dão o histórico rolável acima dela */
        int cols, rows;
        term_view_fit(s_term_view, &cols, &rows);
        herdr_conn_read_pane(s_detail_host, s_detail_pane, 40, cols, rows);
    }

    uint32_t gen = herdr_model_generation();
    if (gen == s_last_generation) {
        return;
    }
    s_last_generation = gen;

    s_ui_agent_count = herdr_model_get_agents(s_ui_agents, HERDR_MAX_AGENTS_TOTAL);
    refresh_home();
    if (s_tab == UI_TAB_SESSIONS && !s_detail_open) {
        rebuild_session_rows();
    }
    if (s_tab == UI_TAB_DASH) {
        rebuild_dash_cards();
    }
    if (s_tab == UI_TAB_ACTIVITY) {
        rebuild_activity();
    }
    refresh_detail();
}

void herdr_ui_init(void)
{
    ui_theme_init();
    lockscreen_init();

    build_home();
    build_sessions();
    build_dash();
    build_activity();
    build_detail();
    build_keyboard_overlay();
    herdr_ui_settings_init(dock_cb);

    rebuild_session_rows();
    refresh_home();
    refresh_lock_icon();
    show_tab(UI_TAB_HOME);

    /* 150ms: o tick quase sempre sai no early-out por geração; o que importa
       é encurtar a janela entre o push da ponte e o refresh da tela. */
    lv_timer_create(ui_timer_cb, 150, NULL);
}
