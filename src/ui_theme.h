/**
 * @file
 * @brief Paleta, fontes e peças compartilhadas da interface.
 *
 * Espelha o design aprovado (projeto "herdr-assist" no Claude Design): fundos
 * quase pretos e sólidos, cor reservada para status, topbar sem barra sólida
 * (título + botões circulares) e dock flutuante nas telas de topo.
 */

#pragma once

#include <lvgl.h>

#include "term_parse.h"

#ifdef __cplusplus
extern "C" {
#endif

/* --- cores --- */
#define UI_BG          lv_color_hex(0x0d0d0f)
#define UI_PANEL       lv_color_hex(0x17171a)
#define UI_BORDER      lv_color_hex(0x26262a)
#define UI_TEXT        lv_color_hex(0xececec)
#define UI_MUTED       lv_color_hex(0x7c7c82)
#define UI_CELL        lv_color_hex(0x1e1e22)   /* célula vazia do mapa de calor */
#define UI_CELL_OFF    lv_color_hex(0x151518)   /* idem, host offline */
#define UI_WORKING     lv_color_hex(0xc9a24a)   /* âmbar: em andamento */
#define UI_DONE        lv_color_hex(0x5f9ea8)   /* teal: terminou, aguarda revisão */
#define UI_IDLE        lv_color_hex(0x7da97d)   /* verde: ocioso / ok */
#define UI_BLOCKED     lv_color_hex(0xc05a55)   /* vermelho: bloqueado / offline */
#define UI_LIMIT_HIGH  lv_color_hex(0xc9814a)   /* laranja: uso alto (aba Dash) */
/* hex cru reutilizável fora da LVGL (paleta do term_view usa RGB888) */
#define UI_TERM_BG_HEX   0x0a0a0b
#define UI_TERM_TEXT_HEX 0xaebfa0
#define UI_TERM_BG     lv_color_hex(UI_TERM_BG_HEX)
#define UI_TERM_TEXT   lv_color_hex(UI_TERM_TEXT_HEX)
#define UI_SWITCH_OFF  lv_color_hex(0x2a2a2e)

/* --- medidas --- */
/* 44px é o alvo de toque confortável num painel de 3,5"; a topbar acompanha */
#define UI_TOPBAR_H    64
#define UI_ICON_BTN    44
#define UI_ROW_H       52
/* Espaço a reservar embaixo para o dock flutuante. O dock tem 50px (item de 38
   + pad 5 + borda 1, dos dois lados) e sobe 10px da base, mas a sombra sai mais
   14px por cima (width 24, ofs_y 10): 74px no total, +6 de folga. */
#define UI_DOCK_SPACE  80
#define UI_PAD         8

/* --- fontes --- */
LV_FONT_DECLARE(lv_font_ui_12);
LV_FONT_DECLARE(lv_font_ui_14);
LV_FONT_DECLARE(lv_font_ui_16);
LV_FONT_DECLARE(lv_font_ui_20);        /* ícones das topbars e valores dos cards */
LV_FONT_DECLARE(lv_font_ui_bold_16);
LV_FONT_DECLARE(lv_font_ui_bold_20);   /* títulos das topbars */
LV_FONT_DECLARE(lv_font_ui_clock_44);
LV_FONT_DECLARE(lv_font_terminal_12);

/* Ícones fora do conjunto LV_SYMBOL, da mesma FontAwesome (ver gen_font_ui.sh).
   Os quatro primeiros vêm em pares: o botão troca de ícone para mostrar em que
   estado está. */
#define UI_ICON_GROUPED  "\xEF\x97\xBD"   /* U+F5FD layer-group: agrupado por host */
#define UI_ICON_FLAT     "\xEF\x83\x89"   /* U+F0C9 bars: lista corrida */
#define UI_ICON_SORT_NAT "\xEF\x83\x9C"   /* U+F0DC sort: ordem natural */
#define UI_ICON_SORT_PRI "\xEF\x85\xA0"   /* U+F160 sort-amount-down: bloqueadas primeiro */
#define UI_ICON_DASH     "\xEF\x8F\xBD"   /* U+F3FD tachometer-alt: aba Dash */
#define UI_ICON_LOCK     "\xEF\x80\xA3"   /* U+F023 lock: bloqueio de tela */

/* Abas do dock. O valor do enum É a posição no dock (ui_dock passa o índice
   do laço como user_data), então a ordem aqui define a ordem dos botões. */
typedef enum {
    UI_TAB_HOME = 0,
    UI_TAB_SESSIONS,
    UI_TAB_DASH,
    UI_TAB_SETTINGS,
} ui_tab_t;

/** Aplica o tema escuro e a fonte padrão. Chamar antes de montar as telas. */
void ui_theme_init(void);

/** Container sem borda/raio/padding, do tamanho e cor que o chamador definir. */
lv_obj_t *ui_plain(lv_obj_t *parent);

/** Tela cheia (fundo UI_BG), oculta por padrão. */
lv_obj_t *ui_screen(void);

/** Bloco #17171a com cantos arredondados. */
lv_obj_t *ui_card(lv_obj_t *parent, lv_coord_t radius);

/** Botão circular só com ícone (34x34) usado nas topbars. */
lv_obj_t *ui_icon_btn(lv_obj_t *parent, const char *symbol, lv_event_cb_t cb, void *user_data);

/**
 * Topbar sem barra sólida: título à esquerda, botões à direita.
 * Devolve o container; o label do título sai em out_title (pode ser NULL).
 */
lv_obj_t *ui_topbar(lv_obj_t *parent, const char *title, lv_obj_t **out_title);

/** Dock flutuante de 4 abas; cb recebe a ui_tab_t em user_data. */
lv_obj_t *ui_dock(lv_obj_t *parent, ui_tab_t active, lv_event_cb_t cb);

/** Cor do status de um agente ("idle", "working", "blocked", "done", ...). */
lv_color_t ui_status_color(const char *status);

/** Paleta ANSI de 16 cores do term_view (SGR indexado; truecolor passa direto). */
extern const term_palette_t ui_term_palette;

#ifdef __cplusplus
}
#endif
