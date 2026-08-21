/* Stub de esp_err.h para os testes de host.
 *
 * panel_cfg.h (puxado por herdr_model.h) só usa esp_err_t em duas assinaturas,
 * mas o header do ESP-IDF não existe fora do build do firmware. Fica FORA de
 * src/ de propósito: src/CMakeLists.txt varre a pasta inteira com GLOB_RECURSE,
 * e um stub lá dentro sombrearia o header real no firmware inteiro.
 */
#pragma once

typedef int esp_err_t;

#define ESP_OK 0
