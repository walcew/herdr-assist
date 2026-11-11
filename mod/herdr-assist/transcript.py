#!/usr/bin/env python3
"""Métricas por sessão a partir do transcript do Claude Code (só stdlib).

Lê modelo, effort e tamanho de contexto do último evento assistant — e, quando
um `/compact` acontece depois dele, o tamanho pós-compact que o próprio CC
grava no compact_boundary. Nenhum conteúdo de mensagem é consumido — só o id
do modelo, o effort e as contagens de tokens do usage.
"""
from __future__ import annotations

import json
import re

_NAMES = {"opus": "Opus", "sonnet": "Sonnet", "haiku": "Haiku", "fable": "Fable"}

# Modelos com janela de 1M. O transcript não grava a janela em lugar nenhum
# (conferido registro a registro), então ela sai desta tabela: a família 5 é
# 1M por documentação oficial, e o Opus 4.7/4.8 aparece nos transcripts reais
# com prompts de 600k+ — na prática o CC também o roda com 1M. Haiku é 200k
# documentado. O que sobrar (Sonnet 4.x e desconhecidos) fica no degrau
# antigo: 200k até o uso provar que a janela é maior.
_WIN_1M = ("claude-opus-5", "claude-sonnet-5", "claude-fable-5",
           "claude-opus-4-7", "claude-opus-4-8")


def model_display(model_id: str) -> str:
    """`claude-opus-4-8` → `Opus 4.8`; `gpt-5` → `gpt-5`; desconhecido → id."""
    parts = (model_id or "").split("-")
    if len(parts) >= 3 and parts[0] == "claude" and parts[1] in _NAMES:
        ver = ".".join(parts[2:4]) if len(parts) >= 4 else parts[2]
        return "%s %s" % (_NAMES[parts[1]], ver)
    return (model_id or "")[:15]


def context_pct(prompt_tokens: int, model_id: str = "") -> int:
    """% da janela ocupada, com a janela derivada do modelo.

    Antes a janela era só inferida do uso (1M se >200k, senão 200k) — numa
    sessão Opus 5 com 150k reais (15% de 1M) o painel mostrava 75%, e ao
    cruzar 200k a barra despencava de 99% para 20%. A inferência sobrevive
    como fallback para modelo fora da tabela.
    """
    if prompt_tokens <= 0:
        return 0
    mid = model_id or ""
    if "haiku" in mid:
        window = 200_000
    elif mid.startswith(_WIN_1M):
        window = 1_000_000
    else:
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

    Um compact_boundary DEPOIS do último assistant substitui a contagem pelo
    postTokens: sem isso a barra segurava o valor pré-compact até o usuário
    mandar o próximo prompt — num painel parado, para sempre. O postTokens
    subestima um pouco (não inclui system prompt e afins, que voltam no
    próximo request), mas cair na hora para ~o tamanho novo vale mais que a
    exatidão de um número velho.
    """
    last = None
    post = None
    try:
        with open(jsonl_path, encoding="utf-8", errors="replace") as fh:
            for line in fh:
                try:
                    o = json.loads(line)
                except ValueError:
                    continue
                if not isinstance(o, dict):
                    continue
                if o.get("subtype") == "compact_boundary":
                    p = (o.get("compactMetadata") or {}).get("postTokens")
                    post = int(p) if isinstance(p, (int, float)) else None
                    continue
                msg = o.get("message")
                if isinstance(msg, dict) and msg.get("role") == "assistant":
                    last = o
                    post = None
    except OSError:
        return None
    if not last:
        return None
    msg = last.get("message") or {}
    u = msg.get("usage") or {}
    prompt = ((u.get("input_tokens") or 0) + (u.get("cache_read_input_tokens") or 0)
              + (u.get("cache_creation_input_tokens") or 0))
    if post is not None:
        prompt = post
    model_id = msg.get("model", "")
    # 7 chars: cabe "medium" (o mais longo hoje) com folga, e é o que o campo
    # effort[8] do herdr_agent_t comporta do outro lado.
    return {"model": model_display(model_id),
            "context_pct": context_pct(prompt, model_id),
            "effort": str(last.get("effort") or "")[:7]}
