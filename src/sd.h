/**
 * @file
 * @brief Cartão microSD do painel: montagem em /sd e estado para a UI.
 *
 * O slot TF da placa é ligado em modo SPI (DAT2 amarrado em 3V3, CD/DAT3 usado
 * como CS — ver docs/GPIO_pinout.jpg), então quem monta é o sdspi, não o sdmmc.
 * O display já ocupa o SPI2 em QSPI; o cartão vai no SPI3.
 *
 * Montar é opcional: sem cartão o painel roda igual, só com o avatar de fábrica.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Ponto de montagem; os pacotes de avatar vivem em SD_ROOT "/avatars". */
#define SD_ROOT "/sd"

/**
 * Monta o cartão (idempotente: chamar de novo com o cartão montado devolve
 * ESP_OK sem tocar no barramento). Sem cartão custa ~1s de timeout e devolve
 * erro — que não é fatal para nada.
 */
esp_err_t sd_mount(void);

/** true enquanto /sd estiver montado. */
bool sd_is_mounted(void);

/** Bytes livres no cartão; 0 sem cartão montado. */
uint64_t sd_free_bytes(void);

/**
 * Formata o cartão em FAT, apagando TUDO — não só os avatares. Só existe
 * porque o usuário pede: nada aqui formata sozinho.
 *
 * Serve também ao cartão que o painel NÃO consegue ler (um SD de fábrica em
 * exFAT, que este FATFS não monta, ou um sistema de arquivos quebrado): sem
 * cartão montado, a formatação acontece na própria montagem.
 *
 * Bloqueia enquanto escreve as tabelas — segundos, e mais em cartão grande.
 * Chamar da task do avatar_store, nunca da LVGL.
 */
esp_err_t sd_format(void);

#ifdef __cplusplus
}
#endif
