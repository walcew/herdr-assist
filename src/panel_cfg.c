#include "panel_cfg.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "panel_cfg";

/* Chaves simples por campo (h0n, h0h, h0p, h0e, ...) em vez de um blob:
   sobrevive a mudanças no struct e permite gerar a partição com
   nvs_partition_gen sem depender do layout de memória do compilador. */
#define CFG_NAMESPACE "panel"

static panel_cfg_t s_cfg;

esp_err_t panel_cfg_init(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }

    memset(&s_cfg, 0, sizeof(s_cfg));
    nvs_handle_t h;
    if (nvs_open(CFG_NAMESPACE, NVS_READONLY, &h) != ESP_OK) {
        ESP_LOGI(TAG, "sem config salva (primeiro boot)");
        return ESP_OK;
    }

    size_t len = sizeof(s_cfg.wifi_ssid);
    nvs_get_str(h, "ssid", s_cfg.wifi_ssid, &len);
    len = sizeof(s_cfg.wifi_pass);
    nvs_get_str(h, "pass", s_cfg.wifi_pass, &len);
    /* chave ausente (config gravada antes do idioma existir) deixa o zero do
       memset, que é o padrão de fábrica */
    nvs_get_u8(h, "lang", &s_cfg.lang);
    nvs_get_u8(h, "orient", &s_cfg.orient);
    nvs_get_u8(h, "noauto", &s_cfg.no_auto_update);

    for (int i = 0; i < CFG_MAX_HOSTS; i++) {
        panel_host_t *hh = &s_cfg.hosts[i];
        char key[8];
        snprintf(key, sizeof(key), "h%dn", i);
        len = sizeof(hh->name);
        nvs_get_str(h, key, hh->name, &len);
        snprintf(key, sizeof(key), "h%dh", i);
        len = sizeof(hh->host);
        nvs_get_str(h, key, hh->host, &len);
        snprintf(key, sizeof(key), "h%dt", i);
        len = sizeof(hh->token);
        nvs_get_str(h, key, hh->token, &len);
        snprintf(key, sizeof(key), "h%dp", i);
        nvs_get_u16(h, key, &hh->port);
        uint8_t en = 0;
        snprintf(key, sizeof(key), "h%de", i);
        nvs_get_u8(h, key, &en);
        /* auto (host vazio + token) também é utilizável: o endereço vem da
           descoberta por broadcast em runtime */
        hh->enabled = en && hh->port && (hh->host[0] || hh->token[0]);
    }
    nvs_close(h);

    int hosts = 0;
    for (int i = 0; i < CFG_MAX_HOSTS; i++) {
        hosts += s_cfg.hosts[i].enabled;
    }
    ESP_LOGI(TAG, "config carregada: ssid=\"%s\", %d host(s) habilitado(s)",
             s_cfg.wifi_ssid, hosts);
    return ESP_OK;
}

const panel_cfg_t *panel_cfg_get(void)
{
    return &s_cfg;
}

esp_err_t panel_cfg_save(const panel_cfg_t *cfg)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(CFG_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    nvs_set_str(h, "ssid", cfg->wifi_ssid);
    nvs_set_str(h, "pass", cfg->wifi_pass);
    nvs_set_u8(h, "lang", cfg->lang);
    nvs_set_u8(h, "orient", cfg->orient);
    nvs_set_u8(h, "noauto", cfg->no_auto_update);
    for (int i = 0; i < CFG_MAX_HOSTS; i++) {
        const panel_host_t *hh = &cfg->hosts[i];
        char key[8];
        snprintf(key, sizeof(key), "h%dn", i);
        nvs_set_str(h, key, hh->name);
        snprintf(key, sizeof(key), "h%dh", i);
        nvs_set_str(h, key, hh->host);
        snprintf(key, sizeof(key), "h%dt", i);
        nvs_set_str(h, key, hh->token);
        snprintf(key, sizeof(key), "h%dp", i);
        nvs_set_u16(h, key, hh->port);
        snprintf(key, sizeof(key), "h%de", i);
        nvs_set_u8(h, key, hh->enabled ? 1 : 0);
    }
    err = nvs_commit(h);
    nvs_close(h);
    ESP_LOGI(TAG, "config salva (%s)", err == ESP_OK ? "ok" : "falha");
    return err;
}

bool panel_cfg_wifi_ok(void)
{
    return s_cfg.wifi_ssid[0] != '\0';
}
