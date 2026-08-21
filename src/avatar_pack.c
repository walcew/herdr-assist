#include "avatar_pack.h"

#include <fcntl.h>
#include <string.h>
#include <unistd.h>

#include "esp_heap_caps.h"
#include "esp_log.h"

static const char *TAG = "avatar_pack";

/* Pedaço de leitura, em RAM interna. Medido no painel lendo 1,1MB do cartão:
   4KB dá 926KB/s e 16KB dá 1142KB/s, então vale pedir os 16 — e cair para 4 se
   não houver, porque a RAM interna aqui é escassa e some ao longo do boot. */
#define READ_CHUNK      16384
#define READ_CHUNK_MIN  4096

/* O gerador escreve os structs byte a byte (struct.pack em Python). Se o
   compilador inserir enchimento, tudo lido daqui para frente sai deslocado —
   e o erro seria silencioso, não uma falha de build. Daí as travas. */
_Static_assert(sizeof(hav_header_t) == 32, "cabeçalho .hav mudou de tamanho");
_Static_assert(sizeof(hav_anim_t) == 20, "entrada de animação .hav mudou de tamanho");

/**
 * Confere a estrutura inteira e resolve os ponteiros.
 *
 * A regra é uma só: ao sair daqui com true, todo ponteiro em *out aponta para
 * dentro de [data, data+size) e todo comprimento cabe. Quem chama não precisa
 * checar mais nada — e nenhuma leitura depois disso pode sair do buffer.
 */
static bool parse(const uint8_t *data, size_t size, avatar_pack_t *out)
{
    if (size < sizeof(hav_header_t)) {
        ESP_LOGW(TAG, "pacote de %u bytes é menor que o cabeçalho", (unsigned)size);
        return false;
    }
    hav_header_t h;
    memcpy(&h, data, sizeof(h));          /* memcpy: data pode não estar alinhado */
    if (memcmp(h.magic, HAV_MAGIC, 4) != 0 || h.version != HAV_VERSION) {
        ESP_LOGW(TAG, "não é um pacote HAV%d", HAV_VERSION);
        return false;
    }
    if (h.anims < 1 || h.anims > HAV_MAX_ANIMS ||
        size < sizeof(hav_header_t) + (size_t)h.anims * sizeof(hav_anim_t)) {
        ESP_LOGW(TAG, "tabela de %u animações não cabe", h.anims);
        return false;
    }

    memset(out, 0, sizeof(*out));
    memset(out->by_role, -1, sizeof(out->by_role));
    memcpy(out->name, h.name, sizeof(out->name) - 1);
    out->key = h.key;
    out->sleep_s = h.sleep_s;
    out->zoom = (hav_zoom_t)(h.flags & HAV_FLAG_ZOOM_MASK);
    out->antialias = !(h.flags & HAV_FLAG_NO_ANTIALIAS);
    out->count = h.anims;

    for (int i = 0; i < h.anims; i++) {
        hav_anim_t a;
        memcpy(&a, data + sizeof(hav_header_t) + (size_t)i * sizeof(a), sizeof(a));

        if (a.role >= HAV_ROLE_COUNT || a.frames < 1 ||
            a.width < 1 || a.height < 1 ||
            a.width > HAV_MAX_DIM || a.height > HAV_MAX_DIM) {
            ESP_LOGW(TAG, "animação %d: papel/dimensões inválidos", i);
            return false;
        }
        /* offsets são uint32: bloco desalinhado faria leitura desalinhada */
        if (a.data_off % 4 != 0 || a.data_off > size) {
            ESP_LOGW(TAG, "animação %d: bloco em offset inválido", i);
            return false;
        }
        size_t off_bytes = ((size_t)a.frames + 1) * sizeof(uint32_t);
        if (size - a.data_off < off_bytes) {
            ESP_LOGW(TAG, "animação %d: tabela de frames não cabe", i);
            return false;
        }

        const uint32_t *offsets = (const uint32_t *)(data + a.data_off);
        /* offsets crescentes e sentinela no fim: é o que garante que
           offsets[f] e offsets[f+1] delimitam um frame de verdade */
        if (offsets[0] != 0) {
            ESP_LOGW(TAG, "animação %d: primeiro offset não é zero", i);
            return false;
        }
        for (int f = 0; f < a.frames; f++) {
            if (offsets[f + 1] < offsets[f]) {
                ESP_LOGW(TAG, "animação %d: offsets fora de ordem", i);
                return false;
            }
        }
        size_t rle_bytes = (size_t)offsets[a.frames] * sizeof(uint16_t);
        size_t need = off_bytes + rle_bytes + a.seq_len;
        if (size - a.data_off < need) {
            ESP_LOGW(TAG, "animação %d: dados não cabem no pacote", i);
            return false;
        }

        const uint16_t *rle = (const uint16_t *)(data + a.data_off + off_bytes);
        const uint8_t *seq = NULL;
        if (a.seq_len) {
            seq = data + a.data_off + off_bytes + rle_bytes;
            for (int s = 0; s < a.seq_len; s++) {
                if (seq[s] >= a.frames) {
                    ESP_LOGW(TAG, "animação %d: sequência aponta fora", i);
                    return false;
                }
            }
            if (a.seq_loop >= a.seq_len) {
                ESP_LOGW(TAG, "animação %d: laço da sequência aponta fora", i);
                return false;
            }
        }

        out->anims[i] = (avatar_anim_t){
            .offsets = offsets,
            .rle = rle,
            .rle_end = rle + offsets[a.frames],
            .seq = seq,
            .frame_ms = a.frame_ms ? a.frame_ms : 100,   /* 0 travaria o tick */
            .width = a.width,
            .height = a.height,
            .frames = a.frames,
            .seq_len = a.seq_len,
            .seq_loop = a.seq_loop,
            .loop = a.loop != 0,
        };
        /* papel repetido: vale o primeiro, e o segundo fica inalcançável */
        if (out->by_role[a.role] < 0) {
            out->by_role[a.role] = (int8_t)i;
        }
    }

    /* Sem ociosidade não há para onde cair quando falta um papel. */
    if (out->by_role[HAV_ROLE_IDLE] < 0) {
        ESP_LOGW(TAG, "pacote sem animação de ociosidade");
        return false;
    }
    return true;
}

bool avatar_pack_load_mem(const uint8_t *data, size_t size, avatar_pack_t *out)
{
    return parse(data, size, out);
}

bool avatar_pack_load_file(const char *path, avatar_pack_t *out)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        ESP_LOGW(TAG, "não abriu %s", path);
        return false;
    }
    off_t size = lseek(fd, 0, SEEK_END);
    lseek(fd, 0, SEEK_SET);
    if (size <= 0 || (size_t)size > HAV_MAX_BYTES) {
        ESP_LOGW(TAG, "%s tem %ld bytes (teto %u)", path, (long)size, HAV_MAX_BYTES);
        close(fd);
        return false;
    }

    uint8_t *blob = heap_caps_malloc((size_t)size, MALLOC_CAP_SPIRAM);
    if (!blob) {
        ESP_LOGW(TAG, "sem PSRAM para %s (%ld bytes)", path, (long)size);
        close(fd);
        return false;
    }
    /* Em pedaços, por um staging em RAM INTERNA — nunca lendo direto para a
       PSRAM nem pelo stdio, e os dois motivos são o mesmo. O FATFS só entrega a
       leitura por DMA multi-bloco quando o destino é alcançável por DMA; um
       destino em PSRAM, ou o buffer interno que o fread do newlib interpõe,
       derrubam para o caminho bloco a bloco. Medido lendo 1,1MB do cartão:
       fread 409KB/s, read() para PSRAM 472KB/s, read() para staging DMA
       1142KB/s. O pedaço cai para 4KB se não houver 16 — a RAM interna aqui é
       escassa e some ao longo do boot. */
    size_t chunk = READ_CHUNK;
    uint8_t *stage = heap_caps_malloc(chunk, MALLOC_CAP_DMA);
    if (!stage) {
        chunk = READ_CHUNK_MIN;
        stage = heap_caps_malloc(chunk, MALLOC_CAP_DMA);
    }
    if (!stage) {
        ESP_LOGW(TAG, "sem RAM interna para o buffer de leitura");
        heap_caps_free(blob);
        close(fd);
        return false;
    }
    size_t got = 0;
    while (got < (size_t)size) {
        size_t want = (size_t)size - got;
        if (want > chunk) {
            want = chunk;
        }
        int n = read(fd, stage, want);
        if (n <= 0) {
            break;
        }
        memcpy(blob + got, stage, (size_t)n);
        got += (size_t)n;
    }
    heap_caps_free(stage);
    close(fd);
    if (got != (size_t)size) {
        ESP_LOGW(TAG, "%s: li %u de %ld bytes", path, (unsigned)got, (long)size);
        heap_caps_free(blob);
        return false;
    }
    if (!parse(blob, (size_t)size, out)) {
        heap_caps_free(blob);
        return false;
    }
    out->owned = blob;
    return true;
}

void avatar_pack_free(avatar_pack_t *p)
{
    heap_caps_free(p->owned);   /* NULL nos embutidos, e free(NULL) é legal */
    memset(p, 0, sizeof(*p));
}
