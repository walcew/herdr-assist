/**
 * @file
 * @brief Motor de avatar da home: drivers plugáveis que reagem ao status global.
 *
 * O motor é dono do slot (62x62 no hero), do timer de animação e da troca de
 * avatar por toque; cada driver desenha um avatar e reage ao estado. Tudo roda
 * na task da LVGL (timers e eventos) — nunca chamar de fora dela.
 */

#pragma once

#include <lvgl.h>

#include "ui_theme.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Área reservada ao avatar na home. Os drivers usam essas medidas para se
   dimensionar; a altura é o que manda no tamanho do Clawd, porque os sprites
   reservam espaço em volta dele para os efeitos — quem cresce a faixa cresce o
   mascote.

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

/* Estado global que o avatar reflete (derivado em refresh_home). */
typedef enum {
    AVATAR_ST_DISCONNECTED = 0,  /* nenhum host habilitado online */
    AVATAR_ST_IDLE,
    AVATAR_ST_DONE,              /* algum agente terminou e ninguém abriu ainda */
    AVATAR_ST_WORKING,
    AVATAR_ST_BLOCKED,
} avatar_state_t;

/*
 * Um avatar plugável. Contrato com o motor:
 *  - create() monta os widgets no slot; todo objeto criado deve ser
 *    não-clicável (o toque pertence ao slot, que alterna o avatar);
 *  - destroy() libera apenas memória própria e zera ponteiros — os widgets
 *    morrem no lv_obj_clean() feito pelo motor logo em seguida;
 *  - set_state() só é chamado depois de create();
 *  - tick() avança a animação (NULL para avatar estático).
 */
typedef struct {
    const char *name;
    void (*create)(lv_obj_t *parent);
    void (*destroy)(void);
    void (*set_state)(avatar_state_t st);
    void (*tick)(uint32_t now);
} avatar_driver_t;

extern const avatar_driver_t avatar_clawd_driver;    /* avatar_clawd.c */
extern const avatar_driver_t avatar_sonic_driver;    /* avatar_sonic.c */
extern const avatar_driver_t avatar_mcqueen_driver;  /* avatar_mcqueen.c */
extern const avatar_driver_t avatar_spiderman_driver;/* avatar_spiderman.c */
extern const avatar_driver_t avatar_sf_driver;       /* avatar_sf.c (Ryu vs Ken) */

/**
 * Posiciona o sprite no slot; `grow` é o quanto ele cresce por causa do zoom
 * (que a LVGL aplica em torno do centro do objeto, sem mudar o tamanho dele).
 *
 * Em retrato quem limita o zoom é a altura: o mascote preenche a faixa e fica
 * apoiado na base, na mesma altura em todas as animações. Em paisagem o slot é
 * estreito e alto, quem limita passa a ser a largura e sobra altura — apoiar na
 * base deixaria o mascote caído num canto, então ele centra e fica na mesma
 * linha dos cards de host ao lado.
 */
void avatar_place(lv_obj_t *img, lv_coord_t pad, lv_coord_t grow);

/** Monta o motor no slot da home (chamar uma vez, em build_home). */
void avatar_create(lv_obj_t *slot);

/** Aplica o estado global ao avatar corrente (early-out se não mudou). */
void avatar_set_state(avatar_state_t st);

#ifdef __cplusplus
}
#endif
