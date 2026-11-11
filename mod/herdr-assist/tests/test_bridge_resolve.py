# cd plugin && python -m unittest tests.test_bridge_resolve -v
import datetime
import json
import os
import tempfile
import time
import unittest

import herdr_bridge as b


def write(path, text="x"):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w", encoding="utf-8") as fh:
        fh.write(text)


class TestResolveTranscript(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.cfg = self.tmp.name
        self.addCleanup(self.tmp.cleanup)
        self.addCleanup(b.pane_pid_cache.clear)

    def pane(self, **extra):
        p = {"pane_id": "p1", "agent": "claude", "cwd": "/x/proj"}
        p.update(extra)
        return p

    def proj(self, name):
        return os.path.join(self.cfg, "projects", "-x-proj", name)

    def test_registro_por_pid_vence_o_mtime(self):
        # um jsonl recém-gravado no mesmo projects/ (fantasma de coleta, outra
        # sessão) não rouba a vez quando o CC registrou pid→sessão
        write(self.proj("sess-real.jsonl"))
        ghost = self.proj("ghost.jsonl")
        write(ghost)
        os.utime(ghost, (time.time() + 60, time.time() + 60))
        write(os.path.join(self.cfg, "sessions", "123.json"),
              json.dumps({"pid": 123, "sessionId": "sess-real"}))
        b.pane_pid_cache[("p1", "claude")] = 123
        got = b._resolve_transcript_path(self.pane(), ("claude", self.cfg))
        self.assertEqual(os.path.basename(got), "sess-real.jsonl")

    def test_uuid_do_herdr_quando_nao_ha_registro_por_pid(self):
        write(self.proj("sess-uuid.jsonl"))
        got = b._resolve_transcript_path(
            self.pane(agent_session={"value": "sess-uuid"}),
            ("claude", self.cfg))
        self.assertEqual(os.path.basename(got), "sess-uuid.jsonl")

    def test_sem_pid_nem_uuid_cai_no_mais_recente_do_cwd(self):
        velho = self.proj("velho.jsonl")
        novo = self.proj("novo.jsonl")
        write(velho)
        write(novo)
        os.utime(velho, (time.time() - 60, time.time() - 60))
        got = b._resolve_transcript_path(self.pane(), ("claude", self.cfg))
        self.assertEqual(got, novo)

    def test_registro_ilegivel_nao_derruba_o_fallback(self):
        write(os.path.join(self.cfg, "sessions", "123.json"), "lixo{")
        b.pane_pid_cache[("p1", "claude")] = 123
        alvo = self.proj("unico.jsonl")
        write(alvo)
        got = b._resolve_transcript_path(self.pane(), ("claude", self.cfg))
        self.assertEqual(got, alvo)


class TestResolveRollout(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.home = self.tmp.name
        self.addCleanup(self.tmp.cleanup)
        self.addCleanup(b.codex_paths.clear)
        self.addCleanup(b.rollout_meta_cache.clear)

    def rollout_file(self, name):
        d = datetime.date.today()
        p = os.path.join(self.home, "sessions", "%04d" % d.year,
                         "%02d" % d.month, "%02d" % d.day, name)
        write(p, json.dumps({"type": "session_meta",
                             "payload": {"cwd": "/x/proj"}}) + "\n")
        return p

    def pane(self):
        return {"pane_id": "p9", "agent": "codex", "cwd": "/x/proj"}

    def test_header_inedito_vai_para_to_meta_e_ainda_nao_concorre(self):
        p1 = self.rollout_file("rollout-a.jsonl")
        to_meta, cands = set(), set()
        got = b._resolve_rollout_path(self.pane(), ("codex", self.home),
                                      to_meta, cands)
        self.assertIsNone(got)          # 1º ciclo: header a caminho do executor
        self.assertEqual(to_meta, {p1})
        self.assertEqual(cands, {p1})

    def test_com_header_no_cache_escolhe_pelo_cwd(self):
        p1 = self.rollout_file("rollout-a.jsonl")
        b.rollout_meta_cache[p1] = "/x/proj"
        got = b._resolve_rollout_path(self.pane(), ("codex", self.home),
                                      set(), set())
        self.assertEqual(got, p1)

    def test_sessao_fora_da_janela_mantem_o_path_anterior(self):
        # sessão criada há mais de 7 dias não aparece nos candidatos, mas o
        # pane que já a resolveu continua com ela
        b.codex_paths["p9"] = "/velho/rollout.jsonl"
        got = b._resolve_rollout_path(self.pane(), ("codex", self.home),
                                      set(), set())
        self.assertEqual(got, "/velho/rollout.jsonl")
