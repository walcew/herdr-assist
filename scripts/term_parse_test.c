// Teste host do term_parse (fora de src/ de propósito: o CMakeLists do
// firmware faz GLOB_RECURSE em src/ e um main() ali quebraria o build).
//
//   cc -std=c11 -Wall -Wextra -Isrc -o /tmp/tpt \
//      scripts/term_parse_test.c src/term_parse.c
//   /tmp/tpt [scripts/fixtures/ansi_sample.txt]
//
// A fixture é opcional (sem ela rodam só os casos sintéticos) e fica fora do
// git, por ser saída real de sessões. Para regerar:
//   herdr pane read <pane_id> --format ansi --lines 40 > scripts/fixtures/ansi_sample.txt
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "term_parse.h"

#define CHECK(cond) do { \
    if (!(cond)) { fprintf(stderr, "FALHOU linha %d: %s\n", __LINE__, #cond); exit(1); } \
} while (0)

static const term_palette_t PAL = {
    .palette16 = {
        0x21222c, 0xff5555, 0x50fa7b, 0xf1fa8c, 0x6272a4, 0xff79c6, 0x8be9fd, 0xf8f8f2,
        0x44475a, 0xff6e6e, 0x69ff94, 0xffffa5, 0xd6acff, 0xff92df, 0xa4ffff, 0xffffff,
    },
    .default_fg = 0xaebfa0,
    .default_bg = 0x0a0a0b,
};

static term_grid_t g;

static const term_run_t *run(int line, int i)
{
    CHECK(line < g.line_count && i < g.lines[line].run_count);
    return &g.runs[g.lines[line].run_start + i];
}

static const char *run_text(const term_run_t *r) { return &g.text[r->text_off]; }

static void synthetics(void)
{
    /* texto puro */
    CHECK(term_parse("plain", &g, &PAL) == 1);
    CHECK(g.lines[0].run_count == 1 && g.lines[0].cols == 5);
    CHECK(run(0, 0)->fg == PAL.default_fg && !(run(0, 0)->flags & TERM_F_HAS_BG));
    CHECK(strcmp(run_text(run(0, 0)), "plain") == 0);

    /* cores indexadas + reset */
    term_parse("a\x1b[31mb\x1b[0mc", &g, &PAL);
    CHECK(g.lines[0].run_count == 3);
    CHECK(run(0, 0)->fg == PAL.default_fg);
    CHECK(run(0, 1)->fg == PAL.palette16[1]);
    CHECK(run(0, 2)->fg == PAL.default_fg);

    /* bold em indexada -> bright */
    term_parse("\x1b[1;31mX", &g, &PAL);
    CHECK(run(0, 0)->fg == PAL.palette16[9] && (run(0, 0)->flags & TERM_F_BOLD));

    /* 256: cubo, cinza e 0-15 */
    term_parse("\x1b[38;5;196mX", &g, &PAL);
    CHECK(run(0, 0)->fg == 0xff0000);
    term_parse("\x1b[38;5;244mX", &g, &PAL);
    CHECK(run(0, 0)->fg == 0x808080);
    term_parse("\x1b[38;5;2mX", &g, &PAL);
    CHECK(run(0, 0)->fg == PAL.palette16[2]);

    /* truecolor ';' e ':' (com e sem colorspace) */
    term_parse("\x1b[38;2;1;2;3mX", &g, &PAL);
    CHECK(run(0, 0)->fg == 0x010203);
    term_parse("\x1b[38:5:2mX", &g, &PAL);
    CHECK(run(0, 0)->fg == PAL.palette16[2]);
    term_parse("\x1b[38:2:10:20:30mX", &g, &PAL);
    CHECK(run(0, 0)->fg == 0x0a141e);
    term_parse("\x1b[38:2::10:20:30mX", &g, &PAL);
    CHECK(run(0, 0)->fg == 0x0a141e);

    /* background + reverse + dim */
    term_parse("\x1b[48;2;5;6;7mX", &g, &PAL);
    CHECK(run(0, 0)->bg == 0x050607 && (run(0, 0)->flags & TERM_F_HAS_BG));
    term_parse("\x1b[7mX", &g, &PAL);
    CHECK(run(0, 0)->fg == PAL.default_bg && run(0, 0)->bg == PAL.default_fg);
    CHECK(run(0, 0)->flags & TERM_F_HAS_BG);
    term_parse("\x1b[2mX", &g, &PAL);
    CHECK(run(0, 0)->fg == 0x575f50);   /* default_fg / 2 por canal */

    /* underline/strike viram flags */
    term_parse("\x1b[4;9mX", &g, &PAL);
    CHECK((run(0, 0)->flags & TERM_F_UNDERLINE) && (run(0, 0)->flags & TERM_F_STRIKE));

    /* sequências que não são SGR: consumidas em silêncio */
    term_parse("\x1b]0;titulo\x07X", &g, &PAL);
    CHECK(g.lines[0].run_count == 1 && strcmp(run_text(run(0, 0)), "X") == 0);
    term_parse("\x1b[2JX", &g, &PAL);
    CHECK(run(0, 0)->fg == PAL.default_fg && strcmp(run_text(run(0, 0)), "X") == 0);
    term_parse("\x1b[?25lX", &g, &PAL);
    CHECK(strcmp(run_text(run(0, 0)), "X") == 0);

    /* escape truncado no fim: descarte silencioso */
    CHECK(term_parse("X\x1b[38;2;1", &g, &PAL) == 1);
    CHECK(strcmp(run_text(run(0, 0)), "X") == 0);
    CHECK(term_parse("X\x1b", &g, &PAL) == 1);

    /* linhas: \r\n, vazia no meio, \n final */
    CHECK(term_parse("a\r\nb", &g, &PAL) == 2);
    CHECK(term_parse("a\n\nb", &g, &PAL) == 3);
    CHECK(g.lines[1].run_count == 0);
    CHECK(term_parse("a\n", &g, &PAL) == 1);

    /* estado SGR atravessa linha */
    term_parse("\x1b[31ma\nb", &g, &PAL);
    CHECK(run(1, 0)->fg == PAL.palette16[1]);

    /* reset redundante nao fragmenta run */
    term_parse("\x1b[31ma\x1b[0m\x1b[31mb", &g, &PAL);
    CHECK(g.lines[0].run_count == 1 && strcmp(run_text(run(0, 0)), "ab") == 0);

    /* UTF-8 multibyte conta 1 coluna por codepoint */
    term_parse("\xe2\x94\x80\xe2\x94\x82", &g, &PAL);   /* ─│ */
    CHECK(g.lines[0].cols == 2 && run(0, 0)->text_len == 6);

    /* cap de colunas por linha */
    {
        char long_line[300];
        memset(long_line, 'x', 299);
        long_line[299] = 0;
        term_parse(long_line, &g, &PAL);
        CHECK(g.lines[0].cols == TERM_MAX_COLS && g.overflow);
    }
}

static void fixture(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "fixture nao encontrada: %s\n", path); exit(1); }
    static char buf[65536];
    size_t n = fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    buf[n] = 0;
    CHECK(n > 10000);

    term_parse(buf, &g, &PAL);
    /* a amostra tem ~160 linhas (4 panes): estoura o cap e trunca */
    CHECK(g.line_count == TERM_MAX_LINES && g.overflow);
    CHECK(g.max_cols > 40 && g.max_cols <= TERM_MAX_COLS);

    int runs_total = 0;
    bool saw_truecolor = false;
    for (int l = 0; l < g.line_count; l++) {
        for (int i = 0; i < g.lines[l].run_count; i++) {
            const term_run_t *r = run(l, i);
            CHECK(g.text[r->text_off + r->text_len] == '\0');
            CHECK(memchr(run_text(r), 0x1b, r->text_len) == NULL);
            if (r->fg == 0xb1b9f9) saw_truecolor = true;   /* 38;2;177;185;249 da amostra */
            runs_total++;
        }
    }
    CHECK(saw_truecolor);
    printf("fixture ok: %d linhas, %d runs, max_cols=%d\n",
           g.line_count, runs_total, g.max_cols);
}

int main(int argc, char **argv)
{
    synthetics();
    if (argc > 1) fixture(argv[1]);
    printf("term_parse: todos os testes passaram\n");
    return 0;
}
