#include "keys.h"

#include <M5Cardputer.h>

/* Repetir só depois de uma pausa clara evita disparar duas linhas da lista com
   um toque; o intervalo curto depois disso é o que faz rolar uma lista longa
   parecer natural. */
#define REPEAT_DELAY_MS   420
#define REPEAT_PERIOD_MS   90

static uint32_t s_last_sig;
static uint32_t s_next_repeat;

/** Assinatura do estado: muda quando muda qualquer coisa que a UI enxerga. */
static uint32_t signature(const KeyEvent &e)
{
    return (uint32_t)(uint8_t)e.ch |
           ((uint32_t)e.enter << 8) | ((uint32_t)e.del << 9) |
           ((uint32_t)e.tab << 10) | ((uint32_t)e.fn << 11) |
           ((uint32_t)e.ctrl << 12) | ((uint32_t)e.shift << 13) |
           ((uint32_t)e.opt << 14) | ((uint32_t)e.alt << 15);
}

/* Segurar Enter mandaria a mesma mensagem de novo para o agente, e segurar uma
   letra encheria a linha de digitação sem querer. Só navegação e backspace. */
static bool repeatable(const KeyEvent &e)
{
    if (e.del) {
        return true;
    }
    return e.ch == KEY_UP_CH || e.ch == KEY_DOWN_CH ||
           e.ch == KEY_LEFT_CH || e.ch == KEY_RIGHT_CH;
}

bool keys_poll(KeyEvent *out)
{
    if (!M5Cardputer.Keyboard.isPressed()) {
        s_last_sig = 0;
        return false;
    }

    Keyboard_Class::KeysState st = M5Cardputer.Keyboard.keysState();
    KeyEvent ev;
    ev.enter = st.enter;
    ev.del   = st.del;
    ev.tab   = st.tab;
    ev.fn    = st.fn;
    ev.ctrl  = st.ctrl;
    ev.shift = st.shift;
    ev.opt   = st.opt;
    ev.alt   = st.alt;
    /* Com duas teclas juntas o vetor traz as duas; a primeira basta — a UI é de
       uma tecla por vez e os modificadores já vêm à parte. */
    if (!st.word.empty()) {
        ev.ch = st.word.front();
    }
    if (!ev.any()) {
        /* só modificador segurado (fn, shift...): não é evento */
        s_last_sig = 0;
        return false;
    }

    uint32_t sig = signature(ev);
    uint32_t now = millis();
    if (sig != s_last_sig) {
        s_last_sig = sig;
        s_next_repeat = now + REPEAT_DELAY_MS;
        *out = ev;
        return true;
    }
    if (repeatable(ev) && (int32_t)(now - s_next_repeat) >= 0) {
        s_next_repeat = now + REPEAT_PERIOD_MS;
        ev.repeat = true;
        *out = ev;
        return true;
    }
    return false;
}
