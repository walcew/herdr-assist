#!/usr/bin/env python3
"""Métricas por sessão a partir do transcript do Claude Code (só stdlib).

Lê modelo e tamanho de contexto do último evento assistant. Nenhum conteúdo de
mensagem é consumido — só o id do modelo e as contagens de tokens do usage.
"""
from __future__ import annotations

import json

_NAMES = {"opus": "Opus", "sonnet": "Sonnet", "haiku": "Haiku", "fable": "Fable"}


def model_display(model_id: str) -> str:
    """`claude-opus-4-8` → `Opus 4.8`; `gpt-5` → `gpt-5`; desconhecido → id."""
    parts = (model_id or "").split("-")
    if len(parts) >= 3 and parts[0] == "claude" and parts[1] in _NAMES:
        ver = ".".join(parts[2:4]) if len(parts) >= 4 else parts[2]
        return "%s %s" % (_NAMES[parts[1]], ver)
    return (model_id or "")[:15]


def context_pct(prompt_tokens: int) -> int:
    """% da janela ocupada. Janela inferida: 1M se uso>200k, senão 200k."""
    if prompt_tokens <= 0:
        return 0
    window = 1_000_000 if prompt_tokens > 200_000 else 200_000
    return min(100, prompt_tokens * 100 // window)


def session_metrics(jsonl_path: str):
    """{"model","context_pct"} do último assistant, ou None se ilegível."""
    last = None
    try:
        with open(jsonl_path, encoding="utf-8", errors="replace") as fh:
            for line in fh:
                try:
                    o = json.loads(line)
                except ValueError:
                    continue
                msg = o.get("message")
                if isinstance(msg, dict) and msg.get("role") == "assistant":
                    last = msg
    except OSError:
        return None
    if not last:
        return None
    u = last.get("usage") or {}
    prompt = ((u.get("input_tokens") or 0) + (u.get("cache_read_input_tokens") or 0)
              + (u.get("cache_creation_input_tokens") or 0))
    return {"model": model_display(last.get("model", "")),
            "context_pct": context_pct(prompt)}
