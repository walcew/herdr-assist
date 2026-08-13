/**
 * @file
 * @brief Bloqueio de tela por padrão 3x3, no modelo do Android.
 *
 * Protege só a entrada no terminal: as demais telas seguem livres. Quem gateia
 * é o chamador — herdr_ui.c consulta lockscreen_is_locked() antes de abrir a
 * sessão e, bloqueado, pede o padrão e abre no sucesso.
 *
 * O overlay vive em lv_layer_sys(), acima de qualquer tela e do toast global de
 * firmware (que mora na layer_top). Enquanto visível, engole todos os toques.
 *
 * Config na NVS (namespace próprio, gravação em runtime como o avatar): o
 * panel_cfg é imutável durante a execução e exigiria reiniciar a cada ajuste.
 * O prazo de desbloqueio só existe em RAM, então reiniciar volta bloqueado.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Carrega a config e monta o overlay. Chamar uma vez, ao montar a UI. */
void lockscreen_init(void);

bool lockscreen_enabled(void);

/** Liga/desliga o bloqueio (grava na NVS). Zera o prazo em vigor. */
void lockscreen_set_enabled(bool enabled);

/** Minutos de tolerância após um desbloqueio; 0 = pedir sempre. */
uint8_t lockscreen_timeout_min(void);
void    lockscreen_set_timeout_min(uint8_t minutes);

bool lockscreen_has_pattern(void);

/** true quando a próxima entrada no terminal precisa do padrão. */
bool lockscreen_is_locked(void);

/**
 * Pede o padrão e, acertando, chama on_success (pode ser NULL) e arma o prazo.
 * Com o painel já desbloqueado, chama on_success na hora.
 */
void lockscreen_request_unlock(void (*on_success)(void));

/**
 * Igual ao unlock, mas o acerto NÃO arma o prazo: serve para provar identidade
 * antes de mexer na configuração, sem que isso destrave o terminal de brinde.
 */
void lockscreen_request_verify(void (*on_success)(void));

/**
 * Captura um padrão novo (desenhar duas vezes) e grava. on_done recebe false se
 * o usuário desistiu.
 */
void lockscreen_request_capture(void (*on_done)(bool saved));

#ifdef __cplusplus
}
#endif
