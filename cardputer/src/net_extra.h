/**
 * @file
 * @brief O que este porte acrescenta ao net.h compartilhado com o painel.
 *
 * Fica separado de propósito: o net.h vem de ../src no build e vale para os
 * dois firmwares, então nada daqui pode entrar lá — o painel não implementaria.
 */

#pragma once

/**
 * Estado do Wi-Fi em uma frase curta, para a tela.
 *
 * Sem isso, uma senha errada é indistinguível de um roteador fora do ar: as
 * duas coisas aparecem como "nenhuma sessão" e ninguém descobre o porquê.
 */
const char *net_wifi_status_text(void);
