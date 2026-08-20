/**
 * @file
 * @brief Textos da interface em pt_BR e en_US.
 *
 * A lista `I18N_STRINGS` é a única fonte: dela saem o enum das chaves (aqui) e
 * a tabela de traduções (`i18n.c`), então não existe o caso clássico de as duas
 * saírem de sincronia. Cada linha traz as duas traduções lado a lado, que é o
 * que torna revisável acrescentar um texto novo.
 *
 * O idioma é escolhido uma vez no boot, a partir da NVS, e não muda em runtime:
 * trocar de idioma na tela de configurações salva e reinicia, como toda mudança
 * de config. Isso dispensa reconstruir as telas já montadas.
 *
 * As tabelas são `const char *const` — ficam em flash, sem custo de RAM.
 */

#pragma once

#include <lvgl.h>   /* LV_SYMBOL_*, usados no meio de algumas frases */

#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Ordem é a da NVS: 0 é o padrão de fábrica. */
typedef enum {
    LANG_EN = 0,
    LANG_PT,
    LANG_COUNT,
} ui_lang_t;

/* X(chave, en_US, pt_BR) */
#define I18N_STRINGS(X)                                                                      \
    /* --- status dos agentes (vêm crus da ponte) --- */                                     \
    X(ST_WORKING,      "working",                     "trabalhando")                         \
    X(ST_BLOCKED,      "blocked",                     "bloqueado")                           \
    X(ST_IDLE,         "idle",                        "ocioso")                              \
    X(ST_DONE,         "done",                        "concluído")                           \
    /* --- home --- */                                                                       \
    X(SYNCING,         "syncing",                     "sincronizando")                       \
    X(AGENT_ONE,       "agent",                       "agente")                              \
    X(AGENT_MANY,      "agents",                      "agentes")                             \
    X(ONLINE,          "Online",                      "Online")                              \
    X(CONNECTING,      "Connecting",                  "Conectando")                          \
    X(OFFLINE,         "Offline",                     "Offline")                             \
    X(NO_HOSTS,        "No hosts configured.",        "Nenhum host configurado.")            \
    X(TAP_TO_ADD,      "Tap " LV_SYMBOL_SETTINGS " to add one.",                             \
                       "Toque em " LV_SYMBOL_SETTINGS " para cadastrar.")                    \
    /* --- sessões --- */                                                                    \
    X(TAB_SESSIONS,    "Sessions",                    "Sessões")                             \
    X(NO_SESSIONS,     "No active sessions.",         "Nenhuma sessão ativa.")               \
    X(WAITING_BRIDGE,  "Waiting for the bridge...",   "Aguardando a ponte...")               \
    /* --- dash --- */                                                                       \
    X(TAB_DASH,        "Dashboards",                  "Dashboards")                          \
    X(NO_USAGE,        "No usage data yet.",          "Sem dados de uso ainda.")             \
    X(STALE,           "not updated",                 "sem atualizar")                       \
    X(STALE_SINCE,     "not updated since %s",        "sem atualizar desde %s")              \
    X(COST_TITLE,      "Cost · estimate",             "Custo · estimativa")                  \
    X(COST_NOW,        "Now",                         "Agora")                               \
    X(COST_WEEK,       "Week",                        "Semana")                              \
    X(COST_LIFE,       "Lifetime",                    "Vitalício")                           \
    /* --- detalhe da sessão --- */                                                          \
    X(LOADING,         "loading...",                  "carregando...")                       \
    X(PROMPT_PH,       "Prompt for the agent...",     "Prompt para o agente...")             \
    /* --- configurações: tela principal --- */                                              \
    X(TAB_ACTIVITY,    "Activity",                    "Atividade")                           \
    X(NO_ACTIVITY,     "No activity yet.",            "Nada por aqui ainda.")                \
    X(EV_STARTED,      "started",                     "começou")                             \
    X(EV_BLOCKED,      "blocked",                     "bloqueou")                            \
    X(EV_DONE,         "done",                        "terminou")                            \
    X(TODAY,           "Today",                       "Hoje")                                \
    X(TAB_SETTINGS,    "Settings",                    "Configurações")                       \
    X(SEC_WIFI,        "Wi-Fi network",               "Rede Wi-Fi")                          \
    X(NOT_CONFIGURED,  "Not configured",              "Não configurada")                     \
    X(WIFI_CONNECTED,  "Connected",                   "Conectado")                           \
    X(WIFI_DISCONNECTED, "Disconnected",              "Desconectado")                        \
    X(WIFI_SIGNAL_FMT, "Connected · %d dBm (%s)",     "Conectado · %d dBm (%s)")             \
    X(WIFI_GOOD,       "good",                        "bom")                                 \
    X(WIFI_FAIR,       "fair",                        "razoável")                            \
    X(WIFI_WEAK,       "weak",                        "fraco")                               \
    X(CHANGE,          "Change " LV_SYMBOL_RIGHT,     "Trocar " LV_SYMBOL_RIGHT)             \
    X(SEC_HOSTS,       "herdr hosts",                 "Hosts herdr")                         \
    X(PAIR_WITH_HOST,  LV_SYMBOL_WIFI "  Pair with a host",                                  \
                       LV_SYMBOL_WIFI "  Parear com um host")                                \
    X(ADD_MANUALLY,    LV_SYMBOL_PLUS "  Add manually",                                      \
                       LV_SYMBOL_PLUS "  Adicionar manualmente")                             \
    X(SEC_DEVICE,      "Device",                      "Dispositivo")                         \
    X(LANGUAGE,        "Language",                    "Idioma")                              \
    X(ORIENTATION,     "Orientation",                 "Orientação")                          \
    X(ORIENT_PORTRAIT, "Portrait",                    "Retrato")                             \
    X(ORIENT_LANDSCAPE, "Landscape",                  "Paisagem")                            \
    X(GOKU_MODE,       "Goku power",                  "Goku: poder")                         \
    X(GOKU_ASC,        "Ascending",                   "Ascendente")                          \
    X(GOKU_DESC,       "Descending",                  "Descendente")                         \
    X(RESTART_DEVICE,  "Restart device",              "Reiniciar dispositivo")               \
    X(PENDING_TITLE,   "Pending changes",             "Configurações pendentes")             \
    X(PENDING_SUB,     "Tap to apply and restart",    "Toque para aplicar e reiniciar")      \
    X(RESTARTING,      "Restarting...",               "Reiniciando...")                      \
    /* --- configurações: Wi-Fi --- */                                                       \
    X(WIFI_NETWORKS,   "Wi-Fi networks",              "Redes Wi-Fi")                         \
    X(SCANNING,        "Scanning networks...",        "Buscando redes...")                   \
    X(NO_NETWORKS,     "No networks found.",          "Nenhuma rede encontrada.")            \
    X(SCAN_FAILED,     "Failed to scan networks.",    "Falha ao buscar redes.")              \
    X(RETRY,           "Try again",                   "Tentar de novo")                      \
    X(PASSWORD,        "Password",                    "Senha")                               \
    X(NETWORK_FMT,     "Network: %s",                 "Rede: %s")                            \
    X(PASSWORD_PH,     "Password (empty if open)",    "Senha (vazio se aberta)")             \
    /* --- configurações: editar host --- */                                                 \
    X(EDIT_HOST,       "Edit host",                   "Editar host")                         \
    X(FIELD_NAME,      "Name",                        "Nome")                                \
    X(FIELD_NAME_PH,   "e.g. mac",                    "ex: mac")                             \
    X(FIELD_ADDR,      "Address",                     "Endereço")                            \
    X(FIELD_ADDR_PH,   "IP or hostname",              "IP ou hostname")                      \
    X(FIELD_PORT,      "Port",                        "Porta")                               \
    X(FIELD_TOKEN,     "Token",                       "Token")                               \
    X(FIELD_TOKEN_PH,  "32 hex from the bridge",      "32 hex da ponte")                     \
    X(FIELD_AUTO,      "Auto discovery",              "Descoberta automática")               \
    X(AUTO_SHORT,      "auto",                        "auto")                                \
    X(REMOVE,          LV_SYMBOL_TRASH "  Remove",    LV_SYMBOL_TRASH "  Remover")           \
    /* --- configurações: pareamento --- */                                                  \
    X(PAIR_TITLE,      "Pair",                        "Parear")                              \
    X(THIS_PANEL,      "This panel",                  "Este painel")                         \
    X(PICK_CODE,       "Pick this code on the host",  "Escolha este código no host")         \
    X(STARTING,        "Starting...",                 "Iniciando...")                        \
    X(PAIR_STEPS_INSTALL, "On the host (the machine running Herdr, on the same "             \
                       "Wi-Fi network as the panel):\n\n"                                    \
                       "1. Install the bridge plugin — only once:",                          \
                       "No host (a máquina com o Herdr, na mesma rede Wi-Fi "                \
                       "do painel):\n\n"                                                     \
                       "1. Instale o plugin da ponte — uma vez só:")                         \
    X(PAIR_STEPS_ADMIN, "2. In a Herdr pane, open the admin screen:",                        \
                       "2. Num pane do Herdr, abra a tela de administração:")                 \
    X(PAIR_STEPS_PICK, "3. Press p (Pair panel) and pick the code shown above "              \
                       "from the list.\n\n"                                                  \
                       "The host sends name, address and token — nothing is "                \
                       "typed on the panel. Windows or SSH without a TUI: "                  \
                       "python3 pair.py (see the manual).",                                  \
                       "3. Tecle p (Parear painel) e escolha na lista o código "             \
                       "mostrado acima.\n\n"                                                 \
                       "O host envia nome, endereço e token — nada é digitado "              \
                       "no painel. Windows ou SSH sem TUI: python3 pair.py "                 \
                       "(veja o manual).")                                                   \
    X(PAIR_NO_WIFI,    "No Wi-Fi: connect to a network first (Settings " LV_SYMBOL_RIGHT     \
                       " Wi-Fi network).",                                                   \
                       "Sem Wi-Fi: conecte a uma rede antes (Configurações "                 \
                       LV_SYMBOL_RIGHT " Rede Wi-Fi).")                                      \
    X(PAIR_MANUAL,     "Full manual — point your phone camera here:",                        \
                       "Manual completo — aponte a câmera do celular:")                      \
    X(PAIR_MANUAL_URL, "https://github.com/walcew/herdr-assist/blob/main/docs/pairing.md",   \
                       "https://github.com/walcew/herdr-assist/blob/main/docs/pairing.pt-BR.md") \
    X(PAIR_PORT_FAIL,  "Could not open the pairing port.",                                   \
                       "Não foi possível abrir a porta de pareamento.")                      \
    X(PAIR_NO_SLOT,    "No room: remove a host before pairing.",                             \
                       "Sem espaço: remova um host antes de parear.")                        \
    X(PAIRED_FMT,      "Paired with %s.\nRestarting...",                                     \
                       "Pareado com %s.\nReiniciando...")                                    \
    X(PAIR_WAITING,    "Waiting for a host... (%ds)",                                        \
                       "Aguardando um host... (%ds)")                                        \
    X(PAIR_CLOSED,     "Window closed. Go back and try again.",                              \
                       "Janela encerrada. Volte e tente de novo.")                           \
    /* --- configurações: atualização de firmware --- */                                     \
    X(FW_ROW,          "Firmware",                    "Firmware")                            \
    X(FW_UPDATE,       "Update firmware",             "Atualizar firmware")                  \
    X(FW_CHECKING,     "Checking...",                 "Verificando...")                      \
    X(FW_UP_TO_DATE,   "Up to date.",                 "Atualizado.")                         \
    X(FW_AVAILABLE_FMT, "%s available",               "%s disponível")                       \
    X(FW_CHECK_NOW,    "Check now",                   "Verificar agora")                     \
    X(FW_INSTALL,      "Download and install",        "Baixar e instalar")                   \
    X(FW_DOWNLOADING_FMT, "Downloading... %d%%",      "Baixando... %d%%")                    \
    X(FW_DONE,         "Installed. Restarting...",    "Instalado. Reiniciando...")           \
    X(FW_ERR_CHECK,    "Check failed. Is Wi-Fi up?",  "Falha ao verificar. Wi-Fi ok?")       \
    X(FW_ERR_DOWNLOAD, "Download failed. Try again.", "Falha no download. Tente de novo.")   \
    X(FW_TOAST_SUB,    "Tap to update",               "Toque para atualizar")                 \
    /* --- configurações: bloqueio de tela --- */                                             \
    X(LOCK_ROW,        "Screen lock",                 "Bloqueio de tela")                     \
    X(LOCK_ON,         "On",                          "Ativado")                              \
    X(LOCK_OFF,        "Off",                         "Desativado")                           \
    X(LOCK_ENABLE,     "Enable lock",                 "Habilitar bloqueio")                   \
    X(LOCK_TIMEOUT,    "Unlock timeout",              "Tempo de desbloqueio")                 \
    X(LOCK_TMO_FMT,    "%d min",                      "%d min")                               \
    X(LOCK_SET_PAT,    "Set pattern",                 "Definir padrão")                       \
    X(LOCK_CHANGE_PAT, "Change pattern",              "Alterar padrão")                       \
    X(LOCK_UNLOCK,     "Unlock",                      "Desbloquear")                          \
    X(LOCK_DRAW,       "Draw your pattern",           "Desenhe seu padrão")                   \
    X(LOCK_DRAW_NEW,   "Draw the new pattern",        "Desenhe o novo padrão")                \
    X(LOCK_CONFIRM,    "Draw again to confirm",       "Desenhe de novo para confirmar")       \
    X(LOCK_WRONG,      "Wrong pattern",               "Padrão incorreto")                     \
    X(LOCK_MISMATCH,   "Doesn't match, start over",   "Não confere, comece de novo")          \
    X(LOCK_MIN_DOTS,   "Connect at least 4 dots",     "Ligue pelo menos 4 pontos")            \
    X(LOCK_SAVED,      "Pattern saved",               "Padrão salvo")

typedef enum {
#define X(id, en, pt) STR_##id,
    I18N_STRINGS(X)
#undef X
    STR_COUNT,
} str_id_t;

/** Fixa o idioma da sessão; chamada uma vez no boot, antes de montar a UI. */
void i18n_set_lang(ui_lang_t lang);

/** Idioma em vigor. */
ui_lang_t i18n_lang(void);

/** Nome do idioma no próprio idioma ("English", "Português"). */
const char *i18n_lang_name(ui_lang_t lang);

/** Texto da chave no idioma em vigor. */
const char *i18n_get(str_id_t id);

/** Açúcar de leitura: `T(STR_OFFLINE)` no meio de uma chamada da LVGL. */
#define T(id) i18n_get(id)

/** Status cru da ponte traduzido; um status desconhecido volta como veio. */
const char *i18n_status(const char *status);

/** Dia da semana abreviado (0 = domingo). */
const char *i18n_weekday(int wday);

/** Mês abreviado (0 = janeiro). */
const char *i18n_month(int mon);

/** Data curta da home, na ordem de cada idioma ("Mon, Aug 11" / "seg, 11 ago"). */
void i18n_format_date(char *buf, size_t size, const struct tm *tm);

#ifdef __cplusplus
}
#endif
