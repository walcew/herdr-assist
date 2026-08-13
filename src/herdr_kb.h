#pragma once

#include <lvgl.h>

/* Aplica ao lv_keyboard o layout do painel: fileiras de no máximo 10 teclas,
   popovers e duas páginas de símbolos. Chamar logo após lv_keyboard_create(). */
void herdr_kb_setup(lv_obj_t *kb);
