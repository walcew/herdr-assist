# cd mod/herdr-assist && python -m unittest tests.test_ctl -v
import signal
import unittest

import ctl


def listen(found):
    return lambda port: found


class TestPortHolder(unittest.TestCase):
    def test_porta_livre(self):
        self.assertIsNone(ctl.port_holder(9375, listen([]), pids=set()))

    def test_ponte_do_mod(self):
        got = ctl.port_holder(9375, listen([(42, "Python")]), pids={41, 42})
        self.assertEqual(got, {"pid": 42, "command": "Python", "is_bridge": True})

    def test_outro_processo(self):
        # a ponte do Herdr TUI: escuta a porta, mas não é o terminal do rótulo
        got = ctl.port_holder(9375, listen([(7, "Python")]), pids={42})
        self.assertEqual(got, {"pid": 7, "command": "Python", "is_bridge": False})

    def test_sem_ponte_do_mod_viva(self):
        got = ctl.port_holder(9375, listen([(7, "Python")]), pids=set())
        self.assertFalse(got["is_bridge"])


class TestListeners(unittest.TestCase):
    def test_parse_do_lsof(self):
        out = "p37040\ncPython\nf5\nn*:9375\np99\ncnode\n"
        orig = ctl.run
        ctl.run = lambda *a, **k: out
        try:
            self.assertEqual(ctl.listeners(9375), [(37040, "Python"), (99, "node")])
        finally:
            ctl.run = orig


class TestKillHolder(unittest.TestCase):
    def setUp(self):
        self.killed = []

    def kill(self, pid, sig):
        self.killed.append((pid, sig))

    def holder(self, h):
        return lambda port: h

    def test_mata_outra_ponte(self):
        h = {"pid": 7, "command": "Python", "is_bridge": False}
        got = ctl.kill_holder(7, 9375, self.holder(h), self.kill)
        self.assertEqual(got, {"ok": True})
        self.assertEqual(self.killed, [(7, signal.SIGTERM)])

    def test_nunca_mata_a_ponte_do_mod(self):
        h = {"pid": 42, "command": "Python", "is_bridge": True}
        got = ctl.kill_holder(42, 9375, self.holder(h), self.kill)
        self.assertFalse(got["ok"])
        self.assertEqual(self.killed, [])

    def test_pid_que_nao_e_mais_o_dono(self):
        # a confirmação demorou e outro processo pegou a porta: não mata
        h = {"pid": 8, "command": "Python", "is_bridge": False}
        self.assertFalse(ctl.kill_holder(7, 9375, self.holder(h), self.kill)["ok"])
        self.assertFalse(ctl.kill_holder(7, 9375, self.holder(None), self.kill)["ok"])
        self.assertEqual(self.killed, [])


if __name__ == "__main__":
    unittest.main()
