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

import herdr_bridge as b

HERE = os.path.dirname(os.path.abspath(__file__))
BRIDGE = os.path.join(os.path.dirname(HERE), "herdr_bridge.py")
FAKE_HC = os.path.join(HERE, "fake_hc.py")
FAKE_APP = os.path.join(HERE, "fake_herdr_app.py")
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
               "HC_BIN": FAKE_HC, "HERDR_APP_BIN": FAKE_APP, "FAKE_HC_DIR": self.dir,
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

    def calls_until(self, method, n=1, timeout=5):
        """Os frames chegam ao hc falso por outro processo: espera o n-ésimo."""
        deadline = time.time() + timeout
        while len(self.calls(method)) < n:
            if time.time() > deadline:
                self.fail("%s: menos de %d em %ss" % (method, n, timeout))
            time.sleep(0.02)
        return self.calls(method)

    def read_locked(self, s, rf, pane="t1a", cols=50, rows=20):
        return self.command(s, rf, {"type": "read_pane", "pane_id": pane, "lines": 5,
                                    "cols": cols, "rows": rows})

    def app_calls(self):
        path = os.path.join(self.dir, "app.jsonl")
        if not os.path.exists(path):
            return []
        with open(path) as fh:
            return [json.loads(line) for line in fh]

    def set_screen(self, text):
        with open(os.path.join(self.dir, "read.txt"), "w") as fh:
            fh.write(text)

    def command(self, s, rf, msg):
        """Manda um comando do painel; devolve as respostas que ele gerou.

        A ponte trata os comandos de uma conexão em ordem: o pong do ping que
        vai atrás só chega depois que o comando terminou.
        """
        s.sendall((json.dumps(msg) + "\n" + '{"type":"ping"}\n').encode())
        replies = []
        while True:
            line = rf.readline()
            if not line:
                self.fail("ponte fechou a conexão")
            m = json.loads(line)
            if m.get("type") == "pong":
                return replies
            if m.get("type") in ("pane_content", "error"):
                replies.append(m)

    def connect_ready(self):
        s, rf = self.connect()
        self.ready(rf)
        return s, rf

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

    def test_read_pane_devolve_o_texto_do_core(self):
        self.set_screen("linha 1\nlinha 2 ação\n")
        s, rf = self.connect_ready()
        out = self.command(s, rf, {"type": "read_pane", "pane_id": "t1a", "lines": 40})
        self.assertEqual(out, [{"type": "pane_content", "pane_id": "t1a",
                                "content": "linha 1\nlinha 2 ação"}])
        self.assertEqual(self.calls("terminal.read"), [{"terminal": "t1a", "lines": 40}])

    def test_read_pane_corta_por_linhas(self):
        self.set_screen("\n".join("l%d" % n for n in range(10)) + "\n\n\n")
        s, rf = self.connect_ready()
        out = self.command(s, rf, {"type": "read_pane", "pane_id": "t1a", "lines": 3,
                                   "cols": 50, "rows": 20})
        self.assertEqual(out[0]["content"], "l7\nl8\nl9")

    def test_read_pane_respeita_o_limite_de_payload(self):
        # aspas dobram no JSON: 12000 bytes de conteúdo viram mais que WIRE_MAX_B
        self.set_screen("\n".join("%02d" % n + '"' * 298 for n in range(40)))
        s, rf = self.connect_ready()
        s.sendall(b'{"type":"read_pane","pane_id":"t1a","lines":60}\n')
        raw = rf.readline()
        while '"pane_content"' not in raw:
            raw = rf.readline()
        self.assertLessEqual(len(raw.rstrip("\n").encode()), b.WIRE_MAX_B)
        content = json.loads(raw)["content"]
        self.assertTrue(content.startswith("0") and content.splitlines()[-1].startswith("39"))
        self.assertLess(len(content.splitlines()), 40)   # as mais antigas caíram

    def test_send_text_chega_ao_core_ja_com_force(self):
        s, rf = self.connect_ready()
        self.assertEqual(self.command(s, rf, {"type": "send_text", "pane_id": "t1a",
                                              "text": "oi agente"}), [])
        self.assertEqual(self.calls("terminal.send"),
                         [{"terminal": "t1a", "text": "oi agente", "force": True}])

    def test_send_text_vazio_ou_longo_e_erro_sem_chegar_ao_core(self):
        s, rf = self.connect_ready()
        for text in ("", "x" * (b.MAX_TEXT + 1)):
            out = self.command(s, rf, {"type": "send_text", "pane_id": "t1a", "text": text})
            self.assertEqual([m["type"] for m in out], ["error"])
        self.assertEqual(self.calls("terminal.send"), [])

    def test_send_keys_traduz_cada_tecla_da_allowlist_para_o_hc(self):
        esperado = {"y": "y", "n": "n", "a": "a", "Enter": "enter", "Tab": "tab",
                    "Escape": "esc", "C-c": "ctrl+c", "Up": "up", "Down": "down",
                    "Left": "left", "Right": "right", "BSpace": "backspace",
                    **{str(n): str(n) for n in range(10)}}
        self.assertEqual(set(esperado), b.SAFE_KEYS)    # nenhuma tecla ficou sem tradução
        s, rf = self.connect_ready()
        for painel, hc in esperado.items():
            self.assertEqual(self.command(s, rf, {"type": "send_keys", "pane_id": "t1a",
                                                  "keys": [painel]}), [])
            self.assertEqual(self.calls("terminal.keys")[-1],
                             {"terminal": "t1a", "keys": [hc]})

    def test_send_keys_varias_teclas_numa_chamada(self):
        s, rf = self.connect_ready()
        self.command(s, rf, {"type": "send_keys", "pane_id": "t1a", "keys": ["Up", "Enter"]})
        self.assertEqual(self.calls("terminal.keys"),
                         [{"terminal": "t1a", "keys": ["up", "enter"]}])

    def test_send_keys_fora_da_allowlist_e_erro_sem_chegar_ao_core(self):
        s, rf = self.connect_ready()
        out = self.command(s, rf, {"type": "send_keys", "pane_id": "t1a",
                                   "keys": ["Enter", "C-d"]})
        self.assertEqual([m["type"] for m in out], ["error"])
        self.assertEqual(self.calls("terminal.keys"), [])

    def test_focus_roda_agent_focus_no_herdr_app(self):
        s, rf = self.connect_ready()
        self.assertEqual(self.command(s, rf, {"type": "focus", "pane_id": "t1b"}), [])
        self.assertEqual(self.app_calls(), [["do", "agent.focus", "--terminal", "t1b"]])

    def test_pane_desconhecido_ou_de_outra_maquina_e_erro(self):
        s, rf = self.connect_ready()
        for pane in ("t9z", "t1c", "m1/t1a", ""):    # t1c existe mas não é agente
            for msg in ({"type": "read_pane"}, {"type": "send_text", "text": "x"},
                        {"type": "send_keys", "keys": ["Enter"]}, {"type": "focus"}):
                out = self.command(s, rf, dict(msg, pane_id=pane))
                self.assertEqual([m["type"] for m in out], ["error"], (pane, msg))
        self.assertEqual(self.app_calls(), [])
        for method in ("terminal.read", "terminal.send", "terminal.keys"):
            self.assertEqual(self.calls(method), [])

    def test_falha_do_core_vira_error(self):
        s, rf = self.connect_ready()
        # o terminal some do core sem a ponte ter visto o evento
        self.set_terminals([terminal("t1b", "idle", "eletrilha / 1")])
        for msg in ({"type": "read_pane"}, {"type": "send_text", "text": "x"},
                    {"type": "send_keys", "keys": ["Enter"]}):
            out = self.command(s, rf, dict(msg, pane_id="t1a"))
            self.assertEqual([m["type"] for m in out], ["error"], msg)

    def test_read_pane_com_geometria_trava_o_terminal_no_tamanho_do_painel(self):
        self.set_screen("tela")
        s, rf = self.connect_ready()
        out = self.read_locked(s, rf)
        self.assertEqual(out[0]["content"], "tela")
        self.assertEqual(self.calls("terminal.attach"),
                         [{"terminal": "t1a", "cols": 50, "rows": 20}])
        # a ponte descarta o stream, mas devolve o crédito da repintura
        self.assertEqual(self.calls_until("frame.credit")[0],
                         {"channel": 1, "n": len(b"\x1b[?1000l\x1b[?1006l\x1b[2J"
                                                 b"\x1b[?1000h\x1b[?1006hrepaint")})
        # mesma trava: nada de novo attach, só retoma o tamanho (o app pode ter tomado)
        self.read_locked(s, rf)
        self.assertEqual(len(self.calls("terminal.attach")), 1)
        self.assertEqual(self.calls_until("frame.resize"),
                         [{"channel": 1, "cols": 50, "rows": 20}])
        self.read_locked(s, rf, cols=60, rows=30)
        self.assertEqual(self.calls_until("frame.resize", 2)[-1],
                         {"channel": 1, "cols": 60, "rows": 30})

    def test_read_pane_sem_geometria_valida_nao_trava(self):
        s, rf = self.connect_ready()
        for cols, rows in ((0, 0), (3, 20), (401, 20), (50, 1), (50, 201)):
            self.assertEqual(self.read_locked(s, rf, cols=cols, rows=rows)[0]["type"],
                             "pane_content")
        self.assertEqual(self.calls("terminal.attach"), [])

    def test_travar_outro_pane_solta_o_anterior(self):
        s, rf = self.connect_ready()
        self.read_locked(s, rf, "t1a")
        self.read_locked(s, rf, "t1b")
        self.assertEqual([c["terminal"] for c in self.calls("terminal.attach")],
                         ["t1a", "t1b"])
        self.assertEqual(self.calls_until("frame.close"), [{"channel": 1}])

    def test_scroll_pane_manda_a_roda_na_celula_sob_o_dedo(self):
        s, rf = self.connect_ready()
        self.read_locked(s, rf)                      # trava em 50x20
        self.assertEqual(self.command(s, rf, {"type": "scroll_pane", "pane_id": "t1a",
                                              "dir": "up", "lines": 3,
                                              "col": 9, "row": 4}), [])
        self.assertEqual(self.calls_until("frame.data")[0],
                         {"channel": 1, "text": "\x1b[<64;10;5M" * 3})
        # célula fora da tela vai para a borda; muitas linhas viram menos eventos
        self.command(s, rf, {"type": "scroll_pane", "pane_id": "t1a", "dir": "down",
                             "lines": 20, "col": 999, "row": -5})
        self.assertEqual(self.calls_until("frame.data", 2)[1]["text"],
                         "\x1b[<65;50;1M" * b.wheel_events(20))
        self.assertEqual(len(self.calls("frame.resize")), 2)   # cada roda retoma o tamanho
        self.assertEqual(self.calls("terminal.keys"), [])     # com mouse, nada de PageUp

    def test_wheel_events_cresce_devagar_por_causa_da_aceleracao(self):
        self.assertEqual([b.wheel_events(n) for n in (1, 2, 3, 4, 5, 11, 20)],
                         [1, 2, 3, 3, 4, 7, 11])

    def test_scroll_pane_sem_trava_nao_faz_nada(self):
        s, rf = self.connect_ready()
        msg = {"type": "scroll_pane", "pane_id": "t1a", "dir": "up", "lines": 3}
        self.assertEqual(self.command(s, rf, msg), [])
        self.read_locked(s, rf, "t1b")
        self.assertEqual(self.command(s, rf, msg), [])     # trava é de outro pane
        time.sleep(0.2)
        self.assertEqual(self.calls("frame.data"), [])
        self.assertEqual(self.calls("terminal.keys"), [])

    def test_scroll_pane_sem_mouse_tracking_usa_pageup_pagedown(self):
        # sem mouse tracking o core repassa a roda como texto (medido num bash)
        self.set_terminals([terminal("t1a", mouse=False)])
        s, rf = self.connect_ready()
        self.read_locked(s, rf)
        self.calls_until("frame.credit")             # a repintura já passou pela ponte
        for d in ("up", "down"):
            self.command(s, rf, {"type": "scroll_pane", "pane_id": "t1a", "dir": d,
                                 "lines": 20})
        self.assertEqual(self.calls("terminal.keys"),
                         [{"terminal": "t1a", "keys": ["pageup"]},
                          {"terminal": "t1a", "keys": ["pagedown"]}])
        time.sleep(0.2)
        self.assertEqual(self.calls("frame.data"), [])

    def test_release_pane_solta_a_trava(self):
        s, rf = self.connect_ready()
        self.read_locked(s, rf)
        self.assertEqual(self.command(s, rf, {"type": "release_pane", "pane_id": "t1b"}), [])
        self.assertEqual(self.command(s, rf, {"type": "release_pane", "pane_id": "t1a"}), [])
        self.assertEqual(self.calls_until("frame.close"), [{"channel": 1}])
        self.command(s, rf, {"type": "scroll_pane", "pane_id": "t1a", "dir": "up",
                             "lines": 1})
        time.sleep(0.2)
        self.assertEqual(self.calls("frame.data"), [])

    def test_ultimo_painel_saindo_solta_a_trava(self):
        s, rf = self.connect_ready()
        self.read_locked(s, rf)
        rf.close()      # o makefile segura o socket aberto
        s.close()
        self.assertEqual(self.calls_until("frame.close"), [{"channel": 1}])

    def test_ponte_morrendo_nao_deixa_o_terminal_travado(self):
        s, rf = self.connect_ready()
        self.read_locked(s, rf)
        self.proc.kill()                     # SIGKILL: sem chance de soltar nada
        self.assertEqual(self.calls_until("frame.eof"), [{"channels": [1]}])


if __name__ == "__main__":
    unittest.main()
