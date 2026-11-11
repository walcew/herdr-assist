# cd plugin && python3 -m unittest tests.test_bridge_machines -v
"""Máquinas SSH salvas: o que o cliente do Herdr mostra junto, a ponte junta."""
import asyncio
import json
import unittest

import herdr_bridge as b

PROFILE = {"id": "78dd71e3", "label": "Hermes VM", "target": "hermes-vm",
           "session": "default", "enabled": True}


def snapshot_payload(pane, cwd, agent="hermes", status="idle",
                     tab="w7:t1", label="1"):
    """Mesmo formato de `herdr --machine <id> api snapshot`."""
    return json.dumps({"id": "cli:api:snapshot", "result": {"snapshot": {
        "agents": [{"pane_id": pane, "agent": agent, "agent_status": status,
                    "cwd": cwd, "workspace_id": "w7", "tab_id": tab}],
        "tabs": [{"tab_id": tab, "label": label, "workspace_id": "w7"}],
    }}})


class TestFriendlyName(unittest.TestCase):
    def test_rotulo_da_aba_vence(self):
        self.assertEqual(b.friendly_name("agent-claude", "/tmp"), "agent-claude")

    def test_aba_sem_nome_cai_no_cwd(self):
        self.assertEqual(b.friendly_name("1", "/home/luis"), "luis")

    def test_sem_rotulo_cai_no_cwd(self):
        self.assertEqual(b.friendly_name("", "/home/luis"), "luis")


class TestMachineCode(unittest.TestCase):
    def test_duas_palavras_vira_iniciais(self):
        self.assertEqual(b.machine_code("Hermes VM"), "HV")

    def test_uma_palavra_vira_tres_letras(self):
        self.assertEqual(b.machine_code("workbox"), "WOR")

    def test_sem_letras(self):
        self.assertEqual(b.machine_code(""), "REM")


class TestRefreshMachines(unittest.TestCase):
    def setUp(self):
        self.orig = b.cli_run
        self.addCleanup(setattr, b, "cli_run", self.orig)
        for name, value in (("MACHINES", "all"), ("remote_agents", []),
                            ("remote_panes", {})):
            old = getattr(b, name)
            setattr(b, name, value)
            self.addCleanup(setattr, b, name, old)

    def fake(self, responses):
        async def cli_run(args, timeout=10):
            return responses.get(" ".join(args))
        b.cli_run = cli_run

    def run_refresh(self):
        asyncio.run(b.refresh_machines())

    def test_traz_o_agente_remoto_com_id_e_projeto_prefixados(self):
        self.fake({
            "machine list --json": json.dumps([PROFILE]),
            "--machine 78dd71e3 api snapshot": snapshot_payload("w7:p1", "/home/luis"),
        })
        self.run_refresh()
        self.assertEqual(len(b.remote_agents), 1)
        a = b.remote_agents[0]
        # o id leva o código da máquina: w7:p1 existe nos dois servidores
        self.assertEqual(a["pane_id"], "HV/w7:p1")
        self.assertEqual(a["project"], "HV/luis")
        self.assertEqual(a["machine"], "Hermes VM")
        self.assertEqual(a["status"], "idle")
        # é este mapa que manda focus/send_keys para o servidor certo
        self.assertEqual(b.remote_panes["HV/w7:p1"], ("78dd71e3", "w7:p1"))

    def test_rotulo_da_aba_remota_vira_nome(self):
        self.fake({
            "machine list --json": json.dumps([PROFILE]),
            "--machine 78dd71e3 api snapshot": snapshot_payload(
                "w7:p1", "/home/luis", label="agent-hermes"),
        })
        self.run_refresh()
        self.assertEqual(b.remote_agents[0]["project"], "HV/agent-hermes")

    def test_maquina_desabilitada_fica_de_fora(self):
        self.fake({
            "machine list --json": json.dumps([dict(PROFILE, enabled=False)]),
            "--machine 78dd71e3 api snapshot": snapshot_payload("w7:p1", "/home/luis"),
        })
        self.run_refresh()
        self.assertEqual(b.remote_agents, [])
        self.assertEqual(b.remote_panes, {})

    def test_selecao_por_rotulo_sem_ligar_tudo(self):
        b.MACHINES = "hermes vm"
        self.fake({
            "machine list --json": json.dumps([PROFILE, dict(PROFILE, id="outra", label="Build box")]),
            "--machine 78dd71e3 api snapshot": snapshot_payload("w7:p1", "/home/luis"),
        })
        self.run_refresh()
        self.assertEqual([a["pane_id"] for a in b.remote_agents], ["HV/w7:p1"])

    def test_uma_maquina_fora_do_ar_nao_derruba_as_outras(self):
        self.fake({
            "machine list --json": json.dumps([PROFILE, dict(PROFILE, id="bbbb", label="Build box")]),
            "--machine 78dd71e3 api snapshot": snapshot_payload("w7:p1", "/home/luis"),
            # "bbbb" não responde (cli_run devolve None)
        })
        self.run_refresh()
        self.assertEqual([a["pane_id"] for a in b.remote_agents], ["HV/w7:p1"])

    def test_json_quebrado_nao_apaga_o_cache(self):
        self.fake({
            "machine list --json": json.dumps([PROFILE]),
            "--machine 78dd71e3 api snapshot": snapshot_payload("w7:p1", "/home/luis"),
        })
        self.run_refresh()
        self.fake({
            "machine list --json": json.dumps([PROFILE]),
            "--machine 78dd71e3 agent list": "{not json",
        })
        self.run_refresh()
        self.assertEqual([a["pane_id"] for a in b.remote_agents], ["HV/w7:p1"])

    def test_sem_config_nao_chama_o_herdr(self):
        b.MACHINES = ""
        calls = []

        async def cli_run(args, timeout=10):
            calls.append(args)
            return None

        b.cli_run = cli_run
        self.run_refresh()
        self.assertEqual(calls, [])


if __name__ == "__main__":
    unittest.main()
