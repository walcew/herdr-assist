# cd plugin && python -m unittest tests.test_rollout -v
import datetime
import json
import os
import tempfile
import unittest
from unittest import mock

import rollout

META = {"type": "session_meta", "payload": {"session_id": "s1", "cwd": "/x/proj"}}
TURN = {"type": "event_msg", "payload": {"type": "turn_context",
                                         "model": "gpt-5.6-sol", "effort": "xhigh"}}


def token_count(last_total, window):
    return {"type": "event_msg", "payload": {"type": "token_count", "info": {
        "total_token_usage": {"total_tokens": 999999},
        "last_token_usage": {"total_tokens": last_total},
        "model_context_window": window}}}


def write_rollout(path, records):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w", encoding="utf-8") as fh:
        for r in records:
            fh.write(json.dumps(r) + "\n")


class TestHeadCwd(unittest.TestCase):
    def test_le_o_cwd_do_session_meta(self):
        with tempfile.TemporaryDirectory() as d:
            fp = os.path.join(d, "r.jsonl")
            write_rollout(fp, [META, TURN])
            self.assertEqual(rollout.head_cwd(fp), "/x/proj")

    def test_primeira_linha_de_outro_tipo_devolve_none(self):
        with tempfile.TemporaryDirectory() as d:
            fp = os.path.join(d, "r.jsonl")
            write_rollout(fp, [TURN])
            self.assertIsNone(rollout.head_cwd(fp))

    def test_arquivo_ilegivel_devolve_none(self):
        self.assertIsNone(rollout.head_cwd("/naoexiste.jsonl"))
        with tempfile.TemporaryDirectory() as d:
            fp = os.path.join(d, "r.jsonl")
            with open(fp, "w") as fh:
                fh.write("lixo{\n")
            self.assertIsNone(rollout.head_cwd(fp))


class TestRecentPaths(unittest.TestCase):
    def test_so_os_ultimos_7_dias(self):
        today = datetime.date(2026, 8, 27)
        with tempfile.TemporaryDirectory() as d:
            def day_file(date, name):
                p = os.path.join(d, "sessions", "%04d" % date.year,
                                 "%02d" % date.month, "%02d" % date.day, name)
                write_rollout(p, [META])
                return p
            novo = day_file(today, "rollout-a.jsonl")
            limite = day_file(today - datetime.timedelta(days=6), "rollout-b.jsonl")
            day_file(today - datetime.timedelta(days=10), "rollout-velho.jsonl")
            got = {p for p, _ in rollout.recent_paths(d, today)}
            self.assertEqual(got, {novo, limite})


class TestPick(unittest.TestCase):
    def test_mais_recente_do_mesmo_cwd(self):
        cands = [("/a.jsonl", 10.0), ("/b.jsonl", 20.0), ("/c.jsonl", 30.0)]
        meta = {"/a.jsonl": "/x/proj", "/b.jsonl": "/x/proj", "/c.jsonl": "/outro"}
        self.assertEqual(rollout.pick(cands, "/x/proj", meta), "/b.jsonl")

    def test_sem_header_conhecido_nao_concorre(self):
        cands = [("/a.jsonl", 10.0), ("/b.jsonl", 20.0)]
        self.assertEqual(rollout.pick(cands, "/x/proj", {"/a.jsonl": "/x/proj"}),
                         "/a.jsonl")
        self.assertIsNone(rollout.pick(cands, "/x/proj", {}))


class TestSessionMetrics(unittest.TestCase):
    def test_ultimo_token_count_e_turn_context(self):
        with tempfile.TemporaryDirectory() as d:
            fp = os.path.join(d, "r.jsonl")
            write_rollout(fp, [META, TURN, token_count(10000, 258400),
                               token_count(21869, 258400)])
            m = rollout.session_metrics(fp)
            # contexto ocupado = last_token_usage do último evento, nunca o
            # total_token_usage (999999, acumulado da sessão)
            self.assertEqual(m["context_pct"], 21869 * 100 // 258400)
            self.assertEqual(m["model"], "gpt-5.6-sol")
            self.assertEqual(m["effort"], "xhigh")

    def test_token_count_sem_janela_nao_conta(self):
        # evento degenerado (sem model_context_window) não pode virar 0% nem
        # esconder o evento válido anterior
        bad = {"type": "event_msg", "payload": {"type": "token_count", "info": {
            "last_token_usage": {"total_tokens": 5}}}}
        with tempfile.TemporaryDirectory() as d:
            fp = os.path.join(d, "r.jsonl")
            write_rollout(fp, [META, token_count(21869, 258400), bad])
            self.assertEqual(rollout.session_metrics(fp)["context_pct"],
                             21869 * 100 // 258400)

    def test_sem_token_count_devolve_none(self):
        with tempfile.TemporaryDirectory() as d:
            fp = os.path.join(d, "r.jsonl")
            write_rollout(fp, [META, TURN])
            self.assertIsNone(rollout.session_metrics(fp))

    def test_linha_gigante_no_fim_nao_esconde_o_evento(self):
        # uma linha maior que o bloco de tail (um diff inteiro num turno)
        # depois do token_count: os blocos crescem até alcançá-lo
        gigante = {"type": "event_msg", "payload": {"type": "agent_message",
                                                    "message": "x" * 2000}}
        with tempfile.TemporaryDirectory() as d:
            fp = os.path.join(d, "r.jsonl")
            write_rollout(fp, [META, TURN, token_count(21869, 258400), gigante])
            with mock.patch.object(rollout, "_TAIL_BLOCK", 256):
                m = rollout.session_metrics(fp)
            self.assertEqual(m["context_pct"], 21869 * 100 // 258400)

    def test_clips_cabem_no_firmware(self):
        # model[16] e effort[8] do herdr_agent_t: 15 e 7 chars úteis
        turn = {"type": "event_msg", "payload": {"type": "turn_context",
                "model": "m" * 40, "effort": "e" * 20}}
        with tempfile.TemporaryDirectory() as d:
            fp = os.path.join(d, "r.jsonl")
            write_rollout(fp, [META, turn, token_count(500000, 258400)])
            m = rollout.session_metrics(fp)
            self.assertEqual(len(m["model"]), 15)
            self.assertEqual(len(m["effort"]), 7)
            self.assertEqual(m["context_pct"], 100)    # clamp
