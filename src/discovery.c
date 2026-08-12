// Implementação do protocolo de descoberta — ver discovery.h.
#include "discovery.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include "cJSON.h"

#ifdef __APPLE__
#include <CommonCrypto/CommonHMAC.h>
#else
#include "mbedtls/md.h"
#endif

/* HMAC-SHA256 one-shot com backend por plataforma: CommonCrypto no teste
   de host, mbedtls (já linkado pelo IDF) no chip. */
static void hmac_sha256(const uint8_t *key, size_t key_len,
                        const uint8_t *msg, size_t msg_len, uint8_t out[32])
{
#ifdef __APPLE__
    CCHmac(kCCHmacAlgSHA256, key, key_len, msg, msg_len, out);
#else
    mbedtls_md_hmac(mbedtls_md_info_from_type(MBEDTLS_MD_SHA256),
                    key, key_len, msg, msg_len, out);
#endif
}

static void to_hex(const uint8_t *in, size_t n, char *out)
{
    static const char hexd[] = "0123456789abcdef";
    for (size_t i = 0; i < n; i++) {
        out[2 * i]     = hexd[in[i] >> 4];
        out[2 * i + 1] = hexd[in[i] & 0xf];
    }
    out[2 * n] = '\0';
}

int discovery_build_probe(char *buf, size_t cap, const char *id, const char *nonce)
{
    /* id e nonce são hex gerados por nós — sem escaping a fazer */
    int n = snprintf(buf, cap, "{\"t\":\"herdr-find\",\"id\":\"%s\",\"n\":\"%s\"}",
                     id, nonce);
    return (n > 0 && (size_t)n < cap) ? n : -1;
}

/* IPv4 unicast plausível para uma ponte: rejeita lixo, 0.x, loopback,
   multicast e acima (224+, inclui 255.255.255.255). */
static bool valid_ipv4(const char *s)
{
    unsigned a, b, c, d;
    char tail;
    if (strlen(s) > 15 ||
        sscanf(s, "%3u.%3u.%3u.%3u%c", &a, &b, &c, &d, &tail) != 4) {
        return false;
    }
    if (a > 255 || b > 255 || c > 255 || d > 255) {
        return false;
    }
    return a != 0 && a != 127 && a < 224;
}

bool discovery_check_reply(const char *buf, size_t len, const char *token,
                           const char *nonce, disc_reply_t *out)
{
    cJSON *root = cJSON_ParseWithLength(buf, len);
    if (!root) {
        return false;
    }
    bool ok = false;
    const cJSON *type = cJSON_GetObjectItem(root, "t");
    const cJSON *name = cJSON_GetObjectItem(root, "name");
    const cJSON *ip   = cJSON_GetObjectItem(root, "ip");
    const cJSON *port = cJSON_GetObjectItem(root, "port");
    const cJSON *h    = cJSON_GetObjectItem(root, "h");

    if (cJSON_IsString(type) && strcmp(type->valuestring, "herdr-here") == 0 &&
        cJSON_IsString(name) && name->valuestring[0] &&
        strlen(name->valuestring) < DISC_NAME_LEN &&
        !strchr(name->valuestring, '|') &&
        cJSON_IsString(ip) && valid_ipv4(ip->valuestring) &&
        cJSON_IsNumber(port) && port->valueint > 0 && port->valueint <= 65535 &&
        cJSON_IsString(h) && strlen(h->valuestring) == 64) {
        /* canônica idêntica à da ponte: nonce|ip|port|name */
        char canon[96];
        int n = snprintf(canon, sizeof(canon), "%s|%s|%d|%s",
                         nonce, ip->valuestring, port->valueint, name->valuestring);
        if (n > 0 && (size_t)n < sizeof(canon)) {
            uint8_t mac[32];
            char expect[65];
            hmac_sha256((const uint8_t *)token, strlen(token),
                        (const uint8_t *)canon, (size_t)n, mac);
            to_hex(mac, sizeof(mac), expect);
            /* tolera caixa alta no h recebido; o nosso é minúsculo */
            char got[65];
            for (int i = 0; i < 64; i++) {
                got[i] = (char)tolower((unsigned char)h->valuestring[i]);
            }
            got[64] = '\0';
            if (strcmp(expect, got) == 0) {
                snprintf(out->ip, sizeof(out->ip), "%s", ip->valuestring);
                snprintf(out->name, sizeof(out->name), "%s", name->valuestring);
                out->port = (uint16_t)port->valueint;
                ok = true;
            }
        }
    }
    cJSON_Delete(root);
    return ok;
}
