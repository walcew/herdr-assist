# Consumo esperado (pace) na Dash — design

**Data:** 2026-08-20
**Estado:** proposto (aguardando plano)

## Problema

A Dash mostra o consumo de cada janela de limite como um valor absoluto (`pct`)
e uma barra. "43%" sozinho não diz se é muito ou pouco **para este ponto da
janela**: 43% no 6º dia de uma semana é folga; 43% no 1º dia é queima rápida.
Falta a referência de **ritmo** — quanto o consumo *deveria* estar se fosse
linear ao longo da janela.

Objetivo: mostrar, em cada barra de limite, um **marcador de consumo esperado**
(o ponto onde o consumo estaria no ritmo linear) e colorir a barra combinando
proximidade do teto e ritmo. Ideia trazida do projeto-irmão `clawd-panel`.

## Decisões (do brainstorming)

- **Tratamento visual:** tracinho no ponto esperado + esquema de cor fundido.
- **Esquema de cor de 3 níveis** (quando o esperado é conhecido):
  - **Vermelho** se `pct ≥ 90` — perto do teto, prioridade máxima (mantém o
    aviso de segurança que a cor por % absoluto já dava).
  - **Âmbar** se `pct > esperado` — adiantado no ritmo.
  - **Verde** caso contrário — no ritmo ou folgado.
- **Janelas:** todas as janelas de tempo (5h sessão, 7d semana, 7d por modelo).
- **Degradação:** sem `window_s` conhecido (ponte antiga, ou `kind`/janela que
  a ponte não mapeia), a barra fica como hoje — cor por `pct` absoluto
  (`limit_color`) e **sem** tracinho. Retrocompat limpa nas duas pontas.

## Arquitetura

Mesma espinha do multi-conta: ponte → fio → modelo → UI. Um campo novo.

### 1. Ponte (`herdr_bridge.py`)

Cada linha de limite (`rows[]` em `collect_claude`/`collect_codex`) ganha
`window_s` (inteiro, segundos da janela; `0`/ausente = desconhecido):

- **Claude** (`collect_claude`): derivado do `kind`.
  - `session` → `18000` (5h)
  - `weekly_all` e `weekly_scoped` → `604800` (7d)
  - outro `kind` → `0` (desconhecido; degrada)
- **Codex** (`collect_codex`): usa o `limit_window_seconds` que já é lido para
  montar o label (`secs`). `window_s = int(secs)` (0 se ausente).

`window_s` é **constante** para uma janela (não muda a cada segundo como o
`reset_after_seconds`), então **não afeta o dedup** de snapshot.

### 2. Protocolo de fio

`limits[].limits[]` (cada linha) ganha `"window_s": <int>`. Opcional: ausente =
comportamento antigo. Retrocompat total.

### 3. Firmware — modelo (`herdr_model.h`)

`herdr_limit_row_t` ganha `uint32_t window_s;` (`0` = desconhecido). O `memset`
já existente em `handle_limits` zera por padrão.

### 4. Firmware — parse (`herdr_conn.c`)

No laço de `rows` em `handle_limits`, ler `window_s` quando presente:
```c
if (cJSON_IsNumber((f = cJSON_GetObjectItem(r, "window_s")))) {
    r_dst->window_s = (uint32_t)f->valuedouble;
}
```

### 5. Firmware — Dash (`herdr_ui.c`, `add_limits_card`, laço de linhas)

Para cada linha, quando `window_s > 0 && resets_at > now`:

```
remaining = resets_at - now
expected  = clamp((window_s - remaining) * 100 / window_s, 0, 100)
```

- **Tracinho:** um `lv_obj` fino (2px) sobre a `lv_bar`, em
  `x = DASH_BAR_W * expected / 100`, altura levemente maior que a barra
  (transborda ~2px em cima e embaixo), cor clara de destaque (`UI_TEXT`). Só
  desenhado quando `0 < expected < 100` (nos extremos ele encostaria nas pontas da barra).
- **Cor da barra (indicador):** função nova `row_bar_color(pct, expected, has_window)`:
  - `!has_window` → `limit_color(pct)` (comportamento atual, por % absoluto).
  - `pct >= 90` → `UI_BLOCKED` (vermelho — perto do teto).
  - `pct > expected` → `UI_WORKING` (âmbar — adiantado).
  - senão → `UI_IDLE` (verde — no ritmo/folgado).
  (Cores do tema já usadas por `limit_color`: `UI_IDLE`/`UI_WORKING`/`UI_BLOCKED`,
  para consistência entre temas.)

Sem `window_s`: nenhum tracinho, cor por `limit_color(pct)` como hoje.

## Edge cases

- **`resets_at ≤ now`** (janela virada / dado stale): trata como desconhecido —
  sem tracinho, cor absoluta. Evita `remaining` negativo.
- **`expected` em 0 ou 100:** não desenha o tracinho (encostaria na ponta);
  a cor ainda vale.
- **`window_s` ausente ou 0:** degrada para o comportamento atual.
- **`pct` e `expected` ambos altos:** `pct ≥ 90` vence (vermelho), mesmo se no
  ritmo — perto do teto é o aviso mais importante.
- **Determinismo do snapshot:** `window_s` constante não muda a serialização
  entre ciclos; o dedup continua válido.

## Testes

- **Ponte (`plugin/tests/test_bridge_limits.py`):** estender as fixtures/asserts
  para conferir `window_s` por linha — Claude `session`→18000 e `weekly_*`→604800;
  Codex a partir de `limit_window_seconds`. Snapshot ainda determinístico.
- **Firmware:** build (`python -m platformio run`, env `LVGL-320-480`) +
  verificação visual no painel (tracinho na posição certa; barra âmbar quando
  `pct > esperado`; vermelho perto do teto; ausência de tracinho quando a janela
  é desconhecida).

## Fora de escopo

- Custo em US$ e contexto-por-agente (garimpados do clawd-panel; incrementos
  futuros, specs próprios).
- Selo de texto "adiantado/no ritmo" (descartado no brainstorming — a cor +
  tracinho já comunicam, sem ocupar altura).
- Configurar o modelo de ritmo (linear é o único; nada configurável).
