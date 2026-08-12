// Protocolo de descoberta de ponte por broadcast (herdr-find / herdr-here).
// Módulo puro: monta o probe e valida a resposta, sem sockets — a janela de
// rede vive em herdr_conn.c. C11 sem ESP-IDF/LVGL: compilável no host para
// teste (scripts/discovery_test.c), com HMAC-SHA256 do CommonCrypto no Mac
// e do mbedtls no chip.
//
// Segurança: a resposta prova posse do token sem transportá-lo —
// h = HMAC-SHA256(token, "nonce|ip|port|name") — e quem valida adota o
// ip/port ASSINADOS, nunca o remetente do datagrama (anti-relay: o probe é
// broadcast, o nonce é público na LAN).
#ifndef DISCOVERY_H
#define DISCOVERY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DISC_IP_LEN   16  /* dotted-quad máx 15 + NUL */
#define DISC_NAME_LEN 16  /* mesmo teto do rótulo (CFG_NAME_LEN) */

typedef struct {
    char     ip[DISC_IP_LEN];
    char     name[DISC_NAME_LEN];
    uint16_t port;
} disc_reply_t;

/** Monta o probe {"t":"herdr-find","id":...,"n":...} em buf.
 *  id = 6 hex do MAC, nonce = 32 hex. Retorna o comprimento, ou -1 se
 *  não couber. */
int discovery_build_probe(char *buf, size_t cap, const char *id, const char *nonce);

/** Valida uma resposta herdr-here contra token+nonce e extrai ip/port/name.
 *  false para qualquer coisa fora do esperado (fail-closed): JSON inválido,
 *  campos ausentes, ip que não é IPv4 unicast plausível, name vazio/longo/
 *  com '|', h que não bate com HMAC(token, "nonce|ip|port|name"). */
bool discovery_check_reply(const char *buf, size_t len, const char *token,
                           const char *nonce, disc_reply_t *out);

#ifdef __cplusplus
}
#endif

#endif
