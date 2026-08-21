/**
 * @file
 * @brief Colapsa cards de uso da mesma conta vindos de hosts diferentes.
 *
 * Limite de uso é propriedade da CONTA (vem do backend do provedor), não da
 * máquina: duas pontes logadas na mesma conta mandam o mesmo número, e a Dash
 * mostrava dois cards idênticos. O modelo indexa por host — este módulo é a
 * view por conta que a UI (e o avatar) querem.
 *
 * Função pura, sem LVGL/ESP — testável no host (scripts/limits_merge_test.c).
 */
#pragma once

#include "herdr_model.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Colapsa in-place as entradas de mesma (provedor, conta) do array achatado.
 *
 * Conta desconhecida ("") nunca casa: um card duplicado é melhor que fundir
 * duas contas e sumir com o uso de uma delas.
 *
 * Preserva a ordem da primeira ocorrência — a posição do card na tela não pode
 * depender do dado, senão os cards trocam de lugar entre gerações.
 *
 * @param list  array achatado (todos os hosts), alterado no lugar.
 * @param n     entradas válidas em `list`.
 * @return novo total (<= n). Idempotente: chamar de novo não soma outra vez.
 */
int limits_merge_accounts(herdr_limits_t *list, int n);

#ifdef __cplusplus
}
#endif
