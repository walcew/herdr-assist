#include "herdr_ui_settings.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <lvgl.h>

#include "esp_system.h"

#include "net.h"
#include "pairing.h"
#include "panel_cfg.h"
#include "ui_theme.h"

#define MAX_APS    12
#define KB_H       (LV_VER_RES / 2)

typedef enum {
    VIEW_MAIN,
    VIEW_SCAN,
    VIEW_PASS,
    VIEW_HOST,
    VIEW_PAIR,
} view_t;

static lv_obj_t *s_panel;
static lv_obj_t *s_bar;
static lv_obj_t *s_content;
static lv_obj_t *s_dock;
static lv_obj_t *s_toast;
static lv_obj_t *s_kb;
static lv_obj_t *s_ta_pass;
static lv_obj_t *s_ta_name;
static lv_obj_t *s_ta_host;
static lv_obj_t *s_ta_port;
static lv_obj_t *s_ta_token;
static lv_obj_t *s_pair_status;
static lv_timer_t *s_pair_timer;

static panel_cfg_t s_edit;          /* cópia em edição; só vale ao salvar */
static view_t s_view;
static int s_edit_host;             /* slot em edição na VIEW_HOST */
static char s_sel_ssid[CFG_SSID_LEN];
static net_ap_t s_aps[MAX_APS];
static int s_ap_count;

static void show_main(void);
static void show_scan(void);
static void show_pass(const char *ssid);
static void show_host(int idx);
static void show_pair(void);
static void update_toast(void);

/* ---------- teclado ---------- */

static void set_content_height(lv_coord_t h)
{
    lv_obj_set_height(s_content, h);
}

static void hide_kb(void)
{
    lv_obj_add_flag(s_kb, LV_OBJ_FLAG_HIDDEN);
    set_content_height(LV_VER_RES - UI_TOPBAR_H);
}

static void show_kb(lv_obj_t *ta, lv_keyboard_mode_t mode)
{
    lv_keyboard_set_textarea(s_kb, ta);
    lv_keyboard_set_mode(s_kb, mode);
    lv_obj_clear_flag(s_kb, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_kb);
    /* encolhe a área útil até o topo do teclado e traz o campo para ela */
    set_content_height(LV_VER_RES - KB_H - UI_TOPBAR_H);
    lv_obj_scroll_to_view(ta, LV_ANIM_OFF);
}

static void ta_focus_cb(lv_event_t *e)
{
    lv_obj_t *ta = lv_event_get_target(e);
    show_kb(ta, ta == s_ta_port ? LV_KEYBOARD_MODE_NUMBER : LV_KEYBOARD_MODE_TEXT_LOWER);
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

/* ---------- peças ---------- */

static lv_obj_t *make_row(lv_event_cb_t cb, void *user_data, lv_coord_t h)
{
    lv_obj_t *row = lv_btn_create(s_content);
    lv_obj_set_size(row, LV_PCT(100), h);
    lv_obj_set_style_bg_color(row, UI_PANEL, 0);
    lv_obj_set_style_radius(row, 6, 0);
    lv_obj_set_style_shadow_width(row, 0, 0);
    lv_obj_set_style_pad_left(row, 12, 0);
    lv_obj_set_style_pad_right(row, 12, 0);
    if (cb) {
        lv_obj_add_event_cb(row, cb, LV_EVENT_CLICKED, user_data);
    }
    return row;
}

static void make_section_label(const char *text)
{
    lv_obj_t *l = lv_label_create(s_content);
    lv_label_set_text(l, text);
    lv_obj_set_style_text_font(l, &lv_font_ui_12, 0);
    lv_obj_set_style_text_color(l, UI_MUTED, 0);
}

static lv_obj_t *make_field(const char *label, const char *value, const char *placeholder)
{
    lv_obj_t *row = ui_plain(s_content);
    lv_obj_set_size(row, LV_PCT(100), 44);

    lv_obj_t *l = lv_label_create(row);
    lv_label_set_text(l, label);
    lv_obj_set_style_text_font(l, &lv_font_ui_12, 0);
    lv_obj_set_style_text_color(l, UI_MUTED, 0);
    lv_obj_set_width(l, 86);
    lv_obj_align(l, LV_ALIGN_LEFT_MID, 0, 0);

    lv_obj_t *ta = lv_textarea_create(row);
    lv_textarea_set_one_line(ta, true);
    lv_textarea_set_text(ta, value);
    lv_textarea_set_placeholder_text(ta, placeholder);
    lv_obj_set_style_text_font(ta, &lv_font_ui_14, 0);
    lv_obj_set_style_bg_color(ta, UI_PANEL, 0);
    lv_obj_set_style_border_color(ta, UI_SWITCH_OFF, 0);
    lv_obj_set_style_border_width(ta, 1, 0);
    lv_obj_set_style_radius(ta, 6, 0);
    /* largura do conteúdo (tela - padding) menos o rótulo de 86px */
    lv_obj_set_size(ta, LV_HOR_RES - 16 - 90, 40);
    lv_obj_align(ta, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_add_event_cb(ta, ta_focus_cb, LV_EVENT_FOCUSED, NULL);
    return ta;
}

/* Reconstrói a topbar da view atual. back/save = NULL quando não se aplicam. */
static void build_bar(const char *title, lv_event_cb_t back, lv_event_cb_t save)
{
    lv_obj_clean(s_bar);
    if (back) {
        ui_icon_btn(s_bar, LV_SYMBOL_LEFT, back, NULL);
    }
    lv_obj_t *t = lv_label_create(s_bar);
    lv_label_set_text(t, title);
    lv_obj_set_style_text_font(t, &lv_font_ui_bold_20, 0);
    lv_obj_set_style_text_color(t, UI_TEXT, 0);
    lv_label_set_long_mode(t, LV_LABEL_LONG_DOT);
    lv_obj_set_flex_grow(t, 1);
    if (save) {
        lv_obj_t *b = ui_icon_btn(s_bar, LV_SYMBOL_SAVE, save, NULL);
        lv_obj_set_style_bg_color(b, UI_IDLE, 0);
        lv_obj_set_style_border_color(b, UI_IDLE, 0);
        lv_obj_set_style_text_color(lv_obj_get_child(b, 0), UI_TERM_BG, 0);
    }
}

/* ---------- view: senha do Wi-Fi ---------- */

static void back_to_scan_cb(lv_event_t *e)
{
    (void)e;
    hide_kb();
    show_scan();
}

static void show_pass(const char *ssid)
{
    s_view = VIEW_PASS;
    strlcpy(s_sel_ssid, ssid, CFG_SSID_LEN);
    lv_obj_add_flag(s_dock, LV_OBJ_FLAG_HIDDEN);
    update_toast();
    build_bar("Senha", back_to_scan_cb, NULL);
    lv_obj_clean(s_content);

    lv_obj_t *l = lv_label_create(s_content);
    lv_label_set_text_fmt(l, "Rede: %s", ssid);
    lv_obj_set_style_text_font(l, &lv_font_ui_14, 0);
    lv_obj_set_style_text_color(l, UI_TEXT, 0);

    s_ta_pass = lv_textarea_create(s_content);
    lv_textarea_set_one_line(s_ta_pass, true);
    lv_textarea_set_password_mode(s_ta_pass, true);
    lv_textarea_set_placeholder_text(s_ta_pass, "Senha (vazio se aberta)");
    lv_obj_set_style_text_font(s_ta_pass, &lv_font_ui_14, 0);
    lv_obj_set_style_bg_color(s_ta_pass, UI_PANEL, 0);
    lv_obj_set_style_border_color(s_ta_pass, UI_SWITCH_OFF, 0);
    lv_obj_set_width(s_ta_pass, LV_PCT(100));

    show_kb(s_ta_pass, LV_KEYBOARD_MODE_TEXT_LOWER);
}

/* ---------- view: redes Wi-Fi ---------- */

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

static void back_to_main_cb(lv_event_t *e)
{
    (void)e;
    hide_kb();
    show_main();
}

static void scan_timer_cb(lv_timer_t *t)
{
    (void)t;
    /* bloqueia a task da LVGL ~2s; o aviso "Buscando" já está na tela */
    s_ap_count = net_wifi_scan(s_aps, MAX_APS);
    if (s_view != VIEW_SCAN) {
        return;                         /* usuário saiu da tela durante o scan */
    }
    lv_obj_clean(s_content);
    if (s_ap_count <= 0) {
        lv_obj_t *l = lv_label_create(s_content);
        lv_label_set_text(l, s_ap_count == 0 ? "Nenhuma rede encontrada."
                                             : "Falha ao buscar redes.");
        lv_obj_set_style_text_color(l, UI_MUTED, 0);
        lv_obj_set_style_text_font(l, &lv_font_ui_14, 0);
        lv_obj_t *row = make_row(scan_retry_cb, NULL, 40);
        lv_obj_t *rl = lv_label_create(row);
        lv_label_set_text(rl, "Tentar de novo");
        lv_obj_set_style_text_font(rl, &lv_font_ui_14, 0);
        lv_obj_center(rl);
        return;
    }
    for (int i = 0; i < s_ap_count; i++) {
        lv_obj_t *row = make_row(ap_clicked_cb, (void *)(intptr_t)i, 40);

        lv_obj_t *name = lv_label_create(row);
        lv_label_set_text(name, s_aps[i].ssid);
        lv_obj_set_style_text_font(name, &lv_font_ui_14, 0);
        lv_obj_set_style_text_color(name, UI_TEXT, 0);
        /* SSID longo não pode invadir o RSSI à direita */
        lv_obj_set_width(name, LV_HOR_RES - 150);
        lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
        lv_obj_align(name, LV_ALIGN_LEFT_MID, 0, 0);

        lv_obj_t *info = lv_label_create(row);
        lv_label_set_text_fmt(info, "%s%d dBm", s_aps[i].secure ? LV_SYMBOL_WIFI "  " : "",
                              s_aps[i].rssi);
        lv_obj_set_style_text_font(info, &lv_font_ui_12, 0);
        lv_obj_set_style_text_color(info, UI_MUTED, 0);
        lv_obj_align(info, LV_ALIGN_RIGHT_MID, 0, 0);
    }
}

static void show_scan(void)
{
    s_view = VIEW_SCAN;
    lv_obj_add_flag(s_dock, LV_OBJ_FLAG_HIDDEN);
    update_toast();
    build_bar("Redes Wi-Fi", back_to_main_cb, NULL);
    hide_kb();
    lv_obj_clean(s_content);

    lv_obj_t *l = lv_label_create(s_content);
    lv_label_set_text(l, "Buscando redes...");
    lv_obj_set_style_text_font(l, &lv_font_ui_14, 0);
    lv_obj_set_style_text_color(l, UI_MUTED, 0);
    /* one-shot: deixa a tela pintar antes do scan bloqueante */
    lv_timer_t *t = lv_timer_create(scan_timer_cb, 60, NULL);
    lv_timer_set_repeat_count(t, 1);
}

/* ---------- view: editar host ---------- */

static void host_apply_cb(lv_event_t *e)
{
    (void)e;
    panel_host_t *h = &s_edit.hosts[s_edit_host];
    bool was_empty = h->host[0] == '\0';
    strlcpy(h->name, lv_textarea_get_text(s_ta_name), CFG_NAME_LEN);
    strlcpy(h->host, lv_textarea_get_text(s_ta_host), CFG_HOST_LEN);
    strlcpy(h->token, lv_textarea_get_text(s_ta_token), CFG_TOKEN_LEN);
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

static void show_host(int idx)
{
    s_view = VIEW_HOST;
    s_edit_host = idx;
    lv_obj_add_flag(s_dock, LV_OBJ_FLAG_HIDDEN);
    update_toast();
    build_bar("Editar host", back_to_main_cb, host_apply_cb);
    hide_kb();
    lv_obj_clean(s_content);

    const panel_host_t *h = &s_edit.hosts[idx];
    char port[8] = "9375";              /* porta padrão da ponte */
    if (h->port) {
        snprintf(port, sizeof(port), "%u", h->port);
    }

    s_ta_name  = make_field("Nome", h->name, "ex: mac");
    s_ta_host  = make_field("Endereço", h->host, "IP ou hostname");
    s_ta_port  = make_field("Porta", port, "9375");
    /* token da ponte deste host (ação show-token do plugin exibe no Herdr) */
    s_ta_token = make_field("Token", h->token, "32 hex da ponte");

    lv_obj_t *rm = lv_btn_create(s_content);
    lv_obj_set_size(rm, LV_PCT(100), 40);
    lv_obj_set_style_bg_color(rm, UI_BLOCKED, 0);
    lv_obj_set_style_radius(rm, 6, 0);
    lv_obj_set_style_shadow_width(rm, 0, 0);
    lv_obj_add_event_cb(rm, host_remove_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *rl = lv_label_create(rm);
    lv_label_set_text(rl, LV_SYMBOL_TRASH "  Remover");
    lv_obj_set_style_text_font(rl, &lv_font_ui_14, 0);
    lv_obj_set_style_text_color(rl, UI_TEXT, 0);
    lv_obj_center(rl);
}

/* ---------- view: pareamento ---------- */

static void pair_leave(void)
{
    if (s_pair_timer) {
        lv_timer_del(s_pair_timer);
        s_pair_timer = NULL;
    }
    pairing_stop();
}

static void back_from_pair_cb(lv_event_t *e)
{
    (void)e;
    pair_leave();
    show_main();
}

/**
 * Grava o host recebido e reinicia.
 *
 * A gravação acontece aqui, na task da LVGL, e não na task de pareamento:
 * assim a NVS tem um único escritor, como no resto da tela. Parte da config
 * salva (não de s_edit) porque o pareamento é uma ação completa em si — o que
 * estiver pendente de salvar continua pendente, sem virar efeito colateral.
 */
static void pair_apply(const panel_host_t *h)
{
    panel_cfg_t cfg = *panel_cfg_get();
    int slot = -1;
    for (int i = 0; i < CFG_MAX_HOSTS; i++) {
        /* reparear o mesmo endereço atualiza o slot em vez de duplicar */
        if (strcmp(cfg.hosts[i].host, h->host) == 0 && cfg.hosts[i].host[0]) {
            slot = i;
            break;
        }
        if (slot < 0 && cfg.hosts[i].host[0] == '\0') {
            slot = i;
        }
    }
    if (slot < 0) {
        lv_label_set_text(s_pair_status, "Sem espaço: remova um host antes de parear.");
        lv_obj_set_style_text_color(s_pair_status, UI_BLOCKED, 0);
        return;
    }
    cfg.hosts[slot] = *h;
    panel_cfg_save(&cfg);
    lv_label_set_text_fmt(s_pair_status, "Pareado com %s.\nReiniciando...", h->name);
    lv_obj_set_style_text_color(s_pair_status, UI_IDLE, 0);
    lv_refr_now(NULL);            /* pinta o aviso antes de sumir a tela */
    esp_restart();
}

static void pair_tick_cb(lv_timer_t *t)
{
    (void)t;
    panel_host_t h;
    switch (pairing_state()) {
    case PAIRING_DONE:
        if (pairing_result(&h)) {
            pair_leave();
            pair_apply(&h);
        }
        break;
    case PAIRING_WAITING:
        lv_label_set_text_fmt(s_pair_status, "Aguardando um host... (%ds)",
                              pairing_seconds_left());
        break;
    default:
        lv_label_set_text(s_pair_status, "Janela encerrada. Volte e tente de novo.");
        lv_obj_set_style_text_color(s_pair_status, UI_MUTED, 0);
        pair_leave();
        break;
    }
}

static void show_pair(void)
{
    s_view = VIEW_PAIR;
    lv_obj_add_flag(s_dock, LV_OBJ_FLAG_HIDDEN);
    update_toast();
    build_bar("Parear", back_from_pair_cb, NULL);
    hide_kb();
    lv_obj_clean(s_content);

    lv_obj_t *card = ui_card(s_content, 8);
    lv_obj_set_size(card, LV_PCT(100), 150);
    lv_obj_set_style_pad_all(card, 12, 0);

    lv_obj_t *cap = lv_label_create(card);
    lv_label_set_text(cap, "Este painel");
    lv_obj_set_style_text_font(cap, &lv_font_ui_12, 0);
    lv_obj_set_style_text_color(cap, UI_MUTED, 0);
    lv_obj_align(cap, LV_ALIGN_TOP_MID, 0, 0);

    lv_obj_t *id = lv_label_create(card);
    lv_label_set_text(id, pairing_device_id());
    lv_obj_set_style_text_font(id, &lv_font_ui_clock_44, 0);
    lv_obj_set_style_text_color(id, UI_TEXT, 0);
    lv_obj_align(id, LV_ALIGN_TOP_MID, 0, 22);

    lv_obj_t *hint = lv_label_create(card);
    lv_label_set_text(hint, "Escolha este código no host");
    lv_obj_set_style_text_font(hint, &lv_font_ui_12, 0);
    lv_obj_set_style_text_color(hint, UI_MUTED, 0);
    lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, 0);

    s_pair_status = lv_label_create(s_content);
    lv_label_set_text(s_pair_status, "Iniciando...");
    lv_obj_set_style_text_font(s_pair_status, &lv_font_ui_14, 0);
    lv_obj_set_style_text_color(s_pair_status, UI_WORKING, 0);
    lv_obj_set_width(s_pair_status, LV_PCT(100));
    lv_label_set_long_mode(s_pair_status, LV_LABEL_LONG_WRAP);

    lv_obj_t *steps = lv_label_create(s_content);
    lv_label_set_text(steps,
        "No host, com o Herdr aberto:\n\n"
        "1. Abra o painel do plugin herdr-assist\n"
        "2. Escolha \"Parear painel\"\n"
        "3. Selecione o código acima\n\n"
        "O host envia nome, endereço e token —\n"
        "nada precisa ser digitado aqui.");
    lv_obj_set_style_text_font(steps, &lv_font_ui_12, 0);
    lv_obj_set_style_text_color(steps, UI_MUTED, 0);
    lv_obj_set_width(steps, LV_PCT(100));
    lv_label_set_long_mode(steps, LV_LABEL_LONG_WRAP);

    if (pairing_start() != ESP_OK) {
        lv_label_set_text(s_pair_status, "Não foi possível abrir a porta de pareamento.");
        lv_obj_set_style_text_color(s_pair_status, UI_BLOCKED, 0);
        return;
    }
    s_pair_timer = lv_timer_create(pair_tick_cb, 500, NULL);
}

/* ---------- view: principal ---------- */

static void pair_open_cb(lv_event_t *e)
{
    (void)e;
    show_pair();
}

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
    update_toast();   /* único caminho de edição que não reconstrói a tela */
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

/**
 * true se a cópia em edição já difere do que está gravado.
 *
 * memcmp basta aqui: os dois lados nascem da mesma struct zerada em
 * panel_cfg_init(), e as edições só usam strlcpy/memset — então os bytes
 * depois do terminador acompanham, sem lixo capaz de gerar falso positivo.
 */
static bool cfg_dirty(void)
{
    return memcmp(&s_edit, panel_cfg_get(), sizeof(s_edit)) != 0;
}

static void save_cb(lv_event_t *e)
{
    (void)e;
    panel_cfg_save(&s_edit);
    lv_obj_clean(s_content);
    lv_obj_t *l = lv_label_create(s_content);
    lv_label_set_text(l, "Reiniciando...");
    lv_obj_set_style_text_font(l, &lv_font_ui_16, 0);
    lv_obj_set_style_text_color(l, UI_TEXT, 0);
    /* reinício limpa Wi-Fi e conexões; mais simples e confiável que teardown */
    esp_restart();
}

/** O aviso de pendência só existe na tela principal, e só quando há mudança. */
static void update_toast(void)
{
    if (s_view == VIEW_MAIN && cfg_dirty()) {
        lv_obj_clear_flag(s_toast, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(s_toast);
    } else {
        lv_obj_add_flag(s_toast, LV_OBJ_FLAG_HIDDEN);
    }
}

static void show_main(void)
{
    s_view = VIEW_MAIN;
    lv_obj_clear_flag(s_dock, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_dock);
    build_bar("Configurações", NULL, save_cb);
    hide_kb();
    lv_obj_clean(s_content);

    make_section_label("Rede Wi-Fi");
    lv_obj_t *wrow = make_row(wifi_change_cb, NULL, 44);
    lv_obj_t *wl = lv_label_create(wrow);
    lv_label_set_text(wl, s_edit.wifi_ssid[0] ? s_edit.wifi_ssid : "Não configurada");
    lv_obj_set_style_text_font(wl, &lv_font_ui_14, 0);
    lv_obj_set_style_text_color(wl, UI_TEXT, 0);
    lv_obj_align(wl, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_t *wc = lv_label_create(wrow);
    lv_label_set_text(wc, "Trocar " LV_SYMBOL_RIGHT);
    lv_obj_set_style_text_font(wc, &lv_font_ui_12, 0);
    lv_obj_set_style_text_color(wc, UI_MUTED, 0);
    lv_obj_align(wc, LV_ALIGN_RIGHT_MID, 0, 0);

    make_section_label("Hosts herdr");
    bool has_free = false;
    for (int i = 0; i < CFG_MAX_HOSTS; i++) {
        const panel_host_t *h = &s_edit.hosts[i];
        if (h->host[0] == '\0') {
            has_free = true;
            continue;
        }
        lv_obj_t *row = make_row(host_row_cb, (void *)(intptr_t)i, 48);

        lv_obj_t *nm = lv_label_create(row);
        lv_label_set_text(nm, h->name[0] ? h->name : h->host);
        lv_obj_set_style_text_font(nm, &lv_font_ui_14, 0);
        lv_obj_set_style_text_color(nm, UI_TEXT, 0);
        lv_obj_align(nm, LV_ALIGN_LEFT_MID, 0, -9);

        lv_obj_t *ad = lv_label_create(row);
        lv_label_set_text_fmt(ad, "%s:%u", h->host, h->port);
        lv_obj_set_style_text_font(ad, &lv_font_ui_12, 0);
        lv_obj_set_style_text_color(ad, UI_MUTED, 0);
        lv_obj_align(ad, LV_ALIGN_LEFT_MID, 0, 10);

        lv_obj_t *sw = lv_switch_create(row);
        lv_obj_set_size(sw, 48, 26);
        /* 26 px de altura é alvo pequeno demais para o dedo: o toque que erra o
           switch cai na linha e abre a edição. A área estendida cobre a altura
           inteira da row, e o hit test só desce aos filhos dentro das coords da
           row — então nada é roubado das linhas vizinhas. */
        lv_obj_set_ext_click_area(sw, 12);
        lv_obj_align(sw, LV_ALIGN_RIGHT_MID, 0, 0);
        lv_obj_set_style_bg_color(sw, UI_SWITCH_OFF, 0);
        if (h->enabled) {
            lv_obj_add_state(sw, LV_STATE_CHECKED);
        }
        lv_obj_add_event_cb(sw, host_switch_cb, LV_EVENT_VALUE_CHANGED, (void *)(intptr_t)i);
    }
    if (has_free) {
        /* caminho principal: o host manda a config pronta, sem digitação */
        lv_obj_t *pair = make_row(pair_open_cb, NULL, 44);
        lv_obj_set_style_bg_color(pair, UI_IDLE, 0);
        lv_obj_t *pl = lv_label_create(pair);
        lv_label_set_text(pl, LV_SYMBOL_WIFI "  Parear com um host");
        lv_obj_set_style_text_font(pl, &lv_font_ui_14, 0);
        lv_obj_set_style_text_color(pl, UI_TERM_BG, 0);
        lv_obj_center(pl);

        lv_obj_t *add = make_row(add_host_cb, NULL, 44);
        lv_obj_t *al = lv_label_create(add);
        lv_label_set_text(al, LV_SYMBOL_PLUS "  Adicionar manualmente");
        lv_obj_set_style_text_font(al, &lv_font_ui_14, 0);
        lv_obj_set_style_text_color(al, UI_MUTED, 0);
        lv_obj_center(al);
    }
    update_toast();
}

/* ---------- init/show ---------- */

void herdr_ui_settings_init(lv_event_cb_t dock_cb)
{
    s_panel = ui_screen();

    s_bar = ui_topbar(s_panel, NULL, NULL);

    s_content = ui_plain(s_panel);
    lv_obj_set_size(s_content, LV_HOR_RES, LV_VER_RES - UI_TOPBAR_H);
    lv_obj_align(s_content, LV_ALIGN_TOP_MID, 0, UI_TOPBAR_H);
    lv_obj_set_style_pad_left(s_content, UI_PAD, 0);
    lv_obj_set_style_pad_right(s_content, UI_PAD, 0);
    lv_obj_set_style_pad_bottom(s_content, UI_DOCK_SPACE, 0);
    lv_obj_set_style_pad_row(s_content, 6, 0);
    lv_obj_set_flex_flow(s_content, LV_FLEX_FLOW_COLUMN);
    lv_obj_add_flag(s_content, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(s_content, LV_DIR_VER);

    s_dock = ui_dock(s_panel, UI_TAB_SETTINGS, dock_cb);

    /* Flutua logo acima do dock: a edição fica só em memória até salvar, e sem
       este aviso não há como perceber que falta aplicar. Tocar salva e reinicia. */
    s_toast = lv_btn_create(s_panel);
    lv_obj_set_size(s_toast, LV_HOR_RES - 24, 56);
    lv_obj_align(s_toast, LV_ALIGN_BOTTOM_MID, 0, -68);
    lv_obj_set_style_bg_color(s_toast, UI_PANEL, 0);
    lv_obj_set_style_border_width(s_toast, 1, 0);
    lv_obj_set_style_border_color(s_toast, UI_WORKING, 0);
    lv_obj_set_style_radius(s_toast, 8, 0);
    lv_obj_set_style_shadow_width(s_toast, 20, 0);
    lv_obj_set_style_shadow_color(s_toast, lv_color_black(), 0);
    lv_obj_set_style_shadow_ofs_y(s_toast, 6, 0);
    lv_obj_add_flag(s_toast, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(s_toast, save_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *ticon = lv_label_create(s_toast);
    lv_label_set_text(ticon, LV_SYMBOL_SAVE);
    lv_obj_set_style_text_font(ticon, &lv_font_ui_20, 0);
    lv_obj_set_style_text_color(ticon, UI_WORKING, 0);
    lv_obj_align(ticon, LV_ALIGN_LEFT_MID, 0, 0);

    lv_obj_t *t1 = lv_label_create(s_toast);
    lv_label_set_text(t1, "Configurações pendentes");
    lv_obj_set_style_text_font(t1, &lv_font_ui_14, 0);
    lv_obj_set_style_text_color(t1, UI_TEXT, 0);
    lv_obj_align(t1, LV_ALIGN_LEFT_MID, 32, -9);

    lv_obj_t *t2 = lv_label_create(s_toast);
    lv_label_set_text(t2, "Toque para aplicar e reiniciar");
    lv_obj_set_style_text_font(t2, &lv_font_ui_12, 0);
    lv_obj_set_style_text_color(t2, UI_MUTED, 0);
    lv_obj_align(t2, LV_ALIGN_LEFT_MID, 32, 10);

    s_kb = lv_keyboard_create(s_panel);
    lv_obj_set_size(s_kb, LV_HOR_RES, KB_H);
    lv_obj_set_style_text_font(s_kb, &lv_font_ui_14, 0);
    lv_obj_add_flag(s_kb, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(s_kb, kb_cb, LV_EVENT_READY, NULL);
    lv_obj_add_event_cb(s_kb, kb_cb, LV_EVENT_CANCEL, NULL);
}

void herdr_ui_settings_show(void)
{
    s_edit = *panel_cfg_get();
    show_main();
    lv_obj_clear_flag(s_panel, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_panel);
}

void herdr_ui_settings_hide(void)
{
    hide_kb();
    lv_obj_add_flag(s_panel, LV_OBJ_FLAG_HIDDEN);
}
