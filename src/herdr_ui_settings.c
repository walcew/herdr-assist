#include "herdr_ui_settings.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <lvgl.h>

#include "esp_system.h"

#include "fw_update.h"
#include "herdr_kb.h"
#include "herdr_ui.h"
#include "i18n.h"
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
    VIEW_UPDATE,
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
static lv_obj_t *s_sw_auto;         /* switch de descoberta automática do editor */
static lv_obj_t *s_lbl_lang;        /* valor da linha de idioma, na tela principal */
static lv_obj_t *s_lbl_wifi;        /* status vivo do Wi-Fi, na tela principal */
static lv_obj_t *s_pair_status;
static lv_timer_t *s_pair_timer;
static lv_obj_t *s_fwup_status;     /* label de estado da tela de atualização */
static lv_obj_t *s_fwup_bar;        /* barra de progresso do download */
static lv_obj_t *s_fwup_btn;        /* botão de ação (verificar/instalar) */
static lv_obj_t *s_fwup_btn_label;
static lv_timer_t *s_fwup_timer;
static lv_obj_t *s_fw_toast;        /* aviso global de versão nova (layer_top) */
static lv_obj_t *s_fw_toast_title;
static char s_fw_notified[32];      /* última versão já vista pelo usuário */

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
static void show_update(void);
static void update_toast(void);

/* ---------- teclado ---------- */

static void set_content_height(lv_coord_t h)
{
    lv_obj_set_height(s_content, h);
}

static void hide_kb(void)
{
    /* Desvincula antes que a tela destrua o textarea: o lv_keyboard guarda o
       ponteiro cru e, no ✓/✕, ainda envia READY/CANCEL para ele DEPOIS do
       nosso callback — com o campo já destruído é use-after-free (crash
       LoadProhibited em lv_keyboard.c:318, visto ao vivo). Com NULL, o guarda
       if(keyboard->ta) da própria LVGL pula esse envio. */
    lv_keyboard_set_textarea(s_kb, NULL);
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
    build_bar(T(STR_PASSWORD), back_to_scan_cb, NULL);
    lv_obj_clean(s_content);

    lv_obj_t *l = lv_label_create(s_content);
    lv_label_set_text_fmt(l, T(STR_NETWORK_FMT), ssid);
    lv_obj_set_style_text_font(l, &lv_font_ui_14, 0);
    lv_obj_set_style_text_color(l, UI_TEXT, 0);

    s_ta_pass = lv_textarea_create(s_content);
    lv_textarea_set_one_line(s_ta_pass, true);
    lv_textarea_set_password_mode(s_ta_pass, true);
    lv_textarea_set_placeholder_text(s_ta_pass, T(STR_PASSWORD_PH));
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
        lv_label_set_text(l, s_ap_count == 0 ? T(STR_NO_NETWORKS) : T(STR_SCAN_FAILED));
        lv_obj_set_style_text_color(l, UI_MUTED, 0);
        lv_obj_set_style_text_font(l, &lv_font_ui_14, 0);
        lv_obj_t *row = make_row(scan_retry_cb, NULL, 40);
        lv_obj_t *rl = lv_label_create(row);
        lv_label_set_text(rl, T(STR_RETRY));
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
    build_bar(T(STR_WIFI_NETWORKS), back_to_main_cb, NULL);
    hide_kb();
    lv_obj_clean(s_content);

    lv_obj_t *l = lv_label_create(s_content);
    lv_label_set_text(l, T(STR_SCANNING));
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
    bool was_empty = panel_host_is_free(h);
    bool auto_on = lv_obj_has_state(s_sw_auto, LV_STATE_CHECKED);
    strlcpy(h->name, lv_textarea_get_text(s_ta_name), CFG_NAME_LEN);
    strlcpy(h->host, lv_textarea_get_text(s_ta_host), CFG_HOST_LEN);
    strlcpy(h->token, lv_textarea_get_text(s_ta_token), CFG_TOKEN_LEN);
    h->port = (uint16_t)atoi(lv_textarea_get_text(s_ta_port));
    if (auto_on) {
        /* modo auto: endereço fora da NVS (a descoberta resolve em runtime);
           a porta zerada assume o padrão da ponte e vira o destino do probe */
        h->host[0] = '\0';
        if (!h->port) {
            h->port = 9375;
        }
    }
    bool complete = h->port && (auto_on ? h->token[0] != '\0' : h->host[0] != '\0');
    if (!complete) {
        h->enabled = false;             /* incompleto não conecta */
    } else if (was_empty) {
        h->enabled = true;              /* host novo entra habilitado */
    }
    if (panel_host_is_free(h)) {
        /* sem endereço e sem token o slot é livre de fato: some inteiro, para
           um nome fantasma não reaparecer no próximo "adicionar" */
        memset(h, 0, sizeof(*h));
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

/* No modo auto, endereço e porta saem de cena: a resposta assinada da
   descoberta fornece ambos (a porta da NVS segue como destino do probe). */
static void apply_auto_visibility(void)
{
    bool on = lv_obj_has_state(s_sw_auto, LV_STATE_CHECKED);
    lv_obj_t *rows[] = { lv_obj_get_parent(s_ta_host), lv_obj_get_parent(s_ta_port) };
    for (size_t i = 0; i < 2; i++) {
        if (on) {
            lv_obj_add_flag(rows[i], LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_clear_flag(rows[i], LV_OBJ_FLAG_HIDDEN);
        }
    }
}

static void host_auto_cb(lv_event_t *e)
{
    (void)e;
    hide_kb();            /* o foco estava num campo que pode ter sumido */
    apply_auto_visibility();
}

static void show_host(int idx)
{
    s_view = VIEW_HOST;
    s_edit_host = idx;
    lv_obj_add_flag(s_dock, LV_OBJ_FLAG_HIDDEN);
    update_toast();
    build_bar(T(STR_EDIT_HOST), back_to_main_cb, host_apply_cb);
    hide_kb();
    lv_obj_clean(s_content);

    const panel_host_t *h = &s_edit.hosts[idx];
    char port[8] = "9375";              /* porta padrão da ponte */
    if (h->port) {
        snprintf(port, sizeof(port), "%u", h->port);
    }

    s_ta_name  = make_field(T(STR_FIELD_NAME), h->name, T(STR_FIELD_NAME_PH));

    /* switch de descoberta automática, entre o nome e o endereço */
    lv_obj_t *arow = ui_plain(s_content);
    lv_obj_set_size(arow, LV_PCT(100), 44);
    lv_obj_t *al = lv_label_create(arow);
    lv_label_set_text(al, T(STR_FIELD_AUTO));
    lv_obj_set_style_text_font(al, &lv_font_ui_12, 0);
    lv_obj_set_style_text_color(al, UI_MUTED, 0);
    lv_obj_align(al, LV_ALIGN_LEFT_MID, 0, 0);
    s_sw_auto = lv_switch_create(arow);
    lv_obj_set_size(s_sw_auto, 48, 26);
    lv_obj_set_ext_click_area(s_sw_auto, 12);
    lv_obj_align(s_sw_auto, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_set_style_bg_color(s_sw_auto, UI_SWITCH_OFF, 0);
    if (panel_host_is_auto(h)) {
        lv_obj_add_state(s_sw_auto, LV_STATE_CHECKED);
    }
    lv_obj_add_event_cb(s_sw_auto, host_auto_cb, LV_EVENT_VALUE_CHANGED, NULL);

    s_ta_host  = make_field(T(STR_FIELD_ADDR), h->host, T(STR_FIELD_ADDR_PH));
    s_ta_port  = make_field(T(STR_FIELD_PORT), port, "9375");
    /* token da ponte deste host (ação show-token do plugin exibe no Herdr) */
    s_ta_token = make_field(T(STR_FIELD_TOKEN), h->token, T(STR_FIELD_TOKEN_PH));
    apply_auto_visibility();

    lv_obj_t *rm = lv_btn_create(s_content);
    lv_obj_set_size(rm, LV_PCT(100), 40);
    lv_obj_set_style_bg_color(rm, UI_BLOCKED, 0);
    lv_obj_set_style_radius(rm, 6, 0);
    lv_obj_set_style_shadow_width(rm, 0, 0);
    lv_obj_add_event_cb(rm, host_remove_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *rl = lv_label_create(rm);
    lv_label_set_text(rl, T(STR_REMOVE));
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
static void pair_apply(const panel_host_t *h, bool auto_mode)
{
    panel_cfg_t cfg = *panel_cfg_get();
    panel_host_t inc = *h;
    if (auto_mode) {
        /* slot auto fica sem endereço na NVS: a descoberta por broadcast o
           resolve em runtime (e o IP recebido morreria no esp_restart) */
        inc.host[0] = '\0';
    }
    int slot = -1;
    /* reparear a mesma máquina atualiza o slot em vez de duplicar: o nome
       (hostname do host) é estável mesmo quando o IP muda */
    for (int i = 0; i < CFG_MAX_HOSTS && slot < 0; i++) {
        if (cfg.hosts[i].name[0] && strcmp(cfg.hosts[i].name, inc.name) == 0) {
            slot = i;
        }
    }
    /* config legada (pareada por IP, sem name igual): mesmo endereço */
    for (int i = 0; i < CFG_MAX_HOSTS && slot < 0; i++) {
        if (inc.host[0] && cfg.hosts[i].host[0] &&
            strcmp(cfg.hosts[i].host, inc.host) == 0) {
            slot = i;
        }
    }
    for (int i = 0; i < CFG_MAX_HOSTS && slot < 0; i++) {
        if (panel_host_is_free(&cfg.hosts[i])) {
            slot = i;
        }
    }
    if (slot < 0) {
        lv_label_set_text(s_pair_status, T(STR_PAIR_NO_SLOT));
        lv_obj_set_style_text_color(s_pair_status, UI_BLOCKED, 0);
        return;
    }
    cfg.hosts[slot] = inc;
    panel_cfg_save(&cfg);
    lv_label_set_text_fmt(s_pair_status, T(STR_PAIRED_FMT), inc.name);
    lv_obj_set_style_text_color(s_pair_status, UI_IDLE, 0);
    lv_refr_now(NULL);            /* pinta o aviso antes de sumir a tela */
    esp_restart();
}

static void pair_tick_cb(lv_timer_t *t)
{
    (void)t;
    panel_host_t h;
    bool auto_mode;
    switch (pairing_state()) {
    case PAIRING_DONE:
        if (pairing_result(&h, &auto_mode)) {
            pair_leave();
            pair_apply(&h, auto_mode);
        }
        break;
    case PAIRING_WAITING:
        lv_label_set_text_fmt(s_pair_status, T(STR_PAIR_WAITING),
                              pairing_seconds_left());
        break;
    default:
        lv_label_set_text(s_pair_status, T(STR_PAIR_CLOSED));
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
    build_bar(T(STR_PAIR_TITLE), back_from_pair_cb, NULL);
    hide_kb();
    lv_obj_clean(s_content);

    lv_obj_t *card = ui_card(s_content, 8);
    lv_obj_set_size(card, LV_PCT(100), 150);
    lv_obj_set_style_pad_all(card, 12, 0);

    lv_obj_t *cap = lv_label_create(card);
    lv_label_set_text(cap, T(STR_THIS_PANEL));
    lv_obj_set_style_text_font(cap, &lv_font_ui_12, 0);
    lv_obj_set_style_text_color(cap, UI_MUTED, 0);
    lv_obj_align(cap, LV_ALIGN_TOP_MID, 0, 0);

    lv_obj_t *id = lv_label_create(card);
    lv_label_set_text(id, pairing_device_id());
    lv_obj_set_style_text_font(id, &lv_font_ui_clock_44, 0);
    lv_obj_set_style_text_color(id, UI_TEXT, 0);
    lv_obj_align(id, LV_ALIGN_TOP_MID, 0, 22);

    lv_obj_t *hint = lv_label_create(card);
    lv_label_set_text(hint, T(STR_PICK_CODE));
    lv_obj_set_style_text_font(hint, &lv_font_ui_12, 0);
    lv_obj_set_style_text_color(hint, UI_MUTED, 0);
    lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, 0);

    s_pair_status = lv_label_create(s_content);
    lv_label_set_text(s_pair_status, T(STR_STARTING));
    lv_obj_set_style_text_font(s_pair_status, &lv_font_ui_14, 0);
    lv_obj_set_style_text_color(s_pair_status, UI_WORKING, 0);
    lv_obj_set_width(s_pair_status, LV_PCT(100));
    lv_label_set_long_mode(s_pair_status, LV_LABEL_LONG_WRAP);

    lv_obj_t *steps = lv_label_create(s_content);
    lv_label_set_text(steps, T(STR_PAIR_STEPS));
    lv_obj_set_style_text_font(steps, &lv_font_ui_12, 0);
    lv_obj_set_style_text_color(steps, UI_MUTED, 0);
    lv_obj_set_width(steps, LV_PCT(100));
    lv_label_set_long_mode(steps, LV_LABEL_LONG_WRAP);

    if (pairing_start() != ESP_OK) {
        lv_label_set_text(s_pair_status, T(STR_PAIR_PORT_FAIL));
        lv_obj_set_style_text_color(s_pair_status, UI_BLOCKED, 0);
        return;
    }
    s_pair_timer = lv_timer_create(pair_tick_cb, 500, NULL);
}

/* ---------- view: atualização de firmware ---------- */

static void fwup_leave(void)
{
    if (s_fwup_timer) {
        lv_timer_del(s_fwup_timer);
        s_fwup_timer = NULL;
    }
}

static void back_from_fwup_cb(lv_event_t *e)
{
    (void)e;
    fwup_leave();
    show_main();                  /* um download em curso segue em background */
}

static void fwup_action_cb(lv_event_t *e)
{
    (void)e;
    fw_update_status_t st;
    fw_update_get_status(&st);
    if (st.state == FW_UPDATE_AVAILABLE) {
        fw_update_start();
    } else {
        fw_update_check_now();
    }
    /* o tick pinta o novo estado; nada a fazer aqui */
}

/**
 * Pinta a tela conforme o estado do fw_update — a task de OTA nunca toca a
 * LVGL; este timer (task da LVGL) é o único pintor, como no pareamento.
 */
static void fwup_tick_cb(lv_timer_t *t)
{
    (void)t;
    fw_update_status_t st;
    fw_update_get_status(&st);

    if (st.state == FW_UPDATE_DONE) {
        /* mesmo gesto do restart_now(); o esp_restart é da task de OTA */
        fwup_leave();
        lv_obj_clean(s_content);
        lv_obj_t *l = lv_label_create(s_content);
        lv_label_set_text(l, T(STR_FW_DONE));
        lv_obj_set_style_text_font(l, &lv_font_ui_16, 0);
        lv_obj_set_style_text_color(l, UI_TEXT, 0);
        lv_refr_now(NULL);
        return;
    }

    bool busy = st.state == FW_UPDATE_CHECKING || st.state == FW_UPDATE_DOWNLOADING;
    switch (st.state) {
    case FW_UPDATE_CHECKING:
        lv_label_set_text(s_fwup_status, T(STR_FW_CHECKING));
        lv_obj_set_style_text_color(s_fwup_status, UI_WORKING, 0);
        break;
    case FW_UPDATE_UP_TO_DATE:
        lv_label_set_text(s_fwup_status, T(STR_FW_UP_TO_DATE));
        lv_obj_set_style_text_color(s_fwup_status, UI_IDLE, 0);
        break;
    case FW_UPDATE_AVAILABLE:
        lv_label_set_text_fmt(s_fwup_status, T(STR_FW_AVAILABLE_FMT), st.latest);
        lv_obj_set_style_text_color(s_fwup_status, UI_WORKING, 0);
        break;
    case FW_UPDATE_DOWNLOADING:
        lv_label_set_text_fmt(s_fwup_status, T(STR_FW_DOWNLOADING_FMT), st.pct);
        lv_obj_set_style_text_color(s_fwup_status, UI_WORKING, 0);
        lv_bar_set_value(s_fwup_bar, st.pct, LV_ANIM_OFF);
        break;
    case FW_UPDATE_ERROR:
        lv_label_set_text(s_fwup_status, st.err == FW_ERR_DOWNLOAD
                          ? T(STR_FW_ERR_DOWNLOAD) : T(STR_FW_ERR_CHECK));
        lv_obj_set_style_text_color(s_fwup_status, UI_BLOCKED, 0);
        break;
    default:                      /* IDLE: ainda sem rede desde o boot */
        lv_label_set_text(s_fwup_status, T(STR_FW_CHECK_NOW));
        lv_obj_set_style_text_color(s_fwup_status, UI_MUTED, 0);
        break;
    }
    if (st.state == FW_UPDATE_DOWNLOADING) {
        lv_obj_clear_flag(s_fwup_bar, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_fwup_bar, LV_OBJ_FLAG_HIDDEN);
    }
    if (busy) {
        lv_obj_add_flag(s_fwup_btn, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_clear_flag(s_fwup_btn, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(s_fwup_btn_label, st.state == FW_UPDATE_AVAILABLE
                          ? T(STR_FW_INSTALL) : T(STR_FW_CHECK_NOW));
    }
}

static void show_update(void)
{
    fwup_leave();                 /* reentrar (toast) não pode deixar timer órfão */
    s_view = VIEW_UPDATE;
    lv_obj_add_flag(s_dock, LV_OBJ_FLAG_HIDDEN);
    update_toast();
    build_bar(T(STR_FW_UPDATE), back_from_fwup_cb, NULL);
    hide_kb();
    lv_obj_clean(s_content);

    /* versão instalada, no molde do card de pareamento */
    lv_obj_t *card = ui_card(s_content, 8);
    lv_obj_set_size(card, LV_PCT(100), 84);
    lv_obj_set_style_pad_all(card, 12, 0);

    lv_obj_t *cap = lv_label_create(card);
    lv_label_set_text(cap, T(STR_FW_ROW));
    lv_obj_set_style_text_font(cap, &lv_font_ui_12, 0);
    lv_obj_set_style_text_color(cap, UI_MUTED, 0);
    lv_obj_align(cap, LV_ALIGN_TOP_MID, 0, 0);

    lv_obj_t *ver = lv_label_create(card);
    lv_label_set_text(ver, fw_update_current_version());
    lv_obj_set_style_text_font(ver, &lv_font_ui_20, 0);
    lv_obj_set_style_text_color(ver, UI_TEXT, 0);
    lv_obj_align(ver, LV_ALIGN_BOTTOM_MID, 0, 0);

    s_fwup_status = lv_label_create(s_content);
    lv_obj_set_style_text_font(s_fwup_status, &lv_font_ui_14, 0);
    lv_obj_set_width(s_fwup_status, LV_PCT(100));
    lv_label_set_long_mode(s_fwup_status, LV_LABEL_LONG_WRAP);

    s_fwup_bar = lv_bar_create(s_content);
    lv_obj_set_size(s_fwup_bar, LV_PCT(100), 10);
    lv_bar_set_range(s_fwup_bar, 0, 100);
    lv_obj_set_style_bg_color(s_fwup_bar, UI_PANEL, 0);
    lv_obj_set_style_bg_color(s_fwup_bar, UI_WORKING, LV_PART_INDICATOR);
    lv_obj_add_flag(s_fwup_bar, LV_OBJ_FLAG_HIDDEN);

    s_fwup_btn = make_row(fwup_action_cb, NULL, 44);
    s_fwup_btn_label = lv_label_create(s_fwup_btn);
    lv_obj_set_style_text_font(s_fwup_btn_label, &lv_font_ui_14, 0);
    lv_obj_set_style_text_color(s_fwup_btn_label, UI_TEXT, 0);
    lv_obj_center(s_fwup_btn_label);

    /* entrar na tela já é "ver" o aviso: cala o toast desta versão */
    fw_update_status_t st;
    fw_update_get_status(&st);
    if (st.latest[0]) {
        strlcpy(s_fw_notified, st.latest, sizeof(s_fw_notified));
    }
    lv_obj_add_flag(s_fw_toast, LV_OBJ_FLAG_HIDDEN);

    s_fwup_timer = lv_timer_create(fwup_tick_cb, 500, NULL);
    fwup_tick_cb(NULL);           /* primeira pintura sem esperar o tick */
}

void herdr_ui_settings_open_update(void)
{
    show_update();
}

/* Aviso global de versão nova: vive em lv_layer_top para flutuar sobre
   qualquer aba, no visual do toast de pendência. Roda devagar (5s) — é só
   um espelho do estado; quem trabalha é a task do fw_update. */
static void fw_notify_tick_cb(lv_timer_t *t)
{
    (void)t;
    fw_update_status_t st;
    fw_update_get_status(&st);
    bool show = st.state == FW_UPDATE_AVAILABLE &&
                strcmp(st.latest, s_fw_notified) != 0;
    if (show) {
        lv_label_set_text_fmt(s_fw_toast_title, T(STR_FW_AVAILABLE_FMT), st.latest);
        lv_obj_clear_flag(s_fw_toast, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(s_fw_toast);
    } else if (st.state != FW_UPDATE_AVAILABLE) {
        lv_obj_add_flag(s_fw_toast, LV_OBJ_FLAG_HIDDEN);
    }
}

static void fw_toast_cb(lv_event_t *e)
{
    (void)e;
    lv_obj_add_flag(s_fw_toast, LV_OBJ_FLAG_HIDDEN);
    herdr_ui_show_settings();     /* traz a aba de configurações para frente */
    show_update();                /* e cai direto na tela de atualização */
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
        if (panel_host_is_free(&s_edit.hosts[i])) {
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

/** Troca o conteúdo pelo aviso e reinicia; não retorna. */
static void restart_now(void)
{
    lv_obj_clean(s_content);
    lv_obj_t *l = lv_label_create(s_content);
    lv_label_set_text(l, T(STR_RESTARTING));
    lv_obj_set_style_text_font(l, &lv_font_ui_16, 0);
    lv_obj_set_style_text_color(l, UI_TEXT, 0);
    lv_refr_now(NULL);            /* pinta o aviso antes de sumir a tela */
    esp_restart();
}

static void save_cb(lv_event_t *e)
{
    (void)e;
    panel_cfg_save(&s_edit);
    /* reinício limpa Wi-Fi e conexões; mais simples e confiável que teardown */
    restart_now();
}

/**
 * Alterna o idioma na cópia em edição.
 *
 * Só o valor da linha se atualiza — o resto da tela continua no idioma em
 * vigor, porque as telas são montadas uma vez e a troca vale a partir do
 * reinício, como toda mudança de config. O toast de pendência é quem diz isso.
 */
static void lang_toggle_cb(lv_event_t *e)
{
    (void)e;
    s_edit.lang = (uint8_t)((s_edit.lang + 1) % LANG_COUNT);
    lv_label_set_text(s_lbl_lang, i18n_lang_name((ui_lang_t)s_edit.lang));
    update_toast();
}

/** Reinício avulso: o que estiver pendente de salvar é descartado. */
static void restart_cb(lv_event_t *e)
{
    (void)e;
    restart_now();
}

static void fw_open_cb(lv_event_t *e)
{
    (void)e;
    show_update();
}

/**
 * Pinta o estado real da associação Wi-Fi (não o SSID salvo).
 *
 * Faixas de RSSI no critério usual de 2.4GHz: acima de -60 dBm a conexão é
 * confortável, entre -60 e -70 ainda serve, abaixo disso começa a cair.
 */
static void refresh_wifi_status(void)
{
    if (!s_lbl_wifi) {
        return;
    }
    int8_t rssi;
    if (!net_wifi_is_up()) {
        lv_label_set_text(s_lbl_wifi, T(STR_WIFI_DISCONNECTED));
        lv_obj_set_style_text_color(s_lbl_wifi, UI_BLOCKED, 0);
    } else if (!net_wifi_rssi(&rssi)) {
        /* tem IP mas o driver não deu o registro (scan em curso, por ex.) */
        lv_label_set_text(s_lbl_wifi, T(STR_WIFI_CONNECTED));
        lv_obj_set_style_text_color(s_lbl_wifi, UI_IDLE, 0);
    } else {
        str_id_t nota = rssi >= -60 ? STR_WIFI_GOOD
                      : (rssi >= -70 ? STR_WIFI_FAIR : STR_WIFI_WEAK);
        lv_color_t cor = rssi >= -60 ? UI_IDLE
                       : (rssi >= -70 ? UI_WORKING : UI_BLOCKED);
        lv_label_set_text_fmt(s_lbl_wifi, T(STR_WIFI_SIGNAL_FMT), rssi, T(nota));
        lv_obj_set_style_text_color(s_lbl_wifi, cor, 0);
    }
}

/** Mantém o status vivo enquanto a tela principal estiver à vista. */
static void wifi_tick_cb(lv_timer_t *t)
{
    (void)t;
    if (s_view != VIEW_MAIN) {
        return;              /* o label foi destruído junto com a tela */
    }
    refresh_wifi_status();
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
    build_bar(T(STR_TAB_SETTINGS), NULL, save_cb);
    hide_kb();
    lv_obj_clean(s_content);

    /* Sem SSID salvo a linha é uma só (nada a reportar); com rede configurada
       ela vira dupla, e a de baixo é o estado real da associação — o SSID
       sozinho não distingue "conectado" de "senha errada". */
    make_section_label(T(STR_SEC_WIFI));
    bool wifi_set = s_edit.wifi_ssid[0] != '\0';
    lv_obj_t *wrow = make_row(wifi_change_cb, NULL, wifi_set ? 48 : 44);
    lv_obj_t *wl = lv_label_create(wrow);
    lv_label_set_text(wl, wifi_set ? s_edit.wifi_ssid : T(STR_NOT_CONFIGURED));
    lv_obj_set_style_text_font(wl, &lv_font_ui_14, 0);
    lv_obj_set_style_text_color(wl, UI_TEXT, 0);
    lv_obj_align(wl, LV_ALIGN_LEFT_MID, 0, wifi_set ? -9 : 0);
    s_lbl_wifi = NULL;
    if (wifi_set) {
        s_lbl_wifi = lv_label_create(wrow);
        lv_obj_set_style_text_font(s_lbl_wifi, &lv_font_ui_12, 0);
        lv_obj_align(s_lbl_wifi, LV_ALIGN_LEFT_MID, 0, 10);
        refresh_wifi_status();       /* pinta antes do primeiro tick */
    }
    lv_obj_t *wc = lv_label_create(wrow);
    lv_label_set_text(wc, T(STR_CHANGE));
    lv_obj_set_style_text_font(wc, &lv_font_ui_12, 0);
    lv_obj_set_style_text_color(wc, UI_MUTED, 0);
    lv_obj_align(wc, LV_ALIGN_RIGHT_MID, 0, 0);

    make_section_label(T(STR_SEC_HOSTS));
    bool has_free = false;
    for (int i = 0; i < CFG_MAX_HOSTS; i++) {
        const panel_host_t *h = &s_edit.hosts[i];
        if (panel_host_is_free(h)) {
            has_free = true;
            continue;
        }
        lv_obj_t *row = make_row(host_row_cb, (void *)(intptr_t)i, 48);

        lv_obj_t *nm = lv_label_create(row);
        lv_label_set_text(nm, h->name[0] ? h->name
                              : (h->host[0] ? h->host : T(STR_AUTO_SHORT)));
        lv_obj_set_style_text_font(nm, &lv_font_ui_14, 0);
        lv_obj_set_style_text_color(nm, UI_TEXT, 0);
        lv_obj_align(nm, LV_ALIGN_LEFT_MID, 0, -9);

        lv_obj_t *ad = lv_label_create(row);
        if (h->host[0]) {
            lv_label_set_text_fmt(ad, "%s:%u", h->host, h->port);
        } else {
            lv_label_set_text(ad, T(STR_AUTO_SHORT));
        }
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
        lv_label_set_text(pl, T(STR_PAIR_WITH_HOST));
        lv_obj_set_style_text_font(pl, &lv_font_ui_14, 0);
        lv_obj_set_style_text_color(pl, UI_TERM_BG, 0);
        lv_obj_center(pl);

        lv_obj_t *add = make_row(add_host_cb, NULL, 44);
        lv_obj_t *al = lv_label_create(add);
        lv_label_set_text(al, T(STR_ADD_MANUALLY));
        lv_obj_set_style_text_font(al, &lv_font_ui_14, 0);
        lv_obj_set_style_text_color(al, UI_MUTED, 0);
        lv_obj_center(al);
    }

    make_section_label(T(STR_SEC_DEVICE));

    /* Duas opções só: a linha alterna no toque em vez de abrir uma tela para
       escolher entre duas. O valor mostrado é sempre no próprio idioma, então
       dá para ver para onde se está indo mesmo sem entender o rótulo. */
    lv_obj_t *lrow = make_row(lang_toggle_cb, NULL, 44);
    lv_obj_t *ll = lv_label_create(lrow);
    lv_label_set_text(ll, T(STR_LANGUAGE));
    lv_obj_set_style_text_font(ll, &lv_font_ui_14, 0);
    lv_obj_set_style_text_color(ll, UI_TEXT, 0);
    lv_obj_align(ll, LV_ALIGN_LEFT_MID, 0, 0);
    s_lbl_lang = lv_label_create(lrow);
    lv_label_set_text(s_lbl_lang, i18n_lang_name((ui_lang_t)s_edit.lang));
    lv_obj_set_style_text_font(s_lbl_lang, &lv_font_ui_14, 0);
    lv_obj_set_style_text_color(s_lbl_lang, UI_MUTED, 0);
    lv_obj_align(s_lbl_lang, LV_ALIGN_RIGHT_MID, 0, 0);

    lv_obj_t *rst = make_row(restart_cb, NULL, 44);
    lv_obj_t *rsl = lv_label_create(rst);
    lv_label_set_text(rsl, T(STR_RESTART_DEVICE));
    lv_obj_set_style_text_font(rsl, &lv_font_ui_14, 0);
    lv_obj_set_style_text_color(rsl, UI_TEXT, 0);
    lv_obj_center(rsl);

    /* versão instalada + atalho de atualização; o valor à direita da segunda
       linha só aparece quando a checagem já anunciou uma versão diferente */
    lv_obj_t *fwrow = make_row(fw_open_cb, NULL, 44);
    lv_obj_t *fwl = lv_label_create(fwrow);
    lv_label_set_text(fwl, T(STR_FW_ROW));
    lv_obj_set_style_text_font(fwl, &lv_font_ui_14, 0);
    lv_obj_set_style_text_color(fwl, UI_TEXT, 0);
    lv_obj_align(fwl, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_t *fwv = lv_label_create(fwrow);
    lv_label_set_text(fwv, fw_update_current_version());
    lv_obj_set_style_text_font(fwv, &lv_font_ui_14, 0);
    lv_obj_set_style_text_color(fwv, UI_MUTED, 0);
    lv_obj_align(fwv, LV_ALIGN_RIGHT_MID, 0, 0);

    lv_obj_t *uprow = make_row(fw_open_cb, NULL, 44);
    lv_obj_t *upl = lv_label_create(uprow);
    lv_label_set_text(upl, T(STR_FW_UPDATE));
    lv_obj_set_style_text_font(upl, &lv_font_ui_14, 0);
    lv_obj_set_style_text_color(upl, UI_TEXT, 0);
    lv_obj_align(upl, LV_ALIGN_LEFT_MID, 0, 0);
    fw_update_status_t fwst;
    fw_update_get_status(&fwst);
    if (fwst.state == FW_UPDATE_AVAILABLE) {
        lv_obj_t *upv = lv_label_create(uprow);
        lv_label_set_text_fmt(upv, T(STR_FW_AVAILABLE_FMT), fwst.latest);
        lv_obj_set_style_text_font(upv, &lv_font_ui_12, 0);
        lv_obj_set_style_text_color(upv, UI_WORKING, 0);
        lv_obj_align(upv, LV_ALIGN_RIGHT_MID, 0, 0);
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
    lv_label_set_text(t1, T(STR_PENDING_TITLE));
    lv_obj_set_style_text_font(t1, &lv_font_ui_14, 0);
    lv_obj_set_style_text_color(t1, UI_TEXT, 0);
    lv_obj_align(t1, LV_ALIGN_LEFT_MID, 32, -9);

    lv_obj_t *t2 = lv_label_create(s_toast);
    lv_label_set_text(t2, T(STR_PENDING_SUB));
    lv_obj_set_style_text_font(t2, &lv_font_ui_12, 0);
    lv_obj_set_style_text_color(t2, UI_MUTED, 0);
    lv_obj_align(t2, LV_ALIGN_LEFT_MID, 32, 10);

    s_kb = lv_keyboard_create(s_panel);
    lv_obj_set_size(s_kb, LV_HOR_RES, KB_H);
    lv_obj_set_style_text_font(s_kb, &lv_font_ui_16, 0);
    herdr_kb_setup(s_kb);
    lv_obj_add_flag(s_kb, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(s_kb, kb_cb, LV_EVENT_READY, NULL);
    lv_obj_add_event_cb(s_kb, kb_cb, LV_EVENT_CANCEL, NULL);

    /* Aviso de versão nova, no visual do toast de pendência mas em layer_top:
       precisa aparecer em qualquer aba, não só nas configurações. */
    s_fw_toast = lv_btn_create(lv_layer_top());
    lv_obj_set_size(s_fw_toast, LV_HOR_RES - 24, 56);
    lv_obj_align(s_fw_toast, LV_ALIGN_BOTTOM_MID, 0, -68);
    lv_obj_set_style_bg_color(s_fw_toast, UI_PANEL, 0);
    lv_obj_set_style_border_width(s_fw_toast, 1, 0);
    lv_obj_set_style_border_color(s_fw_toast, UI_WORKING, 0);
    lv_obj_set_style_radius(s_fw_toast, 8, 0);
    lv_obj_set_style_shadow_width(s_fw_toast, 20, 0);
    lv_obj_set_style_shadow_color(s_fw_toast, lv_color_black(), 0);
    lv_obj_set_style_shadow_ofs_y(s_fw_toast, 6, 0);
    lv_obj_add_flag(s_fw_toast, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(s_fw_toast, fw_toast_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *ficon = lv_label_create(s_fw_toast);
    lv_label_set_text(ficon, LV_SYMBOL_DOWNLOAD);
    lv_obj_set_style_text_font(ficon, &lv_font_ui_20, 0);
    lv_obj_set_style_text_color(ficon, UI_WORKING, 0);
    lv_obj_align(ficon, LV_ALIGN_LEFT_MID, 0, 0);

    s_fw_toast_title = lv_label_create(s_fw_toast);
    lv_obj_set_style_text_font(s_fw_toast_title, &lv_font_ui_14, 0);
    lv_obj_set_style_text_color(s_fw_toast_title, UI_TEXT, 0);
    lv_obj_align(s_fw_toast_title, LV_ALIGN_LEFT_MID, 32, -9);

    lv_obj_t *f2 = lv_label_create(s_fw_toast);
    lv_label_set_text(f2, T(STR_FW_TOAST_SUB));
    lv_obj_set_style_text_font(f2, &lv_font_ui_12, 0);
    lv_obj_set_style_text_color(f2, UI_MUTED, 0);
    lv_obj_align(f2, LV_ALIGN_LEFT_MID, 32, 10);

    lv_timer_create(fw_notify_tick_cb, 5000, NULL);
    lv_timer_create(wifi_tick_cb, 2000, NULL);
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
