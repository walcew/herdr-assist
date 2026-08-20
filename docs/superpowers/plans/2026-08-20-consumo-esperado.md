# Consumo esperado (pace) na Dash — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Mostrar em cada barra de limite da Dash um marcador de consumo esperado (ritmo linear da janela) e colorir a barra fundindo proximidade do teto e ritmo.

**Architecture:** A ponte envia `window_s` (tamanho da janela em segundos) por linha de limite. O firmware guarda o campo, calcula o esperado a partir de `resets_at` e `window_s`, desenha um tracinho no ponto esperado e escolhe a cor da barra (vermelho perto do teto, âmbar adiantado, verde no ritmo). Sem `window_s`, degrada para o comportamento atual.

**Tech Stack:** Python 3.9+ stdlib (ponte), C11 + LVGL 8.4 + cJSON (firmware/ESP32-S3), PlatformIO.

**Spec:** `docs/superpowers/specs/2026-08-20-consumo-esperado-design.md`

## Global Constraints

- Ponte: **só stdlib**, Python 3.9. Testes rodam de dentro de `plugin/` (`cd plugin && python -m unittest ...`); usar `python` (Windows).
- Retrocompat: `window_s` ausente/0 = comportamento antigo (cor por `pct` absoluto, sem tracinho). Nunca quebrar painel/ponte de outra versão.
- Determinismo: `window_s` é constante por janela — não muda a serialização entre ciclos (o dedup de snapshot depende disso).
- Firmware: buffers de tamanho fixo. `window_s` = `uint32_t` (0 = desconhecido).
- Cores do tema (já usadas por `limit_color`): `UI_IDLE` (verde), `UI_WORKING` (âmbar), `UI_BLOCKED` (vermelho); marcador em `UI_TEXT`.
- Build do firmware da tela: `python -m platformio run` (env default `LVGL-320-480`).
- Comentários/commits em português (BR) acentuado.

---

## File Structure

- `plugin/herdr_bridge.py` (MODIFICAR) — `window_s` nas linhas de `collect_claude`/`collect_codex`.
- `plugin/tests/test_bridge_limits.py` (MODIFICAR) — asserts de `window_s`.
- `plugin/tests/fixtures/codex/auth.json` (NOVO) — fixture do Codex.
- `src/herdr_model.h` (MODIFICAR) — `window_s` em `herdr_limit_row_t`.
- `src/herdr_conn.c` (MODIFICAR) — ler `window_s` no laço de rows de `handle_limits`.
- `src/herdr_ui.c` (MODIFICAR) — cálculo do esperado, cor fundida e tracinho na Dash.

---

## Task 1: Ponte — `window_s` por linha de limite

**Files:**
- Modify: `plugin/herdr_bridge.py` (`collect_claude`, `collect_codex`)
- Modify: `plugin/tests/test_bridge_limits.py`
- Create: `plugin/tests/fixtures/codex/auth.json`

**Interfaces:**
- Produces: cada item de `rows` em `collect_claude`/`collect_codex` ganha `"window_s": int` (segundos; 0 se desconhecido).

- [ ] **Step 1: Escrever o teste que falha**

Criar a fixture `plugin/tests/fixtures/codex/auth.json`:
```json
{"tokens": {"access_token": "x", "account_id": "acc", "id_token": "aaa.bbb.ccc"}}
```

Adicionar ao fim de `plugin/tests/test_bridge_limits.py` (antes do `if __name__`):
```python
def fake_codex_usage(url, headers):
    return {"plan_type": "plus", "rate_limit": {
        "primary_window": {"limit_window_seconds": 18000, "used_percent": 20, "reset_at": 9999999999},
        "secondary_window": {"limit_window_seconds": 604800, "used_percent": 40, "reset_at": 9999999999}}}


class TestWindowS(unittest.TestCase):
    def test_claude_window_s_por_kind(self):
        with mock.patch.object(b, "fetch_json", fake_usage):
            cur = b.collect_claude(os.path.join(FIX, "work"))
        ws = {r["label"]: r["window_s"] for r in cur["limits"]}
        self.assertEqual(ws["5h"], 18000)     # session
        self.assertEqual(ws["7d"], 604800)    # weekly_all

    def test_codex_window_s_do_limit_window_seconds(self):
        with mock.patch.object(b, "fetch_json", fake_codex_usage):
            cur = b.collect_codex(os.path.join(FIX, "codex"))
        ws = sorted(r["window_s"] for r in cur["limits"])
        self.assertEqual(ws, [18000, 604800])
```
(O `fake_usage` já existe no arquivo, com `kind` `session` e `weekly_all`.)

- [ ] **Step 2: Rodar e ver falhar**

Run: `cd plugin && python -m unittest tests.test_bridge_limits -v`
Expected: FAIL com `KeyError: 'window_s'`.

- [ ] **Step 3: Implementar**

Em `collect_claude`, no laço que monta `rows`, trocar o `rows.append(...)` por (acrescenta `window_s` derivado do `kind`):
```python
        wsec = (18000 if kind == "session"
                else 604800 if kind in ("weekly_all", "weekly_scoped")
                else 0)
        rows.append({"label": label, "pct": int(round(lim.get("percent") or 0)),
                     "resets_at": iso_epoch(lim["resets_at"]) if lim.get("resets_at") else 0,
                     "window_s": wsec})
```

Em `collect_codex`, no laço de janelas, trocar o `rows.append(...)` por:
```python
        rows.append({"label": label, "pct": int(round(win.get("used_percent") or 0)),
                     "resets_at": round_min(win.get("reset_at") or 0),
                     "window_s": int(secs)})
```
(`secs` já é `limit_window_seconds`, lido logo acima no mesmo laço.)

- [ ] **Step 4: Rodar e ver passar**

Run: `cd plugin && python -m unittest tests.test_bridge_limits -v`
Expected: PASS (incluindo os 2 novos).

- [ ] **Step 5: Suíte inteira não regride**

Run: `cd plugin && python -m unittest discover -s tests -v`
Expected: PASS (proc_env, accounts, bridge_limits).

- [ ] **Step 6: Commit**

```bash
git add plugin/herdr_bridge.py plugin/tests/test_bridge_limits.py plugin/tests/fixtures/codex/
git commit -m "feat(ponte): window_s por linha de limite (tamanho da janela)"
```

---

## Task 2: Firmware — modelo + parse do `window_s`

**Files:**
- Modify: `src/herdr_model.h` (`herdr_limit_row_t`)
- Modify: `src/herdr_conn.c` (`handle_limits`, laço de `rows`)

**Interfaces:**
- Consumes: campo `window_s` do fio (Task 1).
- Produces: `herdr_limit_row_t.window_s` (uint32, 0 = desconhecido), preenchido pelo parse.

- [ ] **Step 1: Adicionar o campo no modelo**

Em `src/herdr_model.h`, em `herdr_limit_row_t`, após `uint32_t resets_at;`:
```c
    uint32_t window_s;   /* tamanho da janela em segundos; 0 = desconhecido */
```

- [ ] **Step 2: Ler no parse**

Em `src/herdr_conn.c`, `handle_limits`, no laço que lê cada `r` de `rows` (onde já lê `label`, `pct`, `resets_at`), acrescentar após a leitura de `resets_at`:
```c
        if (cJSON_IsNumber((f = cJSON_GetObjectItem(r, "window_s")))) {
            r_dst->window_s = (uint32_t)f->valuedouble;
        }
```
(Usar o mesmo nome de ponteiro de destino da linha que o código já usa — verifique se é `r_dst`, `row`, ou índice `&l->rows[k]`; siga o padrão local. O `memset` de `parse_limits` já zera, então ausente fica 0.)

- [ ] **Step 3: Build**

Run: `python -m platformio run` (env `LVGL-320-480`)
Expected: compila sem erro (campo maior; parse novo).

- [ ] **Step 4: Commit**

```bash
git add src/herdr_model.h src/herdr_conn.c
git commit -m "feat(fw): window_s no modelo e no parse de limites"
```

---

## Task 3: Firmware — Dash desenha o esperado (tracinho + cor fundida)

**Files:**
- Modify: `src/herdr_ui.c` (`limit_color` vizinhança + `add_limits_card`, laço de rows ~L1175-1217)

**Interfaces:**
- Consumes: `herdr_limit_row_t.window_s`, `resets_at`, `pct` (Task 2).

> **Verificação:** UI LVGL — build + inspeção visual no painel. Sem host-test.

- [ ] **Step 1: Helper de cor fundida**

Em `src/herdr_ui.c`, logo após a função `limit_color` (~L1076-1082), adicionar:
```c
/* Cor da barra fundindo teto e ritmo. Sem janela conhecida, cai na cor por
   % absoluto de limit_color (comportamento antigo). */
static lv_color_t row_bar_color(uint8_t pct, int expected, bool has_window)
{
    if (!has_window) return limit_color(pct);
    if (pct >= 90) return UI_BLOCKED;         /* perto do teto: aviso máximo */
    if ((int)pct > expected) return UI_WORKING; /* adiantado no ritmo */
    return UI_IDLE;                            /* no ritmo ou folgado */
}
```

- [ ] **Step 2: Calcular o esperado no laço de linhas**

Em `add_limits_card`, dentro do `for (int i = 0; i < n; i++)` das linhas de limite (o bloco que cria label/val/bar, ~L1175), no início do corpo do laço (após `const herdr_limit_row_t *r = &l->rows[i];`), adicionar:
```c
        /* consumo esperado: onde a barra estaria no ritmo linear da janela */
        bool has_window = (r->window_s > 0 && (time_t)r->resets_at > now);
        int expected = 0;
        if (has_window) {
            uint32_t remaining = (uint32_t)((time_t)r->resets_at - now);
            if (remaining > r->window_s) remaining = r->window_s;
            expected = (int)(((uint32_t)(r->window_s - remaining) * 100u) / r->window_s);
        }
```
(`now` já existe no topo de `add_limits_card`.)

- [ ] **Step 3: Usar a cor fundida na barra**

Na criação da barra, trocar:
```c
        lv_obj_set_style_bg_color(bar, limit_color(r->pct), LV_PART_INDICATOR);
```
por:
```c
        lv_obj_set_style_bg_color(bar, row_bar_color(r->pct, expected, has_window), LV_PART_INDICATOR);
```

- [ ] **Step 4: Desenhar o tracinho**

Logo após `lv_bar_set_value(bar, r->pct, LV_ANIM_OFF);`, adicionar:
```c
        /* marcador do esperado: só quando conhecido e sem encostar nas pontas */
        if (has_window && expected > 0 && expected < 100) {
            lv_obj_t *mk = lv_obj_create(bar);
            lv_obj_remove_style_all(mk);
            lv_obj_set_size(mk, 2, DASH_BAR_H + 4);
            lv_obj_set_style_bg_color(mk, UI_TEXT, 0);
            lv_obj_set_style_bg_opa(mk, LV_OPA_COVER, 0);
            lv_obj_set_style_radius(mk, 1, 0);
            lv_obj_clear_flag(mk, LV_OBJ_FLAG_SCROLLABLE);
            lv_coord_t mx = (lv_coord_t)(((int)DASH_BAR_W * expected) / 100 - 1);
            lv_obj_align(mk, LV_ALIGN_LEFT_MID, mx, 0);
        }
```

- [ ] **Step 5: Build**

Run: `python -m platformio run` (env `LVGL-320-480`)
Expected: compila sem erro.

- [ ] **Step 6: Verificação visual (manual, no painel)**

Com a ponte enviando `window_s`: o tracinho aparece no ponto esperado de cada barra; a barra fica **âmbar** quando `pct > esperado`, **verde** quando no ritmo/folgado, **vermelha** quando `pct ≥ 90`; janelas sem `window_s` ficam como antes (sem tracinho). Conferir posição do tracinho em várias larguras.

- [ ] **Step 7: Commit**

```bash
git add src/herdr_ui.c
git commit -m "feat(fw): Dash mostra consumo esperado (tracinho + cor por teto/ritmo)"
```

---

## Self-Review (autor do plano)

**Cobertura do spec:** `window_s` na ponte (T1); modelo+parse (T2); esperado+cor+tracinho na Dash (T3); degradação sem window_s (T3 `has_window`); determinismo (window_s constante, T1). ✔

**Placeholders:** nenhum; código real em cada passo. A única indireção é o nome do ponteiro de destino no parse (T2 Step 2), com instrução explícita de seguir o padrão local do arquivo.

**Consistência de tipos:** `window_s` uint32 no modelo (T2), int `f->valuedouble` no parse (T2), int em `collect_*` (T1); `expected` int em [0,100] (T3); `row_bar_color(uint8_t, int, bool)` e `has_window`/`expected` calculados antes do uso (T3 steps 2→3→4).
