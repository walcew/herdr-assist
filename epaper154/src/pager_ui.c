/* Implementação da tela de 200x200 — ver pager_ui.h. */

/* localtime_r sob -std=c11 no host (glibc); inócuo no newlib do IDF */
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include "pager_ui.h"

#include <stdio.h>
#include <string.h>

#include "epd_bsp.h"
#include "herdr_model.h"

/* ---------------------------------------------------------------- geometria */
#define TOP_H      20     /* 0..19   hora (2x) + estado */
#define ALERT_Y    21     /* 21..77  banda de alerta */
#define ALERT_H    57
#define LIST_Y     80     /* 80..175 seis linhas de 16 px */
#define ROW_H      16
#define ROWS_MAX   6
#define FOOT_Y     177    /* 177..199 legenda + estado */

/* ---------------------------------------------------------------- estado */
typedef struct {
    int  host;
    char pane_id[HERDR_ID_LEN];
    char project[HERDR_NAME_LEN];
    char agent[HERDR_NAME_LEN];
    char status[HERDR_STATUS_LEN];
    uint32_t since;
} row_t;

static ui_screen_t s_screen = UI_MAIN;
static char s_status[32] = "starting";
static bool s_wifi_up, s_muted, s_update;
static int  s_batt_pct = -1;            /* -1 = sem bateria: não mostra */
static char s_setup_ssid[33], s_setup_pass[24], s_setup_url[32];
static char s_pair_id[16];
static int  s_pair_secs;
static bool s_pair_wifi;
static char s_msg_title[20], s_msg_l1[40], s_msg_l2[40];

static row_t s_rows[HERDR_MAX_AGENTS_TOTAL];
static int   s_row_count;
static int   s_waiting;
static int   s_focus = -1;               /* índice em s_rows; -1 = nenhum */
static char  s_focus_pane[HERDR_ID_LEN];
static int   s_focus_host = -1;
static uint8_t s_last_frame[EPD_FB_BYTES];
static bool  s_have_last;

/* ---------------------------------------------------------------- fonte 5x7
 * ASCII 32..126, 5 colunas por glifo, LSB = linha de cima (a tabela 5x7
 * clássica de domínio público), com uma 8ª linha para as descendentes de
 * g j p q y — sem ela, "g" vira "9" no vidro. Avanço de 6 px; 2x/3x nos
 * títulos. */
static const uint8_t FONT[95][5] = {
    {0x00,0x00,0x00,0x00,0x00},{0x00,0x00,0x5F,0x00,0x00},{0x00,0x07,0x00,0x07,0x00},
    {0x14,0x7F,0x14,0x7F,0x14},{0x24,0x2A,0x7F,0x2A,0x12},{0x23,0x13,0x08,0x64,0x62},
    {0x36,0x49,0x55,0x22,0x50},{0x00,0x05,0x03,0x00,0x00},{0x00,0x1C,0x22,0x41,0x00},
    {0x00,0x41,0x22,0x1C,0x00},{0x08,0x2A,0x1C,0x2A,0x08},{0x08,0x08,0x3E,0x08,0x08},
    {0x00,0x50,0x30,0x00,0x00},{0x08,0x08,0x08,0x08,0x08},{0x00,0x60,0x60,0x00,0x00},
    {0x20,0x10,0x08,0x04,0x02},{0x3E,0x51,0x49,0x45,0x3E},{0x00,0x42,0x7F,0x40,0x00},
    {0x42,0x61,0x51,0x49,0x46},{0x21,0x41,0x45,0x4B,0x31},{0x18,0x14,0x12,0x7F,0x10},
    {0x27,0x45,0x45,0x45,0x39},{0x3C,0x4A,0x49,0x49,0x30},{0x01,0x71,0x09,0x05,0x03},
    {0x36,0x49,0x49,0x49,0x36},{0x06,0x49,0x49,0x29,0x1E},{0x00,0x36,0x36,0x00,0x00},
    {0x00,0x56,0x36,0x00,0x00},{0x08,0x14,0x22,0x41,0x00},{0x14,0x14,0x14,0x14,0x14},
    {0x00,0x41,0x22,0x14,0x08},{0x02,0x01,0x51,0x09,0x06},{0x32,0x49,0x79,0x41,0x3E},
    {0x7E,0x11,0x11,0x11,0x7E},{0x7F,0x49,0x49,0x49,0x36},{0x3E,0x41,0x41,0x41,0x22},
    {0x7F,0x41,0x41,0x22,0x1C},{0x7F,0x49,0x49,0x49,0x41},{0x7F,0x09,0x09,0x09,0x01},
    {0x3E,0x41,0x49,0x49,0x7A},{0x7F,0x08,0x08,0x08,0x7F},{0x00,0x41,0x7F,0x41,0x00},
    {0x20,0x40,0x41,0x3F,0x01},{0x7F,0x08,0x14,0x22,0x41},{0x7F,0x40,0x40,0x40,0x40},
    {0x7F,0x02,0x0C,0x02,0x7F},{0x7F,0x04,0x08,0x10,0x7F},{0x3E,0x41,0x41,0x41,0x3E},
    {0x7F,0x09,0x09,0x09,0x06},{0x3E,0x41,0x51,0x21,0x5E},{0x7F,0x09,0x19,0x29,0x46},
    {0x46,0x49,0x49,0x49,0x31},{0x01,0x01,0x7F,0x01,0x01},{0x3F,0x40,0x40,0x40,0x3F},
    {0x1F,0x20,0x40,0x20,0x1F},{0x3F,0x40,0x38,0x40,0x3F},{0x63,0x14,0x08,0x14,0x63},
    {0x07,0x08,0x70,0x08,0x07},{0x61,0x51,0x49,0x45,0x43},{0x00,0x7F,0x41,0x41,0x00},
    {0x02,0x04,0x08,0x10,0x20},{0x00,0x41,0x41,0x7F,0x00},{0x04,0x02,0x01,0x02,0x04},
    {0x40,0x40,0x40,0x40,0x40},{0x00,0x01,0x02,0x04,0x00},{0x20,0x54,0x54,0x54,0x78},
    {0x7F,0x48,0x44,0x44,0x38},{0x38,0x44,0x44,0x44,0x20},{0x38,0x44,0x44,0x48,0x7F},
    {0x38,0x54,0x54,0x54,0x18},{0x08,0x7E,0x09,0x01,0x02},{0x18,0xA4,0xA4,0xA4,0x7C},
    {0x7F,0x08,0x04,0x04,0x78},{0x00,0x44,0x7D,0x40,0x00},{0x40,0x80,0x84,0x7D,0x00},
    {0x7F,0x10,0x28,0x44,0x00},{0x00,0x41,0x7F,0x40,0x00},{0x7C,0x04,0x18,0x04,0x78},
    {0x7C,0x08,0x04,0x04,0x78},{0x38,0x44,0x44,0x44,0x38},{0xFC,0x24,0x24,0x24,0x18},
    {0x18,0x24,0x24,0x18,0xFC},{0x7C,0x08,0x04,0x04,0x08},{0x48,0x54,0x54,0x54,0x20},
    {0x04,0x3F,0x44,0x40,0x20},{0x3C,0x40,0x40,0x20,0x7C},{0x1C,0x20,0x40,0x20,0x1C},
    {0x3C,0x40,0x30,0x40,0x3C},{0x44,0x28,0x10,0x28,0x44},{0x1C,0xA0,0xA0,0xA0,0x7C},
    {0x44,0x64,0x54,0x4C,0x44},{0x00,0x08,0x36,0x41,0x00},{0x00,0x00,0x7F,0x00,0x00},
    {0x00,0x41,0x36,0x08,0x00},{0x08,0x04,0x08,0x10,0x08},
};

static bool s_ink = true;    /* cor do texto: preto, ou branco sobre a banda */

static void draw_char(int x, int y, unsigned char c, int scale)
{
    if (c < 32 || c > 126) c = '?';
    const uint8_t *g = FONT[c - 32];
    for (int col = 0; col < 5; col++)
        for (int row = 0; row < 8; row++)
            if (g[col] & (1u << row))
                epd_fb_rect(x + col * scale, y + row * scale, scale, scale, s_ink);
}

/* Desenha até max_chars glifos; UTF-8 multibyte vira um '?' por caractere.
 * Devolve a largura desenhada em px. */
static int draw_text(int x, int y, const char *s, int max_chars, int scale)
{
    int n = 0;
    for (const unsigned char *p = (const unsigned char *)s; *p && n < max_chars; p++) {
        if ((*p & 0xC0) == 0x80) continue;             /* continuação UTF-8 */
        draw_char(x + n * 6 * scale, y, *p >= 0x80 ? '?' : *p, scale);
        n++;
    }
    return n * 6 * scale;
}

static int text_len(const char *s)
{
    int n = 0;
    for (const unsigned char *p = (const unsigned char *)s; *p; p++)
        if ((*p & 0xC0) != 0x80) n++;
    return n;
}

static int draw_text_width(const char *s) { return text_len(s) * 6 - 1; }

static void draw_text_right(int right_x, int y, const char *s, int scale)
{
    draw_text(right_x - text_len(s) * 6 * scale + scale, y, s, 64, scale);
}

static void draw_text_center(int y, const char *s, int max_chars, int scale)
{
    int n = text_len(s);
    if (n > max_chars) n = max_chars;
    draw_text((EPD_W - n * 6 * scale + scale) / 2, y, s, max_chars, scale);
}

/* Quebra por palavra em linhas de até max_chars; devolve linhas usadas. */
static int draw_wrapped(int x, int y, const char *s, int max_chars, int max_lines, int line_h)
{
    int lines = 0;
    const char *p = s;
    while (*p && lines < max_lines) {
        while (*p == ' ') p++;
        int len = (int)strlen(p);
        int take = len <= max_chars ? len : max_chars;
        if (len > max_chars) {
            int cut = take;
            while (cut > 0 && p[cut] != ' ') cut--;
            if (cut > 0) take = cut;
        }
        char buf[64];
        if (take > (int)sizeof(buf) - 1) take = sizeof(buf) - 1;
        memcpy(buf, p, take);
        buf[take] = '\0';
        draw_text(x, y + lines * line_h, buf, max_chars, 1);
        p += take;
        lines++;
    }
    return lines;
}

/* ---------------------------------------------------------------- marcas */
static void draw_mark(int x, int y, const char *status)
{
    if (!strcmp(status, "working")) {            /* triângulo ▸ */
        for (int c = 0; c < 4; c++) epd_fb_rect(x + c, y + c, 1, 7 - 2 * c, s_ink);
    } else if (!strcmp(status, "blocked")) {     /* círculo cheio ● */
        epd_fb_rect(x + 1, y, 5, 7, s_ink);
        epd_fb_rect(x, y + 1, 7, 5, s_ink);
    } else if (!strcmp(status, "done")) {        /* check ✓ */
        for (int i = 0; i < 3; i++) epd_fb_rect(x + i, y + 3 + i, 1, 2, s_ink);
        for (int i = 0; i < 4; i++) epd_fb_rect(x + 3 + i, y + 4 - i, 1, 2, s_ink);
    } else if (!strcmp(status, "idle")) {        /* ponto · */
        epd_fb_rect(x + 2, y + 2, 3, 3, s_ink);
    } else {                                     /* unknown: ? */
        draw_char(x + 1, y, '?', 1);
    }
}

static void tag_of(const char *agent, char out[3])
{
    if (!agent || !agent[0]) { strcpy(out, "??"); return; }
    if (!strncmp(agent, "claude", 6)) { strcpy(out, "CC"); return; }
    if (!strncmp(agent, "codex", 5)) { strcpy(out, "CX"); return; }
    out[0] = (char)(agent[0] >= 'a' && agent[0] <= 'z' ? agent[0] - 32 : agent[0]);
    out[1] = agent[1] ? (char)(agent[1] >= 'a' && agent[1] <= 'z' ? agent[1] - 32 : agent[1]) : ' ';
    out[2] = '\0';
}

static void fmt_age(time_t now, uint32_t since, char *out, size_t n)
{
    out[0] = '\0';
    if (!now || !since || (time_t)since > now) return;
    long s = (long)(now - since);
    /* nada de segundos: no e-paper cada segundo viraria um refresh (378 ms e
     * um passo de drift) sem informação nenhuma — o pager não é cronômetro */
    if (s < 60) snprintf(out, n, "<1m");
    else if (s < 3600) snprintf(out, n, "%ldm", s / 60);
    else snprintf(out, n, "%ldh", s / 3600);
}

/* ---------------------------------------------------------------- modelo */
static int rank_of(const char *status)
{
    static const char *order[] = { "blocked", "working", "done", "idle" };
    for (int i = 0; i < 4; i++) if (!strcmp(status, order[i])) return i;
    return 4;
}

static void snapshot_model(void)
{
    static herdr_agent_t agents[HERDR_MAX_AGENTS_TOTAL];
    int n = herdr_model_get_agents(agents, HERDR_MAX_AGENTS_TOTAL);
    s_row_count = 0;
    s_waiting = 0;
    /* ordem estável: rank do status, depois a ordem da ponte */
    for (int r = 0; r <= 4; r++) {
        for (int i = 0; i < n; i++) {
            if (rank_of(agents[i].status) != r) continue;
            row_t *row = &s_rows[s_row_count++];
            row->host = agents[i].host;
            row->since = agents[i].since;
            snprintf(row->pane_id, sizeof(row->pane_id), "%s", agents[i].pane_id);
            snprintf(row->project, sizeof(row->project), "%s", agents[i].project);
            snprintf(row->agent, sizeof(row->agent), "%s", agents[i].agent);
            snprintf(row->status, sizeof(row->status), "%s", agents[i].status);
            if (r == 0) s_waiting++;
        }
    }
    /* o foco segue o pane (pela identidade, não pela posição) */
    s_focus = -1;
    for (int i = 0; i < s_row_count && s_focus_pane[0]; i++) {
        if (s_rows[i].host == s_focus_host && !strcmp(s_rows[i].pane_id, s_focus_pane)) {
            s_focus = i;
            break;
        }
    }
    if (s_focus < 0 && s_waiting > 0) s_focus = 0;     /* sem foco: o primeiro esperando */
    if (s_focus >= 0) {
        snprintf(s_focus_pane, sizeof(s_focus_pane), "%s", s_rows[s_focus].pane_id);
        s_focus_host = s_rows[s_focus].host;
    } else {
        s_focus_pane[0] = '\0';
        s_focus_host = -1;
    }
}

static bool any_online(void)
{
    for (int h = 0; h < CFG_MAX_HOSTS; h++)
        if (herdr_model_get_conn(h) == HERDR_CONN_ONLINE) return true;
    return false;
}

/* ---------------------------------------------------------------- telas */
static void draw_top(time_t now)
{
    char hm[6] = "--:--";
    if (now) {
        struct tm tm;
        localtime_r(&now, &tm);
        snprintf(hm, sizeof(hm), "%02d:%02d", tm.tm_hour, tm.tm_min);
    }
    draw_text(3, 3, hm, 5, 2);
    const char *link = !s_wifi_up ? "NO WIFI" : any_online() ? "ONLINE" : "OFFLINE";
    draw_text_right(EPD_W - 3, 2, link, 1);
    if (s_batt_pct >= 0) {
        /* pilha: corpo 13x7 + polo, preenchida na proporção, e o número */
        char pct[12];
        snprintf(pct, sizeof(pct), "%d%%", s_batt_pct);
        int w = draw_text_width(pct);
        int x = EPD_W - 3 - w - 17;
        epd_fb_rect(x, 13, 13, 1, true);
        epd_fb_rect(x, 19, 13, 1, true);
        epd_fb_rect(x, 13, 1, 7, true);
        epd_fb_rect(x + 12, 13, 1, 7, true);
        epd_fb_rect(x + 13, 15, 2, 3, true);
        epd_fb_rect(x + 2, 15, (s_batt_pct * 9 + 50) / 100, 3, true);
        draw_text_right(EPD_W - 3, 13, pct, 1);
        if (s_batt_pct <= 15) epd_fb_invert_rect(x - 2, 12, EPD_W - x + 2, 8);
    }
    if (s_muted) draw_text(72, 7, "MUTED", 5, 1);
    epd_fb_hline(0, TOP_H, EPD_W, true);
}

static void draw_footer(const char *legend, bool with_status)
{
    epd_fb_hline(0, FOOT_Y - 1, EPD_W, true);
    draw_text(3, FOOT_Y + 2, legend, 32, 1);
    if (!with_status) return;
    draw_text(3, FOOT_Y + 12, s_status, 24, 1);
    if (s_update) draw_text_right(EPD_W - 3, FOOT_Y + 12, "UPDATE", 1);
}

static void draw_main(time_t now)
{
    draw_top(now);

    /* banda de alerta */
    char line[40], tag[3];
    if (!any_online()) {
        /* Sem ponte nada aqui é ao vivo: a lista é o último retrato. Dizer
           "ALL CLEAR" nessa hora era mentira — e prometer BOOT/PWR também,
           porque os comandos não têm para onde ir. */
        draw_text_center(ALERT_Y + 10, "NO BRIDGE", 15, 2);
        if (s_row_count) {
            if (s_waiting > 0) {
                snprintf(line, sizeof(line), "last state: %d waiting", s_waiting);
            } else {
                snprintf(line, sizeof(line), "last state, not live");
            }
        } else {
            snprintf(line, sizeof(line), "waiting for herdr-assist");
        }
        draw_text_center(ALERT_Y + 34, line, 30, 1);
    } else if (s_waiting > 0) {
        epd_fb_rect(0, ALERT_Y, EPD_W, ALERT_H, true);
        s_ink = false;
        snprintf(line, sizeof(line), "%d WAITING", s_waiting);
        draw_text(6, ALERT_Y + 5, line, 15, 2);
        const row_t *f = s_focus >= 0 && !strcmp(s_rows[s_focus].status, "blocked")
                         ? &s_rows[s_focus] : &s_rows[0];
        tag_of(f->agent, tag);
        snprintf(line, sizeof(line), "%s %s", tag, f->project);
        draw_text(6, ALERT_Y + 27, line, 31, 1);
        if (s_waiting > 1) {
            snprintf(line, sizeof(line), "+%d more: PWR to cycle", s_waiting - 1);
            draw_text(6, ALERT_Y + 41, line, 31, 1);
        } else {
            draw_text(6, ALERT_Y + 41, "BOOT sends Enter", 31, 1);
        }
        s_ink = true;
    } else if (s_row_count > 0) {
        draw_text_center(ALERT_Y + 10, "ALL CLEAR", 15, 2);
        int working = 0;
        for (int i = 0; i < s_row_count; i++) if (!strcmp(s_rows[i].status, "working")) working++;
        snprintf(line, sizeof(line), "%d working / %d %s", working, s_row_count,
                 s_row_count == 1 ? "session" : "sessions");
        draw_text_center(ALERT_Y + 34, line, 32, 1);
    } else {
        draw_text_center(ALERT_Y + 10, any_online() ? "NO AGENTS" : "NO BRIDGE", 15, 2);
        draw_text_center(ALERT_Y + 34, any_online() ? "start one in Herdr" : "waiting for herdr-assist", 32, 1);
    }
    epd_fb_hline(0, LIST_Y - 2, EPD_W, true);

    /* lista: seis linhas; se houver mais, a última vira "+N more".
       A janela rola junto com o foco: sem isso, com mais sessões que linhas o
       PWR levava o foco para fora da tela e não dava para ver qual estava
       selecionado — justamente o que o pager precisa dizer. */
    int visible = s_row_count <= ROWS_MAX ? s_row_count : ROWS_MAX - 1;
    int first = 0;
    if (s_focus >= visible) {
        first = s_focus - visible + 1;
        if (first + visible > s_row_count) first = s_row_count - visible;
    }
    for (int i = 0; i < visible; i++) {
        const row_t *r = &s_rows[first + i];
        int y = LIST_Y + i * ROW_H;
        char age[8];
        draw_mark(4, y + 4, r->status);
        tag_of(r->agent, tag);
        draw_text(16, y + 4, tag, 2, 1);
        fmt_age(now, r->since, age, sizeof(age));
        int max_proj = age[0] ? 22 : 27;
        draw_text(34, y + 4, r->project, max_proj, 1);
        if (age[0]) draw_text_right(EPD_W - 4, y + 4, age, 1);
        if (first + i == s_focus) epd_fb_invert_rect(0, y, EPD_W, ROW_H - 1);
    }
    if (s_row_count > visible) {
        /* "sessions" para não colidir com o "+N more: PWR to cycle" da banda,
           que conta quem espera, não linhas escondidas */
        snprintf(line, sizeof(line), "+%d more sessions", s_row_count - visible);
        draw_text(34, LIST_Y + visible * ROW_H + 4, line, 20, 1);
    }

    /* o rodapé só promete o que funciona: sem sessão na tela não há "select",
       sem retrato antigo não há "last state shown", e com uma linha só o PWR
       não anda — ele só manda o foco para essa sessão */
    const char *legend;
    if (!any_online()) {
        legend = s_row_count ? "offline - last state shown" : "offline";
    } else if (s_waiting > 0) {
        legend = s_row_count > 1 ? "PWR next  BOOT enter" : "PWR focus  BOOT enter";
    } else if (s_row_count > 1) {
        legend = "PWR select  BOOT-hold mute";
    } else if (s_row_count > 0) {
        legend = "PWR focus  BOOT-hold mute";
    } else {
        legend = "BOOT-hold: mute";
    }
    draw_footer(legend, true);
}

static void draw_title(const char *t)
{
    draw_text_center(8, t, 15, 2);
    epd_fb_hline(0, 30, EPD_W, true);
}

static void draw_setup(void)
{
    draw_title("WI-FI SETUP");
    draw_text(6, 40, "1. Join this Wi-Fi:", 31, 1);
    draw_text(6, 52, s_setup_ssid, 15, text_len(s_setup_ssid) <= 15 ? 2 : 1);
    draw_text(6, 74, "password:", 31, 1);
    draw_text(6, 86, s_setup_pass, 15, 2);
    draw_text(6, 110, "2. Open in a browser:", 31, 1);
    draw_text(6, 122, s_setup_url, 15, text_len(s_setup_url) <= 15 ? 2 : 1);
    draw_wrapped(6, 146, "Pick your network; the pager restarts on save.", 31, 2, 10);
    draw_footer("PWR+BOOT 3s: cancel", false);
}

static void draw_pairing(void)
{
    char line[40];
    draw_title("PAIR");
    if (!s_pair_wifi) {
        draw_wrapped(6, 44, "Waiting for Wi-Fi... hold PWR+BOOT 3s to change the network.", 31, 4, 12);
    } else {
        draw_wrapped(6, 38, "On the Herdr machine: open the herdr-assist dialog, Pair tab, and pick:", 31, 4, 11);
        draw_text_center(82, s_pair_id, 15, 3);
        if (s_pair_secs > 0) {
            snprintf(line, sizeof(line), "window closes in %ds", s_pair_secs);
            draw_text_center(116, line, 31, 1);
        } else {
            draw_text_center(116, "window closed", 31, 1);
        }
    }
    draw_footer(s_pair_secs > 0 ? "PWR+BOOT 3s: Wi-Fi setup" : "PWR hold: open again", false);
}

static void draw_message(void)
{
    draw_title(s_msg_title);
    draw_wrapped(6, 48, s_msg_l1, 31, 3, 12);
    draw_wrapped(6, 96, s_msg_l2, 31, 3, 12);
    draw_footer("", false);
}

/* ---------------------------------------------------------------- API */
void pager_ui_set_screen(ui_screen_t screen) { s_screen = screen; }
ui_screen_t pager_ui_screen(void) { return s_screen; }

void pager_ui_set_setup(const char *ssid, const char *pass, const char *url)
{
    snprintf(s_setup_ssid, sizeof(s_setup_ssid), "%s", ssid ? ssid : "");
    snprintf(s_setup_pass, sizeof(s_setup_pass), "%s", pass ? pass : "");
    snprintf(s_setup_url, sizeof(s_setup_url), "%s", url ? url : "");
}

void pager_ui_set_pairing(const char *id, int secs_left, bool wifi_up)
{
    snprintf(s_pair_id, sizeof(s_pair_id), "%s", id ? id : "");
    s_pair_secs = secs_left;
    s_pair_wifi = wifi_up;
}

void pager_ui_set_message(const char *title, const char *l1, const char *l2)
{
    snprintf(s_msg_title, sizeof(s_msg_title), "%s", title ? title : "");
    snprintf(s_msg_l1, sizeof(s_msg_l1), "%s", l1 ? l1 : "");
    snprintf(s_msg_l2, sizeof(s_msg_l2), "%s", l2 ? l2 : "");
}

void pager_ui_set_status(const char *status)
{
    snprintf(s_status, sizeof(s_status), "%s", status ? status : "");
}

void pager_ui_set_battery(int pct) { s_batt_pct = pct > 100 ? 100 : pct; }

void pager_ui_set_flags(bool wifi_up, bool muted, bool update_available)
{
    s_wifi_up = wifi_up;
    s_muted = muted;
    s_update = update_available;
}

/* ---------------------------------------------------------------- descanso */
/* Sem arte externa em 1 bit: o avatar é desenhado — um mascote dormindo.
   Disco cheio, e os traços do rosto em branco por cima dele. */
static void fill_disc(int cx, int cy, int r, bool black)
{
    for (int y = -r; y <= r; y++) {
        int w = 0;
        while ((w + 1) * (w + 1) + y * y <= r * r) {
            w++;
        }
        if (w > 0) {
            epd_fb_hline(cx - w, cy + y, 2 * w, black);
        }
    }
}

static void draw_sleep(void)
{
    fill_disc(100, 76, 50, true);
    epd_fb_rect(72, 62, 20, 3, false);      /* olhos fechados */
    epd_fb_rect(108, 62, 20, 3, false);
    epd_fb_rect(88, 92, 24, 3, false);      /* boca */
    draw_text(152, 16, "z", 1, 3);
    draw_text(168, 42, "z", 1, 2);
    draw_text(180, 56, "z", 1, 1);
    draw_text_center(136, "OFFLINE", 10, 2);
    draw_text_center(162, "press PWR to retry", 20, 1);
}

/* O relógio do topo não conta como mudança: sem refresh rápido, cada minuto
 * viraria um pisca de ~1,4 s. Ele é repintado quando a tela muda por outro
 * motivo — e mostra a hora daquele momento. A área é alinhada em bytes
 * (64 px = 8 bytes) para poder pular com memcmp. */
#define CLOCK_W_BYTES 8

/* Separa as mudanças que cabem em faixas (hora, idades) das que precisam da
 * tela inteira. Cada byte que difere é localizado; se algum cair fora das duas
 * faixas, é parcial de tela inteira. */
static bool in_strip(int x, int y, int x0, int y0, int x1, int y1)
{
    return x >= x0 && x < x1 && y >= y0 && y < y1;
}

static ui_frame_t what_changed(void)
{
    const uint8_t *a = s_last_frame;
    const uint8_t *b = epd_fb();
    const int stride = EPD_W / 8;
    bool clock = false, age = false;
    for (int y = 0; y < EPD_H; y++) {
        for (int bx = 0; bx < stride; bx++) {
            uint8_t d = a[y * stride + bx] ^ b[y * stride + bx];
            if (!d) continue;
            for (int bit = 0; bit < 8; bit++) {
                if (!(d & (0x80 >> bit))) continue;
                int x = bx * 8 + bit;
                if (in_strip(x, y, 0, 0, PAGER_CLOCK_W, PAGER_CLOCK_H)) clock = true;
                else if (in_strip(x, y, PAGER_AGE_X0, PAGER_AGE_Y0, PAGER_AGE_X1, PAGER_AGE_Y1)) age = true;
                else return UI_FRAME_PARTIAL;
            }
        }
    }
    if (clock && age) return UI_FRAME_STRIPS;
    if (clock) return UI_FRAME_CLOCK;
    if (age) return UI_FRAME_STRIPS;
    return UI_FRAME_NONE;
}

ui_frame_t pager_ui_render(time_t now)
{
    snapshot_model();
    epd_fb_clear();
    s_ink = true;
    switch (s_screen) {
    case UI_SETUP:   draw_setup(); break;
    case UI_PAIRING: draw_pairing(); break;
    case UI_MESSAGE: draw_message(); break;
    case UI_SLEEP:   draw_sleep(); break;
    default:         draw_main(now); break;
    }
    ui_frame_t what = UI_FRAME_NONE;
    if (!s_have_last) {
        what = UI_FRAME_FULL;
    } else if (memcmp(s_last_frame, epd_fb(), EPD_FB_BYTES)) {
        /* só a hora mudou? então dá para dirigir apenas a caixa dela e deixar
         * o resto do vidro em paz (ver epd_bsp_flush_area) */
        what = what_changed();
    }
    if (what != UI_FRAME_NONE) {
        memcpy(s_last_frame, epd_fb(), EPD_FB_BYTES);
        s_have_last = true;
    }
    return what;
}

void pager_ui_invalidate(void) { s_have_last = false; }

void pager_ui_focus_next(void)
{
    if (s_row_count == 0) return;
    /* Uma posição por vez. A lista já vem ordenada blocked -> working -> done ->
       idle, então quem espera aparece primeiro e o resto vem depois — que é o
       que o §4 pede ("percorre quem espera e depois todos"). Antes, com alguém
       esperando, o laço procurava outro "blocked": com um único esperando ele
       reescolhia o mesmo pane, e na tela de espera o PWR parecia morto. */
    s_focus = (s_focus + 1) % s_row_count;
    snprintf(s_focus_pane, sizeof(s_focus_pane), "%s", s_rows[s_focus].pane_id);
    s_focus_host = s_rows[s_focus].host;
}

bool pager_ui_focused(int *host, char *pane_id, int pane_len, char *project, int project_len)
{
    if (s_focus < 0 || s_focus >= s_row_count) return false;
    if (host) *host = s_rows[s_focus].host;
    if (pane_id) snprintf(pane_id, pane_len, "%s", s_rows[s_focus].pane_id);
    if (project) snprintf(project, project_len, "%s", s_rows[s_focus].project);
    return true;
}

bool pager_ui_answer_target(int *host, char *pane_id, int pane_len, char *project, int project_len)
{
    int idx = -1;
    if (s_focus >= 0 && s_focus < s_row_count && !strcmp(s_rows[s_focus].status, "blocked")) {
        idx = s_focus;
    } else {
        /* a lista já começa nos bloqueados: o primeiro é o que a banda nomeia */
        for (int i = 0; i < s_row_count; i++) {
            if (!strcmp(s_rows[i].status, "blocked")) {
                idx = i;
                break;
            }
        }
    }
    if (idx < 0) return false;
    if (host) *host = s_rows[idx].host;
    if (pane_id) snprintf(pane_id, pane_len, "%s", s_rows[idx].pane_id);
    if (project) snprintf(project, project_len, "%s", s_rows[idx].project);
    return true;
}

int pager_ui_waiting(void) { return s_waiting; }
