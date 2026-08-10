#include "herdr_ui.h"

#include <stdio.h>
#include <string.h>

#include <lvgl.h>

#include "herdr_model.h"
#include "herdr_conn.h"

/* Fonte monoespaçada gerada da JetBrainsMono Nerd (ver scripts/gen_font.sh):
   ASCII + box-drawing + braille, os glifos que a saída dos agentes usa. */
LV_FONT_DECLARE(lv_font_terminal_12);

#define HEADER_H      36
#define ROW_H         52
#define ACTION_BAR_H  48
#define DETAIL_POLL_TICKS 6   /* 6 x 500ms = 3s entre read_pane */

/* Paleta (tema escuro) */
#define COL_BG        lv_color_hex(0x14161a)
#define COL_PANEL     lv_color_hex(0x1e2127)
#define COL_TEXT      lv_color_hex(0xe8e8e8)
#define COL_MUTED     lv_color_hex(0x8a8f98)
#define COL_IDLE      lv_color_hex(0x4caf50)
#define COL_WORKING   lv_color_hex(0xffb300)
#define COL_BLOCKED   lv_color_hex(0xef5350)
#define COL_OFFLINE   lv_color_hex(0x616161)

static lv_obj_t *s_header;
static lv_obj_t *s_conn_dot;
static lv_obj_t *s_conn_label;
static lv_obj_t *s_list_panel;
static lv_obj_t *s_detail_panel;
static lv_obj_t *s_detail_title;
static lv_obj_t *s_detail_dot;
static lv_obj_t *s_term_cont;
static lv_obj_t *s_term_label;
static lv_obj_t *s_blocked_modal;
static lv_obj_t *s_blocked_prompt;
static lv_obj_t *s_blocked_btns[HERDR_MAX_OPTIONS];
static lv_obj_t *s_kb_overlay;
static lv_obj_t *s_kb_ta;
static lv_obj_t *s_keyboard;

static herdr_agent_t s_ui_agents[HERDR_MAX_AGENTS];
static int s_ui_agent_count;
static herdr_blocked_t s_ui_blocked;
static char s_detail_pane[HERDR_ID_LEN];
static bool s_detail_open;
static uint32_t s_last_generation = UINT32_MAX;
static int s_poll_tick;

static lv_color_t status_color(const char *status)
{
    if (strcmp(status, "blocked") == 0) return COL_BLOCKED;
    if (strcmp(status, "working") == 0) return COL_WORKING;
    if (strcmp(status, "idle") == 0)    return COL_IDLE;
    return COL_OFFLINE;
}

/* ---------- navegação ---------- */

static void open_detail(const herdr_agent_t *agent)
{
    strlcpy(s_detail_pane, agent->pane_id, HERDR_ID_LEN);
    s_detail_open = true;
    s_poll_tick = DETAIL_POLL_TICKS;  /* força read_pane no próximo tick */
    lv_label_set_text_fmt(s_detail_title, "%s  -  %s", agent->project, agent->agent);
    lv_obj_set_style_bg_color(s_detail_dot, status_color(agent->status), 0);
    lv_label_set_text(s_term_label, "carregando...");
    lv_obj_clear_flag(s_detail_panel, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_list_panel, LV_OBJ_FLAG_HIDDEN);
}

static void close_detail(void)
{
    s_detail_open = false;
    s_detail_pane[0] = '\0';
    lv_obj_add_flag(s_detail_panel, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(s_list_panel, LV_OBJ_FLAG_HIDDEN);
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
        herdr_conn_send_keys(s_detail_pane, &key, 1);
    }
}

static void action_focus_cb(lv_event_t *e)
{
    (void)e;
    if (s_detail_pane[0]) {
        herdr_conn_focus(s_detail_pane);
    }
}

static void action_prompt_cb(lv_event_t *e)
{
    (void)e;
    lv_textarea_set_text(s_kb_ta, "");
    lv_obj_clear_flag(s_kb_overlay, LV_OBJ_FLAG_HIDDEN);
}

static void kb_event_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_READY) {          /* checkmark: envia */
        const char *text = lv_textarea_get_text(s_kb_ta);
        if (s_detail_pane[0] && text[0]) {
            herdr_conn_send_text(s_detail_pane, text);
            static const char *enter = "Enter";
            herdr_conn_send_keys(s_detail_pane, &enter, 1);
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
        herdr_conn_respond(s_ui_blocked.pane_id, s_ui_blocked.options[idx]);
        herdr_model_clear_blocked(s_ui_blocked.pane_id);
        lv_obj_add_flag(s_blocked_modal, LV_OBJ_FLAG_HIDDEN);
    }
}

static void blocked_dismiss_cb(lv_event_t *e)
{
    (void)e;
    /* só esconde na UI; decisão fica para o Mac */
    herdr_model_clear_blocked(s_ui_blocked.pane_id);
    lv_obj_add_flag(s_blocked_modal, LV_OBJ_FLAG_HIDDEN);
}

/* ---------- construção ---------- */

static lv_obj_t *make_panel(lv_obj_t *parent)
{
    lv_obj_t *p = lv_obj_create(parent);
    lv_obj_set_style_bg_color(p, COL_BG, 0);
    lv_obj_set_style_border_width(p, 0, 0);
    lv_obj_set_style_radius(p, 0, 0);
    lv_obj_set_style_pad_all(p, 0, 0);
    return p;
}

static void build_header(void)
{
    s_header = make_panel(lv_scr_act());
    lv_obj_set_size(s_header, LV_HOR_RES, HEADER_H);
    lv_obj_align(s_header, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_bg_color(s_header, COL_PANEL, 0);

    lv_obj_t *title = lv_label_create(s_header);
    lv_label_set_text(title, "herdr-assist");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(title, COL_TEXT, 0);
    lv_obj_align(title, LV_ALIGN_LEFT_MID, 12, 0);

    s_conn_label = lv_label_create(s_header);
    lv_label_set_text(s_conn_label, "offline");
    lv_obj_set_style_text_font(s_conn_label, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(s_conn_label, COL_MUTED, 0);
    lv_obj_align(s_conn_label, LV_ALIGN_RIGHT_MID, -28, 0);

    s_conn_dot = lv_obj_create(s_header);
    lv_obj_set_size(s_conn_dot, 12, 12);
    lv_obj_set_style_radius(s_conn_dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(s_conn_dot, 0, 0);
    lv_obj_set_style_bg_color(s_conn_dot, COL_OFFLINE, 0);
    lv_obj_align(s_conn_dot, LV_ALIGN_RIGHT_MID, -10, 0);
}

static void build_list_panel(void)
{
    s_list_panel = make_panel(lv_scr_act());
    lv_obj_set_size(s_list_panel, LV_HOR_RES, LV_VER_RES - HEADER_H);
    lv_obj_align(s_list_panel, LV_ALIGN_TOP_MID, 0, HEADER_H);
    lv_obj_set_flex_flow(s_list_panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s_list_panel, 4, 0);
    lv_obj_set_style_pad_all(s_list_panel, 8, 0);
}

static void rebuild_list_rows(void)
{
    lv_obj_clean(s_list_panel);
    if (s_ui_agent_count == 0) {
        lv_obj_t *empty = lv_label_create(s_list_panel);
        lv_label_set_text(empty, herdr_model_get_conn() == HERDR_CONN_ONLINE
                          ? "nenhum agente ativo" : "conectando a ponte...");
        lv_obj_set_style_text_color(empty, COL_MUTED, 0);
        lv_obj_set_style_text_font(empty, &lv_font_montserrat_14, 0);
        return;
    }
    for (int i = 0; i < s_ui_agent_count; i++) {
        const herdr_agent_t *a = &s_ui_agents[i];
        lv_obj_t *row = lv_btn_create(s_list_panel);
        lv_obj_set_size(row, LV_PCT(100), ROW_H);
        lv_obj_set_style_bg_color(row, COL_PANEL, 0);
        lv_obj_set_style_radius(row, 6, 0);
        lv_obj_add_event_cb(row, row_clicked_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);

        lv_obj_t *dot = lv_obj_create(row);
        lv_obj_set_size(dot, 14, 14);
        lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_border_width(dot, 0, 0);
        lv_obj_set_style_bg_color(dot, status_color(a->status), 0);
        lv_obj_align(dot, LV_ALIGN_LEFT_MID, 4, 0);
        lv_obj_clear_flag(dot, LV_OBJ_FLAG_CLICKABLE);

        lv_obj_t *name = lv_label_create(row);
        lv_label_set_text_fmt(name, "%s", a->project);
        lv_obj_set_style_text_font(name, &lv_font_montserrat_16, 0);
        lv_obj_set_style_text_color(name, COL_TEXT, 0);
        lv_obj_align(name, LV_ALIGN_LEFT_MID, 30, -8);

        lv_obj_t *sub = lv_label_create(row);
        lv_label_set_text_fmt(sub, "%s - %s", a->agent, a->status);
        lv_obj_set_style_text_font(sub, &lv_font_montserrat_12, 0);
        lv_obj_set_style_text_color(sub, COL_MUTED, 0);
        lv_obj_align(sub, LV_ALIGN_LEFT_MID, 30, 12);
    }
}

static lv_obj_t *make_action_btn(lv_obj_t *parent, const char *label,
                                 lv_event_cb_t cb, void *user_data)
{
    lv_obj_t *btn = lv_btn_create(parent);
    lv_obj_set_flex_grow(btn, 1);
    lv_obj_set_height(btn, ACTION_BAR_H - 8);
    lv_obj_set_style_bg_color(btn, COL_PANEL, 0);
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, user_data);
    lv_obj_t *l = lv_label_create(btn);
    lv_label_set_text(l, label);
    lv_obj_set_style_text_font(l, &lv_font_montserrat_14, 0);
    lv_obj_center(l);
    return btn;
}

static void build_detail_panel(void)
{
    s_detail_panel = make_panel(lv_scr_act());
    lv_obj_set_size(s_detail_panel, LV_HOR_RES, LV_VER_RES);
    lv_obj_align(s_detail_panel, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_add_flag(s_detail_panel, LV_OBJ_FLAG_HIDDEN);

    /* header do detalhe */
    lv_obj_t *hdr = make_panel(s_detail_panel);
    lv_obj_set_size(hdr, LV_HOR_RES, HEADER_H);
    lv_obj_align(hdr, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_bg_color(hdr, COL_PANEL, 0);

    lv_obj_t *back = lv_btn_create(hdr);
    lv_obj_set_size(back, 56, HEADER_H - 6);
    lv_obj_align(back, LV_ALIGN_LEFT_MID, 4, 0);
    lv_obj_set_style_bg_color(back, COL_BG, 0);
    lv_obj_add_event_cb(back, back_clicked_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *bl = lv_label_create(back);
    lv_label_set_text(bl, LV_SYMBOL_LEFT);
    lv_obj_center(bl);

    s_detail_title = lv_label_create(hdr);
    lv_label_set_text(s_detail_title, "");
    lv_obj_set_style_text_font(s_detail_title, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_detail_title, COL_TEXT, 0);
    lv_obj_align(s_detail_title, LV_ALIGN_LEFT_MID, 72, 0);

    s_detail_dot = lv_obj_create(hdr);
    lv_obj_set_size(s_detail_dot, 12, 12);
    lv_obj_set_style_radius(s_detail_dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(s_detail_dot, 0, 0);
    lv_obj_align(s_detail_dot, LV_ALIGN_RIGHT_MID, -10, 0);

    /* terminal */
    s_term_cont = lv_obj_create(s_detail_panel);
    lv_obj_set_size(s_term_cont, LV_HOR_RES, LV_VER_RES - HEADER_H - ACTION_BAR_H);
    lv_obj_align(s_term_cont, LV_ALIGN_TOP_MID, 0, HEADER_H);
    lv_obj_set_style_bg_color(s_term_cont, lv_color_hex(0x0d0f12), 0);
    lv_obj_set_style_border_width(s_term_cont, 0, 0);
    lv_obj_set_style_radius(s_term_cont, 0, 0);
    lv_obj_set_style_pad_all(s_term_cont, 8, 0);

    s_term_label = lv_label_create(s_term_cont);
    lv_obj_set_width(s_term_label, LV_PCT(100));
    lv_label_set_long_mode(s_term_label, LV_LABEL_LONG_WRAP);
    lv_label_set_text(s_term_label, "");
    lv_obj_set_style_text_font(s_term_label, &lv_font_terminal_12, 0);
    lv_obj_set_style_text_color(s_term_label, lv_color_hex(0xc5e1a5), 0);

    /* barra de ações */
    lv_obj_t *bar = make_panel(s_detail_panel);
    lv_obj_set_size(bar, LV_HOR_RES, ACTION_BAR_H);
    lv_obj_align(bar, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(bar, COL_PANEL, 0);
    lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(bar, 4, 0);
    lv_obj_set_style_pad_all(bar, 4, 0);

    make_action_btn(bar, "Esc", action_key_cb, "Escape");
    make_action_btn(bar, LV_SYMBOL_UP, action_key_cb, "Up");
    make_action_btn(bar, LV_SYMBOL_DOWN, action_key_cb, "Down");
    make_action_btn(bar, LV_SYMBOL_NEW_LINE, action_key_cb, "Enter");
    make_action_btn(bar, "C-c", action_key_cb, "C-c");
    make_action_btn(bar, LV_SYMBOL_EYE_OPEN, action_focus_cb, NULL);
    make_action_btn(bar, LV_SYMBOL_KEYBOARD, action_prompt_cb, NULL);
}

static void build_blocked_modal(void)
{
    s_blocked_modal = make_panel(lv_scr_act());
    lv_obj_set_size(s_blocked_modal, LV_HOR_RES, LV_VER_RES);
    lv_obj_set_style_bg_color(s_blocked_modal, lv_color_hex(0x201014), 0);
    lv_obj_add_flag(s_blocked_modal, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t *title = lv_label_create(s_blocked_modal);
    lv_label_set_text(title, LV_SYMBOL_WARNING "  aprovacao pendente");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(title, COL_BLOCKED, 0);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 12, 10);

    lv_obj_t *dismiss = lv_btn_create(s_blocked_modal);
    lv_obj_set_size(dismiss, 44, 32);
    lv_obj_align(dismiss, LV_ALIGN_TOP_RIGHT, -8, 6);
    lv_obj_set_style_bg_color(dismiss, COL_PANEL, 0);
    lv_obj_add_event_cb(dismiss, blocked_dismiss_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *dl = lv_label_create(dismiss);
    lv_label_set_text(dl, LV_SYMBOL_CLOSE);
    lv_obj_center(dl);

    lv_obj_t *prompt_cont = lv_obj_create(s_blocked_modal);
    lv_obj_set_size(prompt_cont, LV_HOR_RES - 24, 120);
    lv_obj_align(prompt_cont, LV_ALIGN_TOP_MID, 0, 44);
    lv_obj_set_style_bg_color(prompt_cont, lv_color_hex(0x0d0f12), 0);
    lv_obj_set_style_border_width(prompt_cont, 0, 0);
    lv_obj_set_style_pad_all(prompt_cont, 8, 0);

    s_blocked_prompt = lv_label_create(prompt_cont);
    lv_obj_set_width(s_blocked_prompt, LV_PCT(100));
    lv_label_set_long_mode(s_blocked_prompt, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(s_blocked_prompt, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(s_blocked_prompt, COL_TEXT, 0);

    lv_obj_t *btn_col = make_panel(s_blocked_modal);
    lv_obj_set_size(btn_col, LV_HOR_RES - 24, 140);
    lv_obj_align(btn_col, LV_ALIGN_BOTTOM_MID, 0, -8);
    lv_obj_set_style_bg_color(btn_col, lv_color_hex(0x201014), 0);
    lv_obj_set_flex_flow(btn_col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(btn_col, 6, 0);

    for (int i = 0; i < HERDR_MAX_OPTIONS; i++) {
        lv_obj_t *btn = lv_btn_create(btn_col);
        lv_obj_set_size(btn, LV_PCT(100), 40);
        lv_obj_set_style_bg_color(btn, COL_PANEL, 0);
        lv_obj_add_event_cb(btn, blocked_option_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        lv_obj_t *l = lv_label_create(btn);
        lv_label_set_text(l, "");
        lv_obj_set_style_text_font(l, &lv_font_montserrat_14, 0);
        lv_obj_center(l);
        s_blocked_btns[i] = btn;
    }
}

static void build_keyboard_overlay(void)
{
    s_kb_overlay = make_panel(lv_scr_act());
    lv_obj_set_size(s_kb_overlay, LV_HOR_RES, LV_VER_RES);
    lv_obj_add_flag(s_kb_overlay, LV_OBJ_FLAG_HIDDEN);

    s_kb_ta = lv_textarea_create(s_kb_overlay);
    lv_obj_set_size(s_kb_ta, LV_HOR_RES - 16, 76);
    lv_obj_align(s_kb_ta, LV_ALIGN_TOP_MID, 0, 8);
    lv_textarea_set_placeholder_text(s_kb_ta, "prompt para o agente...");
    lv_obj_set_style_text_font(s_kb_ta, &lv_font_montserrat_14, 0);

    s_keyboard = lv_keyboard_create(s_kb_overlay);
    lv_obj_set_size(s_keyboard, LV_HOR_RES, LV_VER_RES / 2);
    lv_keyboard_set_textarea(s_keyboard, s_kb_ta);
    lv_obj_add_event_cb(s_keyboard, kb_event_cb, LV_EVENT_READY, NULL);
    lv_obj_add_event_cb(s_keyboard, kb_event_cb, LV_EVENT_CANCEL, NULL);
}

/* ---------- refresh ---------- */

static void refresh_conn(void)
{
    herdr_conn_state_t conn = herdr_model_get_conn();
    switch (conn) {
    case HERDR_CONN_ONLINE:
        lv_obj_set_style_bg_color(s_conn_dot, COL_IDLE, 0);
        lv_label_set_text(s_conn_label, "online");
        break;
    case HERDR_CONN_CONNECTING:
        lv_obj_set_style_bg_color(s_conn_dot, COL_WORKING, 0);
        lv_label_set_text(s_conn_label, "conectando");
        break;
    default:
        lv_obj_set_style_bg_color(s_conn_dot, COL_OFFLINE, 0);
        lv_label_set_text(s_conn_label, "offline");
        break;
    }
}

static void refresh_blocked(void)
{
    bool active = herdr_model_get_blocked(&s_ui_blocked);
    s_ui_blocked.active = active;
    if (active) {
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

static void refresh_detail(void)
{
    if (!s_detail_open) {
        return;
    }
    /* status do agente no header do detalhe */
    for (int i = 0; i < s_ui_agent_count; i++) {
        if (strcmp(s_ui_agents[i].pane_id, s_detail_pane) == 0) {
            lv_obj_set_style_bg_color(s_detail_dot, status_color(s_ui_agents[i].status), 0);
            break;
        }
    }
    /* static: a struct tem ~8KB e a task da LVGL só tem 4KB de stack.
       Seguro porque esta função roda exclusivamente na task da LVGL. */
    static herdr_pane_content_t content;
    if (herdr_model_get_pane_content(&content) &&
        strcmp(content.pane_id, s_detail_pane) == 0 &&
        strcmp(lv_label_get_text(s_term_label), content.content) != 0) {
        lv_label_set_text(s_term_label, content.content);
        lv_obj_scroll_to_y(s_term_cont, LV_COORD_MAX, LV_ANIM_OFF);
    }
}

static void ui_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    /* read_pane periódico com o detalhe aberto */
    if (s_detail_open && ++s_poll_tick >= DETAIL_POLL_TICKS) {
        s_poll_tick = 0;
        herdr_conn_read_pane(s_detail_pane, 40);
    }

    uint32_t gen = herdr_model_generation();
    if (gen == s_last_generation) {
        return;
    }
    s_last_generation = gen;

    s_ui_agent_count = herdr_model_get_agents(s_ui_agents, HERDR_MAX_AGENTS);
    refresh_conn();
    if (!s_detail_open) {
        rebuild_list_rows();
    }
    refresh_detail();
    refresh_blocked();
}

void herdr_ui_init(void)
{
    lv_obj_set_style_bg_color(lv_scr_act(), COL_BG, 0);

    build_header();
    build_list_panel();
    build_detail_panel();
    build_blocked_modal();
    build_keyboard_overlay();
    rebuild_list_rows();

    lv_timer_create(ui_timer_cb, 500, NULL);
}
