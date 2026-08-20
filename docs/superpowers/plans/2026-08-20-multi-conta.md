# Multi-conta no herdr-assist — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Mostrar o uso de cada conta conectada (mesmo quando um provedor tem várias contas) e taguear cada sessão com sua conta, descobrindo as contas pelo ambiente do processo de cada pane.

**Architecture:** A ponte (Python) lê `CLAUDE_CONFIG_DIR`/`CODEX_HOME` do ambiente do processo em foreground de cada pane (pid via `herdr pane process-info`), deriva o config-dir de cada conta, coleta uso por config-dir e resolve o e-mail como rótulo. O protocolo ganha um campo `account` retrocompatível em `limits` e `agents`. O firmware guarda `account` no modelo e desambigua na Dash e na aba Sessões quando há mais de uma conta.

**Tech Stack:** Python 3.9+ stdlib (ponte), C11 + LVGL 8.4 + cJSON (firmware/ESP32-S3), PlatformIO.

**Spec:** `docs/superpowers/specs/2026-08-20-multi-conta-design.md`

## Global Constraints

- Ponte: **só stdlib**, compatível com Python 3.9 (python3 de fábrica do macOS). Nenhuma dependência nova.
- Segurança: do ambiente do processo, **só** os valores de `CLAUDE_CONFIG_DIR`/`CODEX_HOME` podem ser extraídos/guardados. Nada com `KEY`/`TOKEN`/`SECRET` no nome pode ser lido, logado ou transmitido.
- Retrocompatibilidade: campo `account` ausente no payload = comportamento antigo. Nunca quebrar um painel/ponte de versão diferente.
- Determinismo: a serialização de `limits`/`agents` precisa ser estável (mesma ordem sempre) — o dedup de snapshot da ponte depende disso.
- Firmware: buffers de tamanho fixo. `account` = `char[33]` (32 bytes + NUL), e a ponte clipa o e-mail em 32 bytes com `clip()`.
- Teto de cards de uso por host: `HERDR_MAX_PROVIDERS = 8` (provedor × conta), espelhado na ponte.
- Env-read só quando a composição de panes muda; cacheado por pid. Falha degrada gracioso, nunca derruba a ponte.
- Idioma dos comentários/commits: português (BR), com acentuação correta.

---

## File Structure

**Ponte (Python), em `plugin/`:**
- `proc_env.py` (NOVO) — leitura do ambiente de um processo por pid, cross-platform. Parsers puros por SO + dispatcher.
- `accounts.py` (NOVO) — resolução de config-dir, e-mail da conta e descoberta de contas a partir dos panes (com callables injetáveis, sem subprocess).
- `herdr_bridge.py` (MODIFICAR) — coleta por config-dir, `collect_limits` sobre as contas descobertas, `account` nos payloads, wiring de process-info + cache.
- `tests/__init__.py` (NOVO) — pacote de testes.
- `tests/test_proc_env.py` (NOVO) — parsers de ambiente.
- `tests/test_accounts.py` (NOVO) — config-dir, e-mail, descoberta.
- `tests/test_bridge_limits.py` (NOVO) — `collect_limits` com fixtures.
- `tests/fixtures/` (NOVO) — config-dirs de fixture (`.claude.json`, `.credentials.json`, `auth.json`).

**Firmware (C), em `src/`:**
- `herdr_model.h` (MODIFICAR) — `account` em `herdr_limits_t` e `herdr_agent_t`; `HERDR_MAX_PROVIDERS` → 8.
- `herdr_conn.c` (MODIFICAR) — ler `account` em `handle_agents` e `handle_limits`.
- `herdr_ui.c` (MODIFICAR) — desambiguação por conta na Dash (`add_limits_card`/`rebuild_dash_cards`) e chip de conta na aba Sessões.

**CI:**
- `.github/workflows/build.yml` (MODIFICAR) — passo `python -m unittest` no job `host-test`.

---

## Task 1: Parsers de ambiente por SO (`proc_env.py`)

**Files:**
- Create: `plugin/proc_env.py`
- Create: `plugin/tests/__init__.py`
- Test: `plugin/tests/test_proc_env.py`

**Interfaces:**
- Produces:
  - `parse_proc_environ(raw: bytes) -> dict[str, str]` — formato Linux `/proc/<pid>/environ` (pares `KEY=VAL` separados por NUL).
  - `parse_ps_env(text: str) -> dict[str, str]` — saída de `ps eww -p <pid> -o command=` (bloco `KEY=VAL` separado por espaço, valores sem espaço no caso do CONFIG_DIR).
  - `read_process_env(pid: int) -> dict[str, str]` — dispatcher por `os.name`; `{}` em qualquer falha.

- [ ] **Step 1: Escrever o teste que falha**

```python
# plugin/tests/test_proc_env.py
# Rode SEMPRE de dentro de plugin/ (mesmo sys.path que a ponte em produção):
#   cd plugin && python -m unittest tests.test_proc_env -v
import unittest
from proc_env import parse_proc_environ, parse_ps_env


class TestParseProcEnviron(unittest.TestCase):
    def test_pares_separados_por_nul(self):
        raw = b"PATH=/usr/bin\x00CLAUDE_CONFIG_DIR=/home/u/.claude-work\x00"
        env = parse_proc_environ(raw)
        self.assertEqual(env["CLAUDE_CONFIG_DIR"], "/home/u/.claude-work")
        self.assertEqual(env["PATH"], "/usr/bin")

    def test_ignora_entrada_sem_igual(self):
        raw = b"SOZINHO\x00A=1\x00"
        self.assertEqual(parse_proc_environ(raw), {"A": "1"})

    def test_valor_com_igual_preserva_o_resto(self):
        raw = b"Q=a=b=c\x00"
        self.assertEqual(parse_proc_environ(raw)["Q"], "a=b=c")


class TestParsePsEnv(unittest.TestCase):
    def test_extrai_config_dir(self):
        text = "PATH=/usr/bin CODEX_HOME=/Users/u/.codex-work TERM=xterm"
        env = parse_ps_env(text)
        self.assertEqual(env["CODEX_HOME"], "/Users/u/.codex-work")


if __name__ == "__main__":
    unittest.main()
```

- [ ] **Step 2: Rodar e ver falhar**

Run: `cd plugin && python -m unittest tests.test_proc_env -v`
Expected: FAIL com `ModuleNotFoundError: No module named 'proc_env'`

- [ ] **Step 3: Implementar o mínimo**

```python
# plugin/proc_env.py
#!/usr/bin/env python3
"""Lê o ambiente de um processo por pid, cross-platform, só com stdlib.

A ponte usa isto para descobrir CLAUDE_CONFIG_DIR/CODEX_HOME do processo de
cada pane. Nenhum valor além do config-dir é consumido por quem chama (ver
accounts.discover); esta camada só devolve o dict cru.
"""
from __future__ import annotations

import os


def parse_proc_environ(raw: bytes) -> dict:
    """Linux: /proc/<pid>/environ é KEY=VAL separado por NUL."""
    env = {}
    for entry in raw.split(b"\x00"):
        if not entry or b"=" not in entry:
            continue
        k, _, v = entry.partition(b"=")
        if k:
            env[k.decode("utf-8", "replace")] = v.decode("utf-8", "replace")
    return env


def parse_ps_env(text: str) -> dict:
    """macOS: `ps eww -o command=` cospe o argv seguido do env, tudo por espaço.

    Só interessa extrair KEY=VAL; o config-dir não tem espaço, então o split
    simples basta para o que a descoberta precisa.
    """
    env = {}
    for tok in text.split():
        if "=" in tok:
            k, _, v = tok.partition("=")
            if k:
                env[k] = v
    return env


def _read_windows(pid: int) -> dict:
    """Windows: caminha o PEB do processo (ctypes) e lê o bloco de ambiente.

    Só processos do mesmo usuário; sem elevação. Qualquer falha vira {}.
    """
    import ctypes as C
    ntdll = C.WinDLL("ntdll")
    k32 = C.WinDLL("kernel32", use_last_error=True)
    PROCESS_QUERY_INFORMATION = 0x0400
    PROCESS_VM_READ = 0x0010

    class PBI(C.Structure):
        _fields_ = [("Reserved1", C.c_void_p), ("PebBaseAddress", C.c_void_p),
                    ("Reserved2", C.c_void_p * 2), ("UniqueProcessId", C.c_void_p),
                    ("Reserved3", C.c_void_p)]

    h = k32.OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, False, pid)
    if not h:
        return {}
    try:
        def rd(addr, size):
            buf = (C.c_char * size)()
            got = C.c_size_t(0)
            if not k32.ReadProcessMemory(h, C.c_void_p(addr), buf,
                                         C.c_size_t(size), C.byref(got)):
                raise OSError(C.get_last_error())
            return buf.raw[:got.value]

        def ptr(addr):
            return int.from_bytes(rd(addr, 8), "little")

        pbi = PBI()
        if ntdll.NtQueryInformationProcess(h, 0, C.byref(pbi), C.sizeof(pbi), None) != 0:
            return {}
        peb = C.cast(pbi.PebBaseAddress, C.c_void_p).value
        params = ptr(peb + 0x20)        # PEB.ProcessParameters
        env_ptr = ptr(params + 0x80)    # RTL_USER_PROCESS_PARAMETERS.Environment
        env_len = int.from_bytes(rd(params + 0x3F0, 8), "little")  # EnvironmentSize
        if env_len <= 0 or env_len > (1 << 20):
            env_len = 1 << 16
        text = rd(env_ptr, env_len).decode("utf-16-le", "replace")
        env = {}
        for entry in text.split("\x00"):
            if entry and "=" in entry[1:]:
                k, _, v = entry.partition("=")
                env[k] = v
        return env
    except OSError:
        return {}
    finally:
        k32.CloseHandle(h)


def read_process_env(pid: int) -> dict:
    """Ambiente do processo `pid`, pelo mecanismo do SO. {} em qualquer falha."""
    if not pid or pid <= 0:
        return {}
    try:
        if os.name == "nt":
            return _read_windows(pid)
        if os.path.isdir("/proc"):  # Linux
            with open("/proc/%d/environ" % pid, "rb") as fh:
                return parse_proc_environ(fh.read())
        # macOS e demais BSDs
        import subprocess
        out = subprocess.run(["ps", "eww", "-o", "command=", "-p", str(pid)],
                             capture_output=True, timeout=5)
        return parse_ps_env(out.stdout.decode("utf-8", "replace"))
    except (OSError, ValueError, subprocess.SubprocessError):
        return {}
```

- [ ] **Step 4: Rodar e ver passar**

Run: `cd plugin && python -m unittest tests.test_proc_env -v`
Expected: PASS (todos)

- [ ] **Step 5: Commit**

```bash
git add plugin/proc_env.py plugin/tests/__init__.py plugin/tests/test_proc_env.py
git commit -m "feat(ponte): leitura de ambiente de processo por pid (cross-platform)"
```

---

## Task 2: Descoberta de contas (`accounts.py`)

**Files:**
- Create: `plugin/accounts.py`
- Test: `plugin/tests/test_accounts.py`
- Create: `plugin/tests/fixtures/personal/.claude.json`
- Create: `plugin/tests/fixtures/work/.claude.json`

**Interfaces:**
- Consumes: nada de tasks anteriores (usa callables injetados).
- Produces:
  - `CONFIG_VAR: dict` = `{"claude": "CLAUDE_CONFIG_DIR", "codex": "CODEX_HOME"}`
  - `default_dir(agent: str, home: str) -> str`
  - `resolve_config_dir(agent: str, env: dict, home: str) -> str` — normalizado/absoluto.
  - `read_account_email(agent: str, config_dir: str, home: str) -> str` — `""` se não resolver.
  - `discover(panes: list, get_pid, get_env, home: str) -> tuple[dict, set]` — devolve `(pane_account, account_dirs)`. `pane_account: {pane_id: (agent, config_dir)}`. `account_dirs: set[(agent, config_dir)]`, sempre com os defaults de claude e codex.

- [ ] **Step 1: Escrever fixtures e o teste que falha**

`plugin/tests/fixtures/personal/.claude.json`:
```json
{"oauthAccount": {"emailAddress": "bruno@pessoal.dev"}}
```

`plugin/tests/fixtures/work/.claude.json`:
```json
{"oauthAccount": {"emailAddress": "bruno@work.gov.br"}}
```

```python
# plugin/tests/test_accounts.py
# cd plugin && python -m unittest tests.test_accounts -v
import os
import unittest
import accounts

FIX = os.path.join(os.path.dirname(__file__), "fixtures")


class TestResolveConfigDir(unittest.TestCase):
    def test_usa_env_quando_presente(self):
        env = {"CLAUDE_CONFIG_DIR": "/x/.claude-sos"}
        self.assertEqual(accounts.resolve_config_dir("claude", env, "/home/u"),
                         os.path.normpath(os.path.abspath("/x/.claude-sos")))

    def test_cai_no_default_sem_env(self):
        got = accounts.resolve_config_dir("codex", {}, "/home/u")
        self.assertEqual(got, os.path.normpath(os.path.abspath("/home/u/.codex")))


class TestReadEmail(unittest.TestCase):
    def test_le_email_do_config_dir(self):
        got = accounts.read_account_email("claude", os.path.join(FIX, "work"), FIX)
        self.assertEqual(got, "bruno@work.gov.br")

    def test_email_vazio_quando_ausente(self):
        got = accounts.read_account_email("claude", os.path.join(FIX, "naoexiste"), FIX)
        self.assertEqual(got, "")


class TestDiscover(unittest.TestCase):
    def test_descobre_contas_e_mapeia_panes(self):
        panes = [
            {"pane_id": "wY:p1", "agent": "claude"},   # pessoal (sem env)
            {"pane_id": "wZ:p1", "agent": "claude"},   # work (env)
            {"pane_id": "wX:p1", "agent": "codex"},    # codex default
            {"pane_id": "wA:p1", "agent": None},       # sem agente, ignorado
        ]
        envs = {75336: {"CLAUDE_CONFIG_DIR": "/h/.claude-sos"}}
        pids = {"wY:p1": 100, "wZ:p1": 75336, "wX:p1": 200}
        pane_account, account_dirs = accounts.discover(
            panes, lambda pid: pids.get(pid), lambda pid: envs.get(pid, {}), "/h")

        self.assertEqual(pane_account["wZ:p1"],
                         ("claude", os.path.normpath(os.path.abspath("/h/.claude-sos"))))
        self.assertEqual(pane_account["wY:p1"],
                         ("claude", os.path.normpath(os.path.abspath("/h/.claude"))))
        self.assertNotIn("wA:p1", pane_account)
        # defaults sempre presentes + a de work
        self.assertIn(("claude", os.path.normpath(os.path.abspath("/h/.claude"))), account_dirs)
        self.assertIn(("codex", os.path.normpath(os.path.abspath("/h/.codex"))), account_dirs)
        self.assertIn(("claude", os.path.normpath(os.path.abspath("/h/.claude-sos"))), account_dirs)


if __name__ == "__main__":
    unittest.main()
```

- [ ] **Step 2: Rodar e ver falhar**

Run: `cd plugin && python -m unittest tests.test_accounts -v`
Expected: FAIL com `ModuleNotFoundError: No module named 'accounts'`

- [ ] **Step 3: Implementar o mínimo**

```python
# plugin/accounts.py
#!/usr/bin/env python3
"""Descoberta de contas de IA a partir dos panes do Herdr.

Cada pane com agente roda sob um config-dir (CLAUDE_CONFIG_DIR / CODEX_HOME);
o conjunto de config-dirs distintos são as contas conectadas. O e-mail logado
serve de rótulo. Toda a lógica é pura e recebe callables injetados — a ponte
passa os reais (process-info + proc_env); os testes passam dicts.
"""
from __future__ import annotations

import base64
import json
import os

CONFIG_VAR = {"claude": "CLAUDE_CONFIG_DIR", "codex": "CODEX_HOME"}
_DEFAULT = {"claude": ".claude", "codex": ".codex"}


def default_dir(agent: str, home: str) -> str:
    return os.path.normpath(os.path.abspath(os.path.join(home, _DEFAULT[agent])))


def resolve_config_dir(agent: str, env: dict, home: str) -> str:
    d = env.get(CONFIG_VAR[agent]) if env else None
    if not d:
        return default_dir(agent, home)
    return os.path.normpath(os.path.abspath(os.path.expanduser(d)))


def _jwt_email(id_token: str) -> str:
    """E-mail do claim de um JWT, sem validar assinatura (só extração)."""
    try:
        payload = id_token.split(".")[1]
        payload += "=" * (-len(payload) % 4)
        data = json.loads(base64.urlsafe_b64decode(payload))
    except (ValueError, IndexError, json.JSONDecodeError):
        return ""
    if isinstance(data.get("email"), str):
        return data["email"]
    for v in data.values():  # claims namespaced: pega o primeiro que pareça e-mail
        if isinstance(v, str) and "@" in v:
            return v
        if isinstance(v, dict) and isinstance(v.get("email"), str):
            return v["email"]
    return ""


def read_account_email(agent: str, config_dir: str, home: str) -> str:
    """E-mail logado da conta. `""` quando não dá para resolver."""
    try:
        if agent == "claude":
            path = os.path.join(config_dir, ".claude.json")
            if not os.path.exists(path):  # conta default guarda no home
                path = os.path.join(home, ".claude.json")
            with open(path, encoding="utf-8") as fh:
                data = json.load(fh)
            return ((data.get("oauthAccount") or {}).get("emailAddress") or "")
        if agent == "codex":
            with open(os.path.join(config_dir, "auth.json"), encoding="utf-8") as fh:
                tok = (json.load(fh).get("tokens") or {})
            return _jwt_email(tok.get("id_token", "")) or tok.get("account_id", "")
    except (OSError, ValueError, json.JSONDecodeError):
        return ""
    return ""


def discover(panes, get_pid, get_env, home: str):
    """Mapa pane->conta e o conjunto de contas (config-dirs) em uso.

    get_pid(pane_id)->int|None e get_env(pid)->dict são injetados. Os defaults
    de claude e codex entram sempre, para o uso pessoal não sumir quando só há
    panes de outra conta abertos.
    """
    pane_account = {}
    account_dirs = {(a, default_dir(a, home)) for a in CONFIG_VAR}
    for p in panes:
        agent = p.get("agent")
        if agent not in CONFIG_VAR:
            continue
        pid = get_pid(p["pane_id"])
        env = get_env(pid) if pid else {}
        cdir = resolve_config_dir(agent, env, home)
        pane_account[p["pane_id"]] = (agent, cdir)
        account_dirs.add((agent, cdir))
    return pane_account, account_dirs
```

- [ ] **Step 4: Rodar e ver passar**

Run: `cd plugin && python -m unittest tests.test_accounts -v`
Expected: PASS (todos)

- [ ] **Step 5: Commit**

```bash
git add plugin/accounts.py plugin/tests/test_accounts.py plugin/tests/fixtures/
git commit -m "feat(ponte): descoberta de contas por config-dir e e-mail como rótulo"
```

---

## Task 3: Coleta de uso por config-dir (`herdr_bridge.py`)

**Files:**
- Modify: `plugin/herdr_bridge.py` (funções `collect_claude`, `collect_codex`, `collect_limits`; dicts `limits_last_good`/`limits_ok`)
- Test: `plugin/tests/test_bridge_limits.py`
- Create: `plugin/tests/fixtures/personal/.credentials.json`
- Create: `plugin/tests/fixtures/work/.credentials.json`

**Interfaces:**
- Consumes: `accounts.read_account_email`, `accounts.default_dir` (Task 2).
- Produces:
  - `collect_claude(config_dir: str) -> dict` — inclui `"account"`.
  - `collect_codex(config_dir: str) -> dict` — inclui `"account"`.
  - `collect_limits(account_dirs) -> list` — `account_dirs`: iterável de `(agent, config_dir)`; um item de provedor por conta, com `account`, ordenado e cortado em `LIMITS_MAX_CARDS = 8`.

- [ ] **Step 1: Escrever fixtures e o teste que falha**

`plugin/tests/fixtures/personal/.credentials.json`:
```json
{"claudeAiOauth": {"accessToken": "x", "expiresAt": 9999999999000, "rateLimitTier": "default_claude_max_20x"}}
```
`plugin/tests/fixtures/work/.credentials.json`:
```json
{"claudeAiOauth": {"accessToken": "y", "expiresAt": 9999999999000, "rateLimitTier": "default_claude_pro"}}
```

```python
# plugin/tests/test_bridge_limits.py
# cd plugin && python -m unittest tests.test_bridge_limits -v
import os
import unittest
from unittest import mock

import herdr_bridge as b

FIX = os.path.join(os.path.dirname(__file__), "fixtures")


def fake_usage(url, headers):
    # o endpoint do Claude: dois limits normalizados
    return {"limits": [
        {"kind": "session", "percent": 12.4, "resets_at": "2026-08-20T10:00:00Z"},
        {"kind": "weekly_all", "percent": 40.0, "resets_at": "2026-08-27T10:00:00Z"},
    ]}


class TestCollectPorConta(unittest.TestCase):
    def test_claude_inclui_account(self):
        with mock.patch.object(b, "fetch_json", fake_usage):
            cur = b.collect_claude(os.path.join(FIX, "work"))
        self.assertEqual(cur["name"], "Claude")
        self.assertEqual(cur["account"], "bruno@work.gov.br")
        self.assertEqual(len(cur["limits"]), 2)

    def test_collect_limits_uma_por_conta_ordenado(self):
        dirs = [("claude", os.path.join(FIX, "personal")),
                ("claude", os.path.join(FIX, "work"))]
        with mock.patch.object(b, "fetch_json", fake_usage), \
             mock.patch.object(b, "HOME", FIX):
            providers = b.collect_limits(dirs)
        emails = [p["account"] for p in providers]
        self.assertEqual(emails, sorted(emails))               # determinístico
        self.assertIn("bruno@work.gov.br", emails)
        self.assertIn("bruno@pessoal.dev", emails)


if __name__ == "__main__":
    unittest.main()
```

- [ ] **Step 2: Rodar e ver falhar**

Run: `cd plugin && python -m unittest tests.test_bridge_limits -v`
Expected: FAIL (`collect_claude` sem parâmetro / sem chave `account`).

- [ ] **Step 3: Implementar as mudanças**

No topo de `herdr_bridge.py`, junto dos imports do projeto (a ponte roda com
cwd=`plugin/`, então `accounts` é importável direto — mesmo sys.path dos testes,
que rodam de dentro de `plugin/`):
```python
from accounts import read_account_email, default_dir, CONFIG_VAR  # noqa: E402
```

Trocar as constantes fixas por helper de caminho e um HOME injetável:
```python
HOME = os.path.expanduser("~")
LIMITS_MAX_CARDS = 8   # teto espelhado no firmware (HERDR_MAX_PROVIDERS)

def _cred_path(agent: str, config_dir: str) -> str:
    return os.path.join(config_dir, ".credentials.json" if agent == "claude"
                        else "auth.json")
```

`collect_claude` passa a receber `config_dir` (era `CLAUDE_CRED` fixo):
```python
def collect_claude(config_dir: str) -> dict:
    with open(os.path.join(config_dir, ".credentials.json")) as f:
        cred = json.load(f)["claudeAiOauth"]
    if cred.get("expiresAt", 0) / 1000 <= time.time():
        raise RuntimeError("token expirado")
    data = fetch_json("https://api.anthropic.com/api/oauth/usage",
                      {"Authorization": "Bearer " + cred["accessToken"],
                       "anthropic-beta": "oauth-2025-04-20"})
    rows = []
    for lim in data.get("limits", [])[:4]:
        kind = lim.get("kind", "")
        if kind == "session":
            label = "5h"
        elif kind == "weekly_all":
            label = "7d"
        elif kind == "weekly_scoped":
            model = ((lim.get("scope") or {}).get("model") or {}).get("display_name") or "?"
            label = clip("7d " + model, 16)
        else:
            label = clip(kind, 16)
        rows.append({"label": label, "pct": int(round(lim.get("percent") or 0)),
                     "resets_at": iso_epoch(lim["resets_at"]) if lim.get("resets_at") else 0})
    tier = cred.get("rateLimitTier", "")
    plan = (tier.split("claude_")[-1].replace("_", " ").capitalize()
            if tier else cred.get("subscriptionType", "").capitalize())
    return {"name": "Claude", "plan": plan, "limits": rows,
            "account": clip(read_account_email("claude", config_dir, HOME), 32)}
```

`collect_codex` idem (recebe `config_dir`, lê `<dir>/auth.json`, e ao fim):
```python
    return {"name": "Codex", "plan": (data.get("plan_type") or "").capitalize(),
            "limits": rows,
            "account": clip(read_account_email("codex", config_dir, HOME), 32)}
```

`collect_limits` itera as contas descobertas (não mais a dupla fixa). As chaves de `limits_last_good`/`limits_ok` passam a ser `(agent, config_dir)`:
```python
def collect_limits(account_dirs) -> list:
    """Um provedor por conta, degradando com honestidade e ordem determinística."""
    collectors = {"claude": collect_claude, "codex": collect_codex}
    providers = []
    for agent, cdir in sorted(account_dirs):
        key = (agent, cdir)
        path = _cred_path(agent, cdir)
        if not os.path.exists(path):
            limits_last_good.pop(key, None)
            limits_ok.pop(key, None)
            continue
        try:
            cur = collectors[agent](cdir)
            cur.update(ok=True, stale_since=0)
            limits_last_good[key] = {"data": {k: cur[k] for k in
                                     ("name", "plan", "limits", "account")},
                                     "at": int(time.time())}
            providers.append(cur)
            if limits_ok.get(key) is not True:
                log.info("limites %s [%s]: ok (%d janelas)",
                         agent, cur.get("account") or os.path.basename(cdir),
                         len(cur["limits"]))
            limits_ok[key] = True
        except Exception as e:
            reason = ("HTTP %d" % e.code if isinstance(e, urllib.error.HTTPError)
                      else str(e) or type(e).__name__)
            good = limits_last_good.get(key)
            if good:
                stale = dict(good["data"])
                stale.update(ok=False, stale_since=good["at"])
                providers.append(stale)
            if limits_ok.get(key) is not False:
                log.warning("limites %s [%s]: %s", agent, os.path.basename(cdir), reason)
            limits_ok[key] = False
    if len(providers) > LIMITS_MAX_CARDS:
        log.warning("cards de uso acima do teto (%d>%d): cortando",
                    len(providers), LIMITS_MAX_CARDS)
        providers = providers[:LIMITS_MAX_CARDS]
    return providers
```

Remover as constantes `CLAUDE_CRED`/`CODEX_AUTH` (não são mais usadas) — ou mantê-las só se algum outro ponto referenciar (verificar com grep).

- [ ] **Step 4: Rodar e ver passar**

Run: `cd plugin && python -m unittest tests.test_bridge_limits -v`
Expected: PASS

- [ ] **Step 5: Rodar a suíte inteira da ponte**

Run: `cd plugin && python -m unittest discover -s tests -v`
Expected: PASS (proc_env, accounts, bridge_limits)

- [ ] **Step 6: Commit**

```bash
git add plugin/herdr_bridge.py plugin/tests/test_bridge_limits.py plugin/tests/fixtures/
git commit -m "feat(ponte): coleta de uso por conta (config-dir) com e-mail no payload"
```

---

## Task 4: Wiring da descoberta na ponte (process-info + cache + tag)

**Files:**
- Modify: `plugin/herdr_bridge.py` (`cli_request`, `push_agents`, `limits_loop`, estado de cache; nova coroutine `pane_pid`)

**Interfaces:**
- Consumes: `accounts.discover` (Task 2), `proc_env.read_process_env` (Task 1), `collect_limits(account_dirs)` (Task 3).
- Produces: `push_agents` adiciona `account` a cada agente; `limits_loop` coleta sobre `account_dirs` descobertos. Estado global `pane_account`, `account_dirs`, caches `pane_pid_cache`, `pid_env_cache`.

> **Verificação:** esta task integra subprocess/ctypes reais — a lógica pura já é coberta pelas Tasks 1–3. Aqui a verificação é **manual, ao vivo** (ponte + `herdr pane list` reais), mais a suíte que não pode regredir.

- [ ] **Step 1: Adicionar `pane.process_info` ao transporte**

Em `cli_request`, antes do `log.warning` final:
```python
    if method == "pane.process_info":
        out = await cli_run(["pane", "process-info", "--pane", pane])
        if not out:
            return None
        try:
            return json.loads(out).get("result")
        except (ValueError, AttributeError):
            return None
```

- [ ] **Step 2: Estado e helper de descoberta**

Junto dos imports do topo, `import accounts` e `from proc_env import read_process_env`
(cwd=`plugin/`, importáveis direto). Junto dos globais (perto de `working_since`):
```python
pane_account: dict = {}          # pane_id -> (agent, config_dir)
account_dirs: set = set()        # {(agent, config_dir)} em uso + defaults
pane_pid_cache: dict = {}        # pane_id -> pid do foreground
pid_env_cache: dict = {}         # pid -> env (só o que interessa já filtrado)


async def pane_pid(pane_id: str):
    info = await herdr_request("pane.process_info", {"pane_id": pane_id})
    if not info:
        return None
    procs = (info.get("process_info") or {}).get("foreground_processes") or []
    return procs[0].get("pid") if procs else None


def _cached_env(pid):
    if pid not in pid_env_cache:
        env = read_process_env(pid)
        # guarda só os config-dirs — nada sensível fica em memória
        pid_env_cache[pid] = {k: env[k] for k in CONFIG_VAR.values() if k in env}
    return pid_env_cache[pid]


async def refresh_accounts(panes) -> None:
    """Recalcula pane_account/account_dirs. Chamada só quando o conjunto muda."""
    global pane_account, account_dirs

    async def get_pid(pid_pane):
        if pid_pane not in pane_pid_cache:
            pane_pid_cache[pid_pane] = await pane_pid(pid_pane)
        return pane_pid_cache[pid_pane]

    # discover() é síncrona; resolvemos os pids antes e passamos um lookup pronto
    pids = {}
    for p in panes:
        if p.get("agent") in CONFIG_VAR:
            pids[p["pane_id"]] = await get_pid(p["pane_id"])
    pane_account, account_dirs = accounts.discover(
        panes, lambda pane: pids.get(pane), _cached_env, HOME)
    # poda caches de panes/pids que sumiram
    live_panes = {p["pane_id"] for p in panes}
    for k in list(pane_pid_cache):
        if k not in live_panes:
            pane_pid_cache.pop(k, None)
    live_pids = set(pids.values())
    for k in list(pid_env_cache):
        if k not in live_pids:
            pid_env_cache.pop(k, None)
```
(`accounts` já importado no topo.)

- [ ] **Step 3: `push_agents` tagueia e dispara refresh na mudança**

Em `push_agents`, guardar o conjunto anterior e, quando `known_panes` mudar, chamar `refresh_accounts`. Depois de montar cada `a`, anexar a conta:
```python
        a = {
            "pane_id": pid,
            "agent": p.get("agent", ""),
            "status": status,
            "project": os.path.basename(p.get("cwd", "")),
            "workspace_id": p.get("workspace_id", ""),
        }
        acc = pane_account.get(pid)
        if acc:
            a["account"] = acc_email_cache.get(acc, "")
```
onde `acc_email_cache` mapeia `(agent, config_dir) -> email` (preenchido em `refresh_accounts` chamando `read_account_email`, para não reler arquivo a cada ciclo). Adicionar em `refresh_accounts`:
```python
    global acc_email_cache
    acc_email_cache = {(a, d): clip(read_account_email(a, d, HOME), 32)
                       for (a, d) in account_dirs}
```
e a chamada de refresh, logo após recomputar `known_panes`:
```python
    new_set = {a["pane_id"] for a in agents}
    if new_set != _prev_agent_panes[0]:
        _prev_agent_panes[0] = new_set
        await refresh_accounts(panes)
```
com `_prev_agent_panes = [set()]` no escopo do módulo. (Recalcular a partir de `panes`, não só dos que têm agente, para pegar troca de composição.)

- [ ] **Step 4: `limits_loop` coleta sobre as contas descobertas**

Trocar `collect_limits()` por `collect_limits(account_dirs)`:
```python
            providers = await asyncio.get_running_loop().run_in_executor(
                None, collect_limits, set(account_dirs))
```
Se `account_dirs` estiver vazio (nenhum refresh ainda), o `collect_limits` deve ao menos coletar os defaults — garantir semeando `account_dirs` com os defaults na inicialização:
```python
account_dirs = {(a, default_dir(a, HOME)) for a in CONFIG_VAR}
```

- [ ] **Step 5: Verificação manual ao vivo**

Run (com Herdr rodando e ao menos um pane de cada conta):
```bash
cd plugin && python herdr_bridge.py
```
Esperado no log: `limites claude [bruno@work.gov.br]: ok (...)` e `limites claude [bruno@pessoal.dev]: ok (...)` — duas contas. Conferir que um cliente TCP recebe `providers[]` com dois itens Claude e `account` distinto, e `agents[]` com `account` por pane. Encerrar com Ctrl-C.

- [ ] **Step 6: Suíte não regride**

Run: `cd plugin && python -m unittest discover -s tests -v`
Expected: PASS

- [ ] **Step 7: Commit**

```bash
git add plugin/herdr_bridge.py
git commit -m "feat(ponte): descobre contas por pane (process-info+env), cache e tag em agents/limits"
```

---

## Task 5: Modelo do firmware — campo `account` e teto de cards

**Files:**
- Modify: `src/herdr_model.h` (structs `herdr_agent_t`, `herdr_limits_t`; `HERDR_MAX_PROVIDERS`)

**Interfaces:**
- Produces: `herdr_agent_t.account[33]`, `herdr_limits_t.account[33]`, `HERDR_MAX_PROVIDERS == 8`.

> **Verificação:** o modelo é consumido por `herdr_conn.c`/`herdr_ui.c`; a prova é o build (`pio run`) nas Tasks seguintes. Aqui só a mudança de struct.

- [ ] **Step 1: Adicionar os campos e subir o teto**

Em `herdr_model.h`:
- Em `herdr_agent_t`, após `uint32_t since;`:
```c
    char    account[33];   /* e-mail da conta (config-dir); "" se desconhecida */
```
- Em `herdr_limits_t`, após `uint8_t host;`:
```c
    char    account[33];   /* e-mail da conta; distingue contas do mesmo provedor */
```
- Trocar o define:
```c
#define HERDR_MAX_PROVIDERS   8    /* cards de uso por host: provedor × conta */
```
Atualizar o comentário da linha para refletir "provedor × conta".

- [ ] **Step 2: Build de fumaça**

Run: `pio run -d cardputer`
Expected: compila sem erro (structs maiores; sem uso ainda de `account`).

- [ ] **Step 3: Commit**

```bash
git add src/herdr_model.h
git commit -m "feat(fw): campo account no modelo e teto de cards por host = 8"
```

---

## Task 6: Parse do firmware — ler `account`

**Files:**
- Modify: `src/herdr_conn.c` (`handle_agents` ~L106-124, `handle_limits` ~L149-158)

**Interfaces:**
- Consumes: `herdr_agent_t.account`, `herdr_limits_t.account` (Task 5).

> **Verificação:** parse via cJSON não é host-testável (como `discovery_test`, manual). Prova = build + verificação ao vivo com a ponte da Task 4.

- [ ] **Step 1: Ler `account` em `handle_agents`**

Após o bloco que lê `workspace_id`:
```c
        if (cJSON_IsString((f = cJSON_GetObjectItem(item, "account")))) {
            strncpy(a->account, f->valuestring, sizeof(a->account) - 1);
        }
```
(o `memset` no início já zera; campo ausente fica `""`.)

- [ ] **Step 2: Ler `account` em `handle_limits`**

Após o bloco que lê `plan`:
```c
        if (cJSON_IsString((f = cJSON_GetObjectItem(item, "account")))) {
            strncpy(l->account, f->valuestring, sizeof(l->account) - 1);
        }
```

- [ ] **Step 3: Build**

Run: `pio run -d cardputer`
Expected: compila sem erro.

- [ ] **Step 4: Commit**

```bash
git add src/herdr_conn.c
git commit -m "feat(fw): parse do campo account em agents e limits"
```

---

## Task 7: Dash — desambiguar cards por conta

**Files:**
- Modify: `src/herdr_ui.c` (`add_limits_card` ~L1078-1118, `rebuild_dash_cards` ~L1192-1212)

**Interfaces:**
- Consumes: `herdr_limits_t.account` (Task 5).

> **Verificação:** UI LVGL — prova por build + inspeção visual no painel (duas contas Claude → dois cards com e-mail no título).

- [ ] **Step 1: Detectar multi-conta por provedor em `rebuild_dash_cards`**

Hoje calcula `multi` (host). Adicionar a detecção de conta e passar ao card. Substituir o laço final:
```c
    for (int i = 0; i < s_ui_limit_count; i++) {
        /* mostra a conta quando há outra do MESMO provedor: sem isso, dois
           cards "Claude" ficariam indistinguíveis */
        bool show_acct = false;
        for (int j = 0; j < s_ui_limit_count; j++) {
            if (j != i && strcmp(s_ui_limits[j].name, s_ui_limits[i].name) == 0
                && s_ui_limits[i].account[0]) {
                show_acct = true;
                break;
            }
        }
        add_limits_card(&s_ui_limits[i], multi, show_acct);
    }
```

- [ ] **Step 2: Assinar `add_limits_card` com `show_acct` e montar o título**

Trocar a assinatura e o forward-declare (`static void add_limits_card(const herdr_limits_t *l, bool show_host, bool show_acct);`) e o bloco do nome:
```c
static void add_limits_card(const herdr_limits_t *l, bool show_host, bool show_acct)
{
    ...
    lv_obj_t *name = lv_label_create(card);
    if (show_host && show_acct) {
        lv_label_set_text_fmt(name, "%s \xC2\xB7 %s", l->name, l->account);
    } else if (show_host) {
        lv_label_set_text_fmt(name, "%s \xC2\xB7 %s", host_label(l->host), l->name);
    } else if (show_acct) {
        lv_label_set_text_fmt(name, "%s \xC2\xB7 %s", l->name, l->account);
    } else {
        lv_label_set_text(name, l->name);
    }
    lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);   /* e-mail longo não estoura */
    lv_obj_set_width(name, DASH_NAME_MAX_W);           /* largura até o slot do plano */
    ...
}
```
(Definir `DASH_NAME_MAX_W` junto das demais medidas `DASH_*`, ~`LV_HOR_RES - DASH_LOGO - 4*DASH_PAD_X - 60`; ajustar na inspeção visual.)

- [ ] **Step 3: Build**

Run: `pio run -d cardputer`
Expected: compila sem erro.

- [ ] **Step 4: Verificação visual (manual)**

Com a ponte da Task 4 e duas contas Claude ativas: a Dash mostra dois cards "Claude · bruno@…" com percentuais distintos. Conferir truncamento do e-mail longo.

- [ ] **Step 5: Commit**

```bash
git add src/herdr_ui.c
git commit -m "feat(fw): Dash desambigua cards por conta (e-mail no título)"
```

---

## Task 8: Sessões — chip da conta por pane

**Files:**
- Modify: `src/herdr_ui.c` (construção da linha de sessão — localizar via `herdr_model_get_agents`/lista de sessões)

**Interfaces:**
- Consumes: `herdr_agent_t.account` (Task 5).

> **Verificação:** UI LVGL — build + inspeção visual (pane procergs mostra a conta work).

- [ ] **Step 1: Localizar a construção da linha de agente**

Run: `grep -nE "get_agents|s_ui_agents|project|status" src/herdr_ui.c | head`
Identificar onde cada agente vira uma linha (o campo `project`/`status` já é exibido).

- [ ] **Step 2: Detectar multi-conta e adicionar o sufixo**

Seguindo o mesmo critério "só desambigua quando precisa": calcular uma vez se há mais de uma conta distinta entre os agentes visíveis; se sim, anexar `· <account>` (ou um `lv_label` discreto em `UI_MUTED`) na linha de cada agente cujo `account[0]` não seja vazio. Usar `LV_LABEL_LONG_DOT` e largura limitada, como na Dash.

```c
/* multi_acct: há >1 conta distinta entre os agentes? calculado uma vez ao
   reconstruir a lista, análogo ao multi da Dash */
if (multi_acct && a->account[0]) {
    lv_obj_t *acct = lv_label_create(row);
    lv_label_set_text(acct, a->account);
    lv_obj_set_style_text_font(acct, &lv_font_ui_12, 0);
    lv_obj_set_style_text_color(acct, UI_MUTED, 0);
    lv_label_set_long_mode(acct, LV_LABEL_LONG_DOT);
    /* alinhar conforme o layout da linha existente */
}
```

- [ ] **Step 3: Build**

Run: `pio run -d cardputer`
Expected: compila sem erro.

- [ ] **Step 4: Verificação visual (manual)**

Pane procergs aparece com a conta de work; panes da conta pessoal, sem sufixo (ou com o seu e-mail), conforme houver mais de uma conta.

- [ ] **Step 5: Commit**

```bash
git add src/herdr_ui.c
git commit -m "feat(fw): aba Sessões mostra a conta de cada pane quando há mais de uma"
```

---

## Task 9: CI — rodar os testes da ponte

**Files:**
- Modify: `.github/workflows/build.yml` (job `host-test`)

- [ ] **Step 1: Adicionar o passo de teste Python**

No job `host-test`, após o passo `term_parse_test`:
```yaml
      - name: bridge unit tests
        run: cd plugin && python -m unittest discover -s tests -v
```

- [ ] **Step 2: Verificar sintaxe do workflow**

Run: `python -c "import yaml,sys; yaml.safe_load(open('.github/workflows/build.yml'))"` (se PyYAML disponível) ou revisão manual da indentação.
Expected: sem erro de parse.

- [ ] **Step 3: Commit**

```bash
git add .github/workflows/build.yml
git commit -m "ci: roda os testes unitários da ponte no host-test"
```

---

## Self-Review (preenchido pelo autor do plano)

**Cobertura do spec:**
- Descoberta por env (proc_env + accounts.discover) → Tasks 1, 2, 4. ✔
- Cross-platform (Windows/Linux/macOS) → Task 1. ✔
- Coleta por conta + e-mail (Claude via `.claude.json`, Codex via JWT) → Tasks 2, 3. ✔
- Conta default sempre visível → `discover`/`account_dirs` seed (Tasks 2, 4). ✔
- Campo `account` no protocolo → Tasks 3, 4 (ponte) e 6 (parse). ✔
- 1 provedor com N contas → coleta por config-dir (Task 3) + teto 8 (Task 5) + desambiguação (Task 7). ✔
- Tag nas sessões → Tasks 4 (ponte) e 8 (UI). ✔
- Perf (env só na mudança, cache por pid) → Task 4. ✔
- Segurança (só config-dir do env) → `_cached_env` filtra (Task 4); constraint global. ✔
- Testes ponte + CI → Tasks 1–3, 9. Firmware por build/manual (postura do repo). ✔

**Placeholders:** nenhum passo com TBD/TODO; código real em cada passo de código. As duas medidas de layout que exigem ajuste fino (`DASH_NAME_MAX_W` e o alinhamento do chip de sessão) são marcadas como ajuste na inspeção visual, não como lógica pendente.

**Consistência de tipos:** `collect_limits(account_dirs)` recebe iterável de `(agent, config_dir)` em Tasks 3 e 4; `discover` devolve `account_dirs` como `set[(agent, config_dir)]` (Task 2). `account[33]` idêntico em model (Task 5), parse (Task 6) e clip de 32 na ponte (Tasks 3, 4). `read_account_email(agent, config_dir, home)` com a mesma assinatura em Tasks 2, 3, 4.
