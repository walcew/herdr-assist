#!/usr/bin/env python3
# /// script
# requires-python = ">=3.9"
# ///
"""Textos da tela de administração em pt_BR e en_US.

Mesmo formato do `src/i18n.h` do firmware — as duas traduções lado a lado, uma
linha por chave — para que acrescentar um texto novo seja revisável de um lado
só. Não usa `gettext` de propósito: ele exigiria `.mo` compilados, e o plugin
roda no python3 de fábrica do macOS, sem toolchain nem passo de build.

O idioma sai do ambiente do host, na ordem `HERDR_ASSIST_LANG`, `LC_ALL`,
`LC_MESSAGES`, `LANG`. Sem nenhuma delas (o caso de um processo não
interativo), fica en_US — o mesmo padrão de fábrica do painel.
"""
from __future__ import annotations

import os

EN, PT = 0, 1

# chave: (en_US, pt_BR)
STRINGS = {
    # --- tela de status ---
    "title":          ("herdr-assist — panel bridge",
                       "herdr-assist — ponte do painel"),
    "host":           ("Host", "Host"),
    "bridge":         ("Bridge", "Ponte"),
    "bridge.up":      ("running", "ativa"),
    "bridge.down":    ("stopped", "parada"),
    "bridge.hint":    ("use [x] to start it", "use [x] para subir"),
    "bridge.where":   ("pid {pid}, port {port}", "pid {pid}, porta {port}"),
    "token":          ("Token", "Token"),
    "token.missing":  ("missing", "ausente"),
    "panels":         ("Panels", "Painéis"),
    "panels.some":    ("{n} connected", "{n} conectado(s)"),
    "panels.none":    ("none connected", "nenhum conectado"),
    # itens de menu da TUI — cada um é uma linha-botão clicável com [tecla]
    "act.pair":       ("Pair panel", "Parear painel"),
    "act.rotate":     ("Rotate token", "Girar token"),
    "act.restart":    ("Restart bridge", "Reiniciar ponte"),
    "act.keybind":    ("Install shortcut", "Instalar atalho"),
    "act.update":     ("Update bridge", "Atualizar ponte"),
    "act.quit":       ("Quit", "Sair"),
    "hint.main":      ("click, or ↑↓ + enter — status updates live",
                       "clique, ou ↑↓ + enter — o status atualiza sozinho"),
    "hint.back":      ("press any key to go back", "qualquer tecla volta"),
    "hint.confirm":   ("enter confirms · esc cancels", "enter confirma · esc cancela"),
    "btn.install":    ("Install", "Instalar"),
    "btn.cancel":     ("Cancel", "Cancelar"),
    "restarting":     ("restarting the bridge...", "reiniciando a ponte..."),
    # --- atualização (ver update.py) ---
    "version":        ("Version", "Versão"),
    "version.behind": ("{cur} — {new} available", "{cur} — {new} disponível"),
    "updating":       ("checking for a new version...", "procurando versão nova..."),
    # Um texto por estado devolvido pelo update.check; todos existem porque o
    # t() estoura em chave ausente, e o estado vem de outro módulo.
    "update.installed": ("updated to {new}, restarting", "atualizada para {new}, reiniciando"),
    "update.same":    ("already on the newest ({cur})", "já está na mais nova ({cur})"),
    "update.local":   ("linked checkout — update it with git pull",
                       "checkout linkado — atualize com git pull"),
    "update.tried":   ("{new} already failed here; skipping",
                       "a {new} já falhou aqui; pulando"),
    "update.failed":  ("install failed, staying on {cur}",
                       "o install falhou, ficando na {cur}"),
    "update.unknown": ("could not check", "não deu para verificar"),
    "update.off":     ("automatic updates are off", "atualizações automáticas desligadas"),
    "update.available": ("{new} available", "{new} disponível"),
    # --- pareamento ---
    "pair.title":     ("Pair panel", "Parear painel"),
    "pair.where":     ("On the panel: Settings → Pair with a host",
                       "No painel: Configurações → Parear com um host"),
    "pair.no_token":  ("no token — start the bridge before pairing.",
                       "sem token — suba a ponte antes de parear."),
    "pair.listen_er": ("could not listen on port {port}: {err}",
                       "não foi possível escutar a porta {port}: {err}"),
    "pair.listening": ("listening... {n} found — esc cancels",
                       "escutando... {n} encontrado(s) — esc cancela"),
    "pair.check":     ("check the code shown on the panel screen",
                       "confira o código mostrado na tela do painel"),
    "pair.sending":   ("sending configuration to {dev}...",
                       "enviando configuração para {dev}..."),
    "pair.send_fail": ("failed to send: {err}", "falha ao enviar: {err}"),
    "pair.ok":        ("paired.", "pareado."),
    "pair.ok_hint":   ("the panel will restart and connect as \"{host}\".",
                       "o painel vai reiniciar e conectar como \"{host}\"."),
    "pair.refused":   ("the panel rejected the configuration.",
                       "o painel recusou a configuração."),
    # --- girar token ---
    "rotate.title":   ("Rotate token", "Girar token"),
    "rotate.warn1":   ("every panel paired with this host stops connecting",
                       "todos os painéis pareados com este host param de conectar"),
    "rotate.warn2":   ("until it is paired again.",
                       "até serem pareados de novo."),
    "rotate.confirm": ("YES", "SIM"),
    "rotate.prompt":  ("type {word} to confirm: ", "digite {word} para confirmar: "),
    "rotate.cancel":  ("cancelled.", "cancelado."),
    "rotate.new":     ("new token: ", "novo token: "),
    "rotate.done":    ("bridge restarted with the new token.",
                       "ponte reiniciada com o token novo."),
    # --- instalação do atalho ---
    "kb.title":       ("Install keyboard shortcut", "Instalar atalho de teclado"),
    "kb.what":        ("adds \"prefix+a\" (ctrl+b, then a) opening this screen to",
                       "acrescenta \"prefix+a\" (ctrl+b, depois a) abrindo esta tela em"),
    "kb.where":       ("{path} — a backup is saved first.",
                       "{path} — um backup é salvo antes."),
    "kb.already":     ("already installed — press ctrl+b, then a.",
                       "já instalado — aperte ctrl+b, depois a."),
    "kb.conflict":    ("\"prefix+a\" is already bound in your config.",
                       "\"prefix+a\" já está em uso na sua config."),
    "kb.conflict_hint": ("pick another key manually — see the README.",
                         "escolha outra tecla manualmente — veja o README."),
    "kb.check_fail":  ("config check failed — backup restored:",
                       "a validação da config falhou — backup restaurado:"),
    "kb.done":        ("installed — press ctrl+b, then a, anywhere in Herdr.",
                       "instalado — aperte ctrl+b, depois a, em qualquer lugar do Herdr."),
    "kb.reload_warn": ("could not reload the config — run: herdr server reload-config",
                       "não deu para recarregar a config — rode: herdr server reload-config"),
}


def _detect() -> int:
    raw = (os.environ.get("HERDR_ASSIST_LANG") or os.environ.get("LC_ALL")
           or os.environ.get("LC_MESSAGES") or os.environ.get("LANG") or "")
    return PT if raw.lower().replace("-", "_").startswith("pt") else EN


LANG = _detect()


def t(key: str, **kw) -> str:
    """Texto da chave no idioma detectado, já formatado."""
    s = STRINGS[key][LANG]
    return s.format(**kw) if kw else s
