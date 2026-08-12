// Teste host do discovery (fora de src/ de propósito: o CMakeLists do
// firmware faz GLOB_RECURSE em src/ e um main() ali quebraria o build).
//
//   CJSON=~/.platformio/packages/framework-espidf/components/json/cJSON
//   cc -std=c11 -Wall -Wextra -Isrc -I$CJSON -o /tmp/dt \
//      scripts/discovery_test.c src/discovery.c $CJSON/cJSON.c
//   /tmp/dt
//
// Os vetores foram gerados com o MESMO código da ponte (teste cruzado
// Python <-> C); para regerar:
//   python3 -c 'import hmac; print(hmac.new(b"<token>", b"<nonce>|<ip>|<port>|<name>", "sha256").hexdigest())'
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "discovery.h"

#define CHECK(cond) do { \
    if (!(cond)) { fprintf(stderr, "FALHOU linha %d: %s\n", __LINE__, #cond); exit(1); } \
} while (0)

#define TOKEN "0123456789abcdef0123456789abcdef"
#define NONCE "00112233445566778899aabbccddeeff"

/* h = HMAC(TOKEN, NONCE|192.168.150.100|9375|WalcewAir) */
#define REPLY_OK \
    "{\"t\":\"herdr-here\",\"name\":\"WalcewAir\",\"ip\":\"192.168.150.100\"," \
    "\"port\":9375,\"h\":\"60fa45e91c6766ef9a1ecb45d443e4f2936b153bbfed678b9772dc0bc7a313d4\"}"

/* h = HMAC(TOKEN, NONCE|10.0.0.7|19375|quinze-chars-ab) — name no teto de 15 */
#define REPLY_LIMIT \
    "{\"t\":\"herdr-here\",\"name\":\"quinze-chars-ab\",\"ip\":\"10.0.0.7\"," \
    "\"port\":19375,\"h\":\"771c50c218e570ed4cc2c01f6941449a5883c06f5d6b7cc89894b8ea6e3129c9\"}"

static bool check(const char *reply, const char *nonce, disc_reply_t *out)
{
    return discovery_check_reply(reply, strlen(reply), TOKEN, nonce, out);
}

/* troca um campo do REPLY_OK por outro valor (mantendo o h original) */
static void swapped(char *dst, size_t cap, const char *from, const char *to)
{
    const char *p = strstr(REPLY_OK, from);
    CHECK(p != NULL);
    snprintf(dst, cap, "%.*s%s%s", (int)(p - REPLY_OK), REPLY_OK, to,
             p + strlen(from));
}

int main(void)
{
    disc_reply_t r;
    char buf[256];

    /* probe: formato exato e truncamento */
    CHECK(discovery_build_probe(buf, sizeof(buf), "a1b2c3", NONCE) > 0);
    CHECK(strcmp(buf, "{\"t\":\"herdr-find\",\"id\":\"a1b2c3\",\"n\":\"" NONCE "\"}") == 0);
    CHECK(discovery_build_probe(buf, 32, "a1b2c3", NONCE) == -1);

    /* caso feliz: aceita e extrai os campos assinados */
    memset(&r, 0, sizeof(r));
    CHECK(check(REPLY_OK, NONCE, &r));
    CHECK(strcmp(r.ip, "192.168.150.100") == 0);
    CHECK(strcmp(r.name, "WalcewAir") == 0);
    CHECK(r.port == 9375);

    /* name no teto de 15 chars */
    memset(&r, 0, sizeof(r));
    CHECK(check(REPLY_LIMIT, NONCE, &r));
    CHECK(strcmp(r.name, "quinze-chars-ab") == 0);
    CHECK(r.port == 19375);

    /* h em caixa alta continua válido */
    {
        char up[256];
        const char *p = strstr(REPLY_OK, "\"h\":\"");
        size_t at = (size_t)(p - REPLY_OK) + 5;
        snprintf(up, sizeof(up), "%s", REPLY_OK);
        for (size_t i = at; i < at + 64; i++) {
            if (up[i] >= 'a' && up[i] <= 'f') up[i] = (char)(up[i] - 32);
        }
        CHECK(check(up, NONCE, &r));
    }

    /* h errado (último nibble trocado) */
    swapped(buf, sizeof(buf), "a313d4\"}", "a313d5\"}");
    CHECK(!check(buf, NONCE, &r));

    /* nonce diferente do enviado */
    CHECK(!check(REPLY_OK, "ffffffffffffffffffffffffffffffff", &r));

    /* token errado nao valida */
    CHECK(!discovery_check_reply(REPLY_OK, strlen(REPLY_OK),
                                 "deadbeefdeadbeefdeadbeefdeadbeef", NONCE, &r));

    /* qualquer campo assinado adulterado invalida o h */
    swapped(buf, sizeof(buf), "192.168.150.100", "192.168.150.66");
    CHECK(!check(buf, NONCE, &r));
    swapped(buf, sizeof(buf), "9375", "9376");
    CHECK(!check(buf, NONCE, &r));
    swapped(buf, sizeof(buf), "WalcewAir", "Impostora");
    CHECK(!check(buf, NONCE, &r));

    /* ips implausíveis: lixo, 0.x, loopback, multicast, broadcast, longo */
    {
        const char *bad[] = { "abc", "999.1.1.1", "0.0.0.0", "127.0.0.1",
                              "224.0.0.1", "255.255.255.255", "1.2.3.4.5",
                              "192.168.150.1000" };
        for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
            swapped(buf, sizeof(buf), "192.168.150.100", bad[i]);
            CHECK(!check(buf, NONCE, &r));
        }
    }

    /* port fora da faixa */
    swapped(buf, sizeof(buf), "\"port\":9375", "\"port\":0");
    CHECK(!check(buf, NONCE, &r));
    swapped(buf, sizeof(buf), "\"port\":9375", "\"port\":70000");
    CHECK(!check(buf, NONCE, &r));

    /* name inválido: vazio, longo demais (16+), com '|' */
    swapped(buf, sizeof(buf), "WalcewAir", "");
    CHECK(!check(buf, NONCE, &r));
    swapped(buf, sizeof(buf), "WalcewAir", "dezesseis-chars!");
    CHECK(!check(buf, NONCE, &r));
    swapped(buf, sizeof(buf), "WalcewAir", "a|b");
    CHECK(!check(buf, NONCE, &r));

    /* t errado, h curto, JSON truncado, lixo */
    swapped(buf, sizeof(buf), "herdr-here", "herdr-fake");
    CHECK(!check(buf, NONCE, &r));
    swapped(buf, sizeof(buf), "a313d4\"}", "\"}");
    CHECK(!check(buf, NONCE, &r));
    CHECK(!discovery_check_reply(REPLY_OK, strlen(REPLY_OK) / 2, TOKEN, NONCE, &r));
    CHECK(!discovery_check_reply("nem json", 8, TOKEN, NONCE, &r));

    printf("discovery_test: todos os casos passaram\n");
    return 0;
}
