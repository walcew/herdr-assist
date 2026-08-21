#include "sd.h"

#include "driver/sdspi_host.h"
#include "driver/spi_common.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdmmc_cmd.h"

static const char *TAG = "sd";

/* Pinos do slot TF, da tabela do fabricante (docs/GPIO_pinout.jpg):
   DAT2 em 3V3, CLK=12, MOSI(CMD)=11, MISO(DAT0)=13, CS(CD/DAT3)=10. */
#define PIN_CS    GPIO_NUM_10
#define PIN_MOSI  GPIO_NUM_11
#define PIN_CLK   GPIO_NUM_12
#define PIN_MISO  GPIO_NUM_13

/* SPI2 é do display (QSPI, esp_bsp.h). Estes quatro pinos não são os do IOMUX
   do SPI3, então passam pela matriz de GPIO e não sustentam mais que ~40MHz;
   20MHz é o que o modo SPI de cartão entrega com folga. */
#define SD_SPI_HOST   SPI3_HOST
#define SD_FREQ_KHZ   20000

static sdmmc_card_t *s_card;

/* Uma tentativa completa: sobe o barramento, tenta montar e desfaz tudo se
   falhar — inclusive o spi_bus_free, para a tentativa seguinte partir do zero.
   `allow_format` só é verdadeiro vindo do sd_format(), depois de o usuário
   confirmar na tela. */
static esp_err_t try_mount(bool allow_format)
{
    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = SD_SPI_HOST;
    host.max_freq_khz = SD_FREQ_KHZ;

    spi_bus_config_t bus = {
        .mosi_io_num = PIN_MOSI,
        .miso_io_num = PIN_MISO,
        .sclk_io_num = PIN_CLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        /* um setor de cartão (512) por transação basta; o teto só dimensiona
           o descritor de DMA, e pedir mais reservaria memória à toa */
        .max_transfer_sz = 4096,
    };
    esp_err_t err = spi_bus_initialize(SD_SPI_HOST, &bus, SPI_DMA_CH_AUTO);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "spi_bus_initialize: %s", esp_err_to_name(err));
        return err;
    }

    sdspi_device_config_t slot = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot.host_id = SD_SPI_HOST;
    slot.gpio_cs = PIN_CS;

    esp_vfs_fat_mount_config_t mount = {
        /* NUNCA formatar sozinho: o cartão é do usuário e pode ter o que ele
           quiser. Cartão ilegível vira erro e o painel segue sem ele — a menos
           que ele mesmo tenha pedido a formatação. */
        .format_if_mount_failed = allow_format,
        .max_files = 4,
        .allocation_unit_size = 16 * 1024,
    };
    err = esp_vfs_fat_sdspi_mount(SD_ROOT, &host, &slot, &mount, &s_card);
    if (err != ESP_OK) {
        s_card = NULL;
        spi_bus_free(SD_SPI_HOST);
    }
    return err;
}

esp_err_t sd_mount(void)
{
    if (s_card) {
        return ESP_OK;
    }

    /* O VDD do slot é fixo em 3V3 (docs/GPIO_pinout.jpg): o cartão NÃO desliga
       num reset a quente — reinicia no estado em que a execução anterior o
       deixou. Medido em 6 boots por RTS logo após uma regravação, 3 falharam no
       CMD8 (ESP_ERR_INVALID_RESPONSE) e os 3 seguintes montaram, sem nada mudar
       entre eles. Daí o laço: é barato (não custa nada quando a primeira passa)
       e cobre uma falha que foi observada de fato. Slot vazio falha as três, em
       ~1s de boot. Cartão que responde CRC errado a comando de CRC fixo está
       preso de um jeito que nem o laço nem regravar o firmware resolvem — só
       tirar a energia; se acontecer em campo, é desligar e ligar. */
    esp_err_t err = ESP_FAIL;
    for (int attempt = 1; attempt <= 3; attempt++) {
        err = try_mount(false);
        if (err == ESP_OK) {
            break;
        }
        ESP_LOGD(TAG, "tentativa %d: %s", attempt, esp_err_to_name(err));
        vTaskDelay(pdMS_TO_TICKS(120));
    }
    if (err != ESP_OK) {
        /* slot vazio é caminho normal; nada aqui é fatal para o painel */
        ESP_LOGW(TAG, "sem cartão em %s: %s", SD_ROOT, esp_err_to_name(err));
        return err;
    }

    uint64_t total = 0, freeb = 0;
    esp_vfs_fat_info(SD_ROOT, &total, &freeb);
    ESP_LOGI(TAG, "montado em %s: %s %lluMB, %lluMB livres, setor %u",
             SD_ROOT, s_card->cid.name, total >> 20, freeb >> 20,
             (unsigned)s_card->csd.sector_size);
    return ESP_OK;
}

bool sd_is_mounted(void)
{
    return s_card != NULL;
}

uint64_t sd_free_bytes(void)
{
    uint64_t total = 0, freeb = 0;
    if (!s_card || esp_vfs_fat_info(SD_ROOT, &total, &freeb) != ESP_OK) {
        return 0;
    }
    return freeb;
}

esp_err_t sd_format(void)
{
    if (!s_card) {
        /* Uma montagem normal primeiro: se o cartão só estava preso (ver o laço
           do sd_mount), formatar pela via de baixo evita escrever as tabelas
           duas vezes. Só o que resistir a isso é ilegível de fato, e aí montar
           com o format ligado é a única via — o esp_vfs_fat_sdcard_format exige
           o volume montado. O IDF só formata em FR_NO_FILESYSTEM/FR_INT_ERR, ou
           seja com o cartão respondendo: slot vazio falha antes, sem formatar
           nada. */
        if (sd_mount() != ESP_OK) {
            return try_mount(true);
        }
    }

    esp_err_t err = esp_vfs_fat_sdcard_format(SD_ROOT, s_card);

    /* Se o mkfs deu certo mas a remontagem interna falhou, o IDF já liberou o
       card e mesmo assim devolve ESP_OK — o s_card ficaria pendurado. Um
       f_getfree é o teste barato de que o volume seguiu montado. */
    uint64_t total = 0, freeb = 0;
    if (esp_vfs_fat_info(SD_ROOT, &total, &freeb) != ESP_OK) {
        ESP_LOGE(TAG, "o cartão não voltou depois de formatar");
        s_card = NULL;
        spi_bus_free(SD_SPI_HOST);   /* para um sd_mount() seguinte partir do zero */
        return ESP_FAIL;
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "formatação falhou: %s", esp_err_to_name(err));
        return err;
    }
    ESP_LOGI(TAG, "cartão formatado: %lluMB livres de %lluMB", freeb >> 20, total >> 20);
    return ESP_OK;
}
