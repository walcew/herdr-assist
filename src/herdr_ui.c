#include "herdr_ui.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#include <lvgl.h>

#include "herdr_model.h"
#include "herdr_conn.h"
#include "herdr_ui_settings.h"
#include "panel_cfg.h"
#include "ui_theme.h"

#define DETAIL_POLL_TICKS 6    /* 6 x 500ms = 3s entre read_pane */
#define HEAT_CELLS        12   /* células do mapa de calor por host */
#define ACTION_BAR_H      48

/* --- telas --- */
static lv_obj_t *s_home;
static lv_obj_t *s_sessions;
static lv_obj_t *s_detail;
static lv_obj_t *s_blocked_modal;
static lv_obj_t *s_kb_overlay;

/* --- home --- */
static lv_obj_t *s_clock;
static lv_obj_t *s_date;
static lv_obj_t *s_pet_eye[2];
static lv_obj_t *s_pet_mouth;
static lv_obj_t *s_pet_mood;
static lv_obj_t *s_card_val[4];
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

/* --- detalhe --- */
static lv_obj_t *s_detail_title;
static lv_obj_t *s_detail_dot;
static lv_obj_t *s_term_cont;
static lv_obj_t *s_term_label;

/* --- aprovação e teclado --- */
static lv_obj_t *s_blocked_title;
static lv_obj_t *s_blocked_prompt;
static lv_obj_t *s_blocked_btns[HERDR_MAX_OPTIONS];
static lv_obj_t *s_kb_ta;
static lv_obj_t *s_keyboard;

/* --- estado --- */
static ui_tab_t s_tab = UI_TAB_HOME;
static herdr_agent_t s_ui_agents[HERDR_MAX_AGENTS_TOTAL];
static int s_ui_agent_count;
static herdr_blocked_t s_ui_blocked;
static char s_detail_pane[HERDR_ID_LEN];
static int s_detail_host = -1;
static bool s_detail_open;
static uint32_t s_last_generation = UINT32_MAX;
static int s_poll_tick;
static int s_last_minute = -1;

static const char *host_label(int host)
{
    if (host < 0 || host >= CFG_MAX_HOSTS) {
        return "?";
    }
    const panel_host_t *h = &panel_cfg_get()->hosts[host];
    return h->name[0] ? h->name : h->host;
}

/** Ordem de urgência para o modo prioridade: bloqueado primeiro. */
static int status_rank(const char *status)
{
    if (strcmp(status, "blocked") == 0) return 0;
    if (strcmp(status, "working") == 0) return 1;
    if (strcmp(status, "idle") == 0)    return 2;
    return 3;
}

/* ---------- navegação ---------- */

static void rebuild_session_rows(void);

static void show_tab(ui_tab_t tab)
{
    s_tab = tab;
    lv_obj_add_flag(s_home, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_sessions, LV_OBJ_FLAG_HIDDEN);
    herdr_ui_settings_hide();
    switch (tab) {
    case UI_TAB_SESSIONS:
        /* a lista só se reconstrói com a aba aberta, então o que chegou
           enquanto ela estava escondida precisa ser montado agora */
        rebuild_session_rows();
        lv_obj_clear_flag(s_sessions, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(s_sessions);
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

static void open_detail(const herdr_agent_t *agent)
{
    strlcpy(s_detail_pane, agent->pane_id, HERDR_ID_LEN);
    s_detail_host = agent->host;
    s_detail_open = true;
    s_poll_tick = DETAIL_POLL_TICKS;  /* força read_pane no próximo tick */
    lv_label_set_text_fmt(s_detail_title, "%s / %s \xC2\xB7 %s",
                          host_label(agent->host), agent->project, agent->agent);
    lv_obj_set_style_bg_color(s_detail_dot, ui_status_color(agent->status), 0);
    lv_label_set_text(s_term_label, "carregando...");
    lv_obj_clear_flag(s_detail, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_detail);
}

static void close_detail(void)
{
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

static void action_prompt_cb(lv_event_t *e)
{
    (void)e;
    lv_textarea_set_text(s_kb_ta, "");
    lv_obj_clear_flag(s_kb_overlay, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_kb_overlay);
}

static void kb_event_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_READY) {          /* checkmark: envia */
        const char *text = lv_textarea_get_text(s_kb_ta);
        if (s_detail_pane[0] && text[0]) {
            herdr_conn_send_text(s_detail_host, s_detail_pane, text);
            static const char *enter = "Enter";
            herdr_conn_send_keys(s_detail_host, s_detail_pane, &enter, 1);
        }
        lv_obj_add_flag(s_kb_overlay, LV_OBJ_FLAG_HIDDEN);
    } else if (code == LV_EVENT_CANCEL) {  /* teclado fechado: cancela */
        lv_obj_add_flag(s_kb_overlay, LV_OBJ_FLAG_HIDDEN);
    }
}

static void blocked_option_cb(lv_event_t *e)
{
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    if (s_ui_blocked.active && idx < s_ui_blocked.option_count) {
        herdr_conn_respond(s_ui_blocked.host, s_ui_blocked.pane_id, s_ui_blocked.options[idx]);
        herdr_model_clear_blocked(s_ui_blocked.host, s_ui_blocked.pane_id);
        lv_obj_add_flag(s_blocked_modal, LV_OBJ_FLAG_HIDDEN);
    }
}

static void blocked_dismiss_cb(lv_event_t *e)
{
    (void)e;
    /* só esconde na UI; a decisão continua pendente no host */
    herdr_model_clear_blocked(s_ui_blocked.host, s_ui_blocked.pane_id);
    lv_obj_add_flag(s_blocked_modal, LV_OBJ_FLAG_HIDDEN);
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

static void build_pet(lv_obj_t *parent)
{
    lv_obj_t *pet = ui_card(parent, 18);
    lv_obj_set_size(pet, 62, 62);
    lv_obj_set_style_border_width(pet, 1, 0);
    lv_obj_set_style_border_color(pet, UI_BORDER, 0);

    for (int i = 0; i < 2; i++) {
        s_pet_eye[i] = lv_obj_create(pet);
        lv_obj_set_size(s_pet_eye[i], 7, 11);
        lv_obj_set_style_radius(s_pet_eye[i], 4, 0);
        lv_obj_set_style_bg_color(s_pet_eye[i], UI_TEXT, 0);
        lv_obj_set_style_border_width(s_pet_eye[i], 0, 0);
        lv_obj_align(s_pet_eye[i], LV_ALIGN_TOP_MID, i == 0 ? -9 : 9, 19);
        lv_obj_clear_flag(s_pet_eye[i], LV_OBJ_FLAG_SCROLLABLE);
    }

    s_pet_mouth = lv_obj_create(pet);
    lv_obj_set_size(s_pet_mouth, 12, 3);
    lv_obj_set_style_radius(s_pet_mouth, 2, 0);
    lv_obj_set_style_bg_color(s_pet_mouth, UI_MUTED, 0);
    lv_obj_set_style_border_width(s_pet_mouth, 0, 0);
    lv_obj_align(s_pet_mouth, LV_ALIGN_TOP_MID, 0, 37);
    lv_obj_clear_flag(s_pet_mouth, LV_OBJ_FLAG_SCROLLABLE);

    s_pet_mood = lv_obj_create(pet);
    lv_obj_set_size(s_pet_mood, 11, 11);
    lv_obj_set_style_radius(s_pet_mood, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(s_pet_mood, UI_IDLE, 0);
    lv_obj_set_style_border_width(s_pet_mood, 2, 0);
    lv_obj_set_style_border_color(s_pet_mood, UI_BG, 0);
    lv_obj_align(s_pet_mood, LV_ALIGN_TOP_RIGHT, 0, 0);
    lv_obj_clear_flag(s_pet_mood, LV_OBJ_FLAG_SCROLLABLE);
}

static void build_summary_cards(lv_obj_t *parent)
{
    static const char *labels[4] = { "Ativas", "Em andamento", "Ociosas", "Bloqueadas" };

    lv_obj_t *grid = ui_plain(parent);
    lv_obj_set_size(grid, LV_PCT(100), 114);
    lv_obj_set_flex_flow(grid, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_row(grid, 6, 0);
    lv_obj_set_style_pad_column(grid, 6, 0);

    for (int i = 0; i < 4; i++) {
        lv_obj_t *card = ui_card(grid, 8);
        lv_obj_set_size(card, 145, 54);
        lv_obj_set_style_pad_left(card, 12, 0);
        lv_obj_set_style_pad_top(card, 8, 0);

        s_card_val[i] = lv_label_create(card);
        lv_label_set_text(s_card_val[i], "0");
        lv_obj_set_style_text_font(s_card_val[i], &lv_font_ui_20, 0);
        lv_obj_set_style_text_color(s_card_val[i], UI_TEXT, 0);
        lv_obj_align(s_card_val[i], LV_ALIGN_TOP_LEFT, 0, 0);

        lv_obj_t *dot = lv_obj_create(card);
        lv_obj_set_size(dot, 7, 7);
        lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_border_width(dot, 0, 0);
        lv_obj_set_style_bg_color(dot, i == 1 ? UI_WORKING : i == 2 ? UI_IDLE :
                                       i == 3 ? UI_BLOCKED : UI_TEXT, 0);
        lv_obj_align(dot, LV_ALIGN_TOP_LEFT, 0, 28);

        lv_obj_t *key = lv_label_create(card);
        lv_label_set_text(key, labels[i]);
        lv_obj_set_style_text_font(key, &lv_font_ui_12, 0);
        lv_obj_set_style_text_color(key, UI_MUTED, 0);
        lv_obj_align(key, LV_ALIGN_TOP_LEFT, 12, 25);
    }
}

static void build_host_cards(lv_obj_t *parent)
{
    const panel_cfg_t *cfg = panel_cfg_get();

    s_host_area = ui_plain(parent);
    lv_obj_set_size(s_host_area, LV_PCT(100), 198);
    lv_obj_set_flex_flow(s_host_area, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s_host_area, 6, 0);
    lv_obj_add_flag(s_host_area, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(s_host_area, LV_DIR_VER);

    for (int h = 0; h < CFG_MAX_HOSTS; h++) {
        if (!cfg->hosts[h].enabled) {
            continue;
        }
        host_widget_t *w = &s_hw[s_hw_count++];
        w->host = h;

        lv_obj_t *card = ui_card(s_host_area, 8);
        lv_obj_set_size(card, LV_PCT(100), 60);
        lv_obj_set_style_pad_left(card, 12, 0);
        lv_obj_set_style_pad_right(card, 12, 0);
        lv_obj_set_style_pad_top(card, 10, 0);

        lv_obj_t *name = lv_label_create(card);
        lv_label_set_text(name, host_label(h));
        lv_obj_set_style_text_font(name, &lv_font_ui_12, 0);
        lv_obj_set_style_text_color(name, UI_TEXT, 0);
        lv_obj_align(name, LV_ALIGN_TOP_LEFT, 0, 0);

        w->status = lv_label_create(card);
        lv_label_set_text(w->status, "");
        lv_obj_set_style_text_font(w->status, &lv_font_ui_12, 0);
        lv_obj_set_style_text_color(w->status, UI_MUTED, 0);
        lv_obj_align(w->status, LV_ALIGN_TOP_RIGHT, 0, 0);

        for (int c = 0; c < HEAT_CELLS; c++) {
            w->cells[c] = lv_obj_create(card);
            lv_obj_set_size(w->cells[c], 17, 17);
            lv_obj_set_style_radius(w->cells[c], 4, 0);
            lv_obj_set_style_border_width(w->cells[c], 0, 0);
            lv_obj_set_style_bg_color(w->cells[c], UI_CELL, 0);
            lv_obj_align(w->cells[c], LV_ALIGN_TOP_LEFT, c * 21, 23);
            lv_obj_clear_flag(w->cells[c], LV_OBJ_FLAG_SCROLLABLE);
        }
    }

    if (s_hw_count == 0) {
        lv_obj_t *empty = lv_label_create(s_host_area);
        lv_label_set_text(empty, "Nenhum host configurado.\nToque em " LV_SYMBOL_SETTINGS
                                 " para cadastrar.");
        lv_obj_set_style_text_font(empty, &lv_font_ui_12, 0);
        lv_obj_set_style_text_color(empty, UI_MUTED, 0);
    }
}

static void build_home(void)
{
    s_home = ui_screen();
    lv_obj_set_style_pad_top(s_home, 20, 0);
    lv_obj_set_style_pad_left(s_home, 12, 0);
    lv_obj_set_style_pad_right(s_home, 12, 0);
    lv_obj_set_flex_flow(s_home, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s_home, 12, 0);

    lv_obj_t *hero = ui_plain(s_home);
    lv_obj_set_size(hero, LV_PCT(100), 62);

    s_clock = lv_label_create(hero);
    lv_label_set_text(s_clock, "--:--");
    lv_obj_set_style_text_font(s_clock, &lv_font_ui_clock_44, 0);
    lv_obj_set_style_text_color(s_clock, UI_TEXT, 0);
    lv_obj_align(s_clock, LV_ALIGN_LEFT_MID, 0, -8);

    s_date = lv_label_create(hero);
    lv_label_set_text(s_date, "sincronizando");
    lv_obj_set_style_text_font(s_date, &lv_font_ui_12, 0);
    lv_obj_set_style_text_color(s_date, UI_MUTED, 0);
    lv_obj_align(s_date, LV_ALIGN_LEFT_MID, 2, 20);

    lv_obj_t *pet_slot = ui_plain(hero);
    lv_obj_set_size(pet_slot, 62, 62);
    lv_obj_align(pet_slot, LV_ALIGN_RIGHT_MID, 0, 0);
    build_pet(pet_slot);

    build_summary_cards(s_home);
    build_host_cards(s_home);
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

    static const char *wd[] = { "dom", "seg", "ter", "qua", "qui", "sex", "sáb" };
    static const char *mo[] = { "jan", "fev", "mar", "abr", "mai", "jun",
                                "jul", "ago", "set", "out", "nov", "dez" };
    lv_label_set_text_fmt(s_clock, "%02d:%02d", tm.tm_hour, tm.tm_min);
    lv_label_set_text_fmt(s_date, "%s, %d %s", wd[tm.tm_wday], tm.tm_mday, mo[tm.tm_mon]);
}

static void refresh_home(void)
{
    int total = s_ui_agent_count;
    int working = 0;
    int idle = 0;
    int blocked = 0;
    for (int i = 0; i < s_ui_agent_count; i++) {
        switch (status_rank(s_ui_agents[i].status)) {
        case 0: blocked++; break;
        case 1: working++; break;
        case 2: idle++;    break;
        default: break;
        }
    }
    lv_label_set_text_fmt(s_card_val[0], "%d", total);
    lv_label_set_text_fmt(s_card_val[1], "%d", working);
    lv_label_set_text_fmt(s_card_val[2], "%d", idle);
    lv_label_set_text_fmt(s_card_val[3], "%d", blocked);

    /* humor do mascote: o pior estado manda */
    lv_color_t mood = blocked ? UI_BLOCKED : working ? UI_WORKING : UI_IDLE;
    lv_obj_set_style_bg_color(s_pet_mood, mood, 0);
    lv_coord_t eye_h = blocked ? 7 : 11;                    /* apertados = preocupado */
    lv_coord_t mouth_w = blocked ? 8 : working ? 10 : 14;   /* larga = tudo bem */
    for (int i = 0; i < 2; i++) {
        lv_obj_set_height(s_pet_eye[i], eye_h);
        lv_obj_align(s_pet_eye[i], LV_ALIGN_TOP_MID, i == 0 ? -9 : 9, blocked ? 21 : 19);
    }
    lv_obj_set_width(s_pet_mouth, mouth_w);

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
            lv_label_set_text_fmt(hw->status, "%d %s", n, n == 1 ? "agente" : "agentes");
            lv_obj_set_style_text_color(hw->status, UI_IDLE, 0);
        } else if (conn == HERDR_CONN_CONNECTING) {
            lv_label_set_text(hw->status, "Conectando");
            lv_obj_set_style_text_color(hw->status, UI_WORKING, 0);
        } else {
            lv_label_set_text(hw->status, "Offline");
            lv_obj_set_style_text_color(hw->status, UI_BLOCKED, 0);
        }
    }
}

/* ---------- sessões ---------- */

static void build_sessions(void)
{
    s_sessions = ui_screen();

    lv_obj_t *bar = ui_topbar(s_sessions, "Sessões", NULL);
    s_lbl_group = lv_obj_get_child(ui_icon_btn(bar, "", toggle_group_cb, NULL), 0);
    s_lbl_sort = lv_obj_get_child(ui_icon_btn(bar, "", toggle_sort_cb, NULL), 0);
    update_toolbar_icons();

    s_sess_list = ui_plain(s_sessions);
    lv_obj_set_size(s_sess_list, LV_HOR_RES, LV_VER_RES - UI_TOPBAR_H);
    lv_obj_align(s_sess_list, LV_ALIGN_TOP_MID, 0, UI_TOPBAR_H);
    lv_obj_set_flex_flow(s_sess_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s_sess_list, 4, 0);
    lv_obj_set_style_pad_left(s_sess_list, UI_PAD, 0);
    lv_obj_set_style_pad_right(s_sess_list, UI_PAD, 0);
    lv_obj_set_style_pad_bottom(s_sess_list, UI_DOCK_SPACE, 0);
    lv_obj_add_flag(s_sess_list, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(s_sess_list, LV_DIR_VER);

    ui_dock(s_sessions, UI_TAB_SESSIONS, dock_cb);
}

static void add_session_row(int flat_idx)
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
    lv_obj_align(name, LV_ALIGN_LEFT_MID, 26, -8);

    lv_obj_t *sub = lv_label_create(row);
    /* sem host quando a lista já está agrupada por ele */
    if (s_group_by_host) {
        lv_label_set_text_fmt(sub, "%s \xC2\xB7 %s", a->agent, a->status);
    } else {
        lv_label_set_text_fmt(sub, "%s \xC2\xB7 %s \xC2\xB7 %s",
                              host_label(a->host), a->agent, a->status);
    }
    lv_obj_set_style_text_font(sub, &lv_font_ui_12, 0);
    lv_obj_set_style_text_color(sub, UI_MUTED, 0);
    lv_obj_align(sub, LV_ALIGN_LEFT_MID, 26, 11);
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
    const panel_cfg_t *cfg = panel_cfg_get();
    int order[HERDR_MAX_AGENTS_TOTAL];

    int enabled = 0;
    for (int h = 0; h < CFG_MAX_HOSTS; h++) {
        enabled += cfg->hosts[h].enabled;
    }
    if (enabled == 0) {
        add_muted_line("Nenhum host configurado.");
        add_muted_line("Toque em " LV_SYMBOL_SETTINGS " para cadastrar.");
        return;
    }

    if (!s_group_by_host) {
        int n = collect_agents(-1, order, HERDR_MAX_AGENTS_TOTAL);
        if (n == 0) {
            add_muted_line("Nenhuma sessão ativa.");
        }
        for (int i = 0; i < n; i++) {
            add_session_row(order[i]);
        }
        return;
    }

    for (int h = 0; h < CFG_MAX_HOSTS; h++) {
        if (!cfg->hosts[h].enabled) {
            continue;
        }
        herdr_conn_state_t conn = herdr_model_get_conn(h);
        add_section_label(host_label(h),
                          conn == HERDR_CONN_ONLINE ? "Online" :
                          conn == HERDR_CONN_CONNECTING ? "Conectando" : "Offline",
                          conn == HERDR_CONN_ONLINE ? UI_IDLE :
                          conn == HERDR_CONN_CONNECTING ? UI_WORKING : UI_BLOCKED);
        int n = collect_agents(h, order, HERDR_MAX_AGENTS_TOTAL);
        for (int i = 0; i < n; i++) {
            add_session_row(order[i]);
        }
        if (n == 0) {
            add_muted_line(conn == HERDR_CONN_ONLINE ? "Nenhuma sessão ativa."
                                                     : "Aguardando a ponte...");
        }
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

    s_term_label = lv_label_create(s_term_cont);
    lv_obj_set_width(s_term_label, LV_PCT(100));
    lv_label_set_long_mode(s_term_label, LV_LABEL_LONG_WRAP);
    lv_label_set_text(s_term_label, "");
    lv_obj_set_style_text_font(s_term_label, &lv_font_terminal_12, 0);
    lv_obj_set_style_text_color(s_term_label, UI_TERM_TEXT, 0);

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
    /* static: a struct tem ~8KB e a task da LVGL tem 8KB de stack.
       Seguro porque esta função roda exclusivamente na task da LVGL. */
    static herdr_pane_content_t content;
    if (herdr_model_get_pane_content(&content) &&
        content.host == s_detail_host &&
        strcmp(content.pane_id, s_detail_pane) == 0 &&
        strcmp(lv_label_get_text(s_term_label), content.content) != 0) {
        lv_label_set_text(s_term_label, content.content);
        lv_obj_scroll_to_y(s_term_cont, LV_COORD_MAX, LV_ANIM_OFF);
    }
}

/* ---------- aprovação e teclado ---------- */

static void build_blocked_modal(void)
{
    s_blocked_modal = ui_screen();
    lv_obj_set_style_bg_color(s_blocked_modal, UI_MODAL_BG, 0);

    s_blocked_title = lv_label_create(s_blocked_modal);
    lv_label_set_text(s_blocked_title, LV_SYMBOL_WARNING " Aprovação pendente");
    lv_obj_set_style_text_font(s_blocked_title, &lv_font_ui_bold_16, 0);
    lv_obj_set_style_text_color(s_blocked_title, UI_BLOCKED, 0);
    /* fica em 16: com o nome do host junto, 20px não caberia ao lado do X */
    lv_obj_set_width(s_blocked_title, LV_HOR_RES - 12 - UI_ICON_BTN - 20);
    lv_label_set_long_mode(s_blocked_title, LV_LABEL_LONG_DOT);
    lv_obj_align(s_blocked_title, LV_ALIGN_TOP_LEFT, 12, 22);

    lv_obj_t *x = ui_icon_btn(s_blocked_modal, LV_SYMBOL_CLOSE, blocked_dismiss_cb, NULL);
    lv_obj_align(x, LV_ALIGN_TOP_RIGHT, -10, 9);

    lv_obj_t *box = lv_obj_create(s_blocked_modal);
    lv_obj_set_size(box, LV_HOR_RES - 24, 200);
    lv_obj_align(box, LV_ALIGN_TOP_MID, 0, 62);
    lv_obj_set_style_bg_color(box, UI_TERM_BG, 0);
    lv_obj_set_style_border_width(box, 0, 0);
    lv_obj_set_style_radius(box, 6, 0);
    lv_obj_set_style_pad_all(box, 8, 0);

    s_blocked_prompt = lv_label_create(box);
    lv_obj_set_width(s_blocked_prompt, LV_PCT(100));
    lv_label_set_long_mode(s_blocked_prompt, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(s_blocked_prompt, &lv_font_ui_12, 0);
    lv_obj_set_style_text_color(s_blocked_prompt, UI_TEXT, 0);

    lv_obj_t *opts = ui_plain(s_blocked_modal);
    lv_obj_set_size(opts, LV_HOR_RES - 24, 140);
    lv_obj_align(opts, LV_ALIGN_BOTTOM_MID, 0, -8);
    lv_obj_set_flex_flow(opts, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(opts, 6, 0);

    for (int i = 0; i < HERDR_MAX_OPTIONS; i++) {
        lv_obj_t *btn = lv_btn_create(opts);
        lv_obj_set_size(btn, LV_PCT(100), 40);
        lv_obj_set_style_bg_color(btn, UI_PANEL, 0);
        lv_obj_set_style_radius(btn, 6, 0);
        lv_obj_set_style_shadow_width(btn, 0, 0);
        lv_obj_add_event_cb(btn, blocked_option_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        lv_obj_t *l = lv_label_create(btn);
        lv_label_set_text(l, "");
        lv_obj_set_style_text_font(l, &lv_font_ui_14, 0);
        lv_obj_set_style_text_color(l, UI_TEXT, 0);
        lv_obj_center(l);
        s_blocked_btns[i] = btn;
    }
}

static void refresh_blocked(void)
{
    bool active = herdr_model_get_blocked(&s_ui_blocked);
    s_ui_blocked.active = active;
    if (active) {
        lv_label_set_text_fmt(s_blocked_title, LV_SYMBOL_WARNING " Aprovação pendente \xC2\xB7 %s",
                              host_label(s_ui_blocked.host));
        lv_label_set_text(s_blocked_prompt, s_ui_blocked.prompt);
        for (int i = 0; i < HERDR_MAX_OPTIONS; i++) {
            if (i < s_ui_blocked.option_count) {
                lv_label_set_text(lv_obj_get_child(s_blocked_btns[i], 0), s_ui_blocked.options[i]);
                lv_obj_clear_flag(s_blocked_btns[i], LV_OBJ_FLAG_HIDDEN);
            } else {
                lv_obj_add_flag(s_blocked_btns[i], LV_OBJ_FLAG_HIDDEN);
            }
        }
        lv_obj_clear_flag(s_blocked_modal, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(s_blocked_modal);
    } else {
        lv_obj_add_flag(s_blocked_modal, LV_OBJ_FLAG_HIDDEN);
    }
}

static void build_keyboard_overlay(void)
{
    s_kb_overlay = ui_screen();

    s_kb_ta = lv_textarea_create(s_kb_overlay);
    lv_obj_set_size(s_kb_ta, LV_HOR_RES - 16, 76);
    lv_obj_align(s_kb_ta, LV_ALIGN_TOP_MID, 0, 8);
    lv_textarea_set_placeholder_text(s_kb_ta, "Prompt para o agente...");
    lv_obj_set_style_text_font(s_kb_ta, &lv_font_ui_14, 0);
    lv_obj_set_style_bg_color(s_kb_ta, UI_PANEL, 0);
    lv_obj_set_style_border_color(s_kb_ta, UI_BORDER, 0);

    s_keyboard = lv_keyboard_create(s_kb_overlay);
    lv_obj_set_size(s_keyboard, LV_HOR_RES, LV_VER_RES / 2);
    lv_obj_set_style_text_font(s_keyboard, &lv_font_ui_14, 0);
    lv_keyboard_set_textarea(s_keyboard, s_kb_ta);
    lv_obj_add_event_cb(s_keyboard, kb_event_cb, LV_EVENT_READY, NULL);
    lv_obj_add_event_cb(s_keyboard, kb_event_cb, LV_EVENT_CANCEL, NULL);
}

/* ---------- laço da UI ---------- */

static void ui_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    refresh_clock();

    if (s_detail_open && ++s_poll_tick >= DETAIL_POLL_TICKS) {
        s_poll_tick = 0;
        herdr_conn_read_pane(s_detail_host, s_detail_pane, 40);
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
    refresh_detail();
    refresh_blocked();
}

void herdr_ui_init(void)
{
    ui_theme_init();

    build_home();
    build_sessions();
    build_detail();
    build_blocked_modal();
    build_keyboard_overlay();
    herdr_ui_settings_init(dock_cb);

    rebuild_session_rows();
    refresh_home();
    show_tab(UI_TAB_HOME);

    lv_timer_create(ui_timer_cb, 500, NULL);
}
