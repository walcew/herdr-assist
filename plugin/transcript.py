#!/usr/bin/env python3
"""Métricas por sessão a partir do transcript do Claude Code (só stdlib).

Lê modelo, effort e tamanho de contexto do último evento assistant. Nenhum
conteúdo de mensagem é consumido — só o id do modelo, o effort e as contagens
de tokens do usage.
"""
from __future__ import annotations

import json
import re

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


def encode_cwd(cwd: str) -> str:
    """cwd → nome do diretório em projects/ (troca :\\/  por -)."""
    return re.sub(r"[:\\/]", "-", (cwd or "").rstrip("\\/"))


def session_metrics(jsonl_path: str):
    """{"model","context_pct","effort"} do último assistant, ou None se ilegível.

    Guarda o registro inteiro, não a mensagem: `effort` é chave de nível
    superior, IRMÃ de `message` — quem procurar dentro dela não acha nada.
    Vem vazio no Codex e em transcripts de CLI anterior à 2.1.234, que é o
    caminho normal, não um erro.
    """
    last = None
    try:
        with open(jsonl_path, encoding="utf-8", errors="replace") as fh:
            for line in fh:
                try:
                    o = json.loads(line)
                except ValueError:
                    continue
                if not isinstance(o, dict):
                    continue
                msg = o.get("message")
                if isinstance(msg, dict) and msg.get("role") == "assistant":
                    last = o
    except OSError:
        return None
    if not last:
        return None
    msg = last.get("message") or {}
    u = msg.get("usage") or {}
    prompt = ((u.get("input_tokens") or 0) + (u.get("cache_read_input_tokens") or 0)
              + (u.get("cache_creation_input_tokens") or 0))
    # 7 chars: cabe "medium" (o mais longo hoje) com folga, e é o que o campo
    # effort[8] do herdr_agent_t comporta do outro lado.
    return {"model": model_display(msg.get("model", "")),
            "context_pct": context_pct(prompt),
            "effort": str(last.get("effort") or "")[:7]}
