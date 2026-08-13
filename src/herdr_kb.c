#include "herdr_kb.h"

#include <string.h>

/* O mapa default da LVGL enfia 12 alvos por fileira (~26 px em 320 px) —
   pequeno demais para dedo. Este layout copia o teclado do iOS: no máximo
   10 colunas (~32 px) e os símbolos divididos em duas páginas que, juntas,
   cobrem todo o ASCII imprimível — um terminal precisa de | ~ > { } etc.

   Os rótulos "1#", "ABC" e "abc" precisam ser exatamente estes: o
   lv_keyboard_def_event_cb casa a troca de modo por strcmp com essas
   strings. Já "#+=" e "12#" (páginas extras) são desconhecidos do handler
   default — sem o desvio em herdr_kb_event_cb seriam digitados literalmente
   no textarea.

   O LV_SYMBOL_KEYBOARD da barra inferior é o cancelar: manda LV_EVENT_CANCEL,
   que é como as duas telas recolhem o teclado. Sem ele o overlay do prompt,
   que ocupa a tela toda, só teria saída pelo ✓.

   Cada fileira do ctrl map abaixo fica numa linha só, na mesma ordem do mapa
   de teclas: as duas listas têm de ter exatamente o mesmo comprimento, e é
   assim que dá para conferir de olho. As larguras são relativas dentro da
   fileira e ficam no nibble baixo — acima de 15 elas vazam para as flags
   (18 ligaria HIDDEN e sumiria com a tecla). */

/* equivalente do LV_KB_BTN, que é macro privada do lv_keyboard.c */
#define KB_BTN(w) (LV_BTNMATRIX_CTRL_POPOVER | (w))
/* teclas de controle não repetem ao segurar (trocar de página, fechar, ✓) */
#define KB_CTRL(w) (LV_KEYBOARD_CTRL_BTN_FLAGS | (w))
/* backspace repete ao segurar, como no mapa default */
#define KB_BSP(w) (LV_BTNMATRIX_CTRL_CHECKED | (w))

static const char *kb_map_lc[] = {
    "q", "w", "e", "r", "t", "y", "u", "i", "o", "p", "\n",
    "a", "s", "d", "f", "g", "h", "j", "k", "l", "\n",
    "ABC", "z", "x", "c", "v", "b", "n", "m", LV_SYMBOL_BACKSPACE, "\n",
    "1#", LV_SYMBOL_KEYBOARD, ",", " ", ".", LV_SYMBOL_OK, ""
};

static const char *kb_map_uc[] = {
    "Q", "W", "E", "R", "T", "Y", "U", "I", "O", "P", "\n",
    "A", "S", "D", "F", "G", "H", "J", "K", "L", "\n",
    "abc", "Z", "X", "C", "V", "B", "N", "M", LV_SYMBOL_BACKSPACE, "\n",
    "1#", LV_SYMBOL_KEYBOARD, ",", " ", ".", LV_SYMBOL_OK, ""
};

static const lv_btnmatrix_ctrl_t kb_ctrl_letters[] = {
    KB_BTN(4), KB_BTN(4), KB_BTN(4), KB_BTN(4), KB_BTN(4), KB_BTN(4), KB_BTN(4), KB_BTN(4), KB_BTN(4), KB_BTN(4),
    KB_BTN(4), KB_BTN(4), KB_BTN(4), KB_BTN(4), KB_BTN(4), KB_BTN(4), KB_BTN(4), KB_BTN(4), KB_BTN(4),
    KB_CTRL(6), KB_BTN(4), KB_BTN(4), KB_BTN(4), KB_BTN(4), KB_BTN(4), KB_BTN(4), KB_BTN(4), KB_BSP(6),
    KB_CTRL(4), KB_CTRL(4), KB_BTN(3), 10, KB_BTN(3), KB_CTRL(4)
};

/* Página 1 de símbolos (via "1#"): dígitos + os mais frequentes. */
static const char *kb_map_sym1[] = {
    "1", "2", "3", "4", "5", "6", "7", "8", "9", "0", "\n",
    "-", "/", ":", ";", "(", ")", "$", "&", "@", "\"", "\n",
    "#+=", ".", ",", "?", "!", "'", LV_SYMBOL_BACKSPACE, "\n",
    "abc", LV_SYMBOL_KEYBOARD, " ", LV_SYMBOL_OK, ""
};

static const lv_btnmatrix_ctrl_t kb_ctrl_sym1[] = {
    KB_BTN(4), KB_BTN(4), KB_BTN(4), KB_BTN(4), KB_BTN(4), KB_BTN(4), KB_BTN(4), KB_BTN(4), KB_BTN(4), KB_BTN(4),
    KB_BTN(4), KB_BTN(4), KB_BTN(4), KB_BTN(4), KB_BTN(4), KB_BTN(4), KB_BTN(4), KB_BTN(4), KB_BTN(4), KB_BTN(4),
    KB_CTRL(7), KB_BTN(5), KB_BTN(5), KB_BTN(5), KB_BTN(5), KB_BTN(5), KB_BSP(8),
    KB_CTRL(4), KB_CTRL(4), 12, KB_CTRL(4)
};

/* Página 2 (via "#+="): o restante do ASCII de shell. */
static const char *kb_map_sym2[] = {
    "[", "]", "{", "}", "#", "%", "^", "*", "+", "=", "\n",
    "_", "\\", "|", "~", "<", ">", "`", "\n",
    "12#", ".", ",", "?", "!", "'", LV_SYMBOL_BACKSPACE, "\n",
    "abc", LV_SYMBOL_KEYBOARD, " ", LV_SYMBOL_OK, ""
};

static const lv_btnmatrix_ctrl_t kb_ctrl_sym2[] = {
    KB_BTN(4), KB_BTN(4), KB_BTN(4), KB_BTN(4), KB_BTN(4), KB_BTN(4), KB_BTN(4), KB_BTN(4), KB_BTN(4), KB_BTN(4),
    KB_BTN(4), KB_BTN(4), KB_BTN(4), KB_BTN(4), KB_BTN(4), KB_BTN(4), KB_BTN(4),
    KB_CTRL(7), KB_BTN(5), KB_BTN(5), KB_BTN(5), KB_BTN(5), KB_BTN(5), KB_BSP(8),
    KB_CTRL(4), KB_CTRL(4), 12, KB_CTRL(4)
};

static void herdr_kb_event_cb(lv_event_t *e)
{
    lv_obj_t *kb = lv_event_get_target(e);
    uint16_t id = lv_btnmatrix_get_selected_btn(kb);
    if (id == LV_BTNMATRIX_BTN_NONE)
        return;
    const char *txt = lv_btnmatrix_get_btn_text(kb, id);
    if (txt == NULL)
        return;

    if (strcmp(txt, "#+=") == 0) {
        lv_keyboard_set_mode(kb, LV_KEYBOARD_MODE_USER_1);
        return;
    }
    if (strcmp(txt, "12#") == 0) {
        lv_keyboard_set_mode(kb, LV_KEYBOARD_MODE_SPECIAL);
        return;
    }
    lv_keyboard_def_event_cb(e);
}

void herdr_kb_setup(lv_obj_t *kb)
{
    /* set_map grava num vetor global por modo, então vale para todos os
       teclados; repetir por instância é inócuo. */
    lv_keyboard_set_map(kb, LV_KEYBOARD_MODE_TEXT_LOWER, kb_map_lc, kb_ctrl_letters);
    lv_keyboard_set_map(kb, LV_KEYBOARD_MODE_TEXT_UPPER, kb_map_uc, kb_ctrl_letters);
    lv_keyboard_set_map(kb, LV_KEYBOARD_MODE_SPECIAL, kb_map_sym1, kb_ctrl_sym1);
    lv_keyboard_set_map(kb, LV_KEYBOARD_MODE_USER_1, kb_map_sym2, kb_ctrl_sym2);
    lv_keyboard_set_popovers(kb, true);

    /* Sai o handler default, entra o wrapper que delega nele: é o jeito
       documentado de tratar teclas que o default não conhece. */
    lv_obj_remove_event_cb(kb, lv_keyboard_def_event_cb);
    lv_obj_add_event_cb(kb, herdr_kb_event_cb, LV_EVENT_VALUE_CHANGED, NULL);
}
