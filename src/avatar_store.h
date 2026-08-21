/**
 * @file
 * @brief Marketplace de avatares: cataloga repositórios e baixa pacotes .hav.
 *
 * Um repositório é só uma URL base servindo arquivos estáticos — funciona em
 * GitHub raw, GitHub Pages, GitLab ou um nginx qualquer, sem depender de API
 * de fornecedor nenhum:
 *
 *     GET <base>/index.json   {"avatars":[{"id":"sonic","name":"Sonic","size":38392}]}
 *     GET <base>/sonic.hav    -> /sd/avatars/sonic.hav
 *
 * Task própria, sem nenhuma chamada LVGL: a UI faz poll de
 * avatar_store_get_status() dentro de um lv_timer, mesmo padrão do fw_update e
 * do pareamento. Nada começa sozinho — atualizar e instalar são sempre toque
 * do usuário.
 *
 * Esta é a ÚNICA task de I/O de avatar: além do marketplace, ela também lê os
 * pacotes do cartão para o motor (avatar_store_load_pack). Duas eram uma a
 * mais — a RAM interna deste painel acaba durante o boot (medido: 11KB livres
 * quando as pontes conectam) e criar a segunda simplesmente falhava. Juntar
 * também serializa o acesso ao cartão de graça, já que carregar avatar e
 * baixar pacote nunca precisam acontecer ao mesmo tempo.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "avatar_pack.h"

#ifdef __cplusplus
extern "C" {
#endif

#define STORE_ID_LEN    24
#define STORE_NAME_LEN  24
#define STORE_MAX       16   /* entradas no catálogo, somando todos os repos */
#define STORE_REPO_LEN  96   /* uma URL base cabe com folga em 96 */
#define STORE_USER_REPOS 2   /* slots editáveis na tela; o padrão não conta */
#define STORE_BRIDGE_REPOS 2 /* quantos cada ponte pode empurrar */
#define STORE_MAX_REPOS  8   /* teto por refresh, somando todas as fontes */

typedef struct {
    char     id[STORE_ID_LEN];
    char     name[STORE_NAME_LEN];
    uint32_t size;        /* bytes, do index.json; 0 = repositório não disse */
    bool     installed;   /* já existe em /sd/avatars */
    bool     builtin;     /* o de fábrica: não baixa nem apaga */
    uint8_t  repo;        /* de qual repositório veio; 255 = só local */
} avatar_entry_t;

typedef enum {
    STORE_IDLE = 0,     /* nunca atualizou */
    STORE_REFRESHING,
    STORE_DOWNLOADING,  /* pct e id válidos */
    STORE_FORMATTING,   /* apagando o cartão a pedido do usuário */
    STORE_READY,        /* catálogo em mãos */
    STORE_ERROR,        /* err válido */
} store_state_t;

typedef enum {
    STORE_ERR_NONE = 0,
    STORE_ERR_NET,      /* repositório inacessível (rede, TLS, 404) */
    STORE_ERR_INDEX,    /* index.json veio, mas não parseia */
    STORE_ERR_NO_SD,    /* sem cartão: não há onde guardar */
    STORE_ERR_SPACE,    /* cartão sem espaço para o pacote */
    STORE_ERR_DOWNLOAD, /* download ou gravação falhou */
    STORE_ERR_PACK,     /* baixou, mas o arquivo não é um pacote válido */
    STORE_ERR_FORMAT,   /* o cartão não aceitou ser formatado */
} store_err_t;

/** De onde uma URL de repositório veio; a tela mostra o que dá para editar. */
typedef enum {
    REPO_SRC_DEFAULT = 0,  /* embutido no firmware */
    REPO_SRC_USER,         /* NVS, editável na tela */
    REPO_SRC_CARD,         /* /sd/avatars/repos.txt */
    REPO_SRC_BRIDGE,       /* empurrado pela ponte */
} repo_src_t;

typedef struct {
    char       url[STORE_REPO_LEN];
    repo_src_t src;
} avatar_repo_t;

typedef struct {
    store_state_t state;
    store_err_t   err;
    uint8_t       pct;                 /* 0-100, só em DOWNLOADING */
    char          id[STORE_ID_LEN];    /* alvo do download ou do erro */
} avatar_store_status_t;

/**
 * Sobe a task de I/O. Chamar uma vez, logo depois de sd_mount() e ANTES de
 * avatar_create() — que depende dela e cujo caminho de boot já é apertado de
 * RAM interna.
 */
void avatar_store_init(void);

/**
 * Pede a leitura de um pacote do cartão (o motor não pode fazer isso na task
 * da LVGL: são ~3s para o maior). false se a task está ocupada. O resultado se
 * colhe com avatar_store_take_pack().
 */
bool avatar_store_load_pack(const char *path);

/**
 * Colhe o pacote pedido: 1 pronto (movido para *out), 0 ainda carregando,
 * -1 falhou. Chamar da task da LVGL.
 */
int avatar_store_take_pack(avatar_pack_t *out);

/** Cópia consistente do estado (livre de tearing entre tasks). */
void avatar_store_get_status(avatar_store_status_t *out);

/** Agenda uma releitura dos repositórios; false se já há algo em curso. */
bool avatar_store_refresh(void);

/** Agenda o download de `id`; false se já há algo em curso ou o id é estranho. */
bool avatar_store_install(const char *id);

/** Apaga o pacote do cartão (síncrono: é um unlink). false se não deu. */
bool avatar_store_remove(const char *id);

/**
 * Agenda a formatação do cartão — que apaga TUDO nele, não só os avatares.
 * false se já há algo em curso. Só chamar com o usuário tendo confirmado.
 *
 * Formatar leva segundos, então vai para a mesma task de I/O; ao terminar ela
 * refaz o catálogo sozinha, como faria um refresh.
 */
bool avatar_store_format(void);

/**
 * Repositório do usuário, guardado na NVS (idx 0..STORE_USER_REPOS-1). Vale a
 * partir do próximo refresh — não exige reiniciar, ao contrário do panel_cfg.
 */
void avatar_store_get_repo(int idx, char *out, size_t size);
void avatar_store_set_repo(int idx, const char *url);

/**
 * Recolhe, na hora, os repositórios que um refresh varreria, com a procedência
 * de cada um — o padrão, os dois da NVS, os do cartão e os das pontes, já sem
 * repetir URL. Toca a NVS e lê /sd/avatars/repos.txt, então não é de graça;
 * serve à tela de repositórios, que abre a pedido. Retorna quantos.
 */
int avatar_store_repo_list(avatar_repo_t *out, int max);

/**
 * Repositórios empurrados pela ponte, por slot de host.
 *
 * Só em RAM, nunca na NVS: o plugin é a fonte de verdade do que ele empurra, e
 * persistir deixaria entrada velha quando alguém tirasse uma URL de lá. Mesmo
 * raciocínio do host em descoberta automática (panel_host_is_auto).
 */
void avatar_store_set_bridge_repos(int host, const char *const *urls, int count);

/**
 * Copia o catálogo em `out` e devolve quantos. O de fábrica é sempre o
 * primeiro. Chamar da task da LVGL.
 */
int avatar_store_list(avatar_entry_t *out, int max);

#ifdef __cplusplus
}
#endif
