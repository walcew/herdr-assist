
// #include <Arduino.h>
#include <lvgl.h>
#include "display.h"
#include "esp_bsp.h"
#include "i18n.h"
#include "lv_port.h"
#include "net.h"
#include "panel_cfg.h"
#include "herdr_model.h"
#include "herdr_ui.h"
#include "herdr_ui_settings.h"
#include "herdr_conn.h"
#include "fw_update.h"
#include <esp_log.h>   // Add this line to include the header file that declares ESP_LOGI
#include <esp_flash.h> // Add this line to include the header file that declares esp_flash_t
#include <esp_chip_info.h>
#include <esp_system.h>
#include <esp_heap_caps.h>

static const char *TAG = "DEMO_LVGL";

#define BUILD (String(__DATE__) + " - " + String(__TIME__)).c_str()

#define logSection(section) \
  ESP_LOGI(TAG, "\n\n************* %s **************\n", section);

/* Rotação usada quando a config pede paisagem — qual das duas bordas vira o
   topo depende de como o painel está montado: se a imagem sair de cabeça para
   baixo na sua base, troque por LV_DISP_ROT_270. O BSP cuida do resto: troca
   hres/vres, rotaciona o buffer no flush e remapeia as coordenadas do toque. */
#define UI_LANDSCAPE_ROT LV_DISP_ROT_90


void setup();

#if !CONFIG_AUTOSTART_ARDUINO
void app_main()
{
  // initialize arduino library before we start the tasks
  // initArduino();

  setup();
}
#endif
void setup()
{
  //  String title = "LVGL porting example";

  // Serial.begin(115200);
  logSection("LVGL porting example start");
  esp_chip_info_t chip_info;
  uint32_t flash_size;
  esp_chip_info(&chip_info);
  ESP_LOGI(TAG, "This is %s chip with %d CPU core(s), %s%s%s%s, ",
           CONFIG_IDF_TARGET,
           chip_info.cores,
           (chip_info.features & CHIP_FEATURE_WIFI_BGN) ? "WiFi/" : "",
           (chip_info.features & CHIP_FEATURE_BT) ? "BT" : "",
           (chip_info.features & CHIP_FEATURE_BLE) ? "BLE" : "",
           (chip_info.features & CHIP_FEATURE_IEEE802154) ? ", 802.15.4 (Zigbee/Thread)" : "");

  unsigned major_rev = chip_info.revision / 100;
  unsigned minor_rev = chip_info.revision % 100;
  ESP_LOGI(TAG, "silicon revision v%d.%d, ", major_rev, minor_rev);
  if (esp_flash_get_size(NULL, &flash_size) != ESP_OK)
  {
    ESP_LOGI(TAG, "Get flash size failed");
    return;
  }

  ESP_LOGI(TAG, "%" PRIu32 "MB %s flash", flash_size / (uint32_t)(1024 * 1024),
           (chip_info.features & CHIP_FEATURE_EMB_FLASH) ? "embedded" : "external");

  ESP_LOGI(TAG, "Minimum free heap size: %" PRIu32 " bytes", esp_get_minimum_free_heap_size());
  size_t freePsram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
  ESP_LOGI(TAG, "Free PSRAM: %d bytes", freePsram);

  logSection("Config");
  panel_cfg_init();
  /* antes de montar a UI: as telas são construídas uma vez e não se retraduzem */
  i18n_set_lang((ui_lang_t)panel_cfg_get()->lang);
  ESP_LOGI(TAG, "idioma: %s", i18n_lang_name(i18n_lang()));
  ESP_LOGI(TAG, "orientacao: %s",
           panel_cfg_get()->orient == CFG_ORIENT_LANDSCAPE ? "paisagem" : "retrato");

  logSection("Initialize panel device");
  // ESP_LOGI(TAG, "Initialize panel device");
  bsp_display_cfg_t cfg = {
      .lvgl_port_cfg = ESP_LVGL_PORT_INIT_CONFIG(),
      .buffer_size = EXAMPLE_LCD_QSPI_H_RES * EXAMPLE_LCD_QSPI_V_RES,
      /* a orientação vem da NVS, lida logo acima em panel_cfg_init() */
      .rotate = panel_cfg_get()->orient == CFG_ORIENT_LANDSCAPE
                    ? UI_LANDSCAPE_ROT : LV_DISP_ROT_NONE,
  };
  /* O padrão do port são 4KB, apertado para renderizar o texto do terminal */
  cfg.lvgl_port_cfg.task_stack = 8192;

  bsp_display_start_with_config(&cfg);
  bsp_display_backlight_on();

  logSection("Create UI");
  /* Lock the mutex due to the LVGL APIs are not thread-safe */
  bsp_display_lock(0);

  /**
   * Try an example. Don't forget to uncomment header.
   * See all the examples online: https://docs.lvgl.io/master/examples.html
   * source codes: https://github.com/lvgl/lvgl/tree/e7f88efa5853128bf871dde335c0ca8da9eb7731/examples
   */
  //  lv_example_btn_1();

  herdr_model_init();
  herdr_ui_init();

  /* Release the mutex */
  bsp_display_unlock();

  logSection("Wi-Fi");
  net_wifi_init();
  if (panel_cfg_wifi_ok())
  {
    const panel_cfg_t *cfg = panel_cfg_get();
    net_wifi_connect(cfg->wifi_ssid, cfg->wifi_pass);
  }
  else
  {
    /* primeiro boot: sem rede salva, abre direto as configuracoes */
    ESP_LOGI(TAG, "sem Wi-Fi configurado, abrindo tela de configuracoes");
    bsp_display_lock(0);
    herdr_ui_show_settings();
    bsp_display_unlock();
  }

  logSection("Pontes");
  herdr_conn_start();

  /* por último: chegar aqui é o critério de "boot são" do rollback */
  fw_update_init();

  logSection("LVGL porting example end");
}

void loop()
{
  ESP_LOGI(TAG, "IDLE loop");
  // delay(1000);
}