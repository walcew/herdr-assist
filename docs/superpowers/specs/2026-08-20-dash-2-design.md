# Dash 2.0 — design

**Data:** 2026-08-20
**Estado:** proposto (aguardando plano)

## Problema

A Dash mostra o uso de limites por conta (com pace, do consumo-esperado) e o
título do card traz o **e-mail inteiro** quando há multi-conta. Faltam três
coisas pedidas: (1) um **card de custo** em US$; (2) o mesmo tratamento das
Sessões nos cards — **domínio sem sufixo + selo corporativa/pessoal (pill)** no
lugar do e-mail; (3) **quantos agentes rodam por conta**.

## Decisões (do brainstorming + mockup)

- **Card de custo (novo, no topo da Dash):** três linhas — **Agora** (soma das
  sessões ativas), **Semana** (últimos 7 dias), **Vitalício** (tudo). Rótulo
  **estimativa** e valores com prefixo `~` (o transcript não traz custo; é
  cálculo tokens × preço).
- **Cards de uso:** título vira `provedor · [pill corp/pess] org` (org = domínio
  sem sufixo, ex.: `sosdocs`, `gmail`), reaproveitando `org`/`corp` do
  multi-conta/Sessões. Só com multi-conta (mesmo critério de hoje).
- **Agentes por conta:** no canto do card, `N (M ativos)` — N total de agentes
  daquela conta, M os que estão `working`.

## Descoberta (verificada)

- Transcript **não** tem custo → calcular por **tokens × tabela de preços** por
  modelo (mantida na ponte). Preços da API Anthropic (skill claude-api, cache
  2026-06-24), US$ por 1M tokens (input, output): Opus 4.x `5/25`; Sonnet 5/4.x
  `3/15`; Haiku 4.5 `1/5`; Fable/Mythos 5 `10/50`. Cache: read `0.1×` do input,
  write (creation) `1.25×` do input.
- Custo cobre **Claude** (transcripts em `<config-dir>/projects/*/*.jsonl`, já
  acessíveis). Codex fica de fora (transcript em outro formato/local).

## Arquitetura

### 1. Ponte — custo (`plugin/cost.py`, novo)

Funções puras:
- `PRICES: dict[str, tuple[float, float]]` (model_id → (in, out) US$/1M).
- `message_cost(usage: dict, model_id: str) -> float`:
  `in`,`out` = `PRICES.get(model_id, (5.0, 25.0))` (default Opus-ish);
  `(input*in + cache_creation*in*1.25 + cache_read*in*0.1 + output*out) / 1e6`.
- `fmt_usd(v: float) -> str`: formata "~US$ 4,20" (vírgula decimal; milhar com
  ponto; sempre prefixo `~`). Ex.: 4.2→"~US$ 4,20", 1240→"~US$ 1.240".
- `file_cost(jsonl_path) -> dict`: varre o transcript uma vez e devolve
  `{"total": float, "days": {"YYYY-MM-DD": float}}` — `total` soma tudo;
  `days` agrega por data (do `timestamp` de cada evento assistant) para a janela
  semanal. `None`/`{}` em falha.

### 2. Ponte — agregação + cache (`herdr_bridge.py`)

- Cache por arquivo: `cost_cache: dict[path -> (mtime, size, {"total","days"})]`.
  Recalcula só quando `(mtime, size)` muda (arquivo cresce ao usar). Leitura no
  **executor** (varredura pesada; nunca no event loop).
- `aggregate_cost(account_dirs) -> dict`: varre `glob(<dir>/projects/*/*.jsonl)`
  de cada config-dir claude descoberto (dedup por path), somando via cache:
  - `life` = Σ `total` de todos os arquivos.
  - `week` = Σ dos `days` cujas datas ∈ [hoje-6 … hoje].
  - `now` = Σ `session_cost` das **sessões ativas** (os transcripts dos panes
    claude atuais — mesmo mapa pane→transcript das Sessões 2.0).
  Devolve `{"now": fmt_usd(now), "week": fmt_usd(week), "life": fmt_usd(life)}`.
- **Payload novo** `{"type":"cost","now":"~US$ …","week":"~US$ …","life":"~US$ …"}`,
  difundido com o mesmo dedup de snapshot (strings prontas; muda pouco).
- Passo de coleta próprio (cadência baixa, ex.: junto do `limits_loop` ou um
  laço com passo ~30s), sempre no executor. A varredura vitalícia é cara só na
  1ª vez (cache esquenta).

### 3. Ponte — org/corp e contagem nos limites (`herdr_bridge.py`)

- `collect_limits`/o payload de `limits`: cada provedor ganha `org`, `corp`
  (via `accounts.org_and_corp(account)` — reaproveita Sessões 2.0) e
  `agents`, `agents_working` (contagem por conta): a partir do `pane_account`
  (pane→(agent, config_dir)) e dos status dos agentes, contar quantos panes
  batem `(agent==provider-kind, config_dir==este)` e quantos estão `working`.

### 4. Protocolo de fio

- `limits[].providers[]`: `org` (str), `corp` (bool), `agents` (int),
  `agents_working` (int). Todos opcionais (ausente = comportamento antigo).
- Novo tipo `cost` com `now`/`week`/`life` (strings). Ausente = sem card.

### 5. Firmware — modelo (`herdr_model.h`)

- `herdr_limits_t` ganha `char org[24]; bool corp; uint8_t agents; uint8_t agents_working;`.
- Novo `herdr_cost_t { char now[16]; char week[16]; char life[16]; }` + acesso
  thread-safe no modelo (`set_cost`/`get_cost`), como os demais.

### 6. Firmware — parse (`herdr_conn.c`)

- `handle_limits`: ler `org`, `corp`, `agents`, `agents_working`.
- Novo `handle_cost`: ler as 3 strings → `herdr_model_set_cost`.

### 7. Firmware — UI Dash (`herdr_ui.c`)

- `add_limits_card`: quando multi-conta, título `provedor · [pill] org`
  (reaproveitar o pill de corp/pess das Sessões — `UI_CORP`/`UI_MUTED`, bg suave
  + radius); mostrar `N (M ativos)` no canto (só quando `agents>0`).
- `rebuild_dash_cards`: no topo da lista, se houver custo, um **card de custo**
  ("Custo · estimativa") com as 3 linhas (Agora/Semana/Vitalício → strings).

## Edge cases

- **Sem custo/`window` desconhecido:** sem card de custo; cards de uso normais.
- **Modelo fora da tabela:** usa o default (5/25) — estimativa, marcada com `~`.
- **Codex:** sem custo (fora de escopo); a conta ainda conta agentes/limites.
- **Fuso/data:** `days` usa a data local do `timestamp` do evento (o mesmo
  relógio do host); a janela é [hoje-6 … hoje] inclusivo (7 dias).
- **Determinismo:** strings de custo mudam devagar; dedup de snapshot preservado.
- **Perf:** varredura vitalícia cacheada por (mtime,size); só relê arquivos que
  cresceram; tudo no executor.

## Testes

- **Ponte (`plugin/tests/`):**
  - `test_cost.py`: `message_cost` (por modelo + default + cache), `fmt_usd`
    (vírgula/milhar/prefixo), `file_cost` com fixture jsonl (total + days por
    data).
  - `test_bridge_*` (estende): `aggregate_cost` com fixtures (life/week/now) e
    cache por (mtime,size); org/corp e contagem de agentes no payload de limits.
- **Firmware:** build (`python -m platformio run`, `LVGL-320-480`) + verificação
  visual (card de custo; pill/org nos cards; contagem por conta).

## Fora de escopo

- Custo do Codex (formato de transcript diferente).
- Custo por-conta dentro do card de uso (ficou como "card agregado" na decisão).
- Preço configurável / atualização automática da tabela (manual, marcado `~`).
