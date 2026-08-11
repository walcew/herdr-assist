#include "term_parse.h"

#include <string.h>

/* Estado SGR corrente. fg/bg podem ser: default, indexada 0-15 (idx >= 0,
 * resolvida na emissão para bold->bright funcionar) ou RGB direta. */
typedef struct {
    bool     fg_default, bg_default;
    int16_t  fg_idx, bg_idx;   /* 0-15 quando indexada, -1 caso contrário */
    uint32_t fg, bg;           /* RGB888 quando !default && idx < 0 */
    uint8_t  attr;             /* TERM_F_BOLD..TERM_F_REVERSE */
} sgr_state_t;

typedef struct {
    term_grid_t          *g;
    const term_palette_t *pal;
    sgr_state_t           st;
    /* estilo resolvido do run aberto (e cache do estado corrente) */
    uint32_t res_fg, res_bg;
    uint8_t  res_flags;
    bool     res_dirty;
    /* run aberto */
    bool     run_open;
    uint16_t run_off, run_len;
    uint16_t run_cols;
    /* linha corrente */
    uint16_t line_run_start, line_run_count, line_cols;
    /* cursor de escrita em g->text */
    uint16_t text_pos;
    /* estouro: descarta texto até o fim da linha/do parse */
    bool     drop_line_text, text_full, lines_full;
    bool     emit_cont;    /* continuações UTF-8 seguem o destino do lead byte */
} ctx_t;

static uint32_t mix(uint32_t c, uint32_t o, unsigned pct_o)
{
    uint32_t r = ((c >> 16 & 0xFF) * (100 - pct_o) + (o >> 16 & 0xFF) * pct_o) / 100;
    uint32_t g = ((c >> 8 & 0xFF) * (100 - pct_o) + (o >> 8 & 0xFF) * pct_o) / 100;
    uint32_t b = ((c & 0xFF) * (100 - pct_o) + (o & 0xFF) * pct_o) / 100;
    return r << 16 | g << 8 | b;
}

static uint32_t color256(unsigned n, const term_palette_t *pal)
{
    if (n < 16) return pal->palette16[n];
    if (n < 232) {
        n -= 16;
        unsigned r = n / 36, g = (n / 6) % 6, b = n % 6;
        r = r ? 55 + r * 40 : 0;
        g = g ? 55 + g * 40 : 0;
        b = b ? 55 + b * 40 : 0;
        return r << 16 | g << 8 | b;
    }
    unsigned v = 8 + (n - 232) * 10;
    return v << 16 | v << 8 | v;
}

static void sgr_reset(sgr_state_t *st)
{
    st->fg_default = st->bg_default = true;
    st->fg_idx = st->bg_idx = -1;
    st->attr = 0;
}

static void resolve_style(ctx_t *c)
{
    const sgr_state_t *st = &c->st;
    const term_palette_t *pal = c->pal;
    uint32_t fg = st->fg_default ? pal->default_fg
                : st->fg_idx >= 0 ? pal->palette16[st->fg_idx]
                                  : st->fg;
    uint32_t bg = st->bg_default ? pal->default_bg
                : st->bg_idx >= 0 ? pal->palette16[st->bg_idx]
                                  : st->bg;
    if (st->attr & TERM_F_BOLD) {
        if (!st->fg_default && st->fg_idx >= 0 && st->fg_idx < 8)
            fg = pal->palette16[st->fg_idx + 8];
        else
            fg = mix(fg, 0xFFFFFF, 30);
    }
    if (st->attr & TERM_F_DIM) fg = mix(fg, 0x000000, 50);
    bool has_bg = !st->bg_default;
    if (st->attr & TERM_F_REVERSE) {
        uint32_t t = fg; fg = bg; bg = t;
        has_bg = true;
    }
    c->res_fg = fg;
    c->res_bg = bg;
    c->res_flags = st->attr | (has_bg ? TERM_F_HAS_BG : 0);
    c->res_dirty = false;
}

static void close_run(ctx_t *c)
{
    if (!c->run_open) return;
    c->run_open = false;
    if (c->run_len == 0) return;
    c->g->text[c->text_pos++] = '\0';
    term_run_t *r = &c->g->runs[c->line_run_start + c->line_run_count];
    r->text_off = c->run_off;
    r->text_len = c->run_len;
    r->fg = c->res_fg;
    r->bg = c->res_bg;
    r->flags = c->res_flags;
    r->cols = (uint8_t)c->run_cols;
    c->line_run_count++;
}

static void close_line(ctx_t *c)
{
    close_run(c);
    if (c->g->line_count < TERM_MAX_LINES) {
        term_line_t *l = &c->g->lines[c->g->line_count++];
        l->run_start = c->line_run_start;
        l->run_count = c->line_run_count;
        l->cols = c->line_cols;
        if (c->line_cols > c->g->max_cols) c->g->max_cols = c->line_cols;
    } else {
        c->lines_full = true;
        c->g->overflow = true;
    }
    c->line_run_start += c->line_run_count;
    c->line_run_count = 0;
    c->line_cols = 0;
    c->drop_line_text = false;
}

static void emit_byte(ctx_t *c, char b)
{
    bool lead = ((unsigned char)b & 0xC0) != 0x80;
    if (lead) {
        if (c->drop_line_text || c->text_full) { c->emit_cont = false; return; }
        if (c->line_cols >= TERM_MAX_COLS) {
            c->drop_line_text = true;
            c->g->overflow = true;
            c->emit_cont = false;
            return;
        }
        /* reserva: codepoint UTF-8 inteiro (até 4 B) + NUL — nunca corta no meio */
        if (c->text_pos + 5 > TERM_TEXT_CAP) {
            c->text_full = true;
            c->g->overflow = true;
            c->emit_cont = false;
            return;
        }
        c->emit_cont = true;
    } else if (!c->emit_cont) {
        return;
    }
    if (!c->run_open) {
        if (c->line_run_start + c->line_run_count >= TERM_MAX_RUNS) {
            /* runs cheios: reabre o último run da linha e concatena nele
             * (o run fecha depois com o estilo corrente — degradação aceita) */
            if (c->line_run_count > 0) {
                term_run_t *r = &c->g->runs[c->line_run_start + c->line_run_count - 1];
                c->line_run_count--;
                c->run_off = r->text_off;
                c->run_len = r->text_len;
                c->run_cols = r->cols;
                c->text_pos--;   /* sobrescreve o NUL antigo */
                c->run_open = true;
                c->g->overflow = true;
            } else {
                c->g->overflow = true;
                c->emit_cont = false;
                return;
            }
        } else {
            c->run_open = true;
            c->run_off = c->text_pos;
            c->run_len = 0;
            c->run_cols = 0;
        }
    }
    c->g->text[c->text_pos++] = b;
    c->run_len++;
    if (lead) {
        c->run_cols++;
        c->line_cols++;
    }
}

/* Fecha o run corrente se o estilo resolvido mudou (chamado no primeiro
 * texto após um SGR). Evita fragmentar em pares [0m[38;...m redundantes. */
static void sync_style(ctx_t *c)
{
    if (!c->res_dirty) return;
    uint32_t ofg = c->res_fg, obg = c->res_bg;
    uint8_t ofl = c->res_flags;
    bool was_open = c->run_open && c->run_len > 0;
    resolve_style(c);
    if (was_open && (ofg != c->res_fg || obg != c->res_bg || ofl != c->res_flags)) {
        /* restaura o estilo antigo para fechar o run com ele */
        uint32_t nfg = c->res_fg, nbg = c->res_bg;
        uint8_t nfl = c->res_flags;
        c->res_fg = ofg; c->res_bg = obg; c->res_flags = ofl;
        close_run(c);
        c->res_fg = nfg; c->res_bg = nbg; c->res_flags = nfl;
    }
}

static void apply_sgr(ctx_t *c, const uint16_t *p, const bool *colon, int np)
{
    sgr_state_t *st = &c->st;
    if (np == 0) { sgr_reset(st); c->res_dirty = true; return; }
    for (int i = 0; i < np; i++) {
        unsigned v = p[i];
        if (v == 38 || v == 48) {
            bool is_fg = (v == 38);
            int idx = -1;
            long rgb = -1;
            if (i + 1 < np && colon[i + 1]) {
                /* forma com ':' — sub-params consecutivos */
                int s = i + 1, e = s;
                while (e < np && colon[e]) e++;
                int nsub = e - s;
                if (nsub >= 2 && p[s] == 5) idx = p[s + 1] & 0xFF;
                else if (nsub >= 4 && p[s] == 2)
                    rgb = (long)(p[e - 3] & 0xFF) << 16 | (p[e - 2] & 0xFF) << 8 | (p[e - 1] & 0xFF);
                i = e - 1;
            } else if (i + 1 < np) {
                unsigned mode = p[i + 1];
                if (mode == 5 && i + 2 < np) { idx = p[i + 2] & 0xFF; i += 2; }
                else if (mode == 2 && i + 4 < np) {
                    rgb = (long)(p[i + 2] & 0xFF) << 16 | (p[i + 3] & 0xFF) << 8 | (p[i + 4] & 0xFF);
                    i += 4;
                } else i = np;   /* malformado: descarta o resto */
            } else i = np;
            if (idx >= 0) {
                if (is_fg) { st->fg_default = false; st->fg_idx = idx < 16 ? idx : -1;
                             if (idx >= 16) st->fg = color256(idx, c->pal); }
                else       { st->bg_default = false; st->bg_idx = idx < 16 ? idx : -1;
                             if (idx >= 16) st->bg = color256(idx, c->pal); }
            } else if (rgb >= 0) {
                if (is_fg) { st->fg_default = false; st->fg_idx = -1; st->fg = (uint32_t)rgb; }
                else       { st->bg_default = false; st->bg_idx = -1; st->bg = (uint32_t)rgb; }
            }
        }
        else if (v == 0) sgr_reset(st);
        else if (v == 1) st->attr |= TERM_F_BOLD;
        else if (v == 2) st->attr |= TERM_F_DIM;
        else if (v == 3) st->attr |= TERM_F_ITALIC;
        else if (v == 4) st->attr |= TERM_F_UNDERLINE;
        else if (v == 7) st->attr |= TERM_F_REVERSE;
        else if (v == 9) st->attr |= TERM_F_STRIKE;
        else if (v == 22) st->attr &= ~(TERM_F_BOLD | TERM_F_DIM);
        else if (v == 23) st->attr &= ~TERM_F_ITALIC;
        else if (v == 24) st->attr &= ~TERM_F_UNDERLINE;
        else if (v == 27) st->attr &= ~TERM_F_REVERSE;
        else if (v == 29) st->attr &= ~TERM_F_STRIKE;
        else if (v >= 30 && v <= 37) { st->fg_default = false; st->fg_idx = v - 30; }
        else if (v == 39) { st->fg_default = true; st->fg_idx = -1; }
        else if (v >= 40 && v <= 47) { st->bg_default = false; st->bg_idx = v - 40; }
        else if (v == 49) { st->bg_default = true; st->bg_idx = -1; }
        else if (v >= 90 && v <= 97) { st->fg_default = false; st->fg_idx = v - 90 + 8; }
        else if (v >= 100 && v <= 107) { st->bg_default = false; st->bg_idx = v - 100 + 8; }
        /* demais códigos: ignorados */
    }
    c->res_dirty = true;
}

/* Consome uma CSI a partir do byte após "ESC["; devolve o novo cursor.
 * Aplica apenas SGR (final 'm' sem marcador privado). */
static const char *parse_csi(ctx_t *c, const char *p)
{
    uint16_t params[16];
    bool colon[16];
    int np = 0;
    uint32_t cur = 0;
    bool have = false, priv = false, cur_colon = false;
    for (;;) {
        unsigned char b = (unsigned char)*p;
        if (b == 0) return p;                       /* truncado: descarta */
        if (b >= '0' && b <= '9') {
            cur = cur * 10 + (b - '0');
            if (cur > 65535) cur = 65535;
            have = true;
            p++;
        } else if (b == ';' || b == ':') {
            if (np < 16) { params[np] = (uint16_t)cur; colon[np] = cur_colon; np++; }
            cur = 0; have = false;
            cur_colon = (b == ':');
            p++;
        } else if (b >= 0x3C && b <= 0x3F) {        /* ? > = < */
            priv = true;
            p++;
        } else if (b >= 0x20 && b <= 0x2F) {        /* intermediários */
            p++;
        } else if (b >= 0x40 && b <= 0x7E) {        /* final */
            if (have || np > 0)
                if (np < 16) { params[np] = (uint16_t)cur; colon[np] = cur_colon; np++; }
            p++;
            if (b == 'm' && !priv) apply_sgr(c, params, colon, np);
            return p;
        } else {                                    /* byte inválido: aborta */
            return p + 1;
        }
    }
}

/* Consome OSC após "ESC]": até BEL ou ST (ESC \). */
static const char *parse_osc(const char *p)
{
    for (;;) {
        char b = *p;
        if (b == 0) return p;
        if (b == '\x07') return p + 1;
        if (b == '\x1b') {
            if (p[1] == '\\') return p + 2;
            return p + 1;   /* ESC inesperado encerra a OSC */
        }
        p++;
    }
}

int term_parse(const char *ansi, term_grid_t *g, const term_palette_t *pal)
{
    ctx_t c;
    memset(&c, 0, sizeof c);
    c.g = g;
    c.pal = pal;
    sgr_reset(&c.st);
    c.res_dirty = true;
    g->line_count = 0;
    g->max_cols = 0;
    g->overflow = false;

    const char *p = ansi;
    bool line_has_content = false;   /* houve texto/estado desde o último '\n'? */
    while (*p && !c.lines_full) {
        unsigned char b = (unsigned char)*p;
        if (b == 0x1B) {
            p++;
            unsigned char e = (unsigned char)*p;
            if (e == 0) break;                       /* ESC truncado no fim */
            if (e == '[') p = parse_csi(&c, p + 1);
            else if (e == ']') p = parse_osc(p + 1);
            else if (e >= 0x20 && e <= 0x2F) {       /* ESC + intermediários + final */
                p++;
                while ((unsigned char)*p >= 0x20 && (unsigned char)*p <= 0x2F) p++;
                if (*p) p++;
            } else p++;                              /* ESC + final único */
            continue;
        }
        p++;
        if (b == '\n') {
            close_line(&c);
            line_has_content = false;
            continue;
        }
        if (b == '\r') continue;
        if (b == '\t') b = ' ';
        else if (b < 0x20 || b == 0x7F) continue;    /* demais C0/DEL */
        sync_style(&c);
        emit_byte(&c, (char)b);
        line_has_content = true;
    }
    if (line_has_content || c.line_run_count > 0 || c.run_open) close_line(&c);
    return g->line_count;
}
