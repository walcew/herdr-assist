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
    try:
        if isinstance(data.get("email"), str):
            return data["email"]
        for v in data.values():  # claims namespaced: pega o primeiro que pareça e-mail
            if isinstance(v, str) and "@" in v:
                return v
            if isinstance(v, dict) and isinstance(v.get("email"), str):
                return v["email"]
    except (AttributeError, TypeError):
        return ""
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
    except (OSError, ValueError, json.JSONDecodeError, AttributeError, TypeError):
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


PUBLIC_DOMAINS = {"gmail.com", "outlook.com", "hotmail.com", "live.com",
                  "yahoo.com", "yahoo.com.br", "icloud.com", "proton.me",
                  "protonmail.com", "gmx.com", "aol.com"}
_KNOWN_2LD = {"com.br", "com.mx", "co.uk", "com.ar", "com.au"}


def org_and_corp(email: str):
    """(org, corp) — org = domínio sem sufixo; corp = não é domínio público."""
    domain = (email.split("@")[-1] if email and "@" in email else "").lower()
    if not domain:
        return "", False
    corp = domain not in PUBLIC_DOMAINS
    parts = domain.split(".")
    if len(parts) >= 3 and ".".join(parts[-2:]) in _KNOWN_2LD:
        parts = parts[:-2]
    elif len(parts) >= 2:
        parts = parts[:-1]
    org = parts[-1] if parts else domain
    return org, corp
