#include "ui_theme.h"

#include <string.h>

void ui_theme_init(void)
{
    lv_disp_t *disp = lv_disp_get_default();
    /* verde como cor primária deixa switch/checkbox já na cor do design */
    lv_theme_t *th = lv_theme_default_init(disp, UI_IDLE, UI_MUTED, true, &lv_font_ui_14);
    lv_disp_set_theme(disp, th);
    lv_obj_set_style_bg_color(lv_scr_act(), UI_BG, 0);
}

lv_obj_t *ui_plain(lv_obj_t *parent)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_set_style_bg_opa(o, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(o, 0, 0);
    lv_obj_set_style_radius(o, 0, 0);
    lv_obj_set_style_pad_all(o, 0, 0);
    lv_obj_set_style_shadow_width(o, 0, 0);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    return o;
}

lv_obj_t *ui_screen(void)
{
    lv_obj_t *s = ui_plain(lv_scr_act());
    lv_obj_set_size(s, LV_HOR_RES, LV_VER_RES);
    lv_obj_align(s, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_bg_opa(s, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(s, UI_BG, 0);
    lv_obj_add_flag(s, LV_OBJ_FLAG_HIDDEN);
    return s;
}

lv_obj_t *ui_card(lv_obj_t *parent, lv_coord_t radius)
{
    lv_obj_t *c = lv_obj_create(parent);
    lv_obj_set_style_bg_color(c, UI_PANEL, 0);
    lv_obj_set_style_border_width(c, 0, 0);
    lv_obj_set_style_radius(c, radius, 0);
    lv_obj_set_style_shadow_width(c, 0, 0);
    lv_obj_set_style_pad_all(c, 0, 0);
    lv_obj_clear_flag(c, LV_OBJ_FLAG_SCROLLABLE);
    return c;
}

lv_obj_t *ui_icon_btn(lv_obj_t *parent, const char *symbol, lv_event_cb_t cb, void *user_data)
{
    lv_obj_t *b = lv_btn_create(parent);
    lv_obj_set_size(b, UI_ICON_BTN, UI_ICON_BTN);
    lv_obj_set_style_radius(b, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(b, UI_PANEL, 0);
    lv_obj_set_style_border_width(b, 1, 0);
    lv_obj_set_style_border_color(b, UI_BORDER, 0);
    lv_obj_set_style_shadow_width(b, 0, 0);
    if (cb) {
        lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, user_data);
    }
    lv_obj_t *l = lv_label_create(b);
    lv_label_set_text(l, symbol);
    lv_obj_set_style_text_font(l, &lv_font_ui_20, 0);
    lv_obj_set_style_text_color(l, UI_TEXT, 0);
    lv_obj_center(l);
    return b;
}

lv_obj_t *ui_topbar(lv_obj_t *parent, const char *title, lv_obj_t **out_title)
{
    lv_obj_t *bar = ui_plain(parent);
    lv_obj_set_size(bar, LV_HOR_RES, UI_TOPBAR_H);
    lv_obj_align(bar, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(bar, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_left(bar, UI_TOPBAR_PAD, 0);
    lv_obj_set_style_pad_right(bar, UI_TOPBAR_PAD, 0);
    lv_obj_set_style_pad_column(bar, 10, 0);

    if (title) {
        lv_obj_t *t = lv_label_create(bar);
        lv_label_set_text(t, title);
        lv_obj_set_style_text_font(t, &lv_font_ui_bold_20, 0);
        lv_obj_set_style_text_color(t, UI_TEXT, 0);
        lv_label_set_long_mode(t, LV_LABEL_LONG_DOT);
        lv_obj_set_flex_grow(t, 1);
        if (out_title) {
            *out_title = t;
        }
    }
    return bar;
}

lv_obj_t *ui_dock(lv_obj_t *parent, ui_tab_t active, lv_event_cb_t cb)
{
    /* mesma ordem do enum ui_tab_t: o índice do laço vira a aba no callback */
    static const char *icons[] = { LV_SYMBOL_HOME, LV_SYMBOL_LIST, UI_ICON_DASH,
                                   LV_SYMBOL_SETTINGS };

    /* Deitado embaixo em retrato; em pé à esquerda em paisagem, que é onde
       sobra largura e o polegar alcança sem cobrir o conteúdo. */
    bool land = ui_landscape();

    lv_obj_t *dock = lv_obj_create(parent);
    lv_obj_set_size(dock, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    /* o dock flutua: sem isto, numa tela com flex (a home) ele entraria no
       fluxo da coluna e o alinhamento central seria ignorado */
    lv_obj_add_flag(dock, LV_OBJ_FLAG_IGNORE_LAYOUT);
    lv_obj_align(dock, land ? LV_ALIGN_LEFT_MID : LV_ALIGN_BOTTOM_MID,
                 land ? 10 : 0, land ? 0 : -10);
    lv_obj_set_style_bg_color(dock, UI_PANEL, 0);
    lv_obj_set_style_border_width(dock, 1, 0);
    lv_obj_set_style_border_color(dock, UI_BORDER, 0);
    lv_obj_set_style_radius(dock, 22, 0);
    lv_obj_set_style_pad_all(dock, 5, 0);
    /* o gap fica no eixo do fluxo: coluna usa pad_row, linha usa pad_column */
    lv_obj_set_style_pad_row(dock, 4, 0);
    lv_obj_set_style_pad_column(dock, 4, 0);
    lv_obj_set_style_shadow_width(dock, 24, 0);
    lv_obj_set_style_shadow_color(dock, lv_color_black(), 0);
    /* a sombra cai para longe da borda em que o dock está encostado */
    lv_obj_set_style_shadow_ofs_x(dock, land ? 10 : 0, 0);
    lv_obj_set_style_shadow_ofs_y(dock, land ? 0 : 10, 0);
    lv_obj_set_flex_flow(dock, land ? LV_FLEX_FLOW_COLUMN : LV_FLEX_FLOW_ROW);
    lv_obj_clear_flag(dock, LV_OBJ_FLAG_SCROLLABLE);

    for (int i = 0; i < (int)(sizeof(icons) / sizeof(icons[0])); i++) {
        bool on = (i == (int)active);
        lv_obj_t *item = lv_btn_create(dock);
        /* em pé o item é mais alto e mais estreito: 4 deles somam 200px, que
           cabem folgados nos 320 de altura da paisagem. Deitado é o eixo
           apertado: 4*62 + 3*4 + 5*2 + 1*2 = 272 dos 320 de largura — um quinto
           ícone estoura a tela, que foi o que aconteceu com a aba Atividade. */
        lv_obj_set_size(item, land ? 48 : 62, land ? 44 : 38);
        lv_obj_set_style_radius(item, 17, 0);
        lv_obj_set_style_shadow_width(item, 0, 0);
        lv_obj_set_style_bg_color(item, on ? UI_TEXT : UI_PANEL, 0);
        lv_obj_set_style_bg_opa(item, on ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
        lv_obj_add_event_cb(item, cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);

        lv_obj_t *l = lv_label_create(item);
        lv_label_set_text(l, icons[i]);
        lv_obj_set_style_text_font(l, &lv_font_ui_16, 0);
        lv_obj_set_style_text_color(l, on ? UI_BG : UI_MUTED, 0);
        lv_obj_center(l);
    }
    return dock;
}

lv_color_t ui_status_color(const char *status)
{
    if (strcmp(status, "blocked") == 0) return UI_BLOCKED;
    if (strcmp(status, "working") == 0) return UI_WORKING;
    /* "done" = terminou e o pane ainda não foi aberto; vira "idle" quando for */
    if (strcmp(status, "done") == 0)    return UI_DONE;
    if (strcmp(status, "idle") == 0)    return UI_IDLE;
    return UI_MUTED;
}

/* Base Dracula com azul dessaturado (o azul oficial deles é roxo) e brights na
   mesma matiz. Só vale para SGR indexado 30-37/90-97/38;5;0-15 — Claude Code e
   afins emitem truecolor, que passa direto sem tocar aqui. */
const term_palette_t ui_term_palette = {
    .palette16 = {
        0x21222c, 0xff5555, 0x50fa7b, 0xf1fa8c, 0x6272a4, 0xff79c6, 0x8be9fd, 0xf8f8f2,
        0x555866, 0xff6e6e, 0x69ff94, 0xffffa5, 0x9db2ff, 0xff92df, 0xa4ffff, 0xffffff,
    },
    .default_fg = UI_TERM_TEXT_HEX,
    .default_bg = UI_TERM_BG_HEX,
};
