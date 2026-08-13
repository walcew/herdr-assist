/**
 * @file
 * @brief Teclado do Cardputer reduzido a um evento por vez.
 *
 * A lib do M5 entrega o estado bruto da matriz (um vetor de caracteres mais os
 * modificadores) a cada varredura; a UI quer "uma tecla foi pressionada". Aqui
 * viram eventos com auto-repeat — e o repeat só vale para navegação e apagar,
 * porque repetir um Enter significaria mandar a mesma coisa duas vezes para um
 * agente do outro lado.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

/* Teclas de seta do Cardputer: a serigrafia coloca as setas em ; . , / */
#define KEY_UP_CH    ';'
#define KEY_DOWN_CH  '.'
#define KEY_LEFT_CH  ','
#define KEY_RIGHT_CH '/'
/* Canto superior esquerdo: é o "esc" de fato em todo firmware de Cardputer */
#define KEY_ESC_CH   '`'

struct KeyEvent {
    char ch     = 0;      /* caractere imprimível já com shift/capslock aplicado */
    bool enter  = false;
    bool del    = false;  /* backspace */
    bool tab    = false;
    bool fn     = false;
    bool ctrl   = false;
    bool shift  = false;
    bool opt    = false;
    bool alt    = false;
    bool repeat = false;  /* veio do auto-repeat, não de um toque novo */

    bool any() const { return ch || enter || del || tab; }
    bool is(char c) const { return ch == c; }

    /* Com shift segurado o teclado devolve o caractere DESLOCADO — `shift+;`
       chega como ':' e `shift+.` como '>'. Comparar só com o caractere base
       faz toda combinação com shift ser ignorada em silêncio, que é o tipo de
       bug que parece "a tecla não funciona". Daí os dois valores aqui. */
    bool up() const    { return ch == KEY_UP_CH    || ch == ':'; }
    bool down() const  { return ch == KEY_DOWN_CH  || ch == '>'; }
    bool left() const  { return ch == KEY_LEFT_CH  || ch == '<'; }
    bool right() const { return ch == KEY_RIGHT_CH || ch == '?'; }
    bool arrow() const { return up() || down() || left() || right(); }
};

/** Consome a varredura mais recente. true quando há uma tecla a tratar. */
bool keys_poll(KeyEvent *out);
