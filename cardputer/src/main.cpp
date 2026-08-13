/**
 * @file
 * @brief herdr-assist no M5Stack Cardputer.
 *
 * Mesmo cliente do painel de 3,5" — mesma ponte, mesmo protocolo, mesma NVS —
 * numa tela de 240x135 com teclado. O que muda de verdade é a interação: no
 * painel se toca, aqui se digita, e por isso a sessão aberta aceita texto livre
 * além das teclas da allowlist.
 *
 * As tasks de conexão (uma por host) e a do pareamento rodam por fora do laço;
 * o loop() só cuida do teclado e da tela.
 */

#include <string.h>

#include <M5Cardputer.h>

#include "herdr_conn.h"
#include "herdr_model.h"
#include "net.h"
#include "panel_cfg.h"
#include "sd_cfg.h"
#include "ui.h"

/**
 * Aplica o Wi-Fi do cartão, quando houver arquivo.
 *
 * O arquivo manda: se diverge do que está na NVS, a NVS é regravada. É o que
 * permite trocar de rede editando um txt no computador em vez de digitar a
 * senha no teclado de 56 teclas — e, ao contrário de credencial compilada,
 * não vaza no binário.
 */
static void provision_from_sd(void)
{
    char ssid[CFG_SSID_LEN];
    char pass[CFG_PASS_LEN];
    if (!sd_wifi_read(ssid, sizeof(ssid), pass, sizeof(pass))) {
        return;
    }
    const panel_cfg_t *cur = panel_cfg_get();
    if (strcmp(cur->wifi_ssid, ssid) == 0 && strcmp(cur->wifi_pass, pass) == 0) {
        return;                            /* já é o que está gravado */
    }
    panel_cfg_t seed = *cur;
    strlcpy(seed.wifi_ssid, ssid, sizeof(seed.wifi_ssid));
    strlcpy(seed.wifi_pass, pass, sizeof(seed.wifi_pass));
    panel_cfg_save(&seed);
    panel_cfg_init();                      /* recarrega a cópia em uso */
}

void setup()
{
    auto cfg = M5.config();
    M5Cardputer.begin(cfg, true);          /* true: liga o teclado */
    M5Cardputer.Display.setRotation(1);    /* 240x135, teclado para baixo */
    M5Cardputer.Display.setBrightness(140);

    /* a logo entra antes do trabalho pesado: montar o cartão e subir o Wi-Fi
       levam quase um segundo, e assim ela ocupa essa espera em vez de somar */
    ui_splash();

    panel_cfg_init();
    provision_from_sd();
    herdr_model_init();
    net_wifi_init();
    const panel_cfg_t *c = panel_cfg_get();
    if (panel_cfg_wifi_ok()) {
        net_wifi_connect(c->wifi_ssid, c->wifi_pass);
    }
    /* as tasks esperam o Wi-Fi subir sozinhas; sem host habilitado, nenhuma sobe */
    herdr_conn_start();

    ui_splash_hold(1200);   /* tempo mínimo de tela, descontado o que o boot já gastou */
    ui_init();
}

void loop()
{
    M5Cardputer.update();
    ui_tick();
    delay(5);
}
