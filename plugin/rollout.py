#!/usr/bin/env python3
"""Métricas por sessão a partir do rollout do Codex (só stdlib).

O Codex grava sozinho, por sessão, ~/.codex/sessions/AAAA/MM/DD/rollout-*.jsonl:
a primeira linha é o session_meta (com o cwd em que a sessão nasceu) e cada
turno emite um token_count — uso do último request E a janela real do modelo
(model_context_window), que nenhuma tabela local precisaria adivinhar — além de
um turn_context com modelo e effort. Mesmo espírito da leitura de transcript do
Claude em transcript.py: só arquivos que o harness já escreve, nenhum processo,
nenhuma rede, nenhuma configuração no ambiente do usuário.

recent_paths/head_cwd fazem I/O pequeno (glob/stat e uma linha); apenas
session_metrics lê volume — como o transcript, só de dentro do executor.
"""
from __future__ import annotations

import datetime
import glob
import json
import os

_TAIL_BLOCK = 64 * 1024
_TAIL_MAX = 4 * 1024 * 1024


def recent_paths(codex_home: str, today=None) -> list:
    """Candidatos a rollout vivo: [(path, mtime)] das pastas dos últimos 7 dias.

    As pastas são sessions/AAAA/MM/DD pela data de criação — uma sessão aberta
    há mais de 7 dias não aparece aqui, mas o caller segura o path que já
    resolveu enquanto o arquivo existir, então só a descoberta tardia (ponte
    reiniciada com uma sessão velhíssima ainda aberta) fica de fora. Globar a
    árvore inteira cobriria esse caso, mas custaria milhares de stats por
    ciclo num histórico grande.
    """
    today = today or datetime.date.today()
    out = []
    for i in range(7):
        d = today - datetime.timedelta(days=i)
        pat = os.path.join(codex_home, "sessions", "%04d" % d.year,
                           "%02d" % d.month, "%02d" % d.day, "rollout-*.jsonl")
        for p in glob.glob(pat):
            try:
                out.append((p, os.path.getmtime(p)))
            except OSError:
                pass                # sumiu entre o glob e o stat
    return out


def pick(cands: list, cwd: str, meta_cwd: dict):
    """O candidato mais recente cuja sessão nasceu em `cwd`, ou None.

    `meta_cwd` é o cache path→cwd do caller. Candidato ainda sem entrada não
    concorre neste ciclo — o header dele está a caminho do executor; no ciclo
    seguinte o cache cobre. Pura, sem I/O.
    """
    best = None
    for p, mt in cands:
        if meta_cwd.get(p) == cwd and (best is None or mt > best[1]):
            best = (p, mt)
    return best[0] if best else None


def head_cwd(path: str):
    """cwd do session_meta (1ª linha do rollout); None se ilegível.

    A primeira linha nunca muda depois de criada — o resultado vale para
    sempre. Bloqueante (abre o arquivo): só no executor.
    """
    try:
        with open(path, encoding="utf-8", errors="replace") as fh:
            o = json.loads(fh.readline())
    except (OSError, ValueError):
        return None
    if not isinstance(o, dict) or o.get("type") != "session_meta":
        return None
    return (o.get("payload") or {}).get("cwd")


def _payload(line: str):
    try:
        o = json.loads(line)
    except ValueError:
        return None
    return o.get("payload") if isinstance(o, dict) else None


def _scan(lines: list):
    """Último token_count (uso/janela) e último turn_context (modelo/effort)."""
    usage = window = None
    model = effort = ""
    for ln in reversed(lines):
        if usage is None and '"token_count"' in ln:
            p = _payload(ln)
            if p and p.get("type") == "token_count":
                info = p.get("info") or {}
                t = (info.get("last_token_usage") or {}).get("total_tokens")
                w = info.get("model_context_window")
                if isinstance(t, (int, float)) and isinstance(w, (int, float)) \
                        and w > 0:
                    usage, window = int(t), int(w)
        elif not model and '"turn_context"' in ln:
            p = _payload(ln)
            if p and p.get("type") == "turn_context":
                model = str(p.get("model") or "")
                effort = str(p.get("effort") or "")
        if usage is not None and model:
            break
    if usage is None:
        return None
    # clips espelham o outro lado: model[16] e effort[8] do herdr_agent_t
    return {"model": model[:15],
            "context_pct": min(100, usage * 100 // window),
            "effort": effort[:7]}


def session_metrics(path: str):
    """{"model","context_pct","effort"} do fim do rollout, ou None.

    O contexto ocupado é o total_tokens do ÚLTIMO request (last_token_usage):
    o input de um request já carrega o histórico inteiro, e o output vira
    contexto do seguinte. total_token_usage não serve — é o acumulado da
    sessão e cresce além da janela. Sem o desconto de baseline que o TUI do
    Codex aplica ao "% left" dele: aqui vale o ocupado real, alguns pontos
    acima do número maquiado de lá.

    Lê blocos crescentes a partir do fim: uma linha de rollout pode ter
    centenas de KB (um diff inteiro num turno), e um tail fixo poderia não
    conter evento nenhum. model/effort saem do último turn_context que couber
    no mesmo bloco; sem ele, ficam vazios e o painel mostra só a barra.
    """
    try:
        size = os.path.getsize(path)
        with open(path, "rb") as fh:
            block = _TAIL_BLOCK
            while True:
                start = max(0, size - block)
                fh.seek(start)
                lines = fh.read().decode("utf-8", errors="replace").splitlines()
                if start > 0 and lines:
                    lines = lines[1:]   # a primeira pode ser pedaço de linha
                m = _scan(lines)
                if m or start == 0 or block >= _TAIL_MAX:
                    return m
                block *= 8
    except OSError:
        return None
