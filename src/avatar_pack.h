/**
 * @file
 * @brief Formato .hav: um avatar inteiro (animações + sprites RLE) num arquivo.
 *
 * Nasceu para tirar os avatares da flash. Antes cada um era um driver .c com
 * sua tabela de animações e seus arrays `static const`; 65% do firmware eram
 * sprites, e acrescentar um avatar exigia recompilar. O pacote leva a tabela
 * junto com os dados, então o mesmo motor toca qualquer avatar — o de fábrica,
 * embutido na flash, e os que vierem do cartão SD.
 *
 * Os bytes de RLE são os mesmos de sempre (ver rle_sprite.h); o que o formato
 * acrescenta é a tabela que estava hardcoded.
 *
 * Gerado por scripts/hav_pack.py. Little-endian, tudo alinhado em 4.
 *
 * ATENÇÃO: pacote é dado de terceiro — vem do cartão ou de um repositório na
 * internet. Nada aqui pode confiar no conteúdo: avatar_pack_load_* valida a
 * estrutura inteira antes de devolver um ponteiro, e o decoder é limitado pelo
 * fim do buffer.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define HAV_MAGIC       "HAV1"
#define HAV_VERSION     1
#define HAV_MAX_ANIMS   8
/* Teto de pacote: um .hav hostil não pode esgotar a PSRAM. O maior avatar real
   hoje (McQueen) tem 1,5MB, então 2MB dá folga sem abrir a porta. */
#define HAV_MAX_BYTES   (2u * 1024 * 1024)
/* Teto de dimensão de frame: o buffer de decode é w*h*3 e precisa caber num
   int sem estourar. A tela tem 480px; 1024 é generoso e seguro. */
#define HAV_MAX_DIM     1024

/**
 * Papel que a animação cumpre. Os valores 0..4 são os mesmos de avatar_state_t,
 * NA MESMA ORDEM, de propósito: o motor indexa pelo estado sem tradução. Mexer
 * numa das duas listas exige mexer na outra.
 */
typedef enum {
    HAV_ROLE_DISCONNECTED = 0,
    HAV_ROLE_IDLE,
    HAV_ROLE_DONE,
    HAV_ROLE_WORKING,
    HAV_ROLE_BLOCKED,
    HAV_ROLE_SLEEP,       /* opcional: entra após sleep_s de ociosidade */
    HAV_ROLE_TRANSITION,  /* opcional: one-shot antes de entrar no papel alvo */
    HAV_ROLE_COUNT,
} hav_role_t;

/** Como a escala é escolhida; os três modos vieram dos drivers substituídos. */
typedef enum {
    HAV_ZOOM_FIT = 0,   /* amplia o quanto precisar para preencher o slot */
    HAV_ZOOM_INTEGER,   /* idem, arredondado para múltiplo inteiro (pixel art) */
    HAV_ZOOM_SHRINK,    /* nunca amplia além do nativo; reduz se não couber */
} hav_zoom_t;

#define HAV_FLAG_ZOOM_MASK     0x03
#define HAV_FLAG_NO_ANTIALIAS  0x04

/* --- Layout no arquivo. Alterar qualquer um dos dois exige bumpar HAV_VERSION
   e o gerador em scripts/hav_pack.py. --- */

typedef struct {
    char     magic[4];
    uint8_t  version;
    uint8_t  flags;
    uint16_t key;        /* RGB565 tratado como transparente */
    uint8_t  anims;
    uint8_t  _pad;
    uint16_t sleep_s;    /* ociosidade até dormir; 0 = não dorme */
    char     name[20];   /* exibido na lista; UTF-8 com NUL */
} hav_header_t;

typedef struct {
    uint8_t  role;
    uint8_t  loop;       /* 0 = one-shot: ao terminar, encadeia no papel alvo */
    uint16_t frame_ms;
    uint16_t width;
    uint16_t height;
    uint16_t frames;
    uint16_t seq_len;    /* 0 = ciclo direto 0..frames-1 */
    uint16_t seq_loop;   /* passo de retorno do laço (só com seq_len > 0) */
    uint16_t _pad;
    uint32_t data_off;   /* offset do bloco no arquivo, múltiplo de 4 */
} hav_anim_t;

/* --- Em memória, já resolvido em ponteiros para dentro do pacote. --- */

typedef struct {
    const uint32_t *offsets;   /* frames+1 entradas, em WORDS dentro de rle */
    const uint16_t *rle;
    const uint16_t *rle_end;   /* limite do decoder; pacote curto não vaza */
    const uint8_t  *seq;       /* NULL quando seq_len == 0 */
    uint16_t frame_ms, width, height, frames, seq_len, seq_loop;
    bool     loop;
} avatar_anim_t;

typedef struct {
    uint8_t   *owned;          /* buffer em PSRAM a liberar; NULL se embutido */
    char       name[20];
    uint16_t   key;
    uint16_t   sleep_s;
    hav_zoom_t zoom;
    bool       antialias;
    int        count;
    avatar_anim_t anims[HAV_MAX_ANIMS];
    int8_t     by_role[HAV_ROLE_COUNT];   /* índice em anims; -1 = ausente */
} avatar_pack_t;

/**
 * Carrega um .hav do sistema de arquivos para a PSRAM e valida.
 *
 * BLOQUEIA no I/O: o cartão entrega ~520 KB/s, então um pacote de 1,5MB leva
 * ~3s. Nunca chamar da task da LVGL.
 */
bool avatar_pack_load_file(const char *path, avatar_pack_t *out);

/** Valida um pacote já em memória (o embutido na flash) sem copiar nada. */
bool avatar_pack_load_mem(const uint8_t *data, size_t size, avatar_pack_t *out);

/** Libera o que for do pacote; deixa *p zerado e reusável. */
void avatar_pack_free(avatar_pack_t *p);

#ifdef __cplusplus
}
#endif
