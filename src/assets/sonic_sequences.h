#ifndef SONIC_SEQUENCES_H
#define SONIC_SEQUENCES_H

/* Auto-gerado por scripts/sonic_export.py — sequências frame-a-frame
   das animações do Sonic (idle fiel à animação de espera do jogo). */

#include <stdint.h>

#define SONIC_CHEER_SEQ_LEN  11
#define SONIC_CHEER_SEQ_LOOP 7
static const uint8_t sonic_cheer_seq[11] = {
    0, 1, 1, 1, 1, 1, 1, 2, 1, 3, 1,
};

#define SONIC_COMPLAIN_SEQ_LEN  3
#define SONIC_COMPLAIN_SEQ_LOOP 0
static const uint8_t sonic_complain_seq[3] = {
    0, 1, 2,
};

#define SONIC_IDLE_SEQ_LEN  106
#define SONIC_IDLE_SEQ_LOOP 53
static const uint8_t sonic_idle_seq[106] = {
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 1, 2, 2, 3, 3, 4, 4, 3, 3, 4, 4, 3, 3, 4,
    4, 3, 3, 4, 4, 3, 3, 4, 4, 3, 3, 4, 4, 3, 3, 4,
    4, 3, 3, 4, 4, 5, 5, 5, 5, 5, 5, 6, 6, 6, 6, 6,
    6, 7, 8, 8, 8, 8, 8, 8, 7, 7,
};

#define SONIC_KO_SEQ_LEN  2
#define SONIC_KO_SEQ_LOOP 0
static const uint8_t sonic_ko_seq[2] = {
    0, 1,
};

#define SONIC_PUSH_SEQ_LEN  4
#define SONIC_PUSH_SEQ_LOOP 0
static const uint8_t sonic_push_seq[4] = {
    0, 1, 2, 1,
};

#define SONIC_RUN_SEQ_LEN  4
#define SONIC_RUN_SEQ_LOOP 0
static const uint8_t sonic_run_seq[4] = {
    0, 1, 2, 3,
};

#endif // SONIC_SEQUENCES_H
