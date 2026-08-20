# Sessões 2.0 — design

**Data:** 2026-08-20
**Estado:** proposto (aguardando plano)
**Sub-projeto 1 de 2** (o outro é "Custo na Dash", spec próprio).

## Problema

A aba Sessões mostra, por agente: bolinha de status, projeto (basename do cwd),
sub-linha `agente · status` e — quando há multi-conta — o **e-mail inteiro**
(que trunca e ocupa a linha). Faltam informações de alto valor e sobra ruído:

- **Modelo ausente:** "claude" não diz Opus/Sonnet/Haiku.
- **E-mail inteiro** desperdiça espaço; o que importa é a **organização**.
- **Sem noção de contexto:** nada avisa que um agente está perto de compactar.

Objetivo: repaginar a linha de Sessões para mostrar **modelo**, **domínio da
conta sem sufixo + selo corporativa/pessoal**, e um **medidor de contexto**
(aviso de compactação), mantendo a linha de 52 px legível. Layout aprovado no
mockup (variante "Recomendada").

## Descoberta (verificada ao vivo)

Inspeção de transcripts reais do Claude Code (`<config-dir>/projects/<cwd>/<uuid>.jsonl`):

- `message.model` = ex. `claude-opus-4-8` → **modelo disponível**.
- `message.usage`: `input_tokens`, `output_tokens`, `cache_read_input_tokens`,
  `cache_creation_input_tokens` → o **tamanho do contexto** do último evento é
  `input + cache_read + cache_creation`.
- Contexto observado chega a **621k tokens** → há sessões de **1M**; o `model`
  não sinaliza "[1m]", então a janela é inferida (ver Decisões).
- `gitBranch` também está presente (não usado nesta versão; fica p/ depois).
- **Não há campo de custo** — custo é o Sub-projeto 2.
- O transcript é achado por **glob por UUID**: `<config-dir>/projects/*/<uuid>.jsonl`
  (robusto, sem reconstruir o encoding do cwd). O UUID vem de
  `agent_session.value` no `pane.list`; o `<config-dir>` já é conhecido por pane
  (multi-conta). Codex não tem esse transcript — ver Escopo.

## Decisões (do brainstorming)

- **Layout:** variante Recomendada — sub-linha `[selo] org · modelo`; medidor de
  contexto (barra + %) no canto inferior-direito; cronômetro no topo-direito só
  para quem trabalha.
- **Domínio sem sufixo:** tirar `.com`/`.com.br`/etc. e mostrar só o nome —
  `sosdocs.com.br` → **`sosdocs`**, `gmail.com` → **`gmail`**.
- **Classificação:** domínio público (gmail, outlook, hotmail, live, yahoo,
  icloud, proton/protonmail, gmx, aol...) → **pessoal**; senão → **corporativa**.
- **Contexto% com heurística de janela:** `janela = 1_000_000 se uso>200_000
  senão 200_000`; `pct = min(100, uso*100/janela)`. Aproximado mas honesto.
- **Selo/org só com múltiplas contas** (mesmo critério `multi_acct` de hoje).

## Arquitetura

Ponte → fio → modelo → UI, como as features anteriores.

### 1. Ponte — módulo de transcript (`plugin/transcript.py`, novo)

Funções puras, testáveis:

- `model_display(model_id: str) -> str` — normaliza para exibição curta:
  `claude-opus-4-8`→`Opus 4.8`, `claude-sonnet-4-5`→`Sonnet 4.5`,
  `claude-haiku-4-5`→`Haiku 4.5`, `claude-fable-5`→`Fable 5`; `gpt-5`→`gpt-5`;
  desconhecido → o próprio id (clipado a 15 bytes).
- `context_pct(prompt_tokens: int) -> int` — `janela = 1_000_000 if
  prompt_tokens > 200_000 else 200_000`; `min(100, prompt_tokens*100//janela)`.
- `session_metrics(jsonl_path: str) -> dict | None` — lê o transcript, acha o
  **último evento assistant**, devolve `{"model": model_display(...),
  "context_pct": context_pct(input+cache_read+cache_creation)}`. `None` se o
  arquivo não existe/ilegível/sem assistant (degrada).

### 2. Ponte — org + classificação (`plugin/accounts.py`, estende)

- `PUBLIC_DOMAINS = {"gmail.com","outlook.com","hotmail.com","live.com",
  "yahoo.com","yahoo.com.br","icloud.com","proton.me","protonmail.com",
  "gmx.com","aol.com"}` (minúsculas).
- `_KNOWN_2LD = {"com.br","com.mx","co.uk","com.ar","com.au"}` (sufixos de 2
  níveis para o strip).
- `org_and_corp(email: str) -> tuple[str, bool]` — `domain = email.split("@")[-1]
  .lower()`; `corp = domain not in PUBLIC_DOMAINS`; `org` = último rótulo após
  remover o sufixo público (2LD conhecido → tira 2; senão tira 1). Ex.:
  `sosdocs.com.br`→`("sosdocs", True)`; `gmail.com`→`("gmail", False)`;
  `mail.google.com`→`("google", True)`. E-mail vazio/sem `@` → `("", False)`.

### 3. Ponte — integração (`herdr_bridge.py`, `push_agents`/`refresh_accounts`)

Para cada pane com agente **claude** que tenha `agent_session.value` (UUID):
- acha o transcript por glob `<config_dir>/projects/*/<uuid>.jsonl` (o
  `config_dir` vem do `pane_account`);
- `m = session_metrics(path)` (cacheado por `(path, mtime)` — só relê quando o
  arquivo muda; leitura fora do event loop, no mesmo executor do multi-conta);
- anexa ao agente: `"model"`, `"context_pct"` (omitido se desconhecido).
Para todo agente com conta conhecida: `org, corp = org_and_corp(account)` (do
`acc_email_cache`/conta) → anexa `"org"`, `"corp"`.

Codex e panes sem UUID: sem `model`/`context_pct` (degrada); ainda ganham
`org`/`corp` pela conta.

### 4. Protocolo de fio

`agents[]` ganham, todos opcionais (ausente = degrada):
- `"model"`: string curta (ex. `"Opus 4.8"`), clipada a 15 bytes.
- `"context_pct"`: int 0–100 (ausente = desconhecido).
- `"org"`: string curta (ex. `"sosdocs"`), clipada a 23 bytes.
- `"corp"`: bool.

### 5. Firmware — modelo (`herdr_model.h`)

`herdr_agent_t` ganha:
```c
char    model[16];       /* modelo curto p/ exibição; "" desconhecido */
char    org[24];         /* nome da org (domínio sem sufixo); "" desconhecido */
uint8_t context_pct;     /* 0-100; 255 = desconhecido */
bool    corp;            /* true = corporativa, false = pessoal */
```
`herdr_model_set_agents`/o `memset` de parse já zeram; `context_pct` precisa ser
**inicializado a 255** no parse quando o campo estiver ausente (o 0 é um valor
válido de contexto), então o parser seta 255 por padrão antes de ler.

### 6. Firmware — parse (`herdr_conn.c`, `handle_agents`)

Ler `model` (string), `org` (string), `corp` (`cJSON_IsTrue`), e `context_pct`
(número; se ausente, deixar 255). Seguir o padrão dos campos existentes.

### 7. Firmware — Sessões 2.0 (`herdr_ui.c`, `add_session_row`)

Layout Recomendado (linha de 52 px):
- **dot** de status (esquerda) — inalterado.
- **projeto** (nome, `lv_font_ui_16`) no topo — inalterado.
- **sub-linha** (`lv_font_ui_12`, `UI_MUTED`):
  - `multi_acct && a->account[0]`: `[selo] org · modelo` — selo = pill pequeno
    ("corp" em azul `UI_CORP`; "pess" em `UI_MUTED`); depois `org`; depois
    `modelo` (se conhecido). Sem status textual (o dot carrega o status).
  - senão: `modelo · status` (o modelo substitui o "claude" genérico de hoje);
    sem modelo conhecido → `agente · status` (comportamento atual).
- **medidor de contexto** (quando `context_pct != 255`): no canto
  inferior-direito, barra fina (~64 px) + `"ctx NN%"`, cor por preenchimento
  (`ctx_color`: verde `<70`, âmbar `70–90`, vermelho `≥90`). Substitui o chip de
  e-mail de hoje.
- **cronômetro** (working) no topo-direito — inalterado.

Novo: `UI_CORP` em `ui_theme.h` (azul discreto, ex. `0x6f9bd1`), e um helper
`ctx_color(uint8_t pct)`.

## Edge cases

- **context_pct ausente** (255): sem medidor; a linha usa o espaço para o sub.
- **modelo desconhecido:** sub-linha cai em `agente · status`.
- **conta vazia:** sem selo/org (como hoje).
- **uma conta só (`!multi_acct`):** sub-linha `modelo · status`, sem selo/org —
  o modelo ainda aparece (ganho mesmo sem multi-conta).
- **transcript grande:** `session_metrics` lê só o necessário; cache por mtime
  evita reler a cada ciclo; leitura no executor (não bloqueia o event loop).
- **org com um rótulo só** (ex. `localhost`): devolve o próprio.

## Testes

- **Ponte (`plugin/tests/`):**
  - `test_transcript.py`: `model_display` (mapeamentos + fallback);
    `context_pct` (heurística 200k/1M, clamp); `session_metrics` com uma
    **fixture jsonl** pequena (último assistant, tokens → pct, modelo).
  - `test_accounts.py` (estende): `org_and_corp` — corp com 2LD (`sosdocs.com.br`
    →`sosdocs`,True), pessoal (`gmail.com`→`gmail`,False), subdomínio
    (`mail.google.com`→`google`,True), vazio.
- **Firmware:** build (`python -m platformio run`, `LVGL-320-480`) + verificação
  visual (selo corp/pess, org sem sufixo, modelo, medidor de contexto colorido).

## Fora de escopo (deste sub-projeto)

- **Custo em US$** — Sub-projeto 2 (spec próprio).
- **Branch git** por sessão — dado está no transcript, mas não entra no layout
  aprovado; futuro.
- **Modelo/contexto do Codex** — sem transcript equivalente aqui; Codex mantém
  `agente · status` + org/corp.
