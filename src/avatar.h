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

#ifdef __cplusplus
extern "C" {
#endif

/* Área reservada ao avatar na home: largura útil da tela (320 menos as margens
   de 12) por altura. Os drivers usam essas medidas para se dimensionar.
   A altura é o que manda no tamanho do Clawd: os sprites reservam espaço em
   volta dele para os efeitos, então quem cresce a faixa cresce o mascote. */
#define AVATAR_SLOT_W 296
#define AVATAR_SLOT_H 160

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

extern const avatar_driver_t avatar_clawd_driver;  /* avatar_clawd.c */
extern const avatar_driver_t avatar_sonic_driver;  /* avatar_sonic.c */

/** Monta o motor no slot da home (chamar uma vez, em build_home). */
void avatar_create(lv_obj_t *slot);

/** Aplica o estado global ao avatar corrente (early-out se não mudou). */
void avatar_set_state(avatar_state_t st);

#ifdef __cplusplus
}
#endif
