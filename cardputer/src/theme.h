/**
 * @file
 * @brief Paleta e medidas da tela de 240x135.
 *
 * As cores são as mesmas do painel de 3,5" (ver ../src/ui_theme.h): fundo quase
 * preto e cor reservada para status. O que muda é a escala — aqui uma linha de
 * lista tem 11px e a barra de status, 9.
 */

#pragma once

#include <stdint.h>

/** RGB888 (como no painel) -> RGB565 (como o display quer). */
static inline uint16_t rgb565(uint32_t c)
{
    return (uint16_t)(((c >> 8) & 0xF800) | ((c >> 5) & 0x07E0) | ((c >> 3) & 0x001F));
}

#define C_BG          rgb565(0x0d0d0f)
#define C_PANEL       rgb565(0x17171a)
#define C_BORDER      rgb565(0x26262a)
#define C_TEXT        rgb565(0xececec)
#define C_MUTED       rgb565(0x7c7c82)
#define C_WORKING     rgb565(0xc9a24a)   /* âmbar: em andamento */
#define C_DONE        rgb565(0x5f9ea8)   /* teal: terminou, aguarda revisão */
#define C_IDLE        rgb565(0x7da97d)   /* verde: ocioso / ok */
#define C_BLOCKED     rgb565(0xc05a55)   /* vermelho: bloqueado / offline */
/* fase apagada do pisca de "bloqueado": escuro o bastante para o contraste ser
   visível de relance, aceso o bastante para a linha não sumir da lista */
#define C_BLOCKED_DIM rgb565(0x4a2422)
#define C_LIMIT_HIGH  rgb565(0xc9814a)   /* laranja: uso alto */
#define C_ACCENT      rgb565(0xc9a24a)
#define C_TERM_BG_888 0x0a0a0b
#define C_TERM_FG_888 0xaebfa0
#define C_TERM_BG     rgb565(C_TERM_BG_888)

/* --- medidas --- */
#define SCR_W        240
#define SCR_H        135
#define STATUS_H       9    /* barra de status: fonte de 8px + 1 de respiro */
#define ROW_H         11    /* linha de lista */
#define FOOT_H        11    /* rodapé de dicas / linha de digitação */
#define BODY_Y      (STATUS_H + 1)
#define BODY_H      (SCR_H - BODY_Y)

/** Cor do status de um agente ("idle", "working", "blocked", "done"). */
uint16_t status_color(const char *status);
