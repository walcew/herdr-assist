#include "ui_term.h"

#include <stdlib.h>
#include <string.h>

#include <M5Cardputer.h>

#include "term_parse.h"
#include "theme.h"

/* Mesma base Dracula do painel (ui_theme.c): só vale para SGR indexado —
   Claude Code e afins emitem truecolor, que passa direto. */
static const term_palette_t PALETTE = {
    {
        0x21222c, 0xff5555, 0x50fa7b, 0xf1fa8c, 0x6272a4, 0xff79c6, 0x8be9fd, 0xf8f8f2,
        0x555866, 0xff6e6e, 0x69ff94, 0xffffa5, 0x9db2ff, 0xff92df, 0xa4ffff, 0xffffff,
    },
    C_TERM_FG_888,
    C_TERM_BG_888,
};

static term_grid_t *s_grid;
/* sem pai no construtor (ordem de init dos globais); o destino vai no push */
static M5Canvas     s_row;
static int          s_cw = 6, s_chh = 8;
static bool         s_tiny;
static int          s_scroll;          /* linhas acima do fim */
static char         s_msg[48];

/* ---------- redução a ASCII ---------- */

/**
 * Um codepoint -> um caractere ASCII.
 *
 * A fonte de 6x8 do M5GFX é ASCII puro, e o que sobra do terminal é sempre a
 * mesma família: moldura, marcadores, setas e acento. Trocar por vizinho visual
 * mantém o desenho legível; o que importa é nunca mudar a contagem de colunas,
 * porque o grid do host foi montado numa largura fixa.
 */
static char fold_cp(uint32_t cp)
{
    if (cp < 0x80) {
        return (char)cp;
    }
    /* latin-1 acentuado: vira a letra base (a saída dos agentes é bilíngue) */
    static const char *LAT1 =
        "AAAAAAACEEEEIIII" "DNOOOOOxOUUUUYPs"
        "aaaaaaaceeeeiiii" "dnooooo/ouuuuypy";
    if (cp >= 0xC0 && cp <= 0xFF) {
        return LAT1[cp - 0xC0];
    }
    switch (cp) {
    case 0x00A0: return ' ';   /* nbsp */
    case 0x2018: case 0x2019: return '\'';
    case 0x201C: case 0x201D: return '"';
    case 0x2013: case 0x2014: return '-';
    case 0x2026: return '~';   /* … em uma coluna só */
    case 0x2022: case 0x00B7: return '.';
    case 0x2190: return '<';
    case 0x2191: return '^';
    case 0x2192: return '>';
    case 0x2193: return 'v';
    case 0x2713: case 0x2714: return 'v';   /* ✓ */
    case 0x2717: case 0x2718: case 0x2715: return 'x';
    case 0x00B0: return 'o';
    case 0x00B1: return '+';
    case 0x00BB: return '>';
    case 0x00AB: return '<';
    default: break;
    }
    /* box-drawing: horizontais viram '-', verticais '|', o resto '+' */
    if (cp >= 0x2500 && cp <= 0x257F) {
        switch (cp) {
        case 0x2500: case 0x2501: case 0x2504: case 0x2505:
        case 0x2508: case 0x2509: case 0x254C: case 0x254D:
            return '-';
        case 0x2502: case 0x2503: case 0x2506: case 0x2507:
        case 0x250A: case 0x250B: case 0x254E: case 0x254F:
            return '|';
        default:
            return '+';
        }
    }
    if (cp >= 0x2580 && cp <= 0x259F) {
        return '#';            /* blocos: barra de progresso e afins */
    }
    if (cp >= 0x25A0 && cp <= 0x25FF) {
        return '*';            /* formas geométricas: ●, ▶, ■ */
    }
    if (cp >= 0x2600 && cp <= 0x27BF) {
        return '*';            /* dingbats: ✶ do spinner, ✳, ✽ */
    }
    return '?';
}

/** Reduz a string UTF-8 a ASCII in-place (só encolhe: os escapes SGR são ASCII). */
static void fold_ascii(char *s)
{
    unsigned char *w = (unsigned char *)s;
    const unsigned char *r = (const unsigned char *)s;
    while (*r) {
        if (*r < 0x80) {
            *w++ = *r++;
            continue;
        }
        uint32_t cp;
        int len;
        if ((*r & 0xE0) == 0xC0)      { cp = *r & 0x1F; len = 2; }
        else if ((*r & 0xF0) == 0xE0) { cp = *r & 0x0F; len = 3; }
        else if ((*r & 0xF8) == 0xF0) { cp = *r & 0x07; len = 4; }
        else                          { r++; continue; }  /* continuação órfã */
        int i;
        for (i = 1; i < len && (r[i] & 0xC0) == 0x80; i++) {
            cp = (cp << 6) | (r[i] & 0x3F);
        }
        if (i < len) {
            r++;               /* sequência truncada: descarta o líder */
            continue;
        }
        r += len;
        *w++ = (unsigned char)fold_cp(cp);
    }
    *w = '\0';
}

/* ---------- ciclo de vida ---------- */

bool term_alloc(bool tiny)
{
    s_tiny = tiny;
    s_cw  = tiny ? 4 : 6;
    s_chh = tiny ? 6 : 8;
    s_scroll = 0;
    if (!s_grid) {
        s_grid = (term_grid_t *)malloc(sizeof(term_grid_t));
    }
    if (s_grid) {
        memset(s_grid, 0, sizeof(*s_grid));
    }
    s_row.setColorDepth(16);
    s_row.setTextWrap(false);
    if (!s_row.createSprite(SCR_W, s_chh)) {
        return false;
    }
    s_row.setFont(tiny ? (const lgfx::IFont *)&fonts::TomThumb
                       : (const lgfx::IFont *)&fonts::Font0);
    return s_grid != NULL;
}

void term_free(void)
{
    s_row.deleteSprite();
    free(s_grid);
    s_grid = NULL;
    s_msg[0] = '\0';
}

int term_cols(void)      { return SCR_W / s_cw; }
int term_cell_h(void)    { return s_chh; }
int term_rows_for(int h) { return h / s_chh; }
int term_line_count(void) { return s_grid ? s_grid->line_count : 0; }

void term_set_content(char *ansi)
{
    if (!s_grid) {
        return;
    }
    s_msg[0] = '\0';
    fold_ascii(ansi);
    term_parse(ansi, s_grid, &PALETTE);
    /* quem está no fim continua no fim (é o que um terminal faz); quem subiu
       para ler algo não é arrastado de volta pelo refresh de 3s */
    if (s_scroll > s_grid->line_count) {
        s_scroll = s_grid->line_count;
    }
}

void term_set_message(const char *msg)
{
    if (s_grid) {
        s_grid->line_count = 0;
    }
    s_scroll = 0;
    strlcpy(s_msg, msg, sizeof(s_msg));
}

bool term_scroll(int lines, int rows_visible)
{
    /* o teto é o que sobra além da tela, não o total: com line_count linhas e
       rows_visible cabendo, rolar mais que a diferença só empurraria o texto
       para fora e mostraria linhas em branco */
    int total = s_grid ? s_grid->line_count : 0;
    int max_scroll = total > rows_visible ? total - rows_visible : 0;
    int want = s_scroll + lines;
    if (want < 0) {
        want = 0;
    }
    if (want > max_scroll) {
        want = max_scroll;
    }
    if (want == s_scroll) {
        return false;
    }
    s_scroll = want;
    return true;
}

bool term_at_bottom(void) { return s_scroll == 0; }

/* ---------- desenho ---------- */

void term_draw(int y, int rows)
{
    auto &d = M5Cardputer.Display;
    const uint16_t bg = rgb565(PALETTE.default_bg);

    if (!s_grid || s_grid->line_count == 0) {
        d.fillRect(0, y, SCR_W, rows * s_chh, bg);
        d.setFont(&fonts::Font0);
        d.setTextColor(C_MUTED, bg);
        d.setTextDatum(middle_center);
        d.drawString(s_msg[0] ? s_msg : "sem conteudo", SCR_W / 2, y + rows * s_chh / 2);
        d.setTextDatum(top_left);
        return;
    }

    /* a última linha recebida é a de baixo da tela; s_scroll anda para trás */
    int last  = s_grid->line_count - s_scroll;
    int first = last - rows;
    if (first < 0) {
        first = 0;
    }

    const int cols = term_cols();
    for (int i = 0; i < rows; i++) {
        int li = first + i;
        s_row.fillSprite(bg);
        if (li >= 0 && li < last) {
            const term_line_t *line = &s_grid->lines[li];
            int col = 0;
            for (int r = 0; r < line->run_count && col < cols; r++) {
                const term_run_t *run = &s_grid->runs[line->run_start + r];
                if (run->flags & TERM_F_HAS_BG) {
                    s_row.fillRect(col * s_cw, 0, run->cols * s_cw, s_chh, rgb565(run->bg));
                }
                s_row.setTextColor(rgb565(run->fg));
                const char *txt = s_grid->text + run->text_off;
                /* depois do fold cada byte é uma coluna: posiciona célula a
                   célula para o desenho bater com o grid do host */
                for (int k = 0; k < run->text_len && col + k < cols; k++) {
                    char c = txt[k];
                    if (c > ' ') {
                        s_row.setCursor((col + k) * s_cw, 0);
                        s_row.print(c);
                    }
                }
                if (run->flags & TERM_F_UNDERLINE) {
                    s_row.drawFastHLine(col * s_cw, s_chh - 1, run->cols * s_cw,
                                        rgb565(run->fg));
                }
                col += run->cols;
            }
        }
        s_row.pushSprite(&M5Cardputer.Display, 0, y + i * s_chh);
    }
}
