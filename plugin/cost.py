#!/usr/bin/env python3
"""Custo estimado a partir dos transcripts do Claude Code (só stdlib).

O transcript não guarda custo; estima-se tokens × tabela de preços por modelo.
Preços da API Anthropic (US$ por 1M tokens); cache read 0.1×, write 1.25× do
input. É estimativa — a UI mostra tudo com prefixo "~".
"""
from __future__ import annotations

import json

# model_id -> (input US$/1M, output US$/1M)
PRICES = {
    "claude-opus-4-8": (5.0, 25.0), "claude-opus-4-7": (5.0, 25.0),
    "claude-opus-4-6": (5.0, 25.0), "claude-opus-4-5": (5.0, 25.0),
    "claude-sonnet-5": (3.0, 15.0), "claude-sonnet-4-6": (3.0, 15.0),
    "claude-sonnet-4-5": (3.0, 15.0), "claude-haiku-4-5": (1.0, 5.0),
    "claude-fable-5": (10.0, 50.0), "claude-mythos-5": (10.0, 50.0),
}
_DEFAULT = (5.0, 25.0)


def message_cost(usage: dict, model_id: str) -> float:
    """US$ de um evento assistant: input + cache(write 1.25×, read 0.1×) + output."""
    in_p, out_p = PRICES.get(model_id or "", _DEFAULT)
    u = usage or {}
    inp = u.get("input_tokens") or 0
    out = u.get("output_tokens") or 0
    cr = u.get("cache_read_input_tokens") or 0
    cw = u.get("cache_creation_input_tokens") or 0
    return (inp * in_p + cw * in_p * 1.25 + cr * in_p * 0.1 + out * out_p) / 1_000_000.0


def fmt_usd(v: float) -> str:
    """"~US$ 4,20" / "~US$ 1.240" — vírgula decimal, ponto de milhar, prefixo ~."""
    if round(v, 2) >= 100:             # valores grandes: sem centavos
        s = "%.0f" % v
    else:
        s = "%.2f" % v
    intp, _, frac = s.partition(".")
    # milhar com ponto
    grouped = ""
    for i, ch in enumerate(reversed(intp)):
        if i and i % 3 == 0:
            grouped = "." + grouped
        grouped = ch + grouped
    return "~US$ " + (grouped + ("," + frac if frac else ""))


def file_cost(jsonl_path: str):
    """{"total": float, "days": {date: float}} do transcript, ou None em falha."""
    total = 0.0
    days = {}
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
                if not (isinstance(msg, dict) and msg.get("role") == "assistant"):
                    continue
                c = message_cost(msg.get("usage") or {}, msg.get("model", ""))
                total += c
                ts = o.get("timestamp") or ""
                day = ts[:10]            # "YYYY-MM-DD"
                if day:
                    days[day] = days.get(day, 0.0) + c
    except OSError:
        return None
    return {"total": total, "days": days}
