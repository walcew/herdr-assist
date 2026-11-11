# cd mod/herdr-assist && python -m unittest tests.test_ctl -v
import io
import json
import os
import shutil
import signal
import socket
import tempfile
import threading
import unittest
from unittest import mock

import ctl

FAKE_HC = os.path.join(os.path.dirname(os.path.abspath(__file__)), "fake_hc.py")


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


class TestRunningVersion(unittest.TestCase):
    def get(self, labels):
        return mock.patch.object(ctl, "hc_json", return_value={"labels": labels})

    def test_versao_no_ar_vem_do_rotulo_do_terminal(self):
        with self.get({"herdr-assist": "bridge", "herdr-assist.version": "0.15.0"}):
            self.assertEqual(ctl.running_version(["t1d"]), "0.15.0")

    def test_sem_ponte_ou_sem_rotulo_e_none(self):
        self.assertIsNone(ctl.running_version([]))
        with self.get({"herdr-assist": "bridge"}):
            self.assertIsNone(ctl.running_version(["t1d"]))

    def test_status_marca_reinicio_pendente_quando_difere_do_disco(self):
        for running, pending in (("0.14.0", True), ("0.15.0", False), (None, False)):
            with mock.patch.object(ctl, "bridge_terminals", return_value=["t1d"]), \
                    mock.patch.object(ctl, "bridge_pids", return_value=set()), \
                    mock.patch.object(ctl, "bridge_port", return_value=9375), \
                    mock.patch.object(ctl, "panels", return_value=[]), \
                    mock.patch.object(ctl, "lan_ip", return_value=""), \
                    mock.patch.object(ctl, "port_holder", return_value=None), \
                    mock.patch.object(ctl.update, "installed_version", return_value="0.15.0"), \
                    mock.patch.object(ctl, "running_version", return_value=running):
                st = ctl.status()
            self.assertEqual((st["version"], st["running"], st["restart_pending"]),
                             ("0.15.0", running, pending))


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


class FakeCore(unittest.TestCase):
    """ctl.py falando com o tests/fake_hc.py, que guarda os docs em docs.json."""

    def setUp(self):
        self.dir = tempfile.mkdtemp()
        self.addCleanup(shutil.rmtree, self.dir)
        env = mock.patch.dict(os.environ, {"FAKE_HC_DIR": self.dir})
        env.start()
        self.addCleanup(env.stop)
        os.environ.pop("BRIDGE_PORT", None)
        hc = mock.patch.object(ctl, "HC_BIN", FAKE_HC)
        hc.start()
        self.addCleanup(hc.stop)

    def set_doc(self, value, version):
        with open(os.path.join(self.dir, "docs.json"), "w") as fh:
            json.dump({ctl.DOC_NAME: {"value": value, "version": version}}, fh)

    def doc(self):
        with open(os.path.join(self.dir, "docs.json")) as fh:
            return json.load(fh)[ctl.DOC_NAME]


class TestRotateToken(FakeCore):
    # que o painel com o token antigo é recusado depois disso já é coberto no
    # seam da Ponte: test_bridge_hc.test_evento_doc_com_token_novo_recusa_o_antigo_sem_reiniciar

    def test_grava_token_novo_com_a_versao_e_mantem_o_resto(self):
        self.set_doc({"token": "old", "port": 9400, "avatar_repos": ["r"]}, 3)
        got = ctl.rotate_token()
        self.assertTrue(got["ok"])
        self.assertNotEqual(got["token"], "old")
        self.assertEqual(len(got["token"]), 32)
        self.assertEqual(self.doc(), {"value": {"token": got["token"], "port": 9400,
                                                "avatar_repos": ["r"]}, "version": 4})

    def test_conflito_rele_e_tenta_de_novo_uma_vez(self):
        self.set_doc({"token": "old"}, 3)
        open(os.path.join(self.dir, "conflict_once"), "w").close()
        got = ctl.rotate_token()
        self.assertTrue(got["ok"])
        # o outro escritor levou à 4; a segunda tentativa grava a 5
        self.assertEqual(self.doc(), {"value": {"token": got["token"]}, "version": 5})

    def test_porta_do_doc_invalida_cai_no_padrao_como_na_ponte(self):
        for port, want in ((9400, 9400), (True, 9375), (0, 9375), (70000, 9375), ("9400", 9375)):
            self.set_doc({"port": port}, 1)
            self.assertEqual(ctl.bridge_port(), want, port)

    def test_token_le_do_doc(self):
        self.set_doc({"token": "abc"}, 1)
        self.assertEqual(ctl.doc_get()["token"], "abc")


class PanelServer:
    """Painel falso em modo de pareamento: lê a configuração e responde."""

    def __init__(self, ok=True):
        self.sock = socket.socket()
        self.sock.bind(("127.0.0.1", 0))
        self.sock.listen(1)
        self.port = self.sock.getsockname()[1]
        self.got = None
        self.ok = ok
        self.thread = threading.Thread(target=self.serve, daemon=True)
        self.thread.start()

    def serve(self):
        conn, _ = self.sock.accept()
        with conn:
            self.got = json.loads(conn.makefile().readline())
            conn.sendall(json.dumps({"t": "paired", "ok": self.ok}).encode() + b"\n")
        self.sock.close()


class TestPair(FakeCore):
    def test_envia_host_porta_e_token_do_doc(self):
        self.set_doc({"token": "tok", "port": 9400}, 2)
        panel = PanelServer()
        self.assertEqual(ctl.pair("127.0.0.1", panel.port), {"ok": True})
        panel.thread.join(2)
        self.assertEqual(panel.got, {"t": "pair", "name": ctl.host_name(), "host": "127.0.0.1",
                                     "port": 9400, "token": "tok", "auto": True})

    def test_painel_que_recusa_e_erro(self):
        self.set_doc({"token": "tok"}, 1)
        panel = PanelServer(ok=False)
        got = ctl.pair("127.0.0.1", panel.port)
        self.assertFalse(got["ok"])
        self.assertTrue(got["error"])

    def test_sem_token_nao_conecta(self):
        got = ctl.pair("127.0.0.1", 9)
        self.assertEqual(got["ok"], False)
        self.assertIn("no token", got["error"])


class TestMain(unittest.TestCase):
    def test_toda_subida_recupera_uma_troca_interrompida(self):
        with mock.patch.object(ctl.update, "recover") as recover, \
                mock.patch("sys.stdout", new_callable=io.StringIO):
            ctl.main(["nada"])
        recover.assert_called_once_with()

    def run_main(self, argv):
        with mock.patch.object(ctl.update, "recover"), \
                mock.patch("sys.stdout", new_callable=io.StringIO) as out:
            code = ctl.main(argv)
        return code, json.loads(out.getvalue())

    def test_subcomando_com_argumentos(self):
        with mock.patch.object(ctl, "bridge_port", return_value=9375), \
                mock.patch.object(ctl, "kill_holder", return_value={"ok": True}) as kill, \
                mock.patch.object(ctl, "pair", return_value={"ok": True}) as pair:
            self.assertEqual(self.run_main(["kill-holder", "42"]), (0, {"ok": True}))
            self.assertEqual(self.run_main(["pair", "10.0.0.5", "9376"]), (0, {"ok": True}))
        kill.assert_called_once_with(42, 9375)
        pair.assert_called_once_with("10.0.0.5", 9376)

    def test_comando_ou_argumento_invalido_mostra_o_uso_completo(self):
        for argv in ([], ["nada"], ["kill-holder"], ["kill-holder", "x"], ["pair", "1.2.3.4"]):
            code, out = self.run_main(argv)
            self.assertEqual(code, 2, argv)
        for name in ("status", "port-holder", "kill-holder <pid>", "ensure", "restart", "token",
                     "rotate-token", "discover", "pair <ip> <port>"):
            self.assertIn(name, out["error"])


class TestDiscover(unittest.TestCase):
    def test_lista_os_anuncios_e_ignora_o_resto(self):
        probe = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        probe.bind(("127.0.0.1", 0))
        port = probe.getsockname()[1]
        probe.close()
        out = {}
        t = threading.Thread(target=lambda: out.update(ctl.discover(0.6, port)))
        t.start()
        tx = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        for _ in range(5):   # repete como o beacon, até o listener estar de pé
            for msg in (b'{"t":"herdr-assist","id":"B2","port":9376}', b"lixo",
                        b'{"t":"outro","id":"X"}', b'{"t":"herdr-assist","id":"A1","port":"x"}',
                        b'[1]', b'{"t":"herdr-assist","id":"A1"}'):
                tx.sendto(msg, ("127.0.0.1", port))
            threading.Event().wait(0.05)
        tx.close()
        t.join()
        self.assertEqual(out, {"panels": [{"id": "A1", "ip": "127.0.0.1", "port": 9376},
                                          {"id": "B2", "ip": "127.0.0.1", "port": 9376}]})

    def test_porta_ocupada_vira_erro(self):
        with mock.patch.object(socket.socket, "bind", side_effect=OSError("busy")):
            got = ctl.discover(0.1, 9376)
        self.assertEqual(got["panels"], [])
        self.assertIn("busy", got["error"])


if __name__ == "__main__":
    unittest.main()
