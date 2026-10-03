/**
 * @file
 * @brief Fita de LED de status no P3 (7 x WS2812B, dado no IO17).
 *
 * Mostra o mesmo estado global do avatar com uma cor e uma animação por estado
 * (desenho em led_fx.c). Roda numa task própria, fora da LVGL: a UI só publica
 * o estado, e a fita não engasga a tela nem é engasgada por ela.
 */

#pragma once

#include "avatar.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Liga o RMT e sobe a task da fita. Chamar uma vez, cedo no boot. */
void led_status_init(void);

/** Estado a mostrar; troca na hora. Pode ser chamada de qualquer task. */
void led_status_set_state(avatar_state_t st);

#ifdef __cplusplus
}
#endif
