#include "ui.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#include <M5Cardputer.h>
#include <Preferences.h>
#include <WiFi.h>
#include <esp_mac.h>

#include "herdr_conn.h"
#include "herdr_model.h"
#include "keys.h"
#include "net.h"
#include "net_extra.h"
#include "pairing.h"
#include "panel_cfg.h"
#include "theme.h"
#include "ui_term.h"

#include "assets/logo_boot.h"

/* ---------------------------------------------------------------- estado -- */

/* A raiz é um menu, não uma aba escondida atrás de uma tecla: tudo que o
   aparelho faz precisa ser alcançável com setas e enter. As combinações (fn+y,
   opt+;) continuam existindo, mas como atalho de quem já sabe — nunca como o
   único caminho até uma ação. */
enum Screen {
    SCR_MENU = 0,
    SCR_SESSIONS,
    SCR_DASH,
    SCR_SETTINGS,
    SCR_TAB_FIRST = SCR_SESSIONS,   /* faixa que ,/ percorre como atalho */
    SCR_TAB_LAST  = SCR_SETTINGS,
    SCR_TERM,
    SCR_TERMMENU,       /* ações da sessão aberta, em lista */
    SCR_PAIR,
    SCR_WIFI,
    SCR_HOSTS,
    SCR_INFO,
    SCR_LOGO,
    SCR_INPUT,
};

/* Cadência do read_pane. A ponte entrega agentes por push, mas o conteúdo do
   terminal é sob demanda — este é o "polling" da sessão aberta. */
#define TERM_POLL_MS      3000
#define TERM_REQ_LINES      40   /* teto do buffer compartilhado (TERM_MAX_LINES) */
#define MAX_APS             20

static Screen  s_screen = SCR_MENU;
static Screen  s_back   = SCR_MENU;       /* para onde ` volta nas sub-telas */
static bool    s_dirty  = true;
static uint32_t s_gen;                    /* geração do modelo já desenhada */
static time_t  s_last_min;                /* minuto do relógio já desenhado */
static char    s_toast[40];
static uint32_t s_toast_until;

static panel_cfg_t s_edit;                /* cópia editável; vale ao reiniciar */
static bool        s_cfg_dirty;
static bool        s_tiny_font;

static herdr_agent_t s_agents[HERDR_MAX_AGENTS_TOTAL];
static int           s_agent_count;
static int           s_sel, s_top;

static herdr_limits_t s_limits[CFG_MAX_HOSTS * HERDR_MAX_PROVIDERS];
static int            s_limit_count;

static net_ap_t s_aps[MAX_APS];
static int      s_ap_count;
static int      s_ap_sel, s_ap_top;

static int s_menu_sel;       /* configurações */
static int s_host_sel;       /* tela de hosts */
static int s_root_sel;       /* menu raiz */
static bool s_display_off;   /* tela apagada para carregar (qualquer tecla volta) */
static int s_tm_sel, s_tm_top;  /* menu de ações da sessão */

/* --- sessão aberta --- */
static int      s_term_host = -1;
static char     s_term_pane[HERDR_ID_LEN];
static char     s_term_agent[HERDR_NAME_LEN];
static char     s_term_project[HERDR_NAME_LEN];
static char    *s_term_buf;
static uint32_t s_term_seq;
static uint32_t s_term_next_read;
static int      s_term_rows;
static char     s_input[128];
static int      s_input_len;

/* --- caixa de texto (SSID, senha, endereço) --- */
static char        s_in_buf[CFG_PASS_LEN];
static const char *s_in_title;
static void      (*s_in_done)(const char *);

/* -------------------------------------------------------------- desenho -- */

/* Não há RAM para um buffer da tela inteira (240x135x2 = 64KB num chip sem
   PSRAM), então o antiflicker é por linha: um sprite de 240x16 desenhado
   inteiro e empurrado com clip na altura que a linha pede. */
#define ROWC_H 16
static M5Canvas s_rowc;   /* sem pai no construtor: a ordem de init dos globais
                             não garante que o M5Cardputer já exista */

static M5Canvas &rowc(void)
{
    s_rowc.setFont(&fonts::Font0);
    return s_rowc;
}

/* Repintar a tela inteira a cada segundo (o cronômetro das sessões anda) faz
   o corpo piscar: o fillRect preto aparece entre apagar e repintar. Como cada
   linha é um sprite que cobre a própria área, o apagar é dispensável — e a
   maioria das linhas não muda de um segundo para o outro. Estas assinaturas
   guardam o que já está na tela para não reenviar o que está igual. O modelo
   continua mandando na hora de atualizar; muda só o que vai para o display. */
static char   s_sb_cache[72];      /* barra de status */
static char   s_ft_cache[72];      /* rodapé */
static char   s_row_cache[12][64]; /* linhas da lista */
static int    s_rows_drawn = -1;   /* quantas linhas a lista pintou por último */
static Screen s_drawn_screen = (Screen)-1;

static void ui_invalidate(void)
{
    s_sb_cache[0] = '\0';
    s_ft_cache[0] = '\0';
    for (int i = 0; i < (int)(sizeof(s_row_cache) / sizeof(s_row_cache[0])); i++) {
        s_row_cache[i][0] = '\0';
    }
    s_rows_drawn = -1;
}

/** true (e guarda) quando `sig` difere do que está na tela naquele lugar. */
static bool changed(char *cache, size_t cap, const char *sig)
{
    if (strcmp(cache, sig) == 0) {
        return false;
    }
    strlcpy(cache, sig, cap);
    return true;
}

static void push_at(M5Canvas &c, int y, int h)
{
    auto &d = M5Cardputer.Display;
    d.setClipRect(0, y, SCR_W, h);
    c.pushSprite(&d, 0, y);
    d.clearClipRect();
}

uint16_t status_color(const char *status)
{
    if (strcmp(status, "blocked") == 0) return C_BLOCKED;
    if (strcmp(status, "working") == 0) return C_WORKING;
    if (strcmp(status, "done") == 0)    return C_DONE;
    if (strcmp(status, "idle") == 0)    return C_IDLE;
    return C_MUTED;
}

/**
 * Fase do pisca das sessões bloqueadas (~1,1 Hz).
 *
 * Uma sessão bloqueada é a única coisa nesta tela que exige ação humana, e cor
 * parada se perde no meio de uma lista colorida. Movimento não.
 */
static bool blink_on(void) { return (millis() / 450) % 2 == 0; }

static bool agent_blocked(const herdr_agent_t *a)
{
    return strcmp(a->status, "blocked") == 0;
}

/** Trunca em max caracteres, com ~ no lugar do que sobrou. */
static void trunc_to(char *s, int max)
{
    int n = (int)strlen(s);
    if (n > max && max > 0) {
        s[max - 1] = '~';
        s[max] = '\0';
    }
}

static void toast(const char *msg)
{
    strlcpy(s_toast, msg, sizeof(s_toast));
    s_toast_until = millis() + 2500;
    s_dirty = true;
}

/**
 * Carga da bateria em %, ou -1 quando o medidor não responde.
 *
 * Amostrado a cada 10s: a leitura é por ADC e oscila um ponto para cima e para
 * baixo o tempo todo. Reler a cada quadro faria o número tremer na tela e, pior,
 * invalidaria a assinatura da barra de status a cada segundo — devolvendo o
 * piscar que acabamos de tirar.
 */
static int battery_pct(void)
{
    static int s_bat = -1;
    static uint32_t s_bat_at;
    if (s_bat < 0 || (int32_t)(millis() - s_bat_at) > 10000) {
        s_bat_at = millis();
        int v = M5Cardputer.Power.getBatteryLevel();
        if (v >= 0) {
            s_bat = v > 100 ? 100 : v;
        }
    }
    return s_bat;
}

/** Estado agregado das conexões: o pior de todos os hosts habilitados. */
static herdr_conn_state_t conn_overall(void)
{
    herdr_conn_state_t best = HERDR_CONN_OFFLINE;
    bool any = false;
    for (int i = 0; i < CFG_MAX_HOSTS; i++) {
        if (!panel_cfg_get()->hosts[i].enabled) {
            continue;
        }
        any = true;
        herdr_conn_state_t c = herdr_model_get_conn(i);
        if (c > best) {
            best = c;
        }
    }
    return any ? best : HERDR_CONN_OFFLINE;
}

static void draw_status_bar(const char *title)
{
    auto &c = rowc();
    c.fillSprite(C_BG);
    c.setTextColor(C_TEXT);
    c.setCursor(2, 1);
    c.print(title);

    /* direita: relógio (quando o SNTP já pegou) e o ponto de conexão */
    char right[16] = "";
    time_t now = time(NULL);
    struct tm tmv;
    if (now > 1700000000 && localtime_r(&now, &tmv)) {
        snprintf(right, sizeof(right), "%02d:%02d", tmv.tm_hour, tmv.tm_min);
    }
    char bat[8] = "";
    int pct = battery_pct();
    if (pct >= 0) {
        snprintf(bat, sizeof(bat), "%d%%", pct);
    }

    /* montado da direita para a esquerda: ponto de conexão, hora, bateria */
    int x = SCR_W - 8;
    if (right[0]) {
        x -= (int)strlen(right) * 6;
        c.setTextColor(C_MUTED);
        c.setCursor(x, 1);
        c.print(right);
        x -= 4;
    }
    if (bat[0]) {
        x -= (int)strlen(bat) * 6;
        /* abaixo de 20% a cor é o aviso; o número sozinho passa despercebido */
        c.setTextColor(pct < 20 ? C_BLOCKED : C_MUTED);
        c.setCursor(x, 1);
        c.print(bat);
    }
    uint16_t dot = C_BLOCKED;
    if (!net_wifi_is_up()) {
        dot = C_BLOCKED;
    } else {
        switch (conn_overall()) {
        case HERDR_CONN_ONLINE:     dot = C_IDLE; break;
        case HERDR_CONN_CONNECTING: dot = C_WORKING; break;
        default:                    dot = C_BLOCKED; break;
        }
    }
    /* o relógio é HH:MM, então isto vai ao display uma vez por minuto */
    char sig[72];
    snprintf(sig, sizeof(sig), "%s|%s|%s|%u", title, right, bat, (unsigned)dot);
    if (!changed(s_sb_cache, sizeof(s_sb_cache), sig)) {
        return;
    }
    c.fillCircle(SCR_W - 4, STATUS_H / 2, 2, dot);
    push_at(c, 0, STATUS_H);
    M5Cardputer.Display.drawFastHLine(0, STATUS_H, SCR_W, C_BORDER);
}

static void draw_footer(const char *hint)
{
    bool tem_toast = millis() < s_toast_until;
    const char *texto = tem_toast ? s_toast : hint;
    if (!changed(s_ft_cache, sizeof(s_ft_cache), texto)) {
        return;
    }
    auto &c = rowc();
    c.fillSprite(C_BG);
    c.drawFastHLine(0, 0, SCR_W, C_BORDER);
    c.setTextColor(tem_toast ? C_ACCENT : C_MUTED);
    c.setCursor(2, 2);
    c.print(texto);
    push_at(c, SCR_H - FOOT_H, FOOT_H);
}

/** Corpo útil das telas de lista (entre a barra de status e o rodapé). */
static int body_rows(void) { return (SCR_H - BODY_Y - FOOT_H) / ROW_H; }

/** Mantém o item selecionado dentro da janela visível. */
static void clamp_window(int sel, int count, int *top)
{
    int rows = body_rows();
    if (sel < *top) {
        *top = sel;
    }
    if (sel >= *top + rows) {
        *top = sel - rows + 1;
    }
    if (*top > count - rows) {
        *top = count - rows;
    }
    if (*top < 0) {
        *top = 0;
    }
}

static void clear_body(void)
{
    M5Cardputer.Display.fillRect(0, BODY_Y, SCR_W, SCR_H - BODY_Y - FOOT_H, C_BG);
}

/** Uma linha de lista com seleção; devolve o sprite para o chamador completar. */
static M5Canvas &list_row(bool selected)
{
    auto &c = rowc();
    c.fillSprite(selected ? C_PANEL : C_BG);
    if (selected) {
        c.drawFastVLine(0, 0, ROW_H, C_ACCENT);
    }
    return c;
}

static void push_row(M5Canvas &c, int idx_on_screen)
{
    push_at(c, BODY_Y + idx_on_screen * ROW_H, ROW_H);
}

/**
 * Lista de itens navegável — a peça que sustenta a interface inteira.
 *
 * Toda tela que ofereça escolha usa isto: raiz, configurações e as ações da
 * sessão. É o que garante que nada dependa de decorar combinação de teclas.
 */
static void draw_list_menu(const char *title, const char *const *items,
                           int count, int sel, int *top, const char *hint)
{
    draw_status_bar(title);
    clamp_window(sel, count, top);
    clear_body();
    int rows = body_rows();
    for (int i = 0; i < rows && *top + i < count; i++) {
        int idx = *top + i;
        auto &c = list_row(idx == sel);
        char label[44];
        snprintf(label, sizeof(label), "%s", items[idx]);
        trunc_to(label, 36);
        c.setTextColor(idx == sel ? C_TEXT : rgb565(0xc8c8cc));
        c.setCursor(6, 2);
        c.print(label);
        if (idx == sel) {
            c.setTextColor(C_ACCENT);
            c.setCursor(SCR_W - 8, 2);
            c.print(">");
        }
        push_row(c, i);
    }
    /* seta de "tem mais coisa aqui embaixo": sem ela a lista mente sobre o
       próprio tamanho quando não cabe na tela */
    if (count > rows) {
        auto &d = M5Cardputer.Display;
        if (*top > 0) {
            d.fillTriangle(SCR_W - 5, BODY_Y + 2, SCR_W - 9, BODY_Y + 7,
                           SCR_W - 1, BODY_Y + 7, C_MUTED);
        }
        if (*top + rows < count) {
            int y = BODY_Y + rows * ROW_H - 3;
            d.fillTriangle(SCR_W - 5, y, SCR_W - 9, y - 5, SCR_W - 1, y - 5, C_MUTED);
        }
    }
    draw_footer(hint);
}

/** Move a seleção de um menu; true se mudou. */
static bool menu_move(const KeyEvent &e, int *sel, int count)
{
    if (e.up() && *sel > 0) {
        (*sel)--;
        return true;
    }
    if (e.down() && *sel + 1 < count) {
        (*sel)++;
        return true;
    }
    return false;
}

static void center_msg(const char *l1, const char *l2)
{
    /* mesma razão das linhas: o estado vazio também é repintado a cada
       segundo, e sem isto ele pisca igual */
    char sig[64];
    snprintf(sig, sizeof(sig), "msg|%s|%s", l1, l2 ? l2 : "");
    if (!changed(s_row_cache[0], sizeof(s_row_cache[0]), sig)) {
        return;
    }
    /* o corpo passa a ser do recado; a lista limpa antes de retomá-lo */
    s_rows_drawn = -1;
    auto &d = M5Cardputer.Display;
    clear_body();
    d.setFont(&fonts::Font0);
    d.setTextDatum(middle_center);
    d.setTextColor(C_TEXT, C_BG);
    d.drawString(l1, SCR_W / 2, BODY_Y + 40);
    if (l2) {
        d.setTextColor(C_MUTED, C_BG);
        d.drawString(l2, SCR_W / 2, BODY_Y + 56);
    }
    d.setTextDatum(top_left);
}

/* ------------------------------------------------------------- sessões --- */

static void fmt_elapsed(char *out, size_t n, uint32_t since)
{
    time_t now = time(NULL);
    if (!since || now < 1700000000 || (uint32_t)now < since) {
        out[0] = '\0';
        return;
    }
    long d = (long)now - (long)since;
    if (d < 60) {
        snprintf(out, n, "%lds", d);
    } else if (d < 3600) {
        snprintf(out, n, "%ldm", d / 60);
    } else {
        snprintf(out, n, "%ldh%02ld", d / 3600, (d % 3600) / 60);
    }
}

static void refresh_agents(void)
{
    s_agent_count = herdr_model_get_agents(s_agents, HERDR_MAX_AGENTS_TOTAL);
    if (s_sel >= s_agent_count) {
        s_sel = s_agent_count ? s_agent_count - 1 : 0;
    }
    s_limit_count = herdr_model_get_limits(s_limits, (int)(sizeof(s_limits) / sizeof(s_limits[0])));
}

/** true quando mais de um host está habilitado (aí a lista mostra de quem é). */
static bool multi_host(void)
{
    int n = 0;
    for (int i = 0; i < CFG_MAX_HOSTS; i++) {
        n += panel_cfg_get()->hosts[i].enabled ? 1 : 0;
    }
    return n > 1;
}

static void draw_sessions(void)
{
    char title[24];
    snprintf(title, sizeof(title), "SESSIONS %d", s_agent_count);
    draw_status_bar(title);

    if (!panel_cfg_wifi_ok()) {
        center_msg("sem Wi-Fi configurado", "va em SETTINGS");
        draw_footer("` volta");
        return;
    }
    if (s_agent_count == 0) {
        char l2[48];
        if (net_wifi_is_up()) {
            strlcpy(l2, "esperando a ponte...", sizeof(l2));
        } else {
            snprintf(l2, sizeof(l2), "Wi-Fi: %s", net_wifi_status_text());
        }
        center_msg("nenhuma sessao", l2);
        draw_footer("OK abre a sessao   ` volta");
        return;
    }

    clamp_window(s_sel, s_agent_count, &s_top);
    int rows = body_rows();
    if (rows > (int)(sizeof(s_row_cache) / sizeof(s_row_cache[0]))) {
        rows = (int)(sizeof(s_row_cache) / sizeof(s_row_cache[0]));
    }
    /* s_rows_drawn < 0 significa "não sei o que está no corpo": ou acabamos de
       entrar na tela, ou quem pintou por último foi o estado vazio, cujo texto
       fica no meio e não é coberto por linha nenhuma. Nos dois casos a lista
       precisa assumir o corpo limpo antes de desenhar. */
    if (s_rows_drawn < 0) {
        clear_body();
    }
    bool show_host = multi_host();
    int desenhadas = 0;
    for (int i = 0; i < rows; i++) {
        int idx = s_top + i;
        if (idx >= s_agent_count) {
            break;
        }
        desenhadas++;
        const herdr_agent_t *a = &s_agents[idx];

        char label[40];
        if (show_host && a->host < CFG_MAX_HOSTS && panel_cfg_get()->hosts[a->host].name[0]) {
            snprintf(label, sizeof(label), "%s %s/%s",
                     panel_cfg_get()->hosts[a->host].name, a->agent, a->project);
        } else {
            snprintf(label, sizeof(label), "%s/%s", a->agent, a->project);
        }

        char right[12];
        fmt_elapsed(right, sizeof(right), a->since);
        if (!right[0]) {
            strlcpy(right, a->status, sizeof(right));
        }
        int right_cols = (int)strlen(right);
        trunc_to(label, (SCR_W - 12) / 6 - right_cols - 1);

        /* A linha só vai ao display se algo nela mudou. Numa lista parada isso
           é zero escrita por segundo; com uma sessão trabalhando, uma linha. */
        /* a fase entra na assinatura só quando a linha pisca: sem isso, ou
           nada pisca, ou a lista inteira volta a ser reenviada duas vezes por
           segundo */
        const bool pisca = agent_blocked(a);
        char sig[64];
        snprintf(sig, sizeof(sig), "%d|%s|%s|%s|%d", idx == s_sel, label, right,
                 a->status, pisca ? (int)blink_on() : 0);
        if (!changed(s_row_cache[i], sizeof(s_row_cache[i]), sig)) {
            continue;
        }

        uint16_t cor = status_color(a->status);
        if (pisca && !blink_on()) {
            cor = C_BLOCKED_DIM;
        }
        auto &c = list_row(idx == s_sel);
        c.fillRect(3, 2, 3, ROW_H - 4, cor);
        c.setTextColor(idx == s_sel ? C_TEXT : rgb565(0xc8c8cc));
        c.setCursor(9, 2);
        c.print(label);
        c.setTextColor(cor);
        c.setCursor(SCR_W - 2 - right_cols * 6, 2);
        c.print(right);
        push_row(c, i);
    }
    /* encolheu (sessão fechou): limpa só o que sobrou embaixo, sem tocar no
       resto — apagar o corpo inteiro é justamente o que fazia piscar */
    if (s_rows_drawn > desenhadas) {
        int y = BODY_Y + desenhadas * ROW_H;
        M5Cardputer.Display.fillRect(0, y, SCR_W, SCR_H - FOOT_H - y, C_BG);
    }
    s_rows_drawn = desenhadas;
    draw_footer("OK abre a sessao   ` volta");
}

/* ---------------------------------------------------------------- dash --- */

static void draw_dash(void)
{
    draw_status_bar("DASH");
    if (s_limit_count == 0) {
        center_msg("sem limites", "a ponte ainda nao mandou");
        draw_footer("` volta");
        return;
    }
    clear_body();
    int y = BODY_Y;
    const int bottom = SCR_H - FOOT_H;
    for (int p = 0; p < s_limit_count && y + 9 < bottom; p++) {
        const herdr_limits_t *l = &s_limits[p];
        auto &c = rowc();
        c.fillSprite(C_BG);
        char head[40];
        snprintf(head, sizeof(head), "%s %s", l->name, l->plan);
        trunc_to(head, 30);
        c.setTextColor(l->ok ? C_TEXT : C_BLOCKED);
        c.setCursor(2, 1);
        c.print(head);
        if (!l->ok) {
            c.setTextColor(C_BLOCKED);
            c.setCursor(SCR_W - 6 * 6, 1);
            c.print("stale");
        }
        push_at(c, y, 9);
        y += 10;

        for (int r = 0; r < l->row_count && y + 9 < bottom; r++) {
            const herdr_limit_row_t *row = &l->rows[r];
            auto &b = rowc();
            b.fillSprite(C_BG);
            char lab[14];
            strlcpy(lab, row->label, sizeof(lab));
            trunc_to(lab, 9);
            b.setTextColor(C_MUTED);
            b.setCursor(8, 1);
            b.print(lab);

            const int bx = 70, bw = SCR_W - bx - 34;
            b.drawRect(bx, 1, bw, 7, C_BORDER);
            int fill = row->pct * (bw - 2) / 100;
            b.fillRect(bx + 1, 2, fill, 5, row->pct >= 80 ? C_LIMIT_HIGH : C_IDLE);
            char pct[8];
            snprintf(pct, sizeof(pct), "%3u%%", (unsigned)row->pct);
            b.setTextColor(C_TEXT);
            b.setCursor(SCR_W - 4 * 6 - 2, 1);
            b.print(pct);
            push_at(b, y, 9);
            y += 10;
        }
        y += 2;
    }
    draw_footer("` volta");
}

/* ------------------------------------------------------------ terminal --- */

static void term_request(void)
{
    if (s_term_host < 0) {
        return;
    }
    s_term_next_read = millis() + TERM_POLL_MS;
    herdr_conn_read_pane(s_term_host, s_term_pane, TERM_REQ_LINES,
                         term_cols(), s_term_rows);
}

static void term_open(const herdr_agent_t *a)
{
    if (!s_term_buf) {
        s_term_buf = (char *)malloc(HERDR_CONTENT_LEN);
    }
    if (!s_term_buf || !term_alloc(s_tiny_font)) {
        toast("sem memoria para o terminal");
        return;
    }
    s_term_host = a->host;
    strlcpy(s_term_pane, a->pane_id, sizeof(s_term_pane));
    strlcpy(s_term_agent, a->agent, sizeof(s_term_agent));
    strlcpy(s_term_project, a->project, sizeof(s_term_project));
    s_term_rows = term_rows_for(SCR_H - BODY_Y - FOOT_H);
    s_term_seq = 0;
    s_input_len = 0;
    s_input[0] = '\0';
    term_set_message("carregando...");
    s_screen = SCR_TERM;
    s_dirty = true;
    term_request();
}

static void term_close(void)
{
    if (s_term_host >= 0) {
        /* devolve a resolução do pane ao host: enquanto lemos, a sessão dele
           fica travada nas nossas 40 colunas */
        herdr_conn_release_pane(s_term_host, s_term_pane);
    }
    s_term_host = -1;
    term_free();
    free(s_term_buf);
    s_term_buf = NULL;
    s_screen = SCR_SESSIONS;   /* fechou a sessão: volta para a lista dela */
    s_dirty = true;
}

static void draw_term(void)
{
    char title[40];
    snprintf(title, sizeof(title), "%s/%s", s_term_agent, s_term_project);
    trunc_to(title, 26);
    draw_status_bar(title);

    term_draw(BODY_Y, s_term_rows);
    /* sobra entre a última linha e o rodapé: fica do fundo do terminal */
    int used = BODY_Y + s_term_rows * term_cell_h();
    if (used < SCR_H - FOOT_H) {
        M5Cardputer.Display.fillRect(0, used, SCR_W, SCR_H - FOOT_H - used, C_TERM_BG);
    }

    /* rodapé: o que está sendo digitado, ou as dicas quando vazio */
    auto &c = rowc();
    c.fillSprite(C_BG);
    c.drawFastHLine(0, 0, SCR_W, C_BORDER);
    if (millis() < s_toast_until) {
        c.setTextColor(C_ACCENT);
        c.setCursor(2, 2);
        c.print(s_toast);
    } else if (s_input_len > 0) {
        const int cols = SCR_W / 6 - 2;
        const char *view = s_input;
        if (s_input_len > cols) {
            view += s_input_len - cols;      /* a cauda é o que interessa */
        }
        c.setTextColor(C_TEXT);
        c.setCursor(2, 2);
        c.print("> ");
        c.print(view);
        c.fillRect(2 + (2 + (int)strlen(view)) * 6, 2, 5, 8, C_ACCENT);
    } else {
        c.setTextColor(C_MUTED);
        c.setCursor(2, 2);
        c.print(term_at_bottom() ? "digite | OK acoes | ctrl+;. rola"
                                 : "rolado | ctrl+. desce | OK acoes");
    }
    push_at(c, SCR_H - FOOT_H, FOOT_H);
}

/** Manda uma tecla crua para o pane (allowlist da ponte). */
static void term_key(const char *key)
{
    const char *keys[1] = { key };
    if (herdr_conn_send_keys(s_term_host, s_term_pane, keys, 1) != ESP_OK) {
        toast("host offline");
    } else {
        s_term_next_read = millis() + 300;   /* olha o resultado logo */
    }
}

/* Uma "roda de mouse" vale 3 linhas: uma por toque é lento demais para achar
   algo, e o teto da ponte é 20. */
#define TERM_SCROLL_LINES 3

/**
 * Rola o terminal.
 *
 * Duas fontes, nesta ordem. Se o snapshot recebido tem mais linhas do que
 * cabem na tela, a rolagem é local e instantânea. Quando acaba — que é o caso
 * NORMAL, porque a ponte lê o viewport e a tela trava o pane no tamanho dela —
 * quem tem o histórico é o host, e aí vai um scroll_pane: uma roda de mouse
 * de verdade sobre a célula do meio da tela. A posição importa porque uma TUI
 * só rola a região sob o ponteiro.
 */
static void term_do_scroll(bool up, int lines)
{
    if (term_scroll(up ? lines : -lines, s_term_rows)) {
        s_dirty = true;
        return;
    }
    if (herdr_conn_scroll_pane(s_term_host, s_term_pane, lines, up,
                               term_cols() / 2, s_term_rows / 2) != ESP_OK) {
        toast("host offline");
        return;
    }
    /* a rolagem só aparece na próxima leitura; não esperar os 3s cheios é a
       diferença entre "rolou" e "travou" na percepção de quem está segurando
       a tecla */
    s_term_next_read = millis() + 250;
}

static void term_submit(void)
{
    if (s_input_len > 0) {
        if (herdr_conn_send_text(s_term_host, s_term_pane, s_input) != ESP_OK) {
            toast("host offline");
            return;
        }
        s_input_len = 0;
        s_input[0] = '\0';
    }
    term_key("Enter");
}

/* ------------------------------------------------- menu da sessão aberta -- */

/* Tudo que dá para fazer numa sessão mora aqui, em lista. As combinações
   (fn+y, ctrl+c, opt+;) continuam valendo como atalho, mas quem não as conhece
   chega no mesmo lugar por enter e setas — que é o ponto. */
enum {
    TM_TYPE = 0,
    TM_ENTER,
    TM_YES,
    TM_NO,
    TM_ACCEPT,
    TM_ESC,
    TM_CTRLC,
    TM_UP,
    TM_DOWN,
    TM_TAB,
    TM_SCROLL_BACK,
    TM_SCROLL_FWD,
    TM_FOCUS,
    TM_CLOSE,
    TM_COUNT,
};

static const char *const TERM_MENU[TM_COUNT] = {
    "Digitar resposta",
    "Enviar Enter",
    "Responder sim (y)",
    "Responder nao (n)",
    "Aprovar (a)",
    "Enviar Escape",
    "Interromper (Ctrl-C)",
    "Seta para cima",
    "Seta para baixo",
    "Enviar Tab",
    "Rolar para tras",
    "Rolar para frente",
    "Focar no host",
    "Fechar sessao",
};

static void draw_term_menu(void)
{
    draw_list_menu("ACOES", TERM_MENU, TM_COUNT, s_tm_sel, &s_tm_top,
                   "OK escolhe   ` volta");
}

/** Executa o item escolhido; devolve false quando a sessão foi fechada. */
static bool term_menu_run(int item)
{
    switch (item) {
    case TM_TYPE:        break;                 /* só fecha o menu e volta a digitar */
    case TM_ENTER:       term_key("Enter");     break;
    case TM_YES:         term_key("y");         break;
    case TM_NO:          term_key("n");         break;
    case TM_ACCEPT:      term_key("a");         break;
    case TM_ESC:         term_key("Escape");    break;
    case TM_CTRLC:       term_key("C-c");       break;
    case TM_UP:          term_key("Up");        break;
    case TM_DOWN:        term_key("Down");      break;
    case TM_TAB:         term_key("Tab");       break;
    case TM_SCROLL_BACK: term_do_scroll(true, TERM_SCROLL_LINES);  break;
    case TM_SCROLL_FWD:  term_do_scroll(false, TERM_SCROLL_LINES); break;
    case TM_FOCUS:
        herdr_conn_focus(s_term_host, s_term_pane);
        toast("pane focado no host");
        break;
    case TM_CLOSE:
        term_close();
        return false;
    }
    return true;
}

static void key_term_menu(const KeyEvent &e)
{
    if (menu_move(e, &s_tm_sel, TM_COUNT)) {
        s_dirty = true;
        return;
    }
    if (e.is(KEY_ESC_CH)) {
        s_screen = SCR_TERM;
        s_dirty = true;
        return;
    }
    if (e.enter) {
        /* rolar mantém o menu aberto: rolar uma linha de cada vez, tendo que
           reabrir a lista a cada passo, seria hostil */
        bool fica = (s_tm_sel == TM_SCROLL_BACK || s_tm_sel == TM_SCROLL_FWD);
        if (!term_menu_run(s_tm_sel)) {
            return;                             /* sessão fechada: tela já trocou */
        }
        if (!fica) {
            s_screen = SCR_TERM;
        }
        s_dirty = true;
    }
}

/* ------------------------------------------------------- tela apagada ---- */

/**
 * Apaga a tela para deixar o aparelho carregando.
 *
 * Só o backlight, e não o sleep() do painel: o backlight é quase toda a energia
 * que a tela consome, então o ganho é praticamente o mesmo — e um painel que
 * não acorda direito deixaria o aparelho sem saída a não ser reiniciar. O preto
 * por baixo é para o caso de o brilho zero ainda vazar alguma coisa.
 */
static void display_off(void)
{
    s_display_off = true;
    M5Cardputer.Display.fillScreen(TFT_BLACK);
    M5Cardputer.Display.setBrightness(0);
}

static void display_on(void)
{
    s_display_off = false;
    M5Cardputer.Display.setBrightness(UI_BRIGHTNESS);
    /* o que estava desenhado não vale mais: força repintura completa */
    ui_invalidate();
    s_drawn_screen = (Screen)-1;
    s_dirty = true;
}

/* ------------------------------------------------------------ menu raiz -- */

/* enum e tabela lado a lado: com índice solto, acrescentar um item no meio
   muda o destino de todos os seguintes sem aviso do compilador */
enum {
    ROOT_SESSIONS = 0,
    ROOT_DASH,
    ROOT_SETTINGS,
    ROOT_LOGO,
    ROOT_OFF,
    ROOT_INFO,
    ROOT_COUNT,
};

static const char *const ROOT_MENU[ROOT_COUNT] = {
    "Sessoes",
    "Dashboards",
    "Configuracoes",
    "Ver a logo",
    "Desligar a tela",
    "Sobre este aparelho",
};

/* definida junto do splash, lá embaixo */
static void paint_logo(bool com_versao);

static void draw_logo_screen(void)
{
    if (!changed(s_row_cache[0], sizeof(s_row_cache[0]), "logo")) {
        return;
    }
    paint_logo(true);
}

static void draw_root(void)
{
    static int top;
    char titulo[24];
    snprintf(titulo, sizeof(titulo), "HERDR-ASSIST");
    draw_list_menu(titulo, ROOT_MENU, ROOT_COUNT, s_root_sel, &top,
                   "; . navega   OK entra");
}

static void key_root(const KeyEvent &e)
{
    if (menu_move(e, &s_root_sel, ROOT_COUNT)) {
        s_dirty = true;
        return;
    }
    if (!e.enter) {
        return;
    }
    switch (s_root_sel) {
    case ROOT_SESSIONS: s_screen = SCR_SESSIONS; break;
    case ROOT_DASH:     s_screen = SCR_DASH;     break;
    case ROOT_SETTINGS: s_screen = SCR_SETTINGS; break;
    case ROOT_LOGO:
        s_back = SCR_MENU;
        s_screen = SCR_LOGO;
        break;
    case ROOT_OFF:
        display_off();
        return;                  /* nada a redesenhar: a tela está apagada */
    default:
        s_back = SCR_MENU;
        s_screen = SCR_INFO;
        break;
    }
    s_dirty = true;
}

/* --------------------------------------------------------- configurações -- */

enum {
    MENU_WIFI = 0,
    MENU_PASS,
    MENU_PAIR,
    MENU_HOSTS,
    MENU_FONT,
    MENU_INFO,
    MENU_SAVE,
    MENU_COUNT,
};

static void menu_text(int i, char *out, size_t n)
{
    switch (i) {
    case MENU_WIFI:
        snprintf(out, n, "Wi-Fi: %s", s_edit.wifi_ssid[0] ? s_edit.wifi_ssid : "-");
        break;
    case MENU_PASS:
        snprintf(out, n, "Senha: %s", s_edit.wifi_pass[0] ? "********" : "-");
        break;
    case MENU_PAIR:
        snprintf(out, n, "Parear com um host");
        break;
    case MENU_HOSTS: {
        int n_hosts = 0;
        for (int h = 0; h < CFG_MAX_HOSTS; h++) {
            n_hosts += s_edit.hosts[h].enabled ? 1 : 0;
        }
        snprintf(out, n, "Hosts (%d)", n_hosts);
        break;
    }
    case MENU_FONT:
        snprintf(out, n, "Terminal: %s", s_tiny_font ? "60 col" : "40 col");
        break;
    case MENU_INFO:
        snprintf(out, n, "Sobre este aparelho");
        break;
    default:
        snprintf(out, n, "%s", s_cfg_dirty ? "Salvar e reiniciar *" : "Salvar e reiniciar");
        break;
    }
}

static void draw_settings(void)
{
    draw_status_bar("SETTINGS");
    clamp_window(s_menu_sel, MENU_COUNT, &s_top);
    clear_body();
    int rows = body_rows();
    for (int i = 0; i < rows && s_top + i < MENU_COUNT; i++) {
        int idx = s_top + i;
        auto &c = list_row(idx == s_menu_sel);
        char label[40];
        menu_text(idx, label, sizeof(label));
        trunc_to(label, 37);
        c.setTextColor(idx == s_menu_sel ? C_TEXT : rgb565(0xc8c8cc));
        c.setCursor(6, 2);
        c.print(label);
        push_row(c, i);
    }
    draw_footer("OK edita   ` volta");
}

/* -------------------------------------------------------------- hosts ---- */

static void draw_hosts(void)
{
    draw_status_bar("HOSTS");
    clear_body();
    for (int i = 0; i < CFG_MAX_HOSTS; i++) {
        const panel_host_t *h = &s_edit.hosts[i];
        auto &c = list_row(i == s_host_sel);
        char label[48];
        if (panel_host_is_free(h)) {
            snprintf(label, sizeof(label), "%d  <livre>", i + 1);
        } else {
            snprintf(label, sizeof(label), "%d  %s  %s", i + 1,
                     h->name[0] ? h->name : "?",
                     panel_host_is_auto(h) ? "auto" : h->host);
        }
        trunc_to(label, 33);
        c.setTextColor(h->enabled ? C_TEXT : C_MUTED);
        c.setCursor(6, 2);
        c.print(label);
        if (!panel_host_is_free(h)) {
            c.setTextColor(h->enabled ? C_IDLE : C_MUTED);
            c.setCursor(SCR_W - 3 * 6 - 2, 2);
            c.print(h->enabled ? "on" : "off");
        }
        push_row(c, i);
    }
    draw_footer("ok liga/desliga  x apaga  ` volta");
}

/* ------------------------------------------------------------ pareamento -- */

static void pair_apply(const panel_host_t *in, bool auto_mode)
{
    panel_host_t inc = *in;
    if (auto_mode) {
        /* slot auto fica sem endereço na NVS: a descoberta por broadcast o
           resolve em runtime (e o IP recebido morreria no restart) */
        inc.host[0] = '\0';
    }
    int slot = -1;
    /* reparear a mesma máquina atualiza o slot em vez de duplicar */
    for (int i = 0; i < CFG_MAX_HOSTS && slot < 0; i++) {
        if (s_edit.hosts[i].name[0] && strcmp(s_edit.hosts[i].name, inc.name) == 0) {
            slot = i;
        }
    }
    for (int i = 0; i < CFG_MAX_HOSTS && slot < 0; i++) {
        if (inc.host[0] && s_edit.hosts[i].host[0] &&
            strcmp(s_edit.hosts[i].host, inc.host) == 0) {
            slot = i;
        }
    }
    for (int i = 0; i < CFG_MAX_HOSTS && slot < 0; i++) {
        if (panel_host_is_free(&s_edit.hosts[i])) {
            slot = i;
        }
    }
    if (slot < 0) {
        toast("sem slot livre");
        return;
    }
    s_edit.hosts[slot] = inc;
    panel_cfg_save(&s_edit);
    M5Cardputer.Display.fillScreen(C_BG);
    M5Cardputer.Display.setTextDatum(middle_center);
    M5Cardputer.Display.setTextColor(C_IDLE, C_BG);
    M5Cardputer.Display.drawString("pareado! reiniciando...", SCR_W / 2, SCR_H / 2);
    delay(1200);
    ESP.restart();
}

static void draw_pair(void)
{
    draw_status_bar("PAIR");
    clear_body();
    auto &d = M5Cardputer.Display;
    d.setTextDatum(middle_center);
    d.setTextColor(C_MUTED, C_BG);
    d.setFont(&fonts::Font0);
    d.drawString("este aparelho", SCR_W / 2, BODY_Y + 12);
    d.setTextColor(C_TEXT, C_BG);
    d.setTextSize(3);
    d.drawString(pairing_device_id(), SCR_W / 2, BODY_Y + 40);
    d.setTextSize(1);

    char line[48];
    if (!net_wifi_is_up()) {
        strlcpy(line, "sem Wi-Fi", sizeof(line));
    } else if (pairing_state() == PAIRING_WAITING) {
        snprintf(line, sizeof(line), "aguardando o host (%ds)", pairing_seconds_left());
    } else {
        strlcpy(line, "janela fechada - ok reabre", sizeof(line));
    }
    d.setTextColor(C_MUTED, C_BG);
    d.drawString(line, SCR_W / 2, BODY_Y + 72);
    d.setTextDatum(top_left);
    draw_footer("no host: herdr plugin ... admin, tecla p");
}

/* ------------------------------------------------------------- Wi-Fi ----- */

static void wifi_scan_now(void)
{
    center_msg("procurando redes...", NULL);
    s_ap_count = net_wifi_scan(s_aps, MAX_APS);
    if (s_ap_count < 0) {
        s_ap_count = 0;
    }
    s_ap_sel = s_ap_top = 0;
    s_dirty = true;
}

static void draw_wifi(void)
{
    draw_status_bar("WI-FI");
    if (s_ap_count == 0) {
        center_msg("nenhuma rede", "ok procura de novo");
        draw_footer("ok busca  ` volta");
        return;
    }
    clamp_window(s_ap_sel, s_ap_count, &s_ap_top);
    clear_body();
    int rows = body_rows();
    for (int i = 0; i < rows && s_ap_top + i < s_ap_count; i++) {
        int idx = s_ap_top + i;
        auto &c = list_row(idx == s_ap_sel);
        char label[40];
        snprintf(label, sizeof(label), "%s%s", s_aps[idx].secure ? "" : "(aberta) ",
                 s_aps[idx].ssid);
        trunc_to(label, 30);
        c.setTextColor(idx == s_ap_sel ? C_TEXT : rgb565(0xc8c8cc));
        c.setCursor(6, 2);
        c.print(label);
        char dbm[8];
        snprintf(dbm, sizeof(dbm), "%d", s_aps[idx].rssi);
        c.setTextColor(C_MUTED);
        c.setCursor(SCR_W - (int)strlen(dbm) * 6 - 2, 2);
        c.print(dbm);
        push_row(c, i);
    }
    draw_footer("OK escolhe   ` volta");
}

/* -------------------------------------------------------------- sobre ---- */

static void draw_info(void)
{
    draw_status_bar("SOBRE");
    clear_body();
    char lines[6][44];
    uint8_t mac[6] = { 0 };
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    int8_t rssi = 0;
    bool have_rssi = net_wifi_rssi(&rssi);
    /* 28 caracteres no valor: com o rótulo "versao" a linha estouraria os
       240px, então aqui vai só o valor */
    snprintf(lines[0], sizeof(lines[0]), "%s", HERDR_ASSIST_VERSION);
    /* mesmo id que o pareamento anuncia (3 últimos bytes do MAC); aqui é
       calculado direto porque o pairing só o preenche quando é ligado */
    snprintf(lines[1], sizeof(lines[1]), "id      %02X%02X%02X", mac[3], mac[4], mac[5]);
    snprintf(lines[2], sizeof(lines[2]), "ip      %s",
             net_wifi_is_up() ? WiFi.localIP().toString().c_str() : "-");
    snprintf(lines[3], sizeof(lines[3]), "mac     %02x:%02x:%02x:%02x:%02x:%02x",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    if (have_rssi) {
        snprintf(lines[4], sizeof(lines[4]), "sinal   %d dBm", rssi);
    } else {
        snprintf(lines[4], sizeof(lines[4]), "sinal   -");
    }
    snprintf(lines[5], sizeof(lines[5]), "heap    %u KB",
             (unsigned)(ESP.getFreeHeap() / 1024));
    for (int i = 0; i < 6; i++) {
        auto &c = rowc();
        c.fillSprite(C_BG);
        c.setTextColor(C_TEXT);
        c.setCursor(6, 2);
        c.print(lines[i]);
        push_row(c, i);
    }

    /* Crédito no fim da lista, separado por um filete: é assinatura, não mais
       um dado do aparelho, e misturá-lo com ip e mac apagaria essa diferença. */
    {
        auto &c = rowc();
        c.fillSprite(C_BG);
        /* filete rente ao topo e texto em y=3: com 8px de fonte, ocupa 3..10 e
           cabe nos 11 da linha. Em y=6 ele ia até 13 e era cortado pelo clip —
           o que sobrava a linha de baixo comia. */
        c.drawFastHLine(6, 0, SCR_W - 12, C_BORDER);
        c.setTextColor(C_MUTED);
        c.setCursor(6, 3);
        c.print("desenvolvido por");
        push_row(c, 6);
    }
    {
        auto &c = rowc();
        c.fillSprite(C_BG);
        c.setTextColor(C_ACCENT);
        c.setCursor(6, 2);
        c.print("www.nimbcorp.com.br");
        c.setTextColor(C_MUTED);
        c.setCursor(SCR_W - 5 * 6 - 4, 2);
        c.print("1.0.0");
        push_row(c, 7);
    }
    draw_footer("` volta");
}

/* --------------------------------------------------------- caixa de texto - */

/* Sempre em texto claro, inclusive senha: num teclado de 56 teclas usado pela
   primeira vez, nao poder conferir o que se digitou custa mais que o risco de
   alguem ler por cima do ombro. */
static void input_open(const char *title, const char *initial,
                       void (*done)(const char *))
{
    s_in_title = title;
    s_in_done = done;
    strlcpy(s_in_buf, initial ? initial : "", sizeof(s_in_buf));
    s_back = s_screen;
    s_screen = SCR_INPUT;
    s_dirty = true;
}

static void draw_input(void)
{
    draw_status_bar(s_in_title);
    clear_body();
    auto &d = M5Cardputer.Display;
    d.fillRoundRect(6, BODY_Y + 24, SCR_W - 12, 22, 3, C_PANEL);
    d.drawRoundRect(6, BODY_Y + 24, SCR_W - 12, 22, 3, C_BORDER);

    char shown[CFG_PASS_LEN];
    strlcpy(shown, s_in_buf, sizeof(shown));
    const int cols = (SCR_W - 24) / 6;
    const char *view = shown;
    int len = (int)strlen(shown);
    if (len > cols) {
        view += len - cols;
        len = cols;
    }
    d.setFont(&fonts::Font0);
    d.setTextColor(C_TEXT, C_PANEL);
    d.setCursor(12, BODY_Y + 31);
    d.print(view);
    d.fillRect(12 + len * 6, BODY_Y + 30, 5, 9, C_ACCENT);
    draw_footer("ok confirma  ` cancela  del apaga");
}

/* -------------------------------------------------------------- teclas --- */

static void apply_ssid(const char *v)
{
    strlcpy(s_edit.wifi_ssid, v, sizeof(s_edit.wifi_ssid));
    s_cfg_dirty = true;
}

static void apply_pass(const char *v)
{
    strlcpy(s_edit.wifi_pass, v, sizeof(s_edit.wifi_pass));
    s_cfg_dirty = true;
}

static void save_and_reboot(void)
{
    panel_cfg_save(&s_edit);
    Preferences p;
    p.begin("cardp", false);
    p.putBool("tiny", s_tiny_font);
    p.end();
    M5Cardputer.Display.fillScreen(C_BG);
    M5Cardputer.Display.setTextDatum(middle_center);
    M5Cardputer.Display.setTextColor(C_IDLE, C_BG);
    M5Cardputer.Display.drawString("salvo, reiniciando...", SCR_W / 2, SCR_H / 2);
    delay(900);
    ESP.restart();
}

static void key_sessions(const KeyEvent &e)
{
    if (e.up() && s_sel > 0) {
        s_sel--;
        s_dirty = true;
    } else if (e.down() && s_sel + 1 < s_agent_count) {
        s_sel++;
        s_dirty = true;
    } else if (e.enter && s_agent_count > 0) {
        term_open(&s_agents[s_sel]);
    }
}

static void key_settings(const KeyEvent &e)
{
    if (e.up() && s_menu_sel > 0) {
        s_menu_sel--;
        s_dirty = true;
    } else if (e.down() && s_menu_sel + 1 < MENU_COUNT) {
        s_menu_sel++;
        s_dirty = true;
    } else if (e.enter) {
        switch (s_menu_sel) {
        case MENU_WIFI:
            s_back = SCR_SETTINGS;
            s_screen = SCR_WIFI;
            wifi_scan_now();
            break;
        case MENU_PASS:
            input_open("SENHA WI-FI", s_edit.wifi_pass, apply_pass);
            break;
        case MENU_PAIR:
            s_back = SCR_SETTINGS;
            s_screen = SCR_PAIR;
            pairing_start();
            s_dirty = true;
            break;
        case MENU_HOSTS:
            s_back = SCR_SETTINGS;
            s_screen = SCR_HOSTS;
            s_dirty = true;
            break;
        case MENU_FONT:
            s_tiny_font = !s_tiny_font;
            s_cfg_dirty = true;
            s_dirty = true;
            break;
        case MENU_INFO:
            s_back = SCR_SETTINGS;
            s_screen = SCR_INFO;
            s_dirty = true;
            break;
        default:
            save_and_reboot();
            break;
        }
    }
}

static void key_hosts(const KeyEvent &e)
{
    if (e.up() && s_host_sel > 0) {
        s_host_sel--;
    } else if (e.down() && s_host_sel + 1 < CFG_MAX_HOSTS) {
        s_host_sel++;
    } else if (e.enter) {
        panel_host_t *h = &s_edit.hosts[s_host_sel];
        if (!panel_host_is_free(h)) {
            h->enabled = !h->enabled;
            s_cfg_dirty = true;
        }
    } else if (e.is('x')) {
        memset(&s_edit.hosts[s_host_sel], 0, sizeof(panel_host_t));
        s_cfg_dirty = true;
        toast("slot apagado (salve para valer)");
    } else {
        return;
    }
    s_dirty = true;
}

static void key_wifi(const KeyEvent &e)
{
    if (e.up() && s_ap_sel > 0) {
        s_ap_sel--;
        s_dirty = true;
    } else if (e.down() && s_ap_sel + 1 < s_ap_count) {
        s_ap_sel++;
        s_dirty = true;
    } else if (e.enter) {
        if (s_ap_count == 0) {
            wifi_scan_now();
        } else {
            apply_ssid(s_aps[s_ap_sel].ssid);
            s_screen = SCR_SETTINGS;
            input_open("SENHA WI-FI", s_edit.wifi_pass, apply_pass);
        }
    }
}

static void key_input(const KeyEvent &e)
{
    size_t len = strlen(s_in_buf);
    if (e.enter) {
        if (s_in_done) {
            s_in_done(s_in_buf);
        }
        s_screen = s_back;
    } else if (e.del) {
        if (len) {
            s_in_buf[len - 1] = '\0';
        }
    } else if (e.is(KEY_ESC_CH)) {
        s_screen = s_back;
    } else if (e.ch >= ' ' && e.ch < 0x7f && len + 1 < sizeof(s_in_buf)) {
        s_in_buf[len] = e.ch;
        s_in_buf[len + 1] = '\0';
    } else {
        return;
    }
    s_dirty = true;
}

/**
 * Teclado da sessão aberta.
 *
 * A regra é uma só: digitar compõe uma linha (que sai como texto no Enter) e
 * fn manda a tecla crua na hora — é com fn que se responde um menu do agente
 * (fn+1, fn+y) ou se navega nele (as setas estão serigrafadas em ; . , /).
 * Rolar o histórico é opt+;/., para não roubar as setas do agente.
 */
static void key_term(const KeyEvent &e)
{
    if (e.ctrl && e.is('c')) {
        term_key("C-c");
        return;
    }
    if (e.ctrl && e.is('f')) {
        herdr_conn_focus(s_term_host, s_term_pane);
        toast("pane focado no host");
        return;
    }
    /* ctrl+seta rola. NÃO pode ser shift: no Cardputer `shift+;` é como se
       digita ':' — e '<', '>', '?' saem das outras três setas. Roubar shift
       aqui custaria caracteres que aparecem em qualquer URL ou trecho de
       código. ctrl não mexe no caractere, então não tira nada de ninguém.
       opt fica como segundo caminho. */
    if ((e.ctrl || e.opt) && e.arrow()) {
        if (e.up() || e.down()) {
            term_do_scroll(e.up(), TERM_SCROLL_LINES);
        } else {
            /* esquerda/direita: salto de uma tela, para varrer histórico longo */
            term_do_scroll(e.left(), s_term_rows);
        }
        return;
    }
    if (e.fn) {
        if (e.up())         term_key("Up");
        else if (e.down())  term_key("Down");
        else if (e.left())  term_key("Left");
        else if (e.right()) term_key("Right");
        else if (e.is(KEY_ESC_CH))   term_key("Escape");
        else if (e.enter)            term_key("Enter");
        else if (e.del)              term_key("BSpace");
        else if (e.ch == 'y' || e.ch == 'n' || e.ch == 'a' ||
                 (e.ch >= '0' && e.ch <= '9')) {
            char k[2] = { e.ch, '\0' };
            term_key(k);
        }
        return;
    }
    if (e.enter) {
        if (s_input_len == 0) {
            /* linha vazia: enter é a porta do menu, não um Enter perdido no
               agente. Com texto digitado ele envia, que é o caso frequente. */
            s_tm_sel = 0;
            s_tm_top = 0;
            s_screen = SCR_TERMMENU;
        } else {
            term_submit();
        }
        s_dirty = true;
        return;
    }
    if (e.tab) {
        term_key("Tab");
        return;
    }
    if (e.del) {
        if (s_input_len > 0) {
            s_input[--s_input_len] = '\0';
            s_dirty = true;
        } else {
            term_key("BSpace");
        }
        return;
    }
    if (e.is(KEY_ESC_CH) && s_input_len == 0) {
        term_close();
        return;
    }
    if (e.ch >= ' ' && e.ch < 0x7f && s_input_len + 1 < (int)sizeof(s_input)) {
        s_input[s_input_len++] = e.ch;
        s_input[s_input_len] = '\0';
        s_dirty = true;
    }
}

static void handle_key(const KeyEvent &e)
{
    /* telas de conteúdo tratam tudo por conta própria */
    if (s_screen == SCR_TERM) {
        key_term(e);
        return;
    }
    if (s_screen == SCR_TERMMENU) {
        key_term_menu(e);
        return;
    }
    if (s_screen == SCR_INPUT) {
        key_input(e);
        return;
    }
    if (s_screen == SCR_MENU) {
        key_root(e);
        return;
    }

    if (e.is(KEY_ESC_CH)) {
        if (s_screen == SCR_PAIR) {
            pairing_stop();
        }
        /* das telas de topo o esc sobe para a raiz; das sub-telas, volta de
           onde veio. Nunca fica sem saída. */
        s_screen = (s_screen >= SCR_TAB_FIRST && s_screen <= SCR_TAB_LAST)
                   ? SCR_MENU : s_back;
        s_dirty = true;
        return;
    }
    /* esquerda/direita percorre as telas de topo: atalho para quem já sabe */
    if (s_screen >= SCR_TAB_FIRST && s_screen <= SCR_TAB_LAST &&
        (e.left() || e.right())) {
        int n = (int)s_screen + (e.right() ? 1 : -1);
        if (n < SCR_TAB_FIRST) {
            n = SCR_TAB_LAST;
        }
        if (n > SCR_TAB_LAST) {
            n = SCR_TAB_FIRST;
        }
        s_screen = (Screen)n;
        s_dirty = true;
        return;
    }

    if (s_screen == SCR_LOGO) {
        s_screen = s_back;       /* qualquer tecla volta; esc já veio acima */
        s_dirty = true;
        return;
    }

    switch (s_screen) {
    case SCR_SESSIONS: key_sessions(e); break;
    case SCR_SETTINGS: key_settings(e); break;
    case SCR_HOSTS:    key_hosts(e);    break;
    case SCR_WIFI:     key_wifi(e);     break;
    case SCR_PAIR:
        if (e.enter && pairing_state() != PAIRING_WAITING) {
            pairing_start();
            s_dirty = true;
        }
        break;
    default:
        break;
    }
}

/* ---------------------------------------------------------------- laço --- */

static void draw(void)
{
    if (s_screen != s_drawn_screen) {
        ui_invalidate();
        s_drawn_screen = s_screen;
        /* fundo limpo uma vez, na troca: daqui em diante cada peça cuida da
           própria área */
        M5Cardputer.Display.fillRect(0, BODY_Y, SCR_W, SCR_H - BODY_Y, C_BG);
    }
    switch (s_screen) {
    case SCR_MENU:     draw_root();     break;
    case SCR_TERMMENU: draw_term_menu(); break;
    case SCR_SESSIONS: draw_sessions(); break;
    case SCR_DASH:     draw_dash();     break;
    case SCR_SETTINGS: draw_settings(); break;
    case SCR_TERM:     draw_term();     break;
    case SCR_PAIR:     draw_pair();     break;
    case SCR_WIFI:     draw_wifi();     break;
    case SCR_HOSTS:    draw_hosts();    break;
    case SCR_INFO:     draw_info();     break;
    case SCR_LOGO:     draw_logo_screen(); break;
    case SCR_INPUT:    draw_input();    break;
    default:           draw_sessions(); break;
    }
}

/* --------------------------------------------------------------- splash -- */

static uint32_t s_splash_at;

/* Cinza da versão sobre o papel: C_MUTED foi escolhido para o fundo quase
   preto e some no branco. */
#define C_LOGO_VERSAO rgb565(0x55555c)

/**
 * Pinta a logo centralizada, fiel ao original: traço preto sobre papel branco.
 *
 * NÃO inverter. A arte tem 42% de tinta e essa tinta é a própria figura; com as
 * cores trocadas a figura vira uma massa clara e o traço interno (que no
 * original é o branco entre os pretos) some no fundo escuro. Fica um borrão.
 *
 * O papel toma a tela inteira. Recortado no traço, o desenho tem proporção 4:3
 * contra os 16:9 do display, então sobram ~39px de cada lado; pintá-los de
 * branco também é o que evita a borda do "cartão" cruzando a ponta da faixa,
 * que fica com cara de arte cortada.
 *
 * A arte já vem do gen_logo.py no tamanho final e com anti-alias — aqui não se
 * escala nada, só se traduz cinza para RGB565.
 */
static void paint_logo(bool com_versao)
{
    auto &d = M5Cardputer.Display;
    d.fillScreen(TFT_WHITE);                   /* o papel */

    const int reserva = com_versao ? 13 : 0;   /* linha da versão embaixo */
    const int x0 = (SCR_W - LOGO_BOOT_W) / 2;
    const int y0 = (SCR_H - reserva - LOGO_BOOT_H) / 2;

    /* Uma linha por vez: 162 pixels em RGB565 são 324 bytes de pilha, contra os
       ~40KB que a imagem inteira convertida custaria de RAM. */
    uint16_t linha[LOGO_BOOT_W];
    d.startWrite();
    for (int y = 0; y < LOGO_BOOT_H; y++) {
        const uint8_t *origem = logo_boot + (size_t)y * LOGO_BOOT_W;
        for (int x = 0; x < LOGO_BOOT_W; x++) {
            const uint8_t v = origem[x];       /* 0 = tinta, 255 = papel */
            linha[x] = (uint16_t)(((v & 0xf8) << 8) | ((v & 0xfc) << 3) | (v >> 3));
        }
        d.pushImage(x0, y0 + y, LOGO_BOOT_W, 1, linha);
    }
    d.endWrite();

    if (com_versao) {
        d.setFont(&fonts::Font0);
        d.setTextDatum(bottom_center);
        d.setTextColor(C_LOGO_VERSAO, TFT_WHITE);
        d.drawString(HERDR_ASSIST_VERSION, SCR_W / 2, SCR_H - 2);
        d.setTextDatum(top_left);
    }
}

void ui_splash(void)
{
    M5Cardputer.Display.setRotation(1);
    paint_logo(true);
    s_splash_at = millis();
}

void ui_splash_hold(uint32_t min_ms)
{
    uint32_t passou = millis() - s_splash_at;
    if (passou < min_ms) {
        delay(min_ms - passou);
    }
}

void ui_init(void)
{
    s_edit = *panel_cfg_get();
    Preferences p;
    p.begin("cardp", true);
    s_tiny_font = p.getBool("tiny", false);
    p.end();

    M5Cardputer.Display.setRotation(1);
    M5Cardputer.Display.setFont(&fonts::Font0);
    M5Cardputer.Display.setTextWrap(false);
    M5Cardputer.Display.fillScreen(C_BG);

    s_rowc.setColorDepth(16);
    s_rowc.setTextWrap(false);
    s_rowc.createSprite(SCR_W, ROWC_H);
    /* sem Wi-Fi não há o que listar: já abre onde se resolve isso */
    s_screen = panel_cfg_wifi_ok() ? SCR_MENU : SCR_SETTINGS;
    refresh_agents();
    draw();
    s_dirty = false;
}

void ui_tick(void)
{
    KeyEvent ev;
    if (keys_poll(&ev)) {
        if (s_display_off) {
            /* a tecla que acorda não age: quem apagou a tela para carregar não
               quer que o primeiro toque no bolso abra uma sessão */
            display_on();
        } else {
            handle_key(ev);
        }
    }

    /* dados novos da ponte (agentes, limites, estado da conexão) */
    uint32_t gen = herdr_model_generation();
    if (gen != s_gen) {
        s_gen = gen;
        refresh_agents();
        s_dirty = true;
    }

    if (s_screen == SCR_TERM) {
        int host = -1;
        char pane[HERDR_ID_LEN];
        if (s_term_buf &&
            herdr_model_get_pane_content(pane, sizeof(pane), &host, s_term_buf,
                                         HERDR_CONTENT_LEN, &s_term_seq) &&
            host == s_term_host && strcmp(pane, s_term_pane) == 0) {
            term_set_content(s_term_buf);
            s_dirty = true;
        }
        if ((int32_t)(millis() - s_term_next_read) >= 0) {
            term_request();
        }
    }

    if (s_screen == SCR_PAIR) {
        panel_host_t h;
        bool auto_mode = false;
        if (pairing_state() == PAIRING_DONE && pairing_result(&h, &auto_mode)) {
            pair_apply(&h, auto_mode);
        }
        s_dirty = true;      /* o contador regressivo anda sozinho */
    }

    /* o pisca das bloqueadas: custa repintura só enquanto existir uma. Numa
       lista sem bloqueio, nada disto marca a tela como suja. */
    static bool s_blink_last;
    bool fase = blink_on();
    if (fase != s_blink_last) {
        s_blink_last = fase;
        if (s_screen == SCR_SESSIONS) {
            for (int i = 0; i < s_agent_count; i++) {
                if (agent_blocked(&s_agents[i])) {
                    s_dirty = true;
                    break;
                }
            }
        }
    }

    /* o relógio e o tempo decorrido das sessões viram a cada segundo */
    time_t now = time(NULL);
    if (now != s_last_min) {
        s_last_min = now;
        if (s_screen == SCR_SESSIONS || s_screen == SCR_TERM) {
            s_dirty = true;
        }
    }

    if (s_dirty && !s_display_off) {
        s_dirty = false;
        draw();
    }
}
