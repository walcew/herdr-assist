#include "term_view.h"

#include <stdlib.h>

#include <esp_heap_caps.h>
#include <esp_log.h>

#include "term_parse.h"
#include "ui_theme.h"

static const char *TAG = "term_view";

/* Célula da lv_font_terminal_12: adv_w 115/16 arredonda para 7 px na LVGL
   (lv_font_fmt_txt.c), line_height 19. */
#define TERM_CELL_W 7
#define TERM_CELL_H 19
#define TERM_PAD    2

typedef struct {
    term_grid_t *grid;
    lv_obj_t    *cont;   /* container scrollável (pai) */
    lv_obj_t    *tv;
    void       (*scroll_cb)(int lines, int col, int row);
    lv_coord_t   drag_acc;   /* pixels de arraste ainda não convertidos em linha */
} term_view_ctx_t;

static void draw_cb(lv_event_t *e)
{
    lv_obj_t *tv = lv_event_get_target(e);
    term_view_ctx_t *ctx = lv_event_get_user_data(e);
    lv_draw_ctx_t *dc = lv_event_get_draw_ctx(e);
    const term_grid_t *g = ctx->grid;
    if (g->line_count == 0) return;

    lv_area_t obj_area;
    lv_obj_get_coords(tv, &obj_area);
    const lv_area_t *clip = dc->clip_area;
    lv_coord_t ox = obj_area.x1 + TERM_PAD;
    lv_coord_t oy = obj_area.y1 + TERM_PAD;

    int first = (clip->y1 - oy) / TERM_CELL_H;
    int last = (clip->y2 - oy) / TERM_CELL_H;
    if (first < 0) first = 0;
    if (last >= g->line_count) last = g->line_count - 1;

    lv_draw_rect_dsc_t rd;
    lv_draw_rect_dsc_init(&rd);
    rd.radius = 0;
    rd.bg_opa = LV_OPA_COVER;

    lv_draw_label_dsc_t ld;
    lv_draw_label_dsc_init(&ld);
    ld.font = &lv_font_terminal_12;
    ld.flag = LV_TEXT_FLAG_EXPAND;   /* sem re-wrap: o clip corta o excedente */

    for (int l = first; l <= last; l++) {
        const term_line_t *ln = &g->lines[l];
        lv_coord_t y = oy + (lv_coord_t)l * TERM_CELL_H;
        /* fundos primeiro, texto depois (linhas não se sobrepõem) */
        lv_coord_t x = ox;
        for (int i = 0; i < ln->run_count; i++) {
            const term_run_t *r = &g->runs[ln->run_start + i];
            lv_coord_t w = (lv_coord_t)r->cols * TERM_CELL_W;
            if ((r->flags & TERM_F_HAS_BG) && x <= clip->x2 && x + w - 1 >= clip->x1) {
                lv_area_t a = { x, y, x + w - 1, y + TERM_CELL_H - 1 };
                rd.bg_color = lv_color_hex(r->bg);
                lv_draw_rect(dc, &rd, &a);
            }
            x += w;
        }
        x = ox;
        for (int i = 0; i < ln->run_count; i++) {
            const term_run_t *r = &g->runs[ln->run_start + i];
            lv_coord_t w = (lv_coord_t)r->cols * TERM_CELL_W;
            if (r->text_len > 0 && x <= clip->x2 && x + w - 1 >= clip->x1) {
                lv_area_t a = { x, y, x + w - 1, y + TERM_CELL_H - 1 };
                ld.color = lv_color_hex(r->fg);
                ld.decor = ((r->flags & TERM_F_UNDERLINE) ? LV_TEXT_DECOR_UNDERLINE : 0) |
                           ((r->flags & TERM_F_STRIKE) ? LV_TEXT_DECOR_STRIKETHROUGH : 0);
                lv_draw_label(dc, &ld, &a, &g->text[r->text_off], NULL);
            }
            x += w;
        }
    }
}

static void delete_cb(lv_event_t *e)
{
    term_view_ctx_t *ctx = lv_event_get_user_data(e);
    free(ctx->grid);
    free(ctx);
}

/* Arraste vertical no container: vira rolagem do terminal no host quando não
   há o que rolar localmente (o normal, já que a sessão roda no tamanho da
   tela). Uma célula arrastada = uma linha, como numa roda de mouse. */
static void drag_cb(lv_event_t *e)
{
    term_view_ctx_t *ctx = lv_event_get_user_data(e);
    if (lv_event_get_code(e) == LV_EVENT_PRESSED) {
        ctx->drag_acc = 0;
        return;
    }
    if (!ctx->scroll_cb || (lv_obj_get_scroll_dir(ctx->cont) & LV_DIR_VER)) {
        return;
    }
    lv_indev_t *indev = lv_indev_get_act();
    if (!indev) return;
    lv_point_t v;
    lv_indev_get_vect(indev, &v);
    ctx->drag_acc += v.y;
    int lines = ctx->drag_acc / TERM_CELL_H;
    if (lines == 0) return;
    ctx->drag_acc -= (lv_coord_t)lines * TERM_CELL_H;

    /* célula sob o dedo, do jeito que o host a numera */
    lv_point_t p;
    lv_indev_get_point(indev, &p);
    lv_area_t a;
    lv_obj_get_coords(ctx->tv, &a);
    int col = (p.x - (a.x1 + TERM_PAD)) / TERM_CELL_W;
    int row = (p.y - (a.y1 + TERM_PAD)) / TERM_CELL_H;
    if (col < 0) col = 0;
    if (row < 0) row = 0;
    if (ctx->grid->max_cols && col >= ctx->grid->max_cols) col = ctx->grid->max_cols - 1;
    if (ctx->grid->line_count && row >= ctx->grid->line_count) row = ctx->grid->line_count - 1;
    ctx->scroll_cb(lines, col, row);
}

lv_obj_t *term_view_create(lv_obj_t *scroll_parent)
{
    term_view_ctx_t *ctx = calloc(1, sizeof(*ctx));
    if (!ctx) return NULL;
    /* grande e sem pressa: PSRAM, poupando a SRAM interna */
    ctx->grid = heap_caps_malloc(sizeof(term_grid_t), MALLOC_CAP_SPIRAM);
    if (!ctx->grid) {
        ctx->grid = malloc(sizeof(term_grid_t));
    }
    if (!ctx->grid) {
        ESP_LOGE(TAG, "sem memória para o grid");
        free(ctx);
        return NULL;
    }
    ctx->grid->line_count = 0;
    ctx->grid->max_cols = 0;
    ctx->grid->overflow = false;
    ctx->cont = scroll_parent;

    lv_obj_t *tv = lv_obj_create(scroll_parent);
    lv_obj_remove_style_all(tv);   /* obj cru: sem fundo/borda/pad próprios */
    lv_obj_clear_flag(tv, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(tv, 0, 0);
    lv_obj_set_size(tv, TERM_CELL_W, TERM_CELL_H);
    lv_obj_set_user_data(tv, ctx);
    lv_obj_add_event_cb(tv, draw_cb, LV_EVENT_DRAW_MAIN, ctx);
    lv_obj_add_event_cb(tv, delete_cb, LV_EVENT_DELETE, ctx);
    lv_obj_add_event_cb(scroll_parent, drag_cb, LV_EVENT_PRESSED, ctx);
    lv_obj_add_event_cb(scroll_parent, drag_cb, LV_EVENT_PRESSING, ctx);
    ctx->tv = tv;
    return tv;
}

void term_view_set_scroll_cb(lv_obj_t *tv, void (*cb)(int lines, int col, int row))
{
    term_view_ctx_t *ctx = lv_obj_get_user_data(tv);
    ctx->scroll_cb = cb;
}

void term_view_set_ansi(lv_obj_t *tv, const char *ansi)
{
    term_view_ctx_t *ctx = lv_obj_get_user_data(tv);
    lv_coord_t sx = lv_obj_get_scroll_x(ctx->cont);

    int lines = term_parse(ansi, ctx->grid, &ui_term_palette);
    if (ctx->grid->overflow) {
        ESP_LOGW(TAG, "snapshot excedeu caps (%d linhas, %d cols)",
                 lines, ctx->grid->max_cols);
    }
    lv_coord_t w = (lv_coord_t)ctx->grid->max_cols * TERM_CELL_W + 2 * TERM_PAD;
    lv_coord_t h = (lv_coord_t)lines * TERM_CELL_H + 2 * TERM_PAD;
    if (w < TERM_CELL_W) w = TERM_CELL_W;
    if (h < TERM_CELL_H) h = TERM_CELL_H;
    lv_obj_set_size(tv, w, h);
    lv_obj_invalidate(tv);   /* set_size sozinho não invalida quando nada muda */

    /* Conteúdo que cabe libera o eixo vertical para a rolagem nativa (drag_cb);
       sobrando linhas — trava de resolução que não pegou — o LVGL rola aqui. */
    lv_obj_set_scroll_dir(ctx->cont,
                          h > lv_obj_get_content_height(ctx->cont) ? LV_DIR_ALL : LV_DIR_HOR);

    /* âncora no fim (vertical) preservando o pan do usuário (horizontal) */
    lv_obj_update_layout(ctx->cont);
    lv_obj_scroll_to_y(ctx->cont, LV_COORD_MAX, LV_ANIM_OFF);
    lv_obj_scroll_to_x(ctx->cont, sx, LV_ANIM_OFF);
}

void term_view_clear(lv_obj_t *tv, const char *placeholder)
{
    term_view_ctx_t *ctx = lv_obj_get_user_data(tv);
    term_view_set_ansi(tv, placeholder ? placeholder : "");
    lv_obj_scroll_to(ctx->cont, 0, 0, LV_ANIM_OFF);
}

void term_view_fit(lv_obj_t *tv, int *cols, int *rows)
{
    term_view_ctx_t *ctx = lv_obj_get_user_data(tv);
    *cols = (lv_obj_get_content_width(ctx->cont) - 2 * TERM_PAD) / TERM_CELL_W;
    *rows = (lv_obj_get_content_height(ctx->cont) - 2 * TERM_PAD) / TERM_CELL_H;
    if (*cols < 1) *cols = 1;
    if (*rows < 1) *rows = 1;
}
