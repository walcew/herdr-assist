/**
 * @file
 * @brief Provisionamento de Wi-Fi por arquivo no microSD.
 *
 * Digitar uma senha de Wi-Fi num teclado de 56 teclas que você está usando pela
 * primeira vez é onde este porte tropeça — e sem retorno na tela não dá nem
 * para saber se errou. O cartão resolve isso melhor que qualquer atalho de
 * interface: você edita um txt no computador e o aparelho lê no boot.
 *
 * Formato de /herdr-wifi.txt (também aceita /wifi.txt), uma chave por linha:
 *
 *     ssid=MinhaRede
 *     pass=minhasenha
 *
 * Linhas começando com # são comentário. O arquivo manda: se ele existe e é
 * diferente do que está na NVS, o boot regrava a NVS. Para voltar a gerenciar
 * pela tela, apague o arquivo.
 *
 * A senha fica em texto puro num cartão FAT32 — quem tiver o cartão tem a
 * senha. É a mesma exposição de deixá-la compilada no binário, com a vantagem
 * de não entrar no repositório nem no .bin que circula.
 */

#pragma once

#include <stddef.h>

/** Monta o cartão (SPI2, separado do display). Idempotente. */
bool sd_mount(void);

/** Lê ssid/pass do arquivo de provisionamento. false se não houver. */
bool sd_wifi_read(char *ssid, size_t ssid_size, char *pass, size_t pass_size);
