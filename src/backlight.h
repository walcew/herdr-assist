/**
 * @file
 * @brief Brilho da tela (backlight por PWM), ajustado no slider das
 *        Configurações e salvo na NVS. Só do painel; aplica na hora.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Piso do slider: em 0 a tela some e não há como achar o slider de volta. */
#define BACKLIGHT_MIN 10

/** Lê o nível salvo e acende o backlight nele. Depois do panel_cfg_init(). */
void backlight_init(void);

/** Nível atual (BACKLIGHT_MIN..100); 100 de fábrica. */
uint8_t backlight_level(void);

/** Aplica o nível na hora; `save` grava na NVS (só ao soltar o slider). */
void backlight_set_level(uint8_t pct, bool save);

#ifdef __cplusplus
}
#endif
