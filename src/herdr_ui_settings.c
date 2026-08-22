#include "herdr_ui_settings.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <lvgl.h>

#include "esp_system.h"

#include "avatar.h"
#include "sd.h"
#include "avatar_store.h"
#include "fw_update.h"
#include "herdr_conn.h"
#include "herdr_kb.h"
#include "herdr_model.h"
#include "herdr_ui.h"
#include "i18n.h"
#include "lockscreen.h"
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
    VIEW_AVATAR,
    VIEW_AV_REPOS,
    VIEW_AV_FORMAT,
    VIEW_LOCK,
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
static lv_obj_t *s_lbl_orient;      /* idem, linha de orientação */
static lv_obj_t *s_row_lock_tmo;    /* linhas que só valem com o bloqueio ligado */
static lv_obj_t *s_row_lock_pat;
static lv_obj_t *s_lbl_lock_tmo;
static lv_obj_t *s_lbl_wifi;        /* status vivo do Wi-Fi, na tela principal */
static lv_obj_t *s_pair_status;
static lv_timer_t *s_pair_timer;
static lv_obj_t *s_fwup_status;     /* label de estado da tela de atualização */
static lv_obj_t *s_fwup_bar;        /* barra de progresso do download */
static lv_obj_t *s_fwup_btn;        /* botão de ação (verificar/instalar) */
static lv_obj_t *s_fwup_btn_label;
static lv_timer_t *s_fwup_timer;
static lv_obj_t *s_av_list;         /* container das linhas de avatar */
static lv_obj_t *s_av_status;       /* linha de estado (cartão, erro, progresso) */
static lv_obj_t *s_av_repo[STORE_USER_REPOS];
static lv_timer_t *s_av_timer;
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
static void show_avatars(void);
static void show_av_repos(void);
static void show_av_format(void);
static void show_lock(void);
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

/* Linha dentro de `parent`. O make_row abaixo é o caso comum (direto no
   conteúdo da view); quem repinta uma lista sozinha precisa de um container
   próprio, senão o lv_obj_clean dele não alcança as linhas. */
static lv_obj_t *make_row_in(lv_obj_t *parent, lv_event_cb_t cb, void *user_data,
                             lv_coord_t h)
{
    lv_obj_t *row = lv_btn_create(parent);
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

static lv_obj_t *make_row(lv_event_cb_t cb, void *user_data, lv_coord_t h)
{
    return make_row_in(s_content, cb, user_data, h);
}

/* Tamanho legível: MB com uma casa até 1GB, GB acima. Os pacotes vão de 38KB a
   1,5MB e o cartão de centenas de MB — três faixas cobrem tudo que aparece. */
static void fmt_bytes(char *buf, size_t size, uint64_t bytes)
{
    if (bytes >= 1024ULL * 1024 * 1024) {
        snprintf(buf, size, "%.1f GB", (double)bytes / (1024.0 * 1024 * 1024));
    } else if (bytes >= 1024 * 1024) {
        snprintf(buf, size, "%.1f MB", (double)bytes / (1024.0 * 1024));
    } else {
        snprintf(buf, size, "%u KB", (unsigned)(bytes / 1024));
    }
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
    /* largura do conteúdo (tela - dock - padding) menos o rótulo de 86px */
    lv_obj_set_size(ta, LV_HOR_RES - UI_DOCK_W - 16 - 90, 40);
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
        lv_obj_set_width(name, LV_HOR_RES - UI_DOCK_W - 150);
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
        /* sem rede o anúncio não sai — avisar vale mais que a contagem
           (o modo segue ligado: o beacon é reenviado a cada segundo e
           passa a alcançar a LAN assim que o Wi-Fi conectar) */
        if (!net_wifi_is_up()) {
            lv_label_set_text(s_pair_status, T(STR_PAIR_NO_WIFI));
            lv_obj_set_style_text_color(s_pair_status, UI_BLOCKED, 0);
        } else {
            lv_label_set_text_fmt(s_pair_status, T(STR_PAIR_WAITING),
                                  pairing_seconds_left());
            lv_obj_set_style_text_color(s_pair_status, UI_WORKING, 0);
        }
        break;
    default:
        lv_label_set_text(s_pair_status, T(STR_PAIR_CLOSED));
        lv_obj_set_style_text_color(s_pair_status, UI_MUTED, 0);
        pair_leave();
        break;
    }
}

/* Parágrafo do passo a passo: 12px apagado, quebrando na largura da tela. */
static void pair_text(const char *text)
{
    lv_obj_t *l = lv_label_create(s_content);
    lv_label_set_text(l, text);
    lv_obj_set_style_text_font(l, &lv_font_ui_12, 0);
    lv_obj_set_style_text_color(l, UI_MUTED, 0);
    lv_obj_set_width(l, LV_PCT(100));
    lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
}

/* Comando pronto para digitar, tipografado como terminal. */
static void pair_code(const char *cmd)
{
    lv_obj_t *c = ui_card(s_content, 6);
    lv_obj_set_size(c, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(c, UI_TERM_BG, 0);
    lv_obj_set_style_pad_all(c, 8, 0);
    lv_obj_t *l = lv_label_create(c);
    lv_label_set_text(l, cmd);
    lv_obj_set_style_text_font(l, &lv_font_terminal_12, 0);
    lv_obj_set_style_text_color(l, UI_TERM_TEXT, 0);
    lv_obj_set_width(l, LV_PCT(100));
    lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
}

/* Lado do QR: módulos de 4px na URL pt-BR (a maior), legível de perto. */
#define PAIR_QR_PX 180

static void show_pair(void)
{
    /* Os comandos não são tradução — iguais nos dois idiomas. */
    static const char CMD_INSTALL[] =
        "herdr plugin install walcew/herdr-assist/plugin";
    static const char CMD_ADMIN[] =
        "herdr plugin pane open --plugin herdr-assist --entrypoint admin";

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

    pair_text(T(STR_PAIR_STEPS_INSTALL));
    pair_code(CMD_INSTALL);
    pair_text(T(STR_PAIR_STEPS_ADMIN));
    pair_code(CMD_ADMIN);
    pair_text(T(STR_PAIR_STEPS_PICK));

    /* Manual completo no GitHub, alcançável sem digitar URL: QR + endereço.
       O QR fica numa moldura branca — é a zona de silêncio que o leitor
       exige, e a única exceção à paleta escura (contraste de leitura). */
    lv_obj_t *qcard = ui_card(s_content, 8);
    lv_obj_set_size(qcard, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_pad_all(qcard, 12, 0);
    lv_obj_set_flex_flow(qcard, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(qcard, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(qcard, 10, 0);

    lv_obj_t *qcap = lv_label_create(qcard);
    lv_label_set_text(qcap, T(STR_PAIR_MANUAL));
    lv_obj_set_style_text_font(qcap, &lv_font_ui_12, 0);
    lv_obj_set_style_text_color(qcap, UI_MUTED, 0);
    lv_obj_set_width(qcap, LV_PCT(100));
    lv_label_set_long_mode(qcap, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(qcap, LV_TEXT_ALIGN_CENTER, 0);

    lv_obj_t *qbg = ui_card(qcard, 8);
    lv_obj_set_size(qbg, PAIR_QR_PX + 28, PAIR_QR_PX + 28);
    lv_obj_set_style_bg_color(qbg, lv_color_white(), 0);

    lv_obj_t *qr = lv_qrcode_create(qbg, PAIR_QR_PX,
                                    lv_color_black(), lv_color_white());
    lv_qrcode_update(qr, T(STR_PAIR_MANUAL_URL), strlen(T(STR_PAIR_MANUAL_URL)));
    lv_obj_center(qr);

    lv_obj_t *qurl = lv_label_create(qcard);
    lv_label_set_text(qurl, T(STR_PAIR_MANUAL_URL));
    lv_obj_set_style_text_font(qurl, &lv_font_ui_12, 0);
    lv_obj_set_style_text_color(qurl, UI_MUTED, 0);
    lv_obj_set_width(qurl, LV_PCT(100));
    lv_label_set_long_mode(qurl, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(qurl, LV_TEXT_ALIGN_CENTER, 0);

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

/**
 * Só o X.Y.Z do começo da versão do painel, sem o `v`.
 *
 * Build de trabalho traz `v0.9.0-67-g33f1f15-dirty`; comparar a string inteira
 * com o que a ponte anuncia acusaria defasagem em toda build local.
 */
static void fw_core_version(char *out, size_t size)
{
    const char *v = fw_update_current_version();
    while (*v == 'v' || *v == 'V') {
        v++;
    }
    size_t n = 0;
    while (v[n] && ((v[n] >= '0' && v[n] <= '9') || v[n] == '.') && n + 1 < size) {
        n++;
    }
    memcpy(out, v, n);
    out[n] = '\0';
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

    /* Pontes conectadas e a versão de cada uma. Fica nesta tela porque é aqui
       que já se pergunta "que versão é esta"; e é a única forma de ver, do
       painel, que um host ficou para trás — quem atualiza a ponte é o host,
       não o painel, então não há botão nem toast, só o fato. */
    make_section_label(T(STR_FW_SEC_BRIDGES));
    char mine[16];
    fw_core_version(mine, sizeof(mine));
    const panel_cfg_t *cfg = panel_cfg_get();
    int bridges = 0;
    for (int i = 0; i < CFG_MAX_HOSTS; i++) {
        const panel_host_t *h = &cfg->hosts[i];
        if (panel_host_is_free(h) || herdr_model_get_conn(i) != HERDR_CONN_ONLINE) {
            continue;
        }
        const char *bv = herdr_conn_bridge_version(i);
        lv_obj_t *row = make_row(NULL, NULL, 44);

        lv_obj_t *nm = lv_label_create(row);
        lv_label_set_text(nm, h->name[0] ? h->name
                              : (h->host[0] ? h->host : T(STR_AUTO_SHORT)));
        lv_obj_set_style_text_font(nm, &lv_font_ui_14, 0);
        lv_obj_set_style_text_color(nm, UI_TEXT, 0);
        lv_obj_align(nm, LV_ALIGN_LEFT_MID, 0, 0);

        lv_obj_t *vl = lv_label_create(row);
        /* Ponte que não anuncia versão é ponte anterior a este recurso — o que
           já é, em si, a informação de que ela está atrasada. */
        bool ok = bv[0] && mine[0] && strcmp(bv, mine) == 0;
        lv_label_set_text(vl, bv[0] ? bv : T(STR_FW_BRIDGE_UNKNOWN));
        lv_obj_set_style_text_font(vl, &lv_font_ui_12, 0);
        lv_obj_set_style_text_color(vl, ok ? UI_IDLE : UI_WORKING, 0);
        lv_obj_align(vl, LV_ALIGN_RIGHT_MID, 0, 0);
        bridges++;
    }
    if (bridges == 0) {
        lv_obj_t *l = lv_label_create(s_content);
        lv_label_set_text(l, T(STR_FW_BRIDGE_NONE));
        lv_obj_set_style_text_font(l, &lv_font_ui_12, 0);
        lv_obj_set_style_text_color(l, UI_MUTED, 0);
    }

    s_fwup_timer = lv_timer_create(fwup_tick_cb, 500, NULL);
    fwup_tick_cb(NULL);           /* primeira pintura sem esperar o tick */
}


/* ---------- view: avatares ---------- */

static void avatars_leave(void)
{
    if (s_av_timer) {
        lv_timer_del(s_av_timer);
        s_av_timer = NULL;
    }
}

static void back_from_avatars_cb(lv_event_t *e)
{
    (void)e;
    avatars_leave();
    show_main();                  /* um download em curso segue em background */
}

static void av_refresh_cb(lv_event_t *e)
{
    (void)e;
    avatar_store_refresh();
}

static void repos_open_cb(lv_event_t *e)
{
    (void)e;
    avatars_leave();          /* o tick pinta a lista de avatares; lá não há */
    show_av_repos();
}

static void format_open_cb(lv_event_t *e)
{
    (void)e;
    avatars_leave();
    show_av_format();
}

/* Toque na linha: instalado passa a ser o avatar corrente; o resto baixa. */
static void av_row_cb(lv_event_t *e)
{
    const avatar_entry_t *ent = lv_event_get_user_data(e);
    if (ent->installed) {
        avatar_select(ent->builtin ? "" : ent->id);
    } else {
        avatar_store_install(ent->id);
    }
}

static void av_remove_cb(lv_event_t *e)
{
    const avatar_entry_t *ent = lv_event_get_user_data(e);
    /* Se era o que estava tocando, o motor volta para o de fábrica — senão a
       home ficaria animando um pacote que não existe mais. */
    if (strcmp(avatar_current(), ent->id) == 0) {
        avatar_select("");
    }
    avatar_store_remove(ent->id);
    avatar_store_refresh();       /* remonta a lista do jeito certo */
}

static const char *av_err_text(store_err_t err)
{
    switch (err) {
    case STORE_ERR_INDEX:    return T(STR_AV_ERR_INDEX);
    case STORE_ERR_NO_SD:    return T(STR_AV_NO_SD);
    case STORE_ERR_SPACE:    return T(STR_AV_ERR_SPACE);
    case STORE_ERR_PACK:     return T(STR_AV_ERR_PACK);
    case STORE_ERR_DOWNLOAD: return T(STR_AV_ERR_DOWNLOAD);
    case STORE_ERR_FORMAT:   return T(STR_AV_ERR_FORMAT);
    default:                 return T(STR_AV_ERR_NET);
    }
}

/* Catálogo copiado a cada repintura: o original é da task do store, e manter
   ponteiro para lá daria leitura suja no meio de um refresh. */
static avatar_entry_t s_av_cat[STORE_MAX];
static int s_av_n;

static void av_build_list(const avatar_store_status_t *st)
{
    lv_obj_clean(s_av_list);
    s_av_n = avatar_store_list(s_av_cat, STORE_MAX);
    if (s_av_n == 0) {
        lv_obj_t *l = lv_label_create(s_av_list);
        lv_label_set_text(l, T(STR_AV_EMPTY));
        lv_obj_set_style_text_font(l, &lv_font_ui_14, 0);
        lv_obj_set_style_text_color(l, UI_MUTED, 0);
        return;
    }
    const char *cur = avatar_current();
    for (int i = 0; i < s_av_n; i++) {
        avatar_entry_t *ent = &s_av_cat[i];
        bool is_cur = strcmp(cur, ent->builtin ? "" : ent->id) == 0;
        lv_obj_t *row = make_row_in(s_av_list, av_row_cb, ent, 48);

        lv_obj_t *nm = lv_label_create(row);
        lv_label_set_text(nm, ent->name);
        lv_obj_set_style_text_font(nm, &lv_font_ui_14, 0);
        lv_obj_set_style_text_color(nm, UI_TEXT, 0);
        lv_obj_align(nm, LV_ALIGN_LEFT_MID, 0, -9);

        /* sub-linha: tamanho quando o repositório informou, e nada quando não */
        lv_obj_t *sz = lv_label_create(row);
        if (ent->builtin) {
            lv_label_set_text(sz, T(STR_AV_BUILTIN));
        } else if (ent->size) {
            char buf[16];
            fmt_bytes(buf, sizeof(buf), ent->size);
            lv_label_set_text(sz, buf);
        } else {
            lv_label_set_text(sz, T(STR_AV_INSTALLED));
        }
        lv_obj_set_style_text_font(sz, &lv_font_ui_12, 0);
        lv_obj_set_style_text_color(sz, UI_MUTED, 0);
        lv_obj_align(sz, LV_ALIGN_LEFT_MID, 0, 10);

        /* estado à direita; a lixeira substitui o rótulo no que dá para apagar */
        bool busy = st->state == STORE_DOWNLOADING &&
                    strcmp(st->id, ent->id) == 0;
        lv_obj_t *tag = lv_label_create(row);
        if (busy) {
            lv_label_set_text_fmt(tag, T(STR_AV_DOWNLOADING_FMT), st->pct);
            lv_obj_set_style_text_color(tag, UI_WORKING, 0);
        } else if (is_cur) {
            lv_label_set_text(tag, T(STR_AV_IN_USE));
            lv_obj_set_style_text_color(tag, UI_IDLE, 0);
        } else if (ent->builtin) {
            /* Embutido no firmware: não se apaga nem se baixa, e tocar na linha
               já o seleciona — não sobra ação secundária a oferecer. A lixeira
               chegava a ser desenhada aqui, mas o handler abaixo nunca era
               registrado: um botão que prometia o que não fazia. */
            lv_label_set_text(tag, "");
        } else if (ent->builtin) {
            /* Embutido no firmware: não se apaga nem se baixa, e tocar na linha
               já o seleciona — não sobra ação secundária a oferecer. A lixeira
               chegava a ser desenhada aqui, mas o handler abaixo nunca era
               registrado: um botão que prometia o que não fazia. */
            lv_label_set_text(tag, "");
        } else if (ent->installed) {
            lv_label_set_text(tag, LV_SYMBOL_TRASH);
            lv_obj_set_style_text_color(tag, UI_MUTED, 0);
        } else {
            lv_label_set_text(tag, T(STR_AV_GET));
            lv_obj_set_style_text_color(tag, UI_MUTED, 0);
        }
        lv_obj_set_style_text_font(tag, &lv_font_ui_12, 0);
        lv_obj_align(tag, LV_ALIGN_RIGHT_MID, 0, 0);
        if (!busy && !is_cur && ent->installed && !ent->builtin) {
            lv_obj_add_flag(tag, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_set_ext_click_area(tag, 14);   /* alvo de dedo, não de mouse */
            lv_obj_add_event_cb(tag, av_remove_cb, LV_EVENT_CLICKED, ent);
        }
    }
}

/**
 * Pinta conforme o estado do store — a task dele nunca toca a LVGL; este timer
 * é o único pintor, como no pareamento e no firmware.
 *
 * A lista só é remontada quando algo de fato mudou (o estado do store ou o
 * avatar corrente): com full_refresh, repintar custa um quadro inteiro no QSPI.
 */
static void av_tick_cb(lv_timer_t *t)
{
    (void)t;
    static store_state_t last_state = (store_state_t)-1;
    static uint8_t last_pct;
    static char last_cur[24];

    avatar_store_status_t st;
    avatar_store_get_status(&st);
    const char *cur = avatar_current();

    if (st.state == STORE_FORMATTING) {
        lv_label_set_text(s_av_status, T(STR_AV_FORMATTING));
        lv_obj_set_style_text_color(s_av_status, UI_WORKING, 0);
    } else if (st.state == STORE_REFRESHING) {
        lv_label_set_text(s_av_status, T(STR_AV_REFRESHING));
        lv_obj_set_style_text_color(s_av_status, UI_WORKING, 0);
    } else if (st.state == STORE_ERROR) {
        lv_label_set_text(s_av_status, av_err_text(st.err));
        lv_obj_set_style_text_color(s_av_status, UI_BLOCKED, 0);
    } else if (!sd_is_mounted()) {
        lv_label_set_text(s_av_status, T(STR_AV_NO_SD_HINT));
        lv_obj_set_style_text_color(s_av_status, UI_MUTED, 0);
    } else {
        char buf[16];
        fmt_bytes(buf, sizeof(buf), sd_free_bytes());
        lv_label_set_text_fmt(s_av_status, T(STR_AV_CARD_FMT), buf);
        lv_obj_set_style_text_color(s_av_status, UI_MUTED, 0);
    }

    /* O progresso entra de 5 em 5: cada remontagem custa um quadro inteiro no
       QSPI (full_refresh), e repintar a cada 1% seriam ~100 telas por download
       para mover um número. */
    if (st.state != last_state || st.pct / 5 != last_pct / 5 ||
        strcmp(cur, last_cur) != 0) {
        /* Baixar, apagar e formatar mudam o que existe no cartão, e a lista do
           motor é montada uma vez no boot: sem revarrer aqui, o pacote recém
           baixado ficaria inalcançável pelo toque no mascote. */
        if (st.state == STORE_READY && last_state != STORE_READY) {
            avatar_rescan();
        }
        last_state = st.state;
        last_pct = st.pct;
        strlcpy(last_cur, cur, sizeof(last_cur));
        av_build_list(&st);
    }
}


/* ---------- view: repositórios de avatar ---------- */

static void back_from_repos_cb(lv_event_t *e)
{
    (void)e;
    hide_kb();
    show_avatars();
}

/* Grava os dois campos e volta, já mandando reler — trocar de repositório sem
   atualizar a lista não teria efeito visível nenhum. */
static void repos_save_cb(lv_event_t *e)
{
    (void)e;
    for (int i = 0; i < STORE_USER_REPOS; i++) {
        if (s_av_repo[i]) {
            avatar_store_set_repo(i, lv_textarea_get_text(s_av_repo[i]));
        }
    }
    hide_kb();
    avatar_store_refresh();
    show_avatars();
}

static const char *repo_src_name(repo_src_t src)
{
    switch (src) {
    case REPO_SRC_CARD:   return T(STR_AV_SRC_CARD);
    case REPO_SRC_BRIDGE: return T(STR_AV_SRC_BRIDGE);
    default:              return T(STR_AV_SRC_DEFAULT);
    }
}

static avatar_repo_t s_repo_view[STORE_MAX_REPOS];

static void show_av_repos(void)
{
    s_view = VIEW_AV_REPOS;
    lv_obj_add_flag(s_dock, LV_OBJ_FLAG_HIDDEN);
    update_toast();
    build_bar(T(STR_AV_SEC_REPOS), back_from_repos_cb, repos_save_cb);
    hide_kb();
    lv_obj_clean(s_content);

    /* O que dá para editar vem primeiro, e separado: o resto desta tela é
       informação, e misturar os dois faria parecer que tudo é editável. */
    make_section_label(T(STR_AV_SEC_MINE));
    for (int i = 0; i < STORE_USER_REPOS; i++) {
        char url[STORE_REPO_LEN];
        avatar_store_get_repo(i, url, sizeof(url));
        s_av_repo[i] = make_field("URL", url, T(STR_AV_REPO_PH));
    }

    /* Os demais existem e são varridos, mas não se editam daqui — sem mostrá-los
       não havia como saber o que o painel está de fato consultando. */
    make_section_label(T(STR_AV_SEC_OTHER));
    int n = avatar_store_repo_list(s_repo_view, STORE_MAX_REPOS);
    int shown = 0;
    for (int i = 0; i < n; i++) {
        if (s_repo_view[i].src == REPO_SRC_USER) {
            continue;   /* já está nos campos acima */
        }
        lv_obj_t *row = make_row(NULL, NULL, 44);
        lv_obj_t *tag = lv_label_create(row);
        lv_label_set_text(tag, repo_src_name(s_repo_view[i].src));
        lv_obj_set_style_text_font(tag, &lv_font_ui_12, 0);
        lv_obj_set_style_text_color(tag, UI_MUTED, 0);
        lv_obj_align(tag, LV_ALIGN_RIGHT_MID, 0, 0);
        lv_obj_update_layout(tag);

        lv_obj_t *url = lv_label_create(row);
        lv_label_set_text(url, s_repo_view[i].url);
        lv_obj_set_style_text_font(url, &lv_font_ui_12, 0);
        lv_obj_set_style_text_color(url, UI_TEXT, 0);
        /* a largura útil da row menos o rótulo de procedência e o respiro */
        lv_obj_set_width(url, LV_HOR_RES - UI_DOCK_W - 16 - 24
                              - lv_obj_get_width(tag) - 8);
        lv_label_set_long_mode(url, LV_LABEL_LONG_DOT);
        lv_obj_align(url, LV_ALIGN_LEFT_MID, 0, 0);
        shown++;
    }
    if (shown == 0) {
        lv_obj_t *l = lv_label_create(s_content);
        lv_label_set_text(l, T(STR_AV_REPO_NONE));
        lv_obj_set_style_text_font(l, &lv_font_ui_12, 0);
        lv_obj_set_style_text_color(l, UI_MUTED, 0);
    }

    lv_obj_t *hint = lv_label_create(s_content);
    lv_label_set_text(hint, T(STR_AV_REPO_HINT));
    lv_obj_set_style_text_font(hint, &lv_font_ui_12, 0);
    lv_obj_set_style_text_color(hint, UI_MUTED, 0);
    lv_obj_set_width(hint, LV_PCT(100));
    lv_label_set_long_mode(hint, LV_LABEL_LONG_WRAP);
}

/* ---------- view: formatar o cartão ---------- */

static void back_from_format_cb(lv_event_t *e)
{
    (void)e;
    show_avatars();
}

static void format_go_cb(lv_event_t *e)
{
    (void)e;
    /* O pacote em uso morre junto com o cartão: cair para o de fábrica ANTES
       evita a home animando um avatar cujo arquivo já não existe. */
    avatar_select("");
    avatar_store_format();
    show_avatars();               /* quem mostra o andamento é o tick de lá */
}

/**
 * Tela só de confirmar: apagar o cartão inteiro é irreversível e um toque a
 * mais na lista de avatares não pode bastar para disparar isso.
 */
static void show_av_format(void)
{
    s_view = VIEW_AV_FORMAT;
    lv_obj_add_flag(s_dock, LV_OBJ_FLAG_HIDDEN);
    update_toast();
    build_bar(T(STR_AV_FORMAT), back_from_format_cb, NULL);
    hide_kb();
    lv_obj_clean(s_content);

    lv_obj_t *warn = lv_label_create(s_content);
    lv_label_set_text(warn, T(STR_AV_FORMAT_WARN));
    lv_obj_set_style_text_font(warn, &lv_font_ui_14, 0);
    lv_obj_set_style_text_color(warn, UI_BLOCKED, 0);
    lv_obj_set_width(warn, LV_PCT(100));
    lv_label_set_long_mode(warn, LV_LABEL_LONG_WRAP);

    /* Sem cartão montado a formatação ainda vale — e é justamente aí que ela
       mais serve, então a tela diz isso em vez de parecer inútil. */
    lv_obj_t *sub = lv_label_create(s_content);
    lv_label_set_text(sub, sd_is_mounted() ? T(STR_AV_FORMAT_LOST)
                                           : T(STR_AV_FORMAT_UNREAD));
    lv_obj_set_style_text_font(sub, &lv_font_ui_12, 0);
    lv_obj_set_style_text_color(sub, UI_MUTED, 0);
    lv_obj_set_width(sub, LV_PCT(100));
    lv_label_set_long_mode(sub, LV_LABEL_LONG_WRAP);

    lv_obj_t *btn = make_row(format_go_cb, NULL, 44);
    lv_obj_set_style_bg_color(btn, UI_BLOCKED, 0);
    lv_obj_t *bl = lv_label_create(btn);
    lv_label_set_text(bl, T(STR_AV_FORMAT_GO));
    lv_obj_set_style_text_font(bl, &lv_font_ui_14, 0);
    lv_obj_set_style_text_color(bl, UI_TEXT, 0);
    lv_obj_center(bl);
}

static void show_avatars(void)
{
    avatars_leave();              /* reentrar não pode deixar timer órfão */
    s_view = VIEW_AVATAR;
    lv_obj_add_flag(s_dock, LV_OBJ_FLAG_HIDDEN);
    update_toast();
    build_bar(T(STR_AV_TITLE), back_from_avatars_cb, NULL);
    hide_kb();
    lv_obj_clean(s_content);

    s_av_status = lv_label_create(s_content);
    lv_obj_set_style_text_font(s_av_status, &lv_font_ui_12, 0);
    lv_obj_set_width(s_av_status, LV_PCT(100));
    lv_label_set_long_mode(s_av_status, LV_LABEL_LONG_WRAP);

    lv_obj_t *btn = make_row(av_refresh_cb, NULL, 44);
    lv_obj_t *bl = lv_label_create(btn);
    lv_label_set_text(bl, T(STR_AV_REFRESH));
    lv_obj_set_style_text_font(bl, &lv_font_ui_14, 0);
    lv_obj_set_style_text_color(bl, UI_TEXT, 0);
    lv_obj_center(bl);

    /* As linhas vivem num container próprio para a repintura periódica não
       levar junto o botão acima nem os campos abaixo. */
    s_av_list = ui_plain(s_content);
    lv_obj_set_width(s_av_list, LV_PCT(100));
    lv_obj_set_height(s_av_list, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(s_av_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s_av_list, 8, 0);

    /* Os repositórios ficam em tela própria: digitar URL exige teclado, e o
       teclado subindo no meio da lista de avatares empurrava tudo. */
    lv_obj_t *rrow = make_row(repos_open_cb, NULL, 44);
    lv_obj_t *rl = lv_label_create(rrow);
    lv_label_set_text(rl, T(STR_AV_SEC_REPOS));
    lv_obj_set_style_text_font(rl, &lv_font_ui_14, 0);
    lv_obj_set_style_text_color(rl, UI_TEXT, 0);
    lv_obj_align(rl, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_t *rv = lv_label_create(rrow);
    lv_label_set_text_fmt(rv, "%d", avatar_store_repo_list(s_repo_view, STORE_MAX_REPOS));
    lv_obj_set_style_text_font(rv, &lv_font_ui_14, 0);
    lv_obj_set_style_text_color(rv, UI_MUTED, 0);
    lv_obj_align(rv, LV_ALIGN_RIGHT_MID, 0, 0);

    /* Formatar não é sobre avatares — apaga o cartão inteiro —, mas é aqui que
       o cartão aparece; a seção própria evita que pareça mais uma linha da
       lista acima. */
    make_section_label(T(STR_AV_SEC_CARD));
    lv_obj_t *frow = make_row(format_open_cb, NULL, 44);
    lv_obj_t *fl = lv_label_create(frow);
    lv_label_set_text(fl, T(STR_AV_FORMAT));
    lv_obj_set_style_text_font(fl, &lv_font_ui_14, 0);
    lv_obj_set_style_text_color(fl, UI_BLOCKED, 0);
    lv_obj_align(fl, LV_ALIGN_LEFT_MID, 0, 0);

    s_av_timer = lv_timer_create(av_tick_cb, 500, NULL);
    av_tick_cb(NULL);             /* primeira pintura sem esperar o tick */

    /* Entrar na tela já pede a lista: sem isso ela nasce vazia e o usuário tem
       de descobrir que precisa tocar em atualizar. */
    avatar_store_refresh();
}

/* ---------- view: bloqueio de tela ---------- */

/* Opções do prazo de tolerância; 0 = pedir o padrão toda vez. */
static const uint8_t k_lock_timeouts[] = { 0, 5, 15, 30, 60, 120 };
#define LOCK_TMO_COUNT ((int)(sizeof(k_lock_timeouts) / sizeof(k_lock_timeouts[0])))

static void lock_tmo_label(void)
{
    uint8_t min = lockscreen_timeout_min();
    if (min == 0) {
        lv_label_set_text(s_lbl_lock_tmo, T(STR_LOCK_OFF));
    } else {
        lv_label_set_text_fmt(s_lbl_lock_tmo, T(STR_LOCK_TMO_FMT), min);
    }
}

/* Com o bloqueio desligado, prazo e padrão não têm efeito nenhum: somem em vez
   de ficarem à toa. Alternar aqui evita reconstruir a tela de dentro do próprio
   evento do switch, que destruiria o widget no meio do callback. */
static void apply_lock_visibility(void)
{
    bool on = lockscreen_enabled();
    lv_obj_t *rows[] = { s_row_lock_tmo, s_row_lock_pat };
    for (size_t i = 0; i < 2; i++) {
        if (on) {
            lv_obj_clear_flag(rows[i], LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(rows[i], LV_OBJ_FLAG_HIDDEN);
        }
    }
}

/* Volta do overlay de captura: repintar a tela inteira é seguro aqui (o evento
   veio de outra árvore de objetos, não do switch que seria destruído). */
static void lock_capture_done_cb(bool saved)
{
    if (saved && !lockscreen_enabled()) {
        lockscreen_set_enabled(true);
    }
    show_lock();
}

static void lock_enable_cb(lv_event_t *e)
{
    bool on = lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED);
    if (on && !lockscreen_has_pattern()) {
        /* ligar sem padrão: só vale depois de desenhar (o cancelamento volta
           o switch ao repintar a tela) */
        lockscreen_request_capture(lock_capture_done_cb);
        return;
    }
    lockscreen_set_enabled(on);
    apply_lock_visibility();
}

static void lock_tmo_cb(lv_event_t *e)
{
    (void)e;
    uint8_t cur = lockscreen_timeout_min();
    int i = 0;
    while (i < LOCK_TMO_COUNT && k_lock_timeouts[i] != cur) {
        i++;
    }
    i = (i + 1) % LOCK_TMO_COUNT;
    lockscreen_set_timeout_min(k_lock_timeouts[i]);
    lock_tmo_label();
}

static void lock_pattern_cb(lv_event_t *e)
{
    (void)e;
    lockscreen_request_capture(lock_capture_done_cb);
}

static void show_lock(void)
{
    s_view = VIEW_LOCK;
    lv_obj_add_flag(s_dock, LV_OBJ_FLAG_HIDDEN);
    update_toast();
    build_bar(T(STR_LOCK_ROW), back_to_main_cb, NULL);   /* aplica na hora */
    hide_kb();
    lv_obj_clean(s_content);

    lv_obj_t *erow = ui_plain(s_content);
    lv_obj_set_size(erow, LV_PCT(100), 44);
    lv_obj_t *el = lv_label_create(erow);
    lv_label_set_text(el, T(STR_LOCK_ENABLE));
    lv_obj_set_style_text_font(el, &lv_font_ui_14, 0);
    lv_obj_set_style_text_color(el, UI_TEXT, 0);
    lv_obj_align(el, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_t *sw = lv_switch_create(erow);
    lv_obj_set_size(sw, 48, 26);
    lv_obj_set_ext_click_area(sw, 12);
    lv_obj_align(sw, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_set_style_bg_color(sw, UI_SWITCH_OFF, 0);
    if (lockscreen_enabled()) {
        lv_obj_add_state(sw, LV_STATE_CHECKED);
    }
    lv_obj_add_event_cb(sw, lock_enable_cb, LV_EVENT_VALUE_CHANGED, NULL);

    s_row_lock_tmo = make_row(lock_tmo_cb, NULL, 44);
    lv_obj_t *tl = lv_label_create(s_row_lock_tmo);
    lv_label_set_text(tl, T(STR_LOCK_TIMEOUT));
    lv_obj_set_style_text_font(tl, &lv_font_ui_14, 0);
    lv_obj_set_style_text_color(tl, UI_TEXT, 0);
    lv_obj_align(tl, LV_ALIGN_LEFT_MID, 0, 0);
    s_lbl_lock_tmo = lv_label_create(s_row_lock_tmo);
    lv_obj_set_style_text_font(s_lbl_lock_tmo, &lv_font_ui_14, 0);
    lv_obj_set_style_text_color(s_lbl_lock_tmo, UI_MUTED, 0);
    lv_obj_align(s_lbl_lock_tmo, LV_ALIGN_RIGHT_MID, 0, 0);
    lock_tmo_label();

    s_row_lock_pat = make_row(lock_pattern_cb, NULL, 44);
    lv_obj_t *pl = lv_label_create(s_row_lock_pat);
    lv_label_set_text(pl, lockscreen_has_pattern() ? T(STR_LOCK_CHANGE_PAT)
                                                   : T(STR_LOCK_SET_PAT));
    lv_obj_set_style_text_font(pl, &lv_font_ui_14, 0);
    lv_obj_set_style_text_color(pl, UI_TEXT, 0);
    lv_obj_center(pl);

    apply_lock_visibility();
}

/* Mexer na configuração do bloqueio exige provar o padrão, mesmo dentro do
   prazo de tolerância — provar aqui não destrava o terminal. */
static void lock_row_cb(lv_event_t *e)
{
    (void)e;
    if (lockscreen_enabled() && lockscreen_has_pattern()) {
        lockscreen_request_verify(show_lock);
    } else {
        show_lock();
    }
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

/* X do toast: recusa esta versão. Marcar como notificada é o mesmo gesto de
   quem abre a tela de atualização — o aviso só volta quando sair outra versão.
   Como é um botão filho, o clique não chega ao fw_toast_cb do toast. */
static void fw_toast_dismiss_cb(lv_event_t *e)
{
    (void)e;
    fw_update_status_t st;
    fw_update_get_status(&st);
    if (st.latest[0]) {
        strlcpy(s_fw_notified, st.latest, sizeof(s_fw_notified));
    }
    lv_obj_add_flag(s_fw_toast, LV_OBJ_FLAG_HIDDEN);
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

static const char *orient_name(uint8_t o)
{
    return T(o == CFG_ORIENT_LANDSCAPE ? STR_ORIENT_LANDSCAPE : STR_ORIENT_PORTRAIT);
}

/**
 * Alterna a orientação na cópia em edição.
 *
 * Como o idioma, só vale a partir do reinício: girar em runtime exigiria
 * reconfigurar o display e reconstruir todas as telas já montadas.
 */
static void orient_toggle_cb(lv_event_t *e)
{
    (void)e;
    s_edit.orient = (uint8_t)((s_edit.orient + 1) % CFG_ORIENT_COUNT);
    lv_label_set_text(s_lbl_orient, orient_name(s_edit.orient));
    update_toast();
}

/*
 * Liga/desliga a checagem automática de firmware na cópia em edição.
 *
 * Desligado, a task de OTA para de checar sozinha (no boot e diariamente) e o
 * toast de versão nova some junto; "Verificar agora" na tela de Firmware
 * continua funcionando. Como os demais ajustes, só vale após aplicar e
 * reiniciar — a config em uso é imutável em runtime.
 */
static void auto_update_cb(lv_event_t *e)
{
    lv_obj_t *sw = lv_event_get_target(e);
    s_edit.no_auto_update = lv_obj_has_state(sw, LV_STATE_CHECKED) ? 0 : 1;
    update_toast();
}

/** Reinício avulso: o que estiver pendente de salvar é descartado. */
static void restart_cb(lv_event_t *e)
{
    (void)e;
    restart_now();
}

static void avatars_open_cb(lv_event_t *e)
{
    (void)e;
    show_avatars();
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

    lv_obj_t *orow = make_row(orient_toggle_cb, NULL, 44);
    lv_obj_t *ol = lv_label_create(orow);
    lv_label_set_text(ol, T(STR_ORIENTATION));
    lv_obj_set_style_text_font(ol, &lv_font_ui_14, 0);
    lv_obj_set_style_text_color(ol, UI_TEXT, 0);
    lv_obj_align(ol, LV_ALIGN_LEFT_MID, 0, 0);
    s_lbl_orient = lv_label_create(orow);
    lv_label_set_text(s_lbl_orient, orient_name(s_edit.orient));
    lv_obj_set_style_text_font(s_lbl_orient, &lv_font_ui_14, 0);
    lv_obj_set_style_text_color(s_lbl_orient, UI_MUTED, 0);
    lv_obj_align(s_lbl_orient, LV_ALIGN_RIGHT_MID, 0, 0);

    /* Quantos pacotes o motor achou no cartão; o de fábrica não conta, porque
       ele existe sempre e dizer "1" sem cartão nenhum confundiria. */
    lv_obj_t *avrow = make_row(avatars_open_cb, NULL, 44);
    lv_obj_t *avl = lv_label_create(avrow);
    lv_label_set_text(avl, T(STR_AV_ROW));
    lv_obj_set_style_text_font(avl, &lv_font_ui_14, 0);
    lv_obj_set_style_text_color(avl, UI_TEXT, 0);
    lv_obj_align(avl, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_t *avv = lv_label_create(avrow);
    if (!sd_is_mounted()) {
        lv_label_set_text(avv, T(STR_AV_NO_SD));
    } else {
        lv_label_set_text_fmt(avv, "%d", avatar_count() - 1);
    }
    lv_obj_set_style_text_font(avv, &lv_font_ui_14, 0);
    lv_obj_set_style_text_color(avv, UI_MUTED, 0);
    lv_obj_align(avv, LV_ALIGN_RIGHT_MID, 0, 0);

    lv_obj_t *lkrow = make_row(lock_row_cb, NULL, 44);
    lv_obj_t *lkl = lv_label_create(lkrow);
    lv_label_set_text(lkl, T(STR_LOCK_ROW));
    lv_obj_set_style_text_font(lkl, &lv_font_ui_14, 0);
    lv_obj_set_style_text_color(lkl, UI_TEXT, 0);
    lv_obj_align(lkl, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_t *lkv = lv_label_create(lkrow);
    lv_label_set_text(lkv, lockscreen_enabled() ? T(STR_LOCK_ON) : T(STR_LOCK_OFF));
    lv_obj_set_style_text_font(lkv, &lv_font_ui_14, 0);
    lv_obj_set_style_text_color(lkv, UI_MUTED, 0);
    lv_obj_align(lkv, LV_ALIGN_RIGHT_MID, 0, 0);

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

    lv_obj_t *aurow = make_row(NULL, NULL, 44);
    lv_obj_t *aul = lv_label_create(aurow);
    lv_label_set_text(aul, T(STR_FW_AUTO));
    lv_obj_set_style_text_font(aul, &lv_font_ui_14, 0);
    lv_obj_set_style_text_color(aul, UI_TEXT, 0);
    lv_obj_align(aul, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_t *ausw = lv_switch_create(aurow);
    lv_obj_set_size(ausw, 48, 26);
    lv_obj_set_ext_click_area(ausw, 12);
    lv_obj_align(ausw, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_set_style_bg_color(ausw, UI_SWITCH_OFF, 0);
    if (!s_edit.no_auto_update) {
        lv_obj_add_state(ausw, LV_STATE_CHECKED);
    }
    lv_obj_add_event_cb(ausw, auto_update_cb, LV_EVENT_VALUE_CHANGED, NULL);

    update_toast();
}

/* ---------- init/show ---------- */

void herdr_ui_settings_init(lv_event_cb_t dock_cb)
{
    s_panel = ui_screen();

    s_bar = ui_topbar(s_panel, NULL, NULL);
    /* build_bar() só troca os filhos, então o recuo do dock em pé fica aqui */
    lv_obj_set_style_pad_left(s_bar, UI_TOPBAR_PAD + UI_DOCK_W, 0);

    s_content = ui_plain(s_panel);
    lv_obj_set_size(s_content, LV_HOR_RES, LV_VER_RES - UI_TOPBAR_H);
    lv_obj_align(s_content, LV_ALIGN_TOP_MID, 0, UI_TOPBAR_H);
    /* Em paisagem o recuo sai do dock em pé. Ele permanece nas subtelas, onde o
       dock é escondido — custa 80px de largura útil e paga com o alinhamento
       estável ao voltar para a tela principal. */
    lv_obj_set_style_pad_left(s_content, UI_PAD + UI_DOCK_W, 0);
    lv_obj_set_style_pad_right(s_content, UI_PAD, 0);
    lv_obj_set_style_pad_bottom(s_content, UI_DOCK_H, 0);
    lv_obj_set_style_pad_row(s_content, 6, 0);
    lv_obj_set_flex_flow(s_content, LV_FLEX_FLOW_COLUMN);
    lv_obj_add_flag(s_content, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(s_content, LV_DIR_VER);

    s_dock = ui_dock(s_panel, UI_TAB_SETTINGS, dock_cb);

    /* Flutua logo acima do dock: a edição fica só em memória até salvar, e sem
       este aviso não há como perceber que falta aplicar. Tocar salva e reinicia. */
    s_toast = lv_btn_create(s_panel);
    lv_obj_set_size(s_toast, LV_HOR_RES - 24 - UI_DOCK_W, 56);
    /* em paisagem o dock não está embaixo: o toast só se afasta da borda */
    lv_obj_align(s_toast, LV_ALIGN_BOTTOM_RIGHT, -12, ui_landscape() ? -12 : -68);
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
    /* encostado na direita: em paisagem o dock em pé ficaria por baixo dele */
    lv_obj_set_size(s_kb, LV_HOR_RES - UI_DOCK_W, KB_H);
    lv_obj_align(s_kb, LV_ALIGN_BOTTOM_RIGHT, 0, 0);
    lv_obj_set_style_text_font(s_kb, &lv_font_ui_16, 0);
    herdr_kb_setup(s_kb);
    lv_obj_add_flag(s_kb, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(s_kb, kb_cb, LV_EVENT_READY, NULL);
    lv_obj_add_event_cb(s_kb, kb_cb, LV_EVENT_CANCEL, NULL);

    /* Aviso de versão nova, no visual do toast de pendência mas em layer_top:
       precisa aparecer em qualquer aba, não só nas configurações. */
    s_fw_toast = lv_btn_create(lv_layer_top());
    lv_obj_set_size(s_fw_toast, LV_HOR_RES - 24 - UI_DOCK_W, 56);
    lv_obj_align(s_fw_toast, LV_ALIGN_BOTTOM_RIGHT, -12, ui_landscape() ? -12 : -68);
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

    /* recusar: fecha sem abrir a tela de atualização */
    lv_obj_t *fx = lv_btn_create(s_fw_toast);
    /* 32px cabem na altura útil do toast (56 menos o pad do tema); a área
       estendida é que dá o alvo de dedo, como nos switches das linhas. */
    lv_obj_set_size(fx, 32, 32);
    lv_obj_set_ext_click_area(fx, 8);
    lv_obj_align(fx, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_set_style_bg_opa(fx, LV_OPA_TRANSP, 0);
    lv_obj_set_style_shadow_width(fx, 0, 0);
    lv_obj_add_event_cb(fx, fw_toast_dismiss_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *fxl = lv_label_create(fx);
    lv_label_set_text(fxl, LV_SYMBOL_CLOSE);
    lv_obj_set_style_text_font(fxl, &lv_font_ui_14, 0);
    lv_obj_set_style_text_color(fxl, UI_MUTED, 0);
    lv_obj_center(fxl);

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
