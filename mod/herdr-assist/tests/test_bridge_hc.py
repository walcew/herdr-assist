# cd mod/herdr-assist && python3 -m unittest tests.test_bridge_hc -v
"""A ponte contra um `hc` falso, vista de fora: o que o painel recebe por TCP.

Sobe a ponte de verdade (subprocesso, porta efêmera, token conhecido) com
HC_BIN apontando para tests/fake_hc.py e conversa com ela como o painel.
"""
import json
import os
import shutil
import socket
import subprocess
import sys
import tempfile
import time
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
BRIDGE = os.path.join(os.path.dirname(HERE), "herdr_bridge.py")
FAKE_HC = os.path.join(HERE, "fake_hc.py")
TOKEN = "test-token"


def terminal(addr, state="working", where="herdr-assist / 1", app="claude-code",
             labels=None, **extra):
    t = {"address": addr, "state": "live", "cwd": "/x/" + addr,
         "foreground": {"argv": ["claude"], "cwd": "/x/" + addr, "name": "claude",
                        "pid": 999999},
         "labels": dict({"herdr.space": "sp1", "herdr.tab": "tb1", "where": where},
                        **(labels or {})),
         "status": {"app": app, "msg": "", "state": state} if state else None,
         "title": "", "turn": 0}
    t.update(extra)
    return t


def free_port():
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


class TestBridgeOverFakeHc(unittest.TestCase):
    def setUp(self):
        self.dir = tempfile.mkdtemp()
        # os `hc connect` falsos saem no EOF, um instante depois da ponte
        self.addCleanup(shutil.rmtree, self.dir, True)
        self.set_terminals([
            terminal("t1a", "working", "herdr-assist / review"),
            terminal("t1b", "idle", "eletrilha / 1"),
            terminal("t1c", None),                                   # sem status: não é agente
            terminal("t1d", "idle", labels={"herdr-assist": "bridge"}),  # a própria ponte
            dict(terminal("t1e", "done"), state="finished"),        # já terminou
        ])
        self.port = free_port()
        env = {"PATH": "/usr/bin:/bin", "HOME": self.dir,
               "HC_BIN": FAKE_HC, "FAKE_HC_DIR": self.dir,
               "BRIDGE_PORT": str(self.port), "BRIDGE_BIND": "127.0.0.1",
               "BRIDGE_TOKEN": TOKEN, "HERDR_PLUGIN_CONFIG_DIR": self.dir,
               "PYTHONDONTWRITEBYTECODE": "1"}
        self.proc = subprocess.Popen([sys.executable, BRIDGE], env=env,
                                     stdout=subprocess.DEVNULL,
                                     stderr=subprocess.DEVNULL)
        self.addCleanup(self.stop)

    def stop(self):
        self.proc.kill()
        self.proc.wait()

    def set_terminals(self, terms):
        path = os.path.join(self.dir, "terminals.json")
        with open(path + ".tmp", "w") as fh:
            json.dump(terms, fh)
        os.replace(path + ".tmp", path)   # o hc falso nunca lê meio arquivo

    def notify(self, method, params):
        with open(os.path.join(self.dir, "notify.jsonl"), "a") as fh:
            fh.write(json.dumps({"method": method, "params": params}) + "\n")

    def calls(self, method):
        with open(os.path.join(self.dir, "calls.jsonl")) as fh:
            return [c["params"] for c in map(json.loads, fh) if c["method"] == method]

    def connect(self, token=TOKEN):
        deadline = time.time() + 10
        while True:
            try:
                s = socket.create_connection(("127.0.0.1", self.port), timeout=5)
                break
            except OSError:
                if time.time() > deadline:
                    raise
                time.sleep(0.05)
        self.addCleanup(s.close)
        hello = {"type": "hello"}
        if token is not None:
            hello["token"] = token
        s.sendall((json.dumps(hello) + "\n").encode())
        return s, s.makefile("r", encoding="utf-8")

    def next_agents(self, rf, timeout=5):
        """Próxima mensagem `agents`, ignorando as demais (limits, bridge_info...)."""
        deadline = time.time() + timeout
        while time.time() < deadline:
            line = rf.readline()
            if not line:
                self.fail("ponte fechou a conexão")
            msg = json.loads(line)
            if msg.get("type") == "agents":
                return {a["pane_id"]: a for a in msg["agents"]}
        self.fail("nenhum agents em %ss" % timeout)

    def agents_until(self, rf, pred, timeout=5):
        deadline = time.time() + timeout
        while True:
            agents = self.next_agents(rf, max(0.1, deadline - time.time()))
            if pred(agents):
                return agents

    def ready(self, rf):
        """Painel pode conectar antes do snapshot do hc: espera o 1º com agentes."""
        return self.agents_until(rf, lambda a: a)

    def test_snapshot_vira_agents_com_id_status_e_space_tab(self):
        _, rf = self.connect()
        agents = self.ready(rf)
        self.assertEqual(sorted(agents), ["t1a", "t1b"])
        a, b = agents["t1a"], agents["t1b"]
        self.assertEqual((a["status"], a["agent"]), ("working", "claude"))
        self.assertEqual(a["project"], "review")          # tab com nome
        self.assertEqual(a["workspace_id"], "herdr-assist")
        self.assertIsInstance(a["since"], int)
        self.assertEqual((b["status"], b["project"]), ("idle", "eletrilha"))  # tab numérica

    def test_evento_status_gera_agents_novo_na_hora(self):
        _, rf = self.connect()
        self.ready(rf)
        self.set_terminals([terminal("t1a", "blocked", "herdr-assist / review"),
                            terminal("t1b", "idle", "eletrilha / 1")])
        t0 = time.time()
        self.notify("event", {"type": "status", "terminal": "t1a", "seq": 2,
                              "data": {"app": "claude-code", "state": "blocked"}})
        agents = self.agents_until(rf, lambda a: a.get("t1a", {}).get("status") == "blocked")
        self.assertLess(time.time() - t0, 1.0)
        self.assertNotIn("since", agents["t1a"])
        # veio do evento, não de polling: a ponte releu só o terminal citado
        self.assertEqual(self.calls("terminal.get"), [{"terminal": "t1a"}])
        self.assertEqual(self.calls("terminal.list"), [])

    def test_missed_ressincroniza_o_estado(self):
        _, rf = self.connect()
        self.ready(rf)
        # o core mudou sem que a ponte visse os eventos (t1b sumiu, t1f nasceu)
        self.set_terminals([terminal("t1a", "done", "herdr-assist / review"),
                            terminal("t1f", "working", "novo / 1")])
        self.notify("missed", {})
        agents = self.agents_until(rf, lambda a: "t1f" in a)
        self.assertEqual(sorted(agents), ["t1a", "t1f"])
        self.assertEqual(agents["t1a"]["status"], "done")
        self.assertEqual(len(self.calls("terminal.list")), 1)

    def test_hello_sem_token_e_recusado(self):
        for token in (None, "errado"):
            _, rf = self.connect(token)
            self.assertEqual(json.loads(rf.readline()),
                             {"type": "error", "message": "token invalido"})
            self.assertEqual(rf.readline(), "")   # e a ponte desconecta


if __name__ == "__main__":
    unittest.main()
