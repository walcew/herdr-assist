/**
 * @file
 * @brief Busca a versão publicada e baixa o firmware novo para o microSD.
 *
 * NÃO é OTA. O aparelho não se regrava: a tabela de partições que o M5Launcher
 * escreve tem um slot de app só para nós, e nenhum firmware consegue
 * sobrescrever a partição de onde está executando. O que dá para fazer — e é o
 * que resolve o incômodo de verdade — é trazer o binário novo do release para o
 * cartão, de onde o Launcher instala. Some a única parte chata do ciclo: levar o
 * cartão até o computador.
 *
 * O CI publica, a cada tag, um manifest-cardputer.json com versão, URL, tamanho
 * e SHA-256 do binário. A checagem e o download rodam numa task própria: são
 * dezenas de segundos de rede, e no laço da UI isso congelaria a tela.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    UPD_IDLE = 0,      /* nada aconteceu ainda */
    UPD_CHECKING,      /* buscando o manifest */
    UPD_UPTODATE,      /* a publicada é a que está rodando */
    UPD_AVAILABLE,     /* há versão nova; update_download() a traz */
    UPD_DOWNLOADING,   /* baixando (ver update_progress) */
    UPD_READY,         /* baixada e conferida: instalar pelo Launcher */
    UPD_ERROR,         /* ver update_error() */
} update_state_t;

/** Dispara a checagem em background. Ignorada se já houver algo em curso. */
void update_check(void);

/** Baixa a versão encontrada para o cartão. Só vale em UPD_AVAILABLE. */
void update_download(void);

update_state_t update_state(void);
/** Versão publicada, quando conhecida ("" antes da primeira checagem). */
const char *update_latest(void);
/** Nome do arquivo gravado no cartão, válido em UPD_READY. */
const char *update_file(void);
/** Motivo da falha, válido em UPD_ERROR. */
const char *update_error(void);
/** 0-100 durante o download. */
int update_progress(void);

/** true quando há versão nova esperando — o menu marca o item com isso. */
bool update_available(void);
