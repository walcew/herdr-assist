/**
 * @file
 * @brief Motor de avatar da home: toca um pacote .hav e reage ao status global.
 *
 * O motor é dono do slot, do timer de animação e do estado corrente. Um pacote
 * traz as animações e o mapa de papéis (ver avatar_pack.h) — o de fábrica vem
 * embutido na flash, e os demais do cartão SD. Não há mais um driver por
 * avatar: acrescentar um avatar é acrescentar um arquivo.
 *
 * Tudo roda na task da LVGL (timers e eventos) — nunca chamar de fora dela.
 */

#pragma once

#include <lvgl.h>

#include "ui_theme.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Área reservada ao avatar na home. O pacote se dimensiona por essas medidas; a
   altura é o que costuma mandar, porque os sprites reservam espaço em volta do
   personagem para os efeitos — quem cresce a faixa cresce o mascote.

   Em retrato o slot toma a largura útil da tela (menos as margens de 12). Em
   paisagem a home vira duas colunas e o slot divide a faixa com os cards de
   host: fica mais estreito, e mais alto porque nada vem abaixo dele. */
static inline lv_coord_t avatar_slot_w(void)
{
    return ui_landscape() ? 174 : LV_HOR_RES - 24;
}

/* 214 = 320 de altura - 20 de pad_top da home - 62 do hero - 12 de pad_row
   - 12 de folga na base. */
static inline lv_coord_t avatar_slot_h(void)
{
    return ui_landscape() ? 214 : 160;
}

/**
 * Estado global que o avatar reflete (derivado em refresh_home).
 *
 * Os valores são os mesmos de hav_role_t nos cinco primeiros papéis, na mesma
 * ordem: o motor indexa o mapa do pacote direto pelo estado. Mexer numa das
 * duas listas exige mexer na outra.
 */
typedef enum {
    AVATAR_ST_DISCONNECTED = 0,  /* nenhum host habilitado online */
    AVATAR_ST_IDLE,
    AVATAR_ST_DONE,              /* algum agente terminou e ninguém abriu ainda */
    AVATAR_ST_WORKING,
    AVATAR_ST_BLOCKED,
} avatar_state_t;

/** Monta o motor no slot da home (chamar uma vez, em build_home). */
void avatar_create(lv_obj_t *slot);

/** Aplica o estado global ao avatar corrente (early-out se não mudou). */
void avatar_set_state(avatar_state_t st);

#ifdef __cplusplus
}
#endif
