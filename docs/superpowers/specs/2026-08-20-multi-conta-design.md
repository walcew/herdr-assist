# Multi-conta no herdr-assist — design

**Data:** 2026-08-20
**Estado:** proposto (aguardando implementação via plano)

## Problema

O usuário roda dois logins do mesmo provedor na mesma máquina — uma conta
pessoal em `~/.claude` e uma de trabalho em `~/.claude-sos` (apontada por
`CLAUDE_CONFIG_DIR` no ambiente de cada pane de work). A ponte hoje lê caminhos
de credencial **fixos** (`~/.claude/.credentials.json`, `~/.codex/auth.json`),
então o painel só enxerga **uma** conta: a Dash mostra o uso da pessoal e ignora
a de work, e nada na aba Sessões diz sob qual conta cada agente está rodando.

Objetivo: **mostrar as contas conectadas e as métricas de cada uma** quando há
múltiplas contas em uso, e **taguear cada sessão** com sua conta.

## Descoberta (o que foi verificado ao vivo)

A cadeia inteira que sustenta este design foi provada nesta máquina (Windows):

1. **PID do pane** — `herdr pane process-info --pane <id>` devolve
   `foreground_processes[].pid` e `shell_pid`. Ex.: pane `wZ:p1` (procergs) →
   `claude.exe` pid `75336`; pane de codex → `node.exe` pid `1588`. A API expõe
   o pid diretamente; sem heurística.
2. **Config-dir da conta** — lendo o ambiente do pid: o pane procergs tem
   `CLAUDE_CONFIG_DIR=C:\Users\bruno\.claude-sos`. `pane.list`/`process-info`
   **não** trazem env nem via `agent_session`; o env vem do processo.
3. **Credencial e e-mail por conta** — `.claude-sos` é um config-dir real
   (tem `projects/`, `history.jsonl`, `.claude.json`). O e-mail logado está em
   `<config-dir>/.claude.json` no campo `oauthAccount.emailAddress` (confirmado
   em `~/.claude.json` e `~/.claude-sos/.claude.json`).

### Por que não o caminho direto/env-var da API

`pane.list` não expõe `pid` nem env. O `agent_session.value` (UUID da sessão
Claude) permitiria um match por filesystem (`<dir>/projects/<cwd>/<uuid>.jsonl`),
mas nem todo pane traz o UUID no snapshot (codex nunca; alguns panes claude
ainda não). Ler o env do processo via `process-info` é mais robusto e cobre os
dois provedores. **Decisão:** env do processo é a fonte primária.

## Arquitetura

Três camadas, cada uma com uma responsabilidade:

1. **Ponte (Python)** — descobre contas, coleta uso por conta, mapeia pane→conta.
2. **Protocolo de fio** — campo `account` retrocompatível em `limits` e `agents`.
3. **Firmware (C/LVGL)** — modelo e UI ganham a dimensão conta.

### 1. Ponte — leitura de ambiente por processo

Novo módulo isolado `proc_env` (função `read_process_env(pid) -> dict[str,str]`),
cross-platform, testável por injeção:

- **Windows:** `OpenProcess(PROCESS_QUERY_INFORMATION|PROCESS_VM_READ)` +
  `NtQueryInformationProcess(ProcessBasicInformation)` → PEB → `ProcessParameters`
  (offset `0x20`) → `Environment` (`0x80`) e `EnvironmentSize` (`0x3F0`);
  decodifica UTF-16LE até o duplo-NUL. Só stdlib (`ctypes`). Prova de conceito
  já rodada.
- **Linux:** lê `/proc/<pid>/environ` (pares separados por NUL).
- **macOS:** `ps eww -p <pid>` e parse do bloco `KEY=VAL`.
- Qualquer falha (permissão, processo morto) → `{}`; nunca levanta.

**Extração mínima:** a função de descoberta só olha `CLAUDE_CONFIG_DIR` e
`CODEX_HOME` no dict retornado. Nenhum outro valor é guardado, logado ou
transmitido — em especial nada com `KEY`/`TOKEN`/`SECRET` no nome.

### 1b. Ponte — descoberta de contas

Novo passo, disparado quando a composição de panes muda (não a cada
reconciliação de 1s):

```
para cada pane p em pane.list com p.agent in {claude, codex}:
    info = process-info(p.pane_id)
    pid  = info.foreground_processes[0].pid
    env  = read_process_env(pid)            # cacheado por pid
    dir  = env.get(VAR[p.agent]) or DEFAULT_DIR[p.agent]
    accounts[(p.agent, canonical(dir))].panes.add(p.pane_id)
    pane_account[p.pane_id] = (p.agent, canonical(dir))
```

- `VAR = {claude: CLAUDE_CONFIG_DIR, codex: CODEX_HOME}`.
- `DEFAULT_DIR = {claude: ~/.claude, codex: ~/.codex}`.
- **A conta default de cada provedor é sempre incluída**, mesmo sem pane ativo
  usando-a — não regride o comportamento atual (uso pessoal aparece sempre).
- `canonical(dir)` = path absoluto normalizado (chave de identidade e dedup).
- **Cache:** `pid → env` e `pane_id → (foreground pid)`; só relê env quando o
  pid do foreground muda. Env-read não entra no laço de status.

### 1c. Ponte — coleta por conta e e-mail

`collect_claude(config_dir)` e `collect_codex(config_dir)` passam a receber o
diretório (hoje são constantes globais). Mudanças de path:

- Claude: credencial em `<config_dir>/.credentials.json`; e-mail em
  `<config_dir>/.claude.json` → `oauthAccount.emailAddress`, com fallback para
  `~/.claude.json` quando o arquivo não existe no config_dir (caso default).
- Codex: credencial em `<config_dir>/auth.json`; e-mail decodificado do
  `tokens.id_token` (JWT: base64url do payload, **sem** validar assinatura —
  só extração de claim), fallback para `account_id`/plano.

Cada provedor coletado ganha `account` = e-mail (ou fallback), clipado a 32
bytes por `clip()`. Os três estados por provedor (ok / stale com `stale_since` /
omitido por credencial ausente) passam a valer **por conta**. Rótulo repetido
(mesmo e-mail em dois dirs) é desambiguado pelo basename do config-dir.

`collect_limits()` itera sobre as contas descobertas (não mais sobre a dupla
fixa de provedores). Ordem determinística (provider, depois config_dir ordenado)
para o dedup de snapshot continuar válido.

### 2. Protocolo de fio

Campos novos, ambos opcionais (ausência = comportamento antigo):

- `limits`: cada item de `providers[]` ganha `"account": "<email>"`.
- `agents`: cada item de `agents[]` ganha `"account": "<email>"` (a conta do
  pane, do mapa `pane_account`). Pane sem conta resolvida → campo ausente.

### 3. Firmware

**Modelo (`herdr_model.h`):**

- `herdr_limits_t` ganha `char account[33];` (32 + NUL, casa com o clip da ponte).
- `herdr_agent_t` ganha `char account[33];`.
- Parsers em `herdr_conn.c` leem o campo novo quando presente; ausente → `""`.

**UI — Dash (`herdr_ui.c`, `add_limits_card`/`rebuild_dash_cards`):**

- Detecta multi-conta por provedor: se há >1 card com o mesmo `name` (provedor),
  o título do card passa a mostrar a conta. Reaproveita o padrão de `show_host`
  (`"<host> · <provedor>"` → `"<provedor> · <conta>"` ou combinação com host
  quando ambos variam). A conta é truncada pela largura do label.

**UI — Sessões:**

- Cada linha/detalhe de agente mostra a conta quando há mais de uma conta em
  jogo (chip discreto ou sufixo `· <conta>`), seguindo o mesmo critério de
  "só desambigua quando precisa".

**i18n:** rótulos novos (se houver texto fixo) entram em `i18n.h`/`i18n.c` nos
idiomas já suportados.

### 4. Performance e segurança

- Env-read só na mudança de composição de panes, cacheado por pid. Custo por
  ciclo normal permanece o de hoje.
- Só o path do config-dir sai do env. Nada sensível é lido/logado/transmitido.
- Falha de env-read degrada para a conta default; nunca derruba a ponte nem a
  coleta.

## Edge cases

- **Pane sem processo de agente em foreground** (shell ocioso): sem var → conta
  default do provedor, ou sem tag se o provedor não estiver identificado.
- **Provedor sem e-mail** (arquivo ausente/JWT ilegível): fallback para basename
  do config-dir (Claude) ou `account_id`/plano (Codex).
- **Mesmo e-mail em dois dirs:** identidade é o config-dir canônico; rótulo
  desambiguado pelo basename.
- **Só panes de work abertos:** a conta default ainda aparece (uso pessoal não
  some); as de work aparecem por estarem em uso.
- **Estouro de `HERDR_MAX_PROVIDERS`** (4 por host): 2 contas × 2 provedores já
  enche o teto. Manter o cap e cortar de forma determinística, logando o que
  ficou de fora (sem truncar em silêncio).

## Testes

- **Ponte (`scripts/*_test` ou pytest com fixtures):**
  - `read_process_env` mockado por dict-por-pid; descoberta produz o conjunto
    esperado de contas a partir de um `pane.list` + `process-info` de fixture.
  - `collect_claude`/`collect_codex` com `.credentials.json`/`.claude.json`/
    `auth.json` de fixture: assertar `account` correto e os três estados.
  - Snapshot de `limits`/`agents` determinístico (dedup estável) com duas contas.
- **Firmware (host-test):** estender o teste da Dash para dois cards do mesmo
  provedor com contas distintas; assertar que o título desambigua e que o parser
  aceita payload sem `account` (retrocompat).

## Fora de escopo

- Configurar/trocar contas pela UI do painel (a config vem do ambiente dos panes).
- Rótulo manual por conta na config do plugin (e-mail automático cobre o caso).
- Multi-conta para provedores além de Claude e Codex.
