#include "herdr_ui_settings.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <lvgl.h>

#include "esp_system.h"

#include "net.h"
#include "panel_cfg.h"

/* Mesma paleta da herdr_ui.c */
#define COL_BG        lv_color_hex(0x14161a)
#define COL_PANEL     lv_color_hex(0x1e2127)
#define COL_TEXT      lv_color_hex(0xe8e8e8)
#define COL_MUTED     lv_color_hex(0x8a8f98)
#define COL_OK        lv_color_hex(0x4caf50)
#define COL_WARN      lv_color_hex(0xffb300)
#define COL_DANGER    lv_color_hex(0xef5350)

#define HEADER_H   36
#define MAX_APS    12

typedef enum {
    VIEW_MAIN,
    VIEW_SCAN,
    VIEW_PASS,
    VIEW_HOST,
} view_t;

static lv_obj_t *s_panel;
static lv_obj_t *s_title;
static lv_obj_t *s_btn_save;
static lv_obj_t *s_content;
static lv_obj_t *s_kb;
static lv_obj_t *s_ta_pass;
static lv_obj_t *s_ta_name;
static lv_obj_t *s_ta_host;
static lv_obj_t *s_ta_port;

static panel_cfg_t s_edit;          /* cópia em edição; vale ao salvar */
static view_t s_view;
static int s_edit_host;             /* slot em edição na VIEW_HOST */
static char s_sel_ssid[CFG_SSID_LEN];
static net_ap_t s_aps[MAX_APS];
static int s_ap_count;

static void show_main(void);
static void show_scan(void);
static void show_pass(const char *ssid);
static void show_host(int idx);

/* ---------- helpers ---------- */

static void hide_kb(void)
{
    lv_obj_add_flag(s_kb, LV_OBJ_FLAG_HIDDEN);
}

static lv_obj_t *make_btn(lv_obj_t *parent, const char *text, lv_event_cb_t cb,
                          void *user_data, lv_color_t bg)
{
    lv_obj_t *btn = lv_btn_create(parent);
    lv_obj_set_style_bg_color(btn, bg, 0);
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, user_data);
    lv_obj_t *l = lv_label_create(btn);
    lv_label_set_text(l, text);
    lv_obj_set_style_text_font(l, &lv_font_montserrat_14, 0);
    lv_obj_center(l);
    return btn;
}

static lv_obj_t *make_section_label(const char *text)
{
    lv_obj_t *l = lv_label_create(s_content);
    lv_label_set_text(l, text);
    lv_obj_set_style_text_font(l, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(l, COL_MUTED, 0);
    return l;
}

static lv_obj_t *make_ta(lv_obj_t *parent, const char *label, const char *value,
                         const char *placeholder)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_set_size(row, LV_PCT(100), 44);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *l = lv_label_create(row);
    lv_label_set_text(l, label);
    lv_obj_set_style_text_font(l, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(l, COL_MUTED, 0);
    lv_obj_align(l, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_set_width(l, 86);

    lv_obj_t *ta = lv_textarea_create(row);
    lv_textarea_set_one_line(ta, true);
    lv_textarea_set_text(ta, value);
    lv_textarea_set_placeholder_text(ta, placeholder);
    lv_obj_set_style_text_font(ta, &lv_font_montserrat_14, 0);
    /* largura do conteudo (tela - padding) menos o rotulo de 86px */
    lv_obj_set_size(ta, LV_HOR_RES - 16 - 90, 40);
    lv_obj_align(ta, LV_ALIGN_RIGHT_MID, 0, 0);
    return ta;
}

/* ---------- teclado ---------- */

static void ta_focus_cb(lv_event_t *e)
{
    lv_obj_t *ta = lv_event_get_target(e);
    lv_keyboard_set_textarea(s_kb, ta);
    lv_keyboard_set_mode(s_kb, ta == s_ta_port
                         ? LV_KEYBOARD_MODE_NUMBER : LV_KEYBOARD_MODE_TEXT_LOWER);
    lv_obj_clear_flag(s_kb, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_kb);
}

static void kb_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if (s_view == VIEW_PASS) {
        if (code == LV_EVENT_READY) {   /* checkmark: adota a rede escolhida */
            strlcpy(s_edit.wifi_ssid, s_sel_ssid, CFG_SSID_LEN);
            strlcpy(s_edit.wifi_pass, lv_textarea_get_text(s_ta_pass), CFG_PASS_LEN);
        }
        hide_kb();
        show_main();
        return;
    }
    hide_kb();                          /* nas demais telas só recolhe */
}

/* ---------- view: senha do Wi-Fi ---------- */

static void show_pass(const char *ssid)
{
    s_view = VIEW_PASS;
    strlcpy(s_sel_ssid, ssid, CFG_SSID_LEN);
    lv_obj_add_flag(s_btn_save, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clean(s_content);

    lv_obj_t *l = lv_label_create(s_content);
    lv_label_set_text_fmt(l, "rede: %s", ssid);
    lv_obj_set_style_text_font(l, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(l, COL_TEXT, 0);

    s_ta_pass = lv_textarea_create(s_content);
    lv_textarea_set_one_line(s_ta_pass, true);
    lv_textarea_set_password_mode(s_ta_pass, true);
    lv_textarea_set_placeholder_text(s_ta_pass, "senha (vazio se aberta)");
    lv_obj_set_style_text_font(s_ta_pass, &lv_font_montserrat_14, 0);
    lv_obj_set_width(s_ta_pass, LV_PCT(100));

    lv_keyboard_set_textarea(s_kb, s_ta_pass);
    lv_keyboard_set_mode(s_kb, LV_KEYBOARD_MODE_TEXT_LOWER);
    lv_obj_clear_flag(s_kb, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_kb);
}

/* ---------- view: scan de redes ---------- */

static void ap_clicked_cb(lv_event_t *e)
{
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    if (idx < s_ap_count) {
        show_pass(s_aps[idx].ssid);
    }
}

static void scan_retry_cb(lv_event_t *e)
{
    (void)e;
    show_scan();
}

static void scan_timer_cb(lv_timer_t *t)
{
    (void)t;
    /* bloqueia a task da LVGL ~2s; o aviso "buscando" já está na tela */
    s_ap_count = net_wifi_scan(s_aps, MAX_APS);
    if (s_view != VIEW_SCAN) {
        return;                         /* usuário saiu da tela durante o scan */
    }
    lv_obj_clean(s_content);
    if (s_ap_count <= 0) {
        lv_obj_t *l = lv_label_create(s_content);
        lv_label_set_text(l, s_ap_count == 0 ? "nenhuma rede encontrada"
                                             : "falha ao buscar redes");
        lv_obj_set_style_text_color(l, COL_MUTED, 0);
        lv_obj_set_style_text_font(l, &lv_font_montserrat_14, 0);
        lv_obj_t *btn = make_btn(s_content, "tentar de novo", scan_retry_cb, NULL, COL_PANEL);
        lv_obj_set_size(btn, LV_PCT(100), 40);
        return;
    }
    for (int i = 0; i < s_ap_count; i++) {
        lv_obj_t *row = lv_btn_create(s_content);
        lv_obj_set_size(row, LV_PCT(100), 40);
        lv_obj_set_style_bg_color(row, COL_PANEL, 0);
        lv_obj_add_event_cb(row, ap_clicked_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);

        lv_obj_t *name = lv_label_create(row);
        lv_label_set_text(name, s_aps[i].ssid);
        lv_obj_set_style_text_font(name, &lv_font_montserrat_14, 0);
        lv_obj_align(name, LV_ALIGN_LEFT_MID, 0, 0);

        lv_obj_t *info = lv_label_create(row);
        lv_label_set_text_fmt(info, "%s%d dBm", s_aps[i].secure ? LV_SYMBOL_EYE_CLOSE "  " : "",
                              s_aps[i].rssi);
        lv_obj_set_style_text_font(info, &lv_font_montserrat_12, 0);
        lv_obj_set_style_text_color(info, COL_MUTED, 0);
        lv_obj_align(info, LV_ALIGN_RIGHT_MID, 0, 0);
    }
}

static void show_scan(void)
{
    s_view = VIEW_SCAN;
    lv_obj_add_flag(s_btn_save, LV_OBJ_FLAG_HIDDEN);
    hide_kb();
    lv_obj_clean(s_content);
    lv_obj_t *l = lv_label_create(s_content);
    lv_label_set_text(l, "buscando redes...");
    lv_obj_set_style_text_font(l, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(l, COL_MUTED, 0);
    /* one-shot: deixa a tela pintar antes do scan bloqueante */
    lv_timer_t *t = lv_timer_create(scan_timer_cb, 60, NULL);
    lv_timer_set_repeat_count(t, 1);
}

/* ---------- view: editor de host ---------- */

static void host_ok_cb(lv_event_t *e)
{
    (void)e;
    panel_host_t *h = &s_edit.hosts[s_edit_host];
    bool was_empty = h->host[0] == '\0';
    strlcpy(h->name, lv_textarea_get_text(s_ta_name), CFG_NAME_LEN);
    strlcpy(h->host, lv_textarea_get_text(s_ta_host), CFG_HOST_LEN);
    h->port = (uint16_t)atoi(lv_textarea_get_text(s_ta_port));
    if (!h->host[0] || !h->port) {
        h->enabled = false;             /* incompleto não conecta */
    } else if (was_empty) {
        h->enabled = true;              /* host novo entra habilitado */
    }
    hide_kb();
    show_main();
}

static void host_remove_cb(lv_event_t *e)
{
    (void)e;
    memset(&s_edit.hosts[s_edit_host], 0, sizeof(panel_host_t));
    hide_kb();
    show_main();
}

static void host_cancel_cb(lv_event_t *e)
{
    (void)e;
    hide_kb();
    show_main();
}

static void show_host(int idx)
{
    s_view = VIEW_HOST;
    s_edit_host = idx;
    lv_obj_add_flag(s_btn_save, LV_OBJ_FLAG_HIDDEN);
    hide_kb();
    lv_obj_clean(s_content);

    const panel_host_t *h = &s_edit.hosts[idx];
    char port[8] = "9375";              /* porta padrao da ponte */
    if (h->port) {
        snprintf(port, sizeof(port), "%u", h->port);
    }

    s_ta_name = make_ta(s_content, "nome", h->name, "ex: mac");
    s_ta_host = make_ta(s_content, "endereco", h->host, "ip ou hostname");
    s_ta_port = make_ta(s_content, "porta", port, "9375");
    lv_obj_add_event_cb(s_ta_name, ta_focus_cb, LV_EVENT_FOCUSED, NULL);
    lv_obj_add_event_cb(s_ta_host, ta_focus_cb, LV_EVENT_FOCUSED, NULL);
    lv_obj_add_event_cb(s_ta_port, ta_focus_cb, LV_EVENT_FOCUSED, NULL);

    lv_obj_t *bar = lv_obj_create(s_content);
    lv_obj_set_size(bar, LV_PCT(100), 48);
    lv_obj_set_style_bg_opa(bar, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(bar, 0, 0);
    lv_obj_set_style_pad_all(bar, 0, 0);
    lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(bar, 6, 0);
    lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *ok = make_btn(bar, "ok", host_ok_cb, NULL, COL_OK);
    lv_obj_set_flex_grow(ok, 2);
    lv_obj_set_height(ok, 40);
    lv_obj_t *rm = make_btn(bar, "remover", host_remove_cb, NULL, COL_DANGER);
    lv_obj_set_flex_grow(rm, 1);
    lv_obj_set_height(rm, 40);
    lv_obj_t *ca = make_btn(bar, "cancelar", host_cancel_cb, NULL, COL_PANEL);
    lv_obj_set_flex_grow(ca, 1);
    lv_obj_set_height(ca, 40);
}

/* ---------- view: principal ---------- */

static void wifi_change_cb(lv_event_t *e)
{
    (void)e;
    show_scan();
}

static void host_row_cb(lv_event_t *e)
{
    show_host((int)(intptr_t)lv_event_get_user_data(e));
}

static void host_switch_cb(lv_event_t *e)
{
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    lv_obj_t *sw = lv_event_get_target(e);
    s_edit.hosts[idx].enabled = lv_obj_has_state(sw, LV_STATE_CHECKED);
}

static void add_host_cb(lv_event_t *e)
{
    (void)e;
    for (int i = 0; i < CFG_MAX_HOSTS; i++) {
        if (s_edit.hosts[i].host[0] == '\0') {
            show_host(i);
            return;
        }
    }
}

static void show_main(void)
{
    s_view = VIEW_MAIN;
    lv_label_set_text(s_title, "configuracoes");
    lv_obj_clear_flag(s_btn_save, LV_OBJ_FLAG_HIDDEN);
    hide_kb();
    lv_obj_clean(s_content);

    make_section_label("rede wi-fi");
    lv_obj_t *wrow = lv_btn_create(s_content);
    lv_obj_set_size(wrow, LV_PCT(100), 44);
    lv_obj_set_style_bg_color(wrow, COL_PANEL, 0);
    lv_obj_add_event_cb(wrow, wifi_change_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *wl = lv_label_create(wrow);
    lv_label_set_text_fmt(wl, "%s", s_edit.wifi_ssid[0] ? s_edit.wifi_ssid : "nao configurada");
    lv_obj_set_style_text_font(wl, &lv_font_montserrat_14, 0);
    lv_obj_align(wl, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_t *wc = lv_label_create(wrow);
    lv_label_set_text(wc, "trocar " LV_SYMBOL_RIGHT);
    lv_obj_set_style_text_font(wc, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(wc, COL_MUTED, 0);
    lv_obj_align(wc, LV_ALIGN_RIGHT_MID, 0, 0);

    make_section_label("hosts herdr");
    bool has_free = false;
    for (int i = 0; i < CFG_MAX_HOSTS; i++) {
        const panel_host_t *h = &s_edit.hosts[i];
        if (h->host[0] == '\0') {
            has_free = true;
            continue;
        }
        lv_obj_t *row = lv_btn_create(s_content);
        lv_obj_set_size(row, LV_PCT(100), 48);
        lv_obj_set_style_bg_color(row, COL_PANEL, 0);
        lv_obj_add_event_cb(row, host_row_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);

        lv_obj_t *nm = lv_label_create(row);
        lv_label_set_text_fmt(nm, "%s", h->name[0] ? h->name : h->host);
        lv_obj_set_style_text_font(nm, &lv_font_montserrat_14, 0);
        lv_obj_align(nm, LV_ALIGN_LEFT_MID, 0, -9);

        lv_obj_t *ad = lv_label_create(row);
        lv_label_set_text_fmt(ad, "%s:%u", h->host, h->port);
        lv_obj_set_style_text_font(ad, &lv_font_montserrat_12, 0);
        lv_obj_set_style_text_color(ad, COL_MUTED, 0);
        lv_obj_align(ad, LV_ALIGN_LEFT_MID, 0, 10);

        lv_obj_t *sw = lv_switch_create(row);
        lv_obj_set_size(sw, 48, 26);
        lv_obj_align(sw, LV_ALIGN_RIGHT_MID, 0, 0);
        if (h->enabled) {
            lv_obj_add_state(sw, LV_STATE_CHECKED);
        }
        lv_obj_add_event_cb(sw, host_switch_cb, LV_EVENT_VALUE_CHANGED, (void *)(intptr_t)i);
    }
    if (has_free) {
        lv_obj_t *add = make_btn(s_content, LV_SYMBOL_PLUS "  adicionar host",
                                 add_host_cb, NULL, COL_PANEL);
        lv_obj_set_size(add, LV_PCT(100), 44);
    }
}

/* ---------- header ---------- */

static void back_cb(lv_event_t *e)
{
    (void)e;
    if (s_view == VIEW_MAIN) {
        hide_kb();
        lv_obj_add_flag(s_panel, LV_OBJ_FLAG_HIDDEN);   /* descarta s_edit */
    } else {
        show_main();
    }
}

static void save_cb(lv_event_t *e)
{
    (void)e;
    panel_cfg_save(&s_edit);
    lv_obj_clean(s_content);
    lv_obj_t *l = lv_label_create(s_content);
    lv_label_set_text(l, "reiniciando...");
    lv_obj_set_style_text_font(l, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(l, COL_TEXT, 0);
    /* reinício limpa Wi-Fi e conexões; mais simples e confiável que teardown */
    esp_restart();
}

/* ---------- init/open ---------- */

void herdr_ui_settings_init(void)
{
    s_panel = lv_obj_create(lv_scr_act());
    lv_obj_set_size(s_panel, LV_HOR_RES, LV_VER_RES);
    lv_obj_set_style_bg_color(s_panel, COL_BG, 0);
    lv_obj_set_style_border_width(s_panel, 0, 0);
    lv_obj_set_style_radius(s_panel, 0, 0);
    lv_obj_set_style_pad_all(s_panel, 0, 0);
    lv_obj_add_flag(s_panel, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(s_panel, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *hdr = lv_obj_create(s_panel);
    lv_obj_set_size(hdr, LV_HOR_RES, HEADER_H);
    lv_obj_align(hdr, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_bg_color(hdr, COL_PANEL, 0);
    lv_obj_set_style_border_width(hdr, 0, 0);
    lv_obj_set_style_radius(hdr, 0, 0);
    lv_obj_set_style_pad_all(hdr, 0, 0);
    lv_obj_clear_flag(hdr, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *back = lv_btn_create(hdr);
    lv_obj_set_size(back, 56, HEADER_H - 6);
    lv_obj_align(back, LV_ALIGN_LEFT_MID, 4, 0);
    lv_obj_set_style_bg_color(back, COL_BG, 0);
    lv_obj_add_event_cb(back, back_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *bl = lv_label_create(back);
    lv_label_set_text(bl, LV_SYMBOL_LEFT);
    lv_obj_center(bl);

    s_title = lv_label_create(hdr);
    lv_label_set_text(s_title, "configuracoes");
    lv_obj_set_style_text_font(s_title, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_title, COL_TEXT, 0);
    lv_obj_align(s_title, LV_ALIGN_LEFT_MID, 72, 0);

    s_btn_save = lv_btn_create(hdr);
    lv_obj_set_size(s_btn_save, 150, HEADER_H - 6);
    lv_obj_align(s_btn_save, LV_ALIGN_RIGHT_MID, -4, 0);
    lv_obj_set_style_bg_color(s_btn_save, COL_OK, 0);
    lv_obj_add_event_cb(s_btn_save, save_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *sl = lv_label_create(s_btn_save);
    lv_label_set_text(sl, "salvar + reiniciar");
    lv_obj_set_style_text_font(sl, &lv_font_montserrat_12, 0);
    lv_obj_center(sl);

    s_content = lv_obj_create(s_panel);
    lv_obj_set_size(s_content, LV_HOR_RES, LV_VER_RES - HEADER_H);
    lv_obj_align(s_content, LV_ALIGN_TOP_MID, 0, HEADER_H);
    lv_obj_set_style_bg_color(s_content, COL_BG, 0);
    lv_obj_set_style_border_width(s_content, 0, 0);
    lv_obj_set_style_radius(s_content, 0, 0);
    lv_obj_set_style_pad_all(s_content, 8, 0);
    lv_obj_set_style_pad_row(s_content, 6, 0);
    lv_obj_set_flex_flow(s_content, LV_FLEX_FLOW_COLUMN);

    s_kb = lv_keyboard_create(s_panel);
    lv_obj_set_size(s_kb, LV_HOR_RES, LV_VER_RES / 2);
    lv_obj_add_flag(s_kb, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(s_kb, kb_cb, LV_EVENT_READY, NULL);
    lv_obj_add_event_cb(s_kb, kb_cb, LV_EVENT_CANCEL, NULL);
}

void herdr_ui_settings_open(void)
{
    s_edit = *panel_cfg_get();
    show_main();
    lv_obj_clear_flag(s_panel, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_panel);
}
