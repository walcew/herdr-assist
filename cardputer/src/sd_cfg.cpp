#include "sd_cfg.h"

#include <string.h>

#include <SD.h>
#include <SPI.h>

#include "esp_log.h"

static const char *TAG = "sd_cfg";

/* Slot do Cardputer. O display do M5GFX vive no SPI3, então o SD fica com o
   SPI2 — que é justamente o barramento do objeto SPI padrão do Arduino no S3. */
#define SD_SCK   40
#define SD_MISO  39
#define SD_MOSI  14
#define SD_CS    12
#define SD_HZ    25000000

static bool s_mounted;

bool sd_mount(void)
{
    if (s_mounted) {
        return true;
    }
    SPI.begin(SD_SCK, SD_MISO, SD_MOSI, SD_CS);
    s_mounted = SD.begin(SD_CS, SPI, SD_HZ);
    if (!s_mounted) {
        ESP_LOGI(TAG, "sem cartao (ou nao montou)");
    }
    return s_mounted;
}

/** Corta espaços e CR das duas pontas, in-place. */
static char *trim(char *s)
{
    while (*s == ' ' || *s == '\t') {
        s++;
    }
    size_t n = strlen(s);
    while (n && (s[n - 1] == '\r' || s[n - 1] == '\n' ||
                 s[n - 1] == ' '  || s[n - 1] == '\t')) {
        s[--n] = '\0';
    }
    return s;
}

static bool parse_file(File &f, char *ssid, size_t ns, char *pass, size_t np)
{
    ssid[0] = '\0';
    pass[0] = '\0';
    char line[160];
    while (f.available()) {
        size_t len = f.readBytesUntil('\n', line, sizeof(line) - 1);
        line[len] = '\0';
        char *p = trim(line);
        if (!*p || *p == '#') {
            continue;
        }
        char *eq = strchr(p, '=');
        if (!eq) {
            continue;
        }
        *eq = '\0';
        char *key = trim(p);
        char *val = trim(eq + 1);
        if (strcasecmp(key, "ssid") == 0) {
            strlcpy(ssid, val, ns);
        } else if (strcasecmp(key, "pass") == 0 ||
                   strcasecmp(key, "password") == 0 ||
                   strcasecmp(key, "psk") == 0) {
            strlcpy(pass, val, np);
        }
    }
    /* rede aberta é válida (senha vazia); sem SSID não há o que fazer */
    return ssid[0] != '\0';
}

bool sd_wifi_read(char *ssid, size_t ssid_size, char *pass, size_t pass_size)
{
    if (!sd_mount()) {
        return false;
    }
    static const char *NAMES[] = { "/herdr-wifi.txt", "/wifi.txt" };
    for (size_t i = 0; i < sizeof(NAMES) / sizeof(NAMES[0]); i++) {
        File f = SD.open(NAMES[i]);
        if (!f) {
            continue;
        }
        bool ok = parse_file(f, ssid, ssid_size, pass, pass_size);
        f.close();
        if (ok) {
            ESP_LOGI(TAG, "wifi provisionado por %s (ssid \"%s\")", NAMES[i], ssid);
            return true;
        }
        ESP_LOGW(TAG, "%s sem ssid utilizavel", NAMES[i]);
    }
    return false;
}
