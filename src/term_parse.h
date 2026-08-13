// Parser de snapshot ANSI/SGR -> grid de runs estilizados.
// C11 puro, sem LVGL/ESP-IDF: compilável no host para teste
// (scripts/term_parse_test.c). O conteúdo vem do Herdr via
// pane.read format:"ansi" — apenas texto + SGR, sem cursor/OSC
// (qualquer outra sequência é consumida e ignorada).
#ifndef TERM_PARSE_H
#define TERM_PARSE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Dimensionado para o painel de 3,5" (grid em PSRAM). Portes com tela menor —
   e sem PSRAM — reduzem pelo build; daí os guards. */
#ifndef TERM_MAX_LINES
#define TERM_MAX_LINES 48
#endif
#ifndef TERM_MAX_COLS
#define TERM_MAX_COLS  220
#endif
#ifndef TERM_MAX_RUNS
#define TERM_MAX_RUNS  1536   /* 48 linhas x 32 runs (amostra real: máx 31) */
#endif
#ifndef TERM_TEXT_CAP
#define TERM_TEXT_CAP  16384  /* texto limpo + 1 NUL por run */
#endif

#define TERM_F_BOLD      0x01
#define TERM_F_DIM       0x02
#define TERM_F_ITALIC    0x04
#define TERM_F_UNDERLINE 0x08
#define TERM_F_STRIKE    0x10
#define TERM_F_REVERSE   0x20
#define TERM_F_HAS_BG    0x40  /* bg do run difere do fundo default: pintar rect */

typedef struct {
    uint16_t text_off;  /* offset em grid->text (NUL-terminated ali) */
    uint16_t text_len;  /* bytes, sem o NUL */
    uint32_t fg;        /* RGB888 já resolvido (bold/dim/reverse aplicados) */
    uint32_t bg;        /* RGB888; válido quando TERM_F_HAS_BG */
    uint8_t  flags;
    uint8_t  cols;      /* colunas (codepoints) que o run ocupa */
} term_run_t;

typedef struct {
    uint16_t run_start;
    uint16_t run_count;
    uint16_t cols;
} term_line_t;

typedef struct {
    char        text[TERM_TEXT_CAP];
    term_run_t  runs[TERM_MAX_RUNS];
    term_line_t lines[TERM_MAX_LINES];
    uint16_t    line_count;
    uint16_t    max_cols;
    bool        overflow;   /* estourou linhas/colunas/runs/texto (cauda descartada) */
} term_grid_t;

typedef struct {
    uint32_t palette16[16]; /* 8 normais + 8 bright, RGB888 */
    uint32_t default_fg;
    uint32_t default_bg;
} term_palette_t;

/* Parseia o snapshot inteiro; devolve line_count. Estado SGR atravessa
 * linhas; escapes truncados no fim do buffer são descartados em silêncio. */
int term_parse(const char *ansi, term_grid_t *g, const term_palette_t *pal);

#ifdef __cplusplus
}
#endif

#endif
